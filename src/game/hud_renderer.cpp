#include "hud_renderer.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../pixpat_image.hpp"
#include "../sdl_platform.hpp"
#include "escort_commands.hpp"
#include "hud_overlay.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "radar_panel.hpp"
#include "ship_ai.hpp"
#include "sprite_world.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
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
    const Government *g = state.scenario.GovernmentByIndex(gov);
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

// ---------------------------------------------------------------------------
// Shared panel-drawing helpers. The original renders every gameplay panel
// through a DrawContext whose cursor is the text BASELINE, at ui_scale 1 for
// the shipped 640x480 logical surface, so the scale-rounded offsets in the
// Ghidra helpers (e.g. "param_4 - 0x16 + round(ui_scale * 22)") reduce to
// their constants here.
// ---------------------------------------------------------------------------

// The gameplay-panel STR# pools. Resource_LoadStringEntry (0x004b8ca0) and
// Resource_DrawStringEntry (0x004cd1f0) take 1-BASED entry numbers, and so
// does our NovaHud_LoadStringEntry; every entry recorded below is the value
// seen at the original call site. The shipped strings double as fallbacks
// when the archive is unavailable.
constexpr std::uint16_t kMiscStringsId = 0x7d2; // STR# 2002 "misc strings"

struct MiscStrEntry {
  std::uint16_t entry; // 1-based STR# 0x7d2 entry number
  const char *fallback;
};

// Travel panel (0x0045e400).
constexpr MiscStrEntry kMiscNavSystemOff{0x156, "Nav System Off"};
constexpr MiscStrEntry kMiscStellarNavigation{0x157, "Stellar Navigation"};
constexpr MiscStrEntry kMiscNoDestination{0x158, "No Destination"};
constexpr MiscStrEntry kMiscHyperspace{0x159, "Hyperspace"};
constexpr MiscStrEntry kMiscUnexploredSystem{0x15a, "Unexplored System"};
constexpr MiscStrEntry kMiscDisabled{0x15b, "Disabled"};
constexpr MiscStrEntry kMiscWaiting{0x15c, "Waiting"};
// Target panel (0x0045f530).
constexpr MiscStrEntry kMiscNoTarget{0x15d, "No Target"};
constexpr MiscStrEntry kMiscShieldLabel{0x0d, "Shield:"};
constexpr MiscStrEntry kMiscNoShields{0x0e, "No Shields"};
constexpr MiscStrEntry kMiscShieldsDown{0x0f, "Shields Down"};
constexpr MiscStrEntry kMiscArmorLabel{0x10, "Armor:"};
constexpr MiscStrEntry kMiscNotApplicable{0x18c, "N/A"};
constexpr MiscStrEntry kMiscFighter{0xa9, "Fighter"};
constexpr MiscStrEntry kMiscEscort{0xa8, "Escort"};
// Weapon panel (0x00460ec0).
constexpr MiscStrEntry kMiscNoSecondaryWeapon{0x15e, "No Secondary Weapon"};
// Cargo panel (0x004612c0).
constexpr MiscStrEntry kMiscFree{0x13, "Free:"};
constexpr MiscStrEntry kMiscSpecial{0x14, "Special:"};
constexpr MiscStrEntry kMiscMultiple{0x15, "Multiple"};

[[nodiscard]] std::string MiscString(const MiscStrEntry &entry) {
  if (auto text = NovaHud_LoadStringEntry(kMiscStringsId, entry.entry);
      text && !text->empty()) {
    return *text;
  }
  return std::string(entry.fallback);
}

// The cargo-panel bin labels (DAT_0068cccc[0..5] <- STR# 0xfa3) and the
// short commodity names (DAT_0068d2cc <- STR# 0xfa2). The original's loaders
// fill slot n from 1-based entry n+1 (NovaData_LoadDisplayNamePstringTables
// 0x004c7040), so callers below pass entry = slot + 1.
constexpr std::uint16_t kBinLabelsId = 0xfa3;
constexpr std::uint16_t kBinLabelOverrideBase = 0x24b8;
constexpr std::string_view kBinLabelFallbacks[6] = {
    "Food:", "Ind:", "Med:", "LuxG:", "Met:", "Equ:"};
constexpr std::uint16_t kCommodityShortNamesId = 0xfa2;
constexpr std::uint16_t kCommodityShortNameOverrideBase = 0x23f0;

[[nodiscard]] std::string PoolString(std::uint16_t resource_id,
                                     std::uint16_t entry,
                                     std::string_view fallback) {
  std::optional<std::string> text;
  if (resource_id == kBinLabelsId) {
    text = NovaResources_LoadPatchedStringEntry(
        resource_id, entry, kBinLabelOverrideBase);
  } else if (resource_id == kCommodityShortNamesId) {
    text = NovaResources_LoadPatchedStringEntry(
        resource_id, entry, kCommodityShortNameOverrideBase);
  } else {
    text = NovaHud_LoadStringEntry(resource_id, entry);
  }
  if (text && !text->empty()) {
    return *text;
  }
  return std::string(fallback);
}

// DrawContext_DrawCenteredPascalStringInBounds (0x004622f0) against a panel:
// centered between the panel's left/right at baseline panel.top + offset.
void DrawPanelCentered(SdlPlatform &platform,
                       NovaFontCache &font,
                       float font_size,
                       const HudPanelRect &panel,
                       int top_offset,
                       std::string_view text,
                       const SDL_Color &color) {
  if (text.empty()) {
    return;
  }
  NovaText_DrawCentered(platform,
                        font,
                        NovaFontFamily::kGeneva,
                        font_size,
                        kNovaFontStyleRegular,
                        color,
                        static_cast<float>(panel.left),
                        static_cast<float>(panel.right),
                        static_cast<float>(panel.top + top_offset),
                        text);
}

// One DrawPascalString at an explicit cursor; returns the advanced pen x
// (the original's DrawPascalString advances the DrawContext cursor by the
// string width, which the label+value rows rely on).
float DrawPanelTextAt(SdlPlatform &platform,
                      NovaFontCache &font,
                      float font_size,
                      float x,
                      float baseline_y,
                      std::string_view text,
                      const SDL_Color &color) {
  if (!text.empty()) {
    NovaText_Draw(platform,
                  font,
                  NovaFontFamily::kGeneva,
                  font_size,
                  kNovaFontStyleRegular,
                  color,
                  x,
                  baseline_y,
                  text);
  }
  return x +
         static_cast<float>(font.TextWidth(
             NovaFontFamily::kGeneva, font_size, kNovaFontStyleRegular, text));
}

[[nodiscard]] int
PanelTextWidth(NovaFontCache &font, float font_size, std::string_view text) {
  return font.TextWidth(
      NovaFontFamily::kGeneva, font_size, kNovaFontStyleRegular, text);
}

// "<n>%", "0%" or "100%" exactly as the original composes them: 100% when
// current >= max, else ROUND(current/max*100) with a literal "0%" floor.
[[nodiscard]] std::string StatusPercentText(float current, float maximum) {
  if (current >= maximum) {
    return "100%";
  }
  if (current <= 0.0F) {
    return "0%";
  }
  const int pct = static_cast<int>(
      std::lround(std::clamp(current / maximum * 100.0F, 0.0F, 100.0F)));
  return std::to_string(pct) + "%";
}

