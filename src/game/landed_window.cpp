#include "landed_window.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "docked_dialog.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "ship_spawn.hpp"
#include "targeting.hpp"
#include "weapon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace game {

// ---------------------------------------------------------------------------
// Stellar_TravelToSystem (0x00455e10): normal arrival subset.
// ---------------------------------------------------------------------------
bool NovaLanding_EnterDocked(GameState &state, LandedContext &ctx) {
  ctx.landed = false;
  ctx.denial = LandedDenial::kNone;
  const std::int16_t stellar_id = state.travel.selected_stellar_id;
  const auto *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr || !stellar->is_available ||
      stellar->system_id != state.player.current_system_id ||
      (stellar->availability_flags & 0x3000U) != 0U ||
      (stellar->flags & 0x20U) != 0U ||
      !NovaTargeting_StellarTargetsSpriteSetActive(*stellar)) {
    ctx.denial = LandedDenial::kUnavailable;
    return false;
  }
  // The final normal-arrival branch in Stellar_ProcessTravelAndLanding only
  // accepts the dock once both axis deltas are strictly below 0xfa.
  constexpr float kArrivalAxisRange = 250.0F;
  if (std::abs(state.player.pos_x - static_cast<float>(stellar->pos_x)) >=
          kArrivalAxisRange ||
      std::abs(state.player.pos_y - static_cast<float>(stellar->pos_y)) >=
          kArrivalAxisRange) {
    ctx.denial = LandedDenial::kTooFar;
    return false;
  }

  // Stellar_ProcessTravelAndLanding checks affordability before it begins the
  // arrival transition, then deducts the full fee (unless the stellar is in
  // its hostile/hazard state). Do the same before touching player state.
  const bool fee_waived = stellar->hazard_marker;
  if (stellar->service_cost > 0 && !fee_waived &&
      state.player.credits < stellar->service_cost) {
    NovaLog::Info("landing denied at stellar {}: service cost {} exceeds "
                  "available credits {}",
                  stellar_id,
                  stellar->service_cost,
                  state.player.credits);
    ctx.denial = LandedDenial::kTooExpensive;
    return false;
  }

  if (stellar->service_cost > 0 && !fee_waived) {
    state.player.credits -= stellar->service_cost;
  }
  // Stellar_ProcessTravelAndLanding clears the beam queue immediately on
  // accepted landing; Stellar_TravelToSystem retires projectile ShotStates
  // before returning to flight. The modal pauses simulation, so clear the
  // corresponding clean-room transient pools at this boundary.
  NovaWeapon_ClearTransientCombatState(state);
  // Stellar_TravelToSystem (0x00455e10) runs Weapon_ReconcileOutfitPoolWith-
  // WeaponBanks at the start of the travel transition, so any stock weapon
  // bank acquired since the last reconcile (e.g. a ship bought at the
  // shipyard with mounted stock guns) becomes a sellable owned outfit. Landed
  // Offfitter session buys/sells use this same reconcile at modal entry.
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
  state.player.pos_x = static_cast<float>(stellar->pos_x);
  state.player.pos_y = static_cast<float>(stellar->pos_y);
  state.player.vel_x = 0.0F;
  state.player.vel_y = 0.0F;
  state.player.speed = 0.0F;

  // Stellar_TravelToSystem restores the effective hull and shield capacities
  // after the destination interaction loop returns.
  const PlayerEffectiveStats effective =
      Outfit_ComputePlayerEffectiveStats(state);
  state.player.shield_points = effective.max_shield_points;
  state.player.armor_points = effective.max_armor_points;
  state.cached_stats = effective;
  state.stat_cache_valid = true;

  // Stellar_ProcessTravelAndLanding (0x00457580) runs Ship_DeactivateVacant
  // ShipsAndTally('\0') during the normal arrival, then Mission_SpawnSystemMisn
  // Ships + System_TickNpcSpawnMaintenance reseed the system's NPC population.
  // So a landing (and the subsequent launch) leaves the system with a fresh
  // batch of ships rather than the fleet that had accumulated before docking.
  // The port deactivates the whole vacant cohort (idle wanderers / parked /
  // mission ships; only non-fire-restricted ships actively engaging the player
  // are spared -- see ship_spawn.hpp), then replenishes toward avg_ships.
  NovaShip_DeactivateVacantShipsAndTally(state, /*keep_player_engaged=*/false);
  NovaSystem_TickNpcSpawnMaintenance(state, state.player.current_system_id);

  ctx.stellar_id = stellar_id;
  ctx.landed = true;
  ctx.selection = LandedService::kLaunch;
  state.travel.landed_this_frame = true;
  NovaLog::Info("landed at stellar {} ({}); {} credits remain",
                stellar_id,
                stellar->name,
                state.player.credits);
  return true;
}

