#include "mission_script.hpp"

#include "brgr_archive.hpp"
#include "compatibility.hpp"
#include "hud_overlay.hpp"
#include "impact_effects.hpp"
#include "log.hpp"
#include "mission.hpp"
#include "mission_trace.hpp"
#include "nova_random.hpp"
#include "outfit.hpp"
#include "rank.hpp"
#include "ship_ai.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace game {
namespace {

constexpr std::int16_t kResourceIdBase = 0x80;
constexpr std::uint32_t kStringTableType = 0x53545223U;

// The original runs every script byte through the MetroWerks C-locale toupper
// (MWRuntime_ToUpper 0x004d6260, once misnamed NovaCommand_TranslateByInputMap)
// before dispatching. It is an uppercase fold for ASCII letters and identity
// otherwise (the shipped scripts mix `R(b279 b316)` and
// `d358 R(g374 g261)`), so the clean room uses std::toupper.
[[nodiscard]] char TranslateScriptChar(char value) {
  return static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
}

[[nodiscard]] std::optional<std::string>
LoadRandomStringListEntry(std::mt19937 &rng, std::int32_t resource_id) {
  if (resource_id < 0 || resource_id > 0xffff) {
    return std::nullopt;
  }
  const auto bytes = NovaResource_Load(kStringTableType,
                                       static_cast<std::uint16_t>(resource_id));
  if (!bytes || bytes->size() < 2) {
    return std::nullopt;
  }
  const auto count = static_cast<std::uint16_t>(
      (std::to_integer<std::uint8_t>((*bytes)[0]) << 8U) |
      std::to_integer<std::uint8_t>((*bytes)[1]));
  if (count == 0) {
    return std::nullopt;
  }
  // Entry numbers are 1-based (the original rolls NovaRandom_Range(count) + 1,
  // see Mission_PopulateMissionSlotFromDef 0x0043f8c0).
  std::uniform_int_distribution<std::uint16_t> pick(1, count);
  return NovaHud_LoadStringEntry(static_cast<std::uint16_t>(resource_id),
                                 pick(rng));
}

void ReplaceShipTitleMarkers(std::string &title, std::string_view old_name) {
  for (std::size_t pos = 0; pos < title.size(); ++pos) {
    if (title[pos] != '*') {
      continue;
    }
    title.replace(pos, 1, old_name);
    pos += old_name.size();
  }
}

// Ghidra 0x00449370 Mission_ExecuteMisnScriptEngine, cases 'C'/'E'/'H'
// (0x00449824..0x00449978). The original reconciles the outfit pool before the
// swap, drops non-persistent owned outfits for H (Flags 0x24 clear -- 0x04
// "persistent when trading ships" OR 0x20 "persistent for mission set
// operators"), rebuilds the banks from the surviving loadout, changes class,
// adds the new class's stock weapons on top of the retained loadout plus its
// DefaultItems, reconciles, clamps every owned count, rebuilds the banks once
// more, then repairs a disabled player hull one armor point at a time until it
// is no longer disabled, and reinstalls the gameplay interface layout.
[[nodiscard]] bool
ChangePlayerShip(GameState &state, std::int32_t resource_id, char opcode) {
  if (resource_id < kResourceIdBase || resource_id >= 0x380) {
    return false;
  }
  const auto class_id =
      static_cast<std::int16_t>(resource_id - kResourceIdBase);
  const ShipClass *ship =
      state.scenario.Ship(static_cast<std::int16_t>(resource_id));
  if (ship == nullptr) {
    return false;
  }

  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
  if (opcode == 'H') {
    for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size() &&
                            i < state.scenario.outfits.size();
         ++i) {
      if ((state.scenario.outfits[i].flags & 0x24U) == 0) {
        state.inventory.outfit_owned_count[i] = 0;
      }
    }
  }
  NovaWeapon_RebuildBanksFromOwnedOutfits(state);
  state.player.ship_class_id = class_id;

