#include "hud_renderer.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
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
    if (gov == -1) {
      gov = cls.inherent_combat_govt;
    }
    const Government *g = state.scenario.Government(gov);
    if (g && g->interface_id >= 0x80) {
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

const HudRenderer::PortraitEntry *
HudRenderer::TargetPortrait(SdlPlatform &platform,
                            const ScenarioData &scenario,
                            std::int16_t ship_class_id) {
  const auto cached = portraits_.find(ship_class_id);
  if (cached != portraits_.end()) {
    // Null entry = already-tried-and-failed: never return it to a caller that
    // dereferences the texture.
    return cached->second->texture ? cached->second.get() : nullptr;
  }
  // Target-info pict id = 3000 + clone-source class (Bible: "PICT resource ID
  // 3000 + shipID - 128"). The engine reuses one pict for every class that
  // shares the source's base sprites (ShipClassDef.clone_source_ship_class,
  // derived from the sh\x8an BaseImageID at load time), so a Fed Viper
  // duplicate resolves to the original's 3016 rather than a missing 3096.
  std::int16_t pict_class = ship_class_id;
  if (const ShipClass *cls = scenario.Ship(ship_class_id + 0x80);
      cls != nullptr && cls->clone_source_ship_class >= 0) {
    pict_class = cls->clone_source_ship_class;
  }
  auto entry = std::make_unique<PortraitEntry>();
  if (const auto pict_data = NovaResource_LoadPictData(
          static_cast<std::uint16_t>(3000 + pict_class))) {
    if (const auto pict = Resource_LoadPictAsImage(*pict_data)) {
      auto tex = SdlTexture::Create(
          platform.renderer(), pict->width, pict->height, pict->rgba_pixels);
      if (tex) {
        entry->texture = std::move(tex);
        entry->width = pict->width;
        entry->height = pict->height;
      } else {
        NovaLog::Warn("target panel: portrait PICT {:#x} upload failed",
                      3000 + pict_class);
      }
    }
  }
  const auto [it, inserted] =
      portraits_.emplace(ship_class_id, std::move(entry));
  (void)inserted;
  return it->second->texture ? it->second.get() : nullptr;
}

[[nodiscard]] SDL_FRect ProjectPanel(const HudPanelRect &panel) {
  return SDL_FRect{static_cast<float>(panel.left),
                   static_cast<float>(panel.top),
                   static_cast<float>(panel.width()),
                   static_cast<float>(panel.height())};
}

// Draws one life-support bar from its real layout panel rect, faithfully
// reproducing the game's fill geometry (HudBar_FillRect mirrors
// NovaUi_DrawPlayerShieldBar 0x0045ea66 / _ArmorBar 0x0045ebe8 /
// _FuelLevelBar 0x0045f086: the fill axis is chosen by the panel's aspect.
// The shipped HUD slots are wide and grow from left to right. Only the
// filled portion is drawn
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
  const SDL_FRect rect{fill.left, fill.top, fill.width, fill.height};
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderFillRect(renderer, &rect);
}

