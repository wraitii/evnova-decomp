#pragma once

// Clean-room reconstruction of the new-game intro cinematic, mirroring Ghidra
// 0x0048adc0 IntroCinematic_Run. Plays the pre-configured IntroCinematicData
// frame art (PICTs), each for its 1/60s-tick duration. Enter (0x1c) / Space
// (0x39) fast-forward only the current frame; the primary mouse command
// (_DAT_00591514) latches bVar9 and skips the whole sequence. After the last
// frame, if bVar9 was not set and post_intro_dest_id != -1, it opens (a stub
// of) the post-intro travel-selection dialog.
//
// The return value mirrors bVar9 (true = the intro ran, post-intro dialog
// gate holds; false = skipped by the primary mouse command, dialog suppressed).
//
// Note: like IntroCinematic_Run itself, this function does *not* flip the
// intro-played latch; Ship_RunSpaceflightMode sets DAT_00596d35 after the
// intro returns (moved to spaceflight.cpp's NovaSpaceflight_Run).

#include "game_state.hpp"

class SdlPlatform;

namespace game {

// Plays the intro cinematic sequence described by state.intro_cinematic.
// Returns the mirror of the original's bVar9 skip latch: true if the sequence
// was not skipped by the primary mouse command (so the post-intro destination
// dialog may open), false if skipped by a primary click. Enter/Space
// fast-forward only the current frame and do not flip the return value, exactly
// as in Ghidra.
bool NovaIntroCinematic_Run(SdlPlatform &platform, GameState &state);

} // namespace game
