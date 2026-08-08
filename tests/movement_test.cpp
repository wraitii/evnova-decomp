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
