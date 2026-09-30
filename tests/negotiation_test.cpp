#include "game/game_state.hpp"
#include "game/new_pilot_flow.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace game;

// g_travel_interaction_bribe_random_latch (0x007d17dc) is a per-system-visit
// latch for the stellar destination-interaction window: it is rolled once per
// visit (only while negative) and reused by every window, a declined haggle
// writes 0, and the player-ship reset (0x004b3a0b) / jump-arrival block
// (0x0044f81a) re-arm it to -1. The dialog itself is modal and not
// unit-testable here; these pin the reset wiring so the persistence cannot
// silently regress. See src/game/negotiation_dialog.cpp.
TEST_CASE("travel state defaults the bribe latch to unarmed") {
  CHECK(TravelState{}.bribe_random_latch == -1);
}

TEST_CASE("a player-ship reset re-arms the bribe latch") {
  GameState state;
  state.travel.bribe_random_latch = 0; // as a declined haggle leaves it

  NovaShip_ResetPlayerShipState(state);

  CHECK(state.travel.bribe_random_latch == -1);
}
