#include "game/game_state.hpp"
#include "game/ship_visual.hpp"

#include <catch2/catch_approx.hpp>
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

TEST_CASE("passive cloak recovery follows the original raw-call cadence",
          "[ship][visual][timing]") {
  GameState state;
  SetShipClass(state, ShipClass{});
  Ship ship;
  ship.ship_class_id = 0;
  ship.armor_points = 1.0F;
  ship.cloak_transition_latch = 0;

  // At the original 21 ms floor, g_avg_frame_tick_scale is 0.63 and the
  // unscaled original branch subtracts exactly one unit per call.
  ship.cloak_fade_progress = 16.0F;
  NovaShip_TickCloakFadeState(state, ship, 0.63F);
  CHECK(ship.cloak_fade_progress == Catch::Approx(15.0F));

  // A 60 Hz update is 0.5 normalized ticks. Time adjustment prevents the
  // former one-unit-per-display-frame speedup.
  ship.cloak_fade_progress = 16.0F;
  NovaShip_TickCloakFadeState(state, ship, 0.5F);
  CHECK(ship.cloak_fade_progress == Catch::Approx(16.0F - 0.5F / 0.63F));

  // Preserve the original's lack of a zero clamp on the passive branch.
  ship.cloak_fade_progress = 0.5F;
  NovaShip_TickCloakFadeState(state, ship, 0.63F);
  CHECK(ship.cloak_fade_progress == Catch::Approx(-0.5F));
}

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

TEST_CASE("engine glow brightness tracks level with a random flicker",
          "[ship][visual]") {
  GameState state;
  state.rng.seed(0x5eedU);
  ShipClass cls;
  cls.engine_glow_image_id = 1; // GlowImageID present
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_class_id = 0;

  // Full level: level + rand(0..5) - 4 spans 28..33, so the post-clamp level
  // is 28..32 and alpha lies in [28/32, 1.0].
  ship.engine_glow_level = 32;
  for (int i = 0; i < 16; ++i) {
    NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
    CHECK(ship.engine_glow_intensity >= 28.0F / 32.0F);
    CHECK(ship.engine_glow_intensity <= 1.0F);
  }

  // Dark level: level + rand(0..5) - 4 is always < 2 -> hidden (alpha 0).
  ship.engine_glow_level = 0;
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.engine_glow_intensity == 0.0F);

  // Cruise level 24: level + rand(0..5) - 4 spans 20..25, so alpha lies in
  // [20/32, 25/32]. This is the original 0x0042a383 formula, not level/24.
  ship.engine_glow_level = 24;
  for (int i = 0; i < 16; ++i) {
    NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
    CHECK(ship.engine_glow_intensity >= 20.0F / 32.0F);
    CHECK(ship.engine_glow_intensity <= 25.0F / 32.0F);
  }

  // A class with no GlowImageID never lights the layer.
  ShipClass no_glow;
  SetShipClass(state, no_glow);
  ship.engine_glow_level = 32;
  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.engine_glow_intensity == 0.0F);
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
