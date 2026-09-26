#include "game/compatibility.hpp"
#include "game/game_state.hpp"
#include "game/mission.hpp"
#include "game/outfit.hpp"
#include "game/starmap.hpp"
#include "game/travel.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

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

// Ghidra 0x00441b40 Gate 2: AvailRecord -32000 (the selected stellar is
// dominated) / -32001 (any available stellar is dominated), evaluated only
// outside the in-flight context; otherwise the value falls through to the
// ordinary reputation compare and fails.
TEST_CASE("AvailRecord domination arms gate mission availability") {
  GameState state;
  state.scenario.missions.resize(2);
  state.scenario.stellars.resize(2);
  for (auto &mission : state.scenario.missions) {
    mission.present = true;
    mission.avail_random = 100;
    mission.avail_location = 0;
  }
  state.scenario.missions[0].avail_record = -32000;
  state.scenario.missions[1].avail_record = -32001;
  state.travel.selected_stellar_id = 0x80; // zero-based stellar index 0
  state.scenario.stellars[0].is_available = true;
  state.scenario.stellars[1].is_available = true;

  const auto contains = [](const std::vector<std::int16_t> &list,
                           std::int16_t id) {
    return std::find(list.begin(), list.end(), id) != list.end();
  };

  // Nothing dominated: neither definition is offered.
  {
    const auto lists = Mission_EvaluateMissionLists(state);
    CHECK_FALSE(contains(lists.page_zero, 0));
    CHECK_FALSE(contains(lists.page_zero, 1));
  }
  // The selected stellar is dominated: both arms match.
  state.scenario.stellars[0].dominated = 1;
  {
    const auto lists = Mission_EvaluateMissionLists(state);
    CHECK(contains(lists.page_zero, 0));
    CHECK(contains(lists.page_zero, 1));
  }
  // A different stellar is dominated: only the "any dominated" arm matches.
  state.scenario.stellars[0].dominated = 0;
  state.scenario.stellars[1].dominated = 1;
  {
    const auto lists = Mission_EvaluateMissionLists(state);
    CHECK_FALSE(contains(lists.page_zero, 0));
    CHECK(contains(lists.page_zero, 1));
  }
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

  REQUIRE(Mission_ActivateAtSlot(state, 0));
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

TEST_CASE("acceptance dialogs follow script activation order") {
  GameState state;
  state.scenario.missions.resize(2);
  for (auto &definition : state.scenario.missions) {
    definition.present = true;
    definition.travel_stellar_locator = -1;
    definition.return_stellar_locator = -1;
  }
  // Mission 0's on-accept payload (misn payload +0x15b) starts mission 1 with
  // the S opcode: S129 = resource 0x81 = definition 1.
  constexpr std::string_view kStartNested = "S129";
  for (std::size_t i = 0; i < kStartNested.size(); ++i) {
    state.scenario.missions[0].raw_payload[0x15b + i] =
        static_cast<std::byte>(kStartNested[i]);
  }

  std::vector<std::int16_t> presented;
  const MissionAcceptanceSink sink = [&](std::int16_t mission_def) {
    presented.push_back(mission_def);
  };
  REQUIRE(Mission_ActivateAtSlot(state, 0, sink));

  // The nested activation's dialogs run inside the outer on-accept payload, so
  // it reports first; the outer mission follows when activation returns.
  const std::vector<std::int16_t> expected{1, 0};
  CHECK(presented == expected);
  CHECK(state.active_mission_runtime_flags[0].is_active);
  CHECK(state.active_mission_runtime_flags[1].is_active);
}

// Regression for the Tutorial 006 payload shape: mïsn 754's on-accept is
// `S755 b9208`, i.e. an S activation followed by a control-bit command. The
// acceptance sink is now threaded into the nested activation, and this test
// pins that the parser still executes the commands AFTER the S opcode (the
// original `acceptance dialogs follow script activation order` test only
// covered a payload ending at the S).
TEST_CASE("script S activation preserves trailing commands") {
  GameState state;
  state.scenario.missions.resize(2);
  for (auto &definition : state.scenario.missions) {
    definition.present = true;
    definition.travel_stellar_locator = -1;
    definition.return_stellar_locator = -1;
  }
  // S129 = activate definition 1; b424 must then set control bit 424.
  constexpr std::string_view kStartNestedThenBit = "S129 b424";
  for (std::size_t i = 0; i < kStartNestedThenBit.size(); ++i) {
    state.scenario.missions[0].raw_payload[0x15b + i] =
        static_cast<std::byte>(kStartNestedThenBit[i]);
  }

  std::vector<std::int16_t> presented;
  const MissionAcceptanceSink sink = [&](std::int16_t mission_def) {
    presented.push_back(mission_def);
  };
  REQUIRE(Mission_ActivateAtSlot(state, 0, sink));

  CHECK(state.active_mission_runtime_flags[0].is_active);
  CHECK(state.active_mission_runtime_flags[1].is_active);
  CHECK(state.control.ControlBit(424));
  const std::vector<std::int16_t> expected{1, 0};
  CHECK(presented == expected);
}

// 0x0043f100 runs the on-accept payload BEFORE the acceptance fee and BEFORE
// the carrying/briefing latches. A nested S activation therefore observes the
// outer slot pre-latch, and the outer fee is charged against post-payload
// credits.
TEST_CASE("on-accept payload runs before acceptance fee and latches") {
  GameState state;
  state.scenario.missions.resize(2);
  for (auto &definition : state.scenario.missions) {
    definition.present = true;
    definition.travel_stellar_locator = -1;
    definition.return_stellar_locator = -1;
  }
  state.player.credits = 100;
  state.scenario.missions[0].pickup_mode = 0;                 // carrying latch
  state.scenario.missions[0].resource_delta_or_cost = -50010; // fee 10
  constexpr std::string_view kStartNested = "S129";
  for (std::size_t i = 0; i < kStartNested.size(); ++i) {
    state.scenario.missions[0].raw_payload[0x15b + i] =
        static_cast<std::byte>(kStartNested[i]);
  }

  bool saw_nested = false;
  bool carrying_pre_latch = true;
  bool briefing_pre_latch = true;
  bool credits_pre_fee = false;
  const MissionAcceptanceSink sink = [&](std::int16_t mission_def) {
    if (mission_def != 1) {
      return;
    }
    saw_nested = true;
    carrying_pre_latch = state.active_missions[0].carrying_resources;
    briefing_pre_latch =
        state.active_mission_runtime_flags[0].travel_stellar_reached;
    credits_pre_fee = state.player.credits == 100;
  };
  REQUIRE(Mission_ActivateAtSlot(state, 0, sink));

  CHECK(saw_nested);
  CHECK_FALSE(carrying_pre_latch); // outer carrying latch not set yet
  CHECK_FALSE(briefing_pre_latch); // outer briefing latch not set yet
  CHECK(credits_pre_fee);          // outer fee not charged yet
  CHECK(state.active_missions[0].carrying_resources);
  CHECK(state.player.credits == 90);
}

// 0x0043f100 permits duplicate active slots (eligibility, not activation,
// suppresses re-offering) and charges a below--50000 value after clamping the
// credit balance at zero rather than refusing activation.
TEST_CASE("activation permits duplicates and clamps the acceptance fee") {
  GameState state;
  state.scenario.missions.resize(1);
  state.scenario.missions[0].present = true;
  state.scenario.missions[0].travel_stellar_locator = -1;
  state.scenario.missions[0].return_stellar_locator = -1;
  state.scenario.missions[0].resource_delta_or_cost = -50010; // fee 10
  state.player.credits = 5;

  REQUIRE(Mission_ActivateAtSlot(state, 0));
  CHECK(state.player.credits == 0);
  REQUIRE(Mission_ActivateAtSlot(state, 0));
  CHECK(state.active_mission_runtime_flags[0].is_active);
  CHECK(state.active_mission_runtime_flags[1].is_active);
}

// 0x0043f100 tail clears the definition's offering roll on every activation.
TEST_CASE("activation clears the definition offering roll") {
  GameState state;
  state.scenario.missions.resize(1);
  state.scenario.missions[0].present = true;
  state.scenario.missions[0].travel_stellar_locator = -1;
  state.scenario.missions[0].return_stellar_locator = -1;
  state.mission_offering_rolls[0] = 100;

  REQUIRE(Mission_ActivateAtSlot(state, 0));
  CHECK(state.mission_offering_rolls[0] == 0);
}

// 0x0043f8c0 normalizes the aux-ship dude to 0x80..0x27f (else -1) and clamps
// mission_ship_count_max/active below 1 to -1.
TEST_CASE("aux ship normalization matches Mission_PopulateMissionSlotFromDef") {
  GameState state;
  state.scenario.missions.resize(2);
  for (auto &definition : state.scenario.missions) {
    definition.present = true;
    definition.travel_stellar_locator = -1;
    definition.return_stellar_locator = -1;
  }
  // Out-of-range aux dude: -1, budget -1.
  state.scenario.missions[0].auxiliary_ship_dude = 0x50;
  state.scenario.missions[0].mission_ship_count_max = 4;
  // Valid aux dude and budget: rebased, budget preserved.
  state.scenario.missions[1].auxiliary_ship_dude = 0x82;
  state.scenario.missions[1].mission_ship_count_max = 4;

  REQUIRE(Mission_PopulateActiveSlot(state, 0, 0));
  CHECK(state.active_missions[0].aux_ships_dude_def_index == -1);
  CHECK(state.active_missions[0].mission_ship_count_max == -1);
  CHECK(state.active_missions[0].mission_ship_count_active == -1);

  REQUIRE(Mission_PopulateActiveSlot(state, 1, 1));
  CHECK(state.active_missions[1].aux_ships_dude_def_index == 2);
  CHECK(state.active_missions[1].mission_ship_count_max == 4);
  CHECK(state.active_missions[1].mission_ship_count_active == 4);
}

// The resolver 0x0043d240 owns cargo decoding; population copies the resolved
// target directly. An unresolved target stays 0 for both the acceptance gate
// and population, so no fallback draw can smuggle cargo past the gate.
TEST_CASE("population copies resolved cargo without re-resolving") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.travel_stellar_locator = -1;
  definition.return_stellar_locator = -1;
  definition.cargo_qty_tons = 2;
  definition.cargo_type_resource = 7;

  REQUIRE(Mission_PopulateActiveSlot(state, 0, 0));
  CHECK(state.active_missions[0].cargo_qty_tons == 0);
  CHECK(state.active_missions[0].cargo_type_id == -1);
}

