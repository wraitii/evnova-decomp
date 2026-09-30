#include "game/game_state.hpp"
#include "game/mission.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace game;

// Ghidra 0x00423540 Player_CollectStellarTribute. The daily tribute pass over
// available stellars carrying the dominated (+0x46) marker: each one pays its
// Tribute and bumps its day counter, except that an "always dominated" (0x20)
// docked body suppresses only the counter. There is no test coverage of the
// pass otherwise -- the pilot-file tests cover the persisted counter but never
// the income itself.
TEST_CASE("daily tribute pays dominated stellars and bumps the day counter") {
  GameState state;
  state.scenario.stellars.resize(2);

  Stellar &paid = state.scenario.stellars[0];
  paid.is_available = true;
  paid.dominated = 1;
  paid.tribute = 1234;
  paid.domination_days = 5;

  Stellar &not_dominated = state.scenario.stellars[1];
  not_dominated.is_available = true;
  not_dominated.dominated = 0;
  not_dominated.tribute = 999;
  not_dominated.domination_days = 3;

  state.player.credits = 100;
  state.player.ai_secondary_target_slot = -1;

  Player_CollectStellarTribute(state);

  CHECK(state.player.credits == 100 + 1234);
  CHECK(paid.domination_days == 6);
  // A free body neither pays nor counts a day.
  CHECK(not_dominated.domination_days == 3);
}

TEST_CASE("an unavailable dominated stellar is skipped") {
  GameState state;
  state.scenario.stellars.resize(1);
  Stellar &stellar = state.scenario.stellars[0];
  stellar.is_available = false;
  stellar.dominated = 1;
  stellar.tribute = 500;
  stellar.domination_days = 7;

  state.player.credits = 0;
  state.player.ai_secondary_target_slot = -1;

  Player_CollectStellarTribute(state);

  CHECK(state.player.credits == 0);
  CHECK(stellar.domination_days == 7);
}

// The exception reads the player's ai_secondary_target_slot (the dock stellar
// while landed; the travel selection is cleared on landing). Resource id 0x80
// is zero-based index 0, so the docked body and the paying body are the same.
TEST_CASE(
    "an always-dominated docked stellar suppresses only the day counter") {
  GameState state;
  state.scenario.stellars.resize(1);
  Stellar &stellar = state.scenario.stellars[0];
  stellar.is_available = true;
  stellar.dominated = 1;
  stellar.availability_flags = 0x20; // Bible "starts the game dominated"
  stellar.tribute = 500;
  stellar.domination_days = 7;

  state.player.credits = 0;
  state.player.ai_secondary_target_slot = 0x80;

  Player_CollectStellarTribute(state);

  // Still pays out; only the stelAnnoyance counter is held back.
  CHECK(state.player.credits == 500);
  CHECK(stellar.domination_days == 7);
}

// Regression: the pass must read player.ai_secondary_target_slot, not the
// travel selection. Point the always-dominated marker at the travel selection
// only; the counter must still advance for the paying body.
TEST_CASE("tribute reads the docked body, not the travel selection") {
  GameState state;
  state.scenario.stellars.resize(2);
  Stellar &stellar = state.scenario.stellars[0];
  stellar.is_available = true;
  stellar.dominated = 1;
  stellar.tribute = 10;
  stellar.domination_days = 0;

  Stellar &always_dominated = state.scenario.stellars[1];
  always_dominated.is_available = true;
  always_dominated.dominated = 1;
  always_dominated.availability_flags = 0x20;

  // The travel selection points at the always-dominated body, but the player
  // is docked at the ordinary one (index 0). The exception must follow the
  // docked field, so both counters advance.
  state.travel.selected_stellar_id = 0x81;
  state.player.ai_secondary_target_slot = 0x80;

  Player_CollectStellarTribute(state);

  CHECK(stellar.domination_days == 1);
  CHECK(always_dominated.domination_days == 1);
}
