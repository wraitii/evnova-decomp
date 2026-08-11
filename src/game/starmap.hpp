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
// Remaining divergences from the original: no mission-highlight route editing,
// no starmap search dialog and no licence-seed easter-egg branch. The focus is
// a faithful, useful navigation surface for the jump player.
//
// The political/government overlay (the original's
// NovaUi_DrawStarmapPoliticalOverlay) IS implemented: fading government discs
// behind every discovered, reachable system, toggled by the Show/Hide Borders
// button (ON by default). Unlike the original's opaque 16px overlay blocks it
// renders the same strength field per-pixel as a smooth, translucent fade.

#include "../sdl_platform.hpp"
#include "game_state.hpp"

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
[[nodiscard]] StarmapResult NovaStarmap_RunWindow(SdlPlatform &platform,
                                                  GameState &state);

} // namespace game
