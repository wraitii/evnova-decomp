#pragma once

// Clean-room galaxy starmap modal (the in-flight navigation map). The original
// opens this window from Ship_HandlePlayerShip (0x0044b120) when its map
// command is active and the ship is not busy (not landing, not hyperspacing,
// not dead); the modal is NovaUi_RunStarmapWindow (0x004a3aa0) and its
// redraw helper NovaUi_RedrawStarmapWindow (0x004a51f0). The reimplementation
// models the same thing with an SDL3-rendered galaxy graph over the flight
// scene: each System is a marker at its (pos_x,pos_y) mapped into the panel,
// adjacency links are drawn between System.links, and systems that the pilot
// has explored are distinguished from unexplored ones. The player pans/zooms
// and picks a system to inspect; the map does NOT perform the jump itself
// (that stays with NovaTravel_Tick / the 'j' travel command), but it shows the
// reachable neighbourhood so the player can plan the next jump.
//
// The window is the actual starmap dialog resource: DLOG 0x7d0 (601x513) with
// the PICT 0x213d "Map" starfield backdrop blitted as the frame, DITL 0x7d0
// item 2 as the galaxy-graph viewport (UiPanel_GetEntryInfo(window, 3)), item 5
// as the right-hand selected-system detail column (entry 6) and item 1 as the
// bottom status bar (entry 2).
//
// Remaining divergences from the original: the inline Find stands in for the
// modal search dialog (DLOG 0xbbd), the destination-window route-editing
// sub-flow (DAT_007354a6 / hypergate destination selection through the map)
// is not reconstructed, and the Show/Hide Borders preference defaults ON
// (no prefs store yet; the original defaults OFF and persists the choice).
// The view (zoom divisor + pan origin) and the plotted route persist across
// map sessions in GameState; the route re-arms hop-by-hop on jump arrival.
//
// The political/government overlay (NovaUi_DrawStarmapPoliticalOverlay) is
// rendered from the original's exact strength field: radius
// round(22/zoom)+12 grid units (1 unit = 2 screen px, so ~102px at the
// default zoom, discs merging into territory blobs), strength
// clamp((r^2-d^2)*fade*zoom, 1..255), painted opaque as theme*strength/512
// (16-bit RGBColor >> 8). The clean-room rasterizes the same field per-pixel
// instead of the original's 16px blocks, so the gradient reads smooth at any
// window scale. The map graph is fog-gated: markers draw for visited systems
// plus the one-jump-ahead reveal latch (grey rings for the latched-unvisited),
// status colours for visited ones; labels for visited systems only. The per-
// system visibility NCB that can hide systems is TODO(decomp).
// When opened from spaceflight the flight frame keeps rendering behind the
// modal window (world + HUD visible around it); the galaxy viewport itself is
// opaque black. Docked callers fall back to a plain backdrop (logged
// divergence).

#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "hud_renderer.hpp"
#include "spaceflight_view.hpp"

namespace game {

// Result of the starmap modal session. The map never performs a jump or
// landing itself -- it only inspects/selects the galaxy -- but the selected
// destination is surfaced so the caller can plot the next jump.
enum class StarmapExit {
  kContinue, // normal close back to the flight scene
  kQuit,     // the app is quitting (platform.quit_requested)
};

struct StarmapResult {
  StarmapExit exit = StarmapExit::kContinue;
  // The zero-based scenario-system id the player left highlighted in the map,
  // or -1 when no selection was made. The caller may treat this as a plotted
  // next-jump destination.
  std::int16_t destination_system_id = -1;
};

// Runs the modal galaxy starmap until the player closes it or the app quits.
// Opens over the current flight scene (the caller should have already drawn
// and presented the frame). Reads the scenario's System table, the player's
// current System, and the pilot's explored-system bits; it does not mutate
// gameplay state. Requires a valid scenario (a no-scenario starmap is a no-op
// that returns kContinue with no destination). On close the returned
// StarmapResult carries the destination system id the player left selected.
// `preselected_system_id` (>= 0) seeds the highlighted system, mirroring
// g_starmap_selected_system_id which NovaUi_RunMissionComputerWindow
// (0x00446150) arms from the selected mission's destination before opening
// the map; -1 keeps the default current-system selection.
//
// `flight_view`/`hud` (spaceflight caller) composite the modal over the live
// game frame: the world + HUD render behind the window and the world re-draws
// clipped to the galaxy viewport, so the moving starfield shows through the
// map like the original. Callers without a flight view (docked menus) fall
// back to the opaque PICT backdrop.
[[nodiscard]] StarmapResult
NovaStarmap_RunWindow(SdlPlatform &platform,
                      GameState &state,
                      std::int16_t preselected_system_id = -1,
                      SpaceflightView *flight_view = nullptr,
                      HudRenderer *hud = nullptr);

} // namespace game