// ---------------------------------------------------------------------------
// Fuel service.
// ---------------------------------------------------------------------------
// Refuels the player ship toward its effective fuel capacity. `price_per_unit`
// is the price in credits for one fuel point (the raw fuel_points scale where
// a typical ship holds a few hundred units); the player pays per unit topped
// up, clamped so credits never go negative (they may only buy as much as they
// can afford). Returns the credits actually spent.
std::int32_t NovaLanded_Refuel(GameState &state, std::int32_t price_per_unit) {
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  const float missing =
      std::max(0.0F, eff.fuel_capacity - state.player.fuel_points);
  if (missing <= 0.0F) {
    return 0;
  }
  // The original's stellar marker makes refuelling free, not unavailable.
  if (price_per_unit <= 0) {
    state.player.fuel_points = eff.fuel_capacity;
    NovaLog::Info("refuel: granted {:.1f} fuel by free stellar service",
                  missing);
    return 0;
  }
  // Full cost for the whole top-up; the player may only buy as far as credits
  // reach, so cap the spend at the wallet.
  const std::int64_t full_cost =
      std::int64_t{price_per_unit} * static_cast<std::int64_t>(missing);
  const std::int64_t spend =
      std::min<std::int64_t>(state.player.credits, full_cost);
  state.player.credits -= static_cast<std::int32_t>(spend);
  const float gained =
      static_cast<float>(spend) / static_cast<float>(price_per_unit);
  state.player.fuel_points =
      std::min(eff.fuel_capacity, state.player.fuel_points + gained);
  NovaLog::Info("refuel: bought {:.1f} fuel for {} credits ({} remaining)",
                gained,
                spend,
                state.player.credits);
  return static_cast<std::int32_t>(spend);
}

// ---------------------------------------------------------------------------
// Armor/shield repair service.
// ---------------------------------------------------------------------------
std::int32_t NovaLanded_Repair(GameState &state,
                               std::int32_t price_per_armor_point) {
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  // The original repairs the whole hull; shields are already full from the
  // arrival transition, so only the armor gap is billed.
  const float armor_gap =
      std::max(0.0F, eff.max_armor_points - state.player.armor_points);
  if (armor_gap <= 0.0F || price_per_armor_point <= 0) {
    return 0;
  }
  const std::int64_t full_cost = std::int64_t{price_per_armor_point} *
                                 static_cast<std::int64_t>(armor_gap);
  const std::int32_t spend = static_cast<std::int32_t>(
      std::min<std::int64_t>(state.player.credits, full_cost));
  state.player.credits -= spend;
  const float repaired =
      static_cast<float>(spend) / static_cast<float>(price_per_armor_point);
  state.player.armor_points =
      std::min(eff.max_armor_points, state.player.armor_points + repaired);
  NovaLog::Info("repair: fixed {:.1f} armor for {} credits ({} remaining)",
                repaired,
                spend,
                state.player.credits);
  return spend;
}

// ---------------------------------------------------------------------------
// Window / service-button geometry.
//
// The original docked screen is the Spaceport window (DLOG 0x3e8, DITL 0x3e8),
// a near-full-screen 640x480 panel whose backdrop is destination-art PICT
// 0x2134 (Ghidra FUN_0048e970 sets g_travel_overlay_sprite_handle =
// Resource_LoadPictAsImage(0x2134), drawn across the whole window rect). The
// earlier 263x185 PICT 0x2137 is a *sub*-window decoration (the travel-services
// modal DLOG 0x3f5), not the docked backdrop, so presenting it as the full
// screenspace backdrop was wrong. Service controls live in two ~145px-wide
// columns down the left and right edges (DITL 0x3e8 entries 3,6,7,8,9,10,11,
// 12), mirroring the real docked buttons.
//
// We reproduce that docked panel here: the backdrop fills the full 640x480
// screen and the window content (panels, title band, and the two-column x
// 4-row service grid at the lower-left and lower-right) is laid out from the
// real Spaceport DLOG/DITL 0x3e8 via NovaDialogWindow_Layout, so the draw pass
// and the mouse hit-test share the same data-driven rects. The hardcoded
// two-column grid remains only as a fallback when the dialog archive is
// absent.

namespace {

// The original Spaceport DLOG is 618x517. Keep that native size and centre it
// in the unrestricted window coordinate space; a 640x480 clipped viewport
// cannot contain the dialog vertically.
using PanelRect = SDL_FRect;

[[nodiscard]] PanelRect DockedPanel(const SdlPlatform &platform) {
  const SDL_FPoint output = platform.logical_playfield_size();
  constexpr float kDockedWidth = 618.0F;
  constexpr float kDockedHeight = 517.0F;
  return {(output.x - kDockedWidth) / 2.0F,
          (output.y - kDockedHeight) / 2.0F,
          kDockedWidth,
          kDockedHeight};
}

// Fallback button columns (DITL-left-column values) used when the dialog
// archive is absent, so the dock still renders.
struct ServiceColumns {
  float left_x = 3.0F;
  float right_x = 471.0F;
  float width = 145.0F;
  float height = 25.0F;
  float row_pitch = 41.0F;
  float row0_y = 333.0F;
};

} // namespace