// The shared top-right HUD placement transform: every panel translates its
// cached .ntf rect horizontally by RenderOwner.right - DAT_0088c020 (0xc2,
// the 194px cockpit strip width).
[[nodiscard]] HudPanelRect AnchoredPanel(const HudPanelRect &panel,
                                         SdlPlatform &platform) {
  const auto playfield = platform.logical_playfield_size();
  return HudPanel_AnchorTopRight(panel, static_cast<std::int16_t>(playfield.x));
}

[[nodiscard]] const PersDef *PersAt(const ScenarioData &scenario,
                                    std::int16_t pers_slot) {
  if (pers_slot < 0 ||
      pers_slot >= static_cast<std::int16_t>(scenario.pers_defs.size())) {
    return nullptr;
  }
  const PersDef &pers = scenario.pers_defs[static_cast<std::size_t>(pers_slot)];
  return pers.alive ? &pers : nullptr;
}

// Resolves an active mission's ship-name (misn +0x2a pool) or subtitle
// (misn +0x32 pool) string. The original draws the pstring resolved at
// acceptance (Mission_PopulateMissionSlotFromDef 0x0043f8c0: a random 1-based
// entry drawn from the pool via Resource_LoadStringEntry); we stored the
// (pool, entry) pair, so re-resolving at draw time yields the same text.
// The stored entry is already the original's 1-based value
// (NovaRandom_Range(count) + 1, Mission_PopulateMissionSlotFromDef 0x0043f8c0).
[[nodiscard]] std::string MissionShipPoolString(const ActiveMission &mission,
                                                bool name_pool) {
  const std::int16_t pool_id = name_pool ? mission.special_ship_name_string_id
                                         : mission.random_text_string_id;
  const std::int16_t entry =
      name_pool ? mission.special_ship_name_entry : mission.random_text_entry;
  if (pool_id < 0 || entry < 1) {
    return {};
  }
  auto text = NovaHud_LoadStringEntry(static_cast<std::uint16_t>(pool_id),
                                      static_cast<std::uint16_t>(entry));
  return text ? *text : std::string{};
}

// Whether any adjacent, non-hidden system lies within the ship's travel
// range (the travel-panel jump-title colour test). The decompiled loop reads
// g_stellar_defs[adjacency].availability_flags with a 0x3000 mask and a
// zeroed local for one distance endpoint (Ghidra field aliasing); the port
// tests system visibility and the ship-to-system-centre distance.
// TODO(decomp): re-derive the exact table/endpoint once SystemDef adjacency
// typing is settled.
// Thousands-grouped credits (DrawContext_DrawGroupedUInt).
[[nodiscard]] std::string GroupedNumber(std::int32_t value) {
  std::string digits = std::to_string(value);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) {
      out.push_back(',');
    }
    out.push_back(digits[i]);
  }
  return out;
}

void HudRenderer::Draw(SdlPlatform &platform,
                       const GameState &state,
                       bool force_empty_radar) {
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

  // Stellar radar panel (NovaUi_DrawStellarRadarPanel 0x0045d600): composites
  // onto the cockpit art before the other panels refresh.
  DrawRadarPanel(platform, state, force_empty_radar);

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

  // The four text panels (Ghidra NovaUi_Draw*StatusPanel 0x0045e400 /
  // 0x00460ec0 / 0x0045f530 / 0x004612c0). The original blits a saved clean
  // cockpit backdrop into each panel rect before drawing text; this port
  // redraws the full cockpit PICT above instead, which covers the same
  // restore. Label/value colours are the .ntf palette slots +0x00/+0x04
  // (NovaUi_SetupGameplayPanelColors 0x0045cfc0).
  const SDL_Color value_color = ColorOf(layout_.color_word[0]);
  const SDL_Color label_color = ColorOf(layout_.color_word[1]);
  DrawTravelPanel(platform, state, value_color, label_color);
  DrawWeaponPanel(platform, state, value_color, label_color);
  DrawTargetPanel(platform, state, value_color, label_color);
  DrawCargoPanel(platform, state, value_color, label_color);
  DrawEscortCommandsPanel(platform, state, value_color, label_color);
  // Transient HUD overlay message (NovaHud_ShowOverlayMessage / the landing &
  // negotiation feedback text). Mirrors the original's shared message rect
  // (Ghidra 0x004AF020 FUN_004af020 tail runs inline here): left =
  // window left + 25, bottom = window bottom - 5, height 26 * ui_scale (the
  // 640x480 logical band spans x 25..width-244, y height-31..height-5 -- the
  // lower-left). NovaHud_ShowCachedOverlayMessage (0x0047e430) explicitly
  // selects font id 0 (Chicago) and size 0xc; the filled-rect helper starts
  // that text at the rect top, represented here by a 12px baseline offset.
  // The band right edge (--DAT_0088c020-50) is provisional.
  // Multi-line word wrap is TODO(decomp); current messages are single-line.
  if (state.hud_overlay.active &&
      (state.hud_overlay.expiry_ms == 0 ||
       platform.gameplay_ticks_ms() < state.hud_overlay.expiry_ms)) {
    const auto &msg = state.hud_overlay;
    constexpr float kOverlayFontSize = 12.0F;
    const auto logical = platform.logical_playfield_size();
    const float left = 25.0F;
    const float baseline = logical.y - 5.0F - 26.0F + 12.0F;
    NovaText_Draw(platform,
                  *font_cache_,
                  NovaFontFamily::kChicago,
                  kOverlayFontSize,
                  kNovaFontStyleRegular,
                  SDL_Color{msg.red, msg.green, msg.blue, SDL_ALPHA_OPAQUE},
                  left,
                  baseline,
                  msg.message);
  }
}

