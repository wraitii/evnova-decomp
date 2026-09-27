#include "game/game_state.hpp"
#include "game/ship_ai.hpp"
#include "game/spaceflight.hpp"
#include "game/spaceflight_internal.hpp"

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

TEST_CASE("face-target stores the ship target bearing",
          "[player][face-target]") {
  GameState state;
  state.player.is_active = true;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.ShipAt(1).is_active = true;
  state.player.primary_target_ship_slot = 1;
  FlightInput input;
  input.face_target = true;

  // Bearing 0 = up, increasing clockwise, so a target to the right is 90.
  state.ShipAt(1).pos_x = 100.0F;
  state.ShipAt(1).pos_y = 0.0F;
  CHECK(
      PlayerTick_FaceTargetCommand(state, input, /*arm_modifier_held=*/false));
  CHECK(state.player.ai_desired_heading_deg == 90);

  // Below the player is 180; left is 270 (never negative).
  state.ShipAt(1).pos_x = 0.0F;
  state.ShipAt(1).pos_y = 100.0F;
  CHECK(
      PlayerTick_FaceTargetCommand(state, input, /*arm_modifier_held=*/false));
  CHECK(state.player.ai_desired_heading_deg == 180);

  state.ShipAt(1).pos_x = -100.0F;
  state.ShipAt(1).pos_y = 0.0F;
  CHECK(
      PlayerTick_FaceTargetCommand(state, input, /*arm_modifier_held=*/false));
  CHECK(state.player.ai_desired_heading_deg == 270);

  // With no travel stellar selected, Alt still falls back to the ship target.
  CHECK(PlayerTick_FaceTargetCommand(state, input, /*arm_modifier_held=*/true));
  CHECK(state.player.ai_desired_heading_deg == 270);
}

TEST_CASE("cloak shield drain no longer stops at the per-second rate",
          "[player][cloak]") {
  // BUGFIX(original): the original compares the raw per-second drain rate to
  // the shield pool, so the last `shield_drain` shields persist forever. Under
  // BugFixPolicy the drain continues and clamps at zero. See
  // docs/known_original_bugs.md.
  Ship ship;
  ship.shield_points = 0.1F; // below the 4/sec rate

  spaceflight_detail::NovaShip_ApplyCloakShieldDrain(
      ship, /*shield_drain=*/4, 1.0F, /*apply_fix=*/true);

  CHECK(ship.shield_points == 0.0F);
}

TEST_CASE("player afterburner burns without the forward-thrust key",
          "[player][afterburner]") {
  // Ghidra LAB_00451630: once the afterburner key is held and the outfit is
  // owned, g_player_afterburner_active makes the tail emit its own 2.75x
  // thrust; the forward key is irrelevant.
  GameState state;
  state.player.is_active = true;
  state.player.ship_class_id = 0;
  state.player.fuel_points = 1000.0F;
  state.player.heading = 0.0F;
  state.player.armor_points = 100.0F;
  state.stat_cache_valid = true;
  state.cached_stats.thrust_raw = 500.0F;
  state.cached_stats.speed_raw = 400.0F;
  state.cached_stats.turn_raw = 40.0F;
  state.cached_stats.fuel_capacity = 1000.0F;
  state.scenario.ships.resize(1);
  state.scenario.ships[0].accel = 500.0F;
  state.scenario.ships[0].speed = 400.0F;
  state.scenario.ships[0].turn_rate = 40.0F;
  state.scenario.ships[0].base_armor = 100.0F;
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].mod_type = 15; // kAfterburner
  state.scenario.outfits[0].mod_val = 37;  // fuel units/sec
  state.inventory.outfit_owned_count.fill(0);
  state.inventory.outfit_owned_count[0] = 1;

  FlightInput input;
  input.afterburner = true; // deliberately no input.thrust
  for (int i = 0; i < 60; ++i) {
    PlayerTick_ManualFlightAndRegeneration(state, input, 1.0F, false);
  }
  // Heading 0: 2.75 * 0.1 px/tick^2 along -y, bounded by max * 1.8.
  CHECK(state.player.vel_y < -6.0F);
  CHECK(state.player.fuel_points < 1000.0F);
}

} // namespace game
