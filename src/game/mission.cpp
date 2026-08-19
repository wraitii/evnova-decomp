#include "mission.hpp"

#include "game_state.hpp"

#include <algorithm>
#include <cstring>

namespace game {
namespace {

constexpr std::int16_t kResourceIdBase = 0x80;

[[nodiscard]] bool IsActiveMission(const GameState &state,
                                   std::int16_t mission_id) {
  return std::any_of(
      state.active_missions.begin(), state.active_missions.end(),
      [mission_id](const ActiveMission &mission) {
        return mission.is_accepted && mission.mission_template_id == mission_id;
      });
}

[[nodiscard]] std::int16_t ResolveContainingSystem(const GameState &state,
                                                    std::int16_t stellar_id) {
  if (stellar_id < 0 || stellar_id >=
                           static_cast<std::int16_t>(state.scenario.stellars.size())) {
    return -1;
  }
  return state.scenario.stellars[static_cast<std::size_t>(stellar_id)].system_id;
}

[[nodiscard]] std::int16_t ResolveMissionStellar(const GameState &state,
                                                 std::int16_t locator,
                                                 std::int16_t excluded,
                                                 std::int16_t fallback) {
  if (locator >= kResourceIdBase && locator < kResourceIdBase + 0x800) {
    const auto stellar_id = static_cast<std::int16_t>(locator - kResourceIdBase);
    if (stellar_id != excluded && stellar_id >= 0 &&
        stellar_id < static_cast<std::int16_t>(state.scenario.stellars.size())) {
      return stellar_id;
    }
    return fallback;
  }
  // The original -2/-3 families select a random eligible stellar. A stable
  // first-match policy is used until NovaRandom and travel reachability are
  // wired into the clean-room locator subsystem.
  if (locator != -2 && locator != -3) {
    return fallback;
  }
  for (std::size_t i = 0; i < state.scenario.stellars.size(); ++i) {
    const auto &stellar = state.scenario.stellars[i];
    if (static_cast<std::int16_t>(i) == excluded || !stellar.is_available ||
        (locator == -2 && (stellar.flags & 0x20U) != 0) ||
        (locator == -3 && (stellar.flags & 0x10U) == 0)) {
      continue;
    }
    return static_cast<std::int16_t>(i);
  }
  return fallback;
}

[[nodiscard]] std::vector<std::int16_t> EvaluateMissionPage(
    const GameState &state) {
  std::vector<std::int16_t> result;
  for (std::size_t index = 0; index < state.scenario.missions.size(); ++index) {
    const auto &mission = state.scenario.missions[index];
    const auto id = static_cast<std::int16_t>(index + kResourceIdBase);
    if (mission.present && mission.is_available_runtime &&
        !IsActiveMission(state, id)) {
      result.push_back(id);
    }
  }
  std::stable_sort(result.begin(), result.end(), [&](auto lhs, auto rhs) {
    const auto &left = state.scenario.missions[static_cast<std::size_t>(lhs - kResourceIdBase)];
    const auto &right = state.scenario.missions[static_cast<std::size_t>(rhs - kResourceIdBase)];
    if (left.list_priority != right.list_priority) {
      return left.list_priority < right.list_priority;
    }
    return lhs < rhs;
  });
  return result;
}

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
  return static_cast<std::int16_t>(
      roll(state.rng) + (static_cast<int>(magnitude) + 1) / 2);
}

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

} // namespace

void Mission_ResolveMissionStellarLocators(GameState &state) {
  for (std::size_t index = 0; index < state.scenario.missions.size(); ++index) {
    const auto &definition = state.scenario.missions[index];
    if (!definition.present) {
      continue;
    }
    auto &target = state.mission_target_resolutions[index];
    target = {};
    target.on_fail_stellar_id = ResolveMissionStellar(
        state, definition.on_fail_condition, -1, -1);
    target.on_fail_system_id =
        ResolveContainingSystem(state, target.on_fail_stellar_id);
    target.on_success_stellar_id = ResolveMissionStellar(
        state, definition.on_success_condition, target.on_fail_stellar_id,
        target.on_fail_stellar_id);
    target.on_success_system_id =
        ResolveContainingSystem(state, target.on_success_stellar_id);
    target.special_ship_system_id =
        ResolveSpecialShipSystem(state, definition.special_ship_system);
    target.special_ship_count =
        ResolveSpecialShipCount(state, definition.special_ship_count);
  }
}

