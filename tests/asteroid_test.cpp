#include <catch2/catch_test_macros.hpp>

#include "game/asteroid.hpp"
#include "game/game_state.hpp"

namespace game {

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
  // player over the viewport half-size + 0x80 padding, centred with the 0.5
  // shift; the pre-warm writes positions/velocities but not the active flag.
  const float warm_x = static_cast<float>(state.viewport_center_x + 0x80);
  const float warm_y = static_cast<float>(state.viewport_center_y + 0x80);
  for (const auto &m : state.asteroid_pool) {
    CHECK(m.target_pos_x >= 1000.0F - warm_x / 2.0F);
    CHECK(m.target_pos_x < 1000.0F + warm_x / 2.0F);
    CHECK(m.target_pos_y >= 2000.0F - warm_y / 2.0F);
    CHECK(m.target_pos_y < 2000.0F + warm_y / 2.0F);
  }

  // Re-initialising clears the previous field before respawning (so a system
  // change or launch never inherits the old system's live records).
  for (auto &m : state.asteroid_pool) {
    m.active = true;
  }
  state.no_asteroids_latch = true;
  NovaAsteroid_InitSystem(state);
  CHECK(!state.no_asteroids_latch);
  active = 0;
  for (const auto &m : state.asteroid_pool) {
    active += m.active ? 1 : 0;
  }
  CHECK(active == 3);

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

// NovaAsteroid_UpdateSprites (Asteroid_UpdateSprites 0x00436910, simulation
// half): integrates position by velocity and the wander accumulator by
// wander_speed, and deactivates records while the no-asteroids latch is set.
TEST_CASE("asteroid drift tick integrates position and wander", "[asteroid]") {
  GameState state;
  AsteroidState &m = state.asteroid_pool[0];
  m.active = true;
  m.target_pos_x = 100.0F;
  m.target_pos_y = 200.0F;
  m.target_vel_x = 1.5F;
  m.target_vel_y = -0.5F;
  m.wander_frame_accumulator = 3.0F;
  m.wander_speed = 2.0F;

  NovaAsteroid_UpdateSprites(state, 2.0F);
  CHECK(m.active);
  CHECK(m.target_pos_x == 103.0F);
  CHECK(m.target_pos_y == 199.0F);
  CHECK(m.wander_frame_accumulator == 7.0F);

  // A zero tick (frozen gameplay) leaves the record untouched.
  NovaAsteroid_UpdateSprites(state, 0.0F);
  CHECK(m.target_pos_x == 103.0F);
  CHECK(m.wander_frame_accumulator == 7.0F);

  // The no-asteroids latch deactivates every live record.
  state.no_asteroids_latch = true;
  NovaAsteroid_UpdateSprites(state, 1.0F);
  CHECK_FALSE(m.active);

  // The -32000 hidden sentinel also retires a record.
  state.no_asteroids_latch = false;
  m.active = true;
  m.integrity = -32000;
  NovaAsteroid_UpdateSprites(state, 1.0F);
  CHECK_FALSE(m.active);
}

} // namespace game
