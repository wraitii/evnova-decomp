#include "mission.hpp"

#include "game_state.hpp"
#include "government.hpp"
#include "log.hpp"
#include "mission_script.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"
#include "ship_spawn.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <string>

namespace game {
namespace {

constexpr std::int16_t kResourceIdBase = 0x80;

// Mirrors NovaRandom_Range (0x004683b0) -> integer in [0, n), drawn from
// GameState.rng so runs stay reproducible (see ship_spawn.cpp).
[[nodiscard]] std::int16_t RandomBelow(GameState &state, std::int32_t bound) {
  if (bound <= 0) {
    return 0;
  }
  std::uniform_int_distribution<std::int32_t> dist{0, bound - 1};
  return static_cast<std::int16_t>(dist(state.rng));
}

// Reads a NUL-terminated 255-byte text/script buffer (MisnActive text blocks
// copied from the mïsn payload) as a string_view.
[[nodiscard]] std::string_view
TextOf(const std::array<std::byte, 255> &buffer) {
  const auto *begin = reinterpret_cast<const char *>(buffer.data());
  std::size_t length = 0;
  while (length < buffer.size() && begin[length] != '\0') {
    ++length;
  }
  return {begin, length};
}

[[nodiscard]] ControlExpressionState
MissionControlExpressionState(const GameState &state) {
  ControlExpressionState expression;
  expression.get_control_bit = [&state](std::uint32_t bit) {
    return state.control.ControlBit(bit);
  };
  expression.is_registered = [&state](std::uint32_t) {
    return state.control.registered;
  };
  expression.is_male = [&state] { return state.control.male; };
  expression.owns_outfit = [&state](std::int16_t id) {
    return id >= 0 &&
           id < static_cast<std::int16_t>(
                    state.inventory.outfit_owned_count.size()) &&
           state.inventory.outfit_owned_count[static_cast<std::size_t>(id)] > 0;
  };
  expression.has_explored = [&state](std::int16_t id) {
    return id >= 0 && id < 0x800 &&
           state.control.explored_systems.test(static_cast<std::size_t>(id));
  };
  return expression;
}

[[nodiscard]] bool IsActiveMission(const GameState &state,
                                   std::int16_t mission_id) {
  return std::any_of(state.active_missions.begin(),
                     state.active_missions.end(),
                     [mission_id](const ActiveMission &mission) {
                       return mission.is_accepted &&
                              mission.mission_template_id == mission_id;
                     });
}

[[nodiscard]] bool
Mission_PassesAcceptanceResourceGates(const GameState &state,
                                      const MissionDef &definition) {
  if (definition.special_ship_count > 0 && state.player.ship_class_id >= 0) {
    const auto *ship_class = state.scenario.Ship(static_cast<std::int16_t>(
        state.player.ship_class_id + kResourceIdBase));
    if (ship_class != nullptr) {
      // Mission_ActivateMissionAtSlot checks both cargo and hull mass before
      // opening its original error dialog. Outfit purchase mass is already
      // normalized for hull-proportional outfits by ScenarioData.
      std::int32_t total_mass = ship_class->mass_tons;
      for (std::size_t outfit_id = 0;
           outfit_id < state.inventory.outfit_owned_count.size();
           ++outfit_id) {
        const auto owned = state.inventory.outfit_owned_count[outfit_id];
        if (owned <= 0 || outfit_id >= state.scenario.outfits.size()) {
          continue;
        }
        total_mass += static_cast<std::int32_t>(owned) *
                      state.scenario.outfits[outfit_id].PurchaseMass(
                          ship_class->mass_tons);
      }
      if (total_mass < definition.special_ship_count ||
          Outfit_ComputeRemainingCargoSpace(state) <
              definition.special_ship_count) {
        return false;
      }
    }
  }
  // Mission_ActivateMissionAtSlot charges the amount below -50000 as an
  // acceptance cost. The remaining positive/neutral values are rewards or
  // deferred accounting and do not block the BBS entry.
  if (definition.resource_delta_or_cost < -50000 &&
      state.player.credits < -50000 - definition.resource_delta_or_cost) {
    return false;
  }
  return true;
}

[[nodiscard]] std::int16_t ResolveContainingSystem(const GameState &state,
                                                   std::int16_t stellar_id) {
  if (stellar_id < 0 ||
      stellar_id >= static_cast<std::int16_t>(state.scenario.stellars.size())) {
    return -1;
  }
  return state.scenario.stellars[static_cast<std::size_t>(stellar_id)]
      .system_id;
}

[[nodiscard]] std::int16_t ResolveMissionStellar(GameState &state,
                                                 std::int16_t locator,
                                                 std::int16_t excluded,
                                                 std::int16_t fallback) {
  if (locator == -1 || locator == -4) {
    return fallback;
  }

  if (locator >= kResourceIdBase && locator < kResourceIdBase + 0x800) {
    const auto stellar_id =
        static_cast<std::int16_t>(locator - kResourceIdBase);
    if (stellar_id != excluded && stellar_id >= 0 &&
        stellar_id <
            static_cast<std::int16_t>(state.scenario.stellars.size())) {
      return stellar_id;
    }
    return fallback;
  }

  // Mission_SelectMissionStellarByLocator (0x0043d510) chooses randomly from
  // a filtered stellar set. The clean-room model has no separate travel graph
  // yet, so availability/system membership remain the verified gates. Keep
  // the selection random once the candidate set is known; target resolution
  // is an explicit runtime step and state.rng is the port's replacement for
  // NovaRandom_Range.
  const auto same_government_class = [&state](std::int16_t lhs,
                                              std::int16_t rhs,
                                              bool require_match) {
    if (lhs < 0 || rhs < 0 ||
        lhs >= static_cast<std::int16_t>(state.scenario.governments.size()) ||
        rhs >= static_cast<std::int16_t>(state.scenario.governments.size())) {
      return false;
    }
    const auto &left =
        state.scenario.governments[static_cast<std::size_t>(lhs)];
    const auto &right =
        state.scenario.governments[static_cast<std::size_t>(rhs)];
    for (const auto left_class : left.classes) {
      if (left_class < 0) {
        continue;
      }
      for (const auto right_class : right.classes) {
        if (left_class == right_class) {
          return require_match;
        }
      }
    }
    return !require_match;
  };
  const auto matches = [&](const Stellar &stellar) {
    if (!stellar.is_available || stellar.system_id < 0 ||
        stellar.system_id >=
            static_cast<std::int16_t>(state.scenario.systems.size()) ||
        (stellar.flags & 0x20U) != 0U) {
      return false;
    }
    const auto stellar_id =
        static_cast<std::int16_t>(&stellar - state.scenario.stellars.data());
    if (stellar_id == excluded) {
      return false;
    }
    const auto govt = stellar.government_id;
    if (locator == -2) {
      return (stellar.flags & 0x10U) == 0U;
    }
    if (locator == -3) {
      return (stellar.flags & 0x10U) != 0U;
    }
    if (locator >= 10000 && locator < 15000) {
      return govt == static_cast<std::int16_t>(locator - 10000);
    }
    if (locator >= 15000 && locator < 20000) {
      return NovaGovernment_AreGovtsAllied(
          state.scenario, govt, static_cast<std::int16_t>(locator - 15000));
    }
    if (locator >= 20000 && locator < 25000) {
      return govt != static_cast<std::int16_t>(locator - 20000);
    }
    if (locator >= 25000 && locator < 30000) {
      return NovaGovernment_AreGovtsHostileOrXenophobic(
          state.scenario, govt, static_cast<std::int16_t>(locator - 25000));
    }
    if (locator >= 30000 && locator < 31000) {
      const auto wanted = static_cast<std::int16_t>(locator - 30000);
      return same_government_class(govt, wanted, true);
    }
    if (locator >= 31000 && locator < 32000) {
      const auto wanted = static_cast<std::int16_t>(locator - 31000);
      return same_government_class(govt, wanted, false);
    }
    return false;
  };

  std::vector<std::int16_t> candidates;
  for (std::size_t i = 0; i < state.scenario.stellars.size(); ++i) {
    if (matches(state.scenario.stellars[i])) {
      candidates.push_back(static_cast<std::int16_t>(i));
    }
  }
  if (candidates.empty()) {
    return fallback;
  }
  std::uniform_int_distribution<std::size_t> roll(0, candidates.size() - 1);
  return candidates[roll(state.rng)];
}

[[nodiscard]] std::vector<std::int16_t>
EvaluateMissionPage(const GameState &state, std::int16_t page_group) {
  std::vector<std::int16_t> result;
  // Mission_CheckMissionShipInteractionEligibility (0x00441b40) performs
  // these two definition-level gates before evaluating the location and
  // player-state filters. Without them, placeholder/one-shot definitions
  // leak onto the ordinary landed BBS and valid ferry entries can be mixed
  // with the wrong list lane.
  const auto selected_stellar = state.travel.selected_stellar_id;
  for (std::size_t index = 0; index < state.scenario.missions.size(); ++index) {
    const auto &mission = state.scenario.missions[index];
    // Mission lists use the zero-based definition index. The +0x80 offset is
    // only a resource-manager convention and is added at ScenarioData::Mission.
    const auto id = static_cast<std::int16_t>(index);
    if (!mission.present || !mission.is_available_runtime ||
        mission.special_ship_start < 1 || mission.return_stellar_id == -1 ||
        IsActiveMission(state, id) ||
        (page_group >= 0 && mission.return_stellar_id != 2 &&
         mission.return_stellar_id != page_group)) {
      continue;
    }
    // A concrete 0x80..0x87f link is the stellar that advertises the mission.
    // During non-landed list evaluation there is no selected stellar yet, so
    // retain the resource-wide list used by tests and by the travel overlay.
    if (selected_stellar >= kResourceIdBase &&
        mission.link_system_filter >= kResourceIdBase &&
        mission.link_system_filter != selected_stellar) {
      continue;
    }
    if (mission.link_system_filter >= 5000 &&
        mission.link_system_filter < 10000) {
      if (state.player.current_system_id < 0 ||
          state.player.current_system_id >=
              static_cast<std::int16_t>(state.scenario.systems.size())) {
        continue;
      }
      const auto adjacent_resource_id =
          static_cast<std::int16_t>(mission.link_system_filter - 5000);
      const auto &current_system =
          state.scenario.systems[static_cast<std::size_t>(
              state.player.current_system_id)];
      if (std::find(current_system.links.begin(),
                    current_system.links.end(),
                    adjacent_resource_id) == current_system.links.end()) {
        continue;
      }
    }
    if (mission.link_system_filter < -1 ||
        (mission.link_system_filter >= kResourceIdBase + 0x800)) {
      continue;
    }
    if (mission.present && mission.is_available_runtime) {
      if (!IsActiveMission(state, id) &&
          Mission_PassesAcceptanceResourceGates(state, mission)) {
        result.push_back(id);
      }
    }
  }
  std::stable_sort(result.begin(), result.end(), [&](auto lhs, auto rhs) {
    const auto &left = state.scenario.missions[static_cast<std::size_t>(lhs)];
    const auto &right = state.scenario.missions[static_cast<std::size_t>(rhs)];
    if (left.list_priority != right.list_priority) {
      return left.list_priority < right.list_priority;
    }
    return lhs < rhs;
  });
  return result;
}

// Ghidra 0x0043d4c0 Mission_ResolveMissionSpecialShipCount.
[[nodiscard]] std::int16_t ResolveSpecialShipCount(GameState &state,
                                                   std::int16_t encoded) {
  if (encoded >= 0) {
    return encoded;
  }
  if (encoded == -1) {
    return 0;
  }
  const auto magnitude = static_cast<std::int16_t>(-encoded);
  if (magnitude <= 0) {
    return 0;
  }
  std::uniform_int_distribution<int> roll(0, magnitude - 1);
  return static_cast<std::int16_t>(roll(state.rng) +
                                   (static_cast<int>(magnitude) + 1) / 2);
}

// Ghidra 0x0043d490 Mission_ResolveMissionSpecialShipSystem.
[[nodiscard]] std::int16_t ResolveSpecialShipSystem(GameState &state,
                                                    std::int16_t encoded) {
  if (encoded >= 0 && encoded < 1000) {
    return encoded;
  }
  if (encoded != 1000) {
    return -1;
  }
  std::uniform_int_distribution<int> roll(0, 5);
  return static_cast<std::int16_t>(roll(state.rng));
}

[[nodiscard]] std::int16_t ResolveMissionSystemByLocator(GameState &state,
                                                         std::int16_t locator,
                                                         std::int16_t fallback);

[[nodiscard]] std::int16_t
ResolveMissionCurrentSystem(GameState &state,
                            const MissionDef &definition,
                            const MissionTargetResolution &target) {
  const auto locator = definition.current_system_locator;
  if (locator == -1) {
    return state.player.current_system_id;
  }
  if (locator == -3) {
    return target.on_fail_system_id;
  }
  if (locator == -4) {
    return target.on_success_system_id;
  }
  if (locator == -2) {
    return ResolveMissionSystemByLocator(
        state, locator, state.player.current_system_id);
  }
  if (locator >= kResourceIdBase && locator < kResourceIdBase + 0x800) {
    return ResolveContainingSystem(
        state, static_cast<std::int16_t>(locator - kResourceIdBase));
  }
  return ResolveMissionSystemByLocator(state, locator, -1);
}

[[nodiscard]] std::int16_t SelectMissionShipType(GameState &state,
                                                 std::int16_t dude_id,
                                                 std::uint16_t flags) {
  if ((flags & 0x0800U) == 0 || dude_id < 0) {
    return -1;
  }
  const auto *dude =
      state.scenario.Dude(static_cast<std::int16_t>(dude_id + kResourceIdBase));
  if (dude == nullptr || !dude->present) {
    return -1;
  }
  // Mission_PopulateMissionSlotFromDef first permits available ship types,
  // then retries with the ignore-availability selector when none remain.
  const auto selected = NovaDude_SelectShipTypeIndex(*dude, false, state.rng);
  return selected >= 0 ? static_cast<std::int16_t>(selected)
                       : static_cast<std::int16_t>(NovaDude_SelectShipTypeIndex(
                             *dude, true, state.rng));
}

// Ghidra 0x0043e6f0 Mission_SelectMissionSystemByLocator.
[[nodiscard]] std::int16_t ResolveMissionSystemByLocator(
    GameState &state, std::int16_t locator, std::int16_t fallback) {
  if (locator >= kResourceIdBase && locator < kResourceIdBase + 0x800) {
    const auto system_id = static_cast<std::int16_t>(locator - kResourceIdBase);
    return system_id >= 0 && system_id < static_cast<std::int16_t>(
                                             state.scenario.systems.size())
               ? system_id
               : fallback;
  }
  const auto current = state.player.current_system_id;
  const auto choose_random = [&state](
                                 const std::vector<std::int16_t> &candidates,
                                 std::int16_t no_match) {
    if (candidates.empty()) {
      return no_match;
    }
    std::uniform_int_distribution<std::size_t> roll(0, candidates.size() - 1);
    return candidates[roll(state.rng)];
  };
  if (locator == -2) {
    std::vector<std::int16_t> candidates;
    for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
      const auto &system = state.scenario.systems[i];
      if (static_cast<std::int16_t>(i) != current && system.is_visible &&
          system.has_explored_flag) {
        candidates.push_back(static_cast<std::int16_t>(i));
      }
    }
    return choose_random(candidates, fallback);
  }
  if (locator == -5) {
    if (current < 0 ||
        current >= static_cast<std::int16_t>(state.scenario.systems.size())) {
      return fallback;
    }
    std::vector<std::int16_t> candidates;
    for (const auto linked_resource_id :
         state.scenario.systems[static_cast<std::size_t>(current)].links) {
      if (linked_resource_id < kResourceIdBase) {
        continue;
      }
      const auto linked =
          static_cast<std::int16_t>(linked_resource_id - kResourceIdBase);
      if (linked >= 0 &&
          linked < static_cast<std::int16_t>(state.scenario.systems.size()) &&
          state.scenario.systems[static_cast<std::size_t>(linked)].is_visible) {
        candidates.push_back(linked);
      }
    }
    return choose_random(candidates, fallback);
  }
  const auto same_class = [&state](std::int16_t lhs,
                                   std::int16_t rhs,
                                   bool require_same) {
    if (lhs < 0 || rhs < 0 ||
        lhs >= static_cast<std::int16_t>(state.scenario.governments.size()) ||
        rhs >= static_cast<std::int16_t>(state.scenario.governments.size())) {
      return false;
    }
    for (const auto left_class :
         state.scenario.governments[static_cast<std::size_t>(lhs)].classes) {
      for (const auto right_class :
           state.scenario.governments[static_cast<std::size_t>(rhs)].classes) {
        if (left_class >= 0 && left_class == right_class) {
          return require_same;
        }
      }
    }
    return !require_same;
  };
  const auto matches = [&](const System &system) {
    if (!system.is_visible || system.government_id < 0) {
      return false;
    }
    const auto govt = system.government_id;
    if (locator >= 10000 && locator < 15000) {
      return govt == locator - 10000;
    }
    if (locator >= 15000 && locator < 20000) {
      return NovaGovernment_AreGovtsAllied(
          state.scenario, govt, locator - 15000);
    }
    if (locator >= 20000 && locator < 25000) {
      return govt != locator - 20000;
    }
    if (locator >= 25000 && locator < 30000) {
      return NovaGovernment_AreGovtsHostileOrXenophobic(
          state.scenario, govt, locator - 25000);
    }
    if (locator >= 30000 && locator < 31000) {
      return same_class(govt, locator - 30000, true);
    }
    if (locator >= 31000 && locator < 32000) {
      return same_class(govt, locator - 31000, false);
    }
    return false;
  };
  std::vector<std::int16_t> candidates;
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    if (static_cast<std::int16_t>(i) != current &&
        matches(state.scenario.systems[i])) {
      candidates.push_back(static_cast<std::int16_t>(i));
    }
  }
  return choose_random(candidates, fallback);
}

} // namespace

