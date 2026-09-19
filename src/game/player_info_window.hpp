#pragma once

// Clean-room reconstruction of EV Nova's in-flight Player Info window (the
// manual's "press the P key" dialog: General / Cargo / Extras / Honors pages
// plus a Jettison Cargo action). Despite the historical Ghidra names
// ("player special interaction"), this has nothing to do with missions'
// special ships; see docs/player_info_window.md for the full ground truth.
//
// The family:
//   NovaUi_RunPlayerSpecialInteractionWindow  0x00499c10  (run + teardown)
//   NovaUi_DrawPlayerInfoWindow               0x0049a540  (per-frame paint)
//   (dispatch callback)                       0x0049a3a0  (keys + tab clicks)
//   NovaUi_HandlePlayerSpecialInteractionTabs 0x004a1ae0  (press tracking)
//   NovaUi_DrawPlayerSpecialInteractionTabs   0x004a1c40  (strip painter)
//   NovaUi_BuildPlayerSpecialInteractionStrings 0x0049c050 (page texts)

#include <cstdint>
#include <string>

class HudRenderer;
class SdlPlatform;

namespace game {

struct GameState;
class SpaceflightView;

// Ghidra 0x0049c050 NovaUi_BuildPlayerSpecialInteractionStrings: the three
// long-form page texts (cargo 0x7d5278 / extras 0x7d6278 / honors 0x7d7278).
struct PlayerInfoSummaryTexts {
  std::string cargo;
  std::string extras;
  std::string honors;
};

[[nodiscard]] PlayerInfoSummaryTexts
NovaPlayerInfo_BuildSummaryTexts(const GameState &state);

// Ghidra 0x00469030 NovaUi_DrawCombatRankLabel. The zero-based rank selects
// STR# 138 entry rank+1.
[[nodiscard]] int NovaPlayerInfo_CombatRankIndex(std::int32_t points);

// Outcome of one Player Info session. The original latches the jettison arm
// as a local flag in the run loop before closing.
struct PlayerInfoWindowResult {
  // The player confirmed the jettison prompt on the Cargo page. The caller
  // applies the fleet-cargo jettison (Player_RedistributeFleetCargoOverflow
  // 0x0041f330) with the sim clock in scope; see NovaPlayerInfo_RunWindow.
  bool jettison_confirmed = false;
};

// Ghidra 0x00499c10 NovaUi_RunPlayerSpecialInteractionWindow (clean-room).
// Runs the modal Player Info window (DLOG 0x3f9, backdrop PICTs
// 0x2146-0x2148) over the live game view until the player presses Done,
// Escape/Enter, the bound Player Info command, or confirms a jettison. Pages
// 1-4 are selected with the tab strip or Tab (shift reverses, wrapping 1..4),
// exactly as in the dispatch callback 0x0049a3a0. The flight simulation is
// paused while the window is open, as in the original.
[[nodiscard]] PlayerInfoWindowResult
NovaPlayerInfo_RunWindow(SdlPlatform &platform,
                         GameState &state,
                         SpaceflightView &view,
                         HudRenderer &hud);

} // namespace game
