#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "game/game_state.hpp"
#include "game/maneuver.hpp"

namespace game {

// NovaManeuver_SpawnState (Frame_SpawnScriptedManeuverState 0x00421e60): the
// first inactive pool slot is claimed, gains the manoeuvre type, spawn
// position, a scattered target velocity, and the per-type wander values from
// the loaded manoeuvre-type table. Values follow the original's random-factor
// recipe (rand(200)-100 scaled by 0.01 for the velocity, etc.).
TEST_CASE("maneuver spawn claims free slots and scatters a wander target",
          "[maneuver]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Type 0 = Metal Small.
  const int slot = NovaManeuver_SpawnState(state, 100.0F, 200.0F, 0);
  REQUIRE(slot == 0);

  const ManeuverState &m = state.maneuver_pool[0];
  CHECK(m.active);
  CHECK(m.wander_type == 0);
  CHECK(m.target_pos_x == 100.0F);
  CHECK(m.target_pos_y == 200.0F);
  // Velocity is [-1,1) per axis (rand(200)-100 scaled by 0.01).
  CHECK(m.target_vel_x >= -1.0F);
  CHECK(m.target_vel_x < 1.0F);
  CHECK(m.target_vel_y >= -1.0F);
  CHECK(m.target_vel_y < 1.0F);
  // Wander radius is in [0, Metal Small lifetime) and the table value matches
  // the decoded row.
  CHECK(m.wander_radius >= 0.0F);
  CHECK(m.wander_radius < 150.0F);
  CHECK(m.wander_table_value == 100);

  // Fill the whole pool; the next spawn reports no free slot.
  int count = 1;
  for (; count < 16; ++count) {
    const int s = NovaManeuver_SpawnState(state, 0.0F, 0.0F, 3);
    REQUIRE(s == count);
  }
  CHECK(state.maneuver_pool[15].active);
  // Very likely every slot claims a non-zero (or zero) speed; just assert the
  // full pool is busy and a further spawn fails.
  int busy = 0;
  for (const auto &ent : state.maneuver_pool) {
    busy += ent.active ? 1 : 0;
  }
  CHECK(busy == 16);
  CHECK(NovaManeuver_SpawnState(state, 0.0F, 0.0F, 0) == -1);
}

// NovaDude_SpawnRoamingShip (Dude_SpawnRoamingShip 0x00421830): the roaming
// allocator bails when the system has no roaming ships / a clear direction
// bitmap, respects the roaming quota, and claims the first free pool slot whose
// wander direction is permitted by the system bitmap when one is available.
TEST_CASE("roaming spawn allocates a direction-bitmap-valid manoeuvre slot",
          "[maneuver][roaming]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Kania is index 0 -> system 0x80: roaming_ship_count 3, bitmap 0x0711.
  state.player.current_system_id = 0;
  state.player.pos_x = 500.0F;
  state.player.pos_y = 300.0F;

  const System *sys = state.scenario.System(0x80);
  REQUIRE(sys != nullptr);
  REQUIRE(sys->roaming_ship_count == 3);
  REQUIRE(sys->roaming_direction_bitmap == 0x0711);

  // Allocate up to the roaming quota.
  int slot = NovaDude_SpawnRoamingShip(state, /*place_in_ring=*/false);
  REQUIRE(slot == 0);
  CHECK(state.maneuver_pool[0].active);
  // Direction bit must be in the system bitmap.
  CHECK((sys->roaming_direction_bitmap &
         (1U << (state.maneuver_pool[0].wander_type & 0x1f))) != 0);
  // Scatter position is a [-rx*0.5, +rx*0.5) band around the player.
  CHECK(state.maneuver_pool[0].target_pos_x >= 500.0F - 64.0F);
  CHECK(state.maneuver_pool[0].target_pos_x < 500.0F + 64.0F);
  CHECK(state.maneuver_pool[0].target_pos_y >= 300.0F - 64.0F);
  CHECK(state.maneuver_pool[0].target_pos_y < 300.0F + 64.0F);

  REQUIRE(NovaDude_SpawnRoamingShip(state, false) == 1);
  REQUIRE(NovaDude_SpawnRoamingShip(state, false) == 2);
  // Quota of 3 is reached; a further spawn is a no-op (no new slot).
  CHECK(NovaDude_SpawnRoamingShip(state, false) == -1);
}

// NovaSystem_InitRoamingShips (System_InitRoamingShips 0x004216B0): for a
// populated system it allocates roaming_ship_count records and pre-warms all 16
// pool slots; for an empty system it sets the no-ships latch instead.
TEST_CASE("system init restores the roaming population and pre-warms the pool",
          "[maneuver][roaming]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0; // Kania, roaming_ship_count 3
  state.player.pos_x = 1000.0F;
  state.player.pos_y = 2000.0F;

  NovaSystem_InitRoamingShips(state);
  CHECK(!state.no_roaming_ships_latch);

  int active = 0;
  for (const auto &m : state.maneuver_pool) {
    active += m.active ? 1 : 0;
  }
  CHECK(active == 3);

  // Every slot (active or not) got a pre-warmed scatter target around the
  // player; the pre-warm writes positions/velocities but not the active flag.
  for (const auto &m : state.maneuver_pool) {
    CHECK(m.target_pos_x >= 1000.0F - 64.0F);
    CHECK(m.target_pos_x < 1000.0F + 64.0F);
    CHECK(m.target_pos_y >= 2000.0F - 64.0F);
    CHECK(m.target_pos_y < 2000.0F + 64.0F);
  }

  // A system with no roaming ships sets the latch instead of spawning.
  GameState empty;
  REQUIRE(empty.scenario.LoadFromArchives());
  empty.player.current_system_id = 0;
  const System *s0 = empty.scenario.System(0x80);
  REQUIRE(s0 != nullptr);
  // Force Kania's roaming_ship_count to 0 so the no-ship latch path runs
  // deterministically. ScenarioData exposes the element as const; the vector is
  // genuinely mutable, so casting off const to perturb this one test copy is a
  // deliberate, local exception.
  const_cast<System &>(*s0).roaming_ship_count = 0;
  NovaSystem_InitRoamingShips(empty);
  CHECK(empty.no_roaming_ships_latch);
}

} // namespace game
