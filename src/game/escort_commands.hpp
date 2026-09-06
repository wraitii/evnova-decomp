#pragma once

// In-flight Escort Commands overlay + order dispatch (Ghidra
// PlayerTick_AuxiliaryCommands blocks 0x00450ae7/0x00450b4e/0x00450cf4 and
// Ship_CommandPlayerEscortGroup 0x0045c880). The E key (binding slot 0x2a)
// toggles the overlay; the number keys 1..5 (slots 0x2b..0x2f, the original's
// "panel-suppressed command" list) select the group -- 1 All Ships, then one
// row per ShipClassDef.class_category (0 Fighters / 1 Medium Ships /
// 2 Warships / 3 Freighters, the Bible EscortType classes); the order keys
// F/D/V/C (slots 0x30..0x33) issue Attack / Defend / Hold Position /
// Formation, with the arm-modifier pair turning Formation into Return to
// Hangar. Orders dispatch to attached ships (squad_leader_ship_slot == 0);
// with the panel closed they always target every attached ship.

#include <array>
#include <cstdint>

#include "game_state.hpp"

namespace game {

// Order codes stored in EscortCommandState.group_command (STR# 0x7d2
// 0x91..0x95 wording). 0 = Formation (the neutral default).
enum class EscortOrder : std::int16_t {
  kFormation = 0,
  kDefend = 1,
  kAttack = 2,
  kReturnToHangar = 3,
  kHoldPosition = 4,
};

// Edge/level state of the escort-command keys, resolved by the caller from
// the binding table (slots 0x2a, 0x2b..0x2f, 0x30..0x33 and the 0x38/0x6f
// arm-modifier pair) against the live keyboard state.
struct EscortCommandInput {
  bool panel_toggle_held = false;
  // Number keys 1..5, in panel-row order (row 0 = All Ships).
  std::array<bool, 5> select_group_held{};
  bool order_attack_held = false;    // slot 0x30
  bool order_defend_held = false;    // slot 0x31
  bool order_hold_held = false;      // slot 0x32
  bool order_formation_held = false; // slot 0x33
  bool arm_modifier_held = false;    // the 0x38/0x6f modifier pair
};

// Ghidra PlayerTick_AuxiliaryCommands escort blocks: panel toggle (0x00450ae7
// + arms 0x00452820/0x0045293b/0x004529a8), selection rows (0x00450c24..),
// order dispatch (0x00450cf4.. + far arms 0x00452a52..0x00452af5) and the
// per-category auto-cancel arms (0x00450ddb..0x00450f67). `now_60hz` is the
// 60 Hz tick counter (NovaTime_GetTickCount60Hz).
void NovaEscort_TickPlayerEscortCommands(GameState &state,
                                         const EscortCommandInput &input,
                                         std::int64_t now_60hz);

// True when at least one active ship is attached to the player (ai_target_
// ship_slot == 0) whose class_category matches; used by the Escort Commands
// overlay to dim absent groups (Ui_DrawTargetCategoryPanel 0x0049e430).
[[nodiscard]] bool NovaEscort_GroupPresent(const GameState &state,
                                           std::int16_t category);

// Ghidra 0x0045c880 Ship_CommandPlayerEscortGroup: applies `command` to every
// active ship attached to the player (squad_leader_ship_slot == 0) whose class
// category matches, or to all of them when `category` is -1. Deployed
// fighters (ai_behavior_code 5) take every order; other attached ships accept
// every order except Return to Hangar, which reverts to Formation. Attack
// propagates the player's primary target to accepted ships. Returns whether
// any ship accepted; unless `suppress_message`, shows the
// "New escort orders assigned: ..." overlay (STR# 0x7d2 0x86..0x8b + 0x96..).
bool NovaEscort_CommandPlayerEscortGroup(GameState &state,
                                         std::int16_t category,
                                         std::int16_t command,
                                         bool suppress_message);

} // namespace game