// Public adapter (#3): lays the Spaceport dialog (DLOG+ DITL 0x3e8) onto the
// 640x480 logical panel, centering the dialog window on it and mapping every
// DITL item to panel space (screen = window_origin + dialog_rect). This
// replaces the previously hardcoded panel/title/status/button geometry with
// geometry read straight from Nova.rez. Type definitions live in the header.
// When the dialog resources are unavailable it falls back to a minimal
// layout with just the buttons from the hardcoded two-column grid.
bool NovaDialogWindow_Layout(const SDL_FRect &panel, DockedLayout &out) {
  constexpr ServiceColumns kFallback;
  constexpr std::size_t kRows = 4;
  const std::size_t button_count = std::min<std::size_t>(
      static_cast<std::size_t>(LandedService::kCount), kRows * 2U);

  const auto def = NovaResource_LoadDialogDefinition(0x3e8);
  const auto items = NovaResource_LoadDialogItems(0x3e8);
  if (def && items) {
    // Window size = DLOG bounds right-bottom minus left-top (618x517 for the
    // Spaceport window); centered like Dialog_CreateFromDlog centers it, with
    // the centered offset truncated toward zero to match the game's integer
    // (display - windowSize)/2 arithmetic.
    const float win_w = static_cast<float>(def->right - def->left);
    const float win_h = static_cast<float>(def->bottom - def->top);
    const auto center_trunc = [](float span) {
      return std::trunc(span / 2.0F);
    };
    const float win_x = panel.x + center_trunc(panel.w - win_w);
    const float win_y = panel.y + center_trunc(panel.h - win_h);
    out.window = {win_x, win_y, win_w, win_h};
    out.from_ditl = true;

    for (const auto &item : *items) {
      DockedItem d;
      d.rect.x = win_x + static_cast<float>(item.left);
      d.rect.y = win_y + static_cast<float>(item.top);
      d.rect.w = static_cast<float>(item.right - item.left);
      d.rect.h = static_cast<float>(item.bottom - item.top);
      const int w = item.right - item.left;
      const int h = item.bottom - item.top;
      // These controls are addressed explicitly by the original renderer
      // (UiPanel_GetEntryInfo items 3, 5, and 6); use the DITL ordinals when
      // available instead of inferring their role from size. Item 3 holds
      // the stellar name, item 5 the planet image, and item 6 the landing
      // description.
      if (item.index == 2) {
        d.kind = DockedItemKind::kTitleBand;
      } else if (item.index == 4) {
        d.kind = DockedItemKind::kOuterPanel;
      } else if (item.index == 5) {
        d.kind = DockedItemKind::kInnerPanel;
      } else if (w == 145 && h == 25) {
        d.kind = DockedItemKind::kButton;
      } else if (w > 500 && h > 200) {
        d.kind = DockedItemKind::kOuterPanel;
      } else if (w > 250 && h > 150) {
        d.kind = DockedItemKind::kInnerPanel;
      } else if (w > 250 && h < 30) {
        d.kind = DockedItemKind::kTitleBand;
      } else {
        d.kind = DockedItemKind::kOrnament;
      }
      d.ditl_index = item.index;
      out.items.push_back(d);
    }
    return true;
  }
  NovaLog::Todo("Spaceport DLOG/DITL 0x3e8 not usable; using hardcoded dock "
                "grid layout");
  // Minimal fallback: the eight buttons in the reference two-column grid.
  out.from_ditl = false;
  out.window = {panel.x, panel.y, 618.0F, 517.0F};
  // The fallback has no raw DITL parser output, but retain the original
  // control ordinals so service dispatch still follows the same map.
  constexpr std::array<std::size_t, 7> kFallbackDitlItems{
      11, 3, 6, 7, 8, 9, 10};
  for (std::size_t i = 0; i < button_count; ++i) {
    const std::size_t side = i >= kRows ? 1 : 0;
    const std::size_t row = i % kRows;
    DockedItem d;
    d.kind = DockedItemKind::kButton;
    d.rect.x = panel.x + (side == 0 ? kFallback.left_x : kFallback.right_x);
    d.rect.y = panel.y + kFallback.row0_y +
               static_cast<float>(row) * kFallback.row_pitch;
    d.rect.w = kFallback.width;
    d.rect.h = kFallback.height;
    d.ditl_index = i < kFallbackDitlItems.size() ? kFallbackDitlItems[i] : 0;
    out.items.push_back(d);
  }
  return false;
}

std::optional<LandedService>
NovaDialog_DockedServiceForDitlItem(std::size_t ditl_index) {
  switch (ditl_index) {
  // UiPanel_GetEntryInfo uses one-based item numbers; NovaDialogItem::index
  // is deliberately zero-based. Ghidra's {12,4,7,8,9,10,11} therefore maps
  // to the resource ordinals below.
  case 11:
    return LandedService::kLaunch;
  case 3:
    return LandedService::kRefuel;
  case 6:
    return LandedService::kBuySellCargo;
  case 7:
    return LandedService::kOutfit;
  case 8:
    return LandedService::kShipyard;
  case 9:
    return LandedService::kMissionBoard;
  case 10:
    return LandedService::kBar;
  default:
    return std::nullopt;
  }
}

// Greedy word-wrap for the docked landing-description panel (see header).
// Grows a candidate line word-by-word and, when the measured width would
// exceed `max_width`, closes the current line and starts the next with only
// the new word (never re-append the already-drained words, which would show a
// "cumulative" repeat of the text on every line).
std::vector<std::string>
WrapDescriptionLines(std::string_view text,
                     int max_width,
                     const std::function<int(std::string_view)> &measure) {
  std::vector<std::string> lines;
  if (text.empty() || max_width <= 0) {
    return lines;
  }
  std::string line;
  std::size_t i = 0;
  while (i < text.size()) {
    // Skip inter-word whitespace; a newline forces an explicit line break.
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\n' || text[i] == '\t')) {
      if (text[i] == '\n' && !line.empty()) {
        lines.push_back(line);
        line.clear();
      }
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    std::size_t word_end = i;
    while (word_end < text.size() && text[word_end] != ' ' &&
           text[word_end] != '\n' && text[word_end] != '\t') {
      ++word_end;
    }
    const std::size_t word_len = word_end - i;
    if (word_len == 0) {
      break; // trailing whitespace only
    }
    std::string candidate = line;
    if (!candidate.empty()) {
      candidate.push_back(' ');
    }
    candidate.append(text.substr(i, word_len));
    if (measure(candidate) > max_width && !line.empty()) {
      lines.push_back(line);
      line.assign(text.substr(i, word_len)); // new line starts with this word
    } else {
      line = candidate;
    }
    i = word_end;
  }
  if (!line.empty()) {
    lines.push_back(line);
  }
  return lines;
}

