#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "game/asteroid.hpp"
#include "game/game_state.hpp"

namespace game {

// NovaAsteroid_SpawnRecord (Asteroid_SpawnRecord 0x00421e60): the first
// inactive pool slot is claimed, gains the asteroid type, spawn position, a
// scattered target velocity, and the per-type wander values from the loaded
// asteroid-type table. Values follow the original's random-factor recipe
// (rand(200)-100 scaled by 0.01 for the velocity, etc.).
TEST_CASE("asteroid spawn claims free slots and scatters a wander target",
          "[asteroid]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Type 0 = Metal Small.
  const int slot = NovaAsteroid_SpawnRecord(state, 100.0F, 200.0F, 0);
  REQUIRE(slot == 0);

  const AsteroidState &m = state.asteroid_pool[0];
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
    const int s = NovaAsteroid_SpawnRecord(state, 0.0F, 0.0F, 3);
    REQUIRE(s == count);
  }
  CHECK(state.asteroid_pool[15].active);
  // Very likely every slot claims a non-zero (or zero) speed; just assert the
  // full pool is busy and a further spawn fails.
  int busy = 0;
  for (const auto &ent : state.asteroid_pool) {
    busy += ent.active ? 1 : 0;
  }
  CHECK(busy == 16);
  CHECK(NovaAsteroid_SpawnRecord(state, 0.0F, 0.0F, 0) == -1);
}

// NovaAsteroid_Spawn (Asteroid_Spawn 0x00421830): the asteroid allocator
// bails when the system has no asteroids / a clear ast_types mask, respects
// the asteroid quota, and claims the first free pool slot whose wander
// direction is permitted by the system ast_types mask when one is available.
// These records are ASTEROID/drift-debris chars (r\xf6id family), not NPC
// ships.
TEST_CASE("asteroid spawn allocates an ast_types-valid slot", "[asteroid]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Kania is index 0 -> system 0x80: asteroid_count 3, ast_types 0x0711.
  state.player.current_system_id = 0;
  state.player.pos_x = 500.0F;
  state.player.pos_y = 300.0F;

  const System *sys = state.scenario.System(0x80);
  REQUIRE(sys != nullptr);
  REQUIRE(sys->asteroid_count == 3);
  REQUIRE(sys->ast_types == 0x0711);

  // Allocate up to the asteroid quota.
  int slot = NovaAsteroid_Spawn(state, /*place_in_ring=*/false);
  REQUIRE(slot == 0);
  CHECK(state.asteroid_pool[0].active);
  // Direction bit must be in the system ast_types mask.
  CHECK((sys->ast_types &
         (1U << (state.asteroid_pool[0].wander_type & 0x1f))) != 0);
  // Scatter position is a [-rx*0.5, +rx*0.5) band around the player.
  CHECK(state.asteroid_pool[0].target_pos_x >= 500.0F - 64.0F);
  CHECK(state.asteroid_pool[0].target_pos_x < 500.0F + 64.0F);
  CHECK(state.asteroid_pool[0].target_pos_y >= 300.0F - 64.0F);
  CHECK(state.asteroid_pool[0].target_pos_y < 300.0F + 64.0F);

  REQUIRE(NovaAsteroid_Spawn(state, false) == 1);
  REQUIRE(NovaAsteroid_Spawn(state, false) == 2);
  // Quota of 3 is reached; a further spawn is a no-op (no new slot).
  CHECK(NovaAsteroid_Spawn(state, false) == -1);
}

// NovaAsteroid_InitSystem (Asteroid_InitSystem 0x004216B0): for a populated
// system it allocates asteroid_count records and pre-warms all 16 pool slots;
// for an empty system it sets the no-asteroids latch instead.
TEST_CASE("system init restores the asteroid population and pre-warms the pool",
          "[asteroid]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0; // Kania, asteroid_count 3
  state.player.pos_x = 1000.0F;
  state.player.pos_y = 2000.0F;

  NovaAsteroid_InitSystem(state);
  CHECK(!state.no_asteroids_latch);

  int active = 0;
  for (const auto &m : state.asteroid_pool) {
    active += m.active ? 1 : 0;
  }
  CHECK(active == 3);

  // Every slot (active or not) got a pre-warmed scatter target around the
  // player; the pre-warm writes positions/velocities but not the active flag.
  for (const auto &m : state.asteroid_pool) {
    CHECK(m.target_pos_x >= 1000.0F - 64.0F);
    CHECK(m.target_pos_x < 1000.0F + 64.0F);
    CHECK(m.target_pos_y >= 2000.0F - 64.0F);
    CHECK(m.target_pos_y < 2000.0F + 64.0F);
  }

  // A system with no asteroids sets the latch instead of spawning.
  GameState empty;
  REQUIRE(empty.scenario.LoadFromArchives());
  empty.player.current_system_id = 0;
  const System *s0 = empty.scenario.System(0x80);
  REQUIRE(s0 != nullptr);
  // Force Kania's asteroid_count to 0 so the no-asteroid latch path runs
  // deterministically. ScenarioData exposes the element as const; the vector is
  // genuinely mutable, so casting off const to perturb this one test copy is a
  // deliberate, local exception.
  const_cast<System &>(*s0).asteroid_count = 0;
  NovaAsteroid_InitSystem(empty);
  CHECK(empty.no_asteroids_latch);
}

} // namespace game