// Ghidra 0x0043c3e0 Misn_ResolveMissionStellarLocators.
void Mission_ResolveMissionStellarLocators(GameState &state) {
  for (std::size_t index = 0; index < state.scenario.missions.size(); ++index) {
    const auto &definition = state.scenario.missions[index];
    if (!definition.present) {
      continue;
    }
    auto &target = state.mission_target_resolutions[index];
    target = {};
    target.on_fail_stellar_id =
        ResolveMissionStellar(state, definition.on_fail_condition, -1, -1);
    target.on_fail_system_id =
        ResolveContainingSystem(state, target.on_fail_stellar_id);
    target.on_success_stellar_id =
        definition.on_success_condition == -1
            ? target.on_fail_stellar_id
            : ResolveMissionStellar(state,
                                    definition.on_success_condition,
                                    target.on_fail_stellar_id,
                                    target.on_fail_stellar_id);
    target.on_success_system_id =
        ResolveContainingSystem(state, target.on_success_stellar_id);
    target.special_ship_system_id =
        ResolveSpecialShipSystem(state, definition.special_ship_system);
    target.special_ship_count =
        ResolveSpecialShipCount(state, definition.special_ship_count);
  }
}

// Ghidra 0x0043cf00 Mission_EvaluateMissionLists.
MissionListEvaluation Mission_EvaluateMissionLists(GameState &state) {
  const ControlExpressionState expression =
      MissionControlExpressionState(state);
  for (auto &mission : state.scenario.missions) {
    mission.is_available_runtime =
        mission.present && mission.link_system_filter != -32000 &&
        NovaControlExpression_Evaluate(mission.availability_expr, expression);
  }
  Mission_ResolveMissionStellarLocators(state);
  MissionListEvaluation result;
  // The clean-room BBS API historically exposed the complete available list
  // through page_zero. Keep that compatibility lane while page_one exposes
  // the verified return-mission group for callers that need it explicitly.
  result.page_zero = EvaluateMissionPage(state, -1);
  result.page_one = EvaluateMissionPage(state, 1);
  result.has_return_mission = !result.page_one.empty();
  return result;
}

