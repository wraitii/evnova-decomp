#pragma once

// Clean-room reconstruction of the new-game flow, mirroring Ghidra
// 0x00489d70 Menu_RunNewGameFlow. The original runs a chain of modal dialogs
// and setup steps synchronously; this reimplementation keeps the same ordering
// and per-step semantics, but each deeply-data-dependent subsystem is isolated
// behind a small function so unsupported tables can be stubbed with an
// explicit log. See new_pilot_flow.cpp for the per-step NOTES/DIVERGENCE marks.

#include "game_state.hpp"

class SdlPlatform;

namespace game {

// Runs the full "new pilot" path from the main menu: picks random opener
// strings, runs the pilot-name/selection step, resolves the start type,
// resets the per-pilot world for a fresh character, loads scenario tables,
// and configures the intro cinematic. Returns true if a new pilot was created
// (the caller should switch to the intro cinematic / in-game mode). On cancel
// or on any skipped-but-required step it returns false and the main menu stays
// up. Mirrors Menu_RunNewGameFlow's early returns on dialog cancel.
bool NovaNewPilotFlow_Run(SdlPlatform &platform, GameState &state);

} // namespace game