// The cargo gate reads the resolved target CargoQty, not the raw definition.
TEST_CASE("acceptance cargo gate uses the resolved target cargo") {
  GameState state;
  state.scenario.missions.resize(1);
  state.scenario.missions[0].present = true;
  state.scenario.missions[0].travel_stellar_locator = -1;
  state.scenario.missions[0].return_stellar_locator = -1;
  state.scenario.ships.resize(1);
  state.scenario.ships[0].cargo_holds = 1;
  state.player.ship_class_id = 0;
  state.mission_target_resolutions[0].cargo_qty_tons = 5;

  CHECK_FALSE(Mission_ActivateAtSlot(state, 0));
}

// 0x0043f8c0 reads DatePostInc/on_resolve_repeat_count from musn +0x65e, not
// the AuxShipCount at +0x48.
TEST_CASE("mission DatePostInc decodes from payload offset 0x65e") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const auto be16 = [](const std::array<std::byte, 0x7b2> &payload,
                       std::size_t offset) {
    return static_cast<std::int16_t>(
        (std::to_integer<unsigned>(payload[offset]) << 8) |
        std::to_integer<unsigned>(payload[offset + 1]));
  };
  std::size_t checked = 0;
  for (const auto &mission : state.scenario.missions) {
    if (!mission.present) {
      continue;
    }
    CHECK(mission.on_resolve_repeat_count == be16(mission.raw_payload, 0x65e));
    ++checked;
  }
  CHECK(checked > 0);
}

// 0x0043f100 tail auto-resolves a mid-transition flags-0x0001 mission with no
// target ships and no return stellar once the briefing latch is set.
TEST_CASE("transition activation auto-resolves a flags-1 mission") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.flags_primary = 0x0001;
  definition.target_ship_count = 0;
  definition.travel_stellar_locator = -1;
  definition.return_stellar_locator = -1;
  state.system_transition_active = true;

  REQUIRE(Mission_ActivateAtSlot(state, 0));
  CHECK_FALSE(state.active_mission_runtime_flags[0].is_active);
}

TEST_CASE("Tutorial 006a preserves its shipped lore fleet") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  constexpr std::int16_t kTutorial006aIndex = 755 - 0x80;
  REQUIRE(Mission_PopulateActiveSlot(state, kTutorial006aIndex, 0));
  CHECK(state.active_missions[0].current_system_id == 129 - 0x80);
  CHECK(state.active_missions[0].dude_def_index == 155 - 0x80);
}

TEST_CASE("concrete mission ShipSyst decodes as a system resource id") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.current_system_locator = 0x81;
  state.scenario.stellars.resize(2);
  state.scenario.stellars[1].system_id = 38;

  REQUIRE(Mission_PopulateActiveSlot(state, 0, 0));
  CHECK(state.active_missions[0].current_system_id == 1);
}

// Regression for Ghidra 0x004438d0
// Mission_ProcessInteractionReactionSlotResources: the TravelStel pickup arm is
// controlled by PickupMode, not DropOffMode. A mission that starts without
// cargo and only drops off at ReturnStel must not pick up cargo merely because
// DropOffMode is 1.
TEST_CASE("mission cargo pickup follows PickupMode") {
  GameState state;
  state.scenario.stellars.resize(1);
  state.scenario.ships.resize(1);
  state.scenario.ships[0].cargo_holds = 10;
  state.player.ship_class_id = 0;

  auto &mission = state.active_missions[0];
  mission.travel_stellar_id = 0;
  mission.cargo_qty_tons = 3;
  mission.pickup_mode = 0;
  mission.drop_off_mode = 1;
  auto &runtime = state.active_mission_runtime_flags[0];
  runtime.is_active = true;

  Mission_ProcessInteractionReactionSlotResources(state, 0, 0);

  CHECK_FALSE(mission.carrying_resources);
}

// Ui_LoadSelectionDialogResource + Stellar_BuildTravelDestinationDescription:
// a dësc id loads its body text and dialog variant and expands the mission
// wildcards against the given slot.
TEST_CASE("mission selection dialog text loads and expands a desc") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.active_mission_runtime_flags[0].is_active = true;

  const MissionDialogText message =
      Mission_LoadSelectionDialogText(state, 9200, false, 0);
  CHECK_FALSE(message.text.empty());
}

// Mission_HandleMissionOrSurrenderShipReaction FIRST-COMPLETION arm: the
// ShipDone desc (+0x43) is handed to the debrief sink on the transition.
TEST_CASE("mission ShipDone dialog reaches the debrief sink") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  auto &mission = state.active_missions[0];
  auto &runtime = state.active_mission_runtime_flags[0];
  runtime.is_active = true;
  mission.ship_goal = 0; // destroy
  mission.mission_target_count = 0;
  mission.brief_description_ids[7] = 9200;

  std::string text;
  int calls = 0;
  Mission_HandleMissionOrSurrenderShipReaction(
      state, 0, 0, [&](const MissionDialogText &message) {
        ++calls;
        text = message.text;
      });
  CHECK(calls == 1);
  CHECK_FALSE(text.empty());
}

TEST_CASE("landing reaction routes first ShipDone dialog to the debrief sink") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  auto &mission = state.active_missions[0];
  auto &runtime = state.active_mission_runtime_flags[0];
  runtime.is_active = true;
  mission.ship_goal = 0;
  mission.mission_target_count = 0;
  mission.return_stellar_id = 1;
  mission.brief_description_ids[7] = 9200;

  int calls = 0;
  Mission_TickReactionSlotsForTravelInteraction(
      state,
      /*landed_stellar_id=*/0x80,
      0,
      [&](const MissionDialogText &message) {
        ++calls;
        CHECK_FALSE(message.text.empty());
      });

  CHECK(runtime.objective_complete);
  CHECK(calls == 1);
}

// Mission_TryConsumeMissionInteractionResources denial arm: STR# 0x7d2
// 0x165 (total capacity short) is handed to the sink and the gate rejects.
TEST_CASE("mission cargo denial dialog reaches the debrief sink") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  std::string text;
  int calls = 0;
  const bool ok = Mission_TryConsumeMissionInteractionResources(
      state, 30000, [&](const MissionDialogText &message) {
        ++calls;
        text = message.text;
      });
  CHECK_FALSE(ok);
  REQUIRE(calls == 1);
  CHECK_FALSE(text.empty());
}

// Regression for Ghidra 0x0043f8c0 Mission_PopulateMissionSlotFromDef:
// special-ship spawn mode 1 starts with no completed target-goal count, even
// when the definition has a nonzero target-ship count.
TEST_CASE("special-ship mission starts with zero remaining goals") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.target_ship_count = 7;
  definition.ship_start = 1;

  REQUIRE(Mission_PopulateActiveSlot(state, 0, 0));

  const auto &mission = state.active_missions[0];
  CHECK(mission.target_ship_count == 7);
  CHECK(mission.mission_target_count == 7);
  CHECK(mission.goal_count_remaining == 0);
}

TEST_CASE("mission spawn refresh gives friendly escort arrivals short delay") {
  GameState state;
  state.player.current_system_id = 4;
  auto &mission = state.active_missions[0];
  state.active_mission_runtime_flags[0].is_active = true;
  mission.current_system_id = -6; // Bible ShipSyst: follow the player.
  mission.target_ship_count = 3;
  mission.ship_start = 1;
  mission.ship_goal = 3;     // Bible: escort them.
  mission.ship_behavior = 1; // Bible: protect the player.
  mission.goal_count_remaining = 2;
  mission.flags_primary = 0x10;
  mission.mission_ship_count_max = 5;
  mission.mission_ship_count_active = 1;
  mission.aux_ships_spawned = 4;

  Mission_RefreshActiveMissionSpawnState(state);

  CHECK(mission.spawn_rearm_timer == 30);
  CHECK(mission.goal_count_remaining == 0);
  CHECK(mission.mission_ship_count_active == 5);
  CHECK(mission.rearm_roll_clock >= 70);
  CHECK(mission.rearm_roll_clock <= 139);
  CHECK(mission.aux_ships_spawned == 0);

  // ShipBehav 0 is the hostile/pursuing case and retains the longer random
  // hyperspace-arrival delay.
  mission.ship_behavior = 0;
  Mission_RefreshActiveMissionSpawnState(state);
  CHECK(mission.spawn_rearm_timer >= 100);
  CHECK(mission.spawn_rearm_timer <= 199);
}

