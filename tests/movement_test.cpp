#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/spaceflight.hpp"

#include <numbers>

namespace {

game::ShipClass TestShipClass() {
  game::ShipClass ship_class;
  ship_class.accel = 500.0F;
  ship_class.speed = 400.0F;
  ship_class.turn_rate = 40.0F;
  return ship_class;
}

} // namespace

TEST_CASE("flight integration uses elapsed original-cadence ticks") {
  game::PlayerShip ship;
  FlightInput input;
  input.thrust = true;

  const auto stats =
      game::NovaPlayer_IntegrateMovement(ship, input, TestShipClass(), 0.5F);

  CHECK(stats.max_speed_px_per_tick == Catch::Approx(4.0F));
  CHECK(stats.thrust_px_per_tick2 == Catch::Approx(0.1F));
  CHECK(ship.vel_y == Catch::Approx(-0.05F));
  CHECK(ship.pos_y == Catch::Approx(-0.025F));
}

TEST_CASE("reverse command turns the ship but preserves its velocity") {
  game::PlayerShip ship;
  ship.vel_y = -2.0F;
  FlightInput input;
  input.reverse = true;

  (void)game::NovaPlayer_IntegrateMovement(ship, input, TestShipClass(), 1.0F);

  CHECK(ship.heading ==
        Catch::Approx(4.0F * std::numbers::pi_v<float> / 180.0F));
  CHECK(ship.vel_x == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(-2.0F));
  CHECK(ship.pos_y == Catch::Approx(-2.0F));
  CHECK_FALSE(ship.engine_thrust);
}

TEST_CASE("flight turns at the original rounded effective turn rate") {
  game::PlayerShip ship;
  game::ShipClass ship_class = TestShipClass();
  // Resource maneuver 45 becomes 4.5 degrees/tick, which the original player
  // control path rounds before applying the frame-time multiplier.
  ship_class.turn_rate = 45.0F;
  FlightInput input;
  input.turn_right = true;

  (void)game::NovaPlayer_IntegrateMovement(ship, input, ship_class, 1.0F);

  CHECK(ship.heading ==
        Catch::Approx(5.0F * std::numbers::pi_v<float> / 180.0F));
}

// --- NPC ship movement (NovaShip_IntegrateNpcMovement; Ghidra Ship_HandleShip
// movement block) ---

TEST_CASE("npc turns toward its desired heading at the class turn rate") {
  game::GameState state;
  game::Ship ship;
  ship.ai_desired_heading_deg = 90; // heading 90 deg = right/down-clockwise
  // Class turn 40 raw -> 4.0 deg/tick continuous (not integer-rounded like the
  // player keyboard path).
  game::ShipClass cls = TestShipClass();
  cls.turn_rate = 40.0F;

  // Two ticks: 8 degrees toward the desired heading from 0.
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 2.0F);
  CHECK(ship.heading ==
        Catch::Approx(8.0F * std::numbers::pi_v<float> / 180.0F));
}

TEST_CASE("npc snaps exactly onto the desired heading within one turn step") {
  game::GameState state;
  game::Ship ship;
  ship.ai_desired_heading_deg = -45; // 315
  game::ShipClass cls = TestShipClass();
  cls.turn_rate = 40.0F; // 4 deg/tick

  // Far enough ticks to overshoot the snap window, so it lands exactly.
  for (int i = 0; i < 30; ++i) {
    game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  }
  CHECK(ship.heading ==
        Catch::Approx(315.0F * std::numbers::pi_v<float> / 180.0F));
}

TEST_CASE("npc forward thrust accelerates along the heading") {
  game::GameState state;
  game::Ship ship;
  ship.ai_desired_heading_deg = 0; // heading 0 = up (-y)
  ship.ai_desired_speed = 100.0F;
  // ai_forward_thrust_cmd is the instantaneous per-frame thrust magnitude the
  // AI orders (not the class accel); the step is cmd * ticks.
  ship.ai_forward_thrust_cmd = 10.0F;
  game::ShipClass cls = TestShipClass();
  cls.accel = 500.0F; // -> thrust 0.1 px/tick^2 (cruise cap / speed projection)
  cls.speed = 400.0F; // -> max speed 4.0 px/tick

  // One tick of forward thrust: step = cmd * ticks = 10.
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  CHECK(ship.vel_y == Catch::Approx(-10.0F)); // polar(0)= up / -y for sin/cos
  CHECK(ship.pos_y == Catch::Approx(-10.0F));
}

TEST_CASE("npc reverse absolute-sets velocity to heading * abs(desired)") {
  game::GameState state;
  game::Ship ship;
  ship.ai_desired_heading_deg = 0;
  ship.ai_desired_speed = -2.0F;
  ship.ai_forward_thrust_cmd = 1.0F;
  ship.vel_x = 99.0F;
  ship.vel_y = -50.0F;
  game::ShipClass cls = TestShipClass();

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  // Velocity re-imposed at abs(desired)=2 along heading 0 (up/-y); old velocity
  // discarded.
  CHECK(ship.vel_x == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(-2.0F));
  // The reverse path arms the coast-through reversal timer (30..60) in open
  // space (no current target).
  CHECK(ship.reverse_speed_bias >= 30.0F);
  CHECK(ship.reverse_speed_bias <= 60.0F);
}

TEST_CASE("npc inertia-less ships are pinned (vel zeroed, no motion)") {
  game::GameState state;
  game::Ship ship;
  ship.vel_x = 3.0F;
  ship.vel_y = -4.0F;
  game::ShipClass cls = TestShipClass();
  cls.accel = 0.0F;
  cls.speed = 0.0F;

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 10.0F);
  CHECK(ship.vel_x == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(0.0F));
  CHECK(ship.pos_x == Catch::Approx(0.0F));
  CHECK(ship.pos_y == Catch::Approx(0.0F));
}

TEST_CASE("npc coasts without thrust (no forward command)") {
  game::GameState state;
  game::Ship ship;
  // ai_desired_speed == 0 and ai_forward_thrust_cmd == 0: the thrust block is
  // skipped entirely; velocity is preserved and position integrated.
  ship.vel_x = 1.0F;
  ship.vel_y = -2.0F;
  game::ShipClass cls = TestShipClass();

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 3.0F);
  CHECK(ship.vel_x == Catch::Approx(1.0F)); // momentum preserved
  CHECK(ship.vel_y == Catch::Approx(-2.0F));
  CHECK(ship.pos_x == Catch::Approx(3.0F)); // pos += vel * ticks
  CHECK(ship.pos_y == Catch::Approx(-6.0F));
}
