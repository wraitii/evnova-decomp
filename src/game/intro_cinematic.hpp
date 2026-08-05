#pragma once

// Clean-room reconstruction of the new-game intro cinematic, mirroring Ghidra
// 0x0048adc0 IntroCinematic_Run. Plays the pre-configured IntroCinematicData
// frame art (PICTs), each for its 1/60s-tick duration with input-to-skip, then
// exposes whether it was skipped so the caller can go straight to in-game.

#include "game_state.hpp"

class SdlPlatform;

namespace game {

// Plays the intro cinematic sequence described by state.intro_cinematic.
// Returns true if the sequence ran to completion (or there were zero frames),
// false if the player skipped/quit partway so the caller can skip any
// post-intro dialog. Mirrors IntroCinematic_Run's effect of *not* opening the
// travel-selection dialog on skip.
bool NovaIntroCinematic_Run(SdlPlatform &platform, GameState &state);

} // namespace game
