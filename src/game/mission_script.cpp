#include "mission_script.hpp"

#include "brgr_archive.hpp"
#include "hud_overlay.hpp"
#include "mission.hpp"
#include "outfit.hpp"
#include "rank.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace game {
namespace {

constexpr std::int16_t kResourceIdBase = 0x80;
constexpr std::uint32_t kStringTableType = 0x53545223U;

void AddDiagnostic(MissionScriptResult &result,
                   std::size_t offset,
                   std::string message) {
  result.diagnostics.push_back({offset, std::move(message)});
}

[[nodiscard]] bool IsDigit(char value) {
  return std::isdigit(static_cast<unsigned char>(value)) != 0;
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

[[nodiscard]] bool
ChangePlayerShip(GameState &state, std::int32_t resource_id, char opcode) {
  if (resource_id < kResourceIdBase || resource_id >= 0x380) {
    return false;
  }
  const auto class_id =
      static_cast<std::int16_t>(resource_id - kResourceIdBase);
  const auto *ship =
      state.scenario.Ship(static_cast<std::int16_t>(resource_id));
  if (ship == nullptr) {
    return false;
  }
  state.player.ship_class_id = class_id;

  // C preserves the installed loadout. E adds the new class's defaults. H
  // additionally drops outfits without the Bible's Persistent flag.
  if (opcode == 'H') {
    for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size() &&
                            i < state.scenario.outfits.size();
         ++i) {
      if (!state.scenario.outfits[i].persistent_on_ship_swap) {
        state.inventory.outfit_owned_count[i] = 0;
      }
    }
  }
  if (opcode == 'E' || opcode == 'H') {
    for (std::size_t i = 0; i < ship->default_outfit_ids.size(); ++i) {
      if (ship->default_outfit_ids[i] >= 0 &&
          ship->default_outfit_counts[i] > 0) {
        (void)Outfit_AddInstalledOutfit(
            state, ship->default_outfit_ids[i], ship->default_outfit_counts[i]);
      }
    }
    NovaWeapon_SeedBanksFromShipStock(state, class_id);
    NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
  }
  state.stat_cache_valid = false;
  return true;
}

[[nodiscard]] bool
MovePlayer(GameState &state, std::int32_t resource_id, char opcode) {
  if (resource_id < kResourceIdBase || resource_id >= kResourceIdBase + 0x800) {
    return false;
  }
  const auto system_id =
      static_cast<std::int16_t>(resource_id - kResourceIdBase);
  const auto *system =
      state.scenario.System(static_cast<std::int16_t>(resource_id));
  if (system == nullptr) {
    return false;
  }
  state.player.current_system_id = system_id;
  if (opcode == 'M') {
    bool placed = false;
    for (const auto stellar_resource_id : system->nav_defs) {
      if (stellar_resource_id < kResourceIdBase) {
        continue;
      }
      if (const auto *stellar = state.scenario.Stellar(stellar_resource_id);
          stellar != nullptr) {
        state.player.pos_x = static_cast<float>(stellar->pos_x);
        state.player.pos_y = static_cast<float>(stellar->pos_y);
        placed = true;
        break;
      }
    }
    if (!placed) {
      state.player.pos_x = static_cast<float>(system->pos_x);
      state.player.pos_y = static_cast<float>(system->pos_y);
    }
  } else {
    state.player.pos_x = static_cast<float>(system->pos_x);
    state.player.pos_y = static_cast<float>(system->pos_y);
  }
  state.player.vel_x = 0.0F;
  state.player.vel_y = 0.0F;
  return true;
}

} // namespace

