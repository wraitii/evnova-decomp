#include "game/game_state.hpp"
#include "game/ship_visual.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace game {
namespace {

// Builds a single-class scenario with the running-lights and weapon-effects
// fields the visual tick reads, indexed as class resource id 0x80.
void SetShipClass(GameState &state, const ShipClass &cls) {
  state.scenario.ships.assign(1, cls);
}

} // namespace

TEST_CASE("running lights square wave blinks on and off", "[ship][visual]") {
  GameState state;
  ShipClass cls;
  cls.light_image_id = 1; // layer present
  cls.blink_mode = 1;     // square wave
  cls.blink_val_a = 4;    // off-time
  cls.blink_val_b = 1;    // on-time
  cls.blink_val_c = 2;    // blinks per group
  cls.blink_val_d = 20;   // group delay
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_class_id = 0;

  // Tick 1: timer expired at zero -> light on for BlinkValB ticks.
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.light_intensity == 32.0F);
  CHECK(ship.light_blink_timer == 1.0F);
  CHECK(ship.light_blink_phase == 0);

  // Tick 2: on-time elapses.
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.light_intensity == 32.0F);
  CHECK(ship.light_blink_timer == 0.0F);

  // Tick 3: off for BlinkValA ticks, first blink counted.
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.light_intensity == 0.0F);
  CHECK(ship.light_blink_timer == 4.0F);
  CHECK(ship.light_blink_phase == 1);

  for (int i = 0; i < 4; ++i) {
    NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  }
  // Off-time expired -> second blink on.
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.light_intensity == 32.0F);
  CHECK(ship.light_blink_phase == 1);

  // Finish the second on/off leg and enter the group delay.
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F); // on elapses
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F); // off
  for (int i = 0; i < 5; ++i) {
    NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  }
  CHECK(ship.light_blink_phase == 0);
  CHECK(ship.light_intensity == 0.0F);
  CHECK(ship.light_blink_timer == 20.0F);
}

TEST_CASE("running lights steady mode stays at full brightness",
          "[ship][visual]") {
  GameState state;
  ShipClass cls;
  cls.light_image_id = 1;
  cls.blink_mode = -1;
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_class_id = 0;

  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.light_intensity == 32.0F);
}

TEST_CASE("running lights triangle pulse ramps to the ceiling",
          "[ship][visual]") {
  GameState state;
  ShipClass cls;
  cls.light_image_id = 1;
  cls.blink_mode = 2;
  cls.blink_val_a = 10;  // floor
  cls.blink_val_b = 100; // rise 1.0 per tick (x100)
  cls.blink_val_c = 32;  // ceiling
  cls.blink_val_d = 100; // fall 1.0 per tick (x100)
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_class_id = 0;

  for (int i = 0; i < 5; ++i) {
    NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  }
  CHECK(ship.light_intensity == 5.0F);
  CHECK(ship.light_blink_phase == 0);

  for (int i = 0; i < 28; ++i) {
    NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  }
  CHECK(ship.light_intensity == 32.0F);
  CHECK(ship.light_blink_phase == 1);

  // Falling leg stops at the floor.
  for (int i = 0; i < 23; ++i) {
    NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  }
  CHECK(ship.light_intensity == 10.0F);
  CHECK(ship.light_blink_phase == 0);
}

TEST_CASE("running lights random pulse draws within the min/max band",
          "[ship][visual]") {
  GameState state;
  state.rng.seed(0x5eedU);
  ShipClass cls;
  cls.light_image_id = 1;
  cls.blink_mode = 3;
  cls.blink_val_a = 5;  // min
  cls.blink_val_b = 25; // max
  cls.blink_val_c = 7;  // change delay
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_class_id = 0;

  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.light_intensity >= 5.0F);
  CHECK(ship.light_intensity <= 25.0F);
  CHECK(ship.light_blink_timer == 7.0F);

  // The delay counts down; the intensity holds until it expires.
  const float held = ship.light_intensity;
  for (int i = 0; i < 6; ++i) {
    NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  }
  CHECK(ship.light_intensity == held);
  CHECK(ship.light_blink_timer == 1.0F);
}

TEST_CASE("weapon-effects flash decays at the class rate and latches off",
          "[ship][visual]") {
  GameState state;
  ShipClass cls;
  cls.weapon_glow_decay_rate = 0.5F;
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_class_id = 0;
  ship.weapon_sprite_flash_level = 32.0F;

  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.weapon_sprite_flash_level == 31.5F);

  // The ceiling clamps oversized values before decaying.
  ship.weapon_sprite_flash_level = 100.0F;
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.weapon_sprite_flash_level == 31.5F);

  // A small negative overshoot simply hides the layer; it is not clamped.
  ship.weapon_sprite_flash_level = 0.4F;
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.weapon_sprite_flash_level < 0.0F);
  CHECK(ship.weapon_sprite_flash_level > -1.0F);

  // A decay that overshoots below -1.0 clamps to -1.0.
  cls.weapon_glow_decay_rate = 2.0F;
  SetShipClass(state, cls);
  ship.weapon_sprite_flash_level = 0.4F;
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.weapon_sprite_flash_level == -1.0F);
}

} // namespace game