// Ghidra 0x00457580 Stellar_HandleStellarEntryAndExit restricted-travel slice:
// the follow-player rearm seeding forces ShipBehav 1 into the immediate
// restore path and stages ShipBehav 0 at 0x7fff (or 0 for the government's
// immediate gate-arrival arm).
TEST_CASE("restricted travel rearm seeds follow-player fleets") {
  GameState state;
  state.scenario.dudes.resize(1);
  state.scenario.dudes[0].government_id = 0;
  state.scenario.governments.resize(1);
  state.player.current_system_id = 4;

  auto &mission = state.active_missions[0];
  state.active_mission_runtime_flags[0].is_active = true;
  mission.current_system_id = -6;
  mission.target_ship_count = 3;
  mission.ship_start = 0;
  mission.aux_ship_system_locator = 5;
  mission.rearm_roll_clock = 100;

  // ShipBehav 1: forced immediate (spawn_rearm_timer = -1, goal = 0).
  mission.ship_behavior = 1;
  mission.spawn_rearm_timer = 77;
  mission.goal_count_remaining = 3;
  Mission_RearmFollowPlayerFleetsForRestrictedTravel(state,
                                                     /*hypergate=*/true);
  CHECK(mission.spawn_rearm_timer == -1);
  CHECK(mission.goal_count_remaining == 0);
  CHECK(mission.rearm_roll_clock == 100);

  // ShipBehav 0 on a hypergate whose government routes to the wormhole arm
  // (flags_secondary 0x20): no immediate rearm, staged at 0x7fff/goal=target,
  // and the -1 aux locator disables the aux top-up clock.
  mission.ship_behavior = 0;
  mission.dude_def_index = 0;
  mission.aux_ship_system_locator = -1;
  state.scenario.governments[0].flags_secondary = 0x20;
  Mission_RearmFollowPlayerFleetsForRestrictedTravel(state,
                                                     /*hypergate=*/true);
  CHECK(mission.spawn_rearm_timer == 0x7fff);
  CHECK(mission.goal_count_remaining == 3);
  CHECK(mission.rearm_roll_clock == 0x7fff);

  // The 0x80 government flag takes the wormhole immediate arm.
  state.scenario.governments[0].flags_secondary = 0x80;
  Mission_RearmFollowPlayerFleetsForRestrictedTravel(state,
                                                     /*hypergate=*/false);
  CHECK(mission.spawn_rearm_timer == 0);
  CHECK(mission.goal_count_remaining == 0);

  // A non-follow mission is untouched.
  mission.current_system_id = 4;
  mission.spawn_rearm_timer = 55;
  mission.goal_count_remaining = 7;
  Mission_RearmFollowPlayerFleetsForRestrictedTravel(state,
                                                     /*hypergate=*/true);
  CHECK(mission.spawn_rearm_timer == 55);
  CHECK(mission.goal_count_remaining == 7);
}

TEST_CASE("random mission locator -2 selects ordinary travel stellars") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.travel_stellar_locator = -2;
  definition.return_stellar_locator = -1;

  state.scenario.systems.resize(3);
  for (auto &system : state.scenario.systems) {
    system.is_visible = true;
    system.has_explored_flag = true;
  }
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.systems[1].nav_defs[0] = 0x81;
  state.scenario.systems[2].nav_defs[0] = 0x82;
  state.scenario.stellars.resize(3);
  for (auto &stellar : state.scenario.stellars) {
    stellar.is_available = true;
    stellar.is_defined = true;
    stellar.flags = 0x81; // travel target active, ordinary lane
    stellar.strength_capacity = 1;
    stellar.strength = 1;
    stellar.destroyed_days_remaining = 1;
  }
  state.scenario.stellars[0].system_id = 0;
  state.scenario.stellars[1].system_id = 1;
  state.scenario.stellars[2].system_id = 2;
  state.travel.selected_stellar_id = 0x80;

  Mission_ResolveMissionStellarLocators(state);
  CHECK(state.mission_target_resolutions[0].travel_stellar_id >= 1);
  CHECK(state.mission_target_resolutions[0].travel_stellar_id <= 2);
}

TEST_CASE("random mission locator -3 requires the 0x20 travel lane") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.travel_stellar_locator = -3;
  definition.return_stellar_locator = -1;

  state.scenario.systems.resize(2);
  for (auto &system : state.scenario.systems) {
    system.is_visible = true;
    system.has_explored_flag = true;
  }
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.systems[1].nav_defs[0] = 0x81;
  state.scenario.stellars.resize(2);
  for (auto &stellar : state.scenario.stellars) {
    stellar.is_available = true;
    stellar.is_defined = true;
    stellar.flags = 0xa1; // active + 0x20 mission travel lane
    stellar.strength_capacity = 1;
    stellar.strength = 1;
    stellar.destroyed_days_remaining = 1;
  }
  state.scenario.stellars[0].system_id = 0;
  state.scenario.stellars[1].system_id = 1;
  state.travel.selected_stellar_id = 0x80;

  Mission_ResolveMissionStellarLocators(state);
  CHECK(state.mission_target_resolutions[0].travel_stellar_id == 1);

  // 0x10 is excluded by the original -3 selection arm even when 0x20 is set.
  state.scenario.stellars[1].flags = 0xb1;
  Mission_ResolveMissionStellarLocators(state);
  CHECK(state.mission_target_resolutions[0].travel_stellar_id == -1);
}

TEST_CASE("mission random locator uses the current travel stellar as anchor") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.travel_stellar_locator = -2;
  definition.return_stellar_locator = -1;

  state.scenario.systems.resize(2);
  for (auto &system : state.scenario.systems) {
    system.is_visible = true;
    system.has_explored_flag = true;
  }
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.systems[1].nav_defs[0] = 0x81;
  state.scenario.stellars.resize(2);
  for (auto &stellar : state.scenario.stellars) {
    stellar.is_available = true;
    stellar.is_defined = true;
    stellar.flags = 0x81;
    stellar.strength_capacity = 1;
    stellar.strength = 1;
    stellar.destroyed_days_remaining = 1;
  }
  state.scenario.stellars[0].system_id = 0;
  state.scenario.stellars[1].system_id = 0;
  state.travel.selected_stellar_id = 0x80;

  // Both candidates are in the anchor's system; the reference plumbing must
  // reject them rather than using the chain-only destination arm.
  Mission_ResolveMissionStellarLocators(state);
  CHECK(state.mission_target_resolutions[0].travel_stellar_id == -1);
}

// Ghidra 0x0043d510 (disassembled at 0x0043da4c/0x0043db4d): the random
// allied-government stellar family compares the target government against
// g_system_defs indexed by the STELLAR slot (not the candidate's own system),
// excludes an exact stellar-government match, and ORs in the real alliance
// relation. A stellar can therefore qualify solely because the unrelated
// system at the same index is governed by the target.
TEST_CASE("stellar allied locator reads the same-index system government") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.travel_stellar_locator = 15005;
  definition.return_stellar_locator = -1;

  state.scenario.governments.resize(8);
  state.scenario.systems.resize(3);
  for (auto &system : state.scenario.systems) {
    system.is_visible = true;
    system.has_explored_flag = true;
  }
  state.scenario.systems[0].nav_defs[0] = 0x80; // reference system
  state.scenario.systems[1].nav_defs[0] = 0x81;
  state.scenario.systems[2].nav_defs[0] = 0x82;
  state.scenario.systems[0].government_id = 1;
  state.scenario.systems[1].government_id = 5; // same slot as stellar 1
  state.scenario.systems[2].government_id = 0;

  state.scenario.stellars.resize(3);
  for (auto &stellar : state.scenario.stellars) {
    stellar.is_available = true;
    stellar.is_defined = true;
    stellar.flags = 0x81;
    stellar.strength_capacity = 1;
    stellar.strength = 1;
    stellar.destroyed_days_remaining = 1;
  }
  state.scenario.stellars[0].system_id = 0;
  state.scenario.stellars[0].government_id = 1; // reference anchor
  state.scenario.stellars[1].system_id = 1;
  state.scenario.stellars[1].government_id = 7; // not allied to 5
  state.scenario.stellars[2].system_id = 2;
  state.scenario.stellars[2].government_id = 5; // exact, excluded
  state.travel.selected_stellar_id = 0x80;

  Mission_ResolveMissionStellarLocators(state);
  CHECK(state.mission_target_resolutions[0].travel_stellar_id == 1);
}

// Ghidra 0x0043d510: the 30000..30999 share-class stellar family guards its
// DoGovtsShareClass call with `wanted != govt`, so a stellar of the exact
// target government is not selectable by that id even though it trivially
// shares classes with itself.
TEST_CASE("stellar share-class locator excludes the exact government") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.travel_stellar_locator = 30005;
  definition.return_stellar_locator = -1;

  state.scenario.governments.resize(8);
  state.scenario.governments[5].classes[0] = 2;
  state.scenario.governments[6].classes[0] = 2;
  state.scenario.systems.resize(3);
  for (auto &system : state.scenario.systems) {
    system.is_visible = true;
    system.has_explored_flag = true;
  }
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.systems[1].nav_defs[0] = 0x81;
  state.scenario.systems[2].nav_defs[0] = 0x82;

  state.scenario.stellars.resize(3);
  for (auto &stellar : state.scenario.stellars) {
    stellar.is_available = true;
    stellar.is_defined = true;
    stellar.flags = 0x81;
    stellar.strength_capacity = 1;
    stellar.strength = 1;
    stellar.destroyed_days_remaining = 1;
  }
  state.scenario.stellars[0].system_id = 0;
  state.scenario.stellars[0].government_id = 1;
  state.scenario.stellars[1].system_id = 1;
  state.scenario.stellars[1].government_id = 5; // exact, excluded
  state.scenario.stellars[2].system_id = 2;
  state.scenario.stellars[2].government_id = 6; // shares class 2
  state.travel.selected_stellar_id = 0x80;

  Mission_ResolveMissionStellarLocators(state);
  CHECK(state.mission_target_resolutions[0].travel_stellar_id == 2);
}

// Ghidra 0x0043e6f0: the system allied family's rejection-sampling loop guards
// its Government_AreGovtsAllied call with `wanted != govt`, so the exact target
// government is not selectable. A lone exact-match system leaves the family
// with no candidate and the populate arm stores -1.
TEST_CASE("system allied locator excludes an exact government twin") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.current_system_locator = 15005;
  definition.travel_stellar_locator = -1;
  definition.return_stellar_locator = -1;

  state.scenario.governments.resize(8);
  state.scenario.systems.resize(2);
  state.scenario.systems[0].is_visible = true;
  state.scenario.systems[0].government_id = 1;
  state.scenario.systems[1].is_visible = true;
  state.scenario.systems[1].government_id = 5; // exact match only
  state.player.current_system_id = 0;

  REQUIRE(Mission_ActivateAtSlot(state, 0));
  CHECK(state.active_missions[0].current_system_id == -1);
}

// Regression for Ghidra 0x00448670 Mission_RunAvailLocOffers:
// an ordinary decline removes the offer from the current interaction walk and
// must not immediately present the same definition again in that context.
TEST_CASE("declined mission offer is suppressed for the current context") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.avail_location = 3;
  definition.avail_random = 100;

  int offer_count = 0;
  std::int16_t offered_definition = -1;
  const auto decline = [&](std::int16_t mission_def) {
    ++offer_count;
    offered_definition = mission_def;
    return MissionOfferResult::kDeclined;
  };

  CHECK(Mission_RunAvailLocOffers(state, 3, 100, decline));
  CHECK(offer_count == 1);
  CHECK(offered_definition == 0);
  CHECK_FALSE(Mission_RunAvailLocOffers(state, 3, 200, decline));
  CHECK(offer_count == 1);
}