bool Mission_PopulateActiveSlot(GameState &state,
                                std::int16_t mission_id,
                                std::size_t active_slot) {
  if (active_slot >= GameState::kMaxActiveMissions) {
    return false;
  }
  // Ghidra's Mission_PopulateMissionSlotFromDef receives the zero-based
  // mission definition index; translate only when loading the mïsn resource.
  const auto definition_index = static_cast<std::int32_t>(mission_id);
  if (definition_index < 0 ||
      definition_index >=
          static_cast<std::int32_t>(state.scenario.missions.size())) {
    return false;
  }
  const auto *definition = state.scenario.Mission(
      static_cast<std::int16_t>(mission_id + kResourceIdBase));
  if (definition == nullptr || !definition->present) {
    return false;
  }

  auto &active = state.active_missions[active_slot];
  active = {};
  const auto &target =
      state.mission_target_resolutions[static_cast<std::size_t>(mission_id)];

  active.on_fail_stellar_id = target.on_fail_stellar_id;
  active.on_success_stellar_id = target.on_success_stellar_id;
  active.target_ship_count = definition->target_ship_count;
  active.dude_def_index = definition->special_ship_dude;
  if (active.dude_def_index >= kResourceIdBase) {
    active.dude_def_index =
        static_cast<std::int16_t>(active.dude_def_index - kResourceIdBase);
  }
  active.spawn_behavior = definition->spawn_behavior;
  active.fleet_spawn_goal = definition->fleet_spawn_goal;
  active.special_ship_spawn_mode = definition->special_ship_spawn_mode;
  active.current_system_id =
      ResolveMissionCurrentSystem(state, *definition, target);
  active.special_ship_system_id =
      target.special_ship_system_id >= 0
          ? target.special_ship_system_id
          : ResolveSpecialShipSystem(state, definition->special_ship_system);
  active.special_ship_count =
      target.special_ship_count > 0
          ? target.special_ship_count
          : ResolveSpecialShipCount(state, definition->special_ship_count);
  active.mission_link_systems = target.on_fail_system_id;
  active.mission_system_b = target.on_success_system_id;
  active.comp_govt_id = definition->competing_government_id;
  if (active.comp_govt_id < kResourceIdBase || active.comp_govt_id > 0x17f) {
    active.comp_govt_id = -1;
  } else {
    active.comp_govt_id =
        static_cast<std::int16_t>(active.comp_govt_id - kResourceIdBase);
  }
  active.comp_reward_delta = definition->competing_reputation_delta;
  active.on_resolve_repeat_count = definition->on_resolve_repeat_count;
  active.flags_primary = definition->flags_primary;
  active.flags_secondary = definition->flags_secondary;
  active.resource_delta_or_cost = definition->resource_delta_or_cost;
  active.goal_counter_a = 0;
  active.goal_counter_b = 0;
  active.goal_counter_c = 0;
  active.goal_count_remaining = definition->target_ship_count;
  active.goal_counter_e = 0;
  active.mission_target_count = definition->target_ship_count;
  active.has_been_visited = definition->start_visited;
  active.is_accepted = false;
  active.mission_template_id = mission_id;
  active.mission_ship_count_max = definition->mission_ship_count_max;
  active.aux_ships_dude_def_index = definition->auxiliary_ship_dude;
  if (active.aux_ships_dude_def_index >= kResourceIdBase) {
    active.aux_ships_dude_def_index = static_cast<std::int16_t>(
        active.aux_ships_dude_def_index - kResourceIdBase);
  }
  active.mission_ship_count_active = active.mission_ship_count_max;
  active.mission_fleet_metric_b = definition->auxiliary_ship_dude;
  active.mission_fleet_metric_c = 0;
  active.special_ship_type_index =
      SelectMissionShipType(state, active.dude_def_index, active.flags_primary);
  active.special_ship_name_string_id = definition->special_ship_name_string_id;
  active.random_text_string_id = definition->random_text_string_id;
  active.brief_description_ids = definition->brief_description_ids;
  active.brief_description_id = definition->initial_briefing_id;
  if (definition->special_ship_spawn_mode < 1) {
    active.spawn_rearm_timer = -1;
  } else if (definition->fleet_spawn_goal == 1 &&
             definition->spawn_behavior == 3) {
    active.spawn_rearm_timer = 30;
  } else {
    std::uniform_int_distribution<int> roll(0, 99);
    active.spawn_rearm_timer = static_cast<std::int16_t>(100 + roll(state.rng));
  }
  std::uniform_int_distribution<int> rearm_roll(0, 69);
  active.rearm_roll_clock =
      static_cast<std::int16_t>(70 + rearm_roll(state.rng));
  const auto copy_text_buffer = [&](auto &destination,
                                    std::size_t source_offset) {
    if (source_offset < definition->raw_payload.size()) {
      const auto available = definition->raw_payload.size() - source_offset;
      std::copy_n(definition->raw_payload.begin() +
                      static_cast<std::ptrdiff_t>(source_offset),
                  std::min(destination.size(), available),
                  destination.begin());
    }
  };
  copy_text_buffer(active.on_accept_text, 0x15b);
  copy_text_buffer(active.mission_payload_text_b, 0x25a);
  copy_text_buffer(active.on_success_text, 0x359);
  copy_text_buffer(active.on_failure_text, 0x458);
  copy_text_buffer(active.resolve_script_buffer_start, 0x557);
  copy_text_buffer(active.state_latch, 0x660);
  std::copy(definition->raw_payload.begin(),
            definition->raw_payload.end(),
            active.raw_payload.begin());
  return true;
}