  // C preserves the installed loadout. E adds the new class's defaults on top.
  // H additionally dropped non-persistent outfits above.
  if (opcode == 'E' || opcode == 'H') {
    NovaWeapon_AddShipClassStockBanks(state, class_id);
    for (std::size_t i = 0; i < ship->default_outfit_ids.size(); ++i) {
      const std::int16_t outfit_id = ship->default_outfit_ids[i];
      const std::int16_t count = ship->default_outfit_counts[i];
      if (outfit_id < 0 || count <= 0) {
        continue;
      }
      const auto index = static_cast<std::size_t>(outfit_id);
      if (index < state.inventory.outfit_owned_count.size()) {
        state.inventory.outfit_owned_count[index] = static_cast<std::int16_t>(
            state.inventory.outfit_owned_count[index] + count);
      }
    }
    NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
    for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size() &&
                            i < state.scenario.outfits.size();
         ++i) {
      if (state.inventory.outfit_owned_count[i] <= 0) {
        continue;
      }
      const OutfitOwnership ownership =
          Outfit_ClampOwnedCountToLimits(state, static_cast<std::int16_t>(i));
      if (ownership.max_allowed > 0 &&
          ownership.max_allowed < state.inventory.outfit_owned_count[i]) {
        state.inventory.outfit_owned_count[i] = ownership.max_allowed;
      }
    }
  }
  NovaWeapon_RebuildBanksFromOwnedOutfits(state);

  // 0x00449932..0x00449978: repair armor +1.0 (DAT_00575530 = 1.0f) while the
  // hull still reads disabled, so a class swap out of a crippled hull does not
  // leave it stuck.
  while (NovaAiShip_IsDisabled(state, state.player) &&
         state.player.armor_points <
             NovaAi_ComputeMaxArmorPoints(state, state.player)) {
    state.player.armor_points += 1.0F;
  }
  // Ui_InstallGameplayInterfaceLayout (0x004cda50): the script engine has no
  // renderer, so raise the flag the spaceflight loop consumes.
  state.gameplay_interface_dirty = true;
  state.InvalidateDerivedStatCaches();
  return true;
}

// The non-transition tail shared by M and N (0x0044a3c0/0x0044a400); N runs it
// even when the system id was out of range.
void RefreshTransientStateAfterMove(GameState &state);

// Ghidra 0x00449370 Mission_ExecuteMisnScriptEngine, cases 'M' and 'N'
// (0x0044997a..0x00449c26). Both set the player's current system, clear
// ai_secondary_target_slot, and re-home attached ships (squad leader slot 0).
// M additionally positions at the destination's first nav stellar when flying,
// or stashes that stellar for the launch tail when docked. N never touches the
// position and latches g_skip_player_reposition_once. The non-transition tail
// (0x0044a3c0/0x0044a400) clears the transient combat latches, drops the
// primary target, refreshes the stellar display state and re-arms mission
// spawn state; the docked tail rewrites the mission locator lists (the clean
// room rebuilds those lists lazily, so there is nothing to clear).
[[nodiscard]] bool
MovePlayer(GameState &state, std::int32_t resource_id, char opcode) {
  if (resource_id < kResourceIdBase || resource_id >= kResourceIdBase + 0x800) {
    return false;
  }
  const auto system_id =
      static_cast<std::int16_t>(resource_id - kResourceIdBase);
  const System *system =
      state.scenario.System(static_cast<std::int16_t>(resource_id));
  if (system == nullptr) {
    return false;
  }
  state.player.current_system_id = system_id;
  state.player.ai_secondary_target_slot = -1;
  if (opcode == 'M') {
    bool placed = false;
    for (const std::int16_t nav : system->nav_defs) {
      if (nav < kResourceIdBase) {
        continue;
      }
      const Stellar *stellar = state.scenario.Stellar(nav);
      if (stellar == nullptr) {
        continue;
      }
      if (state.system_transition_active) {
        // Docked: queue the destination's first nav resource id for the launch
        // tail (0x004499e1..0x004499f5). The original stores its 0-based
        // g_stellar_defs index; the port keeps the 0x80-based resource id (see
        // ShipState::ai_secondary_target_slot).
        state.player.ai_secondary_target_slot = nav;
      } else {
        state.player.pos_x = static_cast<float>(stellar->pos_x);
        state.player.pos_y = static_cast<float>(stellar->pos_y);
        state.player.vel_x = 0.0F;
        state.player.vel_y = 0.0F;
      }
      placed = true;
      break;
    }
    if (!placed && kApplyOriginalBugFixes && !state.system_transition_active) {
      // BUGFIX(original): see the function comment -- no nav stellar left the
      // executable with no reposition, so patch that residual here.
      state.player.pos_x = 0.0F;
      state.player.pos_y = 0.0F;
      state.player.vel_x = 0.0F;
      state.player.vel_y = 0.0F;
    }
  } else {
    // 0x00449bda: docked N suppresses the launch tail's stellar snap. The
    // original sets it whenever the system id is valid, docked or not.
    state.skip_player_reposition_once = true;
  }
  // 0x00449aa5..0x00449ae3: attached ships (squad leader slot 0) re-home to
  // the player's new system.
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &attached = state.ShipAt(slot);
    if (attached.is_active && attached.squad_leader_ship_slot == 0) {
      attached.current_system_id = state.player.current_system_id;
    }
  }
  // 0x0044a3c0 (case 'M') / 0x0044a400 (case 'N'): the transition-frame
  // clear latches, manual target clear, display-state refresh and
  // mission-spawn re-arm only run while flying.
  RefreshTransientStateAfterMove(state);
  return true;
}