// Regression for the Trade Center arm of Ghidra 0x00448670: this uses its
// own AvailLoc 4 lane, so tutorial follow-ups must not be limited to the
// Spaceport's AvailLoc 3 pass.
TEST_CASE("trade center mission interaction offers the AvailLoc 4 lane") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.avail_location = 4;
  definition.avail_random = 100;

  std::int16_t offered_definition = -1;
  const auto decline = [&](std::int16_t mission_def) {
    offered_definition = mission_def;
    return MissionOfferResult::kDeclined;
  };

  CHECK(Mission_RunAvailLocOffers(state, 4, 100, decline));
  CHECK(offered_definition == 0);
}

// Ghidra 0x00492f30 / 0x0048ea70 enter the normal Shipyard and Outfitter
// modals in their respective mission-offer contexts 5 and 6.
TEST_CASE("shipyard and outfitter mission interactions use their own lanes") {
  GameState state;
  state.scenario.missions.resize(2);
  for (std::size_t i = 0; i < state.scenario.missions.size(); ++i) {
    state.scenario.missions[i].present = true;
    state.scenario.missions[i].avail_location =
        static_cast<std::int16_t>(i + 5);
    state.scenario.missions[i].avail_random = 100;
  }

  std::vector<std::int16_t> offered;
  const auto decline = [&](std::int16_t mission_def) {
    offered.push_back(mission_def);
    return MissionOfferResult::kDeclined;
  };

  CHECK(Mission_RunAvailLocOffers(state, 5, 100, decline));
  CHECK(Mission_RunAvailLocOffers(state, 6, 200, decline));
  CHECK(offered == std::vector<std::int16_t>{0, 1});
}

// Regression for the post-accept arm of Ghidra 0x00454910:
// Flags 0x40 replaces a linked single-ship mission personality with a fresh
// mission ship of the same class, preserving the hailed ship's kinematics.
TEST_CASE("accepted single-ship mission replaces hailed personality ship") {
  GameState state;
  state.scenario.ships.resize(3);
  state.scenario.ships[1].base_shield = 20;
  state.scenario.ships[1].base_armor = 40;
  state.scenario.dudes.resize(1);
  state.scenario.dudes[0].ai_type = 1;
  state.scenario.dudes[0].ship_types[0] = 1;
  state.scenario.dudes[0].ship_probabilities[0] = 1;
  state.scenario.pers_defs.resize(1);
  state.scenario.pers_defs[0].alive = true;
  state.scenario.pers_defs[0].link_mission_id = 0;
  state.scenario.pers_defs[0].flags_primary = 0x0940;

  state.player.current_system_id = 7;
  state.player.primary_target_ship_slot = 1;
  Ship &target = state.ShipAt(1);
  target.is_active = true;
  target.ship_instance_id = 1;
  target.current_system_id = 7;
  target.ship_class_id = 1;
  target.pers_def_slot = 0;
  target.pos_x = 123.0F;
  target.pos_y = -45.0F;
  target.vel_x = 2.5F;
  target.vel_y = -4.0F;
  target.heading = 0.75F;

  ActiveMission &mission = state.active_missions[0];
  mission.mission_template_id = 0;
  mission.target_ship_count = 1;
  mission.dude_def_index = 0;
  mission.ship_goal = 3;
  state.active_mission_runtime_flags[0].is_active = true;

  REQUIRE(Mission_HandleAcceptedShipInteraction(state, 1, 1234));
  CHECK_FALSE(target.is_active);
  CHECK_FALSE(state.scenario.pers_defs[0].alive);
  CHECK(state.player.primary_target_ship_slot == 2);
  const Ship &replacement = state.ShipAt(2);
  CHECK(replacement.is_active);
  CHECK(replacement.mission_fleet_slot == 0);
  CHECK(replacement.ship_class_id == 1);
  CHECK(replacement.pos_x == 123.0F);
  CHECK(replacement.pos_y == -45.0F);
  CHECK(replacement.vel_x == 2.5F);
  CHECK(replacement.vel_y == -4.0F);
  CHECK(replacement.heading == 0.75F);
  CHECK(target.ai_state_code == 2);
  CHECK(target.primary_target_ship_slot == -1);
  CHECK(state.ship_reticle_pulse == 0.0F);
}

// Regression for Ghidra 0x00441b40
// Mission_CheckMissionShipInteractionEligibility: Flags 0x0008 uses
// FLOAT_00575510 (100.0), and the original comparison is strict: exactly one
// jump of fuel passes while any lesser amount fails.
TEST_CASE("mission fuel gate requires one jump of fuel") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.is_available_runtime = true;
  definition.avail_location = 0;
  definition.avail_random = 100;
  definition.flags_primary = 0x0008;

  state.player.fuel_points = kJumpFuelCost - 0.01F;
  CHECK_FALSE(Mission_CheckMissionShipInteractionEligibility(
      state, 0, /*interaction_context=*/false, /*recompute_reaction=*/false));

  state.player.fuel_points = kJumpFuelCost;
  CHECK(Mission_CheckMissionShipInteractionEligibility(
      state, 0, /*interaction_context=*/false, /*recompute_reaction=*/false));
}

// Regression for the original's param_2 (Ghidra 0x00442310):
// Mission_CheckMissionShipInteractionEligibility with recompute_reaction set
// re-evaluates the definition's availability expression and caches it at
// MisnDef +0x16, so a stale cache is corrected by the call and then read by
// gate [1]. The hail ladder passes false and keeps the cached value.
TEST_CASE("mission eligibility recompute refreshes the availability cache") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.avail_location = 0;
  definition.avail_random = 100;
  definition.availability_expr = "b1";
  definition.is_available_runtime = false; // stale

  state.control.bits.set(1); // satisfy "b1"

  CHECK_FALSE(Mission_CheckMissionShipInteractionEligibility(
      state, 0, /*interaction_context=*/false, /*recompute_reaction=*/false));
  CHECK_FALSE(definition.is_available_runtime);

  CHECK(Mission_CheckMissionShipInteractionEligibility(
      state, 0, /*interaction_context=*/false, /*recompute_reaction=*/true));
  CHECK(definition.is_available_runtime);
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

TEST_CASE("mission wildcard expansion resolves destinations and identity") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.scenario.missions.resize(1);
  auto &def = state.scenario.missions[0];
  def.present = true;
  def.resource_delta_or_cost = 5000;
  def.travel_stellar_locator = 0x80;
  def.return_stellar_locator = -1;
  def.cargo_type_resource = 6; // *passengers
  def.cargo_qty_tons = 3;
  state.scenario.stellars.resize(2);
  state.scenario.stellars[0].name = "Earth";
  state.scenario.stellars[0].system_id = 0;
  state.scenario.stellars[1].name = "Tichel II";
  state.scenario.stellars[1].system_id = 1;
  state.scenario.systems.resize(2);
  state.scenario.systems[0].name = "Sol";
  state.scenario.systems[1].name = "Tichel";
  state.pilot.first_name = "Jane Trader";
  state.player.ship_name = "Kestrel";
  state.player.ship_class_id = 0;
  if (state.scenario.Ship(0x80) != nullptr) {
    // ship type name comes from the shp resource name
  }
  Mission_ResolveMissionStellarLocators(state);

  const std::string expanded = Mission_ExpandMissionWildcards(
      state,
      "Ferry <CQ> tons of <CT> from <RST> to <DST> in <DSY> for <PAY> cr, "
      "<PN> of the <PSN> (<PST>). Deadline <DL>, signed <OSN>/<SN>/<REG>",
      true,
      0);
  CHECK(expanded.find("Ferry 3 tons of passengers") != std::string::npos);
  CHECK(expanded.find("to Earth in Sol") != std::string::npos);
  CHECK(expanded.find("for 5,000 cr") != std::string::npos);
  CHECK(expanded.find("Jane Trader of the Kestrel") != std::string::npos);
  CHECK(expanded.find("<DST>") == std::string::npos);
  CHECK(expanded.find("<CT>") == std::string::npos);
  // No TimeLimit on the definition: the target block's deadline stays zeroed
  // (0x0043d240 leaves the fields untouched for steps < 1) and the offer-row
  // <DL> arm formats that zero date -- the original's " 0th, 0" garbage. The
  // "[Error]" init is only kept when the stored deadline equals today.
  CHECK(expanded.find("Deadline  0th, 0") != std::string::npos);
  CHECK(expanded.find("signed [Error]/[Error]/EV Nova Community") !=
        std::string::npos);
  // <PST> comes from the ship class display name.
  INFO("expanded: " << expanded);
  CHECK(expanded.find("(<") == std::string::npos);
}

TEST_CASE("per-government rank tokens resolve by government id") {
  GameState state;
  state.scenario.ranks.assign(0x80, {});
  auto &ranks = state.scenario.ranks;
  ranks[0].defined = true;
  ranks[0].active = true;
  ranks[0].government_id = 0;
  ranks[0].weight = 1;
  ranks[0].conv_name = "Commander";
  ranks[0].short_name = "Cdr";
  ranks[1].defined = true;
  ranks[1].active = true;
  ranks[1].government_id = 3;
  ranks[1].weight = 5;
  ranks[1].conv_name = "Ambassador";
  ranks[1].short_name = "Amb";

  const auto expand = [&state](std::string_view text) {
    return Mission_ExpandMissionWildcards(state, text, false, -1);
  };
  // nnn is the 0x80-based government resource id (128 -> government 0).
  CHECK(expand("<PRK128>") == "Commander");
  CHECK(expand("<SRK128>") == "Cdr");
  CHECK(expand("<PRK131>") == "Ambassador");
  CHECK(expand("<SRK131>") == "Amb");
  // Not crossed: each token uses its own government.
  CHECK(expand("<PRK128> and <SRK131>") == "Commander and Amb");
  // Shipped-text shape: the Federation <PRK128> briefing must read the real
  // rank name, not the "captain" fallback.
  const std::string briefing =
      expand("given the diplomatic rank of '<PRK128>', with all ...");
  CHECK(briefing.find("'Commander'") != std::string::npos);
  // Valid id with no matching rank -> the STR# 0x7d2 0x155 "captain" entry.
  CHECK(expand("<PRK135>") == "captain");
  // Out-of-range ids and malformed tokens are left untouched.
  CHECK(expand("<PRK999>") == "<PRK999>");
  CHECK(expand("<PRK5>") == "<PRK5>");
  CHECK(expand("<PRK>") == "Ambassador");
}