bool Mission_ActivateAtSlot(GameState &state, std::int16_t mission_id) {
  if (mission_id < 0 ||
      mission_id >= static_cast<std::int16_t>(state.scenario.missions.size())) {
    return false;
  }
  const auto &definition =
      state.scenario.missions[static_cast<std::size_t>(mission_id)];
  if (!definition.present || IsActiveMission(state, mission_id) ||
      !Mission_PassesAcceptanceResourceGates(state, definition)) {
    return false;
  }
  std::size_t free_slot = GameState::kMaxActiveMissions;
  for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions; ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      free_slot = slot;
      break;
    }
  }
  if (free_slot == GameState::kMaxActiveMissions ||
      !Mission_PopulateActiveSlot(state, mission_id, free_slot)) {
    return false;
  }

  auto &runtime = state.active_mission_runtime_flags[free_slot];
  runtime = {};
  runtime.is_active = true;
  runtime.flags_primary_at_accept =
      state.active_missions[free_slot].flags_primary;
  state.active_missions[free_slot].is_accepted = true;
  // Mission_ActivateMissionAtSlot suppresses the initial destination briefing
  // when there is no failure stellar to present. The travel-window half of
  // this condition is UI-owned; this state-only API can preserve the verified
  // no-destination branch.
  runtime.initial_briefing_done =
      state.active_missions[free_slot].on_fail_stellar_id == -1;
  // The original treats values below -50000 as an immediate acceptance fee;
  // the encoded value includes the -50000 sentinel, so preserve its unusual
  // arithmetic and clamp the resulting credit balance at zero.
  const auto acceptance_value =
      state.active_missions[free_slot].resource_delta_or_cost;
  if (acceptance_value < -50000) {
    const auto charge = -50000 - acceptance_value;
    state.player.credits =
        std::max<std::int32_t>(0, state.player.credits - charge);
  }
  return true;
}

