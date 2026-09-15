#include "game/flight_automation.hpp"
#include "game/spaceflight.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

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

// Runs MoveToAndStopAt against the real free-flight integrator (no gravity) and
// reports where the ship settled. `envelope` mirrors the caller-supplied
// per-axis arrival gate; the maneuver is asked to arrive within half of it.
namespace {
struct ArrivalRun {
  float distance = 0.0F;
  float axis_speed = 0.0F;
  float pos_x = 0.0F;
  int frames = 0;
  bool converged = false;
};

ArrivalRun RunArrival(float start_distance, float envelope) {
  ShipClass cls;
  cls.accel = 500.0F;    // raw -> thrust 0.10 px/tick^2
  cls.speed = 400.0F;    // raw -> 4.0 px/tick
  cls.turn_rate = 40.0F; // raw -> 4.0 deg/tick
  PlayerShip ship;
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.heading = 0.0F; // pointing up; target is to the right
  const float tx = start_distance;
  const float ty = 0.0F;
  const float radius = envelope * 0.5F;
  ArrivalRun run;
  for (int frame = 0; frame < 3000; ++frame) {
    FlightInput input;
    FlightAutomationController::MoveToAndStopAt(
        ship, tx, ty, radius, 4.0F, 0.1F, input);
    (void)NovaPlayer_IntegrateMovement(ship, input, cls, 1.0F);
    run.frames = frame + 1;
    run.distance = std::hypot(tx - ship.pos_x, ty - ship.pos_y);
    run.axis_speed = std::max(std::abs(ship.vel_x), std::abs(ship.vel_y));
    run.pos_x = ship.pos_x;
    if (run.distance <= radius && run.axis_speed <= 0.55F) {
      run.converged = true;
      break;
    }
  }
  return run;
}
} // namespace

TEST_CASE("automation arrival settles within tolerance without flying past") {
  constexpr float kTargetX = 1400.0F;
  constexpr float kRadius = 110.0F;
  const ArrivalRun run = RunArrival(kTargetX, /*envelope=*/220.0F);
  CHECK(run.converged);
  // The approach comes from -x; crossing the centre is fine, but ending up on
  // the far side by more than the tolerance would mean it lost control.
  CHECK(run.pos_x <= kTargetX + kRadius);
  CHECK(run.distance <= kRadius);
  CHECK(run.axis_speed <= 0.55F);
}

TEST_CASE("automation arrival matches a moving target's velocity") {
  ShipClass cls;
  cls.accel = 500.0F;
  cls.speed = 400.0F;
  cls.turn_rate = 40.0F;
  PlayerShip ship;
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.heading = 0.0F;
  constexpr float kRadius = 100.0F;
  constexpr float kTargetVelX = 0.0F;
  constexpr float kTargetVelY = -1.5F; // target drifts upward
  float target_x = 1200.0F;
  float target_y = 0.0F;
  bool converged = false;
  for (int frame = 0; frame < 4000 && !converged; ++frame) {
    FlightInput input;
    FlightAutomationController::MoveToAndStopAt(ship,
                                                target_x,
                                                target_y,
                                                kRadius,
                                                4.0F,
                                                0.1F,
                                                input,
                                                1.0F,
                                                kTargetVelX,
                                                kTargetVelY);
    (void)NovaPlayer_IntegrateMovement(ship, input, cls, 1.0F);
    target_x += kTargetVelX;
    target_y += kTargetVelY;
    const float distance =
        std::hypot(target_x - ship.pos_x, target_y - ship.pos_y);
    const float rel_speed =
        std::hypot(ship.vel_x - kTargetVelX, ship.vel_y - kTargetVelY);
    converged = distance <= kRadius && rel_speed <= 0.55F;
  }
  CHECK(converged);
  // The ship should have matched the target's drift, not stopped dead.
  CHECK(std::abs(ship.vel_y - kTargetVelY) <= 0.55F);
}

TEST_CASE("automation arrival sheds speed to tighten a slow turn") {
  // A slow-turning hull approaching with lateral velocity used to circle the
  // target indefinitely (thousands of frames): the seek phase never braked
  // because its speed was well under the stopping cap. The turn-radius cap must
  // shed speed so the nose can curve onto the point.
  ShipClass cls;
  cls.accel = 500.0F;
  cls.speed = 400.0F;
  cls.turn_rate = 10.0F; // 1 deg/tick
  PlayerShip ship;
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.heading = 0.0F; // pointing at the target
  ship.vel_x = 4.0F;   // but drifting sideways at top speed
  constexpr float kTargetY = -1500.0F;
  constexpr float kRadius = 100.0F;
  bool converged = false;
  int frames = 0;
  for (; frames < 2000 && !converged; ++frames) {
    FlightInput input;
    FlightAutomationController::MoveToAndStopAt(
        ship, 0.0F, kTargetY, kRadius, 1.0F, 0.1F, input);
    (void)NovaPlayer_IntegrateMovement(ship, input, cls, 1.0F);
    const float distance = std::hypot(ship.pos_x, kTargetY - ship.pos_y);
    converged =
        distance <= kRadius && std::hypot(ship.vel_x, ship.vel_y) <= 0.55F;
  }
  CHECK(converged);
  CHECK(frames < 900);
}

TEST_CASE("automation land tap waits for the real per-axis envelope") {
  GameState state = AutomationState();
  state.scenario.ships.resize(1);
  state.scenario.ships[0].accel = 500.0F;
  state.scenario.ships[0].speed = 400.0F;
  state.scenario.ships[0].turn_rate = 40.0F;
  state.player.ship_class_id = 0;
  state.player.is_active = true;
  state.scenario.stellars[0].pos_x = 0;
  state.scenario.stellars[0].pos_y = 0;
  state.scenario.stellars[0].system_id = 0;
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = 0x2ee;

  FlightAutomationController automation;
  REQUIRE(automation.LandAt(state,
                            "Earth",
                            0,
                            180000,
                            /*landing_envelope_axis_range=*/75.0F));
  // 150 px out is inside the old 220 px circle but outside the 75 px gate.
  state.player.pos_x = 150.0F;
  state.player.pos_y = 0.0F;
  FlightInput input;
  automation.Tick(state, 0, input);
  CHECK(automation.status().phase == FlightAutomationPhase::kMove);
  CHECK_FALSE(input.land);

  // Inside the gate and slow: the one-frame land tap is issued.
  state.player.pos_x = 50.0F;
  state.player.vel_x = 0.0F;
  state.player.vel_y = 0.0F;
  input = {};
  automation.Tick(state, 16, input);
  CHECK(automation.status().phase == FlightAutomationPhase::kEngage);
  CHECK(input.land);
}

TEST_CASE("automation arrival reaches a small-sprite envelope") {
  // A 75 px envelope was impossible with the old fixed 220 px stop radius.
  const ArrivalRun run = RunArrival(/*start_distance=*/900.0F,
                                    /*envelope=*/75.0F);
  CHECK(run.converged);
  CHECK(run.distance <= 37.5F);
  CHECK(run.axis_speed <= 0.55F);
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
