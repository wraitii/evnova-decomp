#include "ship_visual.hpp"

#include "collision.hpp"
#include "game_state.hpp"
#include "hud_overlay.hpp"
#include "impact_effects.hpp"
#include "mission.hpp"
#include "scenario_data.hpp"
#include "ship_ai.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

namespace game {
namespace {

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) << 8U |
      std::to_integer<std::uint8_t>(bytes[offset + 1]));
}

[[nodiscard]] std::int16_t ReadBeI16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  if (offset + 1 >= bytes.size()) {
    return 0; // descriptor too short for this field; treat as zero
  }
  return static_cast<std::int16_t>(ReadBe16(bytes, offset));
}

constexpr std::size_t kMinDescriptorSize = 0x36;

// Mirrors the original's NovaRandom_Range(n) -> integer in [0, n). The
// destruction visuals roll from the same GameState.rng as every other
// clean-room roll site.
[[nodiscard]] std::int16_t RollRandom(GameState &state, std::int32_t n) {
  if (n <= 0) {
    return 0;
  }
  return static_cast<std::int16_t>(
      std::uniform_int_distribution<std::int32_t>{0, n - 1}(state.rng));
}

} // namespace

std::optional<ShipVisualDescriptor>
DecodeShipVisualDescriptor(std::span<const std::byte> resource_data) {
  if (resource_data.size() < kMinDescriptorSize) {
    return std::nullopt;
  }
  ShipVisualDescriptor d;
  d.base_image_id = ReadBe16(resource_data, 0x00);   // BaseImageID
  d.base_mask_id = ReadBe16(resource_data, 0x02);    // BaseMaskID
  d.base_set_count = ReadBeI16(resource_data, 0x04); // BaseSetCount (>=1)
  if (d.base_set_count < 1) {
    d.base_set_count = 1;
  }
  d.base_x_size = ReadBe16(resource_data, 0x06);        // BaseXSize
  d.base_y_size = ReadBe16(resource_data, 0x08);        // BaseYSize
  d.base_transparency = ReadBeI16(resource_data, 0x0a); // BaseTransp
  // Alternate (bank/unfold) sprite sheet. Ghidra Ship_UpdateVisualState
  // (0x00428340) assigns g_ship_sprite_alt[class] to the ship sprite and
  // composes the displayed frame as row * FramesPer + heading_frame, with the
  // alternate rows living after the base rows in frame order.
  d.alt_image_id = ReadBe16(resource_data, 0x0c);          // AltImageID
  d.alt_mask_id = ReadBe16(resource_data, 0x0e);           // AltMaskID
  d.alt_set_count = ReadBeI16(resource_data, 0x10);        // AltSetCount
  d.sprite_behavior_flags = ReadBe16(resource_data, 0x2e); // Flags
  d.anim_delay = ReadBeI16(resource_data, 0x30);           // AnimDelay
  d.weapon_decay = ReadBeI16(resource_data, 0x32);         // WeapDecay
  // Engine-glow layer (+0x16 image / +0x18 mask / +0x1a x / +0x1c y), read as
  // big-endian shorts like the base fields. Negative/zero id = no glow layer.
  d.engine_glow_image_id = ReadBeI16(resource_data, 0x16); // GlowImageID
  d.engine_glow_mask_id = ReadBeI16(resource_data, 0x18);  // GlowMaskID
  d.engine_glow_x_size = ReadBe16(resource_data, 0x1a);    // GlowXSize
  d.engine_glow_y_size = ReadBe16(resource_data, 0x1c);    // GlowYSize
  // Running-lights layer (+0x1e) and weapon-effects layer (+0x26). The loader
  // reads the same image/mask/x/y quadruple per role and builds a per-class
  // sprite set for each; non-positive ids mean the role is absent.
  d.light_image_id = ReadBeI16(resource_data, 0x1e);  // LightImageID
  d.light_mask_id = ReadBeI16(resource_data, 0x20);   // LightMaskID
  d.light_x_size = ReadBe16(resource_data, 0x22);     // LightXSize
  d.light_y_size = ReadBe16(resource_data, 0x24);     // LightYSize
  d.weapon_image_id = ReadBeI16(resource_data, 0x26); // WeapImageID
  d.weapon_mask_id = ReadBeI16(resource_data, 0x28);  // WeapMaskID
  d.weapon_x_size = ReadBe16(resource_data, 0x2a);    // WeapXSize
  d.weapon_y_size = ReadBe16(resource_data, 0x2c);    // WeapYSize
  // Running-lights blink program (Bible BlinkMode + BlinkValA..D). The Ghidra
  // loader labels these gun/turret/guided exit positions, but the only
  // consumer is the blink state machine below and the loader clamps BlinkMode
  // 2/3 values to the 0x1f intensity ceiling, so this is the Bible layout.
  d.blink_mode = ReadBeI16(resource_data, 0x36);          // BlinkMode
  d.blink_val_a = ReadBeI16(resource_data, 0x38);         // BlinkValA
  d.blink_val_b = ReadBeI16(resource_data, 0x3a);         // BlinkValB
  d.blink_val_c = ReadBeI16(resource_data, 0x3c);         // BlinkValC
  d.blink_val_d = ReadBeI16(resource_data, 0x3e);         // BlinkValD
  d.frames_per_rotation = ReadBeI16(resource_data, 0x34); // FramesPer
  if (d.frames_per_rotation == 0) {
    d.frames_per_rotation = 36;
  }
  return d;
}