TEST_CASE("mission pay wildcard handles the cost and percentage encodings") {
  GameState state;
  state.player.credits = 10000;
  auto expand = [&state](std::int32_t pay) {
    GameState probe = state;
    probe.scenario.missions.resize(1);
    probe.scenario.missions[0].present = true;
    probe.scenario.missions[0].resource_delta_or_cost = pay;
    probe.scenario.stellars.resize(1);
    Mission_ResolveMissionStellarLocators(probe);
    return Mission_ExpandMissionWildcards(probe, "<PAY>", true, 0);
  };
  CHECK(expand(5000) == "5,000");
  CHECK(expand(1234567) == "1.23M");
  CHECK(expand(-55000) == "5,000");  // acceptance cost encoding
  CHECK(expand(-40005) == "500");    // 5% of 10000 credits
  CHECK(expand(-60000) == "10,000"); // acceptance cost encoding
  CHECK(expand(0) == "0");
}

TEST_CASE("unresolvable offer destinations fall back to the return target") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.scenario.missions.resize(1);
  state.scenario.missions[0].present = true;
  // TravelStel unresolvable (no stellar), ReturnStel = stellar 0.
  state.scenario.missions[0].travel_stellar_locator = 0x87f;
  state.scenario.missions[0].return_stellar_locator = 0x80;
  state.scenario.stellars.resize(1);
  state.scenario.stellars[0].name = "Earth";
  state.scenario.stellars[0].system_id = 0;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].name = "Sol";
  Mission_ResolveMissionStellarLocators(state);
  const std::string expanded = Mission_ExpandMissionWildcards(
      state, "Deliver to <DST> in <DSY>", true, 0);
  CHECK(expanded == "Deliver to Earth in Sol");
}

TEST_CASE("string placeholder expansion handles gender blocks and quirks") {
  GameState state; // pilot defaults: male ('m' latch = g_player_is_male)

  std::string male = R"(a {G"m'boy" "lass"} b)";
  Mission_ExpandStringPlaceholders(state, male);
  CHECK(male == "a m'boy b");

  state.control.male = false;
  std::string female = R"(a {G"m'boy" "lass"} b)";
  Mission_ExpandStringPlaceholders(state, female);
  CHECK(female == "a lass b");

  // '!' negator swaps the arms.
  state.control.male = true;
  std::string negated = R"({!G"first" "second"})";
  Mission_ExpandStringPlaceholders(state, negated);
  CHECK(negated == "second");

  // Backslash escapes protect quotes inside an arm.
  std::string escaped = R"({G"a\"b" "c"})";
  Mission_ExpandStringPlaceholders(state, escaped);
  CHECK(escaped == "a\"b");

  // Literal text without braces passes through untouched.
  std::string plain = "no blocks here";
  Mission_ExpandStringPlaceholders(state, plain);
  CHECK(plain == "no blocks here");

  // Quirk: a '{' header with no g/G/p/P/b/B/'!' swallows the rest of the text
  // (the original's header state has no terminator branch).
  std::string swallowed = "keep {x drop";
  Mission_ExpandStringPlaceholders(state, swallowed);
  CHECK(swallowed == "keep ");
}

TEST_CASE("string placeholder expansion tests registration and control bits") {
  GameState state; // registered by default (the port models a licensed game)

  // {P30}: the registration conditional. Registered -> first arm; the trailing
  // day count is only consulted while unregistered (ncb Pxxx semantics).
  std::string registered =
      R"({P30"License" "License, REQUIRES YOU TO REGISTER"})";
  Mission_ExpandStringPlaceholders(state, registered);
  CHECK(registered == "License");

  state.control.registered = false;
  std::string unregistered =
      R"({P30"License" "License, REQUIRES YOU TO REGISTER"})";
  Mission_ExpandStringPlaceholders(state, unregistered);
  CHECK(unregistered == "License, REQUIRES YOU TO REGISTER");
  state.control.registered = true;

  // {bN}: the Nova control bit test reads control.bits[N]; a clear bit takes
  // the second arm, and an unknown/out-of-range bit is false.
  state.control.bits.set(424);
  std::string set_bit = R"({b424"on" "off"})";
  Mission_ExpandStringPlaceholders(state, set_bit);
  CHECK(set_bit == "on");

  std::string clear_bit = R"({b425"on" "off"})";
  Mission_ExpandStringPlaceholders(state, clear_bit);
  CHECK(clear_bit == "off");

  std::string negated_bit = R"({!b424"on" "off"})";
  Mission_ExpandStringPlaceholders(state, negated_bit);
  CHECK(negated_bit == "off");
}

// Regression test for the Sol tutorial (mïsn resource 251, "Head to Sol;
// Tutorial 001"): the on-accept payload (b8339 X130) must reveal Sol, and
// landing on Earth (spob 128) must resolve the mission through the debrief
// sink and set the on-success bit 9200 that gates Tutorial 002. The landing
// gate receives the docked stellar as a 0x80-based resource id; the resolved
// ReturnStel is a 0-based index — this flow broke when the two conventions
// were compared directly.
TEST_CASE("tutorial 001 reveals Sol on accept and completes on landing") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const std::size_t tutorial_001 = 251 - 0x80;
  const std::size_t tutorial_002 = 630 - 0x80;
  REQUIRE(tutorial_001 < state.scenario.missions.size());
  REQUIRE(state.scenario.missions[tutorial_001].present);
  REQUIRE(state.scenario.missions[tutorial_002].present);

  Mission_ResolveMissionStellarLocators(state);
  REQUIRE(
      Mission_ActivateAtSlot(state, static_cast<std::int16_t>(tutorial_001)));

  // On-accept payload: bit 8339 set, Sol (system resource 130, def 2)
  // explored.
  CHECK(state.control.ControlBit(8339));
  CHECK(state.scenario.systems[2].discovery_state > 0);

  // Tutorial 002's AvailBits (b9200 & !(b9201 | b9215)) must still fail
  // before completion.
  CHECK_FALSE(state.scenario.missions[tutorial_002].is_available_runtime);

  // Land on Earth (spob resource 128). The gate must resolve the mission as a
  // success and hand the Comp dësc (9200) text to the debrief sink.
  std::string debrief_text;
  int debrief_calls = 0;
  Mission_TickReactionSlotsForTravelInteraction(
      state,
      /*landed_stellar_id=*/0x80,
      /*now_ms=*/0,
      [&](const MissionDialogText &message) {
        ++debrief_calls;
        debrief_text = message.text;
      });
  CHECK(debrief_calls == 1);
  CHECK_FALSE(debrief_text.empty());

  bool still_active = false;
  for (std::size_t slot = 0; slot < state.active_missions.size(); ++slot) {
    if (state.active_mission_runtime_flags[slot].is_active &&
        state.active_missions[slot].mission_template_id ==
            static_cast<std::int16_t>(tutorial_001)) {
      still_active = true;
    }
  }
  CHECK_FALSE(still_active);

  // On-success payload: bit 9200 set, which unlocks Tutorial 002's
  // availability expression.
  CHECK(state.control.ControlBit(9200));

  // Re-run the availability pass: Tutorial 002's expression now passes.
  Mission_ResolveMissionStellarLocators(state);
  const auto evaluation = Mission_EvaluateMissionLists(state);
  CHECK(state.scenario.missions[tutorial_002].is_available_runtime);
  (void)evaluation;
}

TEST_CASE("crön events arm on the daily tick, contribute, and retire") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  // The original keeps 0x200 crön slots indexed by resource id - 0x80.
  REQUIRE(state.scenario.cron_events.size() == 0x200);

  // Synthetic event: eligible immediately, two active days, observable
  // OnStart/OnEnd (control bit 300 set on start, cleared on end), and a
  // Contribute bit the Require gate can verify.
  auto &def = state.scenario.cron_events[0];
  def = {};
  def.present = true;
  def.duration = 2;
  def.trigger_odds = 100; // rand(101) <= 100: activates on the first tick
  def.on_start = "b300";
  def.on_end = "!b300";
  def.contribute_lo = 1U << 5;
  // (Require left empty: the gate tests the PLAYER's aggregate mask, and the
  // event's own contribute only counts while it is active.)

  // Before the first tick the event is idle and contributes nothing.
  CHECK_FALSE(NovaOutfit_EvaluateRequireMask(state, 1U << 5, 0U));

  // Day 1: armed, OnStart ran, past the (zero) pre-holdoff -> contributes.
  Mission_TickDailyCronEvents(state);
  CHECK(state.cron_event_states[0].is_active);
  CHECK(state.cron_event_states[0].duration_counter == 2);
  CHECK(state.control.ControlBit(300));
  CHECK(NovaOutfit_EvaluateRequireMask(state, 1U << 5, 0U));

  // Day 2: duration counts down, still active.
  Mission_TickDailyCronEvents(state);
  CHECK(state.cron_event_states[0].is_active);
  CHECK(state.cron_event_states[0].duration_counter == 1);

  // Day 3: duration expires -> OnEnd ran, slot deactivates, contribute drops.
  Mission_TickDailyCronEvents(state);
  CHECK_FALSE(state.cron_event_states[0].is_active);
  CHECK_FALSE(state.control.ControlBit(300));
  CHECK_FALSE(NovaOutfit_EvaluateRequireMask(state, 1U << 5, 0U));
}

