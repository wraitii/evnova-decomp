#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/landed_window.hpp"
#include "game/scenario_data.hpp"
#include "game/targeting.hpp"

namespace {

using game::GameState;
using game::LandedContext;
using game::LandedService;
using game::ShipClass;
using game::Stellar;
using game::System;

// Deterministic scenario with a player ship and one landable stellar, injected
// directly (no archive), so the landed-window housekeeping is unit-testable.
struct Fixture {
  GameState state;
  Stellar *landing = nullptr;

  Fixture() {
    state.player.current_system_id = 0;
    state.player.ship_class_id = 0;

    ShipClass sc;
    sc.base_shield = 30;
    sc.base_armor = 30;
    sc.base_fuel = 300;
    sc.cargo_holds = 10;
    state.scenario.ships.push_back(sc); // index 0 == resource 0x80

    System s;
    s.name = "Test System";
    s.links = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    s.nav_defs = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    s.is_visible = true;
    s.nav_defs[0] = 0x81;
    state.scenario.systems.push_back(s); // index 0 == resource 0x80

    // Scenario accessor maps resource id -> index (id - 0x80). The colony is
    // resource 0x81, i.e. stellars[1]; reserve index 0 with an inert filler so
    // the index/position stays aligned with that convention.
    Stellar filler;
    filler.name = "(unused orbital)";
    filler.system_id = 0;
    state.scenario.stellars.push_back(filler); // index 0 == resource 0x80

    Stellar l;
    l.name = "Orbital Colony";
    // Engaged landable target: control bit + engaged + landable (0x83).
    l.flags = 0x83;
    l.availability_flags = 0;
    l.pos_x = 200;
    l.pos_y = 0;
    l.sprite_population = 1;
    l.sprite_handle_active = true;
    l.system_id = 0;
    state.scenario.stellars.push_back(l); // index 1 == resource 0x81

    landing = &state.scenario.stellars[1];
  }
};

} // namespace

// The landed context starts at "launch" (top of the services list) with a
// resolved stellar id once entered, mirroring the landing transition wiring.

TEST_CASE("entering the dock files the stellar id and resets selection",
          "[landed_window]") {
  Fixture f;
  f.state.player.pos_x = 200.0F;
  f.state.player.pos_y = 0.0F;
  f.state.player.shield_points = 5.0F;
  f.state.player.armor_points = 3.0F;
  f.state.player.credits = 500;
  f.landing->service_cost = 150;
  game::NovaTargeting_UpdatePlayerTarget(f.state);

  LandedContext ctx;
  REQUIRE(game::NovaLanding_EnterDocked(f.state, ctx) == true);
  CHECK(ctx.landed);
  CHECK(ctx.stellar_id == 0x81);
  CHECK(ctx.selection == LandedService::kLaunch);
  // The landing transition refills shields/armor and bills the service cost.
  CHECK(f.state.player.shield_points == Catch::Approx(30.0F));
  CHECK(f.state.player.armor_points == Catch::Approx(30.0F));
  CHECK(f.state.player.credits == 500 - 150);
}

// Fuel service prices a top-up from the (outfit derived) effective capacity.

TEST_CASE("refuel buys fuel toward capacity and bills credits",
          "[landed_window]") {
  Fixture f;
  // Deplete to a third full.
  f.state.player.fuel_points = 100.0F;
  f.state.player.credits = 500;
  // 2 credits per fuel point -> 400 credits fills the missing 200.
  const std::int32_t spent = game::NovaLanded_Refuel(f.state, 2);
  CHECK(spent == 400);
  CHECK(f.state.player.credits == 500 - 400);
  CHECK(f.state.player.fuel_points == Catch::Approx(300.0F));
}

TEST_CASE("refuel clamps at an empty wallet", "[landed_window]") {
  Fixture f;
  f.state.player.fuel_points = 0.0F;
  f.state.player.credits = 3;
  const std::int32_t spent = game::NovaLanded_Refuel(f.state, 2);
  CHECK(f.state.player.credits == 0);
  CHECK(spent == 3);
  // 3 credits at 2/point -> 1.5 fuel units.
  CHECK(f.state.player.fuel_points == Catch::Approx(1.5F));
}

TEST_CASE("refuel does nothing when the tank is already full",
          "[landed_window]") {
  Fixture f;
  f.state.player.fuel_points = 300.0F;
  f.state.player.credits = 500;
  CHECK(game::NovaLanded_Refuel(f.state, 2) == 0);
  CHECK(f.state.player.credits == 500);
}

// Armor repair service: hidden fleet charges a per-point price for the gap.

TEST_CASE("repair tops up depleted armor and bills credits",
          "[landed_window]") {
  Fixture f;
  f.state.player.armor_points = 10.0F;
  f.state.player.credits = 500;
  // 5 credits per armor point -> 100 credits to close the 20-point gap.
  const std::int32_t spent = game::NovaLanded_Repair(f.state, 5);
  CHECK(spent == 100);
  CHECK(f.state.player.credits == 500 - 100);
  CHECK(f.state.player.armor_points == Catch::Approx(30.0F));
}

TEST_CASE("repair clamps at an empty wallet", "[landed_window]") {
  Fixture f;
  f.state.player.armor_points = 10.0F;
  f.state.player.credits = 7;
  const std::int32_t spent = game::NovaLanded_Repair(f.state, 5);
  CHECK(spent == 7);
  CHECK(f.state.player.credits == 0);
  CHECK(f.state.player.armor_points == Catch::Approx(10.0F + 7.0F / 5.0F));
}

TEST_CASE("repair does nothing when armor is intact", "[landed_window]") {
  Fixture f;
  f.state.player.armor_points = 30.0F;
  f.state.player.credits = 500;
  CHECK(game::NovaLanded_Repair(f.state, 5) == 0);
  CHECK(f.state.player.credits == 500);
}