// Ghidra 0x00428340 Ship_UpdateVisualState, debris-puff window (the
// Shot_SpawnAreaImpactEffects call at 0x00428d6a). While the
// death timer is above the finale threshold the original rolls 1-in-1/2/4/8
// by the 20/40/60-tick bands and, on a hit, spawns an Explode1 area impact at
// a small random hull offset (randomly silent). The scatter extent is the
// class's decoded sh\x8an BaseYSize (the runtime sprite frame height) times
// g_death_puff_offset_scale_f64 (0.25).
void NovaShip_TickDestroyedDebrisPuffs(GameState &state, Ship &ship) {
  // g_destroyed_finale_threshold (0x0057531c) = 2.0: at or below it the
  // finale owns the frame.
  if (ship.death_timer_active <= 2.0F) {
    return;
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls == nullptr) {
    return;
  }
  // Bands: below 20 always, below 40 1-in-2, below 60 1-in-4, else 1-in-8.
  std::int32_t roll_bound = 8;
  if (ship.death_timer_active < 20.0F) {
    roll_bound = 1;
  } else if (ship.death_timer_active < 40.0F) {
    roll_bound = 2;
  } else if (ship.death_timer_active < 60.0F) {
    roll_bound = 4;
  }
  if (RollRandom(state, roll_bound) != 0) {
    return;
  }
  // Original: round(Sprite_GetFrameFullHeight(ship) *
  // g_death_puff_offset_scale_f64 (0.25, 0x00575330)). Use the decoded sh\x8an
  // BaseYSize for the class; fall back to the collision envelope only for
  // unit/test states whose ship table has no sprite descriptor. 0x00428340
  // truncates both spans toward zero (x87 FIST + residual/sign correction).
  const int sprite_height =
      cls->base_y_size > 0
          ? static_cast<int>(cls->base_y_size)
          : static_cast<int>(std::max(0.0F, ship.collision_radius_px) * 2.0F);
  int extent = static_cast<int>(static_cast<float>(sprite_height) * 0.25F);
  if (extent < 1) {
    extent = 1;
  }
  const float offset_x = static_cast<float>(RollRandom(state, extent * 2)) -
                         static_cast<float>(extent);
  const float offset_y = static_cast<float>(RollRandom(state, extent * 2)) -
                         static_cast<float>(extent);
  // The original discards one draw for long, early-band presentations.
  if (roll_bound < 3 && cls->death_delay_frames > 0x3b) {
    (void)RollRandom(state, 2);
  }
  const bool play_sound = RollRandom(state, 4) == 0;
  NovaEffects_SpawnAreaImpact(state,
                              ship.pos_x + offset_x,
                              ship.pos_y + offset_y,
                              cls->destruction_effect_while_breaking,
                              /*radius=*/0,
                              play_sound);
}

