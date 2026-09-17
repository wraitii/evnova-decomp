#include "mission_script.hpp"

#include "brgr_archive.hpp"
#include "hud_overlay.hpp"
#include "log.hpp"
#include "mission.hpp"
#include "mission_trace.hpp"
#include "outfit.hpp"
#include "rank.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace game {
namespace {

constexpr std::int16_t kResourceIdBase = 0x80;
constexpr std::uint32_t kStringTableType = 0x53545223U;

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

void Mission_ExecuteScript(GameState &state,
                           std::string_view script,
                           const MissionScriptContext &context,
                           const MissionAcceptanceSink &acceptance) {
  MissionTrace::LogScript(context.source, context.mission_slot, script);

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
          NovaLog::Warn("mission script {} +{}: control bit is missing its "
                        "number",
                        context.source,
                        command_offset);
          continue;
        }
        std::uint32_t bit = 0;
        const auto parsed = std::from_chars(
            script.data() + number_begin, script.data() + cursor, bit);
        if (parsed.ec != std::errc{} ||
            bit >= PilotControlState::kControlBitCount) {
          NovaLog::Warn("mission script {} +{}: control bit is out of range",
                        context.source,
                        command_offset);
          continue;
        }
        const bool old_value = state.control.ControlBit(bit);
        const bool value = modifier == '^' ? !old_value : modifier != '!';
        state.control.SetControlBit(bit, value);
        MissionTrace::LogScriptBit(context.source,
                                   context.mission_slot,
                                   command_offset,
                                   modifier,
                                   bit,
                                   old_value,
                                   value);
        continue;
      }
      if (modifier != '\0') {
        NovaLog::Warn("mission script {} +{}: ! and ^ modifiers require a "
                      "b-prefixed control bit",
                      context.source,
                      command_offset);
        continue;
      }
      if (cursor < end && (script[cursor] == 'r' || script[cursor] == 'R')) {
        ++cursor;
        while (cursor < end &&
               std::isspace(static_cast<unsigned char>(script[cursor])) != 0) {
          ++cursor;
        }
        if (cursor >= end || script[cursor] != '(') {
          NovaLog::Warn("mission script {} +{}: R requires parenthesized "
                        "alternatives",
                        context.source,
                        command_offset);
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
          NovaLog::Warn("mission script {} +{}: unterminated R expression",
                        context.source,
                        command_offset);
          return;
        }
        const auto content_end = cursor - 1;
        std::size_t split = content_begin;
        while (split < content_end &&
               std::isspace(static_cast<unsigned char>(script[split])) == 0) {
          ++split;
        }
        if (split == content_end) {
          NovaLog::Warn("mission script {} +{}: R requires two alternatives",
                        context.source,
                        command_offset);
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
        NovaLog::Warn("mission script {} +{}: expected mission script opcode",
                      context.source,
                      command_offset);
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
        NovaLog::Warn(
            "mission script {} +{}: mission opcode is missing its number",
            context.source,
            command_offset);
        continue;
      }
      std::int32_t operand = 0;
      const auto parsed = std::from_chars(
          script.data() + number_begin, script.data() + cursor, operand);
      if (parsed.ec != std::errc{}) {
        NovaLog::Warn("mission script {} +{}: invalid mission script number",
                      context.source,
                      command_offset);
        continue;
      }

      bool applied = false;
      bool known_opcode = true;
      std::string_view action = "unmodelled opcode";
      switch (opcode) {
      case 'A':
      case 'F': {
        action = opcode == 'F' ? "fail active mission" : "reset active mission";
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
        action = "remove outfit";
        if (operand >= kResourceIdBase && operand < 0x280) {
          (void)Outfit_RemoveOutfit(
              state, static_cast<std::int16_t>(operand - kResourceIdBase), 1);
          applied = true;
        }
        break;
      case 'G':
        action = "grant outfit";
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
        action = "activate mission";
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
        action = "switch ship class (keep loadout)";
        applied = ChangePlayerShip(state, operand, opcode);
        break;
      case 'E':
        action = "switch ship class (add default outfits)";
        applied = ChangePlayerShip(state, operand, opcode);
        break;
      case 'H':
        action = "switch ship class (defaults, drop non-persistent)";
        applied = ChangePlayerShip(state, operand, opcode);
        break;
      case 'K':
      case 'L':
        action = opcode == 'K' ? "activate rank" : "deactivate rank";
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
        action = "move player to system, center on first nav";
        applied = MovePlayer(state, operand, opcode);
        break;
      case 'N':
        action = "move player to system center";
        applied = MovePlayer(state, operand, opcode);
        break;
      case 'P':
        action = "queue transient sound";
        if (operand >= 0 && operand <= 0xffff) {
          // Ghidra 0x00449370: g_pending_transient_sound_id = operand (a
          // single slot, overwritten by any later P in the same script).
          state.pending_transient_sound_id = static_cast<std::int16_t>(operand);
          applied = true;
        }
        break;
      case 'Q': {
        action = "show message and force leave landing";
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
        action = "rename ship from string list";
        const auto old_name = state.player.ship_name;
        if (auto title = LoadRandomStringListEntry(state.rng, operand)) {
          ReplaceShipTitleMarkers(*title, old_name);
          state.player.ship_name = std::move(*title);
          applied = true;
        }
        break;
      }
      case 'U':
        action = "restore stellar";
        if (operand >= kResourceIdBase && operand < kResourceIdBase + 0x800) {
          const auto stellar_index =
              static_cast<std::size_t>(operand - kResourceIdBase);
          if (stellar_index < state.scenario.stellars.size()) {
            auto &stellar = state.scenario.stellars[stellar_index];
            stellar.is_destroyed = false;
            applied = true;
          }
        }
        break;
      case 'Y':
        action = "destroy stellar";
        if (operand >= kResourceIdBase && operand < kResourceIdBase + 0x800) {
          const auto stellar_index =
              static_cast<std::size_t>(operand - kResourceIdBase);
          if (stellar_index < state.scenario.stellars.size()) {
            auto &stellar = state.scenario.stellars[stellar_index];
            stellar.is_destroyed = true;
            applied = true;
          }
        }
        break;
      case 'X':
        action = "reveal system";
        if (operand >= kResourceIdBase && operand < kResourceIdBase + 0x800) {
          // Ghidra 0x00449370 'X': the original only writes discovery_state
          // (min 1); the galaxy-map reveal is that same write.
          NovaSystem_MarkSystemVisited(
              state, static_cast<std::int16_t>(operand - kResourceIdBase), 1);
          applied = true;
        }
        break;
      default:
        known_opcode = false;
        break;
      }

      if (applied) {
        MissionTrace::LogCommand(context.source,
                                 context.mission_slot,
                                 command_offset,
                                 opcode,
                                 operand,
                                 action);
      } else if (known_opcode) {
        NovaLog::Warn("mission script {} +{}: operand {} invalid for opcode {}",
                      context.source,
                      command_offset,
                      operand,
                      opcode);
      } else if (MissionTrace::Enabled()) {
        // Unmodelled opcodes are expected while the grammar is reconstructed;
        // surface them only when tracing so ordinary play stays quiet.
        NovaLog::Todo("mission script {} +{}: opcode {} (operand {}) is not "
                      "modelled",
                      context.source,
                      command_offset,
                      opcode,
                      operand);
      }
    }
  };

  execute_range(0, script.size());
}

// Ghidra 0x00448020 Mission_ExecuteReactionScript.
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
  // nested payload (the engine's S opcode -> Mission_ActivateAtSlot) clobbers
  // it and clears it again on return. Model it as state rather than a
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