// The non-transition tail shared by M and N (0x0044a3c0/0x0044a400); N runs it
// even when the system id was out of range.
void RefreshTransientStateAfterMove(GameState &state) {
  if (state.system_transition_active) {
    return;
  }
  NovaWeapon_ClearTransientCombatState(state);
  state.player.primary_target_ship_slot = -1;
  NovaTargeting_UpdateStellarAvailability(state);
  Mission_RefreshActiveMissionSpawnState(state);
}

// Runs one armed command. Mirrors the original switch body exactly; the parser
// above decides when a token executes. Returns true when the command letter is
// a known opcode (even if its operand is out of range), false when the letter
// is unmodelled. `action_out` carries the trace description.
[[nodiscard]] bool
ExecuteMissionScriptCommand(GameState &state,
                            char command,
                            std::int16_t operand,
                            const MissionScriptContext &context,
                            const MissionAcceptanceSink &acceptance,
                            std::size_t command_offset,
                            std::string_view &action_out) {
  action_out = "unmodelled opcode";
  switch (command) {
  case ' ':
  case '!':
  case '^': {
    // The original writes g_nova_control_bits[operand] for any
    // (short)operand < 10000, including negative (out-of-bounds) values. The
    // clean room keeps the < 10000 upper bound and drops negatives.
    if (operand < 0 || static_cast<std::size_t>(operand) >=
                           PilotControlState::kControlBitCount) {
      return true;
    }
    const auto bit = static_cast<std::uint32_t>(operand);
    const bool old_value = state.control.ControlBit(bit);
    const bool value = command == '^'   ? !old_value
                       : command == '!' ? false
                                        : true;
    state.control.SetControlBit(bit, value);
    MissionTrace::LogScriptBit(context.source,
                               context.mission_slot,
                               command_offset,
                               command == ' ' ? 'b' : command,
                               bit,
                               old_value,
                               value);
    action_out = command == ' '   ? "set control bit"
                 : command == '!' ? "clear control bit"
                                  : "toggle control bit";
    return true;
  }
  case 'A': {
    action_out = "reset active mission";
    if (operand >= kResourceIdBase && operand < 0x468) {
      const auto mission_id =
          static_cast<std::int16_t>(operand - kResourceIdBase);
      for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions; ++slot) {
        if (!state.active_mission_runtime_flags[slot].is_active ||
            state.active_missions[slot].mission_template_id != mission_id) {
          continue;
        }
        // The original runs the OnAbort payload and releases the mission fleet
        // (0x004497b4 calls Mission_ClearMisnSlotAssignments(slot, 1)).
        Mission_ClearMisnSlotAssignments(
            state, static_cast<std::int16_t>(slot), true, 0, acceptance);
      }
    }
    return true;
  }
  case 'F': {
    action_out = "fail active mission";
    if (operand >= kResourceIdBase && operand < 0x468) {
      const auto mission_id =
          static_cast<std::int16_t>(operand - kResourceIdBase);
      for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions; ++slot) {
        if (state.active_mission_runtime_flags[slot].is_active &&
            state.active_missions[slot].mission_template_id == mission_id) {
          state.active_mission_runtime_flags[slot].is_failed = true;
        }
      }
    }
    return true;
  }
  case 'D':
    action_out = "remove outfit";
    if (operand >= kResourceIdBase && operand < 0x280) {
      const auto index = static_cast<std::size_t>(operand - kResourceIdBase);
      NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
      state.inventory.outfit_owned_count[index] = std::max<std::int16_t>(
          0,
          static_cast<std::int16_t>(state.inventory.outfit_owned_count[index] -
                                    1));
      NovaWeapon_RebuildBanksFromOwnedOutfits(state);
    }
    return true;
  case 'G':
    action_out = "grant outfit";
    if (operand >= kResourceIdBase && operand < 0x280) {
      NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
      // Ghidra 0x00427770 Outfit_GrantOutfitToPlayer: mission outfit grants
      // run the same on-acquire effects (map reveal / paint / clean-record) as
      // a shop take.
      (void)NovaOutfit_GrantOutfitToPlayer(
          state, static_cast<std::int16_t>(operand - kResourceIdBase));
      NovaWeapon_RebuildBanksFromOwnedOutfits(state);
    }
    return true;
  case 'S':
    action_out = "activate mission";
    if (operand >= kResourceIdBase && operand < 0x468) {
      const auto mission_id =
          static_cast<std::int16_t>(operand - kResourceIdBase);
      // 0x00449c3c: the activation is wrapped in
      // g_travel_scene_ctx = (g_is_system_transition_active == 0), restored
      // afterwards, and resolves the mission's locator targets first.
      const bool saved_travel_scene = state.in_travel_scene;
      state.in_travel_scene = !state.system_transition_active;
      Mission_ResolveMissionStellarTargets(state, mission_id);
      (void)Mission_ActivateAtSlot(state, mission_id, acceptance);
      state.in_travel_scene = saved_travel_scene;
    }
    return true;
  case 'C':
  case 'E':
  case 'H':
    action_out = command == 'C'   ? "switch ship class (keep loadout)"
                 : command == 'E' ? "switch ship class (add default outfits)"
                                  : "switch ship class (defaults, drop "
                                    "non-persistent)";
    (void)ChangePlayerShip(state, operand, command);
    return true;
  case 'K':
  case 'L':
    action_out = command == 'K' ? "activate rank" : "deactivate rank";
    if (operand >= kResourceIdBase && operand < kResourceIdBase + 0x80) {
      const auto rank_slot =
          static_cast<std::int16_t>(operand - kResourceIdBase);
      if (command == 'K') {
        Rank_Activate(state, rank_slot);
      } else {
        Rank_Deactivate(state, rank_slot);
      }
    }
    return true;
  case 'M':
    action_out = "move player to system, center on first nav";
    (void)MovePlayer(state, operand, 'M');
    return true;
  case 'N':
    action_out = "move player to system center";
    // The original's non-transition tail runs even when the system id is
    // invalid (the update block sits outside the range guard).
    if (operand < kResourceIdBase || operand >= kResourceIdBase + 0x800) {
      RefreshTransientStateAfterMove(state);
      return true;
    }
    (void)MovePlayer(state, operand, 'N');
    return true;
  case 'P':
    action_out = "queue transient sound";
    // Ghidra 0x00449370: g_pending_transient_sound_id = (short)operand (a
    // single slot, overwritten by any later P in the same script).
    state.pending_transient_sound_id = operand;
    return true;
  case 'Q': {
    action_out = "stage pending overlay message";
    // Ghidra 0x00449370 'Q': Resource_LoadRandomStringEntry fills the
    // 256-byte g_pending_overlay_message buffer (0x007354d0); with a live
    // payload context slot (0..15) the original then expands mission text
    // tags through Stellar_BuildTravelDestinationDescription. The message is
    // NOT shown here -- the launch tail (Stellar_RunDockAndLaunchSequence
    // 0x00456134) shows and clears it. A live docked BBS sees the staged
    // buffer on its next poll (NovaUi_PollMissionBbsWindow 0x00440c90) and
    // force-leaves with action 7.
    if (auto message = LoadRandomStringListEntry(state.rng, operand)) {
      const std::int16_t context_slot = state.script_mission_context_slot;
      if (context_slot >= 0 && context_slot < 0x10) {
        *message = Mission_ExpandMissionWildcards(
            state, *message, false, context_slot);
      }
      state.pending_overlay_message = std::move(*message);
    }
    // Missing STR# data is an asset-loading issue, not a script syntax
    // failure; the original still stages an empty buffer.
    return true;
  }
  case 'T': {
    action_out = "rename ship from string list";
    const auto old_name = state.player.ship_name;
    // The original only rewrites the name when the loaded pstring is
    // non-empty (0x00449e6c tests the length byte), so an empty STR entry
    // leaves the current name alone rather than blanking it.
    if (auto title = LoadRandomStringListEntry(state.rng, operand);
        title && !title->empty()) {
      ReplaceShipTitleMarkers(*title, old_name);
      state.player.ship_name = std::move(*title);
    }
    return true;
  }
  case 'U':
    action_out = "restore stellar";
    if (operand >= kResourceIdBase && operand < kResourceIdBase + 0x800) {
      const auto stellar_index =
          static_cast<std::size_t>(operand - kResourceIdBase);
      if (stellar_index < state.scenario.stellars.size()) {
        Stellar &stellar = state.scenario.stellars[stellar_index];
        stellar.destroyed_days_remaining = -1;
        stellar.strength = stellar.strength_capacity;
      }
    }
    return true;
  case 'Y':
    action_out = "destroy stellar";
    if (operand >= kResourceIdBase && operand < kResourceIdBase + 0x800) {
      const auto stellar_index =
          static_cast<std::size_t>(operand - kResourceIdBase);
      if (stellar_index < state.scenario.stellars.size()) {
        Stellar &stellar = state.scenario.stellars[stellar_index];
        // 0x0044a228: blow up the body in place only when the player is
        // watching, the body is still active and it has an explosion type.
        if (state.player.current_system_id == stellar.system_id &&
            !NovaTargeting_IsStellarActive(stellar) &&
            stellar.explosion_type != -1) {
          NovaEffects_SpawnAreaImpact(state,
                                      static_cast<float>(stellar.pos_x),
                                      static_cast<float>(stellar.pos_y),
                                      stellar.explosion_type,
                                      0,
                                      true);
        }
        // A negative schedule seed pins the regeneration at one day.
        stellar.destroyed_days_remaining =
            stellar.schedule_days < 0 ? 1 : stellar.schedule_days;
        stellar.strength = -1;
      }
    }
    return true;
  case 'X':
    action_out = "reveal system";
    if (operand >= kResourceIdBase && operand < kResourceIdBase + 0x800) {
      // Ghidra 0x00449370 'X': the original only writes discovery_state
      // (min 1); the galaxy-map reveal is that same write.
      NovaSystem_MarkSystemVisited(
          state, static_cast<std::int16_t>(operand - kResourceIdBase), 1);
    }
    return true;
  default:
    return false;
  }
}

} // namespace

