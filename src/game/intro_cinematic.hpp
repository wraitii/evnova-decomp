#pragma once

// Clean-room reconstruction of the new-game intro cinematic, mirroring Ghidra
// 0x0048adc0 IntroCinematic_Run. Plays the pre-configured IntroCinematicData
// frame art (PICTs), each for its 1/60s-tick duration. Enter (0x1c) / Space
// (0x39) fast-forward only the current frame; the primary mouse command
// (_DAT_00591514) latches bVar9 and skips the whole sequence. After the last
// frame, if bVar9 was not set and intro_text_desc_id != -1, it shows that
// desc in (a stub of) the generic text-reader dialog.
//
// The return value mirrors bVar9 (true = the intro ran, intro-text dialog
// gate holds; false = skipped by the primary mouse command, dialog suppressed).
//
// Note: like IntroCinematic_Run itself, this function does *not* flip the
// intro-played latch; Ship_RunSpaceflightMode sets g_intro_played after the
// intro returns (moved to spaceflight.cpp's NovaSpaceflight_Run).

#include "game_state.hpp"

class SdlPlatform;
class SdlAudio;

namespace game {

// Ghidra 0x004cd3b0 IntroCinematic_SetupFrames: fills state.intro_cinematic
// from the pilot-save block keyed by `block_key` (the selected character
// template's registered name; Menu_RunNewGameFlow falls back to family entry
// 1 when the 0xc1e dialog variant left the selection empty). Absent block ->
// the no-save default (single PICT 0x2008 for 10 ticks, intro text 0x7ffd).
// Frame ids below 0x80 are rewritten to -1 with duration 0; durations clamp
// to [0, 300] 1/60s ticks.
void NovaIntroCinematic_SetupFrames(GameState &state,
                                    std::string_view block_key);

// Plays the intro cinematic sequence described by state.intro_cinematic.
// Returns the mirror of the original's bVar9 skip latch: true if the sequence
// was not skipped by the primary mouse command (so the intro-text dialog may
// open), false if skipped by a primary click. Enter/Space
// fast-forward only the current frame and do not flip the return value, exactly
// as in Ghidra.
bool NovaIntroCinematic_Run(SdlPlatform &platform,
                            SdlAudio &audio,
                            GameState &state);

} // namespace game