// ---------------------------------------------------------------------------
// Ghidra 0x0045e400 NovaUi_DrawTravelStatusPanel.
//
// Three states keyed off the player's travel sequence:
//   * idle (travel_transfer_mode == -1): the "Nav System Off" label centered
//     at baseline top+22.
//   * transfer (mode 2): "Stellar Navigation" title at top+12 plus the
//     targeted stellar's display name (or "No Destination") at top+29.
//   * jump (mode 3): "Hyperspace" title at top+12 plus the destination
//     system's name ("Unexplored System" while undiscovered) at top+29.
// Titles draw in the label colour, switching to the value colour while the
// station-hold timer runs; the jump destination draws in the value colour
// only while the ship is beyond the no-jump radius (NovaTravel_PlayerIn-
// JumpRange, the same probe as the flight-tail range cue) or the hold has
// started, and fuel covers a jump -- label colour otherwise. The port
// derives the mode from the explicit TravelState (the original reads the
// player ship's travel_transfer_mode latch).
// ---------------------------------------------------------------------------
void HudRenderer::DrawTravelPanel(SdlPlatform &platform,
                                  const GameState &state,
                                  const SDL_Color &value_color,
                                  const SDL_Color &label_color) {
  const HudPanelRect panel =
      AnchoredPanel(layout_.travel_status_panel, platform);
  if (!panel.valid()) {
    return;
  }
  NovaFontCache &font = *font_cache_;
  const float font_size = static_cast<float>(layout_.font_size);
  const auto &travel = state.travel;

  if (travel.engaging || travel.hyperspace_mode ||
      state.player.travel_transfer_mode == 3) {
    // Jump sequence / plotted destination (original travel_transfer_mode ==
    // 3): the "Hyperspace" title plus the destination system's name.
    const bool holding =
        state.travel.jump_phase == TravelState::JumpPhase::kHold;
    DrawPanelCentered(platform,
                      font,
                      font_size,
                      panel,
                      12,
                      MiscString(kMiscHyperspace),
                      holding ? value_color : label_color);

    std::int16_t destination = -1;
    if (travel.engaging) {
      destination = travel.destination_system_id;
      if (destination < 0 && travel.travel_slot >= 0 &&
          travel.travel_slot < 16) {
        if (const System *current =
                state.scenario.System(static_cast<std::int16_t>(
                    state.player.current_system_id + 0x80))) {
          destination =
              current->links[static_cast<std::size_t>(travel.travel_slot)];
        }
      }
    } else {
      destination = travel.starmap_destination_system_id;
    }

    const System *system =
        destination >= 0 ? state.scenario.System(
                               static_cast<std::int16_t>(destination + 0x80))
                         : nullptr;
    if (system == nullptr) {
      DrawPanelCentered(platform,
                        font,
                        font_size,
                        panel,
                        29,
                        MiscString(kMiscNoDestination),
                        label_color);
      return;
    }
    // Undiscovered systems hide their name (SystemDef.discovery_state < 1;
    // the original's has_explored_flag is load-time-true and cannot serve as
    // this gate).
    const std::string name = system->discovery_state > 0
                                 ? system->name
                                 : MiscString(kMiscUnexploredSystem);
    const bool fueled = state.player.fuel_points >= 100.0F;
    const bool in_jump_range = NovaTravel_PlayerInJumpRange(state);
    const bool dimmed = (!in_jump_range || !fueled) && !holding;
    DrawPanelCentered(platform,
                      font,
                      font_size,
                      panel,
                      29,
                      name,
                      dimmed ? label_color : value_color);
    return;
  }

  if (travel.selected_stellar_id >= 0) {
    // Travel transfer (original travel_transfer_mode == 2).
    const bool holding = state.player.ai_station_hold_timer > 0.0F;
    DrawPanelCentered(platform,
                      font,
                      font_size,
                      panel,
                      12,
                      MiscString(kMiscStellarNavigation),
                      holding ? value_color : label_color);
    const Stellar *stellar = state.scenario.Stellar(travel.selected_stellar_id);
    if (stellar == nullptr || stellar->name.empty()) {
      DrawPanelCentered(platform,
                        font,
                        font_size,
                        panel,
                        29,
                        MiscString(kMiscNoDestination),
                        label_color);
    } else {
      DrawPanelCentered(
          platform, font, font_size, panel, 29, stellar->name, value_color);
    }
    return;
  }

  DrawPanelCentered(platform,
                    font,
                    font_size,
                    panel,
                    22,
                    MiscString(kMiscNavSystemOff),
                    label_color);
}

// ---------------------------------------------------------------------------
// Ghidra 0x00460ec0 NovaUi_DrawActiveWeaponAmmoPanel. One centered row at
// baseline top+12: the idle STR# label when no bank is active (label colour),
// otherwise the active bank's name in the value colour, suffixed
// " - <ammo>" for ammo-driven weapons (energy weapons: ammo_type -1 or
// flags_secondary 0x40; mode <-999 codes also draw the bare name).
// ---------------------------------------------------------------------------
void HudRenderer::DrawWeaponPanel(SdlPlatform &platform,
                                  const GameState &state,
                                  const SDL_Color &value_color,
                                  const SDL_Color &label_color) {
  const HudPanelRect panel = AnchoredPanel(layout_.weapon_ammo_panel, platform);
  if (!panel.valid()) {
    return;
  }
  NovaFontCache &font = *font_cache_;
  const float font_size = static_cast<float>(layout_.font_size);

  const std::int16_t bank = state.player.active_weapon_bank_slot;
  const Weapon *weapon =
      bank >= 0 ? state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80))
                : nullptr;
  if (bank < 0 || weapon == nullptr) {
    DrawPanelCentered(platform,
                      font,
                      font_size,
                      panel,
                      12,
                      MiscString(kMiscNoSecondaryWeapon),
                      label_color);
    return;
  }

  std::string text = weapon->name;
  const bool energy_weapon =
      weapon->ammo_type == -1 || (weapon->flags_secondary & 0x40U) != 0U;
  const bool special_mode = weapon->ammo_type < -999;
  if (!energy_weapon && !special_mode) {
    if (const std::int16_t ammo = NovaWeapon_BankAmmoCount(state, bank);
        ammo >= 0) {
      text += " - ";
      text += std::to_string(ammo);
    }
  }
  DrawPanelCentered(platform, font, font_size, panel, 12, text, value_color);
}