std::int16_t Misn_ResolveVisibleSystemForTravel(const GameState &state,
                                                std::int16_t system_id) {
  const auto count = static_cast<std::int16_t>(state.scenario.systems.size());
  if (system_id < 0 || system_id >= 0x800 || system_id >= count) {
    return -1;
  }
  const System &entry =
      state.scenario.systems[static_cast<std::size_t>(system_id)];
  const std::int16_t root = entry.visibility_root_system_id;
  if (root == -1) {
    return entry.is_visible ? system_id : -1;
  }
  std::int16_t current = root;
  while (current != -1) {
    // Bounds-guarded chain walk; the original trusts the loader-written ids.
    if (current < 0 || current >= count) {
      return -1;
    }
    if (state.scenario.systems[static_cast<std::size_t>(current)].is_visible) {
      return current;
    }
    current = state.scenario.systems[static_cast<std::size_t>(current)]
                  .visible_parent_system_id;
  }
  return -1;
}

void Misn_TickActiveMissionTimers(GameState &state) {
  for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
    if (!state.active_mission_runtime_flags[slot].is_active) {
      continue;
    }
    ActiveMission &mission = state.active_missions[slot];
    const std::int16_t raw_system = mission.current_system_id;
    const std::int16_t resolved =
        (raw_system < 0 || raw_system >= 0x800)
            ? -1
            : Misn_ResolveVisibleSystemForTravel(state, raw_system);
    // A mission with live target ships whose destination is the player's
    // current system (or the "here" sentinel -6) rearms its spawn clock:
    // behavior-3 fleets on goal 1 use the fixed 30-tick clock, everything else
    // rolls 100..199. The goal-countdown latch clears with it.
    if (mission.target_ship_count > 0 &&
        ((resolved == state.player.current_system_id || raw_system == -6) &&
         mission.special_ship_spawn_mode == 1)) {
      if (mission.fleet_spawn_goal == 1 && mission.spawn_behavior == 3) {
        mission.spawn_rearm_timer = 30;
      } else {
        mission.spawn_rearm_timer =
            static_cast<std::int16_t>(RandomBelow(state, 100) + 100);
      }
      mission.goal_count_remaining = 0;
    }
    if ((mission.flags_primary & 0x10) != 0) {
      mission.mission_ship_count_active = mission.mission_ship_count_max;
    }
    mission.rearm_roll_clock =
        static_cast<std::int16_t>(RandomBelow(state, 0x46) + 0x46);
    mission.mission_fleet_metric_c = 0;
  }
}