MissionListEvaluation Mission_EvaluateMissionLists(GameState &state) {
  ControlExpressionState expression;
  expression.get_control_bit = [&state](std::uint32_t bit) {
    return state.control.ControlBit(bit);
  };
  expression.is_registered = [&state](std::uint32_t) {
    return state.control.registered;
  };
  expression.is_male = [&state] { return state.control.male; };
  expression.owns_outfit = [&state](std::int16_t id) {
    return id >= 0 && id < static_cast<std::int16_t>(state.inventory.outfit_owned_count.size()) &&
           state.inventory.outfit_owned_count[static_cast<std::size_t>(id)] > 0;
  };
  expression.has_explored = [&state](std::int16_t id) {
    return id >= 0 && id < 0x800 && state.control.explored_systems.test(
                                      static_cast<std::size_t>(id));
  };
  for (auto &mission : state.scenario.missions) {
    mission.is_available_runtime =
        mission.present && mission.link_system_filter != -32000 &&
        NovaControlExpression_Evaluate(mission.availability_expr, expression);
  }
  Mission_ResolveMissionStellarLocators(state);
  MissionListEvaluation result;
  result.page_zero = EvaluateMissionPage(state);
  // The lane discriminator has not yet been identified in the clean-room
  // MissionDef; preserve the native two-lane API with an empty second lane.
  return result;
}

bool Mission_PopulateActiveSlot(GameState &state, std::int16_t mission_id,
                                std::size_t active_slot) {
  if (active_slot >= GameState::kMaxActiveMissions) {
    return false;
  }
  const auto definition_index = static_cast<std::int32_t>(mission_id) -
                                kResourceIdBase;
  if (definition_index < 0 ||
      definition_index >= static_cast<std::int32_t>(
                              state.scenario.missions.size())) {
    return false;
  }
  const auto *definition = state.scenario.Mission(mission_id);
  if (definition == nullptr || !definition->present) {
    return false;
  }

  auto &active = state.active_missions[active_slot];
  active = {};
  const auto &target = state.mission_target_resolutions[
      static_cast<std::size_t>(mission_id - kResourceIdBase)];

  active.on_fail_stellar_id = target.on_fail_stellar_id;
  active.on_success_stellar_id = target.on_success_stellar_id;
  active.target_ship_count = definition->target_ship_count;
  active.dude_def_index = definition->special_ship_dude;
  active.spawn_behavior = definition->spawn_behavior;
  active.fleet_spawn_goal = definition->fleet_spawn_goal;
  active.special_ship_spawn_mode = definition->special_ship_spawn_mode;
  active.current_system_id = definition->current_system_locator;
  active.special_ship_system_id = target.special_ship_system_id >= 0
                                      ? target.special_ship_system_id
                                      : ResolveSpecialShipSystem(
                                            state,
                                            definition->special_ship_system);
  active.special_ship_count = target.special_ship_count > 0
                                  ? target.special_ship_count
                                  : ResolveSpecialShipCount(
                                        state, definition->special_ship_count);
  active.mission_link_systems = target.on_fail_system_id;
  active.mission_system_b = target.on_success_system_id;
  active.comp_govt_id = definition->competing_government_id;
  active.comp_reward_delta = definition->competing_reputation_delta;
  active.flags_primary = definition->flags_primary;
  active.flags_secondary = definition->flags_secondary;
  active.resource_delta_or_cost = definition->resource_delta_or_cost;
  active.goal_count_remaining = definition->target_ship_count;
  active.has_been_visited = definition->start_visited;
  active.is_accepted = false;
  active.mission_template_id = mission_id;
  active.mission_ship_count_max = definition->mission_ship_count_max;
  active.aux_ships_dude_def_index = definition->auxiliary_ship_dude;
  active.mission_ship_count_active = active.mission_ship_count_max;
  active.special_ship_name_string_id = definition->special_ship_name_string_id;
  active.random_text_string_id = definition->random_text_string_id;
  active.brief_description_ids = definition->brief_description_ids;
  active.brief_description_id = definition->initial_briefing_id;
  active.spawn_rearm_timer = definition->special_ship_spawn_mode > 0
                                 ? 100
                                 : -1;
  std::copy(definition->raw_payload.begin(), definition->raw_payload.end(),
            active.raw_payload.begin());
  return true;
}

bool Mission_ActivateAtSlot(GameState &state, std::int16_t mission_id) {
  if (mission_id < kResourceIdBase ||
      mission_id >= kResourceIdBase +
                         static_cast<std::int16_t>(state.scenario.missions.size())) {
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
  return true;
}

} // namespace game