// ---------------------------------------------------------------------------
// Ghidra 0x0045f530 NovaUi_DrawTargetStatusPanel.
//
// With no primary target: the centered "No Target" label at baseline
// top+47. With one: centered name at top+16 (mission-ship STR# name, else
// p}brs display name, else ship-class name), centered subtitle at top+29 in
// the .ntf secondary font size (mission subtitle pool, p}brs special-ship
// name, else the class Subtitle), the 128x64 clone-source portrait blitted
// into a rect centered on the panel, a bottom-left shield/armor status row
// at baseline bottom-6 starting at left+5, and a bottom-right government (or
// fighter/escort) footer right-aligned at right-7. The original's AI/debug
// overlay (g_target_status_debug_overlay_active) is not reproduced.
// TODO(decomp(0x0045f530)) skipped: debug overlay rows (gated by a debug
// config byte, never set in normal play).
// ---------------------------------------------------------------------------
void HudRenderer::DrawTargetPanel(SdlPlatform &platform,
                                  const GameState &state,
                                  const SDL_Color &value_color,
                                  const SDL_Color &label_color) {
  const HudPanelRect panel =
      AnchoredPanel(layout_.target_status_panel, platform);
  if (!panel.valid()) {
    return;
  }
  NovaFontCache &font = *font_cache_;
  const float font_size = static_cast<float>(layout_.font_size);
  // DAT_0073567a: the .ntf SubtitleSize (+0xa2).
  const float subtitle_size = layout_.font_size_2 > 0
                                  ? static_cast<float>(layout_.font_size_2)
                                  : font_size;

  const std::int16_t slot = state.player.primary_target_ship_slot;
  if (slot < 0 || !state.SlotInRange(static_cast<std::size_t>(slot))) {
    DrawPanelCentered(platform,
                      font,
                      font_size,
                      panel,
                      47,
                      MiscString(kMiscNoTarget),
                      label_color);
    return;
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(slot));
  const ShipClass *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(target.ship_class_id + 0x80));
  const PersDef *pers = PersAt(state.scenario, target.pers_def_slot);
  const bool has_fleet =
      target.mission_fleet_slot >= 0 &&
      target.mission_fleet_slot <
          static_cast<std::int16_t>(state.active_missions.size());
  const ActiveMission *mission =
      has_fleet ? &state.active_missions[static_cast<std::size_t>(
                      target.mission_fleet_slot)]
                : nullptr;

  // Top name: mission ship name > p}brs display name > class name.
  std::string name;
  if (mission != nullptr) {
    name = MissionShipPoolString(*mission, /*name_pool=*/true);
  }
  if (name.empty()) {
    if (target.pers_def_slot >= 0 && pers != nullptr) {
      name = pers->display_name;
    } else {
      name = ship_class != nullptr ? ship_class->display_name : "?";
    }
  }
  DrawPanelCentered(platform, font, font_size, panel, 16, name, value_color);

  // Portrait: 128x64 rect centered on the panel (FUN_008747f3), blitting the
  // clone-source class's target PICT (DAT_00596d44 table). Drawn after the
  // name line but BEFORE the subtitle, exactly as the original orders the
  // three (NovaUi_DrawTargetStatusPanel 0x0045f530): the class-variant
  // subtitle draws over the portrait.
  if (ship_class != nullptr) {
    if (const auto *portrait =
            TargetPortrait(platform, state.scenario, target.ship_class_id)) {
      const float center_x =
          static_cast<float>(panel.left + panel.right + 1) / 2.0F;
      const float center_y =
          static_cast<float>(panel.top + panel.bottom + 1) / 2.0F;
      const SDL_FRect box{center_x - 64.0F, center_y - 32.0F, 128.0F, 64.0F};
      SDL_RenderTexture(
          platform.renderer(), portrait->texture->get(), nullptr, &box);
    }
  }

  // Subtitle: mission subtitle pool > p}brs special-ship name > class
  // Subtitle (each level falls through when its source is empty).
  std::string subtitle;
  if (mission != nullptr) {
    subtitle = MissionShipPoolString(*mission, /*name_pool=*/false);
  } else if (pers != nullptr) {
    subtitle = pers->special_ship_name;
  }
  if (subtitle.empty() && ship_class != nullptr) {
    subtitle = ship_class->subtitle;
  }
  DrawPanelCentered(
      platform, font, subtitle_size, panel, 29, subtitle, value_color);

  const Government *government =
      target.faction_or_government_id >= 0
          ? state.scenario.Government(static_cast<std::int16_t>(
                target.faction_or_government_id + 0x80))
          : nullptr;

  // ---- Bottom-left status row (baseline bottom-6) -----------------------
  if (target.pers_def_slot == 0x3ff) {
    // Shareware-Enforcer sentinel: the "Df " static pstring (DAT_0056c740).
    DrawPanelCentered(platform,
                      font,
                      font_size,
                      panel,
                      panel.bottom - panel.top - 6,
                      "Df ",
                      value_color);
    return;
  }
  if (ship_class != nullptr && (ship_class->capability_flags & 0x200U) != 0U) {
    // Flags 0x200: suppress the status line, show the government centered.
    if (government != nullptr) {
      DrawPanelCentered(platform,
                        font,
                        font_size,
                        panel,
                        panel.bottom - panel.top - 6,
                        government->target_code,
                        label_color);
    }
    return;
  }
  if (pers != nullptr && pers->shield_armor_scale < 0.0F) {
    // p}brs ShieldMod below zero: government name replaces the status row.
    if (government != nullptr) {
      DrawPanelCentered(platform,
                        font,
                        font_size,
                        panel,
                        panel.bottom - panel.top - 6,
                        government->target_code,
                        label_color);
    }
    return;
  }

  const float status_y = static_cast<float>(panel.bottom - 6);
  const float status_x = static_cast<float>(panel.left + 5);
  const bool fire_restricted = NovaAiShip_IsDisabled(state, target);
  const float shields = target.shield_points;
  if (fire_restricted) {
    // Special mission ships held for pickup read "Waiting" once their armor
    // clears the decoded fraction; everything else reads "Disabled".
    // TODO(decomp): the original also gates on the mission's
    // special_ship_attacking latch and the target's boarded_target_latch;
    // neither field is modelled yet.
    bool waiting = false;
    if (mission != nullptr && mission->ship_goal == 5 &&
        state
            .active_mission_runtime_flags[static_cast<std::size_t>(
                target.mission_fleet_slot)]
            .is_active &&
        ship_class != nullptr) {
      const float max_armor = static_cast<float>(ship_class->base_armor);
      const float armor = target.armor_points;
      // The non-0x10 branch's decompiled threshold is negative (max * -0.241
      // <= armor * 100), i.e. always satisfied for non-negative armor.
      waiting = (ship_class->capability_flags & 0x10U) != 0U
                    ? max_armor * 10.0F <= armor * 100.0F
                    : max_armor * -0.241F <= armor * 100.0F;
    }
    DrawPanelTextAt(platform,
                    font,
                    font_size,
                    status_x,
                    status_y,
                    MiscString(waiting ? kMiscWaiting : kMiscDisabled),
                    value_color);
  } else if (shields > 0.0F) {
    // "Shield: <n>%" (100% once current reaches the computed maximum).
    float pen = DrawPanelTextAt(platform,
                                font,
                                font_size,
                                status_x,
                                status_y,
                                MiscString(kMiscShieldLabel) + " ",
                                label_color);
    const float max_shield = ship_class != nullptr
                                 ? static_cast<float>(ship_class->base_shield)
                                 : 0.0F;
    // TODO(decomp): Ship_ComputeShipMaxShieldPoints includes outfit mods;
    // the port reads the class base until the NPC outfit pipeline exists.
    const std::string pct =
        max_shield > 0.0F ? StatusPercentText(shields, max_shield) : "100%";
    DrawPanelTextAt(platform, font, font_size, pen, status_y, pct, value_color);
  } else if (ship_class != nullptr &&
             (ship_class->capability_flags & 0x100U) != 0U) {
    // Shields down + Flags 0x100: armor percentage readout.
    float pen = DrawPanelTextAt(platform,
                                font,
                                font_size,
                                status_x,
                                status_y,
                                MiscString(kMiscArmorLabel) + " ",
                                label_color);
    const float max_armor = static_cast<float>(ship_class->base_armor);
    const std::string pct =
        max_armor > 0.0F ? StatusPercentText(target.armor_points, max_armor)
                         : MiscString(kMiscNotApplicable);
    DrawPanelTextAt(platform, font, font_size, pen, status_y, pct, value_color);
  } else {
    // Shields down: "No Shields" when the class carries no shield generator
    // at all, "Shields Down" otherwise.
    const bool no_shield_generator =
        ship_class == nullptr || ship_class->base_shield < 1;
    DrawPanelTextAt(
        platform,
        font,
        font_size,
        status_x,
        status_y,
        MiscString(no_shield_generator ? kMiscNoShields : kMiscShieldsDown),
        value_color);
  }

  // ---- Bottom-right footer ----------------------------------------------
  if (government != nullptr) {
    // The original draws the g_government_name_table entry here -- the
    // gövt payload +0x44 "TargetCode" short string (" Fed.", "Auroran"),
    // not the resource record name (NovaUi_DrawTargetStatusPanel 0x0045f530).
    const std::string &text = government->target_code;
    NovaText_Draw(platform,
                  font,
                  NovaFontFamily::kGeneva,
                  font_size,
                  kNovaFontStyleRegular,
                  label_color,
                  static_cast<float>(panel.right - 7 -
                                     PanelTextWidth(font, font_size, text)),
                  status_y,
                  text);
  } else if ((target.squad_leader_ship_slot == 0 ||
              target.post_hit_mode_hint >= 0) &&
             (target.ai_behavior_code == 5 || target.post_hit_mode_hint == 0)) {
    const bool light_ship =
        ship_class != nullptr && ship_class->mass_tons < 100;
    const std::string text =
        MiscString(light_ship ? kMiscFighter : kMiscEscort);
    NovaText_Draw(platform,
                  font,
                  NovaFontFamily::kGeneva,
                  font_size,
                  kNovaFontStyleRegular,
                  label_color,
                  static_cast<float>(panel.right - 7 -
                                     PanelTextWidth(font, font_size, text)),
                  status_y,
                  text);
  }
}