// Ghidra 0x00428340 Ship_UpdateVisualState, destruction slice. See the header
// for scope notes. Constants decoded from data: g_cloak_fade_passive_decay
// (0x00575318) = 1.0 tick, destroyed-finale threshold DAT_0057531c = 2.0,
// player death-timer scale _DAT_00575378 = 3.0, blast radius scale/addend
// g_hull_blast_radius_scale_f64/_addend_f64 (0x00575380/88) = 0.075/50.0,
// blast damage scale/addend g_hull_blast_damage_scale_f64/_addend_f64
// (0x00575390/98) = 0.0375/25.0 (all four are doubles; FMUL/FADD double ptr).
void NovaShip_TickDestroyedShipVisualState(GameState &state,
                                           Ship &ship,
                                           float elapsed_ticks) {
  (void)elapsed_ticks;
  if (!ship.is_active) {
    return;
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  // Ship_HandleShip's validation prologue deactivates hulls outside the class
  // range or with the -9999 nonexistent sentinel before the visual scope.
  if (cls == nullptr || cls->tech_level == kShipClassNonexistentTechLevel) {
    return;
  }
  if (!NovaAiShip_IsDestroyed(ship)) {
    return;
  }
  // The original seeds the presentation the first time it observes the
  // destroyed state and then relies on its constant 1.0/frame countdown always
  // landing inside the 0<timer<=2.0 finale window. The port advances the timer
  // on the normalized 30 Hz basis (elapsed_ticks), where a long frame can step
  // it below zero; the latch keeps that overshoot from re-seeding a fresh
  // presentation and swallowing the Explode2 finale.
  if (!ship.death_timer_seeded) {
    ship.death_timer_seeded = true;
    if (ship.death_timer_active <= 0.0F) {
      float reseed = static_cast<float>(cls->death_delay_frames);
      if (ship.ship_instance_id == 0) {
        reseed *= kPlayerDeathTimerScale;
      }
      ship.death_timer_active = reseed;
      if (reseed <= 0.0F) {
        // Port divergence: a zero DeathDelay hull would linger forever in the
        // original (reseed to 0 keeps the finale from ever firing); the port
        // treats those as immediate destructions.
        NovaShip_RunShipDestructionFinale(state, ship);
      }
      return;
    }
  }
  if (ship.death_timer_active > 2.0F) {
    // Debris-puff window: roll and spawn the Explode1 cadence. The original's
    // repeated explosions are driven from here.
    NovaShip_TickDestroyedDebrisPuffs(state, ship);
    return;
  }
  NovaShip_RunShipDestructionFinale(state, ship);
}

void NovaShip_RunShipDestructionFinale(GameState &state, Ship &ship) {
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const std::int16_t self_slot = ship.ship_instance_id;

  // Hull blast geometry: hull mass drives the splash radius and damage;
  // capability flags 0x400 hulls (mass-less/damped) do not blast. The radius
  // also feeds the finale explosion below.
  std::int16_t blast_radius = 0;
  std::int16_t blast_damage = 0;
  if (cls != nullptr && (cls->capability_flags & 0x0400U) == 0U) {
    const std::int16_t mass = cls->mass_tons;
    if (mass >= 100) {
      // 0x00428340 truncates both hull-blast values toward zero (x87 FIST +
      // residual/sign correction), not round-to-nearest.
      blast_radius = static_cast<std::int16_t>(static_cast<double>(mass) *
                                                   kHullBlastRadiusScale +
                                               kHullBlastRadiusAddend);
      blast_damage = static_cast<std::int16_t>(static_cast<double>(mass) *
                                                   kHullBlastDamageScale +
                                               kHullBlastDamageAddend);
    }
  }

  // Ships within the per-axis radius take the damage as a disable
  // restriction-checking hit (no aggro; the original passes force_armor_only
  // 1, transition check 1). The original has no same-system gate here - quirk
  // preserved, matching the weapon splash path. The 0x3ff personality
  // sentinel is exempt.
  if (cls != nullptr && blast_radius > 0 && ship.pers_def_slot != 0x3ff) {
    for (std::int16_t slot = 0;
         slot < static_cast<std::int16_t>(GameState::kMaxShips);
         ++slot) {
      if (slot == self_slot) {
        continue;
      }
      Ship &victim = state.ShipAt(static_cast<std::size_t>(slot));
      if (!victim.is_active) {
        continue;
      }
      if (std::fabs(victim.pos_x - ship.pos_x) > blast_radius ||
          std::fabs(victim.pos_y - ship.pos_y) > blast_radius) {
        continue;
      }
      ResolveShipHitFromWeapon(state,
                               slot,
                               victim,
                               ship.pos_x,
                               ship.pos_y,
                               /*impact_impulse=*/0,
                               blast_damage,
                               blast_damage,
                               /*attacker_ship_slot=*/self_slot,
                               /*allow_aggro_updates=*/false,
                               /*suppress_retarget_logic=*/false,
                               /*force_armor_only=*/true,
                               /*bypass_shields=*/false,
                               /*player_aggro_delta=*/0,
                               /*check_fire_restriction_transition=*/true);
    }
  }

  // Mission DESTRUCTION bookkeeping (non-player mission ships only).
  const std::int16_t fleet_slot = ship.mission_fleet_slot;
  if (fleet_slot != -1 && self_slot != 0) {
    const auto slot_idx = static_cast<std::size_t>(fleet_slot);
    MissionRuntimeFlags &runtime = state.active_mission_runtime_flags[slot_idx];
    ActiveMission &mission = state.active_missions[slot_idx];
    const std::int16_t spawn_behavior = mission.spawn_behavior;
    const bool quick_fail_shape =
        spawn_behavior == 1 || spawn_behavior == 3 ||
        ((spawn_behavior == 2 || spawn_behavior == 5) &&
         ship.boarded_target_latch == 0);
    if (runtime.is_active && mission.goal_counter_a == 0 && quick_fail_shape &&
        !runtime.is_failed &&
        (runtime.flags_primary_at_accept & 0x0400U) == 0U) {
      state.pending_ui_sounds.push_back(GameState::PendingUiSound{1, 1});
      if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x11c)) {
        NovaHud_ShowOverlayMessage(state,
                                   *text,
                                   /*duration_frames=*/std::uint64_t{0xf0});
      }
      Mission_FailMissionSlotQuick(
          state, fleet_slot, static_cast<std::uint32_t>(SDL_GetTicks()));
    }
    mission.goal_counter_a =
        static_cast<std::int16_t>(mission.goal_counter_a + 1);
    if (mission.target_ship_count > 0) {
      mission.target_ship_count =
          static_cast<std::int16_t>(mission.target_ship_count - 1);
    }
  }

  // TODO(decomp) skipped: the escort cargo-return arm (destroyed escort with
  // squad_leader_ship_slot 0 / behavior 6 / default AI < 3 runs
  // Outfit_TransferCargoAndJunkToEscortByRatio 0x00469810 plus the weapon-bank
  // pool reconciliation), and the finale audio de-registrations.

  // Personality deactivation: the wreck's personality def leaves the ambient
  // spawn pool (1-in-8 chance for the 0x3fe forced sentinel; otherwise unless
  // Flags 0x0002 keeps it available).
  if (ship.pers_def_slot != -1 &&
      ship.pers_def_slot <
          static_cast<std::int16_t>(state.scenario.pers_defs.size())) {
    PersDef &def =
        state.scenario.pers_defs[static_cast<std::size_t>(ship.pers_def_slot)];
    if (ship.pers_def_slot == 0x3fe) {
      std::uniform_int_distribution<std::int32_t> roll{0, 7};
      if (roll(state.rng) == 0) {
        def.present = false;
      }
    } else if ((def.flags_primary & 0x0002U) == 0U) {
      def.present = false;
    }
  }

  // Ghidra tail at 0x0042c041: Shot_SpawnAreaImpactEffects(pos, Explode2
  // (field_0xa1e), blast radius, play_sound=1) then hide + deactivate the
  // hull. This is the "boom" that fires for player and NPC wrecks alike. The
  // SDL view and the zero-DeathDelay hit path no longer spawn duplicates;
  // destruction_finale_triggered keeps the once-only contract.
  if (cls != nullptr && !ship.destruction_finale_triggered) {
    NovaEffects_SpawnAreaImpact(state,
                                ship.pos_x,
                                ship.pos_y,
                                cls->destruction_effect_final,
                                blast_radius,
                                /*play_sound=*/true);
    ship.destruction_finale_triggered = true;
  }

  // Ship_UpdateVisualState tail: the hull deactivates with its target cleared.
  ship.is_active = false;
  ship.squad_leader_ship_slot = -1;
}

