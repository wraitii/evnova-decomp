#include "hud_renderer.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "nova_font.hpp"
#include "targeting.hpp"
#include "weapon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace game {
// The HudRenderer owns a screen-font cache (NovaFontCache); its destructor must
// live where that type is complete, so it is defined here rather than inline.
HudRenderer::~HudRenderer() = default;

namespace {

// The gameplay interface layout key (the negative key -0x6a918b9a stored in
// ScenarioData_FindObjectKey); NovaResource_LoadGameplayInterfaceLayout uses
// the same .ntf FourCC as /type lookup.
constexpr std::int16_t kDefaultInterfaceId = 0x80;

// The panel colour slots decoded from the .ntf layout (little-endian 32-bit
// `00 rr gg bb`, opaque). The renderer applies them to the bars/readouts, in
// the order NovaUi_SetupGameplayPanelColors maps them to its colour pointers.
[[nodiscard]] SDL_Color ColorOf(std::uint32_t word) {
  // Archive bytes are `00 rr gg bb`; read back as (r,g,b).
  const std::uint8_t r = static_cast<std::uint8_t>((word >> 8) & 0xff);
  const std::uint8_t g = static_cast<std::uint8_t>((word >> 16) & 0xff);
  const std::uint8_t b = static_cast<std::uint8_t>((word >> 24) & 0xff);
  return SDL_Color{r, g, b, SDL_ALPHA_OPAQUE};
}

// Resolves the HUD interface id exactly as Ui_InstallGameplayInterfaceLayout
// (0x004cda50): the player ship class's inherited government (attributes, then
// combat), whose Government.interface_id (>= 0x80) selects the .ntf layout.
// Falls back to the Default interface when the chain yields nothing usable.
[[nodiscard]] std::int16_t ResolveInterfaceId(const GameState &state) {
  const std::int16_t ship_id =
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80);
  if (long ship_index = static_cast<long>(ship_id) - 0x80;
      ship_index >= 0 &&
      ship_index < static_cast<long>(state.scenario.ships.size())) {
    const ShipClass &cls =
        state.scenario.ships[static_cast<std::size_t>(ship_index)];
    std::int16_t gov = cls.inherent_attributes_govt;
    if (gov < 0x80 || gov >= 0x180) {
      gov = cls.inherent_combat_govt;
    }
    const Government *g = state.scenario.Government(gov);
    if (g && g->interface_id >= 0x80 && g->interface_id < 0x180) {
      return g->interface_id;
    }
  }
  return kDefaultInterfaceId;
}

// Loads the `.ntf` interface layout for the given interface id (the .ntf
// resources are keyed by their government interface id >= 0x80 in Nova
// Graphics 3).
[[nodiscard]] bool LoadLayoutFor(std::int16_t interface_id,
                                 GameplayInterfaceLayout &out) {
  const auto layout = NovaResource_LoadGameplayInterfaceLayout(
      static_cast<std::uint16_t>(interface_id));
  if (!layout) {
    return false;
  }
  out = *layout;
  return true;
}

} // namespace

bool HudRenderer::Install(SdlPlatform &platform, const GameState &state) {
  // Resolve and load the interface layout. A missing gov silently falls back
  // to the Default interface (0x80), mirroring
  // Ui_InstallGameplayInterfaceLayout's `sVar4 = 0x80` default.
  const std::int16_t interface_id = ResolveInterfaceId(state);
  GameplayInterfaceLayout layout;
  if (!LoadLayoutFor(interface_id, layout)) {
    // No .ntf record at all: keep the existing cockpit texture (if any) but
    // record the failure so Draw() falls back to flat bars.
    NovaLog::Warn("HUD: no interface layout {:#04x}; falling back to flat HUD",
                  static_cast<unsigned>(interface_id));
    installed_ = false;
    return false;
  }

  interface_id_ = interface_id;
  layout_ = layout;

  // Upload the cockpit PICT (the "status bar" strip) at its native size. A
  // failed decode leaves cockpit_ null and Draw() supplies flat bar frames.
  cockpit_.reset();
  cockpit_w_ = 0;
  cockpit_h_ = 0;
  if (const auto pict_data =
          NovaResource_LoadPictData(layout_.interface_bg_pict_id)) {
    if (const auto pict = Resource_LoadPictAsImage(*pict_data)) {
      auto tex = SdlTexture::Create(
          platform.renderer(), pict->width, pict->height, pict->rgba_pixels);
      if (tex) {
        cockpit_ = std::move(tex);
        cockpit_w_ = pict->width;
        cockpit_h_ = pict->height;
      } else {
        NovaLog::Warn("HUD: cockpit PICT {:#04x} uploaded failed",
                      layout_.interface_bg_pict_id);
      }
    } else {
      NovaLog::Warn("HUD: cockpit PICT {:#04x} could not be decoded",
                    layout_.interface_bg_pict_id);
    }
  } else {
    NovaLog::Info("HUD: cockpit PICT {:#04x} not found; flat HUD bars",
                  layout_.interface_bg_pict_id);
  }

  installed_ = true;
  NovaLog::Info("HUD: installed interface {:#04x} (cockpit PICT {:#04x}, "
                "{}x{})",
                static_cast<unsigned>(interface_id),
                static_cast<unsigned>(layout_.interface_bg_pict_id),
                cockpit_w_,
                cockpit_h_);
  return true;
}

