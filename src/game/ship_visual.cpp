#include "ship_visual.hpp"

#include "../util/byte_reader.hpp"
#include "collision.hpp"
#include "compatibility.hpp"
#include "frame_timing.hpp"
#include "game_state.hpp"
#include "hud_overlay.hpp"
#include "impact_effects.hpp"
#include "landed_store.hpp"
#include "mission.hpp"
#include "nova_random.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "ship_ai.hpp"
#include "sprite_world.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

namespace game {
namespace {

using evnova::util::ReadBe16;
using evnova::util::ReadBeI16;

constexpr std::size_t kMinDescriptorSize = 0x36;

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
  // Alternate overlay sprite sheet. Ship_UpdateVisualState (0x00428340)
  // assigns g_ship_sprite_alt[class] to a separate per-ship sprite drawn over
  // the hull, composing its frame as
  // AltSetCount-cycled-index * FramesPer + heading_frame. It is NOT appended to
  // the base sheet's rows (Ghidra ShipClass_LoadShipClassVisualAndLaunchData
  // 0x004b4ee0 builds it only when AltImageID > 0 && AltSetCount > 0).
  d.alt_image_id = ReadBeI16(resource_data, 0x0c);         // AltImageID
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
  // Shield-bubble layer (+0x40 image / +0x42 mask / +0x44 x / +0x46 y).
  d.shield_image_id = ReadBeI16(resource_data, 0x40); // ShieldImageID
  d.shield_mask_id = ReadBeI16(resource_data, 0x42);  // ShieldMaskID
  d.shield_x_size = ReadBe16(resource_data, 0x44);    // ShieldXSize
  d.shield_y_size = ReadBe16(resource_data, 0x46);    // ShieldYSize
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
  if (RandomBelow(state, roll_bound) != 0) {
    return;
  }
  // Original: round(Sprite_GetFrameFullWidth(ship) *
  // g_death_puff_offset_scale_f64 (0.25, 0x00575330)). Use the decoded sh\x8an
  // BaseXSize for the class; fall back to the collision envelope only for
  // unit/test states whose ship table has no sprite descriptor. 0x00428340
  // truncates both spans toward zero (x87 FIST + residual/sign correction).
  const int sprite_width =
      cls->base_x_size > 0
          ? static_cast<int>(cls->base_x_size)
          : static_cast<int>(std::max(0.0F, ship.collision_radius_px) * 2.0F);
  int extent = static_cast<int>(static_cast<float>(sprite_width) * 0.25F);
  if (extent < 1) {
    extent = 1;
  }
  const float offset_x = static_cast<float>(RandomBelow(state, extent * 2)) -
                         static_cast<float>(extent);
  const float offset_y = static_cast<float>(RandomBelow(state, extent * 2)) -
                         static_cast<float>(extent);
  // Preserve the original RNG sequence: long destruction animations consume
  // one otherwise-unused draw when roll_bound is in its first three bands.
  if (roll_bound < 3 && cls->death_delay_frames > 0x3b) {
    (void)RandomBelow(state, 2);
  }
  const bool play_sound = RandomBelow(state, 4) == 0;
  NovaEffects_SpawnAreaImpact(state,
                              ship.pos_x + offset_x,
                              ship.pos_y + offset_y,
                              cls->destruction_effect_while_breaking,
                              /*radius=*/0,
                              play_sound);
}

// Ghidra 0x00428340 Ship_UpdateVisualState, destruction slice. See the header
// for scope notes. Constants decoded from data: k_unit_f32
// (0x00575318) = 1.0 tick, destroyed-finale threshold DAT_0057531c = 2.0,
// player death-timer scale _DAT_00575378 = 3.0, blast radius scale/addend
// g_hull_blast_radius_scale_f64/_addend_f64 (0x00575380/88) = 0.075/50.0,
// blast damage scale/addend g_hull_blast_damage_scale_f64/_addend_f64
// (0x00575390/98) = 0.0375/25.0 (all four are doubles; FMUL/FADD double ptr).
void NovaShip_TickDestroyedShipVisualState(GameState &state,
                                           Ship &ship,
                                           float elapsed_ticks) {
  // Clean-room scheduler only. The original receives one invocation from each
  // outer loop; SDL can present faster, so retain fractional time and invoke
  // the recovered discrete body only for whole 21 ms logical calls.
  ship.destruction_raw_tick_accumulator +=
      std::max(0.0F, RawSpaceflightCallTicks(elapsed_ticks));
  while (ship.is_active &&
         ship.destruction_raw_tick_accumulator + 1.0e-6F >= 1.0F) {
    ship.destruction_raw_tick_accumulator -= 1.0F;
    ship.destruction_raw_tick_accumulator =
        std::max(0.0F, ship.destruction_raw_tick_accumulator);
    if (ship.death_timer_active > 0.0F) {
      ship.death_timer_active -= 1.0F;
    }
    NovaShip_TickDestroyedShipVisualStateRawCall(state, ship);
  }
}

void NovaShip_TickDestroyedShipVisualStateRawCall(GameState &state,
                                                  Ship &ship) {
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
  // Ghidra 0x00428b5c: a destroyed hull whose class is the 0x2ff escape pod
  // skips the seed/debris/finale and falls through to the normal sprite
  // presentation. The pod's class is 0-shield/0-armor, so Ship_IsShipDestroyed
  // is true from birth; without this exemption the pod seeds the death timer
  // and explodes immediately. Faithful port, not a BUGFIX(original).
  if (ship.ship_class_id == kEscapePodShipClassIndex) {
    return;
  }
  // The original reseeds the presentation whenever death_timer_active <= 0.
  // The port-side latch makes that ownership explicit across relaunches: the
  // time-adjusted scheduler can drive an already-owned timer below zero, which
  // the original's constant 1.0 step cannot, and a reseed there would skip the
  // 0 < timer <= 2 finale window.
  const bool was_seeded = ship.death_timer_seeded;
  ship.death_timer_seeded = true;
  float reseed = static_cast<float>(cls->death_delay_frames);
  if (ship.ship_instance_id == 0) {
    reseed *= kPlayerDeathTimerScale;
  }
  // DeathDelay 0/1 (resolved reseed <= 1) is the original's immortal-ghost
  // case: the per-call decrement lands the timer on zero before the check, so
  // the original's `fVar1 <= 0` branch retakes every call and neither the
  // > 2.0 debris window nor the finale ever runs. Keep reseeding -- and stay
  // alive -- when kApplyOriginalBugFixes is off; run the finale immediately
  // when it is on. A larger delay resolves (next check sees 1..2) so the latch
  // owns it after the first seed.
  const bool ghost_reseed = reseed <= 1.0F;
  if (ship.death_timer_active <= 0.0F && (!was_seeded || ghost_reseed)) {
    ship.death_timer_active = reseed;
    if (ghost_reseed) {
      if (!kApplyOriginalBugFixes) {
        ship.death_timer_seeded = false;
        return;
      }
      // BUGFIX(original): the zero/low reseed leaves the wreck lingering as
      // an immortal ghost sprite. Treat it as an immediate destruction.
      NovaShip_RunShipDestructionFinale(state, ship);
    }
    return;
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
      Ship_ApplyDamageToShip(state,
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
    const std::int16_t ship_goal = mission.ship_goal;
    const bool quick_fail_shape =
        ship_goal == 1 || ship_goal == 3 ||
        ((ship_goal == 2 || ship_goal == 5) && ship.boarded_target_latch == 0);
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
          state, fleet_slot, static_cast<std::uint32_t>(state.gameplay_now_ms));
    }
    mission.goal_counter_a =
        static_cast<std::int16_t>(mission.goal_counter_a + 1);
    if (mission.target_ship_count > 0) {
      mission.target_ship_count =
          static_cast<std::int16_t>(mission.target_ship_count - 1);
    }
  }