TEST_CASE("crön date window and post-holdoff gate the daily tick") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  auto &def = state.scenario.cron_events[1];
  def = {};
  def.present = true;
  def.duration = 1;
  def.trigger_odds = 100;
  def.on_start = "b301";
  // A FirstYear in the future keeps the event idle even at odds 100.
  def.first_year = 2999;
  Mission_TickDailyCronEvents(state);
  CHECK_FALSE(state.cron_event_states[1].is_active);
  CHECK_FALSE(state.control.ControlBit(301));

  // Zero-duration event with a post-holdoff: start+end fire on the same day
  // and the slot stays busy (holding off) before deactivating.
  def.first_year = -1;
  def.duration = 0;
  def.post_holdoff = 2;
  def.on_start = "b301";
  def.on_end = "!b301";
  Mission_TickDailyCronEvents(state);
  CHECK(state.cron_event_states[1].is_active);
  CHECK(state.cron_event_states[1].duration_counter == -1);
  CHECK_FALSE(state.control.ControlBit(301)); // OnEnd already ran
  CHECK(state.cron_event_states[1].holdoff_counter == 2);
  Mission_TickDailyCronEvents(state);
  CHECK(state.cron_event_states[1].is_active); // waiting out the holdoff
  Mission_TickDailyCronEvents(state);
  CHECK_FALSE(state.cron_event_states[1].is_active);
}

TEST_CASE("crön month-only start gate opens no earlier than FirstMonth") {
  GameState state;
  state.scenario.cron_events.resize(4);
  auto &def = state.scenario.cron_events[2];
  def = {};
  def.present = true;
  def.duration = 1;
  def.trigger_odds = 100;
  def.first_month = 3; // FirstDay 0: the month alone gates the window.
  def.first_day = 0;

  // Before March the window is closed (0x0046c835: month < FirstMonth).
  state.date.year = 1180;
  state.date.month = 2;
  state.date.day = 15;
  Mission_TickDailyCronEvents(state);
  CHECK_FALSE(state.cron_event_states[2].is_active);

  // March onwards it is open (0x0046c835: month >= FirstMonth).
  state.date.month = 4;
  Mission_TickDailyCronEvents(state);
  CHECK(state.cron_event_states[2].is_active);
}

TEST_CASE("crön post-end wait follows BugFixPolicy") {
  GameState state;
  state.scenario.cron_events.resize(4);
  auto &def = state.scenario.cron_events[3];
  def = {};
  def.present = true;
  def.duration = 1;
  def.trigger_odds = 100;
  def.pre_holdoff = 3;
  def.post_holdoff = 1;
  def.on_start = "b303";
  def.on_end = "!b303";

  Mission_TickDailyCronEvents(state); // arm; wait the PreHoldoff
  CHECK(state.cron_event_states[3].holdoff_counter == 3);
  Mission_TickDailyCronEvents(state); // 3 -> 2
  Mission_TickDailyCronEvents(state); // 2 -> 1
  Mission_TickDailyCronEvents(state); // 1 -> 0: OnStart
  CHECK(state.control.ControlBit(303));
  Mission_TickDailyCronEvents(state); // duration expires: OnEnd, re-arm
  CHECK_FALSE(state.control.ControlBit(303));
  // Original 0x004395d9 reloads PreHoldoff here; the Bible documents
  // PostHoldoff, which the port applies under BugFixPolicy.
  CHECK(state.cron_event_states[3].holdoff_counter ==
        (state.bugfixes.cron_events ? def.post_holdoff : def.pre_holdoff));
}

TEST_CASE("crön multi-year date range is contiguous under "
          "BugFixPolicy") {
  GameState state;
  state.scenario.cron_events.resize(4);
  auto &def = state.scenario.cron_events[2];
  def = {};
  def.present = true;
  def.duration = 1;
  def.trigger_odds = 100;
  def.first_day = 1;
  def.first_month = 1;
  def.first_year = 1178;
  def.last_day = 1;
  def.last_month = 1;
  def.last_year = 1179;

  const auto activates_on =
      [&](std::int16_t day, std::int16_t month, std::int16_t year) {
        state.date.day = day;
        state.date.month = month;
        state.date.year = year;
        state.cron_event_states[2] = {};
        Mission_TickDailyCronEvents(state);
        return state.cron_event_states[2].is_active;
      };

  CHECK(activates_on(1, 1, 1178));
  CHECK(activates_on(1, 1, 1179));
  CHECK_FALSE(activates_on(31, 12, 1177));
  CHECK_FALSE(activates_on(2, 1, 1179));
  // The original collapses the range to 1 January (month*0x20+day equal at
  // both ends); the fix keeps it open through 1178 and up to 1/1/1179.
  CHECK(activates_on(15, 6, 1178) == state.bugfixes.cron_events);
  CHECK_FALSE(activates_on(15, 6, 1179));
}

TEST_CASE("crön Random 0 never activates under BugFixPolicy") {
  GameState state;
  state.scenario.cron_events.resize(4);
  auto &def = state.scenario.cron_events[2];
  def = {};
  def.present = true;
  def.duration = 1;
  def.trigger_odds = 0;

  // With the original 0..100 roll, roll 0 matches Random 0 (~1/101 per
  // eligible day). The fix rolls 1..100, so 0 never fires.
  for (int day = 0; day < 400 && state.bugfixes.cron_events; ++day) {
    state.date.day = static_cast<std::int16_t>(1 + day % 28);
    Mission_TickDailyCronEvents(state);
    REQUIRE_FALSE(state.cron_event_states[2].is_active);
  }
}

TEST_CASE("crön duration-0 event runs OnEnd once under "
          "BugFixPolicy") {
  GameState state;
  state.scenario.cron_events.resize(4);
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].max_count = 10; // stackable grant target
  auto &def = state.scenario.cron_events[2];
  def = {};
  def.present = true;
  def.duration = 0;
  def.trigger_odds = 100;
  def.on_end = "G128"; // grant outfit 0x80: owned count +1 per OnEnd run

  Mission_TickDailyCronEvents(state);
  CHECK(state.inventory.outfit_owned_count[0] == 1);

  // Block re-arming so the second tick only exercises the lingering slot.
  def.trigger_odds = 0;
  Mission_TickDailyCronEvents(state);
  CHECK(state.inventory.outfit_owned_count[0] ==
        (state.bugfixes.cron_events ? 1 : 2));
}

TEST_CASE("disaster slots roll their per-day chance and count down",
          "[scenario][disaster]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(state.scenario.disaster_defs.size() == 0x100);

  // Synthetic record bound to stellar 0x89, guaranteed to activate.
  auto &def = state.scenario.disaster_defs[0];
  def = {};
  def.present = true;
  def.target_stellar = 0x89;
  def.duration_days = 30;
  def.start_chance_percent = 100; // roll+1 <= 100 always holds

  System_UpdateDisasterStates(state);
  CHECK(def.active_stellar == 0x89 - 0x80); // rebased to the 0-based index
  CHECK(def.days_remaining == 30);

  // Active: the slot is now past the roll branch and burns one day per sweep,
  // keeping the originally chosen stellar.
  System_UpdateDisasterStates(state);
  CHECK(def.days_remaining == 29);
  CHECK(def.active_stellar == 0x09);

  // Zero chance never activates.
  auto &never = state.scenario.disaster_defs[1];
  never = {};
  never.present = true;
  never.target_stellar = 0x89;
  never.duration_days = 5;
  never.start_chance_percent = 0;
  System_UpdateDisasterStates(state);
  CHECK(never.active_stellar == -1);
  CHECK(never.days_remaining == -1);

  // The ActivateOn expression gates the activation; "b300" is false until the
  // control bit is set. Because the failed roll leaves the slot idle and
  // not-started, a later sweep retries it.
  auto &gated = state.scenario.disaster_defs[2];
  gated = {};
  gated.present = true;
  gated.target_stellar = 0x89;
  gated.duration_days = 5;
  gated.start_chance_percent = 100;
  gated.activation_expression = "b300";
  System_UpdateDisasterStates(state);
  CHECK(gated.active_stellar == -1);
  state.control.SetControlBit(300, true);
  System_UpdateDisasterStates(state);
  CHECK(gated.active_stellar == 0x09);
  CHECK(gated.days_remaining == 5);

  // Undefined slots are reset to the idle sentinels.
  for (auto &slot : state.scenario.disaster_defs) {
    slot.present = false;
  }
  System_UpdateDisasterStates(state);
  CHECK(def.active_stellar == -1);
  CHECK(def.days_remaining == -1);
  CHECK_FALSE(def.started_once);
}

TEST_CASE("an Any-target disaster picks an available non-travel stellar",
          "[scenario][disaster]") {
  GameState state;
  state.scenario.stellars.resize(4);
  state.scenario.stellars[0].is_available = true;
  state.scenario.stellars[1].is_available = false;
  state.scenario.stellars[2].is_available = true;
  state.scenario.stellars[2].flags = 0x20U; // travel-only lane: not eligible
  state.scenario.stellars[3].is_available = true;

  state.scenario.disaster_defs.assign(1, {});
  auto &def = state.scenario.disaster_defs[0];
  def.present = true;
  def.target_stellar = -1; // "any"
  def.duration_days = 7;
  def.start_chance_percent = 100;

  System_UpdateDisasterStates(state);
  REQUIRE(def.days_remaining == 7);
  // Only slots 0 and 3 are available and non-travel; the original rejects
  // samples over the full table.
  CHECK((def.active_stellar == 0 || def.active_stellar == 3));
}

// Ghidra 0x0043bbb0 NovaResources_LoadMisnResourceDefs runtime half: the
// loader clears the cross-cutting mission interaction latches at
// session/reload time. Without this a second new game in one session would
// inherit the previous pilot's shown/context/speaker state (Game_ResetNewGame-
// State 0x004b4690 does not touch them).
TEST_CASE("mission loader reset clears interaction latches") {
  GameState state;
  state.mission_interaction_shown[0] = 1;
  state.mission_interaction_shown[3] = 1;
  state.mission_interaction_context = 3;
  state.mission_speaker_ship_slot = 7;
  state.script_mission_context_slot = 2;
  state.in_flight = true;
  state.active_mission_runtime_flags[1].is_active = true;
  state.active_mission_runtime_flags[1].is_failed = true;
  state.control.SetControlBit(311, true);

  Mission_ResetRuntimeStateOnMissionDefsLoad(state);

  CHECK(state.mission_interaction_shown[0] == 0);
  CHECK(state.mission_interaction_shown[3] == 0);
  CHECK(state.mission_interaction_context == -1);
  CHECK(state.mission_speaker_ship_slot == -1);
  CHECK(state.script_mission_context_slot == -1);
  CHECK_FALSE(state.in_flight);
  CHECK_FALSE(state.active_mission_runtime_flags[1].is_active);
  CHECK_FALSE(state.active_mission_runtime_flags[1].is_failed);
  CHECK_FALSE(state.control.ControlBit(311));
}

