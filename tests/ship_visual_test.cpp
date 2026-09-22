#include "game/game_state.hpp"
#include "game/outfit.hpp"
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

TEST_CASE("flags 0x40 running lights go dark when disabled", "[ship][visual]") {
  GameState state;
  ShipClass cls;
  cls.light_image_id = 1;
  cls.blink_mode = -1; // steady full brightness
  cls.sprite_behavior_flags = 0x0040;
  SetShipClass(state, cls);
  // A derelict government is enough to make the hull read as disabled.
  state.scenario.governments.assign(1, Government{});
  state.scenario.governments[0].flags_primary = 0x800;
  Ship ship;
  ship.ship_class_id = 0;
  ship.faction_or_government_id = 0;

  NovaShip_TickWeaponSpriteAndRunningLights(state, ship, 1.0F);
  CHECK(ship.light_intensity == 0.0F);

  // Without the flag the same disabled hull keeps its steady lights.
  state.scenario.ships[0].sprite_behavior_flags = 0;
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

TEST_CASE("banking flags pick the base row from the turn bias",
          "[ship][visual][sprite]") {
  GameState state;
  ShipClass cls;
  cls.base_set_count = 3;
  cls.sprite_behavior_flags = 0x0001; // banking
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_class_id = 0;

  CHECK(ComposeShipBaseRow(state, ship, cls) == 0);
  ship.ai_turn_bias_dir = -1;
  CHECK(ComposeShipBaseRow(state, ship, cls) == 1);
  ship.ai_turn_bias_dir = 1;
  CHECK(ComposeShipBaseRow(state, ship, cls) == 2);

  // A single-set base sheet always uses row 0 regardless of the flags.
  ShipClass single = cls;
  single.base_set_count = 1;
  CHECK(ComposeShipBaseRow(state, ship, single) == 0);
}

TEST_CASE("sequence flags advance the combat animation at AnimDelay",
          "[ship][visual][sprite]") {
  GameState state;
  ShipClass cls;
  cls.base_set_count = 6;
  cls.animation_cycle_count = 6;
  // Cargo Drone flags: hide lights when disabled | stop when disabled |
  // sequence.
  cls.sprite_behavior_flags = 0x0040U | 0x0010U | 0x0008U;
  cls.combat_state_init_range = 3; // AnimDelay
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_class_id = 0;
  ship.ship_instance_id = 1;
  ship.defense_fleet_home_stellar_id = -1;
  ship.armor_points = 100.0F;

  // Strict `AnimDelay < timer` means the first advance lands on the 4th tick.
  for (int i = 0; i < 3; ++i) {
    NovaShip_TickSpriteAnimation(state, ship, 1.0F);
  }
  CHECK(ship.sprite_animation_cycle_index == 0);
  NovaShip_TickSpriteAnimation(state, ship, 1.0F);
  CHECK(ship.sprite_animation_cycle_index == 1);
  CHECK(ComposeShipBaseRow(state, ship, cls) == 1);

  // The subtract-dwell loop leaves the remainder (1 here), so the next step
  // lands 3 ticks later.
  CHECK(ship.sprite_animation_timer == 1.0F);
  for (int i = 0; i < 3; ++i) {
    NovaShip_TickSpriteAnimation(state, ship, 1.0F);
  }
  CHECK(ship.sprite_animation_cycle_index == 2);

  // Wraps at animation_cycle_count.
  ship.sprite_animation_cycle_index =
      static_cast<std::int16_t>(cls.animation_cycle_count - 1);
  ship.sprite_animation_timer = static_cast<float>(cls.combat_state_init_range);
  NovaShip_TickSpriteAnimation(state, ship, 1.0F);
  CHECK(ship.sprite_animation_cycle_index == 0);
}

TEST_CASE("a disabled Flags-0x0010 class freezes its sequence animation",
          "[ship][visual][sprite]") {
  GameState state;
  ShipClass cls;
  cls.base_set_count = 4;
  cls.animation_cycle_count = 4;
  cls.sprite_behavior_flags = 0x0010U | 0x0008U;
  cls.combat_state_init_range = 1;
  cls.base_armor = 100; // defeat the critically-damaged disabled gate
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_class_id = 0;
  ship.ship_instance_id = 1;
  ship.defense_fleet_home_stellar_id = -1;
  ship.armor_points = 1.0F; // below base_armor/3 -> disabled

  for (int i = 0; i < 8; ++i) {
    NovaShip_TickSpriteAnimation(state, ship, 1.0F);
  }
  CHECK(ship.sprite_animation_cycle_index == 0);
  CHECK(ship.sprite_animation_timer == 0.0F);
}

TEST_CASE("the alt overlay cycles at AnimDelay", "[ship][visual][sprite]") {
  GameState state;
  ShipClass cls;
  cls.base_set_count = 1;
  cls.alt_image_id = 1330; // Auroran Thunderforge
  cls.alt_sprite_cycle_count = 6;
  cls.sprite_behavior_flags = 0x0040U | 0x0010U | 0x0008U;
  cls.combat_state_init_range = 3;
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_class_id = 0;
  ship.ship_instance_id = 1;
  ship.defense_fleet_home_stellar_id = -1;
  ship.armor_points = 100.0F;

  // The base sheet has a single set, so the Flag-0x0008 arm is skipped and the
  // alt advances on its own timer (strict `AnimDelay < timer`).
  for (int i = 0; i < 3; ++i) {
    NovaShip_TickSpriteAnimation(state, ship, 1.0F);
  }
  CHECK(ship.alternate_sprite_cycle_index == 0);
  NovaShip_TickSpriteAnimation(state, ship, 1.0F);
  CHECK(ship.alternate_sprite_cycle_index == 1);

  // Wraps at alt_sprite_cycle_count.
  for (int i = 0; i < 5 * 4; ++i) {
    NovaShip_TickSpriteAnimation(state, ship, 1.0F);
  }
  CHECK(ship.alternate_sprite_cycle_index == 0);
}

TEST_CASE("destroyed escape pod skips the destruction presentation",
          "[ship][visual][destruction]") {
  GameState state;
  // The escape pod is class index 0x2ff (resource ship id 0x37f); the raw call
  // resolves ship_class_id + 0x80, so size the table past that index.
  state.scenario.ships.assign(0x300, ShipClass{});
  ShipClass pod;
  pod.base_shield = 0;
  pod.base_armor = 0;
  pod.death_delay_frames = 20;
  state.scenario.ships[kEscapePodShipClassIndex] = pod;

  Ship ship;
  ship.is_active = true;
  ship.ship_class_id = kEscapePodShipClassIndex;
  ship.shield_points = 0.0F;
  ship.armor_points = 0.0F; // Ship_IsShipDestroyed is true from birth.
  ship.death_timer_active = -1.0F;
  ship.death_timer_seeded = false;

  // Ghidra 0x00428b5c: destroyed class-0x2ff hulls fall through to normal
  // sprite presentation instead of seeding the death timer and exploding.
  NovaShip_TickDestroyedShipVisualStateRawCall(state, ship);
  CHECK(ship.death_timer_seeded == false);
  CHECK(ship.death_timer_active == Catch::Approx(-1.0F));
  CHECK(ship.is_active);
}

TEST_CASE("destroyed non-pod hull seeds the destruction timer",
          "[ship][visual][destruction]") {
  GameState state;
  ShipClass cls;
  cls.death_delay_frames = 20;
  SetShipClass(state, cls);

  Ship ship;
  ship.is_active = true;
  ship.ship_instance_id = 1; // NPC: no player 3x death-timer scale.
  ship.ship_class_id = 0;
  ship.armor_points = 0.0F;
  ship.death_timer_active = -1.0F;
  ship.death_timer_seeded = false;

  NovaShip_TickDestroyedShipVisualStateRawCall(state, ship);
  CHECK(ship.death_timer_seeded);
  CHECK(ship.death_timer_active == Catch::Approx(20.0F));
  CHECK(ship.is_active);
}

TEST_CASE("cloak fade rate reads the device fast-fade bit under the bug fix",
          "[ship][visual][cloak]") {
  // BUGFIX(original): the original gates 1.5/0.75 on ShipClass.flags_secondary
  // bit 0x0001 (swarming), not the ModType-17 device's ModVal 0x0001 ("Faster
  // fading"); the shipped data inverts the intent. Under kApplyOriginalBugFixes
  // the rate follows the device bit. See docs/known_original_bugs.md.
  GameState state;
  ShipClass cls;
  cls.default_outfit_ids[0] = 0x80;
  cls.default_outfit_counts[0] = 1;
  state.scenario.ships.assign(1, cls);
  state.scenario.outfits.assign(1, Outfit{});
  state.scenario.outfits[0].mod_type = 0x11; // cloaking device

  Ship ship;
  ship.ship_class_id = 0;
  ship.ship_instance_id = 1; // NPC: class default loadout
  ship.armor_points = 1.0F;  // alive
  ship.cloak_transition_latch = 1;

  // Swarming class bit set, but the device lacks ModVal 0x0001 -> slow.
  cls.flags_secondary = 0x0001;
  state.scenario.ships[0] = cls;
  state.scenario.outfits[0].mod_val = 0x0002;
  ship.cloak_fade_progress = 0.0F;
  NovaShip_TickCloakFadeState(state, ship, 1.0F);
  CHECK(ship.cloak_fade_progress == Catch::Approx(0.75F));

  // Non-swarming class, device carries ModVal 0x0001 -> fast.
  cls.flags_secondary = 0x0000;
  state.scenario.ships[0] = cls;
  state.scenario.outfits[0].mod_val = 0x0001;
  ship.cloak_fade_progress = 0.0F;
  ship.cloak_transition_latch = 1;
  NovaShip_TickCloakFadeState(state, ship, 1.0F);
  CHECK(ship.cloak_fade_progress == Catch::Approx(1.5F));
}

TEST_CASE("cloak ability caches populate lazily and reset when out of system",
          "[ship][visual][cloak]") {
  // Ghidra 0x00428340 tail. One outfit: primary ModType 30 radar reveal plus an
  // alternate ModType 17 slot carrying the damage-deactivate bit.
  GameState state;
  ShipClass cls;
  SetShipClass(state, cls);
  state.scenario.outfits.assign(1, Outfit{});
  state.scenario.outfits[0].mod_type = 0x1e;  // cloak scanner
  state.scenario.outfits[0].mod_val = 0x0001; // radar reveal
  state.scenario.outfits[0].alt_mod_types[0] = 0x11;
  state.scenario.outfits[0].alt_mod_vals[0] = 0x0008; // damage-deactivate
  state.inventory.outfit_owned_count[0] = 1;

  Ship &p = state.player;
  p.ship_instance_id = 0;
  p.pers_def_slot = -1;
  p.is_active = true;
  p.current_system_id = 0;
  p.cloak_scanner_reveal_screen = -1;
  p.cloak_scanner_reveal_radar = -1;
  p.cloak_damage_deactivate_latch = -1;

  NovaShip_RefreshCloakAbilityCaches(state, p);
  CHECK(p.cloak_scanner_reveal_screen == 0);
  CHECK(p.cloak_scanner_reveal_radar == 1);
  CHECK(p.cloak_damage_deactivate_latch == 1);

  // An inactive hull is reset to the not-yet-computed sentinel.
  p.is_active = false;
  NovaShip_RefreshCloakAbilityCaches(state, p);
  CHECK(p.cloak_scanner_reveal_screen == -1);
  CHECK(p.cloak_scanner_reveal_radar == -1);
  CHECK(p.cloak_damage_deactivate_latch == -1);
}

TEST_CASE("cloak render presentation follows the fade slice",
          "[ship][visual][cloak][render]") {
  // Ghidra 0x00428340 0x0042b0b2..: partial fade is an additive ghost whose
  // per-channel hull intensity falls with progress; full fade hides the hull
  // unless the player/reveal rules apply.
  GameState state;
  Ship ship;
  ship.ship_instance_id = 1; // NPC
  ship.squad_leader_ship_slot = 5;
  ship.pers_def_slot = -1;
  const NovaShipTintColor tint{0x20, 0x20, 0x20};

  // No fade: an ordinary draw.
  ship.cloak_fade_progress = 0.0F;
  ShipCloakPresentation p = NovaShip_CloakPresentation(state, ship, tint);
  CHECK_FALSE(p.hidden);
  CHECK_FALSE(p.additive);
  CHECK(p.hull_alpha == Catch::Approx(1.0F));

  // Partial fade: additive, per-channel tint = resolved - progress (0x20 - 16).
  ship.cloak_fade_progress = 16.0F;
  ship.cloak_jitter_x = -2;
  ship.cloak_jitter_y = 1;
  p = NovaShip_CloakPresentation(state, ship, tint);
  CHECK_FALSE(p.hidden);
  CHECK(p.additive);
  CHECK(p.hull_tint[0] == 0x10);
  CHECK(p.hull_tint[1] == 0x10);
  CHECK(p.hull_tint[2] == 0x10);
  CHECK(p.effect_cap == Catch::Approx(16.0F));
  CHECK(p.weapon_cap == Catch::Approx(24.0F));
  CHECK(p.jitter_x == Catch::Approx(-2.0F));
  CHECK(p.jitter_y == Catch::Approx(1.0F));

  // Near the top of the fade a sub-2 channel collapses to the 2 floor.
  ship.cloak_fade_progress = 31.0F;
  p = NovaShip_CloakPresentation(state, ship, tint);
  CHECK(p.hull_tint[0] == 2);
  CHECK(p.effect_cap == Catch::Approx(1.0F));

  // The dim is a truncation, not a round: (0x20 - 16.5) = 15.5 -> 15.
  ship.cloak_fade_progress = 16.5F;
  p = NovaShip_CloakPresentation(state, ship, tint);
  CHECK(p.hull_tint[0] == 15);

  // A resolved channel below 0x10 collapses to 0 instead of 2.
  ship.cloak_fade_progress = 16.0F;
  p = NovaShip_CloakPresentation(
      state, ship, NovaShipTintColor{0x08, 0x08, 0x08});
  CHECK(p.hull_tint[0] == 0);

  // Full fade with no reveal: hidden (brightness 0x40, hull/alt skipped), but
  // the shared weapon cap still applies (0x0042b7f2).
  ship.cloak_fade_progress = 32.0F;
  p = NovaShip_CloakPresentation(state, ship, tint);
  CHECK(p.hidden);
  CHECK(p.weapon_cap == Catch::Approx(8.0F));
}

TEST_CASE("full cloak ghosts the player, escorts and screen-scanner reveals",
          "[ship][visual][cloak][render]") {
  // Ghidra 0x00428340 0x0042b714: brightness 0x1e (faint ghost) for the
  // player, a player escort, or a hull the player's screen scanner reveals.
  GameState state;
  Ship ship;
  ship.pers_def_slot = -1;
  ship.cloak_fade_progress = 32.0F;
  const NovaShipTintColor tint{0x20, 0x20, 0x20};

  // Generic un-revealed NPC: hull/alt skipped, weapon cap still 40 - p.
  ship.ship_instance_id = 1;
  ship.squad_leader_ship_slot = 7;
  state.player.cloak_scanner_reveal_screen = 0;
  ShipCloakPresentation hidden = NovaShip_CloakPresentation(state, ship, tint);
  CHECK(hidden.hidden);
  CHECK(hidden.weapon_cap == Catch::Approx(8.0F));

  // The player's own hull ghosts (src weight = (2 + 2)/32).
  ship.ship_instance_id = 0;
  ShipCloakPresentation p = NovaShip_CloakPresentation(state, ship, tint);
  CHECK_FALSE(p.hidden);
  CHECK_FALSE(p.additive);
  CHECK(p.hull_alpha == Catch::Approx(0.125F));
  CHECK(p.weapon_cap == Catch::Approx(8.0F));

  // A player escort (squad_leader_ship_slot == 0) ghosts.
  ship.ship_instance_id = 1;
  ship.squad_leader_ship_slot = 0;
  CHECK_FALSE(NovaShip_CloakPresentation(state, ship, tint).hidden);

  // The player's screen-reveal scanner reveals any hull.
  ship.squad_leader_ship_slot = 7;
  state.player.cloak_scanner_reveal_screen = 1;
  CHECK_FALSE(NovaShip_CloakPresentation(state, ship, tint).hidden);
}

TEST_CASE("partial cloak draws the hull jitter from the session RNG",
          "[ship][visual][cloak][render]") {
  // Ghidra 0x00428340 0x0042b0b2: m = trunc(progress/10), each axis offset in
  // [-m, m] from two NovaRandom_Range(2m+1) rolls.
  GameState state;
  ShipClass cls{};
  cls.base_image_id = 1; // the jitter needs a hull sprite (0x0042b0a7)
  SetShipClass(state, cls);
  Ship ship;
  ship.ship_instance_id = 1;
  ship.squad_leader_ship_slot = 7;
  ship.armor_points = 1.0F;

  // progress < 10 -> magnitude 0, so both offsets clamp to 0.
  ship.cloak_fade_progress = 5.0F;
  ship.cloak_transition_latch = 1;
  NovaShip_TickCloakFadeState(state, ship, 1.0F);
  CHECK(ship.cloak_jitter_x == 0);
  CHECK(ship.cloak_jitter_y == 0);

  // progress ~25 -> magnitude 2, each offset within [-2, 2].
  ship.cloak_fade_progress = 25.0F;
  ship.cloak_transition_latch = 1;
  NovaShip_TickCloakFadeState(state, ship, 1.0F);
  CHECK(ship.cloak_jitter_x >= -2);
  CHECK(ship.cloak_jitter_x <= 2);
  CHECK(ship.cloak_jitter_y >= -2);
  CHECK(ship.cloak_jitter_y <= 2);

  // A stable/clear fade resets the offsets.
  ship.cloak_fade_progress = 0.0F;
  ship.cloak_transition_latch = 0;
  NovaShip_TickCloakFadeState(state, ship, 1.0F);
  CHECK(ship.cloak_jitter_x == 0);
  CHECK(ship.cloak_jitter_y == 0);
}

} // namespace game