  if (ship.squad_leader_ship_slot == 0 && ship.ai_behavior_code == 6 &&
      ship.mission_fleet_slot == -1 && cls != nullptr &&
      cls->default_ai_behavior < 3) {
    // The active wreck remains in the capacity denominator as the transfer
    // recipient, even though other destroyed escorts are excluded.
    Player_TransferCargoAndJunkToEscortByRatio(state, self_slot);
    NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
    state.InvalidateDerivedStatCaches();
  }
  // TODO(decomp): finale audio de-registrations.

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
        def.alive = false;
      }
    } else if ((def.flags_primary & 0x0002U) == 0U) {
      def.alive = false;
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

int ComposeShipBaseRow(const GameState &state,
                       const Ship &ship,
                       const ShipClass &cls) {
  // The original guards the whole row-selection block on the base sprite
  // having more than one rotation set (fpr < sprite.num_frames). With the
  // ship-animations preference forced ON (TODO(decomp): the pref is not
  // threaded into the renderer), the loader sizes the base sprite to
  // base_set_count * fpr, so base_set_count >= 2 is the equivalent guard. A
  // pref-off run would size the base sheet to one set and skip this block.
  if (cls.base_set_count < 2) {
    return 0;
  }
  const std::uint16_t flags = cls.sprite_behavior_flags;
  if ((flags & 0x0001U) != 0U) {
    if (ship.ai_turn_bias_dir < 0) {
      return 1;
    }
    if (ship.ai_turn_bias_dir > 0) {
      return 2;
    }
    return 0;
  }
  if ((flags & 0x0002U) != 0U) {
    return std::max<std::int16_t>(0, ship.waypoint_arrival_marker_b);
  }
  if ((flags & 0x0004U) != 0U) {
    return NovaWeapon_HasLoadedLaunchBayAmmo(state, ship) ? 1 : 0;
  }
  if ((flags & 0x0008U) != 0U) {
    return std::max<std::int16_t>(0, ship.sprite_animation_cycle_index);
  }
  return 0;
}

// Ghidra 0x00428340 Ship_UpdateVisualState, sprite-frame composition slice.
// See the header for the flags mapping. The original holds two transient
// latches across the block: local_15 = the Flags-0x0008 arm owns the shared
// sprite_animation_timer, and local_42 = an animation stepped this frame
// (which drives the alt cycle in lockstep).
void NovaShip_TickSpriteAnimation(GameState &state,
                                  Ship &ship,
                                  float elapsed_ticks) {
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls == nullptr) {
    return;
  }
  const float scale = std::max(0.0F, elapsed_ticks);
  const std::uint16_t flags = cls->sprite_behavior_flags;
  const bool disabled = NovaAiShip_IsDisabled(state, ship);
  const std::int16_t dwell = cls->combat_state_init_range;
  const auto dwell_f = static_cast<float>(dwell);

  bool base8_active = false; // local_15
  bool stepped = false;      // local_42 high byte

  // Same pref-forced-on guard as ComposeShipBaseRow (see that comment).
  if (cls->base_set_count >= 2) {
    if ((flags & 0x0001U) == 0U) {
      if ((flags & 0x0002U) == 0U) {
        if ((flags & 0x0004U) == 0U) {
          if ((flags & 0x0008U) != 0U) {
            base8_active = true;
            if ((flags & 0x0010U) == 0U || !disabled) {
              ship.sprite_animation_timer += scale;
            }
            if (dwell_f < ship.sprite_animation_timer) {
              stepped = true;
              if (dwell < 1) {
                ship.sprite_animation_timer = 0.0F;
              } else {
                while (dwell_f < ship.sprite_animation_timer) {
                  ship.sprite_animation_timer -= dwell_f;
                }
              }
              if (ship.sprite_animation_cycle_index < 0) {
                ship.sprite_animation_cycle_index = 0;
              }
              ship.sprite_animation_cycle_index = static_cast<std::int16_t>(
                  ship.sprite_animation_cycle_index + 1);
              if (cls->animation_cycle_count <=
                  ship.sprite_animation_cycle_index) {
                ship.sprite_animation_cycle_index = 0;
              }
            }
          }
          // Flags 0x0004 (carry) needs no per-frame state: the row follows the
          // launch-bay ammo at draw time (ComposeShipBaseRow).
        } else {
          // Flags 0x0002 fold/unfold. waypoint_arrival_marker_a < 0 folds
          // (marker_b counts down), marker_a >= 1 unfolds (marker_b counts up),
          // and marker_a == 0 parks at the current row.
          if (ship.waypoint_arrival_marker_a < 1) {
            if (ship.waypoint_arrival_marker_a < 0) {
              ship.turn_bank_animation_phase += scale;
              if (dwell_f < ship.turn_bank_animation_phase) {
                ship.turn_bank_animation_phase = 0.0F;
                ship.waypoint_arrival_marker_b = static_cast<std::int16_t>(
                    ship.waypoint_arrival_marker_b - 1);
                if (ship.waypoint_arrival_marker_b < 1) {
                  ship.waypoint_arrival_marker_b = 0;
                  ship.waypoint_arrival_marker_a = 0;
                }
              }
            } else {
              ship.turn_bank_animation_phase = 0.0F;
            }
          } else {
            ship.turn_bank_animation_phase += scale;
            if (dwell_f < ship.turn_bank_animation_phase) {
              ship.turn_bank_animation_phase = 0.0F;
              ship.waypoint_arrival_marker_b =
                  static_cast<std::int16_t>(ship.waypoint_arrival_marker_b + 1);
              if (cls->animation_cycle_count <=
                  ship.waypoint_arrival_marker_b) {
                ship.waypoint_arrival_marker_b =
                    static_cast<std::int16_t>(cls->animation_cycle_count - 1);
                ship.waypoint_arrival_marker_a = 0;
              }
            }
          }
          // Flags 0x0080: re-trigger the unfold once the ship has gone 45
          // ticks (0x2d) without firing and is not already fully unfolded.
          if ((flags & 0x0080U) != 0U && ship.waypoint_arrival_marker_a < 1 &&
              ship.waypoint_arrival_marker_b <
                  static_cast<std::int16_t>(cls->animation_cycle_count - 1) &&
              ship.last_weapon_fire_time_ms + 0x2dU <= state.tick_60hz) {
            ship.waypoint_arrival_marker_a = 1;
          }
        }
      }
      // Flags 0x0001 (banking) needs no per-frame state: ai_turn_bias_dir is
      // set by the movement tick and read at draw time.
    }
  }

  // Alt overlay cycle. The original gates the whole alt update on the sheet
  // existing and (Flags 0x0020 clear or the ship not disabled). When that
  // gate is false it simply skips the assignment/cycle; it does NOT hide the
  // sprite, so the sheet keeps its last drawn frame (the renderer therefore
  // draws alt whenever the sheet exists, matching the original).
  const bool alt_visible = cls->alt_image_id > 0 &&
                           cls->alt_sprite_cycle_count > 0 &&
                           !((flags & 0x0020U) != 0U && disabled);
  if (alt_visible) {
    if ((flags & 0x0010U) == 0U || !disabled) {
      ship.sprite_animation_timer += scale;
    }
    // When the Flags-0x0008 arm owns the timer the alt reuses its step latch;
    // otherwise the alt advances on its own AnimDelay timer.
    if (!base8_active && dwell_f < ship.sprite_animation_timer) {
      ship.sprite_animation_timer = 0.0F;
      stepped = true;
    }
    if (stepped) {
      ship.alternate_sprite_cycle_index =
          static_cast<std::int16_t>(ship.alternate_sprite_cycle_index + 1);
      if (cls->alt_sprite_cycle_count <= ship.alternate_sprite_cycle_index) {
        ship.alternate_sprite_cycle_index = 0;
      }
    }
  }
}