// Ghidra 0x00449370 Mission_ExecuteMisnScriptEngine.
//
// The original walks the script one byte at a time with a small state machine:
// command (local_119), number accumulator (local_14), executed-token counter
// (local_124), the R branch choice (local_128, -1 = none) and a per-iteration
// "execute now" flag (local_118 low byte). An opcode letter arms `command` and
// zeroes the accumulator; a 'b' (only when no opcode is armed) selects the
// control-bit set form ' '; an 'R' resets the token counter and draws a 0/1
// branch. Digits accumulate into the number. Any other byte is a delimiter:
// it executes the pending command, then resets. The R pair is skipped by a
// two-byte skip at the first delimiter when branch 0 wins (so the first
// alternative's leading byte is swallowed) and by comparing the executed-token
// counter to the chosen branch. A trailing token executes at the NUL
// terminator, so the loop runs one byte past the end of the string.
void Mission_ExecuteScript(GameState &state,
                           std::string_view script,
                           const MissionScriptContext &context,
                           const MissionAcceptanceSink &acceptance) {
  MissionTrace::LogScript(context.source, context.mission_slot, script);

  const std::int32_t length = static_cast<std::int32_t>(script.size());
  std::int32_t cursor = 0;
  std::int32_t number = 0;
  char command = '?';
  int token_index = 0;
  int random_branch = -1;
  std::int32_t command_offset = 0;

  while (true) {
    const char raw =
        cursor < length ? script[static_cast<std::size_t>(cursor)] : '\0';
    bool execute = false;
    const char translated = TranslateScriptChar(raw);
    switch (translated) {
    case '!':
    case 'A':
    case 'C':
    case 'D':
    case 'E':
    case 'F':
    case 'G':
    case 'H':
    case 'K':
    case 'L':
    case 'M':
    case 'N':
    case 'P':
    case 'Q':
    case 'S':
    case 'T':
    case 'U':
    case 'X':
    case 'Y':
    case '^':
      number = 0;
      command = translated;
      command_offset = cursor;
      break;
    case 'B':
      number = 0;
      command_offset = cursor;
      if (command == '?') {
        command = ' ';
      }
      break;
    case 'R':
      token_index = 0;
      command = '?';
      command_offset = cursor;
      random_branch = RandomBelow(state, 2);
      break;
    default:
      if (raw < '0' || raw > '9') {
        if (random_branch == -1 || token_index != random_branch) {
          execute = true;
        } else {
          if (token_index == 0) {
            ++cursor;
          } else {
            ++token_index;
          }
          random_branch = -1;
          command = '?';
        }
      } else {
        number = number * 10 + (raw - '0');
      }
      break;
    }

    if (execute) {
      std::string_view action;
      const bool recognized =
          ExecuteMissionScriptCommand(state,
                                      command,
                                      static_cast<std::int16_t>(number),
                                      context,
                                      acceptance,
                                      static_cast<std::size_t>(command_offset),
                                      action);
      if (command != '?') {
        if (recognized && command != ' ' && command != '!' && command != '^') {
          MissionTrace::LogCommand(context.source,
                                   context.mission_slot,
                                   static_cast<std::size_t>(command_offset),
                                   command,
                                   static_cast<std::int16_t>(number),
                                   action);
        } else if (!recognized && MissionTrace::Enabled()) {
          NovaLog::Todo("mission script {} +{}: opcode {} (operand {}) is not "
                        "modelled",
                        context.source,
                        command_offset,
                        command,
                        static_cast<std::int16_t>(number));
        }
        command = '?';
        ++token_index;
      }
    }

    ++cursor;
    if (cursor > length) {
      return;
    }
  }
}

