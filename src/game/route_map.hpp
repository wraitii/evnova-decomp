#pragma once

// Clean-room in-flight route-map overlay (the small non-blocking galaxy chart
// that pops over the flight view while a travel destination is selected).
//
// Original chain (comments live at the cited sites):
// - Open/refresh: NovaUi_UpdateTravelSelectionOverlay 0x004a9b30, called by
//   Ship_HandlePlayerShipCore after destination changes (0x0044b8ab cycle,
//   0x004b95f hyperspace-mode arm) and by the overlay's own click handler.
// - Draw: Ui_DrawSystemRouteMap 0x004a99f0 re-renders the shared starmap
//   marker/route pass into a dedicated square surface (g_routeMapSurfaceFrame
//   0x00596d14, rect g_routeMapScreenRect 0x00733bb4, created in FUN_004ab9d4
//   0x004ab9d4) which Frame_DrawViewportFlashOverlays 0x00439bb0 blits over
//   the flight view with a fade.
// - Zoom: PlayerTick_RouteMapZoomCommands 0x0045216e (minus/equals + numpad
//   -/+ keys, x1.3333 / x0.75, capped [0.5, 2.0]).
// - Clicks: PlayerTick_RouteMapClickBranch 0x0044e027 routes clicks inside the
//   chart to neighbour-system destination selection.
// - Auto-dismiss: Frame_TickHudOverlayAndRouteMapTimers 0x0042f23e clears the
//   flag
//   500 ticks after the last interaction (the chart visually fades at +150).
//
// The reimplementation draws the chart per frame (no offscreen surface) and
// keeps the flag/zoom/timer state in GameState::route_map (RouteMapState,
// defined in game_state.hpp next to the other travel state).

#include "game_state.hpp"
#include "nova_font.hpp"
#include "starmap.hpp"
#include "util/placement.hpp"

namespace game {

// Raw zoom-key state sampled in the flight loop (the original reads raw
// scancodes through NovaInput_IsCommandActiveWithGameplayGuards).
struct RouteMapZoomInput {
  bool zoom_in_held = false;  // DIK 0x0c (minus) / 0x4a (numpad minus)
  bool zoom_out_held = false; // DIK 0x0d (equals) / 0x4e (numpad plus)
  // The modifier-combo guard (0x0045217c..0x004521e3 commands
  // 0x2a/0x36/0x1d/0x6b/0x38/0x6f): suppresses zoom while the modifier combos
  // are held. The port guards on the shift/ctrl/alt keys (0x2a/0x36/0x1d/
  // 0x38); 0x6b/0x6f are TODO(decomp).
  bool modifier_combo_held = false;
};

enum class RouteMapClickResult : std::uint8_t {
  // Overlay down or player station-held: run normal click-to-target.
  kNotHandled,
  // Click outside the chart rect: run normal click-to-target.
  kOutside,
  // Click on the chart centre: the travel selection was cleared.
  kClearedSelection,
  // Click on a neighbour marker: that system became the travel destination.
  kSelectedDestination,
  // Click inside the chart but on no marker: consumed, no state change (the
  // original reconverges without touching the target).
  kConsumedNoHit,
};

// Ghidra 0x004a9b30 NovaUi_UpdateTravelSelectionOverlay. Shows the overlay
// (g_routeMapVisibleFlag = 1 unconditionally -- the selected-system highlight
// gate only decides what the chart emphasizes), stamps the interaction timer
// and resets the zoom when it fell to/below the minimum (TODO(decomp): the
// reset threshold DAT_005759e8 is an unresolved global; the port uses the 0.5
// zoom floor).
void RouteMap_Open(GameState &state);

// Ghidra 0x0045216e PlayerTick_RouteMapZoomCommands (zoom steps, edge-latched
// via g_playerRouteMapZoomCommandLatch, each step re-stamps the interaction
// timer and queues the transition-table click sound) plus the auto-dismiss
// tail of Frame_TickHudOverlayAndRouteMapTimers at 0x0042f23e (flag clears
// 500 ticks after the last interaction; the
// zoom handling stays live in the original's invisible-but-flagged window).
void RouteMap_Tick(GameState &state, const RouteMapZoomInput &input);

// Ghidra Ship_HandlePlayerShipCore multi-exit synthetic CFG:
// 0x0044E035 -> [0x0044BC1E, 0x0044E490], entered through
// PlayerTick_RouteMapClickBranch at 0x0044E027. While the overlay is up and
// ai_station_hold_timer <= 0, clicks inside the overlay rect select an adjacent
// system as travel destination or clear the selection at the chart centre;
// everything else keeps normal click-to-target. `window_x`/`window_y` are raw
// window points; the shared overlay placement maps them into the authored
// chart rect (see RouteMap_OverlayPlacement).
RouteMapClickResult RouteMap_HandleClick(GameState &state,
                                         SdlPlatform &platform,
                                         float window_x,
                                         float window_y);

// Ghidra FUN_004ab9d4 rect derivation: top-left square of the view, side
// round(view_width * DAT_00575a80) clamped to min 200, where DAT_00575a80 is
// the double 0.25 and the width is the live render-owner width. Returned in
// authored chart units (the overlay placement maps it to the window).
[[nodiscard]] SDL_FRect RouteMap_OverlayRect(class SdlPlatform &platform);

// The route-map overlay's placement, shared by Draw and the click hit-test so
// they cannot disagree: top-left anchored, authored square `(0,0,b,b)` mapped
// by `s_map = min(U, W/b, H/b)`. DIVERGENCE(original):
// the responsive b = max(200, round(W*0.25)) matches the original's live-width
// derivation rather than the authored-UI model; at U = 1 it reproduces today's
// window-proportional square.
[[nodiscard]] Placement RouteMap_OverlayPlacement(class SdlPlatform &platform);

// Fade of the chart for the current tick (Ghidra 0x00439bd0 blit tint):
// solid until +118 ticks after the last interaction, fades out over the next
// 32, fully gone at +150 (while the flag lingers until +500 -- original
// quirk, see the auto-dismiss comment).
[[nodiscard]] float RouteMap_FadeAlpha(const GameState &state);

// Session assets for the chart (Geneva label font cache + marker CICNs + the
// c\x9alr frame colour), held by the flight loop.
class RouteMapView {
public:
  void Load(class SdlPlatform &platform);

  // Ghidra 0x004a99f0 Ui_DrawSystemRouteMap + the 0x00439bd0 blit: renders
  // the chart at RouteMapState::zoom_scale centred on the current system into
  // the overlay rect, multiplied by the fade alpha (1.0 = opaque). No-op
  // while the overlay is down or fully faded.
  void Draw(class SdlPlatform &platform, const GameState &state);

private:
  NovaFontCache font_cache_;
  NovaStarmap_MarkerIcons icons_;
  bool icons_loaded_ = false;
  // Frame colour (Ghidra DAT_00735658 <- c\x9alr style floating_map +0x8a).
  // Black until Load resolves the Colors record, matching the global default.
  SDL_Color border_color_{0, 0, 0, SDL_ALPHA_OPAQUE};
};

} // namespace game