// BUGFIX(original): Mission_ResolveMisnSlot (0x00447d90) never ran the
// CompGovt/CompReward walk. Under BugFixPolicy it applies the
// success walk when the mission sets Flags2 0x0002 (pay on auto-abort), or the
// manual-abort Flags 0x0040 -5x reversal
// (NovaUi_RunMissionComputerWindow 0x00446150) when that flag is set. With the
// policy off, the original omission is reproduced. See
// docs/known_original_bugs.md.
TEST_CASE("auto-abort applies the competing-government reward under the fix "
          "policy") {
  const auto seed = [](GameState &state,
                       std::int16_t comp_govt,
                       std::int16_t comp_reward,
                       std::uint16_t flags_primary,
                       std::uint16_t flags_secondary) {
    state.scenario.systems.resize(2);
    state.scenario.systems[0].government_id = comp_govt;
    state.scenario.systems[1].government_id = -1; // independent
    state.system_reputation.assign(2, 0);
    auto &mission = state.active_missions[0];
    mission.comp_govt_id = comp_govt;
    mission.comp_reward_delta = comp_reward;
    mission.flags_primary = flags_primary;
    mission.flags_secondary = flags_secondary;
    state.active_mission_runtime_flags[0].is_active = true;
  };

  // Flags2 0x0002 (pay on auto-abort): the full success delta lands on the
  // competing government's systems.
  {
    GameState state;
    seed(state,
         /*comp_govt=*/0,
         /*comp_reward=*/10,
         /*flags_primary=*/0x0001,
         /*flags_secondary=*/0x0002);
    Mission_ResolveMisnSlot(state, 0, 0);
    CHECK(state.system_reputation[0] == (state.bugfixes.safe ? 10 : 0));
    CHECK(state.system_reputation[1] == 0);
  }

  // Flags 0x0040 alone (the stock Avoid case): the -5x reversal applies to
  // exact-government systems only.
  {
    GameState state;
    seed(state,
         /*comp_govt=*/0,
         /*comp_reward=*/10,
         /*flags_primary=*/0x0041,
         /*flags_secondary=*/0x0000);
    Mission_ResolveMisnSlot(state, 0, 0);
    CHECK(state.system_reputation[0] == (state.bugfixes.safe ? -50 : 0));
    CHECK(state.system_reputation[1] == 0);
  }

  // Neither flag: the reward stays inert.
  {
    GameState state;
    seed(state,
         /*comp_govt=*/0,
         /*comp_reward=*/10,
         /*flags_primary=*/0x0001,
         /*flags_secondary=*/0x0000);
    Mission_ResolveMisnSlot(state, 0, 0);
    CHECK(state.system_reputation[0] == 0);
    CHECK(state.system_reputation[1] == 0);
  }
}

// Ghidra 0x004aa980 Mission_RebuildMissionTargetSystemList: the starmap emits
// the TravelStel system until the TravelStel leg is satisfied, then the
// ReturnStel system REPLACES it (a single sVar2, not both). An unavailable
// ReturnStel falls back to keeping the TravelStel system.
TEST_CASE("travel_stellar_reached gates the starmap T to R arrow") {
  GameState state;
  state.scenario.stellars.resize(2);
  state.scenario.stellars[0].system_id = 11;
  state.scenario.stellars[0].name = "Alpha";
  state.scenario.stellars[0].is_available = true;
  state.scenario.stellars[1].system_id = 12;
  state.scenario.stellars[1].name = "Beta";
  state.scenario.stellars[1].is_available = true;

  auto &mission = state.active_missions[0];
  mission.travel_stellar_id = 0;
  mission.return_stellar_id = 1;
  mission.flags_primary = 0;
  auto &runtime = state.active_mission_runtime_flags[0];
  runtime.is_active = true;

  CHECK(BuildMissionTargetSystems(state) == std::vector<std::int16_t>{11});
  runtime.travel_stellar_reached = true;
  CHECK(BuildMissionTargetSystems(state) == std::vector<std::int16_t>{12});

  // ReturnStel unavailable: the original keeps the TravelStel arrow.
  state.scenario.stellars[1].is_available = false;
  CHECK(BuildMissionTargetSystems(state) == std::vector<std::int16_t>{11});
}

// Ghidra 0x0043e6f0: the system locators reject a candidate whose
// System_ResolveSystemDiscoverySlot equals the player's current slot, so a
// mission never picks a discovery-twin of the system the player is in.
TEST_CASE("system locators exclude same-discovery-slot twins") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.current_system_locator = 10005; // government 5
  definition.travel_stellar_locator = 0x80;
  definition.return_stellar_locator = 0x81;

  state.scenario.stellars.resize(2);
  state.scenario.stellars[0].system_id = 9;
  state.scenario.stellars[1].system_id = 10;
  state.scenario.systems.resize(3);
  for (auto &system : state.scenario.systems) {
    system.is_visible = true;
  }
  state.scenario.systems[0].government_id = 1;
  state.scenario.systems[1].government_id = 5;
  // System 1 shares system 0's discovery slot; system 2 is distinct.
  state.scenario.systems[1].visibility_root_system_id = 0;
  state.scenario.systems[2].government_id = 5;

  state.player.current_system_id = 0;
  REQUIRE(Mission_ActivateAtSlot(state, 0));
  // Only system 2 is both government 5 and on a different discovery slot.
  CHECK(state.active_missions[0].current_system_id == 2);
}

// Bible mission Flags 0x0002 ("Don't show the red destination arrows") and
// 0x0200 ("Show an additional arrow on the map for the ShipSyst").
TEST_CASE("starmap mission arrows honor Bible Flags 0x0002 and 0x0200") {
  GameState state;
  state.scenario.stellars.resize(1);
  state.scenario.stellars[0].system_id = 11;
  state.scenario.stellars[0].name = "Alpha";
  state.scenario.stellars[0].is_available = true;
  state.scenario.systems.resize(3);
  state.scenario.systems[2].is_visible = true;
  state.scenario.systems[2].visibility_root_system_id = -1;

  auto &mission = state.active_missions[0];
  mission.travel_stellar_id = 0;
  mission.current_system_id = 2;
  mission.target_ship_count = 1;
  auto &runtime = state.active_mission_runtime_flags[0];
  runtime.is_active = true;

  mission.flags_primary = 0x0002;
  CHECK(BuildMissionTargetSystems(state).empty());

  mission.flags_primary = 0x0200;
  CHECK(BuildMissionTargetSystems(state) == (std::vector<std::int16_t>{11, 2}));
}

// Ghidra 0x0043f100: the TravelStel predicate only fires while the mission
// interaction window is open (g_mission_interaction_window != 0); a
// TravelStel-less mission sets the latch unconditionally.
TEST_CASE("travel_stellar_reached requires an actual landed visit") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.travel_stellar_locator = 0x80;
  definition.return_stellar_locator = -1;
  state.scenario.stellars.resize(1);
  state.scenario.stellars[0].name = "Alpha";
  Mission_ResolveMissionStellarLocators(state);

  // In-flight mode-2 target match: the raw ShipState +0x6C compare would
  // collide, but the port-only landed gate keeps the latch clear.
  state.system_transition_active = false;
  state.player.travel_transfer_mode = 2;
  state.travel.selected_stellar_id = 0x80;
  {
    MissionInteractionWindowScope window_scope(state);
    REQUIRE(Mission_ActivateAtSlot(state, 0));
  }
  CHECK_FALSE(state.active_mission_runtime_flags[0].travel_stellar_reached);

  // Mode-3 adjacency collision against the same 0-based travel_stellar_id.
  state.player.travel_transfer_mode = 3;
  state.travel.travel_slot = 0;
  {
    MissionInteractionWindowScope window_scope(state);
    REQUIRE(Mission_ActivateAtSlot(state, 0));
  }
  CHECK_FALSE(state.active_mission_runtime_flags[1].travel_stellar_reached);

  // Landed at the TravelStel with the window open: the latch fires.
  state.system_transition_active = true;
  state.player.travel_transfer_mode = -1;
  state.travel.travel_slot = -1;
  state.player.ai_secondary_target_slot = 0x80;
  {
    MissionInteractionWindowScope window_scope(state);
    REQUIRE(Mission_ActivateAtSlot(state, 0));
  }
  CHECK(state.active_mission_runtime_flags[2].travel_stellar_reached);

  // Landed at a different stellar: still clear.
  state.player.ai_secondary_target_slot = 0x81;
  {
    MissionInteractionWindowScope window_scope(state);
    REQUIRE(Mission_ActivateAtSlot(state, 0));
  }
  CHECK_FALSE(state.active_mission_runtime_flags[3].travel_stellar_reached);

  // Landed match but no interaction window (scripted activation): clear.
  state.player.ai_secondary_target_slot = 0x80;
  REQUIRE(Mission_ActivateAtSlot(state, 0));
  CHECK_FALSE(state.active_mission_runtime_flags[4].travel_stellar_reached);

  // No TravelStel: the latch is set regardless of window or landed context.
  definition.travel_stellar_locator = -1;
  Mission_ResolveMissionStellarLocators(state);
  state.system_transition_active = false;
  REQUIRE(Mission_ActivateAtSlot(state, 0));
  CHECK(state.active_mission_runtime_flags[5].travel_stellar_reached);
}

