#pragma once

// Clean-room reconstruction of the new-game flow, mirroring Ghidra
// 0x00489d70 Menu_RunNewGameFlow. The original runs a chain of modal dialogs
// and setup steps synchronously; this reimplementation keeps the same ordering
// and per-step semantics. The pilot-selection dialog is a port of
// Menu_RunPilotSelectionDialog (0x0048a7e0) running on the SDL-backed dialog
// runtime (ui_dialog.hpp) with the real DLOG 0xc1d/0xc1e resources; the
// ship-christening box is the shared text-entry dialog (0x00497900, DLOG
// 0xbb9). Each deeply-data-dependent setup step is isolated behind a small
// function so unsupported tables can be stubbed with an explicit log. See
// new_pilot_flow.cpp for the per-step NOTES/DIVERGENCE marks.

#include "game_state.hpp"

#include <functional>

class SdlPlatform;

namespace game {

// Ghidra 0x004b3350 Ship_ResetPlayerShipState. Resets slot 0 and the
// per-flight/per-pilot tables before either a new game or pilot-file load.
void NovaShip_ResetPlayerShipState(GameState &state);

// Runs the full "new pilot" path from the main menu: picks random opener
// strings, runs the pilot-name/selection step, resolves the start type,
// resets the per-pilot world for a fresh character, loads scenario tables,
// and configures the intro cinematic. Returns true if a new pilot was created
// (the caller should switch to the intro cinematic / in-game mode). On cancel
// or on any skipped-but-required step it returns false and the main menu stays
// up. Mirrors Menu_RunNewGameFlow's early returns on dialog cancel.
// `render_background` is invoked once per dialog frame to keep the menu
// rendering behind the modal windows (see ui_dialog.hpp).
bool NovaNewPilotFlow_Run(SdlPlatform &platform,
                          GameState &state,
                          const std::function<void()> &render_background = {});

// Test seam for the Game_ResetNewGameState (0x004b4690) stellar Strength/hazard
// reset: starts-destroyed bodies (availability_flags 0x40) go to live strength
// -1 with the regeneration countdown pinned, all others reset to the loaded
// strength capacity.
void NovaNewPilot_ResetStellarStrengthForNewGame(GameState &state);

} // namespace game