// Ghidra 0x00428340 Ship_UpdateVisualState, weapon-effects + running-lights
// slice. See the header for scope notes. Constants decoded from data: the
// weapon flash ceiling is 32.0 (0x42000000), the decay scale is
// g_ship_weapon_glow_decay_scale (0x00575ab8) = 0.003484, and the fully-off
// latch is k_jammed_turn_sign_f32 (-1.0, 0xbf800000). The blink triangle/random
// rates use k_one_percent_f64 = 0.01.
void NovaShip_TickWeaponSpriteAndRunningLights(GameState &state,
                                               Ship &ship,
                                               float elapsed_ticks) {
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls == nullptr) {
    return;
  }
  const float scale = std::max(0.0F, elapsed_ticks);

  // Weapon-effects sprite flash. Weapon_FirePlayerWeaponBank /
  // Weapon_FireShipWeapons raise this to 32 when the fired weapon carries
  // flags_secondary 0x200; here it fades at weapon_glow_decay_rate per tick
  // (after the visible brightness is sampled by the renderer) and latches to
  // -1 once it crosses zero.
  if (ship.weapon_sprite_flash_level > 0.0F) {
    if (ship.weapon_sprite_flash_level > 32.0F) {
      ship.weapon_sprite_flash_level = 32.0F;
    }
    ship.weapon_sprite_flash_level -= cls->weapon_glow_decay_rate * scale;
    // The original clamps an overshoot below -1.0 (k_jammed_turn_sign_f32
    // 0x00575350) back to -1.0; a small negative value simply hides the layer
    // and stops further decay on the next tick (the decay only runs while
    // flash > 0).
    if (ship.weapon_sprite_flash_level < -1.0F) {
      ship.weapon_sprite_flash_level = -1.0F;
    }
  }

  // Engine-glow sprite brightness (Ship_UpdateVisualState 0x00428340). When
  // the class has a GlowImageID layer, the original computes
  //   level = engine_glow_level + NovaRandom_Range(6) - 4
  // and hides the glow when level < 2 (0x0042a391), else clamps level to 32
  // (0x0042a3a4) and writes it into the sprite's RGB tint channels at
  // brightness 32 (0x0042a4ca), i.e. additive contribution src * level/32.
  // The port stores that fraction in engine_glow_intensity for the renderer.
  // The original also folds the per-ship distance-brightness fog into NPC
  // glow (level = min(level, 32 - distance_brightness*1.5), 0x00575340/
  // 0x00575348); the port does not track the murk fog yet, so that reduction
  // is not applied (TODO(decomp)).
  if (cls->engine_glow_image_id > 0) {
    const std::int16_t flicker =
        static_cast<std::int16_t>(RollRandom(state, 6));
    std::int16_t level =
        static_cast<std::int16_t>(ship.engine_glow_level + flicker - 4);
    if (level < 2) {
      ship.engine_glow_intensity = 0.0F;
    } else {
      if (level > 0x20) {
        level = 0x20;
      }
      ship.engine_glow_intensity = static_cast<float>(level) / 32.0F;
    }
  } else {
    ship.engine_glow_intensity = 0.0F;
  }

  // Running lights: only classes with a light layer run the blink state
  // machine (the original gates the whole block on g_ship_sprite_light[class]
  // being bound). BlinkMode 0/-1 leaves the lights at full brightness; 1 is the
  // Bible square wave, 2 the triangle pulse and 3 the random pulse.
  if (cls->light_image_id <= 0) {
    ship.light_intensity = 0.0F;
    return;
  }
  const std::int16_t mode = cls->blink_mode;
  if (mode == 1) {
    // Square wave. BlinkValA = off-time between blinks, BlinkValB = on-time,
    // BlinkValC = blinks per group, BlinkValD = delay between groups. The
    // timer is clamped to max(on-time, group-delay) before use.
    const std::int16_t max_timer = std::max(cls->blink_val_b, cls->blink_val_d);
    if (static_cast<float>(max_timer) < ship.light_blink_timer) {
      ship.light_blink_timer = static_cast<float>(max_timer);
    }
    if (ship.light_blink_phase < cls->blink_val_c) {
      if (ship.light_blink_timer <= 0.0F) {
        if (ship.light_intensity <= 0.0F) {
          ship.light_intensity = 32.0F;
          ship.light_blink_timer = static_cast<float>(cls->blink_val_b);
        } else {
          ship.light_intensity = 0.0F;
          ship.light_blink_phase =
              static_cast<std::int16_t>(ship.light_blink_phase + 1);
          ship.light_blink_timer = static_cast<float>(cls->blink_val_a);
        }
      } else {
        ship.light_blink_timer -= scale;
      }
    } else if (ship.light_blink_timer <= 0.0F) {
      ship.light_blink_phase = 0;
      ship.light_intensity = 0.0F;
      ship.light_blink_timer = static_cast<float>(cls->blink_val_d);
    } else {
      ship.light_blink_timer -= scale;
    }
  } else if (mode == 2) {
    // Triangle pulse. BlinkValA = minimum intensity, BlinkValB = rise per
    // frame x100, BlinkValC = maximum intensity, BlinkValD = fall per frame
    // x100. Animation phase 0 ramps up, 1 ramps down.
    if (ship.light_blink_phase == 0) {
      if (ship.light_intensity < static_cast<float>(cls->blink_val_c)) {
        ship.light_intensity +=
            static_cast<float>(cls->blink_val_b) * 0.01F * scale;
      } else {
        ship.light_intensity = static_cast<float>(cls->blink_val_c);
        ship.light_blink_phase = 1;
      }
    } else if (static_cast<float>(cls->blink_val_a) < ship.light_intensity) {
      ship.light_intensity -=
          static_cast<float>(cls->blink_val_d) * 0.01F * scale;
    } else {
      ship.light_intensity = static_cast<float>(cls->blink_val_a);
      ship.light_blink_phase = 0;
    }
  } else if (mode == 3) {
    // Random pulse. BlinkValA/B = min/max intensity, BlinkValC = delay between
    // changes, BlinkValD ignored.
    if (ship.light_blink_timer > 0.0F) {
      ship.light_blink_timer -= scale;
      if (static_cast<float>(cls->blink_val_c) < ship.light_blink_timer) {
        ship.light_blink_timer = static_cast<float>(cls->blink_val_c);
      }
    } else {
      const std::int32_t span =
          static_cast<std::int32_t>(cls->blink_val_b) + 1 - cls->blink_val_a;
      ship.light_intensity =
          static_cast<float>(RollRandom(state, span) + cls->blink_val_a);
      ship.light_blink_timer = static_cast<float>(cls->blink_val_c);
    }
  } else {
    // 0 / -1: always on at full brightness.
    ship.light_intensity = 32.0F;
  }
}

