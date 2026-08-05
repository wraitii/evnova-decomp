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
// pin the behaviours that differ most from the old fixed-constant stand-in:
// ship-class-scaled stats, near-instant top-out thrust governed by a per-axis
// polar clamp (which tops out a step above the nominal max and lets a full-
// thrust turn exceed it), and inertia-preserving coast (no per-frame drag).

TEST_CASE("turn rate follows the ship class at the loader-scaled degrees/frame",
          "[movement]") {
  ShipClass sc;
  StarterClass(sc);
  PlayerShip ship;
  FlightInput in;
  in.turn_right = true;

  // turn_rate=40 -> base_turn_rate_deg = 40*0.1 = 4.0 deg/frame.
  const float heading_deg_before =
      ship.heading * 180.0F / 3.14159265358979323846F;
  (void)game::NovaPlayer_IntegrateMovement(ship, in, sc);
  const float heading_deg_after =
      ship.heading * 180.0F / 3.14159265358979323846F;
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

  // max speed = 400/640 = 0.625 px/frame; thrust = 500/10000 = 0.05 px/frame.
  // The per-axis polar clamp adds a whole thrust step per frame, so top-out
  // settles one step ABOVE the nominal max (ceil(0.625/0.05)*0.05 = 0.65), not
  // pinned to it -- an authentic quirk of Math_AddPolarVelocityWithClamp.
  for (int i = 0; i < 120; ++i) {
    (void)game::NovaPlayer_IntegrateMovement(ship, in, sc);
  }
  CHECK(ship.speed == Catch::Approx(0.650F).margin(1e-4F));
  // Plateau is stable: further thrust frames do not keep building speed.
  for (int i = 0; i < 60; ++i) {
    (void)game::NovaPlayer_IntegrateMovement(ship, in, sc);
  }
  CHECK(ship.speed == Catch::Approx(0.650F).margin(1e-4F));
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
  CHECK(ship.speed == Catch::Approx(0.650F).margin(1e-4F));
}

// The forward-thrust top-speed governor is a per-AXIS polar clamp (Ghidra
// Math_AddPolarVelocityWithClamp 0x0043b4e0), not a vector-magnitude clamp.
// While it caps each axis's additional thrust at the polar projection of the
// nominal top speed, an existing cross-track component is never pulled back, so
// turning at full thrust can carry the net speed a little past the nominal max.
// A magnitude clamp would flatten speed to exactly `max`; this test asserts the
// per-axis behaviour that reproduces the authentic EVN drift.
TEST_CASE("per-axis clamp lets a full-thrust turn exceed the nominal top speed",
          "[movement]") {
  ShipClass sc;
  StarterClass(sc);
  PlayerShip ship;
  FlightInput in;
  in.thrust = true;
  for (int i = 0; i < 120; ++i) {
    (void)game::NovaPlayer_IntegrateMovement(ship, in, sc);
  }
  // Straight up at the top-out plateau: v = (0, -0.65), |v_x| ~ 0.
  REQUIRE(ship.speed == Catch::Approx(0.650F).margin(1e-4F));
  REQUIRE(std::abs(ship.vel_x) < 1e-4F);

  // Point at 45 degrees and keep thrusting (no turning). The +x component now
  // builds toward sin(45)*max (~0.442, plus up to one thrust step of overshoot)
  // while the -y component stays pinned at its earlier plateau (the clamp never
  // pulls an existing component back), so the net speed climbs above `max`. A
  // magnitude clamp would flatten |v| to exactly 0.625 instead.
  constexpr float kMax = 0.625F;
  const float sin45 = 0.70710678118F;
  ship.heading = 3.14159265358979323846F / 4.0F;
  FlightInput thrust_only;
  thrust_only.thrust = true;
  for (int i = 0; i < 200; ++i) {
    (void)game::NovaPlayer_IntegrateMovement(ship, thrust_only, sc);
  }
  // -y stays at the plateau reached before the turn (never pulled back).
  CHECK(ship.vel_y == Catch::Approx(-0.650F).margin(1e-4F));
  // +x accumulates toward within one thrust step above sin(45)*max.
  CHECK(ship.vel_x >= kMax * sin45 - 1e-3F);
  CHECK(ship.vel_x <= kMax * sin45 + 0.05F + 1e-3F);
  // Net speed now above the nominal 0.625 cap.
  CHECK(ship.speed > 0.7F);
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