namespace {
// Builds the service buttons from a laid-out dock: the DITL's eight 145x25
// button rects split into left/right columns, each sorted top-to-bottom. Each
// physical button is assigned the real service it represents (see the two
// k*ColumnServices tables), so the label and dispatch match the original
// Spaceport screen. `slot` carries the LandedService value (not a packed
// column*4+row index), so the draw/hit-test/navigation resolve the same
// service the number keys select.
std::vector<ServiceButton> BuildServiceButtons(const DockedLayout &layout) {
  std::vector<ServiceButton> buttons;
  buttons.reserve(7);
  for (const auto &item : layout.items) {
    if (item.kind != DockedItemKind::kButton) {
      continue;
    }
    if (const auto service =
            NovaDialog_DockedServiceForDitlItem(item.ditl_index)) {
      buttons.push_back(
          ServiceButton{item.rect, static_cast<std::uint8_t>(*service)});
    }
  }
  return buttons;
}

} // namespace

namespace {

// Human-readable service-row labels for the MVP menu. The original draws these
// as PICT sub-window frames (Nova Graphics 3: 0x2135 Shipyard, 0x2136 Outfit,
// 0x2137 Bar, 0x2139 Mission BBS, 0x213e Trade, ...); we fall back to text so
// the MVP is
// navigable.
const char *ServiceLabel(LandedService t) {
  switch (t) {
  case LandedService::kLaunch:
    return "Leave";
  case LandedService::kRefuel:
    return "Refuel";
  case LandedService::kBuySellCargo:
    return "Trade center";
  case LandedService::kOutfit:
    return "Outfitter";
  case LandedService::kShipyard:
    return "Shipyard";
  case LandedService::kBar:
    return "Bar";
  case LandedService::kMissionBoard:
    return "Mission BBS";
  case LandedService::kCount:
    break;
  }
  return "";
}

// Whether a docked service slot is usable right now, mirroring the original's
// per-slot gating in NovaUi_RedrawTravelActionButtons (0x004a0220) and its
// mouse hit-test (NovaUi_HitTestAndTrackTravelActionButtons 0x0049fe10).
//   Leave            always
//   Refuel/Recharge  credits>0, fuel below capacity, non-hypergate;
//                    (hyper-gate travel_flags bit 0x20 disables services)
//   Trade center, Shipyard, Bar    travel_flags bits 0x2/0x8/0x40
//   Outfitter                       travel_flags bit 0x4
//   Mission BBS                     non-hypergate (travel_flags bit 0x20 clear)
// Unavailable slots render grey (disabled art) and refuse activation.
bool ServiceAvailable(const GameState &state,
                      std::int16_t stellar_id,
                      LandedService t) {
  const auto *st = state.scenario.Stellar(stellar_id);
  const std::uint32_t flags = st ? st->flags : 0U;
  const bool allows_services = (flags & 0x20U) == 0U; // non-hypergate
  const auto &eff = state.cached_stats;
  switch (t) {
  case LandedService::kLaunch:
    return true;
  case LandedService::kRefuel: {
    if (!allows_services || state.player.credits <= 0) {
      return false;
    }
    return state.player.fuel_points + 0.5F < eff.fuel_capacity;
  }
  case LandedService::kBuySellCargo:
    return (flags & 0x2U) != 0U;
  case LandedService::kOutfit:
    return (flags & 0x4U) != 0U;
  case LandedService::kShipyard:
    return (flags & 0x8U) != 0U;
  case LandedService::kBar:
    return (flags & 0x40U) != 0U;
  case LandedService::kMissionBoard:
    return allows_services;
  case LandedService::kCount:
    break;
  }
  return false;
}

// Draws the docked-screen backdrop + panels + header + service list with the
// real screen fonts: the destination title in Chicago (charcoal) and the status
// lines in Geneva. The Spaceport backdrop (`destination_art`) is drawn at
// native size in the centred DLOG window; the destination planet picture (`planet_art`, PICT
// link_a + 0x10000) is drawn 1:1 into the Spaceport DITL's 612x285 outer panel
// at the top-centre (its natural size), over the spaceport. The title band,
// inner status panel, and buttons are positioned from the laid-out DITL items
// (`layout`) rather than hardcoded geometry.
void DrawLandedMenu(SdlPlatform &platform,
                    NovaFontCache &font_cache,
                    const ServicesButtonArt &buttons,
                    const GameState &state,
                    const LandedContext &ctx,
                    SDL_Texture *destination_art,
                    SDL_Texture *planet_art,
                    std::string_view description,
                    const SDL_FRect &panel,
                    const DockedLayout &layout,
                    const std::vector<ServiceButton> &button_rects,
                    std::optional<std::uint8_t> hovered) {
  SDL_Renderer *renderer = platform.renderer();
  const auto *st = state.scenario.Stellar(ctx.stellar_id);

  // The Spaceport backdrop is the DLOG 0x3e8 window artwork. Draw it at its
  // native 618x517 size in the same centred coordinate space as the DITL.
  SDL_SetRenderDrawColor(renderer, 1, 4, 12, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  platform.SetFullscreenPlayfield();
  if (destination_art != nullptr) {
    const SDL_FRect backdrop_rect = layout.from_ditl ? layout.window : panel;
    SDL_RenderTexture(renderer, destination_art, nullptr, &backdrop_rect);
  }

  const SDL_Color kTitle{255, 255, 255, 255};
  const SDL_Color kBody{255, 255, 255, 255};
  const SDL_Color kPanel{16, 40, 72, 255};       // flat panel frame fill

  // Lay out the panels. The large 612x285 outer panel at the top-centre
  // is the destination-planet frame: the planet picture is drawn into it at its
  // natural size (falling back to a flat frame when no planet PICT loads). The
  // inner (status/description) panel and the title band are handled below.
  SDL_FRect title_band = panel;
  SDL_FRect outer_panel = {0.0F, 0.0F, 0.0F, 0.0F};
  SDL_FRect status_panel = {0.0F, 0.0F, 0.0F, 0.0F};
  for (const auto &item : layout.items) {
    if (item.kind == DockedItemKind::kInnerPanel) {
      status_panel = item.rect;
    } else if (item.kind == DockedItemKind::kOuterPanel) {
      outer_panel = item.rect;
    } else if (item.kind == DockedItemKind::kTitleBand) {
      title_band = item.rect;
    }
  }

  // Destination planet picture in the outer panel (1:1; it is the same 612x285
  // size as the panel). The PICT backdrop supplies the frame artwork; do not
  // add an SDL border over it.
  if (outer_panel.w > 0.0F && outer_panel.h > 0.0F) {
    if (planet_art != nullptr) {
      SDL_RenderTexture(renderer, planet_art, nullptr, &outer_panel);
    } else {
      SDL_SetRenderDrawColor(renderer, kPanel.r, kPanel.g, kPanel.b, kPanel.a);
      SDL_RenderFillRect(renderer, &outer_panel);
    }
  }

  // The inner panel is already part of the Spaceport backdrop. Only provide a
  // flat fallback when the backdrop resource is unavailable; an extra border
  // here was the visible blue rectangle around the description.
  if (status_panel.w > 0.0F && status_panel.h > 0.0F) {
    if (destination_art == nullptr) {
      SDL_SetRenderDrawColor(renderer, kPanel.r, kPanel.g, kPanel.b, kPanel.a);
      SDL_RenderFillRect(renderer, &status_panel);
    }
  }

  // Destination name centred in the DITL header band (Chicago/title font),
  // falling back to a top-of-panel line if the band is unavailable.
  std::string title = st ? st->name : std::string("(unknown stellar)");
  // Item 6 is drawn by the original with its text cursor 18 logical pixels
  // below the rect's top edge; this is a baseline, not the rect midpoint.
  const float title_baseline = title_band.y + 18.0F;
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kChicago,
                        18.0F,
                        kNovaFontStyleRegular,
                        kTitle,
                        title_band.x,
                        title_band.x + title_band.w,
                        title_baseline,
                        title);

  // The stellar's landing description text in the inner content panel (small
  // Geneva dialog font), word-wrapped to the panel width. The description comes from the
  // stellar's "desc" landing-description block (NovaResource_LoadStellar-
  // Description, Ghidra Ui_LoadSelectionDialogResource); when it is absent the
  // panel falls back to the credits/fuel/hull status lines.
  const bool have_status = status_panel.w > 0.0F && status_panel.h > 0.0F;
  constexpr float kDescriptionInset = 4.0F;
  const float body_x = have_status ? status_panel.x + kDescriptionInset
                                  : panel.x + kDescriptionInset;
  float baseline = have_status ? status_panel.y + 13.0F : panel.y + 60.0F;
  const float body_w = have_status
                           ? std::max(40.0F, status_panel.w - 2.0F * kDescriptionInset)
                           : std::max(40.0F, panel.w - 2.0F * kDescriptionInset);

  if (!description.empty()) {
    // Word-wrap the stellar description to the panel width (measured with the
    // same Geneva 9pt face used to draw) and lay the lines out from the inner
    // panel top, mirroring how the original fills the docked landing panel.
    constexpr float kDescriptionFontSize = 9.0F;
    constexpr float kLineHeight = 11.0F;
    const int wrap_w = static_cast<int>(std::lround(body_w));
    const auto desc_lines =
        WrapDescriptionLines(description, wrap_w, [&](std::string_view s) {
          return font_cache.TextWidth(NovaFontFamily::kGeneva,
                                      kDescriptionFontSize,
                                      kNovaFontStyleRegular,
                                      s);
        });
    for (const auto &desc_line : desc_lines) {
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    kDescriptionFontSize,
                    kNovaFontStyleRegular,
                    kBody,
                    body_x,
                    baseline,
                    desc_line);
      baseline += kLineHeight;
    }
  } else {
    // Fallback when no landing description resolved: the credits/fuel/hull
    // status lines the panel used to show (kept so a missing desc block never
    // leaves the inner panel blank).
    baseline += 20.0F;
    const std::string credits =
        "Credits: " + std::to_string(state.player.credits);
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kBody,
                  body_x,
                  baseline,
                  credits);