// Draws the readout panel text (travel / weapon ammo / target / cargo) at the
// panel's genuine position, in the interface's value colour and the .ntf body
// font size (e.g. 12 for Geneva).
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

  // The cockpit PICT is a 194x767 strip. The original positions it against the
  // current render owner's right edge and translates every panel by the same
  // amount (RenderOwner.right - DAT_0088c020), keeping this UI top-right
  // anchored while the flight viewport expands.
  const auto playfield = platform.logical_playfield_size();
  const auto render_right = static_cast<std::int16_t>(playfield.x);
  const std::int16_t hud_left =
      static_cast<std::int16_t>(render_right - kGameplayHudStripWidth);
  const auto anchor_panel = [render_right](const HudPanelRect &panel) {
    return HudPanel_AnchorTopRight(panel, render_right);
  };

  // Cockpit PICT: native size, pinned to the upper-right HUD origin.
  if (cockpit_) {
    const SDL_FRect strip_rect{static_cast<float>(hud_left),
                               0.0F,
                               static_cast<float>(cockpit_w_),
                               static_cast<float>(cockpit_h_)};
    SDL_RenderTexture(renderer, cockpit_->get(), nullptr, &strip_rect);
  }

  // Life-support bars, drawn inside their genuine top-right-strip rects. The
  // Federation shield/armor/fuel slots are x=35..184 at y=200/216/234, and
  // each fills from the left by the current/max fraction.
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
              anchor_panel(layout_.shield_panel),
              state.player.shield_points,
              shield_max,
              ColorOf(layout_.color_word[4]));
  DrawLifeBar(renderer,
              anchor_panel(layout_.armor_panel),
              state.player.armor_points,
              armor_max,
              ColorOf(layout_.color_word[5]));
  DrawLifeBar(renderer,
              anchor_panel(layout_.fuel_panel),
              state.player.fuel_points,
              fuel_max,
              ColorOf(layout_.color_word[6]));

  // Readout panels: value colour (slot 0), body font.
  const SDL_Color value_color = ColorOf(layout_.color_word[0]);

  // Travel-status panel: shows the engaged-jump state while the hyperspace
  // sequence runs, else the selected destination stellar's name (or the idle
  // fallback). Mirrors NovaUi_DrawTravelStatusPanel (0x0045e400): a transfer
  // (mode 2) title + the destination name, the idle message when no target.
  //
  // The travel state stores destination ids zero-based; the scenario accessor
  // keys systems by resource id (index + 0x80), so add the offset before the
  // lookup or the name resolves to nothing and the HUD shows a bare '?'.
  {
    std::string travel;
    if (state.travel.engaging) {
      travel = "JUMPING"; // transfer title (STR# 0x7d2/0x157)
      const auto *dst = state.scenario.System(
          static_cast<std::int16_t>(state.travel.destination_system_id + 0x80));
      if (dst && !dst->name.empty()) {
        travel += " > " + dst->name;
      }
    } else if (state.travel.starmap_destination_system_id >= 0) {
      // A destination is armed -- either plotted from the galaxy starmap, or
      // cycled with Backslash after entering hyperspace mode (H). Show the
      // jump/hyperspace destination system (the manual: the nav display reads
      // "Hyperspace" and lists the destination name).
      const auto *dst = state.scenario.System(static_cast<std::int16_t>(
          state.travel.starmap_destination_system_id + 0x80));
      const std::string prefix =
          state.travel.hyperspace_mode ? "HYP > " : "JUMP > ";
      if (dst && !dst->name.empty()) {
        travel = prefix + dst->name;
      } else {
        travel = prefix + "?";
      }
    } else {
      const std::int16_t sid = state.travel.selected_stellar_id;
      const auto *st = state.scenario.Stellar(sid);
      if (st && !st->name.empty()) {
        travel = st->name;
      } else {
        travel = "NO TARGET"; // idle message (STR# 0x7d2/0x156)
      }
    }
    DrawReadout(platform,
                *font_cache_,
                static_cast<float>(layout_.font_size),
                anchor_panel(layout_.travel_status_panel),
                travel,
                value_color,
                4.0F);
  }

  // Weapon/ammo panel: current active weapon bank name, with an ammo count for
  // ammo-based weapons (mirrors NovaUi_DrawActiveWeaponAmmoPanel 0x00460ec0).
  // Energy/unlimited weapons (ammo_type == -1 or flags_secondary & 0x40) show
  // the bank name without a count; a missing/invalid bank shows the idle
  // label instead of a count.
  {
    std::string text;
    const std::int16_t bank = state.player.active_weapon_bank_slot;
    const bool empty = bank < 0 || bank >= 0x100 ||
                       NovaWeapon_BankDisplayName(state, bank) == "?";
    if (empty) {
      text = "NO WEAPON"; // STR# 0x7d2/0x15e idle label
    } else {
      text = NovaWeapon_BankDisplayName(state, bank);
      const std::int16_t ammo = NovaWeapon_BankAmmoCount(state, bank);
      if (ammo >= 0) {
        text += " - " + std::to_string(ammo);
      }
    }
    DrawReadout(platform,
                *font_cache_,
                static_cast<float>(layout_.font_size),
                anchor_panel(layout_.weapon_ammo_panel),
                text,
                value_color,
                4.0F);
  }

  // Target panel: when a ship is the primary target (backquote cycle / 'o' /
  // mouse click) shows the ship's class name + government + a shield/armor
  // status line, mirroring NovaUi_DrawTargetStatusPanel (0x0045f530). Class
  // capability flag 0x0200 suppresses the status (gov name only); 0x0100 shows
  // armor percent instead of shield. A manually selected travel stellar
  // persists while flying toward it. (The per-class Subtitle is deferred:
  // TODO(decomp) -- needs the shp Subtitle field.)
  {
    std::string tgt;
    const std::int16_t ship_slot = state.player.primary_target_ship_slot;
    if (ship_slot > 0 &&
        state.SlotInRange(static_cast<std::size_t>(ship_slot))) {
      const Ship &target = state.ShipAt(static_cast<std::size_t>(ship_slot));
      const ShipClass *target_cls = state.scenario.Ship(
          static_cast<std::int16_t>(target.ship_class_id + 0x80));
      std::string name = target_cls ? target_cls->display_name : "?";
      const Government *govt = state.scenario.Government(
          static_cast<std::int16_t>(target.faction_or_government_id + 0x80));
      // Portrait: blit the class PICT into the panel's left area at native
      // size (clipped to the panel). The original's portrait box is centred
      // toward the panel's left; we place it at the top-left and let the text
      // read out to its right. TargetPortrait resolves the PICT through the
      // class's clone source and never returns a textureless entry.
      if (const auto *portrait =
              TargetPortrait(platform, state.scenario, target.ship_class_id)) {
        const HudPanelRect pp = anchor_panel(layout_.target_status_panel);
        SDL_FRect box{static_cast<float>(pp.left),
                      static_cast<float>(pp.top),
                      static_cast<float>(portrait->width),
                      static_cast<float>(portrait->height)};
        if (box.w > static_cast<float>(pp.width())) {
          box.w = static_cast<float>(pp.width());
        }
        if (box.h > static_cast<float>(pp.height())) {
          box.h = static_cast<float>(pp.height());
        }
        SDL_RenderTexture(
            platform.renderer(), portrait->texture->get(), nullptr, &box);
      }
      tgt = name;
      if (govt != nullptr) {
        tgt += " [" + govt->name + "]";
      }
      // Status: suppressed by 0x0200; otherwise armour % (0x0100) or shield %.
      const bool no_status = target_cls != nullptr &&
                             (target_cls->capability_flags & 0x0200U) != 0U;
      if (!no_status && target_cls != nullptr) {
        const bool show_armor = (target_cls->capability_flags & 0x0100U) != 0U;
        const float maximum =
            std::max(1.0F,
                     static_cast<float>(show_armor ? target_cls->base_armor
                                                   : target_cls->base_shield));
        const float current =
            show_armor ? target.armor_points : target.shield_points;
        const int pct = static_cast<int>(
            std::clamp(current / maximum * 100.0F, 0.0F, 999.0F));
        tgt += std::string(show_armor ? " ARM " : " SHD ") +
               std::to_string(pct) + "%";
      }
    } else {
      const std::int16_t sid = state.travel.selected_stellar_id;
      const auto *st = state.scenario.Stellar(sid);
      if (st && !st->name.empty()) {
        tgt = st->name;
        if (NovaTargeting_CanOpenTravelDestinationInteraction(state)) {
          tgt += " [INTERACT]";
        }
      } else {
        tgt = "(none)";
      }
    }
    DrawReadout(platform,
                *font_cache_,
                static_cast<float>(layout_.font_size),
                anchor_panel(layout_.target_status_panel),
                tgt,
                value_color,
                4.0F);
  }

  // Cargo/mission panel: current credits, the used cargo holding and the
  // remaining free fleet cargo space. Mirrors
  // NovaUi_DrawCargoMissionStatusPanel (0x004612c0)'s essential readout: the 6
  // cargo-bin list (only non-empty bins, and their labels are not reconstructed
  // here, TODO(decomp)) and the fleet free-space value
  // (Outfit_ComputeFleetCargoCapacity - cargo+junk total). The escort/command
  // summary line is omitted until fleet state exists.
  {
    const std::int16_t free_space = Outfit_ComputeRemainingCargoSpace(state);
    const std::int16_t used = Outfit_ComputePlayerCargoAndJunkTotal(state);
    char cargo[96];
    std::snprintf(cargo,
                  sizeof(cargo),
                  "CR %d  CARGO %d/%d",
                  state.player.credits,
                  used,
                  used + free_space);
    DrawReadout(platform,
                *font_cache_,
                static_cast<float>(layout_.font_size),
                anchor_panel(layout_.cargo_status_panel),
                cargo,
                value_color,
                4.0F);
  }

  // Transient HUD overlay message (NovaHud_ShowOverlayMessage / the landing &
  // negotiation feedback text): drawn centered near the bottom of the flight
  // viewport while the wall-clock expiry has not passed. Mirrors the original
  // drawing the shared message rect (g_hud_overlay_message_rect) with the
  // cached message colour. See hud_overlay.hpp. The spaceflight loop clears an
  // expired message (HudRenderer::Draw is const over state).
  if (state.hud_overlay.active &&
      (state.hud_overlay.expiry_ms == 0 ||
       SDL_GetTicks() < state.hud_overlay.expiry_ms)) {
    const auto &msg = state.hud_overlay;
    const float size = static_cast<float>(layout_.font_size) + 3.0F;
    const auto logical = platform.logical_playfield_size();
    const float left = 0.0F;
    const float right = logical.x;
    const float baseline = logical.y - 8.0F;
    NovaText_DrawCentered(
        platform,
        *font_cache_,
        NovaFontFamily::kGeneva,
        size,
        kNovaFontStyleRegular,
        SDL_Color{msg.red, msg.green, msg.blue, SDL_ALPHA_OPAQUE},
        left,
        right,
        baseline,
        msg.message);
  }
}

} // namespace game
