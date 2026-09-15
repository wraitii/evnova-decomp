#include "game/flight_automation.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace game;

namespace {
GameState AutomationState() {
  GameState state;
  state.player.is_active = true;
  state.player.current_system_id = 0;
  state.scenario.systems.resize(3);
  state.scenario.systems[0].name = "Sol";
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.systems[0].nav_defs[1] = 0x81;
  state.scenario.systems[0].links[0] = 0x81;
  state.scenario.systems[1].name = "Alpha Centauri";
  state.scenario.systems[2].name = "Unlinked";
  state.scenario.stellars.resize(2);
  state.scenario.stellars[0].name = "Earth";
  state.scenario.stellars[1].name = "Mars";
  return state;
}
} // namespace

TEST_CASE(
    "automation resolves current-system stellars and direct links exactly") {
  GameState state = AutomationState();
  CHECK(FlightAutomationController::ResolveStellar(state, "eArTh") == 0x80);
  CHECK_FALSE(FlightAutomationController::ResolveStellar(state, "Ear"));
  CHECK(FlightAutomationController::ResolveLinkedSystem(state,
                                                        "alpha centauri") == 1);
  CHECK_FALSE(
      FlightAutomationController::ResolveLinkedSystem(state, "Unlinked"));
}

TEST_CASE("automation stop turns around without thrust until aligned") {
  PlayerShip ship;
  ship.heading = 0.0F;
  ship.vel_y = -4.0F;
  FlightInput input;
  FlightAutomationController::Stop(ship, 5.0F, input);
  CHECK((input.turn_left || input.turn_right));
  CHECK_FALSE(input.thrust);
  ship.heading = 3.14159265358979323846F;
  input = {};
  FlightAutomationController::Stop(ship, 5.0F, input);
  CHECK(input.thrust);
}

TEST_CASE("automation move-away faces radially outward before thrusting") {
  PlayerShip ship;
  ship.pos_x = 100.0F;
  ship.heading = 3.14159265358979323846F;
  FlightInput input;
  FlightAutomationController::MoveAwayFrom(ship, 0.0F, 0.0F, 5.0F, input);
  CHECK((input.turn_left || input.turn_right));
  CHECK_FALSE(input.thrust);

  ship.heading = 3.14159265358979323846F / 2.0F;
  input = {};
  FlightAutomationController::MoveAwayFrom(ship, 0.0F, 0.0F, 5.0F, input);
  CHECK(input.thrust);
}

TEST_CASE("automation edge commands include a release frame") {
  GameState state = AutomationState();
  FlightAutomationController automation;
  REQUIRE(automation.JumpTo(state, "Alpha Centauri", 0));
  FlightInput first;
  automation.Tick(state, 0, first);
  CHECK(first.hyperspace_mode);
  FlightInput release;
  automation.Tick(state, 1, release);
  CHECK_FALSE(release.hyperspace_mode);
}

TEST_CASE("automation fails on death and simulation deadline") {
  GameState state = AutomationState();
  FlightAutomationController automation;
  REQUIRE(automation.LandAt(state, "Earth", 10, 5));
  FlightInput input;
  automation.Tick(state, 16, input);
  CHECK(automation.status().phase == FlightAutomationPhase::kFailed);
  REQUIRE(automation.LandAt(state, "Earth", 20));
  state.game_over_pending = true;
  automation.Tick(state, 20, input);
  CHECK(automation.status().phase == FlightAutomationPhase::kFailed);
}