bool Mission_CheckReactionConditionSatisfied(const GameState &state,
                                             std::string_view condition) {
  if (condition.empty()) {
    return true;
  }
  switch (condition.front()) {
  case 'b':
  case 'B':
  case '(':
  case '!':
  case 'P':
  case 'p':
  case 'G':
  case 'g':
  case 'O':
  case 'o':
  case 'E':
  case 'e':
    break;
  default:
    return false;
  }
  // The original copies the condition into the shared script scratch buffer,
  // inserting a space between adjacent open parens so the tokenizer sees
  // nested groups. The clean-room evaluator takes the string directly.
  std::string normalized;
  normalized.reserve(condition.size() + 1);
  char previous = ' ';
  for (const char c : condition) {
    if (c == '(' && previous == '(') {
      normalized.push_back(' ');
    }
    normalized.push_back(c);
    previous = c;
  }
  return NovaControlExpression_Evaluate(normalized,
                                        MissionControlExpressionState(state));
}

// Ghidra 0x00440aa0 Mission_ClearMisnSlotAssignments. Releases every ship
// assigned to the mission-fleet slot: clears its fleet link, restores the
// class-default AI behavior when it held a target, and re-enters AI state 2.
// TODO(decomp): the original despawns the assigned ships when
// g_travel_scene_ctx != 0 (the landing/travel scene owns the world); the
// clean-room runtime has no such context latch yet.
void Mission_ClearMisnSlotAssignments(GameState &state,
                                      std::int16_t mission_slot,
                                      bool emit_completion_payload,
                                      std::uint32_t now_ms) {
  for (Ship &ship : state.ships_) {
    if (!ship.is_active || ship.mission_fleet_slot != mission_slot) {
      continue;
    }
    ship.mission_fleet_slot = -1;
    if (ship.ai_target_ship_slot != -1) {
      const auto *ship_class = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + kResourceIdBase));
      if (ship_class != nullptr) {
        ship.ai_behavior_code = ship_class->default_ai_behavior;
      }
      ship.ai_target_ship_slot = -1;
      NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
    }
  }
  if (emit_completion_payload) {
    Mission_RunMisnScriptPayload(
        state,
        TextOf(state.active_missions[static_cast<std::size_t>(mission_slot)]
                   .resolve_script_buffer_start),
        mission_slot);
  }
  state.active_missions[static_cast<std::size_t>(mission_slot)].is_accepted =
      false;
  state.active_mission_runtime_flags[static_cast<std::size_t>(mission_slot)]
      .is_active = false;
  // The original also invalidates g_last_system_for_ambient_rolls (0xffff);
  // the clean-room ambient-roll cache latch is not modelled (TODO(decomp)).
}