// Ghidra 0x00428340 Ship_UpdateVisualState, cloak-fade slice. See the header
// for scope notes. Constants decoded from data: g_cloak_fade_rate_default
// (0x00575308) = 1.5, g_cloak_fade_rate_flags2_swarming (0x0057530c) = 0.75,
// g_cloak_fade_progress_max (0x00575314) = 32.0, g_cloak_fade_passive_decay
// (0x00575318) = 1.0. The original indexes g_ship_class_defs directly; the
// port's defensive class lookup only changes behavior for hulls the
// Ship_HandleShip prologue would already have deactivated.
void NovaShip_TickCloakFadeState(GameState &state,
                                 Ship &ship,
                                 float elapsed_ticks) {
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (ship.cloak_transition_latch == 0) {
    // Passive decay: an interrupted fade bleeds back toward fully visible at
    // one tick per frame (no frame-scale on this arm in the original).
    if (ship.cloak_fade_progress > 0.0F && ship.cloak_fade_progress < 32.0F) {
      ship.cloak_fade_progress -= 1.0F;
    }
  } else {
    const float fade_rate =
        (cls != nullptr && (cls->flags_secondary & 0x1U) != 0U) ? 1.5F : 0.75F;
    ship.cloak_fade_progress +=
        static_cast<float>(ship.cloak_transition_latch) * fade_rate *
        elapsed_ticks;
    if (ship.cloak_fade_progress <= 0.0F && ship.cloak_transition_latch < 0) {
      ship.cloak_fade_progress = 0.0F;
      ship.cloak_transition_latch = 0;
    }
    if (ship.cloak_fade_progress >= 32.0F && ship.cloak_transition_latch > 0) {
      ship.cloak_fade_progress = 32.0F;
      ship.cloak_transition_latch = 0;
    }
  }
  // A wreck whose fade is still running clears through the lower threshold.
  if (ship.cloak_fade_progress > 0.0F && NovaAiShip_IsDestroyed(ship)) {
    ship.cloak_transition_latch = -2;
  }
}

} // namespace game
