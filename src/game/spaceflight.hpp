#pragma once

// Clean-room reconstruction of the in-game spaceflight mode, mirroring Ghidra
// 0x00489210 Ship_RunSpaceflightMode and its contained Frame_SpaceflightLoop.
// The full flight/AI AI/combat simulation is not reconstructed; this module is
// a faithful *mode container*: it plays the new-game intro cinematic on first
// entry, then runs an in-system main loop whose gameplay is a documented stub
// (see spaceflight.cpp for the divergence marks). Entering and leaving mirrors
// the original mode transition between the UI shell (main menu) and active
// flight.

#include "game_state.hpp"

class SdlPlatform;

namespace game {

// Enters in-system spaceflight mode. On the pilot's first entry (state
// intro_played == false) it first plays the intro cinematic, then sets
// intro_played = true immediately after it returns -- mirroring
// Ship_RunSpaceflightMode setting DAT_00596d35 = 0x01 right after
// IntroCinematic_Run(). Then it runs the in-game main loop until the player
// returns to the menu, at which point it returns. Mirrors
// Ship_RunSpaceflightMode's gating (DAT_00596d35) and clean return to the menu
// shell.
void NovaSpaceflight_Run(SdlPlatform &platform, GameState &state);

} // namespace game