// Ghidra 0x00448020 Mission_ExecuteReactionScript.
//
// BUGFIX(original), currently ungated: the original copies the script into the
// one global g_reaction_script_buffer (DAT_007c8a10) and the engine re-reads
// that buffer every byte. The S opcode activates a mission, which runs its
// OnAccept payload through Mission_RunMisnScriptPayload into that same buffer,
// so a non-empty payload replaces the remainder of the outer script mid-scan.
// That is the recorded engine bug "a mission that aborts itself in OnAccept and
// starts another mission displays the second briefing twice"
// (docs/known_original_bugs.md); the reused buffer lets the outer scan re-run
// bytes of the nested payload. The clean room threads an explicit
// std::string_view, so an outer script that starts a mission with an OnAccept
// payload keeps running its remaining opcodes and the second briefing is shown
// once.
//
// TODO(decomp): neither the shared mutable buffer nor the engine's per-byte
// length re-read is reproduced, and there is deliberately no
// kApplyOriginalBugFixes gate for the broken path yet; reproducing it would
// need both.
void Mission_ExecuteReactionScript(GameState &state,
                                   std::string_view script,
                                   const MissionScriptContext &context,
                                   const MissionAcceptanceSink &acceptance) {
  // The original returns before touching any state for an empty script.
  if (script.empty()) {
    return;
  }
  Mission_ExecuteScript(state, script, context, acceptance);
  // The reaction entrypoint also recomputes derived outfit state after the
  // engine (Outfit_RecomputeOutfitDerivedState 0x0046d4b0).
  NovaOutfit_RecomputeOutfitDerivedState(state);
}

