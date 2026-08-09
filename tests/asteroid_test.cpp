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
