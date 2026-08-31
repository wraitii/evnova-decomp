#include "game/game_state.hpp"
#include "game/mission.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace game;

TEST_CASE("mission lists use zero-based definition indices") {
  GameState state;
  state.scenario.missions.resize(3);
  state.scenario.missions[0].present = true;
  state.scenario.missions[0].list_priority = 20;
  state.scenario.missions[0].avail_random = 1;
  state.scenario.missions[0].avail_location = 0;
  state.scenario.missions[1].present = true;
  state.scenario.missions[1].list_priority = 10;
  state.scenario.missions[1].avail_random = 1;
  state.scenario.missions[1].avail_location = 0;
  state.scenario.missions[2].present = true;
  state.scenario.missions[2].availability_expr = "B7";

  auto result = Mission_EvaluateMissionLists(state);

  // List priority sorts descending (the original bucket pass emits the
  // highest MisnDef +0x128 priority first).
  REQUIRE(result.page_zero == std::vector<std::int16_t>{0, 1});
  CHECK(result.page_zero != std::vector<std::int16_t>{0x80});
}

TEST_CASE("scenario ferry missions expose their decoded availability fields") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  std::size_t ferry_count = 0;
  std::int16_t first_link = 0;
  std::string first_availability;
  for (std::size_t index = 0; index < state.scenario.missions.size(); ++index) {
    const auto &mission = state.scenario.missions[index];
    if (!mission.present ||
        mission.display_name.find("Ferry") == std::string::npos) {
      continue;
    }
    ++ferry_count;
    if (ferry_count == 1) {
      first_link = mission.link_system_filter;
      first_availability = mission.availability_expr;
    }
    INFO("ferry mission index="
         << index << " link=" << mission.link_system_filter
         << " availability=" << mission.availability_expr);
  }
  INFO("first ferry link=" << first_link
                           << " availability=" << first_availability);
  CHECK(ferry_count > 0);

  // Tichel's BBS advertises the concrete source stellar carried by the
  // mission definition rather than the containing system. The first Ferry
  // (Mu'Randa) is an AvailLoc-3 (spaceport-dialog) definition gated by
  // AvailRating 600, so simulate the plot-stage player and expect it on the
  // services lane (page_one) once the source stellar is selected.
  state.player_combat_rating_points = 1000;
  state.travel.selected_stellar_id = first_link;
  const auto at_tichel = Mission_EvaluateMissionLists(state);
  CHECK(std::any_of(at_tichel.page_one.begin(),
                    at_tichel.page_one.end(),
                    [&state](std::int16_t mission_id) {
                      const auto *mission = state.scenario.Mission(
                          static_cast<std::int16_t>(mission_id + 0x80));
                      return mission != nullptr &&
                             mission->display_name.find("Ferry") !=
                                 std::string::npos;
                    }));
}

TEST_CASE(
    "mission activation translates resource fields at the active boundary") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.target_ship_count = 4;
  definition.special_ship_dude = 0x82;
  definition.auxiliary_ship_dude = 0x81;
  definition.competing_government_id = 0x80;
  definition.competing_reputation_delta = -3;
  definition.flags_primary = 0x8123;
  definition.flags_secondary = 0x0040;
  definition.travel_stellar_locator = 0x80;
  definition.return_stellar_locator = 0x81;
  definition.cargo_type_resource = 7;
  definition.cargo_qty_tons = 2;
  definition.current_system_locator = -1;
  definition.pickup_mode = 0;

  state.scenario.stellars.resize(2);
  state.scenario.stellars[0].system_id = 11;
  state.scenario.stellars[1].system_id = 12;
  state.player.current_system_id = 9;
  Mission_ResolveMissionStellarLocators(state);

  REQUIRE(Mission_ActivateAtSlot(state, 0, -1));
  const auto &active = state.active_missions[0];
  CHECK(active.carrying_resources); // PickupMode 0: cargo aboard at accept
  CHECK(active.mission_template_id == 0);
  CHECK(active.travel_stellar_id == 0);
  CHECK(active.return_stellar_id == 1);
  CHECK(active.dude_def_index == 2);
  CHECK(active.aux_ships_dude_def_index == 1);
  CHECK(active.comp_govt_id == 0);
  CHECK(active.current_system_id == 9);
  CHECK(active.spawn_rearm_timer == -1);
  CHECK(active.goal_count_remaining == 4);
  CHECK(state.active_mission_runtime_flags[0].flags_primary_at_accept ==
        0x8123);
}

// Regression: a fresh new-game pilot landed at Tichel (system 0x81) must find
// the generic Federation "Ferry Passengers to <DST>" missions on the mission
// computer lane. Exercises the 0x00448090 EvaluateAvailability stellar
// membership prologue + the AvailStel govt-lane gate end to end.
TEST_CASE("fresh BBS at Tichel offers Federation ferry passenger missions") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 1; // Tichel (zero-based)
  state.player.ship_class_id = 0;
  state.player.credits = 10000;
  state.system_reputation.assign(state.scenario.systems.size(), 0);
  // Selected stellar = the landing anchor the land command auto-picks.
  std::int16_t anchor = -1;
  if (const auto *sys = state.scenario.System(0x81); sys != nullptr) {
    for (const auto nav : sys->nav_defs) {
      if (nav >= 0x80) {
        anchor = nav;
        break;
      }
    }
  }
  REQUIRE(anchor >= 0x80);
  state.travel.selected_stellar_id = anchor;
  Mission_RerollOfferingRolls(state);
  // Pin the per-def AvailRandom rolls to 1 so the 60% definitions always pass
  // gate 4; the reroll cadence itself is covered elsewhere.
  for (auto &roll : state.mission_offering_rolls) {
    roll = 1;
  }

  const auto evaluation = Mission_EvaluateMissionLists(state);
  int ferry_rows = 0;
  for (const auto mission_id : evaluation.page_zero) {
    const auto &mission =
        state.scenario.missions[static_cast<std::size_t>(mission_id)];
    if (mission.display_name.find("Ferry Passengers") != std::string::npos) {
      // Only the Federation variants (AvailStel 10000 = "stellar of government
      // 0") may appear at Tichel.
      CHECK(mission.link_system_filter == 10000);
      ++ferry_rows;
    }
  }
  CHECK(ferry_rows == 3);
}