    const auto *eff = &state.cached_stats; // set during landing/refuel/repair
    const float cap = eff ? eff->fuel_capacity : 0.0F;
    char fuel[96];
    std::snprintf(fuel,
                  sizeof(fuel),
                  "Fuel: %.0f / %.0f      Hull: %.0f / %.0f",
                  state.player.fuel_points,
                  cap,
                  state.player.armor_points,
                  eff ? eff->max_armor_points : 0.0F);
    baseline += 18.0F;
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kBody,
                  body_x,
                  baseline,
                  fuel);
  }

  // The service buttons, drawn with the real three-state button art and their
  // labels centred in the body font. Following the original, there is NO
  // keyboard-focus/selection highlight: the pressed ("click") art marks the
  // slot the mouse currently hovers (NovaUi_HitTestAndTrackTravelActionButtons
  // redraws with the hovered index), and unavailable slots are drawn grey with
  // the disabled art (NovaUi_RedrawTravelActionButtons). Label baseline is
  // centred on the +5px-below-centre rule the original uses
  // (NovaUi_DrawThreeStateButton). Label colours follow the original's
  // three-state label table (NovaUi_InitThreeStateButtonArt DAT_007d8350):
  // white on the normal art, 50% grey on the pressed/hover and grey/disabled
  // art -- the shared renderer draws the label in the plain screen font (it
  // sets only the font id + size, never a bold style), so no bold here either.
  constexpr SDL_Color kButtonLabelNormal{255, 255, 255, 255};
  constexpr SDL_Color kButtonLabelGrey{128, 128, 128, 255};
  for (std::size_t i = 0; i < button_rects.size(); ++i) {
    const auto slot = button_rects[i].slot;
    const auto svc = static_cast<LandedService>(slot);
    const bool enabled = ServiceAvailable(state, ctx.stellar_id, svc);
    const bool hovered_by_mouse =
        enabled && hovered.has_value() && *hovered == slot;
    const auto button_state =
        !enabled
            ? ButtonState::kDisabled
            : (hovered_by_mouse ? ButtonState::kHover : ButtonState::kNormal);
    buttons.Draw(platform, button_rects[i].rect, button_state);
    const SDL_Color &label_color = !enabled           ? kButtonLabelGrey
                                   : hovered_by_mouse ? kButtonLabelGrey
                                                      : kButtonLabelNormal;
    const float label_baseline =
        ThreeStateButtonLabelBaseline(button_rects[i].rect);
    NovaText_DrawCentered(platform,
                          font_cache,
                          kThreeStateButtonFontFamily,
                          kThreeStateButtonFontSize,
                          kNovaFontStyleRegular,
                          label_color,
                          button_rects[i].rect.x,
                          button_rects[i].rect.x + button_rects[i].rect.w,
                          label_baseline,
                          ServiceLabel(svc));
  }

  // Footer hint row in the body font, just below the docked content (the
  // window bottom when laid out; otherwise the top of the panel).
  const float footer_y = layout.from_ditl
                             ? layout.window.y + layout.window.h - 20.0F
                             : panel.y + 52.0F;
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        11.0F,
                        kNovaFontStyleRegular,
                        kBody,
                        panel.x + 300.0F,
                        panel.x + 530.0F,
                        footer_y,
                        "R/F refuel  C/T trade  O outfit  S shipyard  "
                        "N mission  B bar  Esc/Enter leave");
}