MissionScriptResult
Mission_ExecuteScript(GameState &state,
                      std::string_view script,
                      const MissionAcceptanceSink &acceptance) {
  MissionScriptResult result;

  std::function<void(std::size_t, std::size_t)> execute_range;
  execute_range = [&](std::size_t begin, std::size_t end) {
    std::size_t cursor = begin;
    while (cursor < end) {
      while (cursor < end &&
             (std::isspace(static_cast<unsigned char>(script[cursor])) != 0 ||
              script[cursor] == ',' || script[cursor] == ';')) {
        ++cursor;
      }
      if (cursor >= end) {
        return;
      }
      const auto command_offset = cursor;
      char modifier = '\0';
      if (script[cursor] == '!' || script[cursor] == '^') {
        modifier = script[cursor++];
      }
      if (cursor < end && (script[cursor] == 'b' || script[cursor] == 'B')) {
        ++cursor;
        const auto number_begin = cursor;
        while (cursor < end && IsDigit(script[cursor])) {
          ++cursor;
        }
        if (number_begin == cursor) {
          AddDiagnostic(
              result, command_offset, "control bit is missing its number");
          continue;
        }
        std::uint32_t bit = 0;
        const auto parsed = std::from_chars(
            script.data() + number_begin, script.data() + cursor, bit);
        if (parsed.ec != std::errc{} ||
            bit >= PilotControlState::kControlBitCount) {
          AddDiagnostic(result, command_offset, "control bit is out of range");
          continue;
        }
        const bool value =
            modifier == '^' ? !state.control.ControlBit(bit) : modifier != '!';
        state.control.SetControlBit(bit, value);
        ++result.commands_executed;
        continue;
      }
      if (modifier != '\0') {
        AddDiagnostic(result,
                      command_offset,
                      "! and ^ modifiers require a b-prefixed control bit");
        continue;
      }
      if (cursor < end && (script[cursor] == 'r' || script[cursor] == 'R')) {
        ++cursor;
        while (cursor < end &&
               std::isspace(static_cast<unsigned char>(script[cursor])) != 0) {
          ++cursor;
        }
        if (cursor >= end || script[cursor] != '(') {
          AddDiagnostic(
              result, command_offset, "R requires parenthesized alternatives");
          continue;
        }
        const auto content_begin = ++cursor;
        int depth = 1;
        while (cursor < end && depth != 0) {
          if (script[cursor] == '(')
            ++depth;
          if (script[cursor] == ')')
            --depth;
          ++cursor;
        }
        if (depth != 0) {
          AddDiagnostic(result, command_offset, "unterminated R expression");
          return;
        }
        const auto content_end = cursor - 1;
        std::size_t split = content_begin;
        while (split < content_end &&
               std::isspace(static_cast<unsigned char>(script[split])) == 0) {
          ++split;
        }
        if (split == content_end) {
          AddDiagnostic(result, command_offset, "R requires two alternatives");
          continue;
        }
        while (split < content_end &&
               std::isspace(static_cast<unsigned char>(script[split])) != 0) {
          ++split;
        }
        std::uniform_int_distribution<int> pick(0, 1);
        if (pick(state.rng) == 0) {
          execute_range(content_begin, split);
        } else {
          execute_range(split, content_end);
        }
        continue;
      }

      if (cursor >= end ||
          std::isalpha(static_cast<unsigned char>(script[cursor])) == 0) {
        AddDiagnostic(result, command_offset, "expected mission script opcode");
        ++cursor;
        continue;
      }
      const char opcode = static_cast<char>(
          std::toupper(static_cast<unsigned char>(script[cursor++])));
      const auto number_begin = cursor;
      while (cursor < end && IsDigit(script[cursor])) {
        ++cursor;
      }
      if (number_begin == cursor) {
        AddDiagnostic(
            result, command_offset, "mission opcode is missing its number");
        continue;
      }
      std::int32_t operand = 0;
      const auto parsed = std::from_chars(
          script.data() + number_begin, script.data() + cursor, operand);
      if (parsed.ec != std::errc{}) {
        AddDiagnostic(result, command_offset, "invalid mission script number");
        continue;
      }

      bool applied = false;
      bool known_opcode = true;
      switch (opcode) {
      case 'A':
      case 'F': {
        if (operand >= kResourceIdBase && operand < 0x468) {
          const auto mission_id =
              static_cast<std::int16_t>(operand - kResourceIdBase);
          for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions;
               ++slot) {
            auto &active = state.active_missions[slot];
            auto &runtime = state.active_mission_runtime_flags[slot];
            if (!runtime.is_active || active.mission_template_id != mission_id)
              continue;
            if (opcode == 'F')
              runtime.is_failed = true;
            else {
              active = {};
              runtime = {};
            }
            applied = true;
          }
        }
        // The original treats A/F on an inactive mission as a no-op.
        applied = operand >= kResourceIdBase && operand < 0x468;
        break;
      }
      case 'D':
        if (operand >= kResourceIdBase && operand < 0x280) {
          (void)Outfit_RemoveOutfit(
              state, static_cast<std::int16_t>(operand - kResourceIdBase), 1);
          applied = true;
        }
        break;
      case 'G':
        if (operand >= kResourceIdBase && operand < 0x280) {
          // Ghidra 0x00427770 Outfit_GrantOutfitToPlayer: mission outfit
          // grants run the same on-acquire effects (map reveal / paint /
          // clean-record) as a shop take.
          (void)NovaOutfit_GrantOutfitToPlayer(
              state, static_cast<std::int16_t>(operand - kResourceIdBase));
          applied = true;
        }
        break;
      case 'S':
        if (operand >= kResourceIdBase && operand < 0x468) {
          // The original's activation reads ai_secondary_target_slot (the
          // current travel/landed stellar) for its briefing check, and shows
          // the Brief/LoadCarg acceptance dialogs inline; forward the sink so
          // a script-started mission still presents them.
          // TODO(decomp(0x00449370)) skipped: the original wraps the call in
          // g_travel_scene_ctx = (g_is_system_transition_active == 0), then
          // restores the previous value -- i.e. state.in_travel_scene =
          // !state.travel.engaging around the activation. The port leaves
          // in_travel_scene as the landing pass set it, so any travel-scene-
          // gated branch inside Mission_ActivateMissionAtSlot (destination
          // window / overlay) is not reproduced on the script-S path.
          (void)Mission_ActivateAtSlot(
              state,
              static_cast<std::int16_t>(operand - kResourceIdBase),
              state.player.ai_secondary_target_slot,
              acceptance);
          applied = true;
        }
        break;
      case 'C':
      case 'E':
      case 'H':
        applied = ChangePlayerShip(state, operand, opcode);
        break;
      case 'K':
      case 'L':
        if (operand >= kResourceIdBase && operand < kResourceIdBase + 0x80) {
          const auto rank_slot =
              static_cast<std::int16_t>(operand - kResourceIdBase);
          if (opcode == 'K') {
            Rank_Activate(state, rank_slot);
          } else {
            Rank_Deactivate(state, rank_slot);
          }
          applied = true;
        }
        break;
      case 'M':
      case 'N':
        applied = MovePlayer(state, operand, opcode);
        break;
      case 'P':
        if (operand >= 0 && operand <= 0xffff) {
          // Ghidra 0x00449370: g_pending_transient_sound_id = operand (a
          // single slot, overwritten by any later P in the same script).
          state.pending_transient_sound_id = static_cast<std::int16_t>(operand);
          applied = true;
        }
        break;
      case 'Q': {
        state.script_forced_leave_landing = true;
        state.pending_script_message_string_id =
            operand >= 0 && operand <= 0x7fff
                ? static_cast<std::int16_t>(operand)
                : -1;
        if (state.pending_script_message_string_id >= 0) {
          if (auto message = LoadRandomStringListEntry(
                  state.rng, state.pending_script_message_string_id)) {
            // Ghidra 0x00449370 'Q': with a live payload context slot (0..15)
            // the original runs Stellar_BuildTravelDestinationDescription(
            // '\0', g_script_mission_context_slot) over the loaded message,
            // expanding its mission text tags.
            const std::int16_t context_slot = state.script_mission_context_slot;
            if (context_slot >= 0 && context_slot < 0x10) {
              *message = Mission_ExpandMissionWildcards(
                  state, *message, false, context_slot);
            }
            NovaHud_ShowOverlayMessage(state, std::move(*message));
          }
          // Missing STR# data is an asset-loading issue, not a script syntax
          // failure; the original still leaves the stellar.
          applied = true;
        }
        break;
      }
      case 'T': {
        const auto old_name = state.player.ship_name;
        if (auto title = LoadRandomStringListEntry(state.rng, operand)) {
          ReplaceShipTitleMarkers(*title, old_name);
          state.player.ship_name = std::move(*title);
          applied = true;
        }
        break;
      }
      case 'U':
      case 'Y':
        if (operand >= kResourceIdBase && operand < kResourceIdBase + 0x800) {
          const auto stellar_index =
              static_cast<std::size_t>(operand - kResourceIdBase);
          if (stellar_index < state.scenario.stellars.size()) {
            auto &stellar = state.scenario.stellars[stellar_index];
            stellar.is_destroyed = opcode == 'Y';
            applied = true;
          }
        }
        break;
      case 'X':
        if (operand >= kResourceIdBase && operand < kResourceIdBase + 0x800) {
          state.control.explored_systems.set(
              static_cast<std::size_t>(operand - kResourceIdBase));
          // Reveal on the galaxy map too: the starmap draws from the per-
          // system discovery state, so a script-revealed system needs its
          // discovery_state bumped (level 1 = visited). TODO(decomp): the
          // original opcode's exact discovery write is not yet traced.
          NovaSystem_MarkSystemVisited(
              state, static_cast<std::int16_t>(operand - kResourceIdBase), 1);
          applied = true;
        }
        break;
      default:
        known_opcode = false;
        AddDiagnostic(result,
                      command_offset,
                      "script opcode is not modelled: " +
                          std::string(1, opcode));
        break;
      }
      if (applied) {
        ++result.commands_executed;
      } else if (known_opcode) {
        AddDiagnostic(
            result, command_offset, "script operand is invalid for opcode");
      }
    }
  };

  execute_range(0, script.size());
  return result;
}

