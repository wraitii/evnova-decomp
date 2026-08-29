#include "mission_script.hpp"

#include "brgr_archive.hpp"
#include "hud_overlay.hpp"
#include "mission.hpp"
#include "outfit.hpp"
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
  std::uniform_int_distribution<std::uint16_t> pick(0, count - 1);
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

MissionScriptResult Mission_ExecuteScript(GameState &state,
                                          std::string_view script) {
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
          (void)Outfit_AddInstalledOutfit(
              state, static_cast<std::int16_t>(operand - kResourceIdBase), 1);
          applied = true;
        }
        break;
      case 'S':
        if (operand >= kResourceIdBase && operand < 0x468) {
          // The original's activation reads ai_secondary_target_slot (the
          // current travel/landed stellar) for its briefing check.
          (void)Mission_ActivateAtSlot(
              state,
              static_cast<std::int16_t>(operand - kResourceIdBase),
              state.player.ai_secondary_target_slot);
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
          state.control.active_ranks.set(
              static_cast<std::size_t>(operand - kResourceIdBase),
              opcode == 'K');
          applied = true;
        }
        break;
      case 'M':
      case 'N':
        applied = MovePlayer(state, operand, opcode);
        break;
      case 'P':
        if (operand >= 0 && operand <= 0xffff) {
          state.pending_script_sounds.push_back(
              static_cast<std::int16_t>(operand));
          applied = true;
        }
        break;
      case 'Q': {
        state.script_forced_leave_landing = true;
        state.pending_script_message_string_list =
            operand >= 0 && operand <= 0x7fff
                ? static_cast<std::int16_t>(operand)
                : -1;
        if (state.pending_script_message_string_list >= 0) {
          if (auto message = LoadRandomStringListEntry(
                  state.rng, state.pending_script_message_string_list)) {
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
MissionScriptResult Mission_ExecuteReactionScript(GameState &state,
                                                  std::string_view script) {
  return Mission_ExecuteScript(state, script);
}

// Ghidra 0x00448050 Mission_RunMisnScriptPayload.
MissionScriptResult Mission_RunMisnScriptPayload(GameState &state,
                                                 std::string_view script,
                                                 std::size_t mission_slot) {
  if (mission_slot >= GameState::kMaxActiveMissions) {
    MissionScriptResult result;
    result.diagnostics.push_back(
        {0, "mission payload context slot is out of range"});
    return result;
  }
  return Mission_ExecuteScript(state, script);
}

// Ghidra 0x00449370 Mission_ExecuteMisnScriptEngine.
MissionScriptResult Mission_ExecuteMisnScriptEngine(GameState &state,
                                                    std::string_view script) {
  return Mission_ExecuteScript(state, script);
}

} // namespace game
