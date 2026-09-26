#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/world_wrap.hpp"

namespace {

constexpr float kTrigger = 15000.0F;
constexpr float kDelta = 25000.0F;

} // namespace

TEST_CASE("relative wrap shifts only positions inside the pull-in radius",
          "[world-wrap]") {
  float inside_x = 100.0F;
  float inside_y = 100.0F;
  game::WorldWrapPositionRelativeToPlayer(
      inside_x, inside_y, 0.0F, 0.0F, -kDelta, kDelta);
  CHECK(inside_x == Catch::Approx(100.0F - kDelta));
  CHECK(inside_y == Catch::Approx(100.0F + kDelta));

  // Exactly at the radius is outside (the original truncates before the
  // unsigned "less than 15000" test, and 15000 is not < 15000).
  float edge_x = kTrigger;
  float edge_y = 0.0F;
  game::WorldWrapPositionRelativeToPlayer(
      edge_x, edge_y, 0.0F, 0.0F, -kDelta, kDelta);
  CHECK(edge_x == Catch::Approx(kTrigger));
  CHECK(edge_y == Catch::Approx(0.0F));

  // Outside on one axis only: the other axis is not enough.
  float far_x = 0.0F;
  float far_y = kTrigger + 1.0F;
  game::WorldWrapPositionRelativeToPlayer(
      far_x, far_y, 0.0F, 0.0F, -kDelta, kDelta);
  CHECK(far_x == Catch::Approx(0.0F));
  CHECK(far_y == Catch::Approx(kTrigger + 1.0F));
}

TEST_CASE("recenter is a no-op while the player is inside the band",
          "[world-wrap]") {
  game::GameState state;
  state.player.pos_x = kTrigger;
  state.player.pos_y = -kTrigger;
  state.ShipAt(1).pos_x = 1234.0F;

  const game::WorldWrapDelta delta =
      game::RecenterSpaceObjectsForWorldWrap(state);

  CHECK_FALSE(delta.applied());
  CHECK(state.player.pos_x == Catch::Approx(kTrigger));
  CHECK(state.player.pos_y == Catch::Approx(-kTrigger));
  CHECK(state.ShipAt(1).pos_x == Catch::Approx(1234.0F));
}

TEST_CASE("recenter pulls the player and nearby pools back across the boundary",
          "[world-wrap]") {
  game::GameState state;
  state.player.pos_x = kTrigger + 1.0F;
  state.player.pos_y = 0.0F;

  // Nearby ship: inside the radius, so it travels with the player.
  game::Ship &near_ship = state.ShipAt(1);
  near_ship.pos_x = state.player.pos_x + 100.0F;
  near_ship.pos_y = 50.0F;
  // Far ship: outside the radius, so it stays put.
  game::Ship &far_ship = state.ShipAt(2);
  far_ship.pos_x = state.player.pos_x + kTrigger + 1.0F;
  far_ship.pos_y = 0.0F;

  state.active_shots.push_back(game::ActiveShot{});
  state.active_shots.back().pos_x = 500.0F;
  state.active_shots.back().pos_y = 500.0F;
  state.impact_effect_instances[0].pos_x = 600.0F;
  state.impact_effect_instances[0].pos_y = 600.0F;
  state.freeflight_objects[0].pos_x = 700.0F;
  state.freeflight_objects[0].pos_y = 700.0F;
  state.weapon_smoke_puffs[0].pos_x = 800.0F;
  state.weapon_smoke_puffs[0].pos_y = 800.0F;
  state.fading_effect_instances[0].pos_x = 900.0F;
  state.fading_effect_instances[0].pos_y = 900.0F;
  state.beam_hit_queue[0].source_x = 1000.0F;
  state.beam_hit_queue[0].source_y = 1100.0F;
  state.beam_hit_queue[0].target_x = 1200.0F;
  state.beam_hit_queue[0].target_y = 1300.0F;

  const game::WorldWrapDelta delta =
      game::RecenterSpaceObjectsForWorldWrap(state);

  REQUIRE(delta.applied());
  CHECK(delta.x == Catch::Approx(-kDelta));
  CHECK(delta.y == Catch::Approx(0.0F));

  // The player is recentered by the delta.
  CHECK(state.player.pos_x == Catch::Approx(kTrigger + 1.0F - kDelta));
  CHECK(state.player.pos_y == Catch::Approx(0.0F));
  // Nearby objects shift with it.
  CHECK(near_ship.pos_x == Catch::Approx(kTrigger + 101.0F - kDelta));
  CHECK(near_ship.pos_y == Catch::Approx(50.0F));
  // The far ship does not.
  CHECK(far_ship.pos_x == Catch::Approx(kTrigger + 1.0F + kTrigger + 1.0F));
  for (const auto &shot : state.active_shots) {
    CHECK(shot.pos_x == Catch::Approx(500.0F - kDelta));
    CHECK(shot.pos_y == Catch::Approx(500.0F));
  }
  CHECK(state.impact_effect_instances[0].pos_x ==
        Catch::Approx(600.0F - kDelta));
  CHECK(state.freeflight_objects[0].pos_x == Catch::Approx(700.0F - kDelta));
  CHECK(state.weapon_smoke_puffs[0].pos_x == Catch::Approx(800.0F - kDelta));
  CHECK(state.fading_effect_instances[0].pos_x ==
        Catch::Approx(900.0F - kDelta));
  CHECK(state.beam_hit_queue[0].source_x == Catch::Approx(1000.0F - kDelta));
  CHECK(state.beam_hit_queue[0].source_y == Catch::Approx(1100.0F));
  CHECK(state.beam_hit_queue[0].target_x == Catch::Approx(1200.0F - kDelta));
  CHECK(state.beam_hit_queue[0].target_y == Catch::Approx(1300.0F));
}

TEST_CASE("recenter handles the negative boundary and both axes",
          "[world-wrap]") {
  game::GameState state;
  state.player.pos_x = -kTrigger - 1.0F;
  state.player.pos_y = kTrigger + 1.0F;

  const game::WorldWrapDelta delta =
      game::RecenterSpaceObjectsForWorldWrap(state);

  REQUIRE(delta.applied());
  CHECK(delta.x == Catch::Approx(kDelta));
  CHECK(delta.y == Catch::Approx(-kDelta));
  CHECK(state.player.pos_x == Catch::Approx(-kTrigger - 1.0F + kDelta));
  CHECK(state.player.pos_y == Catch::Approx(kTrigger + 1.0F - kDelta));
}