// The in-game HUD is composed on the game's fixed 1024x768 logical canvas and
// scaled 0.625 onto the default 640x480 window (the same 0.625 scale the main
// menu uses -- see GameplayGeometry_FromSurface / the gameplay_interface
// header). All panel rects from the .ntf layout are 1024-canvas coordinates;
// this is the constant that projects them onto the window. The HUD is top-left
// anchored at the (0,0) canvas anchor for a full 1024x768 surface (resolution
// extension shows more *world*, not a bigger HUD).
constexpr float kHudScale = 640.0F / 1024.0F; // 0.625

// Converts a layout panel rect (1024-canvas coords) into a window-space rect at
// the 0.625 scale. Mirrors the game painting the HUD panels onto the canvas
// that the window then displays scaled.
[[nodiscard]] SDL_FRect ProjectPanel(const HudPanelRect &panel) {
  return SDL_FRect{panel.left * kHudScale,
                   panel.top * kHudScale,
                   static_cast<float>(panel.width()) * kHudScale,
                   static_cast<float>(panel.height()) * kHudScale};
}

// Draws one life-support bar from its real layout panel rect, faithfully
// reproducing the game's fill geometry (HudBar_FillRect mirrors
// NovaUi_DrawPlayerShieldBar 0x0045ea66 / _ArmorBar 0x0045ebe8 /
// _FuelLevelBar 0x0045f086: the fill axis is chosen by the panel's aspect, and
// the tall life-bar slots fill from the top down). The 1024-canvas geometry is
// projected by kHudScale into window space. Only the filled portion is drawn
// (opaque on the panel); the surrounding bar trough art lives in the cockpit
// PICT, which is composited separately.
void DrawLifeBar(SDL_Renderer *renderer,
                 const HudPanelRect &panel,
                 float current,
                 float maximum,
                 SDL_Color color) {
  const float fraction =
      maximum > 0.0F ? std::clamp(current / maximum, 0.0F, 1.0F) : 0.0F;
  const HudBarFill fill = HudBar_FillRect(panel, fraction);
  if (fill.empty()) {
    return;
  }
  const SDL_FRect rect{fill.left * kHudScale,
                       fill.top * kHudScale,
                       fill.width * kHudScale,
                       fill.height * kHudScale};
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderFillRect(renderer, &rect);
}

// Draws the readout panel text (travel / weapon ammo / target / cargo) at the
// panel's genuine 0.625-projected position, in the interface's value colour and
// the .ntf body font size (e.g. 12 for Geneva).
void DrawReadout(SdlPlatform &platform,
                 NovaFontCache &font,
                 float font_size_px,
                 const HudPanelRect &panel,
                 std::string_view text,
                 const SDL_Color &color,
                 float baseline_offset) {
  if (!panel.valid() || text.empty()) {
    return;
  }
  const SDL_FRect box = ProjectPanel(panel);
  NovaText_DrawCentered(platform,
                        font,
                        NovaFontFamily::kGeneva,
                        font_size_px,
                        kNovaFontStyleRegular,
                        color,
                        box.x,
                        box.x + box.w,
                        box.y + baseline_offset,
                        text);
}

