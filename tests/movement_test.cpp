#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/spaceflight.hpp"
#include "sdl_platform.hpp"

#include <cmath>

namespace {

using game::PlayerShip;
using game::ShipClass;

// The Baktun-style starter (ship 0x80) movement stats verified in the scenario
// loader test: accel 500, speed 400, maneuver 40.
ShipClass &StarterClass(ShipClass &sc) {
  sc.accel = 500.0F;
  sc.speed = 400.0F;
  sc.turn_rate = 40.0F;
  return sc;
}

constexpr float kEps = 1e-4F;

} // namespace

// The player free-flight movement is a pure per-frame integrator mirroring the
// original Ship_HandleShip / Math_AddPolarVelocityWithClamp model. These tests
// pin the three behaviours that differ most from the old fixed-constant
// stand-in: ship-class-scaled stats, near-instant-top-out thrust with a genuine
// top-speed clamp, and inertia-preserving coast (no per-frame drag).

TEST_CASE("turn rate follows the ship class at the loader-scaled degrees/frame",
          "[movement]") {
  ShipClass sc;
  StarterClass(sc);
  PlayerShip ship;
  FlightInput in;
  in.turn_right = true;

  // turn_rate=40 -> base_turn_rate_deg = 40*0.1 = 4.0 deg/frame.
  const float heading_deg_before = ship.heading * 180.0F / 3.14159265358979323846F;
  (void)game::NovaPlayer_IntegrateMovement(ship, in, sc);
  const float heading_deg_after = ship.heading * 180.0F / 3.14159265358979323846F;
  // Frame 0 turns toward positive heading by exactly the class rate.
  CHECK(heading_deg_after - heading_deg_before ==
        Catch::Approx(4.0F).margin(1e-3F));
}

TEST_CASE("thrust reaches and clamps at the class top speed", "[movement]") {
  ShipClass sc;
  StarterClass(sc);
  PlayerShip ship;
  FlightInput in;
  in.thrust = true;

  // max speed = 400/640 = 0.625 px/frame; thrust = 500/10000 = 0.05 px/frame^2,
  // so top-out is reached in ~13 frames (near-instant, like the original).
  for (int i = 0; i < 120; ++i) {
    (void)game::NovaPlayer_IntegrateMovement(ship, in, sc);
  }
  CHECK(ship.speed == Catch::Approx(0.625F).margin(kEps));
}

TEST_CASE("coast preserves velocity when thrust is released", "[movement]") {
  ShipClass sc;
  StarterClass(sc);
  PlayerShip ship;
  FlightInput in;
  in.thrust = true;
  for (int i = 0; i < 120; ++i) {
    (void)game::NovaPlayer_IntegrateMovement(ship, in, sc);
  }
  const float vx = ship.vel_x;
  const float vy = ship.vel_y;

  // Release thrust: no input. The ship keeps its velocity (no per-frame drag).
  FlightInput coast;
  (void)game::NovaPlayer_IntegrateMovement(ship, coast, sc);
  CHECK(ship.vel_x == vx);
  CHECK(ship.vel_y == vy);
  CHECK(ship.speed == Catch::Approx(0.625F).margin(kEps));
}

TEST_CASE("brake applies reverse thrust toward zero velocity", "[movement]") {
  ShipClass sc;
  StarterClass(sc);
  PlayerShip ship;
  FlightInput in;
  in.thrust = true;
  for (int i = 0; i < 120; ++i) {
    (void)game::NovaPlayer_IntegrateMovement(ship, in, sc);
  }
  REQUIRE(ship.speed > 0.5F);

  FlightInput brake;
  brake.brake = true;
  for (int i = 0; i < 400; ++i) {
    (void)game::NovaPlayer_IntegrateMovement(ship, brake, sc);
  }
  CHECK(ship.speed < 1e-3F);
}

TEST_CASE("heading 0 points up and 90 degrees points right", "[movement]") {
  ShipClass sc;
  StarterClass(sc);
  PlayerShip ship; // heading 0: 'up' (negative y)
  FlightInput in;
  in.thrust = true;
  (void)game::NovaPlayer_IntegrateMovement(ship, in, sc);
  CHECK(ship.vel_y < 0.0F);
  CHECK(std::abs(ship.vel_x) < kEps);

  // Turn right only (no thrust) to 90 degrees: heading should rise by exactly
  // the class turn rate per frame, and momentum stays zero while coasting.
  PlayerShip ship2;
  FlightInput turn;
  turn.turn_right = true;
  constexpr float kHalfTurn = 3.14159265358979323846F / 2.0F;
  for (int i = 0; i < 23; ++i) {
    (void)game::NovaPlayer_IntegrateMovement(ship2, turn, sc);
  }
  CHECK(ship2.heading > kHalfTurn - 0.05F);
  CHECK(ship2.heading < kHalfTurn + 0.05F);
  // Pure turn computes no velocity.
  CHECK(ship2.vel_x == 0.0F);
  CHECK(ship2.vel_y == 0.0F);

  // Now thrust at the heading-90 orientation: velocity must point +x.
  PlayerShip ship3;
  ship3.heading = kHalfTurn;
  FlightInput thrust_right;
  thrust_right.thrust = true;
  (void)game::NovaPlayer_IntegrateMovement(ship3, thrust_right, sc);
  CHECK(ship3.vel_x > 0.0F);
  CHECK(std::abs(ship3.vel_y) < kEps);
}