// Ghidra 0x00448020 Mission_ExecuteReactionScript.
MissionScriptResult
Mission_ExecuteReactionScript(GameState &state,
                              std::string_view script,
                              const MissionAcceptanceSink &acceptance) {
  // The original returns before touching any state for an empty script.
  if (script.empty()) {
    return {};
  }
  auto result = Mission_ExecuteScript(state, script, acceptance);
  // The reaction entrypoint also recomputes derived outfit state after the
  // engine (Outfit_RecomputeOutfitDerivedState 0x0046d4b0).
  NovaOutfit_RecomputeOutfitDerivedState(state);
  return result;
}

// Ghidra 0x00448050 Mission_RunMisnScriptPayload.
MissionScriptResult
Mission_RunMisnScriptPayload(GameState &state,
                             std::string_view script,
                             std::int16_t mission_slot,
                             const MissionAcceptanceSink &acceptance) {
  // The original returns before touching any state for an empty payload.
  if (script.empty()) {
    return {};
  }
  // g_script_mission_context_slot is a real global in the original, so a
  // nested payload (the engine's S opcode -> Mission_ActivateAtSlot) clobbers
  // it and clears it again on return. Model it as state rather than a
  // parameter so an outer Q after an S sees no context, exactly like the
  // original. The original does no range validation on the slot; the engine's
  // Q case only consults slots 0..15.
  state.script_mission_context_slot = mission_slot;
  auto result = Mission_ExecuteScript(state, script, acceptance);
  state.script_mission_context_slot = -1;
  // Outfit_RecomputeOutfitDerivedState (0x0046d4b0) after the engine.
  NovaOutfit_RecomputeOutfitDerivedState(state);
  return result;
}

// Ghidra 0x00449370 Mission_ExecuteMisnScriptEngine.
MissionScriptResult
Mission_ExecuteMisnScriptEngine(GameState &state,
                                std::string_view script,
                                const MissionAcceptanceSink &acceptance) {
  return Mission_ExecuteScript(state, script, acceptance);
}

} // namespace game