void HudRenderer::Draw(SdlPlatform &platform, const GameState &state) {
  if (!installed_) {
    return;
  }
  SDL_Renderer *renderer = platform.renderer();
  // Mirror the original DrawContext font state: one cache per installed HUD,
  // so font handles are decoded once rather than reloaded every frame.
  if (!font_cache_) {
    font_cache_ = std::make_unique<NovaFontCache>();
  }

  // The HUD is composed on the game's fixed 1024x768 logical canvas, scaled
  // 0.625 onto the default 640x480 window and top-left anchored at the canvas
  // origin (anchor (0,0) for a full 1024x768 surface -- see
  // GameplayGeometry_FromSurface). The gov cockpit PICT and every panel rect
  // below are laid out in those 1024-canvas coordinates and projected by
  // kHudScale. Resolution extension shows more world (the viewport widens);
  // the HUD chrome itself stays at this fixed 0.625 scale.
  //
  // Cockpit PICT: the gov interface background art, drawn at its 0.625-scaled
  // canvas position (top-left anchored).
  if (cockpit_) {
    const SDL_FRect strip_rect{0.0F,
                               0.0F,
                               static_cast<float>(cockpit_w_) * kHudScale,
                               static_cast<float>(cockpit_h_) * kHudScale};
    SDL_RenderTexture(renderer, cockpit_->get(), nullptr, &strip_rect);
  }

  // Life-support bars, drawn inside their genuine layout panel rects at their
  // genuine canvas positions. The Federation shield/armor/fuel panels are thin
  // tall slots (e.g. shield 200..207 x 35..184), so DrawLifeBar anchors each
  // fill to the panel TOP and grows it downward by the current/max fraction.
  // Before the first movement update fills the stats cache the bars read as
  // full (fresh dock == full shields/armor/fuel).
  const PlayerEffectiveStats &eff = state.cached_stats;
  const float shield_max = state.stat_cache_valid ? eff.max_shield_points
                                                  : state.player.shield_points;
  const float armor_max =
      state.stat_cache_valid ? eff.max_armor_points : state.player.armor_points;
  const float fuel_max =
      state.stat_cache_valid ? eff.fuel_capacity : state.player.fuel_points;
  DrawLifeBar(renderer,
              layout_.shield_panel,
              state.player.shield_points,
              shield_max,
              ColorOf(layout_.color_word[4]));
  DrawLifeBar(renderer,
              layout_.armor_panel,
              state.player.armor_points,
              armor_max,
              ColorOf(layout_.color_word[5]));
  DrawLifeBar(renderer,
              layout_.fuel_panel,
              state.player.fuel_points,
              fuel_max,
              ColorOf(layout_.color_word[6]));

  // Readout panels: value colour (slot 0), body font.
  const SDL_Color value_color = ColorOf(layout_.color_word[0]);

  // Travel-status panel: destination system name when a travel/land target is
  // selected, else the idle fallback.
  {
    std::string travel;
    const std::int16_t sid = state.travel.selected_stellar_id;
    const auto *st = state.scenario.Stellar(sid);
    if (st && !st->name.empty()) {
      travel = st->name;
    } else {
      travel = "NO TARGET";
    }
    DrawReadout(platform,
                *font_cache_,
                static_cast<float>(layout_.font_size) * kHudScale,
                layout_.travel_status_panel,
                travel,
                value_color,
                4.0F);
  }

  // Weapon/ammo panel: current primary bank display name.
  {
    const std::string wpn = NovaWeapon_BankDisplayName(state, 0);
    DrawReadout(platform,
                *font_cache_,
                static_cast<float>(layout_.font_size) * kHudScale,
                layout_.weapon_ammo_panel,
                wpn,
                value_color,
                4.0F);
  }

  // Target panel: the auto-targeted stellar, or an empty target hint.
  {
    std::string tgt;
    const std::int16_t sid = state.travel.selected_stellar_id;
    const auto *st = state.scenario.Stellar(sid);
    if (st && !st->name.empty()) {
      tgt = st->name;
      if (NovaTargeting_IsLandingAvailable(state)) {
        tgt += " [L]";
      }
    } else {
      tgt = "(none)";
    }
    DrawReadout(platform,
                *font_cache_,
                static_cast<float>(layout_.font_size) * kHudScale,
                layout_.target_status_panel,
                tgt,
                value_color,
                4.0F);
  }

  // Cargo/mission panel: current credits + cargo total.
  {
    char cargo[96];
    int total = 0;
    for (const auto n : state.inventory.cargo_bins) {
      total += n;
    }
    std::snprintf(
        cargo, sizeof(cargo), "CR %d  CARGO %d", state.player.credits, total);
    DrawReadout(platform,
                *font_cache_,
                static_cast<float>(layout_.font_size) * kHudScale,
                layout_.cargo_status_panel,
                cargo,
                value_color,
                4.0F);
  }
}

} // namespace game