// A nested S in an OnAccept payload must inherit both the open offer window
// and the landed context, so its own TravelStel match latches.
TEST_CASE("nested OnAccept S inherits the landed offer window") {
  GameState state;
  state.scenario.missions.resize(2);
  for (auto &definition : state.scenario.missions) {
    definition.present = true;
    definition.travel_stellar_locator = 0x80;
    definition.return_stellar_locator = -1;
  }
  state.scenario.stellars.resize(1);
  state.scenario.stellars[0].name = "Alpha";
  Mission_ResolveMissionStellarLocators(state);

  // Outer mission 0's on-accept payload activates mission 1 (S129).
  constexpr std::string_view kStartNested = "S129";
  for (std::size_t i = 0; i < kStartNested.size(); ++i) {
    state.scenario.missions[0].raw_payload[0x15b + i] =
        static_cast<std::byte>(kStartNested[i]);
  }

  state.system_transition_active = true;
  state.player.ai_secondary_target_slot = 0x80;
  {
    MissionInteractionWindowScope window_scope(state);
    REQUIRE(Mission_ActivateAtSlot(state, 0));
  }

  CHECK(state.active_mission_runtime_flags[0].travel_stellar_reached);
  CHECK(state.active_mission_runtime_flags[1].travel_stellar_reached);
}

// 0x0043f100 reads ai_secondary_target_slot AFTER the on-accept payload, so an
// M/N opcode can repoint the live field before the TravelStel latch is judged.
// A pre-payload snapshot would take the opposite branch in both arms below.
TEST_CASE("OnAccept M/N repoints the target before the TravelStel latch") {
  const auto prepare = [](GameState &state) {
    state.scenario.missions.resize(1);
    state.scenario.missions[0].present = true;
    state.scenario.missions[0].travel_stellar_locator = 0x80;
    state.scenario.missions[0].return_stellar_locator = -1;
    state.scenario.stellars.resize(1);
    state.scenario.stellars[0].name = "Alpha";
    state.scenario.systems.resize(3);
    state.scenario.systems[2].nav_defs[0] = 0x80;
    Mission_ResolveMissionStellarLocators(state);
    state.system_transition_active = true;
  };
  const auto write_payload = [](GameState &state, std::string_view script) {
    for (std::size_t i = 0; i < script.size(); ++i) {
      state.scenario.missions[0].raw_payload[0x15b + i] =
          static_cast<std::byte>(script[i]);
    }
  };

  // M130 repositions to system 2's first nav (resource 0x80 -> index 0), the
  // TravelStel. Pre-script the player was elsewhere, so only the late read
  // latches.
  {
    GameState state;
    prepare(state);
    write_payload(state, "M130");
    state.player.ai_secondary_target_slot = 0x85;
    MissionInteractionWindowScope window_scope(state);
    REQUIRE(Mission_ActivateAtSlot(state, 0));
    CHECK(state.active_mission_runtime_flags[0].travel_stellar_reached);
  }

  // N130 clears the navigation target, so the post-script value no longer
  // matches the TravelStel even though the pre-script value did.
  {
    GameState state;
    prepare(state);
    write_payload(state, "N130");
    state.player.ai_secondary_target_slot = 0x80;
    MissionInteractionWindowScope window_scope(state);
    REQUIRE(Mission_ActivateAtSlot(state, 0));
    CHECK_FALSE(state.active_mission_runtime_flags[0].travel_stellar_reached);
  }
}

// Nested offers (e.g. an OnAccept S that starts another mission) must restore
// the enclosing window state rather than clobber it.
TEST_CASE("mission interaction window scope restores nested state") {
  GameState state;
  CHECK_FALSE(state.mission_offer_window_open);
  {
    MissionInteractionWindowScope outer(state);
    CHECK(state.mission_offer_window_open);
    {
      MissionInteractionWindowScope inner(state);
      CHECK(state.mission_offer_window_open);
    }
    CHECK(state.mission_offer_window_open);
  }
  CHECK_FALSE(state.mission_offer_window_open);
}

// Ghidra 0x004438d0: arriving at the TravelStel sets travel_stellar_reached
// whether or not cargo is picked up; the ReturnStel does not set it for a
// distinct TravelStel.
TEST_CASE("TravelStel arrival sets travel_stellar_reached") {
  GameState state;
  state.scenario.stellars.resize(2);
  state.scenario.stellars[0].name = "Alpha";
  state.scenario.stellars[1].name = "Beta";

  auto &mission = state.active_missions[0];
  mission.travel_stellar_id = 0;
  mission.return_stellar_id = 1;
  mission.pickup_mode = -1;
  auto &runtime = state.active_mission_runtime_flags[0];
  runtime.is_active = true;

  Mission_ProcessInteractionReactionSlotResources(state, 0, 0);
  CHECK(runtime.travel_stellar_reached);
  CHECK_FALSE(runtime.objective_complete);

  runtime.travel_stellar_reached = false;
  Mission_ProcessInteractionReactionSlotResources(state, 0, 1);
  CHECK_FALSE(runtime.travel_stellar_reached);
}

// Ghidra 0x00443780: success at the ReturnStel needs BOTH
// travel_stellar_reached and the ship-goal latch; the same landing without the
// TravelStel leg leaves the mission active.
TEST_CASE("ReturnStel success gate needs travel_stellar_reached") {
  GameState state;
  state.scenario.missions.resize(1);
  state.scenario.missions[0].present = true;
  state.scenario.stellars.resize(2);
  state.scenario.stellars[0].name = "Alpha";
  state.scenario.stellars[1].name = "Beta";

  auto &mission = state.active_missions[0];
  mission.travel_stellar_id = 0;
  mission.return_stellar_id = 1;
  mission.ship_goal = -1;
  auto &runtime = state.active_mission_runtime_flags[0];
  runtime.is_active = true;
  runtime.objective_complete = true;
  runtime.travel_stellar_reached = false;

  int debriefs = 0;
  Mission_TickReactionSlotsForTravelInteraction(
      state, /*landed_stellar_id=*/1, 0, [&](const MissionDialogText &) {
        ++debriefs;
      });
  CHECK(runtime.is_active);
  CHECK(debriefs == 0);

  runtime.travel_stellar_reached = true;
  Mission_TickReactionSlotsForTravelInteraction(
      state, /*landed_stellar_id=*/1, 0, [&](const MissionDialogText &) {
        ++debriefs;
      });
  CHECK_FALSE(runtime.is_active);
}

// Mission_OriginalAiSecondaryTargetSlot units: a stale docked resource id must
// be rebased even when no in-flight target exists, so it cannot numerically
// alias a different 0-based travel_stellar_id. The docked M-script re-point and
// the raw mode-3 adjacency slot keep precedence.
TEST_CASE("mission offer target helper rebases a stale docked stellar") {
  GameState state;
  state.system_transition_active = false;
  state.player.travel_transfer_mode = -1;
  state.travel.travel_slot = -1;
  state.travel.selected_stellar_id = -1;
  state.player.ai_secondary_target_slot = 0x85; // resource id -> index 5
  CHECK(Mission_OriginalAiSecondaryTargetSlot(state) == 5);

  state.system_transition_active = true;
  state.player.ai_secondary_target_slot = 0x83;
  CHECK(Mission_OriginalAiSecondaryTargetSlot(state) == 3);

  state.system_transition_active = false;
  state.player.travel_transfer_mode = 3;
  state.travel.travel_slot = 7;
  state.player.ai_secondary_target_slot = 0x85;
  CHECK(Mission_OriginalAiSecondaryTargetSlot(state) == 7);
}

// Goal-4 (observe) cloaked-target visibility: the original sects the hull
// frame's screen rect with the gameplay surface inset by half the frame. A
// mission ship carrying a ModType-17 cloak (so Ship_CanMaintainCloakState is
// true) counts as observed only while it is fully on screen.
TEST_CASE("observe mission counts a cloaked mission ship only when on screen") {
  const auto setup = [] {
    GameState state;
    state.viewport_center_x = 320;
    state.viewport_center_y = 200;
    state.player.current_system_id = 0;

    ActiveMission &mission = state.active_missions[0];
    mission.ship_goal = 4;          // Bible ShipGoal: observe.
    mission.current_system_id = -6; // -6: the player's current system.
    mission.target_ship_count = 1;
    mission.mission_target_count = 1;
    mission.goal_count_remaining = 1;
    mission.brief_description_ids[7] = -1; // no completion desc dialog.
    state.active_mission_runtime_flags[0].is_active = true;

    // One ship class whose default loadout carries a ModType-17 cloak.
    state.scenario.ships.resize(1);
    state.scenario.ships[0].default_outfit_ids[0] = 0x80;
    state.scenario.ships[0].default_outfit_counts[0] = 1;
    state.scenario.outfits.resize(1);
    state.scenario.outfits[0].mod_type = 0x11;

    Ship &ship = state.ships_[1];
    ship.is_active = true;
    ship.mission_fleet_slot = 0;
    ship.ship_class_id = 0;
    ship.ship_instance_id = 1; // NPC
    ship.current_system_id = 0;
    ship.armor_points = 100.0F;
    ship.shield_points = 100.0F;
    ship.fuel_points = 100.0F;
    return state;
  };

  SECTION("fully on screen completes the objective") {
    GameState state = setup();
    state.ships_[1].pos_x = 0.0F;
    state.ships_[1].pos_y = 0.0F;

    Mission_HandleMissionOrSurrenderShipReaction(state, 0, 0);

    CHECK(state.active_mission_runtime_flags[0].objective_complete);
  }

  SECTION("off screen leaves the objective incomplete") {
    GameState state = setup();
    state.ships_[1].pos_x = 1000.0F; // screen x well past the 640px surface.

    Mission_HandleMissionOrSurrenderShipReaction(state, 0, 0);

    CHECK_FALSE(state.active_mission_runtime_flags[0].objective_complete);
  }

  SECTION("an uncloaked mission ship is seen even off screen") {
    GameState state = setup();
    state.scenario.ships[0].default_outfit_counts[0] = 0; // no cloak device.
    state.ships_[1].pos_x = 1000.0F;

    Mission_HandleMissionOrSurrenderShipReaction(state, 0, 0);

    CHECK(state.active_mission_runtime_flags[0].objective_complete);
  }
}