// Ghidra 0x00440410 Mission_ResolveMissionSuccess.
void Mission_ResolveMissionSuccess(GameState &state,
                                   std::int16_t mission_slot) {
  const auto slot = static_cast<std::size_t>(mission_slot);
  ActiveMission &mission = state.active_missions[slot];
  // MisnActive +0x3d selects the success debrief selection dialog
  // (Ui_LoadSelectionDialogResource + Stellar_BuildTravelDestination-
  // Description + Ui_RunTravelSelectionDialog). UI-owned; not reconstructed.
  if (mission.brief_description_ids[4] != -1) {
    NovaLog::Todo("mission success debrief dialog (misn {} id {}) not "
                  "reconstructed yet",
                  mission.mission_template_id,
                  mission.brief_description_ids[4]);
  }
  state.active_mission_runtime_flags[slot].is_active = false;
  Mission_RunMisnScriptPayload(
      state, TextOf(mission.on_success_text), mission_slot);
  // On-resolve repeat count re-runs the daily availability reroll
  // (ShipClass_RerollShipClassAvailabilityChances 0x00466cb0) once per count.
  // That driver is not ported yet.
  if (mission.on_resolve_repeat_count > 0) {
    NovaLog::Todo("mission success repeat-count {} requires the daily "
                  "availability reroll (0x00466cb0), not yet ported",
                  mission.on_resolve_repeat_count);
  }
  const std::int16_t govt = mission.comp_govt_id;
  const std::int16_t delta = mission.comp_reward_delta;
  if (govt >= 0 && govt < 0x100) {
    const auto count = static_cast<std::int16_t>(
        std::min<std::size_t>(state.system_reputation.size(), 0x800));
    for (std::int16_t i = 0; i < count; ++i) {
      const std::int16_t system_govt =
          state.scenario.systems[static_cast<std::size_t>(i)].government_id;
      auto &rep = state.system_reputation[static_cast<std::size_t>(i)];
      if (system_govt == govt) {
        rep = static_cast<std::int16_t>(rep + delta);
      } else if (system_govt != -1) {
        if (NovaGovernment_AreGovtsHostileOrXenophobic(
                state.scenario, system_govt, govt)) {
          rep = static_cast<std::int16_t>(std::lrint(
              static_cast<float>(rep) - static_cast<float>(delta) * 0.5F));
        } else if (NovaGovernment_AreGovtsAllied(
                       state.scenario, system_govt, govt)) {
          rep = static_cast<std::int16_t>(std::lrint(
              static_cast<float>(rep) + static_cast<float>(delta) * 0.5F));
        }
      }
    }
  }
  NovaGovernment_ApplyReputationCreditDelta(state,
                                            mission.resource_delta_or_cost);
  // Ambient-roll latch invalidation is not modelled (TODO(decomp)).
}

