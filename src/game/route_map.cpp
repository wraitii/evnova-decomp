// Ghidra: in-flight route-map overlay (see route_map.hpp for the chain map).
#include "route_map.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "travel.hpp"

namespace game {
namespace {

// Fade / dismissal schedule (Ghidra 0x00439bd0 tint math + 0x0042f23e auto-
// dismiss), in 60Hz ticks after g_routeMapInteractionTick60hz.
constexpr std::uint32_t kFadeStartTicks = 0x96 - 0x20; // 118: solid until here
constexpr std::uint32_t kFadeEndTicks = 0x96;          // 150: fully faded
constexpr std::uint32_t kDismissTicks = 0x1f4;         // 500: flag clears

// Zoom step factors and caps (0x0045222a zoom-in vs g_lit_2p0, 0x00452278
// zoom-out vs g_bomb_damage_armor_fraction = 0.5).
constexpr float kZoomInStep = 1.3333F;
constexpr float kZoomOutStep = 0.75F;
constexpr float kZoomMax = 2.0F;
constexpr float kZoomMin = 0.5F;

// Route-map surface side as a fraction of the view width (Ghidra
// DAT_00575a80 0x00575a80, the double 0.25 read by the only xref, the
// `FMUL double ptr [DAT_00575a80]` at 0x004abc66).
constexpr float kRouteMapViewScale = 0.25F;

[[nodiscard]] const System *CurrentSystem(const GameState &state) {
  const std::int16_t current = state.player.current_system_id;
  if (current < 0 ||
      static_cast<std::size_t>(current) >= state.scenario.systems.size()) {
    return nullptr;
  }
  return &state.scenario.systems[static_cast<std::size_t>(current)];
}

} // namespace

// @port 0x004A9B30 95% correctness
// Ghidra 0x004a9b30 NovaUi_UpdateTravelSelectionOverlay: zoom reset gate, flag
// set, interaction stamp.
// The original reset threshold is a separate global, DAT_005759e8 = 0.0 (not
// the 0.5 zoom-out clamp kZoomMin), so it is effectively dead; the port resets
// at <= kZoomMin instead, visible when a fully zoomed-out map is reopened.
// TODO(decomp(0x004a9b30)): align the reset threshold to 0.0.
// The mission-target list rebuild the original performs here happens at draw
// time in the port. The highlight (g_starmap_selected_system_id) is re-derived
// at draw time from travel_transfer_mode/travel_slot.
void RouteMap_Open(GameState &state) {
  if (state.route_map.zoom_scale <= kZoomMin) {
    state.route_map.zoom_scale = 1.0F;
  }
  state.route_map.overlay_visible = true;
  state.route_map.interaction_tick_60hz = state.tick_60hz;
}

void RouteMap_Tick(GameState &state, const RouteMapZoomInput &input) {
  auto &rm = state.route_map;
  // Auto-dismiss (0x0042f23e): 500 ticks after the last interaction. The
  // original leaves zoom + click handling live until this fires even though
  // the chart is invisible past +150 ticks (quirk preserved).
  if (rm.overlay_visible &&
      state.tick_60hz > rm.interaction_tick_60hz + kDismissTicks) {
    rm.overlay_visible = false;
    return;
  }
  if (!rm.overlay_visible) {
    return;
  }
  // Ghidra 0x0045216e: zoom-in while scale < 2.0, zoom-out while scale >
  // 0.5, edge-latched; TODO(decomp): the original additionally skips zoom
  // while g_navigation_override_done is set (0x0045217c), not modelled.
  if (input.modifier_combo_held) {
    rm.zoom_command_latch = input.zoom_in_held || input.zoom_out_held;
    return;
  }
  const bool zoom_latch = input.zoom_in_held || input.zoom_out_held;
  if (input.zoom_in_held && !rm.zoom_command_latch &&
      rm.zoom_scale < kZoomMax) {
    rm.zoom_scale *= kZoomInStep;
    rm.interaction_tick_60hz = state.tick_60hz;
    state.pending_ui_sounds.push_back({0, 1});
  } else if (input.zoom_out_held && !rm.zoom_command_latch &&
             rm.zoom_scale > kZoomMin) {
    rm.zoom_scale *= kZoomOutStep;
    rm.interaction_tick_60hz = state.tick_60hz;
    state.pending_ui_sounds.push_back({0, 1});
  }
  rm.zoom_command_latch = zoom_latch;
}

RouteMapClickResult RouteMap_HandleClick(GameState &state,
                                         SdlPlatform &platform,
                                         float click_x,
                                         float click_y) {
  auto &rm = state.route_map;
  // 0x0044e027 gate: overlay up and the player not station-held (the x87
  // chain proceeds only when ai_station_hold_timer <= 0.0).
  if (!rm.overlay_visible || state.player.ai_station_hold_timer > 0.0F) {
    return RouteMapClickResult::kNotHandled;
  }
  const SDL_FRect rect = RouteMap_OverlayRect(platform);
  const bool inside = click_x >= rect.x && click_x < rect.x + rect.w &&
                      click_y >= rect.y && click_y < rect.y + rect.h;
  if (!inside) {
    return RouteMapClickResult::kOutside;
  }
  const System *sys = CurrentSystem(state);
  if (sys == nullptr) {
    return RouteMapClickResult::kNotHandled;
  }
  const float centre_x = rect.x + rect.w * 0.5F;
  const float centre_y = rect.y + rect.h * 0.5F;

  auto clear_selection = [&] {
    // 0x0044e200: mode 3, slot -1 (selection cleared), dirty flag, overlay
    // refresh + click sound (transition table entry 2).
    state.travel.travel_slot = -1;
    state.travel.starmap_destination_system_id = -1;
    state.travel.destination_system_id = -1;
    state.player.travel_transfer_mode = 3;
    state.travel.selected_stellar_id = -1;
    state.travel.selected_stellar_is_manual = false;
    RouteMap_Open(state);
    state.pending_ui_sounds.push_back({2, 1});
  };

  // Centre hit (0x0044e0c8: centre rect inset -8): clears the selection.
  if (std::fabs(click_x - centre_x) <= 8.0F &&
      std::fabs(click_y - centre_y) <= 8.0F) {
    clear_selection();
    return RouteMapClickResult::kClearedSelection;
  }

  // Neighbour hit-test (0x0044e240..0x0044e488): each current-system
  // adjacency link resolves (System_ResolveVisibleSystemForTravel) and is
  // hit-tested at centre + (neighbour.pos - current.pos) / zoom, truncated
  // toward zero, with a half-extent of 12 when zoom >= 0.5 else 8
  // (0x0044e3ee compares against g_bomb_damage_armor_fraction = 0.5).
  const float half = rm.zoom_scale >= 0.5F ? 12.0F : 8.0F;
  for (std::size_t slot = 0; slot < sys->links.size(); ++slot) {
    const std::int16_t link = sys->links[slot];
    if (link < 0x80) {
      continue;
    }
    const std::int16_t dest = NovaSystem_ResolveVisibleForTravel(
        state, static_cast<std::int16_t>(link - 0x80));
    if (dest < 0 ||
        static_cast<std::size_t>(dest) >= state.scenario.systems.size()) {
      continue;
    }
    const System &target =
        state.scenario.systems[static_cast<std::size_t>(dest)];
    const float sx = std::trunc(centre_x + (static_cast<float>(target.pos_x) -
                                            static_cast<float>(sys->pos_x)) /
                                               rm.zoom_scale);
    const float sy = std::trunc(centre_y + (static_cast<float>(target.pos_y) -
                                            static_cast<float>(sys->pos_y)) /
                                               rm.zoom_scale);
    if (click_x < sx - half || click_x > sx + half || click_y < sy - half ||
        click_y > sy + half) {
      continue;
    }
    // 0x0044e452: slot = adjacency index, mode 3, dirty flag, overlay
    // refresh + click sound.
    state.travel.travel_slot = static_cast<std::int16_t>(slot);
    state.travel.starmap_destination_system_id = dest;
    state.travel.destination_system_id = dest;
    state.player.travel_transfer_mode = 3;
    state.travel.selected_stellar_id = -1;
    state.travel.selected_stellar_is_manual = false;
    RouteMap_Open(state);
    state.pending_ui_sounds.push_back({2, 1});
    return RouteMapClickResult::kSelectedDestination;
  }
  // No marker hit: the original consumes the click (reconverges at
  // 0x0044bc1e without touching the target).
  return RouteMapClickResult::kConsumedNoHit;
}

SDL_FRect RouteMap_OverlayRect(SdlPlatform &platform) {
  // Ghidra NovaUi_InitializeFlightViewSurfaces 0x004ab9d4 (rect assembly at
  // 0x004abc33..0x004abc73): a top-left square whose side is
  // round((view_right - view_left) * DAT_00575a80), clamped to min 200, where
  // DAT_00575a80 is the double 0.25 (see kRouteMapViewScale) and the width is
  // the live render-owner width. The flight view tracks the window
  // (SpaceflightView::DrawGameFrame uses PlaceWindow), so the logical
  // playfield width is that view width. Clicks and draws share window-point
  // coordinates in the fullscreen flight presentation.
  float side =
      std::round(platform.logical_playfield_size().x * kRouteMapViewScale);
  if (side < 200.0F) {
    side = 200.0F;
  }
  return SDL_FRect{0.0F, 0.0F, side, side};
}

float RouteMap_FadeAlpha(const GameState &state) {
  const auto &rm = state.route_map;
  if (!rm.overlay_visible) {
    return 0.0F;
  }
  // 0x00439bd0 tint: outer tint = clamp(interaction + 0x96 - now, 0..0x20);
  // tint 0 = full colour, so alpha = 1 - tint/0x20.
  const std::uint32_t elapsed = state.tick_60hz > rm.interaction_tick_60hz
                                    ? state.tick_60hz - rm.interaction_tick_60hz
                                    : 0;
  if (elapsed <= kFadeStartTicks) {
    return 1.0F;
  }
  if (elapsed >= kFadeEndTicks) {
    return 0.0F;
  }
  return 1.0F - static_cast<float>(elapsed - kFadeStartTicks) /
                    static_cast<float>(kFadeEndTicks - kFadeStartTicks);
}

void RouteMapView::Load(SdlPlatform &platform) {
  if (!icons_loaded_) {
    icons_ = NovaStarmap_LoadMarkerIcons(platform);
    icons_loaded_ = true;
  }
  // Ghidra 0x004c686e: the overlay frame uses DAT_00735658, the c\x9alr
  // Colors style floating_map colour at style +0x8a. A missing Colors record
  // leaves the original global at its zero default (black).
  if (const auto style = NovaResource_LoadMainMenuStyle()) {
    border_color_ = SDL_Color{style->floating_map.red,
                              style->floating_map.green,
                              style->floating_map.blue,
                              SDL_ALPHA_OPAQUE};
  }
}

void RouteMapView::Draw(SdlPlatform &platform, const GameState &state) {
  const float alpha = RouteMap_FadeAlpha(state);
  if (alpha <= 0.0F) {
    return;
  }
  // Highlight: the resolved adjacent system of the armed travel slot (the
  // original's g_starmap_selected_system_id derivation in 0x004a9b30).
  std::int16_t selected = -1;
  if (state.player.travel_transfer_mode == 3) {
    const System *sys = CurrentSystem(state);
    const std::int16_t slot = state.travel.travel_slot;
    if (sys != nullptr && slot >= 0 &&
        slot < static_cast<std::int16_t>(sys->links.size()) &&
        sys->links[static_cast<std::size_t>(slot)] >= 0x80) {
      selected = NovaSystem_ResolveVisibleForTravel(
          state,
          static_cast<std::int16_t>(sys->links[static_cast<std::size_t>(slot)] -
                                    0x80));
    }
  }
  NovaStarmap_DrawRouteMapChart(platform,
                                font_cache_,
                                state,
                                RouteMap_OverlayRect(platform),
                                state.route_map.zoom_scale,
                                selected,
                                alpha,
                                icons_,
                                border_color_);
}

} // namespace game
