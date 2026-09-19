#include "game/game_state.hpp"
#include "game/ship_ai.hpp"
#include "game/spaceflight.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>

namespace game {

TEST_CASE("armed escape-pod timed action yields the destroyed player frame",
          "[player][timed-action][eject]") {
  GameState state;
  PlayerShip &p = state.player;
  p.is_active = true;
  p.ship_class_id = kEscapePodShipClassIndex;
  p.shield_points = 0.0F;
  p.armor_points = 0.0F; // The pod reads as destroyed from birth.
  p.timed_action_counter = 0x15e;

  // The destroyed branch must return false so the spaceflight loop can run
  // PlayerTick_TimedActionTransition (the original reaches 0x0044d490 before
  // the death/eject block at 0x00451024); otherwise the pod neither moves nor
  // counts down and never respawns.
  CHECK_FALSE(PlayerTick_StatusAndOutfitEvents(state, 1.0F, /*eject=*/false));
  CHECK(p.timed_action_counter == 0x15e);

  // Without an armed timed action the same destroyed hull consumes the frame
  // (death bookkeeping owns the presentation).
  p.timed_action_counter = -1;
  CHECK(PlayerTick_StatusAndOutfitEvents(state, 1.0F, /*eject=*/false));
}

namespace {

// A player in class 0 (resource 0x80) with the escape-pod class 0x2ff present.
void SetUpEjectScenario(GameState &state) {
  state.scenario.ships.assign(0x300, ShipClass{});
  ShipClass player_cls;
  player_cls.base_armor = 100;
  player_cls.base_shield = 100;
  player_cls.death_delay_frames = 20;
  state.scenario.ships[0] = player_cls;
  ShipClass pod;
  pod.death_delay_frames = 20;
  state.scenario.ships[kEscapePodShipClassIndex] = pod;
}

void OwnOutfits(GameState &state,
                std::initializer_list<std::int16_t> mod_types) {
  state.scenario.outfits.assign(mod_types.size(), Outfit{});
  state.inventory.outfit_owned_count.fill(0);
  std::size_t idx = 0;
  for (const std::int16_t mod_type : mod_types) {
    state.scenario.outfits[idx].mod_type = mod_type;
    state.inventory.outfit_owned_count[idx] = 1;
    ++idx;
  }
}

} // namespace

TEST_CASE("disabled player ejects manually with the arm-modifier pair",
          "[player][eject]") {
  GameState state;
  SetUpEjectScenario(state);
  OwnOutfits(state, {0x0b}); // escape pod

  PlayerShip &p = state.player;
  p.is_active = true;
  p.ship_class_id = 0;
  p.armor_points = 1.0F; // Below the 33% disabled gate, above 0.

  REQUIRE(NovaAiShip_IsDisabled(state, p));
  CHECK(PlayerTick_StatusAndOutfitEvents(state, 1.0F, /*eject=*/true));
  CHECK(p.ship_class_id == kEscapePodShipClassIndex);
  CHECK(p.timed_action_counter == 0x15e);
}

TEST_CASE("disabled player without an escape-pod outfit cannot eject",
          "[player][eject]") {
  GameState state;
  SetUpEjectScenario(state);

  PlayerShip &p = state.player;
  p.is_active = true;
  p.ship_class_id = 0;
  p.armor_points = 1.0F;

  REQUIRE(NovaAiShip_IsDisabled(state, p));
  CHECK_FALSE(PlayerTick_StatusAndOutfitEvents(state, 1.0F, /*eject=*/true));
  CHECK(p.ship_class_id == 0);
}

TEST_CASE("destroyed player auto-ejects without a key press",
          "[player][eject]") {
  GameState state;
  SetUpEjectScenario(state);
  OwnOutfits(state, {0x14, 0x0b}); // auto-eject + escape pod

  PlayerShip &p = state.player;
  p.is_active = true;
  p.ship_class_id = 0;
  p.armor_points = 0.0F;       // destroyed
  p.death_timer_active = 0.0F; // past the half-spent gate

  // The original ejects automatically when a destroyed hull owns auto-eject;
  // no Alt+X is required.
  CHECK(PlayerTick_StatusAndOutfitEvents(state, 1.0F, /*eject=*/false));
  CHECK(p.ship_class_id == kEscapePodShipClassIndex);
  CHECK(p.timed_action_counter == 0x15e);
}

} // namespace game