// Ghidra 0x00428340 Ship_UpdateVisualState, weapon-effects + running-lights
// slice. See the header for scope notes. Constants decoded from data: the
// weapon flash ceiling is 32.0 (0x42000000), the binary64 decay scale is
// g_ship_weapon_glow_decay_scale (0x00575ab8) = 0.333, and the fully-off
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
  // The original also folds the per-ship distance-brightness fog into NPC glow
  // (level = min(level, trunc(32 - distance_brightness*1.5)) while
  // distance_brightness > 0, then clamps level >= 0; 32.0 = double at
  // 0x00575348, 1.5 = k_pd_range_scalar_mult_f64 0x00575340). The player
  // (ship_instance_id 0) is always at distance 0, so the cap only bites NPCs.
  if (cls->engine_glow_image_id > 0) {
    const std::int16_t flicker =
        static_cast<std::int16_t>(RandomBelow(state, 6));
    std::int16_t level =
        static_cast<std::int16_t>(ship.engine_glow_level + flicker - 4);
    if (level < 2) {
      ship.engine_glow_intensity = 0.0F;
    } else {
      if (level > 0x20) {
        level = 0x20;
      }
      const int distance_brightness =
          Sprite_DistanceBrightness(NovaSystem_GetEffectiveMurkPercent(state),
                                    state.player.pos_x,
                                    state.player.pos_y,
                                    ship.pos_x,
                                    ship.pos_y);
      const float fog_cap = 32.0F - static_cast<float>(distance_brightness) *
                                        1.5F; // k_pd_range_scalar_mult_f64
      if (fog_cap < static_cast<float>(level)) {
        level = static_cast<std::int16_t>(fog_cap); // trunc toward zero
        if (level < 0) {
          level = 0;
        }
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
          static_cast<float>(RandomBelow(state, span) + cls->blink_val_a);
      ship.light_blink_timer = static_cast<float>(cls->blink_val_c);
    }
  } else {
    // 0 / -1: always on at full brightness.
    ship.light_intensity = 32.0F;
  }

  // Flags 0x0040 (Bible: hide running light sprites when the ship is
  // disabled) forces the light layer off after the blink state machine has
  // run, so a disabled hull goes dark regardless of BlinkMode
  // (Ghidra 0x0042947c tests flags & 0x40, 0x0042948e writes intensity 0).
  // The renderer's <= 1.0 visibility threshold then hides the layer.
  if ((cls->sprite_behavior_flags & 0x0040U) != 0U &&
      NovaAiShip_IsDisabled(state, ship)) {
    ship.light_intensity = 0.0F;
  }
}