// Ghidra 0x00440930 Mission_ResolveMissionFailure.
void Mission_ResolveMissionFailure(GameState &state,
                                   std::int16_t mission_slot,
                                   std::uint32_t now_ms) {
  const auto slot = static_cast<std::size_t>(mission_slot);
  ActiveMission &mission = state.active_missions[slot];
  Mission_RunMisnScriptPayload(
      state, TextOf(mission.on_failure_text), mission_slot);
  const std::int16_t govt = mission.comp_govt_id;
  if (govt != -1) {
    // Failure subtracts half the reputation delta (integer division rounded
    // toward zero) from every system owned by the competing government.
    const std::int16_t half_delta =
        static_cast<std::int16_t>(mission.comp_reward_delta / 2);
    const auto count = static_cast<std::int16_t>(
        std::min<std::size_t>(state.system_reputation.size(), 0x800));
    for (std::int16_t i = 0; i < count; ++i) {
      const std::int16_t system_govt =
          state.scenario.systems[static_cast<std::size_t>(i)].government_id;
      if (system_govt == govt) {
        auto &rep = state.system_reputation[static_cast<std::size_t>(i)];
        rep = static_cast<std::int16_t>(rep - half_delta);
      }
    }
  }
  state.active_mission_runtime_flags[slot].is_active = false;
  // MisnActive +0x3f selects the failure debrief dialog. UI-owned; not
  // reconstructed.
  if (mission.brief_description_ids[5] != -1) {
    NovaLog::Todo("mission failure debrief dialog (misn {} id {}) not "
                  "reconstructed yet",
                  mission.mission_template_id,
                  mission.brief_description_ids[5]);
  }
  Mission_ClearMisnSlotAssignments(state, mission_slot, false, now_ms);
}

} // namespace game