// ---------------------------------------------------------------------------
// Ghidra 0x004612c0 NovaUi_DrawCargoMissionStatusPanel.
//
// Left-aligned rows at fixed offsets from the panel origin:
//   * six cargo-bin rows (labels STR# 0xfa3[i], values at x+41) starting at
//     top+12 on a 14px pitch, only when the bin holds tonnage;
//   * "Free: <n>" (label x+77, value x+110, top+12) with the clamped fleet
//     free cargo space;
//   * "Special:" (x+77, top+30) with the carried mission-cargo commodity
//     short name (STR# 0xfa2), a junk-def name when exactly one junk type is
//     held, or "Multiple" (value x+87, top+46);
//   * "Cr:" (x+77, top+66) with the grouped credits value (x+87, top+82).
// ---------------------------------------------------------------------------
void HudRenderer::DrawCargoPanel(SdlPlatform &platform,
                                 const GameState &state,
                                 const SDL_Color &value_color,
                                 const SDL_Color &label_color) {
  const HudPanelRect panel =
      AnchoredPanel(layout_.cargo_status_panel, platform);
  if (!panel.valid()) {
    return;
  }
  NovaFontCache &font = *font_cache_;
  const float font_size = static_cast<float>(layout_.font_size);
  const float left = static_cast<float>(panel.left);
  const float top = static_cast<float>(panel.top);

  // Cargo-bin rows (cursor offsets are raw constants, no ui_scale).
  for (std::size_t i = 0; i < state.inventory.cargo_bins.size(); ++i) {
    const std::int16_t count = state.inventory.cargo_bins[i];
    if (count <= 0) {
      continue;
    }
    const std::string label = PoolString(
        kBinLabelsId, static_cast<std::uint16_t>(i + 1), kBinLabelFallbacks[i]);
    const float y = top + static_cast<float>((i + 1) * 14 - 2);
    DrawPanelTextAt(
        platform, font, font_size, left + 3.0F, y, label, label_color);
    DrawPanelTextAt(platform,
                    font,
                    font_size,
                    left + 41.0F,
                    y,
                    std::to_string(count),
                    value_color);
  }

  // Free fleet cargo space: capacity - carried (the original 0x004612c0
  // computes this inline, NOT via Player_ComputeRemainingCargoSpace, which
  // has the separate mass-bounded mission-reward semantics).
  const std::int32_t free_space = std::max<std::int32_t>(
      0,
      static_cast<std::int32_t>(Player_ComputeFleetCargoCapacity(state)) -
          static_cast<std::int32_t>(Player_ComputeCargoAndJunkTotal(state)));
  DrawPanelTextAt(platform,
                  font,
                  font_size,
                  left + 77.0F,
                  top + 12.0F,
                  MiscString(kMiscFree),
                  label_color);
  DrawPanelTextAt(platform,
                  font,
                  font_size,
                  left + 110.0F,
                  top + 12.0F,
                  std::to_string(free_space),
                  value_color);

  // "Special:" row: carried mission cargo / junk summary.
  std::int16_t cargo_missions = 0;
  std::int16_t first_cargo_type = -1;
  for (std::size_t i = 0; i < state.active_missions.size(); ++i) {
    const auto &flags = state.active_mission_runtime_flags[i];
    const auto &mission = state.active_missions[i];
    if (flags.is_active && mission.carrying_resources &&
        mission.cargo_type_id >= 0 && mission.cargo_type_id < 0x4f) {
      if (cargo_missions == 0) {
        first_cargo_type = mission.cargo_type_id;
      }
      ++cargo_missions;
    }
  }
  std::int16_t junk_types = 0;
  std::int16_t first_junk_index = -1;
  for (std::size_t i = 0; i < state.inventory.junk_counts.size(); ++i) {
    if (state.inventory.junk_counts[i] > 0) {
      if (junk_types == 0) {
        first_junk_index = static_cast<std::int16_t>(i);
      }
      ++junk_types;
    }
  }
  if (cargo_missions > 0 || junk_types > 0) {
    DrawPanelTextAt(platform,
                    font,
                    font_size,
                    left + 77.0F,
                    top + 30.0F,
                    MiscString(kMiscSpecial),
                    label_color);
    std::string value;
    if (cargo_missions == 1) {
      value = PoolString(kCommodityShortNamesId,
                         static_cast<std::uint16_t>(first_cargo_type + 1),
                         "cargo");
    } else if (junk_types == 1) {
      // The single held junk type's short status-bar label (STR# 0xfa3-style
      // g_junk_defs +0x128 "Abbrev" field; the scenario decoder exposes it as
      // JunkDef::abbrev).
      if (const JunkDef *junk = state.scenario.Junk(
              static_cast<std::int16_t>(0x80 + first_junk_index))) {
        value = junk->abbrev;
      }
    } else {
      value = MiscString(kMiscMultiple);
    }
    DrawPanelTextAt(platform,
                    font,
                    font_size,
                    left + 87.0F,
                    top + 46.0F,
                    value,
                    value_color);
  }

  // Credits row: the label is the "credits" pstring (DAT_0072f1cc = STR#
  // 0x7d2 pool 0x20) whose first character the original translates through
  // the input map, so the leading letter always shows the key bound to the
  // credits command; the port keeps the literal 'c'.
  // TODO(decomp(0x004612c0)) skipped: NovaCommand_TranslateByInputMap key
  // translation (input-map table not reconstructed).
  std::string credits_label = PoolString(kMiscStringsId, 0x21, "credits");
  if (!credits_label.empty()) {
    credits_label[0] = 'c';
  }
  credits_label += ":";
  DrawPanelTextAt(platform,
                  font,
                  font_size,
                  left + 77.0F,
                  top + 66.0F,
                  credits_label,
                  label_color);
  DrawPanelTextAt(platform,
                  font,
                  font_size,
                  left + 87.0F,
                  top + 82.0F,
                  GroupedNumber(state.player.credits),
                  value_color);
}

