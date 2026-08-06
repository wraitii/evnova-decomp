#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/scenario_data.hpp"
#include "game/travel.hpp"

namespace {

using game::GameState;
using game::ShipClass;
using game::Stellar;
using game::System;

// A deterministic scenario with one star system and a few travel stellars,
// injected directly (no archive), so the travel geometry/gating is
// unit-testable in isolation.
struct Fixture {
  GameState state;
  System *sys = nullptr;
  ShipClass *cls = nullptr;

  Fixture() {
    // Player in zero-based system 0 (resource id 0x80).
    state.player.current_system_id = 0;
    state.player.ship_class_id = 0;

    System s;
    s.name = "Test System";
    s.nav_defs = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    s.links = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    // Travel stellar in slot 0 -> links[0] = system 0x81.
    s.nav_defs[0] = 0x80;                // stellar resource id 0x80
    s.links[0] = 0x81;                   // destination system resource id
    state.scenario.systems.push_back(s); // index 0 == resource 0x80
    sys = &state.scenario.systems[0];

    // The travel stellar at (100, 0).
    Stellar st;
    st.name = "Travel Point";
    st.pos_x = 100;
    st.pos_y = 0;
    state.scenario.stellars.push_back(st); // index 0 == resource 0x80

    // A starter-class ship with enough fuel to jump (fuel 300 = 3 jumps).
    ShipClass sc;
    sc.base_fuel = 300;
    sc.base_shield = 30;
    sc.base_armor = 30;
    state.scenario.ships.push_back(sc); // index 0 == resource 0x80
    cls = &state.scenario.ships[0];

    state.player.fuel_points = 250.0F;
  }
};

} // namespace

// The nearest-travel-point search is pure geometry over the current system's
// nav-defs (Ghidra Stellar_FindNearestAvailableTravelStellar 0x00462db0).

TEST_CASE("finds the nearest available travel point by world distance",
          "[travel]") {
  Fixture f;
  f.state.player.pos_x = 100.0F; // right on the stellar
  f.state.player.pos_y = 0.0F;
  REQUIRE(game::NovaTravel_FindNearestTravelPoint(f.state) == 0);

  // Farther away but still the only point.
  f.state.player.pos_x = 9000.0F;
  f.state.player.pos_y = 4000.0F;
  REQUIRE(game::NovaTravel_FindNearestTravelPoint(f.state) == 0);
}

TEST_CASE("returns -1 when the current system has no travel point",
          "[travel]") {
  Fixture f;
  f.sys->nav_defs = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  REQUIRE(game::NovaTravel_FindNearestTravelPoint(f.state) == -1);

  // A travel slot naming a stellar that doesn't exist is also skipped.
  f.sys->nav_defs[0] = 0xC0; // no stellar at resource 0xC0
  REQUIRE(game::NovaTravel_FindNearestTravelPoint(f.state) == -1);
}

TEST_CASE("restricted travel stellar requires being within jump range",
          "[travel]") {
  Fixture f;
  f.state.player.pos_x = 100.0F;
  f.state.player.pos_y = 0.0F;
  // Mark the travel point restricted (availability_flags & 0x3000).
  Stellar &st = f.state.scenario.stellars[0];
  st.availability_flags = 0x3000;

  // In range (within 1000): usable.
  REQUIRE(game::NovaTravel_FindNearestTravelPoint(f.state) == 0);
  // Out of range: skipped.
  f.state.player.pos_x = 5000.0F;
  REQUIRE(game::NovaTravel_FindNearestTravelPoint(f.state) == -1);
}

TEST_CASE("nearest travel point picks the closest of several", "[travel]") {
  Fixture f;
  // Second travel stellar (slot 1) nearer the ship.
  f.sys->nav_defs[1] = 0x81;
  f.sys->links[1] = 0x82; // destination system resource 0x82
  Stellar st2;
  st2.name = "Near Point";
  st2.pos_x = 200;
  st2.pos_y = 0;
  st2.availability_flags = 0;
  f.state.scenario.stellars.push_back(st2); // index 1 == resource 0x81

  f.state.player.pos_x = 205.0F;
  f.state.player.pos_y = 0.0F;
  REQUIRE(game::NovaTravel_FindNearestTravelPoint(f.state) == 1);
}