// Ghidra 0x00448050 Mission_RunMisnScriptPayload.
void Mission_RunMisnScriptPayload(GameState &state,
                                  std::string_view script,
                                  std::int16_t mission_slot,
                                  const MissionScriptContext &context,
                                  const MissionAcceptanceSink &acceptance) {
  // The original returns before touching any state for an empty payload.
  if (script.empty()) {
    return;
  }
  // g_script_mission_context_slot is a real global in the original, so a
  // nested payload (the engine's S opcode -> Mission_ActivateMissionAtSlot)
  // clobbers it and clears it again on return. Model it as state rather than a
  // parameter so an outer Q after an S sees no context, exactly like the
  // original. The original does no range validation on the slot; the engine's
  // Q case only consults slots 0..15.
  MissionScriptContext effective = context;
  effective.mission_slot = mission_slot;
  state.script_mission_context_slot = mission_slot;
  Mission_ExecuteScript(state, script, effective, acceptance);
  state.script_mission_context_slot = -1;
  // Outfit_RecomputeOutfitDerivedState (0x0046d4b0) after the engine.
  NovaOutfit_RecomputeOutfitDerivedState(state);
}

// Ghidra 0x00449370 Mission_ExecuteMisnScriptEngine.
void Mission_ExecuteMisnScriptEngine(GameState &state,
                                     std::string_view script,
                                     const MissionScriptContext &context,
                                     const MissionAcceptanceSink &acceptance) {
  Mission_ExecuteScript(state, script, context, acceptance);
}

} // namespace game