// Handles one service selection from the docked menu. Returns the exit intent:
// kLaunched when the player leaves the dock, otherwise kServiceComplete (the
// sub-screen ran and control returns to the menu). The buy/sell/outfit/shipyard
// /bar/starmap/mission sub-screens are all mocked for the MVP and return
// immediately; a reconstructed sub-screen would instead open its own modal and
// return kServiceComplete when it closes.
LandedExit DispatchService(SdlPlatform &platform,
                           GameState &state,
                           LandedContext &ctx,
                           SDL_Texture *docked_snapshot) {
  (void)platform;
  switch (ctx.selection) {
  case LandedService::kLaunch:
    NovaLog::Info("launching from stellar {} back into space",
                  static_cast<int>(ctx.stellar_id));
    return LandedExit::kLaunched;

  case LandedService::kRefuel: {
    const Stellar *stellar = state.scenario.Stellar(ctx.stellar_id);
    NovaLanded_Refuel(state,
                      stellar != nullptr && stellar->hazard_marker ? 0 : 1);
    return LandedExit::kServiceComplete;
  }

  case LandedService::kBuySellCargo:
  case LandedService::kOutfit:
  case LandedService::kShipyard:
  case LandedService::kBar:
  case LandedService::kMissionBoard: {
    // Render the sub-window as a real on-screen dialog over the docked scene
    // (frame PICT + heading + Leave), instead of the previous TODO mock. The
    // service content (buy/sell tables, outfit list, shipyard purchases, bar
    // holovid/gamble, map navigation) is still out of scope behind the frame.
    const LandedExit dialog_exit = NovaLanded_RunSubWindowDialog(
        platform, state, ctx.selection, ctx.stellar_id, docked_snapshot);
    if (dialog_exit == LandedExit::kQuit) {
      return LandedExit::kQuit;
    }
    // Otherwise the dialog closed back to the dock menu normally. The sub-
    // window content is out of scope, so there is no kLaunched hand-off yet;
    // a future service-internal "launch" path would return kLaunched here.
    return LandedExit::kServiceComplete;
  }

  case LandedService::kCount:
    break;
  }
  return LandedExit::kServiceComplete;
}

} // namespace

