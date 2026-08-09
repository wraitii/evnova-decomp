#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"

namespace {

using game::GameState;
using game::Ship;

TEST_CASE("GameState exposes player as ships_[0] (g_ship_states convention)") {
  GameState state;

  // The `player` convenience reference aliases ships_[0], so writes through
  // either handle reach the same object (the original's g_ship_states[0]).
  state.player.pos_x = 123.0F;
  state.player.speed = 45.0F;
  CHECK(state.ships_[0].pos_x == Catch::Approx(123.0F));
  CHECK(state.ships_[0].speed == Catch::Approx(45.0F));

  state.ships_[0].heading = 2.0F;
  CHECK(state.player.heading == Catch::Approx(2.0F));
}

TEST_CASE("NPC ship slots are distinct from the player slot") {
  GameState state;
  state.player.ship_class_id = 1;

  Ship &npc = state.ShipAt(1);
  npc.ship_class_id = 7;
  npc.faction_or_government_id = 4;
  npc.is_active = true;

  CHECK(state.player.ship_class_id == 1);    // unaffected
  CHECK(state.ShipAt(1).ship_class_id == 7); // NPC write landed
  CHECK(state.ShipAt(1).faction_or_government_id == 4);
  CHECK(state.ShipAt(1).is_active);

  // Defaults mirror Ship_AllocateShipSlotInSystem's expectations later: new
  // NPC slots start inactive with no target/hostility.
  Ship const &fresh = state.ShipAt(2);
  CHECK_FALSE(fresh.is_active);
  CHECK(fresh.primary_target_ship_slot == -1);
  CHECK(fresh.ai_target_ship_slot == -1);
  CHECK(fresh.faction_or_government_id == -1);
  CHECK(fresh.mission_fleet_slot == -1);
}

TEST_CASE("ship slot bounds are enforced by SlotInRange") {
  GameState state;
  CHECK(state.SlotInRange(0));
  CHECK(state.SlotInRange(GameState::kMaxShips - 1));
  CHECK_FALSE(state.SlotInRange(GameState::kMaxShips));
  CHECK_FALSE(state.SlotInRange(GameState::kMaxShips + 5));
}

} // namespace
