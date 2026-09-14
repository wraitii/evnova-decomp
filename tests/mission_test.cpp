#include "game/game_state.hpp"
#include "game/mission.hpp"
#include "game/outfit.hpp"
#include "game/travel.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <string>
#include <string_view>

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

// Regression for Ghidra 0x0043f8c0 Mission_PopulateMissionSlotFromDef:
// special-ship spawn mode 1 starts with no completed target-goal count, even
// when the definition has a nonzero target-ship count.
TEST_CASE("special-ship mission starts with zero remaining goals") {
  GameState state;
  state.scenario.missions.resize(1);
  auto &definition = state.scenario.missions[0];
  definition.present = true;
  definition.target_ship_count = 7;
  definition.special_ship_spawn_mode = 1;

  REQUIRE(Mission_PopulateActiveSlot(state, 0, 0));

  const auto &mission = state.active_missions[0];
  CHECK(mission.target_ship_count == 7);
  CHECK(mission.mission_target_count == 7);
  CHECK(mission.goal_count_remaining == 0);
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
    stellar.engage_access = 1;
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
    stellar.engage_access = 1;
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
    stellar.engage_access = 1;
  }
  state.scenario.stellars[0].system_id = 0;
  state.scenario.stellars[1].system_id = 0;
  state.travel.selected_stellar_id = 0x80;

  // Both candidates are in the anchor's system; the reference plumbing must
  // reject them rather than using the chain-only destination arm.
  Mission_ResolveMissionStellarLocators(state);
  CHECK(state.mission_target_resolutions[0].travel_stellar_id == -1);
}

// Regression for Ghidra 0x00448670 Mission_TriggerReturnMissionInteractions:
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

  CHECK(Mission_TriggerLandingInteractions(state, 3, 100, decline));
  CHECK(offer_count == 1);
  CHECK(offered_definition == 0);
  CHECK_FALSE(Mission_TriggerLandingInteractions(state, 3, 200, decline));
  CHECK(offer_count == 1);
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
  state.scenario.pers_defs[0].present = true;
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
  mission.spawn_behavior = 3;
  state.active_mission_runtime_flags[0].is_active = true;

  REQUIRE(Mission_HandleAcceptedShipInteraction(state, 1, 1234));
  CHECK_FALSE(target.is_active);
  CHECK_FALSE(state.scenario.pers_defs[0].present);
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
  CHECK_FALSE(Mission_CheckMissionShipInteractionEligibility(state, 0, false));

  state.player.fuel_points = kJumpFuelCost;
  CHECK(Mission_CheckMissionShipInteractionEligibility(state, 0, false));
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
  GameState state; // pilot defaults: male ('m' latch = DAT_00734c1c)

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
  REQUIRE(Mission_ActivateAtSlot(state,
                                 static_cast<std::int16_t>(tutorial_001),
                                 /*landed_stellar_id=*/-1));

  // On-accept payload: bit 8339 set, Sol (system resource 130, def 2)
  // explored.
  CHECK(state.control.ControlBit(8339));
  CHECK(state.control.explored_systems.test(2));

  // Tutorial 002's AvailBits (b9200 & !(b9201 | b9215)) must still fail
  // before completion.
  CHECK_FALSE(state.scenario.missions[tutorial_002].is_available_runtime);

  // Land on Earth (spob resource 128). The gate must resolve the mission as a
  // success and hand the Comp dësc (9200) text to the debrief sink.
  std::string debrief_text;
  int debrief_calls = 0;
  Mission_TickReactionSlotsForTravelInteraction(state,
                                                /*landed_stellar_id=*/0x80,
                                                /*now_ms=*/0,
                                                [&](const std::string &text) {
                                                  ++debrief_calls;
                                                  debrief_text = text;
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