// ---------------------------------------------------------------------------
// Landed window modal run loop.
// ---------------------------------------------------------------------------
LandedExit NovaLanded_RunWindow(SdlPlatform &platform,
                                GameState &state,
                                LandedContext &ctx) {
  NovaLog::Info("opening landed services window at stellar {}",
                static_cast<int>(ctx.stellar_id));

  // Load the docked-screen backdrop and the destination planet picture.
  //
  // destination_art is the full-width dock backdrop: the Spaceport PICT 0x2134
  // (618x517) drawn across the whole 640x480 panel (via
  // g_travel_overlay_sprite_handle, Ghidra FUN_0048e970's base overlay); the
  // earlier 263x185 PICT 0x2137 is only a fallback when 0x2134 is unavailable.
  //
  // planet_art is the destination stellar's own planet picture (the "you are
  // here" surface panorama, PICT 0x2710 + link_a_id, or its custom picture id
  // when it sets one at engage_highlight_frame >= 0x80 - mirrors FUN_0048e970's
  // planet-PICT selection `engage_highlight_frame >= 0x80 ?
  // engage_highlight_frame : link_a_id + 0x2710`). The Spaceport DITL 0x3e8
  // carves a 612x285 outer panel at the top-centre of the docked window that is
  // exactly the pict's natural size, so the planet is drawn there at 1:1, over
  // the spaceport backdrop. When no stellar picture exists (hypergate / missing
  // strip) the panel falls back to a flat frame.
  std::unique_ptr<SdlTexture> destination_art;
  const auto *st_dec = state.scenario.Stellar(ctx.stellar_id);
  const auto load_pict = [&](std::uint16_t pict_id) {
    const auto data = NovaResource_LoadPictData(pict_id);
    const auto img = data ? Resource_LoadPictAsImage(*data) : std::nullopt;
    if (data && img) {
      return SdlTexture::Create(
          platform.renderer(), img->width, img->height, img->rgba_pixels);
    }
    return std::unique_ptr<SdlTexture>{};
  };
  std::unique_ptr<SdlTexture> planet_art;
  if (st_dec) {
    const std::int16_t stell_pict =
        (st_dec->engage_highlight_frame >= 0x80)
            ? st_dec->engage_highlight_frame
            : static_cast<std::int16_t>(st_dec->link_a_id + 0x2710);
    if (stell_pict >= 0x80) {
      planet_art = load_pict(static_cast<std::uint16_t>(stell_pict));
      if (planet_art) {
        NovaLog::Info("destination planet PICT 0x{} ({}) for stellar {}",
                      std::to_string(static_cast<int>(stell_pict)),
                      st_dec->name,
                      static_cast<int>(ctx.stellar_id));
      } else {
        NovaLog::Todo(
            "no destination planet PICT {} (link_a {} + 0x10000) for stellar "
            "'{}'; the outer panel stays a flat frame",
            static_cast<int>(stell_pict),
            static_cast<int>(st_dec->link_a_id),
            st_dec->name);
      }
    }
  }
  destination_art = load_pict(0x2134);
  if (destination_art) {
    NovaLog::Info("landed dock backdrop PICT 0x2134");
  } else {
    NovaLog::Todo("docked backdrop PICT 0x2134 unavailable; falling back to "
                  "0x2137");
    destination_art = load_pict(0x2137);
  }
  if (!destination_art) {
    NovaLog::Warn("no docked backdrop decoded; drawing a flat backdrop");
  }
  // The docked panel is the native 618x517 Spaceport dialog, centred in the
  // unrestricted window coordinate space. Set the presentation before asking
  // for the window dimensions used by the DLOG/DITL layout.
  platform.SetFullscreenPlayfield();
  const SDL_FRect panel = DockedPanel(platform);

  // Lay out the docked screen from the real Spaceport DLOG/DITL 0x3e8
  // (centered window + per-item screen rects). Falls back to a hardcoded
  // grid when the dialog resources are unavailable.
  DockedLayout layout;
  NovaDialogWindow_Layout(panel, layout);

  // Load this stellar's landing description (the text shown in the docked inner
  // panel). Mirrors NovaUi_RunTravelDestinationInteractionLoop populating its
  // prompt buffer via Ui_LoadSelectionDialogResource with the destination
  // stellar's resource id.
  std::string description;
  const auto desc = NovaResource_LoadStellarDescription(ctx.stellar_id);
  if (desc) {
    description = desc->text;
    NovaLog::Info("landed description for stellar {} ({} chars)",
                  static_cast<int>(ctx.stellar_id),
                  description.size());
  } else {
    NovaLog::Todo("no landing description desc for stellar {}; docked inner "
                  "panel falls back to the status lines",
                  static_cast<int>(ctx.stellar_id));
  }

  // Load the docked buttons' real three-state strip art (normal 0x1d4c..,
  // pressed 0x1d4f.., grey 0x1d52..) and lay out their desk rects. If the
  // strips are unavailable the buttons still render as flat fills behind the
  // labels.
  ServicesButtonArt button_art;
  const bool have_buttons = button_art.Initialize(platform);
  if (!have_buttons) {
    NovaLog::Warn("service button art unavailable");
  }
  const std::vector<ServiceButton> button_rects = BuildServiceButtons(layout);

  NovaLog::Todo(
      "docked screen geometry (panels, title band, buttons) is laid out from "
      "the real DITL 0x3e8, but the PICT sub-window frames (0x2135..) and the "
      "sub-window modals are out of scope");

  // Font subsystem init (SDL3_ttf) is refcounted and managed lazily by the
  // NovaFontCache itself (TTF_Init on first use, TTF_Quit in ~NovaFontCache),
  // so repeated dockings stay stable. Here we only surface the availability of
  // the faces the original ships/substitutes so a missing bundle is loud.
  NovaFontCache font_cache;
  NovaLog::Info("landed window font families available: Charcoal={} "
                "Geneva={} Times={} Helvetica={} NewYork={}",
                font_cache.IsFamilyAvailable(NovaFontFamily::kChicago),
                font_cache.IsFamilyAvailable(NovaFontFamily::kGeneva),
                font_cache.IsFamilyAvailable(NovaFontFamily::kTimes),
                font_cache.IsFamilyAvailable(NovaFontFamily::kHelvetica),
                font_cache.IsFamilyAvailable(NovaFontFamily::kNewYork));
  if (!font_cache.IsFamilyAvailable(NovaFontFamily::kChicago) ||
      !font_cache.IsFamilyAvailable(NovaFontFamily::kGeneva)) {
    NovaLog::Warn(
        "bundled Charcoal.ttf/Geneva.ttf not found next to the executable "
        "(looked in EV Nova/); text will fall back to debug font");
  }

  bool entered_sub_screen = false;
  std::unique_ptr<SdlTexture> docked_snapshot;

  // Activates the currently-selected service, leaving the dock when the player
  // picks Launch. On a mocked sub-screen (trade/outfit/shipyard/bar/...) the
  // modal stays open (DispatchService returns kServiceComplete). Guards against
  // activating a gated/disabled slot, mirroring the original only accepting
  // usable action buttons. Returns true when the dock is left.
  auto activate_selection = [&]() -> bool {
    if (!ServiceAvailable(state, ctx.stellar_id, ctx.selection)) {
      return false;
    }
    LandedExit exit =
        DispatchService(platform,
                        state,
                        ctx,
                        docked_snapshot ? docked_snapshot->get() : nullptr);
    if (exit == LandedExit::kLaunched) {
      return true;
    }
    entered_sub_screen = false;
    return false;
  };

  // Keep mouse coordinates in the same unrestricted window space as the
  // native-size DLOG/DITL geometry, even if the previous context was flight.
  platform.SetFullscreenPlayfield();

  while (!platform.quit_requested()) {
    // Compute the mouse-hovered service (for hover state on the buttons).
    std::optional<std::uint8_t> hovered;
    if (!entered_sub_screen) {
      const auto c = ServiceButtonAt(button_rects, platform.mouse_position());
      // Only enabled slots highlight (the original's hit-test filters gated
      // buttons before returning a hovered index).
      if (c) {
        const auto svc = static_cast<LandedService>(*c);
        hovered =
            ServiceAvailable(state, ctx.stellar_id, svc) ? c : std::nullopt;
      }
    }
    // Draw the current face. When a sub-screen is "open" the original swaps to
    // a nested modal widget; the MVP keeps the same menu surface and only
    // distinguishes via the hint, so we always redraw the list.
    DrawLandedMenu(platform,
                   font_cache,
                   button_art,
                   state,
                   ctx,
                   destination_art ? destination_art->get() : nullptr,
                   planet_art ? planet_art->get() : nullptr,
                   description,
                   panel,
                   layout,
                   button_rects,
                   hovered);
    // Capture the actual dock before SDL presents/swaps its backbuffer.
    docked_snapshot = NovaLanded_CaptureDockedBackground(platform);
    SDL_RenderPresent(platform.renderer());

    // Poll discrete raw keys for the modal (dedicated channel, so it never
    // interferes with the spaceflight flight controls). Keyboard shortcuts are
    // the first/mnemonic letter of each service, matching the travel-scene
    // action dispatch in NovaUi_RunTravelDestinationInteractionLoop
    // (0x00491f30): r/f refuel, c/t trade, o outfit, s shipyard, n mission,
    // b bar, Enter/Esc leave. There is no keyboard focus/highlight state.
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      if (in->key == TextKey::escape) {
        // Esc leaves a sub-screen back to the menu, or leaves the dock.
        if (entered_sub_screen) {
          entered_sub_screen = false;
        } else {
          return LandedExit::kLaunched;
        }
        continue;
      }
      if (in->key == TextKey::enter) {
        // Enter leaves the dock, matching the original's Enter/Esc exit code.
        return LandedExit::kLaunched;
      }
      if (in->key == TextKey::primary) {
        // Left-click a service button: activate the hovered (and only if
        // enabled) slot, like the original's mouse-driven services buttons.
        if (!entered_sub_screen) {
          if (const auto slot =
                  ServiceButtonAt(button_rects, platform.mouse_position())) {
            ctx.selection = static_cast<LandedService>(*slot);
            if (ServiceAvailable(state, ctx.stellar_id, ctx.selection)) {
              LandedExit exit =
                  DispatchService(platform,
                                  state,
                                  ctx,
                                  docked_snapshot ? docked_snapshot->get()
                                                  : nullptr);
              if (exit == LandedExit::kLaunched) {
                return LandedExit::kLaunched;
              }
              entered_sub_screen = false;
              // Do not dispatch another queued key against the just-closed
              // service. The next outer iteration must redraw the dock first,
              // so a following Mission BBS opens over the dock rather than
              // snapshotting the previous store frame.
              break;
            }
          }
        }
        continue;
      }
      if (in->key == TextKey::character) {
        if (entered_sub_screen) {
          continue;
        }
        // First-letter service shortcuts (mirror the travel interaction loop
        // key comparisons). Each is case-insensitive (SDL3 reports letter keys
        // lower-case, but normalise anyway).
        const char kc = static_cast<char>(
            std::tolower(static_cast<unsigned char>(in->character)));
        switch (kc) {
        case 'r':
        case 'f':
          ctx.selection = LandedService::kRefuel;
          break;
        case 'c':
        case 't':
          ctx.selection = LandedService::kBuySellCargo;
          break;
        case 'o':
          ctx.selection = LandedService::kOutfit;
          break;
        case 's':
          ctx.selection = LandedService::kShipyard;
          break;
        case 'n':
          ctx.selection = LandedService::kMissionBoard;
          break;
        case 'b':
          ctx.selection = LandedService::kBar;
          break;
        case 'l':
          // "Launch": a synonym for Enter/Esc (the docked Leave slot).
          return LandedExit::kLaunched;
        default:
          continue;
        }
        if (activate_selection()) {
          return LandedExit::kLaunched;
        }
        // A nested service may have changed the renderer presentation and
        // left its frame visible. Establish a redraw boundary before handling
        // any additional queued input.
        break;
      }
    }
    SDL_Delay(16);
  }
  return LandedExit::kQuit;
}

} // namespace game