// Jump gating (Ghidra Stellar_CanShipInitiateJumpSequence 0x00415b80).

TEST_CASE("can start a jump only with enough class and held fuel", "[travel]") {
  Fixture f;
  REQUIRE(game::NovaTravel_CanStartJump(f.state)); // class 300, holding 250

  f.state.player.fuel_points = 50.0F; // not enough on hand
  CHECK(!game::NovaTravel_CanStartJump(f.state));

  f.state.player.fuel_points = 250.0F;
  f.cls->base_fuel = 50; // class can't carry a jump's fuel
  CHECK(!game::NovaTravel_CanStartJump(f.state));
}

// The jump state machine (NovaTravel_Tick).

TEST_CASE("travel key near a travel point engages a jump and jumps on timer",
          "[travel]") {
  Fixture f;
  f.state.player.pos_x = 100.0F;
  f.state.player.pos_y = 0.0F;

  // Press travel with no jump currently engaged.
  game::NovaTravel_Tick(
      f.state, /*travel_input=*/true, /*frame_time_ms=*/16.0F);
  CHECK(f.state.travel.engaging);
  CHECK(f.state.travel.destination_system_id == 1); // links[0]=0x81-0x80
  CHECK(f.state.travel.jump_countdown_ticks > 0);
  CHECK(!f.state.travel.just_completed);

  // Keep ticking (travel key held) until the countdown elapses.
  int guard = 0;
  while (f.state.travel.engaging && guard++ < 300) {
    game::NovaTravel_Tick(
        f.state, /*travel_input=*/true, /*frame_time_ms=*/16.0F);
  }
  // The jump landed: new system, fuel burned, refilled, flag set.
  CHECK(f.state.player.current_system_id == 1);
  CHECK(f.state.player.fuel_points == Catch::Approx(250.0F - 100.0F));
  CHECK(f.state.player.shield_points == Catch::Approx(30.0F));
  CHECK(f.state.player.armor_points == Catch::Approx(30.0F));
  CHECK(f.state.travel.just_completed);
  CHECK(!f.state.travel.engaging);
}

TEST_CASE("travel key with no viable destination does not engage", "[travel]") {
  Fixture f;
  f.state.player.pos_x = 100.0F;
  f.state.player.pos_y = 0.0F;
  // links[0] points back at the current system (0x80 == destination 0).
  f.sys->links[0] = 0x80;
  game::NovaTravel_Tick(
      f.state, /*travel_input=*/true, /*frame_time_ms=*/16.0F);
  CHECK(!f.state.travel.engaging);
  CHECK(!f.state.travel.just_completed);
}

TEST_CASE("travel key with no fuel cannot start a jump", "[travel]") {
  Fixture f;
  f.state.player.pos_x = 100.0F;
  f.state.player.pos_y = 0.0F;
  f.state.player.fuel_points = 10.0F;
  game::NovaTravel_Tick(
      f.state, /*travel_input=*/true, /*frame_time_ms=*/16.0F);
  CHECK(!f.state.travel.engaging);
  CHECK(!f.state.travel.just_completed);
}

TEST_CASE("idle travel ticks do nothing while not engaging", "[travel]") {
  Fixture f;
  f.state.player.pos_x = 100.0F;
  f.state.player.pos_y = 0.0F;
  game::NovaTravel_Tick(
      f.state, /*travel_input=*/false, /*frame_time_ms=*/16.0F);
  CHECK(!f.state.travel.engaging);
  CHECK(!f.state.travel.just_completed);
  CHECK(f.state.player.current_system_id == 0);
}