// ---------------------------------------------------------------------------
// Ghidra 0x0045d600 NovaUi_DrawStellarRadarPanel (with 0x0045d0a0 the rebuild/
// backdrop path, 0x004ba350 the blip disc plot, 0x004bbdc0 the static tiling
// and 0x0045d320 the blink-poll cadence).
//
// The original renders the panel into an offscreen buffer that is blitted
// back onto the gameplay surface; this port composites directly, redrawing
// the cockpit PICT every frame where the original re-blits the cached backing
// rect (RebuildStellarRadarPanel's backdrop restore). Everything else mirrors
// the decompilation: 1/32 position scale, blip colour/size tiers, the blink
// phase, the far-from-origin arrow and the interference static.
//
// Deliberate rendering divergence (improvement): the original only recomposites
// the panel when DAT_00596d25 is dirty, so in normal flight every contact, the
// origin arrow, and the target blink step once per >=15-tick (250 ms) poll. The
// port recomputes contacts and the arrow every presentation frame instead, so
// blips track live positions smoothly rather than at ~4 Hz. The blink phase and
// the interference static pattern are still latched to the poll (see the poll
// block below) so their cadence matches the original; the per-frame contact
// pass is cosmetic and does not feed gameplay state.
// ---------------------------------------------------------------------------
namespace {

// Fixed radar constants (DAT_005756d0 / b0 / b8 / bc / c0, DAT_00733b74).
constexpr double kRadarScale = 0.03125;            // contact scale (1/32)
constexpr double kRadarHomeDistanceSq = 7000000.0; // origin-arrow threshold
constexpr float kRadarArrowShaftNear = 25.0F;
constexpr float kRadarArrowShaftFar = 50.0F;
constexpr float kRadarArrowWingLength = 6.0F;
constexpr float kRadarArrowWingDeg = 135.0F; // 0x87 bearing offsets
constexpr std::int16_t kRadarDefaultFrameHeight = 0x20;
// The interference static is one of the ten preloaded 'ppat' resources
// 128..137 (DAT_00733b7c); DrawContext_TileImageInRect tiles the 64x64 source.
constexpr std::uint16_t kRadarStaticFirstPpatId = 128;
constexpr std::uint32_t kRadarBlinkHalfPeriodMs = 250; // 15 ticks at 60 Hz

// The original's ROUND(value) + (fraction > 0) pattern resolves to ceil for
// non-integers, identity for integers (FIST round-to-nearest then bump).
[[nodiscard]] int CeilRadar(float value) {
  const int truncated = static_cast<int>(value);
  return value > static_cast<float>(truncated) ? truncated + 1 : truncated;
}

// Rect_Intersect (0x004b8df0) is a QD SectRect: it clips `blip` to
// `radar` and reports whether any area survives (empty when edges touch).
[[nodiscard]] bool IntersectRadarRect(HudPanelRect &blip,
                                      const HudPanelRect &radar) {
  blip.left = std::max(blip.left, radar.left);
  blip.top = std::max(blip.top, radar.top);
  blip.right = std::min(blip.right, radar.right);
  blip.bottom = std::min(blip.bottom, radar.bottom);
  return blip.left < blip.right && blip.top < blip.bottom;
}

void DrawRadarPoint(SDL_Renderer *renderer,
                    int x,
                    int y,
                    const HudPanelRect &clip) {
  if (x < clip.left || x > clip.right || y < clip.top || y > clip.bottom) {
    return;
  }
  SDL_RenderPoint(renderer, static_cast<float>(x), static_cast<float>(y));
}

// Ghidra 0x004ba350 DrawContext_DrawCircleInRect with the draw context's 1x1
// pixel scale: a midpoint-circle outline (8-way symmetric, single-pixel plot)
// inscribed in the rect, radius (bottom-top)/2, centre ((left+right+1)/2,
// (top+bottom+1)/2).
void DrawRadarDisc(SDL_Renderer *renderer, const HudPanelRect &rect) {
  const int cx = (rect.left + rect.right + 1) / 2;
  const int cy = (rect.top + rect.bottom + 1) / 2;
  const int radius = (rect.right - rect.left) / 2;
  int x = 0;
  int y = radius;
  int d = 1 - radius;
  while (x <= y) {
    DrawRadarPoint(renderer, cx + x, cy + y, rect);
    DrawRadarPoint(renderer, cx - x, cy + y, rect);
    DrawRadarPoint(renderer, cx + x, cy - y, rect);
    DrawRadarPoint(renderer, cx - x, cy - y, rect);
    DrawRadarPoint(renderer, cx + y, cy + x, rect);
    DrawRadarPoint(renderer, cx - y, cy + x, rect);
    DrawRadarPoint(renderer, cx + y, cy - x, rect);
    DrawRadarPoint(renderer, cx - y, cy - x, rect);
    if (d < 0) {
      d += 2 * x + 3;
    } else {
      d += 2 * (x - y) + 5;
      --y;
    }
    ++x;
  }
}

// DrawContext_FrameRect16WithCurrentColor: QuickDraw FrameRect with exclusive
// right/bottom bounds. The radar's point rect inset by one therefore becomes
// a 2x2 contact.
void DrawRadarBox(SDL_Renderer *renderer, const HudPanelRect &rect) {
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  const SDL_FRect top{static_cast<float>(rect.left),
                      static_cast<float>(rect.top),
                      static_cast<float>(width),
                      1.0F};
  const SDL_FRect bottom{static_cast<float>(rect.left),
                         static_cast<float>(rect.bottom - 1),
                         static_cast<float>(width),
                         1.0F};
  const SDL_FRect left_edge{static_cast<float>(rect.left),
                            static_cast<float>(rect.top),
                            1.0F,
                            static_cast<float>(height)};
  const SDL_FRect right_edge{static_cast<float>(rect.right - 1),
                             static_cast<float>(rect.top),
                             1.0F,
                             static_cast<float>(height)};
  SDL_RenderFillRect(renderer, &top);
  SDL_RenderFillRect(renderer, &bottom);
  SDL_RenderFillRect(renderer, &left_edge);
  SDL_RenderFillRect(renderer, &right_edge);
}

// Math_BearingFromPointToPoint (0x0043b670) / Math_AddPolarVelocity
// (0x0043b4a0) conventions: heading 0 = up, clockwise, x += sin, y -= -cos.
[[nodiscard]] float
RadarBearingDeg(float from_x, float from_y, float to_x, float to_y) {
  constexpr float kRadToDeg = 180.0F / 3.14159265358979F;
  const float deg = std::atan2(to_x - from_x, -(to_y - from_y)) * kRadToDeg;
  return deg < 0.0F ? deg + 360.0F : deg;
}

void RadarPolarOffset(float bearing_deg, float distance, float &x, float &y) {
  constexpr float kDegToRad = 3.14159265358979F / 180.0F;
  x += std::sin(bearing_deg * kDegToRad) * distance;
  y -= std::cos(bearing_deg * kDegToRad) * distance;
}

} // namespace

// Ghidra 0x0049E430 Ui_DrawTargetCategoryPanel.
// The in-flight "Escort Commands" overlay (header STR# 0x7d2 0x85). Five rows
// numbered 1..5: All
// Ships (-1) then one per class_category group (0 Fighters .. 3 Freighters,
// 0x8c..0x8f); rows whose group has no attached ships draw dimmed, the
// selected row draws highlighted, and present groups show their current
// order word (0x91 Defend / 0x92 Attack / 0x93 Hold Position / 0x94 Return
// to Hangar). The original draws this as an opaque boxed strip over the
// gameplay view; the clean-room centers a boxed panel in the viewport.
void HudRenderer::DrawEscortCommandsPanel(SdlPlatform &platform,
                                          const GameState &state,
                                          const SDL_Color &value_color,
                                          const SDL_Color &label_color) {
  const auto &escort = state.escort;
  if (escort.panel_timer <= 0) {
    return;
  }
  SDL_Renderer *renderer = platform.renderer();
  NovaFontCache &font = *font_cache_;
  const float font_size = static_cast<float>(layout_.font_size);
  constexpr int kRowHeight = 18;
  constexpr int kRowBaseline = 12;
  constexpr float kBoxWidth = 220.0F;
  constexpr float kBoxHeight = 34.0F + 5.0F * kRowHeight;

  const auto logical = platform.logical_playfield_size();
  const float left = (logical.x - kBoxWidth) / 2.0F;
  const float top = 40.0F;

  // Opaque backing + frame (the original's FillRect16/FrameRect16 pair).
  const SDL_FRect box{left, top, kBoxWidth, kBoxHeight};
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &box);
  SDL_SetRenderDrawColor(
      renderer, label_color.r, label_color.g, label_color.b, SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &box);

  if (auto title = NovaHud_LoadStringEntry(0x7d2, 0x85)) {
    NovaText_DrawCentered(platform,
                          font,
                          NovaFontFamily::kGeneva,
                          font_size,
                          kNovaFontStyleRegular,
                          value_color,
                          left,
                          left + kBoxWidth,
                          top + 8.0F,
                          *title);
  }

  struct RowDef {
    std::uint16_t name_entry;
    std::int16_t category; // -1 = All Ships
  };

  static constexpr RowDef kRows[5] = {
      {0x90, -1}, {0x8c, 0}, {0x8d, 1}, {0x8e, 2}, {0x8f, 3}};
  // Order word per code 1..4 (0x95 Formation covers code 0, not shown).
  static constexpr std::uint16_t kOrderEntries[5] = {
      0x95, 0x91, 0x92, 0x94, 0x93};

  for (int row = 0; row < 5; ++row) {
    const float row_top = top + 26.0F + row * kRowHeight;
    const auto &def = kRows[row];
    const bool selected = escort.selected_category == def.category;
    const bool present =
        def.category < 0 || Player_HasEscortGroup(state, def.category);

    if (selected) {
      const SDL_FRect highlight{left + 3.0F,
                                row_top,
                                kBoxWidth - 6.0F,
                                static_cast<float>(kRowHeight)};
      SDL_SetRenderDrawColor(
          renderer, label_color.r, label_color.g, label_color.b, 48);
      SDL_RenderFillRect(renderer, &highlight);
    }

    const SDL_Color &color = present ? value_color : label_color;
    const float baseline = row_top + static_cast<float>(kRowBaseline);
    float pen = DrawPanelTextAt(platform,
                                font,
                                font_size,
                                left + 10.0F,
                                baseline,
                                std::to_string(row + 1),
                                color);
    pen = DrawPanelTextAt(platform,
                          font,
                          font_size,
                          pen + 4.0F,
                          baseline,
                          MiscString({def.name_entry, ""}),
                          color);
    if (def.category >= 0 && present) {
      const std::int16_t order =
          escort.group_command[static_cast<std::size_t>(def.category)];
      if (order > 0 && order <= 4) {
        if (auto word = NovaHud_LoadStringEntry(
                0x7d2, kOrderEntries[static_cast<std::size_t>(order)])) {
          DrawPanelTextAt(platform,
                          font,
                          font_size,
                          pen + 10.0F,
                          baseline,
                          *word,
                          label_color);
        }
      }
    }
  }
}