// Ghidra 0x00428340 Ship_UpdateVisualState, cloak-fade slice. See the header
// for scope notes. Constants decoded from data: g_cloak_fade_rate_fast
// (0x00575308) = 1.5, g_cloak_fade_rate_slow (0x0057530c) = 0.75,
// g_cloak_fade_progress_max (0x00575314) = 32.0, k_unit_f32
// (0x00575318) = 1.0. The original indexes g_ship_class_defs directly; the
// port's defensive class lookup only changes behavior for hulls the
// Ship_HandleShip prologue would already have deactivated. The rate selector
// is a gated BUGFIX(original); see the block below.
void NovaShip_TickCloakFadeState(GameState &state,
                                 Ship &ship,
                                 float elapsed_ticks) {
  if (ship.cloak_transition_latch == 0) {
    // Passive decay: an interrupted fade bleeds back toward fully visible at
    // one unit per raw call (no g_avg_frame_tick_scale multiplication in the
    // original). Time-adjust against the original loop's 21 ms floor.
    if (ship.cloak_fade_progress > 0.0F && ship.cloak_fade_progress < 32.0F) {
      ship.cloak_fade_progress -= RawSpaceflightCallTicks(elapsed_ticks);
    }
  } else {
    // The original selects 1.5 iff ShipClass.flags_secondary bit 0x0001 is set
    // (disasm 0x004289a3-0x004289d0), i.e. Bible shïp Flags2 0x0001 "Ship
    // exhibits swarming behavior" -- not the cloaking outfit's ModType 17
    // ModVal 0x0001 "Faster fading". The shipped data inverts the intent (the
    // fast-fading Polaris organ ships are non-swarming; swarming Wraiths carry
    // a non-fast-fade device). BUGFIX(original): under the compatibility
    // policy, gate the fade on the actual device bit instead.
    float fade_rate;
    if constexpr (kApplyOriginalBugFixes) {
      fade_rate = NovaOutfit_HasCloakFastFade(state, ship) ? 1.5F : 0.75F;
    } else {
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      fade_rate = (cls != nullptr && (cls->flags_secondary & 0x1U) != 0U)
                      ? 1.5F
                      : 0.75F;
    }
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
  // Destroyed ships cannot remain partially cloaked; force the transition
  // negative so the remaining fade returns to zero.
  if (ship.cloak_fade_progress > 0.0F && NovaAiShip_IsDestroyed(ship)) {
    ship.cloak_transition_latch = -2;
  }

  // Hull-rect jitter (Ghidra 0x00428340 at 0x0042b0b2): a partially cloaked
  // hull is nudged by an independent random offset on each axis in
  // [-m, m], m = trunc(progress / g_cloak_jitter_divisor_f32 (10.0)); every
  // composite layer (hull, glow, lights, weapon, alt) gets the same offsets.
  // Consumes the two NovaRandom_Range rolls the original spends here so the
  // session RNG stream stays aligned. The original skips the whole block when
  // the hull Sprite is null (0x0042b0a7); the port gates on the class's decoded
  // base image.
  const ShipClass *hull_class = ShipClassFor(state, ship);
  const bool has_hull_sprite =
      hull_class != nullptr && hull_class->base_image_id != 0;
  if (has_hull_sprite && ship.cloak_fade_progress > 0.0F &&
      ship.cloak_fade_progress < 32.0F) {
    const auto magnitude =
        static_cast<std::int32_t>(ship.cloak_fade_progress / 10.0F);
    const std::int32_t range = magnitude * 2 + 1;
    ship.cloak_jitter_x =
        static_cast<std::int16_t>(magnitude - RandomBelow(state.rng, range));
    ship.cloak_jitter_y =
        static_cast<std::int16_t>(magnitude - RandomBelow(state.rng, range));
  } else {
    ship.cloak_jitter_x = 0;
    ship.cloak_jitter_y = 0;
  }
}

// Ghidra 0x00428340 Ship_UpdateVisualState, cloak-ability cache tail. The
// original resets the three caches to -1 for any hull that is inactive or in a
// different system than the player, and otherwise lazily populates each one on
// its first active frame. Index 0 of the cache pair (screen) is tested first
// by Outfit_HasCloakScannerRevealForSurface; radar follows. The
// damage-deactivate latch is independent of the two scanner surfaces.
void NovaShip_RefreshCloakAbilityCaches(GameState &state, Ship &ship) {
  if (!ship.is_active ||
      ship.current_system_id != state.player.current_system_id) {
    ship.cloak_scanner_reveal_screen = -1;
    ship.cloak_scanner_reveal_radar = -1;
    ship.cloak_damage_deactivate_latch = -1;
    return;
  }
  for (int surface = 0; surface < 2; ++surface) {
    std::int16_t &cache = surface == 0 ? ship.cloak_scanner_reveal_screen
                                       : ship.cloak_scanner_reveal_radar;
    if (cache < 0) {
      cache = NovaOutfit_HasCloakScannerRevealForSurface(
                  state,
                  ship,
                  surface == 0 ? CloakScannerSurface::kScreen
                               : CloakScannerSurface::kRadar)
                  ? 1
                  : 0;
    }
  }
  if (ship.cloak_damage_deactivate_latch < 0) {
    ship.cloak_damage_deactivate_latch =
        NovaOutfit_HasCloakDamageDeactivateFlag(state, ship) ? 1 : 0;
  }
}

// Ghidra 0x0046e470 Ship_ResolveShipTintColor.
NovaShipTintColor NovaShip_ResolveTintColor(const GameState &state,
                                            const Ship &ship) {
  std::int16_t red = static_cast<std::int16_t>(state.ship_paint_rgb5[0]);
  std::int16_t green = static_cast<std::int16_t>(state.ship_paint_rgb5[1]);
  std::int16_t blue = static_cast<std::int16_t>(state.ship_paint_rgb5[2]);

  if (ship.ship_instance_id != 0) {
    if (ship.pers_def_slot == -1) {
      red = 0x20;
      green = 0x20;
      blue = 0x20;
      if (ship.faction_or_government_id != -1) {
        const Government *gov =
            state.scenario.GovernmentByIndex(ship.faction_or_government_id);
        if (gov != nullptr) {
          red = static_cast<std::int16_t>(gov->ship_red << 8);
          green = static_cast<std::int16_t>(gov->ship_green << 8);
          blue = static_cast<std::int16_t>(gov->ship_blue << 8);
        }
      }
    } else if (ship.pers_def_slot >= 0 &&
               static_cast<std::size_t>(ship.pers_def_slot) <
                   state.scenario.pers_defs.size()) {
      const PersDef &pers =
          state.scenario
              .pers_defs[static_cast<std::size_t>(ship.pers_def_slot)];
      red = pers.color_r5;
      green = pers.color_g5;
      blue = pers.color_b5;
    }
  }
  if (red == 0 && green == 0 && blue == 0) {
    red = 0x20;
    green = 0x20;
    blue = 0x20;
  }
  return NovaShipTintColor{red, green, blue};
}

// Ghidra 0x00428340 Ship_UpdateVisualState cloak render slice. See the header
// for the field semantics; this is the pure derivation the SDL draw options
// consume. Constants decoded from data: g_cloak_fade_progress_max (0x00575314)
// = 32.0, g_cloak_jitter_divisor_f32 (0x00575374) = 10.0. The per-channel
// dim is a C integer truncation of (resolved - progress) (x87 FIST plus the
// sign-corrected truncation idiom at 0x0042b1b7..), then a sub-2 result
// collapses to 0 when the resolved channel is below 0x10, else 2.
ShipCloakPresentation NovaShip_CloakPresentation(
    const GameState &state, const Ship &ship, const NovaShipTintColor &tint) {
  ShipCloakPresentation p;
  const float progress = ship.cloak_fade_progress;
  if (!(progress > 0.0F)) {
    return p;
  }
  const std::array<std::int16_t, 3> resolved{tint.red, tint.green, tint.blue};
  const auto dim_tint = [progress](std::int16_t channel) -> std::int16_t {
    const auto dimmed =
        static_cast<std::int16_t>(static_cast<float>(channel) - progress);
    if (dimmed < 2) {
      return channel < 0x10 ? 0 : 2;
    }
    return dimmed;
  };
  p.progress = progress;
  if (progress < 32.0F) {
    // Partial fade: additive ghost whose per-channel intensity falls with the
    // fade; the glow/light/weapon layers are capped so they vanish with it.
    p.additive = true;
    p.hull_tint = {
        dim_tint(resolved[0]), dim_tint(resolved[1]), dim_tint(resolved[2])};
    p.effect_cap = 32.0F - progress;
    p.weapon_cap = 40.0F - progress;
    p.jitter_x = static_cast<float>(ship.cloak_jitter_x);
    p.jitter_y = static_cast<float>(ship.cloak_jitter_y);
    return p;
  }
  // Full fade: the original sets hull brightness 0x40 (no blit at all) except
  // for the player, a player escort (squad_leader_ship_slot == 0), or a hull
  // the player's screen scanner reveals, which get brightness 0x1e. That
  // brightness gives src_factor = tint + 2 and a 30/32 destination retention;
  // the port approximates it with a source-alpha blend (documented
  // divergence). The glow/light/shield layers are hidden outright, but the
  // weapon layer still runs through the shared cap block at 0x0042b7f2.
  const bool reveal = ship.ship_instance_id == 0 ||
                      ship.squad_leader_ship_slot == 0 ||
                      state.player.cloak_scanner_reveal_screen == 1;
  p.effect_cap = 0.0F;
  p.weapon_cap = 40.0F - progress;
  if (!reveal) {
    p.hidden = true;
    return p;
  }
  float source_weight = 0.0F;
  for (std::int16_t channel : resolved) {
    source_weight = std::max(source_weight,
                             static_cast<float>(dim_tint(channel) + 2) / 32.0F);
  }
  p.hull_alpha = source_weight;
  return p;
}

} // namespace game