void HudRenderer::DrawRadarPanel(SdlPlatform &platform,
                                 const GameState &state,
                                 bool force_empty) {
  // RadarArea validity gate (DAT_007355de < DAT_007355e2 && dc < e0).
  if (!(layout_.radar_panel.left < layout_.radar_panel.right) ||
      !(layout_.radar_panel.top < layout_.radar_panel.bottom)) {
    return;
  }
  SDL_Renderer *renderer = platform.renderer();
  const auto playfield = platform.logical_playfield_size();
  const HudPanelRect radar = HudPanel_AnchorTopRight(
      layout_.radar_panel, static_cast<std::int16_t>(playfield.x));

  // Target-status poll (NovaUi_RefreshGameplayPanels 0x0045d320): toggles the
  // blink phase every >= 15 ticks of the original 60 Hz clock (250 ms), which
  // also re-marks the radar dirty (DAT_00596d25) and is the only normal-flight
  // trigger to redraw the panel. The interference pattern is re-rolled inside
  // that draw, so it advances once per poll -- not once per rendered frame.
  const std::uint32_t now =
      static_cast<std::uint32_t>(platform.gameplay_ticks_ms());
  if (now - radar_poll_ms_ >= kRadarBlinkHalfPeriodMs) {
    radar_poll_ms_ = now;
    radar_blink_phase_ =
        static_cast<std::int16_t>((radar_blink_phase_ + 1) & 1);
    // The original decides the static/contacts branch and runs
    // NovaRandom_Range(10) inside the draw, i.e. once per >= 15-tick radar
    // refresh -- g_proximity_scan_detected itself is re-rolled every
    // simulation tick. The port redraws every frame, so it samples the scan
    // latch here and holds both the active flag and the chosen pattern between
    // polls; reading the per-frame roll would flicker the static at frame
    // rate.
    radar_static_active_ = state.proximity_scan_detected;
    if (radar_static_active_) {
      std::uniform_int_distribution<int> pick(
          0, static_cast<int>(radar_static_.size()) - 1);
      radar_static_index_ = static_cast<std::size_t>(pick(radar_rng_));
    }
  }

  const bool iff = Outfit_PlayerHasIffOutfit(state);
  const bool density = Outfit_PlayerHasDensityScanner(state);
  // Ghidra g_is_system_transition_active (0x007354a9) is set ONLY around the
  // docked/landing visit (Stellar_RunDockAndLaunchSequence 0x00455e10 sets it
  // at 0x00455e19, clears it on the launch tail 0x0045612d; also cleared at
  // Ship_RunSpaceflightMode 0x00489241 and Ship_ResetPlayerShipState
  // 0x004b32bc). It is NOT the hyperspace jump: no jump/arrival path writes it,
  // so the radar keeps rendering contacts and static through the whole
  // brake/hold/tunnel (the blink/static still advance on the 250 ms poll). The
  // port's dock is a blocking modal, so the live flag cannot be read here;
  // NovaLanded_RunWindow passes force_empty=true for the docked HUD render.
  // It must NOT track `travel.engaging`: that wrongly blanked the radar for
  // the entire jump.

  // Backdrop: IFF radar fills black (DAT_00733b74); otherwise the cockpit
  // PICT drawn above is the backing the original re-blits.
  if (iff) {
    const SDL_FRect backdrop{static_cast<float>(radar.left),
                             static_cast<float>(radar.top),
                             static_cast<float>(radar.right - radar.left),
                             static_cast<float>(radar.bottom - radar.top)};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &backdrop);
  }

  const SDL_Color bright = ColorOf(layout_.color_word[2]); // BrightRadar
  const SDL_Color dim = ColorOf(layout_.color_word[3]);    // DimRadar
  const float center_x = static_cast<float>(radar.left + radar.right) * 0.5F;
  const float center_y = static_cast<float>(radar.top + radar.bottom) * 0.5F;

  const auto blip_pos = [&](float world_x, float world_y) {
    const int dx = CeilRadar(world_x - state.player.pos_x);
    const int dy = CeilRadar(world_y - state.player.pos_y);
    return std::pair<int, int>{
        CeilRadar(static_cast<float>(static_cast<double>(center_x) +
                                     static_cast<double>(dx) * kRadarScale)),
        CeilRadar(static_cast<float>(static_cast<double>(center_y) +
                                     static_cast<double>(dy) * kRadarScale))};
  };

  if (!force_empty && radar_static_active_) {
    // Interference static: tile one of ten pre-rendered noise patterns over
    // the panel (DrawContext_TileImageInRect 0x004bbdc0). The original tiles a
    // random NovaRandom-picked 'ppat' resource 128..137 (DAT_00733b7c); the
    // port decodes the same resources once and re-rolls the index on the
    // 250 ms radar poll (see above), then holds it between polls. This is the
    // timing-critical half of the direct-composite divergence: unlike the
    // contacts, the static is random, so it must be latched to the poll or it
    // would flicker at presentation rate.
    if (!radar_static_loaded_) {
      radar_static_loaded_ = true;
      // ppat 128 exists in both Nova.rez (4-bit grayscale) and Nova Graphics 1
      // (8-bit colour); the port's first-match lookup takes Nova.rez, while the
      // original's newest-first archive search takes Nova Graphics 1.
      NovaLog::Todo("radar static: ppat lookup uses first-match archive order, "
                    "not the original newest-first order (ppat 128 only)");
      for (std::size_t i = 0; i < radar_static_.size(); ++i) {
        const auto data = NovaResource_Load(
            kResourceTypePpat,
            static_cast<std::uint16_t>(kRadarStaticFirstPpatId + i));
        if (!data) {
          NovaLog::Warn("radar static: ppat {} is missing",
                        kRadarStaticFirstPpatId + i);
          continue;
        }
        const auto image = Resource_LoadPixPatAsImage(*data);
        if (!image) {
          continue;
        }
        radar_static_[i] = SdlTexture::Create(
            renderer, image->width, image->height, image->rgba_pixels);
      }
    }
    if (const SdlTexture *tile = radar_static_[radar_static_index_].get()) {
      const SDL_FRect dst{static_cast<float>(radar.left),
                          static_cast<float>(radar.top),
                          static_cast<float>(radar.right - radar.left),
                          static_cast<float>(radar.bottom - radar.top)};
      SDL_RenderTextureTiled(renderer, tile->get(), nullptr, 1.0F, &dst);
    }
    // The original's center-dot still executes here but at an uninitialized
    // cursor position (NovaRandom's tile index reuse); skipped as invisible.
    return;
  }

  if (!force_empty) {
    const auto *sys = state.scenario.System(
        static_cast<std::int16_t>(state.player.current_system_id + 0x80));
    if (sys != nullptr) {
      // Stellar bodies (NavDef1-16, the decomp's adjacency_system_ids +0x10
      // alias of SystemDef stellar_ids at +0x2a).
      for (const std::int16_t nav : sys->nav_defs) {
        if (nav < 0x80) {
          continue;
        }
        const Stellar *st = state.scenario.Stellar(nav);
        if (st == nullptr) {
          continue;
        }
        const auto [bx, by] = blip_pos(static_cast<float>(st->pos_x),
                                       static_cast<float>(st->pos_y));
        const SDL_Color color =
            iff ? Stellar_RadarDisplayColor(state, *st) : dim;
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        const bool planet = (st->flags & 0x10U) == 0U &&
                            (st->availability_flags & 0x3000U) == 0U;
        if (planet) {
          // Disc radius tier from the spin sprite's full frame height
          // (Sprite_GetFrameFullHeight 0x00462390, default 0x20).
          std::int16_t frame_height = kRadarDefaultFrameHeight;
          if (sprite_store_ != nullptr) {
            const SpriteAsset *set = sprite_store_->Spin(
                renderer, static_cast<std::uint16_t>(st->link_a_id + 1000));
            if (set != nullptr && !set->frames.empty()) {
              frame_height = static_cast<std::int16_t>(set->tile_height);
            }
          }
          const int inset =
              frame_height < 200 ? (frame_height < 90 ? -1 : -2) : -3;
          HudPanelRect rect{static_cast<std::int16_t>(bx),
                            static_cast<std::int16_t>(by),
                            static_cast<std::int16_t>(bx),
                            static_cast<std::int16_t>(by)};
          rect.left += inset;
          rect.top += inset;
          rect.right -= inset;
          rect.bottom -= inset;
          if (IntersectRadarRect(rect, radar)) {
            DrawRadarDisc(renderer, rect);
          }
        } else {
          // Stations / hypergates / wormholes: 2x2 QuickDraw frame.
          HudPanelRect rect{static_cast<std::int16_t>(bx - 1),
                            static_cast<std::int16_t>(by - 1),
                            static_cast<std::int16_t>(bx + 1),
                            static_cast<std::int16_t>(by + 1)};
          if (IntersectRadarRect(rect, radar)) {
            DrawRadarBox(renderer, rect);
          }
        }
      }
      // Ships (slots 1..0x3f; 0 is the player).
      const bool scanner_radar = Player_HasCloakScannerRadarReveal(state);
      for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
        const Ship &ship = state.ShipAt(slot);
        if (!ship.is_active ||
            ship.current_system_id != state.player.current_system_id) {
          continue;
        }
        bool visible = false;
        if (!NovaTargeting_ShipAtCloakVisibilityThreshold(ship)) {
          visible = true;
        } else if (Outfit_HasCloakRadarVisibility(state, ship) ||
                   scanner_radar) {
          visible = true;
        }
        if (!visible) {
          continue;
        }
        const auto [bx, by] = blip_pos(ship.pos_x, ship.pos_y);
        // Without IFF, ordinary contacts are DimRadar and the selected ship
        // flashes BrightRadar. With IFF, it flashes grey over its relation
        // colour instead (NovaUi_DrawStellarRadarPanel 0x0045d600).
        const bool selected = state.player.primary_target_ship_slot ==
                              static_cast<std::int16_t>(slot);
        SDL_Color color = iff ? Ship_RadarDisplayColor(state, ship) : dim;
        if (selected && radar_blink_phase_ != 0) {
          color = iff ? kRadarTargetBlinkColor : bright;
        }
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        const ShipClass *cls = state.scenario.Ship(
            static_cast<std::int16_t>(ship.ship_class_id + 0x80));
        const bool big_blip =
            density && cls != nullptr && cls->mass_tons >= 100;
        if (!big_blip) {
          // Single-pixel dot, inclusive radar-bounds test.
          if (bx >= radar.left && bx <= radar.right && by >= radar.top &&
              by <= radar.bottom) {
            SDL_RenderPoint(
                renderer, static_cast<float>(bx), static_cast<float>(by));
          }
        } else {
          HudPanelRect rect{static_cast<std::int16_t>(bx - 1),
                            static_cast<std::int16_t>(by - 1),
                            static_cast<std::int16_t>(bx + 1),
                            static_cast<std::int16_t>(by + 1)};
          if (IntersectRadarRect(rect, radar)) {
            DrawRadarBox(renderer, rect);
          }
        }
      }
    }
  }

  // Player blip (centre dot).
  {
    const SDL_Color color =
        iff ? Ship_RadarDisplayColor(state, state.player) : bright;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderPoint(renderer, center_x, center_y);
  }

  // Far-from-origin direction arrow: while a primary target keeps the blink
  // phase cycling, not in hyperspace transfer, the current system has navs,
  // and the ship is > sqrt(7000000) px from the system origin, flash an arrow
  // pointing back toward the origin.
  if (radar_blink_phase_ != 0 && state.player.travel_transfer_mode != 3) {
    const auto *sys = state.scenario.System(
        static_cast<std::int16_t>(state.player.current_system_id + 0x80));
    const int nav_count =
        sys ? static_cast<int>(
                  std::count_if(sys->nav_defs.begin(),
                                sys->nav_defs.end(),
                                [](std::int16_t nav) { return nav >= 0x80; }))
            : 0;
    const float dist_sq = state.player.pos_x * state.player.pos_x +
                          state.player.pos_y * state.player.pos_y;
    if (nav_count > 0 && static_cast<double>(dist_sq) > kRadarHomeDistanceSq) {
      const float bearing =
          RadarBearingDeg(state.player.pos_x, state.player.pos_y, 0.0F, 0.0F);
      float start_x = center_x;
      float start_y = center_y;
      RadarPolarOffset(bearing, kRadarArrowShaftNear, start_x, start_y);
      float end_x = center_x;
      float end_y = center_y;
      RadarPolarOffset(bearing, kRadarArrowShaftFar, end_x, end_y);
      SDL_SetRenderDrawColor(renderer, dim.r, dim.g, dim.b, dim.a);
      SDL_RenderLine(renderer,
                     CeilRadar(start_x),
                     CeilRadar(start_y),
                     CeilRadar(end_x),
                     CeilRadar(end_y));
      for (const float wing : {-kRadarArrowWingDeg, kRadarArrowWingDeg}) {
        float wing_x = end_x;
        float wing_y = end_y;
        RadarPolarOffset(bearing + wing, kRadarArrowWingLength, wing_x, wing_y);
        SDL_RenderLine(renderer,
                       CeilRadar(end_x),
                       CeilRadar(end_y),
                       CeilRadar(wing_x),
                       CeilRadar(wing_y));
      }
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    }
  }
}

} // namespace game
