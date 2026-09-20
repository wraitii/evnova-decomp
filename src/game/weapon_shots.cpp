#include "weapon.hpp"
#include "weapon_internal.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "collision.hpp"
#include "game_state.hpp"
#include "impact_effects.hpp"
#include "nova_math.hpp"
#include "nova_random.hpp"
#include "outfit.hpp"
#include "preferences.hpp"
#include "ship_ai.hpp"
#include "spaceflight.hpp"
#include "targeting.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <random>

namespace game {
using weapon_detail::AddPolarVelocity;
using weapon_detail::ApplyTurretSpreadVelocity;
using weapon_detail::ChooseBestTurretQuadrantForTarget;
using weapon_detail::RoundHeadingDeg;
using weapon_detail::RoundRangeEnvelope;
using weapon_detail::TurretBearingDegForShip;
using weapon_detail::WeaponAt;

namespace {
// Shot_UpdateShotGuidance constants (typed + pre-commented in the Ghidra DB).
inline constexpr float kGuidanceAgeGateF64 = 15.0F; // 0x00575400
inline constexpr float kJamTurnSignF32 = -1.0F;     // 0x00575350
inline constexpr float kRocketBlendOldF32 = 95.0F;  // 0x0057540c
inline constexpr float kRocketBlendNewF32 = 5.0F;   // 0x00575408
inline constexpr float kOnePercentF64 = 0.01F;      // 0x00575368
inline constexpr float kBombNoseTurnRate = 1.0F;    // 0x00575318
inline constexpr float kOriginalRawCallTicks = 21.0F * 0.03F;
} // namespace

// Ghidra 0x00422210 Ship_TallyInboundWeaponThreat.
void NovaWeapon_TallyInboundWeaponThreat(GameState &state) {
  for (std::size_t ship_slot = 0; ship_slot < GameState::kMaxShips;
       ++ship_slot) {
    Ship &ship = state.ShipAt(ship_slot);
    if (!ship.is_active) {
      continue;
    }
    ship.inbound_weapon_threat = 0;

    const std::size_t shot_count = std::min<std::size_t>(
        state.active_shots.size(), static_cast<std::size_t>(0x80));
    for (std::size_t shot_slot = 0; shot_slot < shot_count; ++shot_slot) {
      const ActiveShot &shot = state.active_shots[shot_slot];
      if (shot.consumed || !(shot.life_ticks_remaining > 0.0F) ||
          shot.target_ship_slot != static_cast<std::int16_t>(ship_slot) ||
          shot.guidance_state != 0) {
        continue;
      }
      const Weapon *weapon = state.scenario.Weapon(
          static_cast<std::int16_t>(shot.weapon_id + 0x80));
      if (weapon == nullptr) {
        NovaLog::Todo(
            "inbound threat tally (0x00422210): skipped shot slot {} with "
            "invalid weapon bank {}",
            shot_slot,
            shot.weapon_id);
        continue;
      }

      // The x87 sequence converts the running total after every shot, toward
      // zero. Because the prior total is integral, this is integer division
      // toward zero for each combined-damage contribution independently.
      const int contribution = (static_cast<int>(weapon->mass_damage) +
                                static_cast<int>(weapon->energy_damage)) /
                               2;
      const std::uint16_t wrapped = static_cast<std::uint16_t>(
          static_cast<std::uint16_t>(ship.inbound_weapon_threat) +
          static_cast<std::uint16_t>(contribution));
      ship.inbound_weapon_threat = std::bit_cast<std::int16_t>(wrapped);
    }
  }
}

namespace {

// ---- Guidance math (Math_* helpers cited from Shot_UpdateShotGuidance) ----

// Math_ShortestAngleDeltaDeg is shared from nova_math.hpp (see
// NovaWeapon_TurnShotToward for the direction-recovery use).

// Ghidra 0x0043b6a0 Math_ClampVelocityComponents: componentwise box clamp to
// +/- max_speed.
void ClampVelocityComponents(float &vel_x, float &vel_y, float max_speed) {
  vel_x = std::clamp(vel_x, -max_speed, max_speed);
  vel_y = std::clamp(vel_y, -max_speed, max_speed);
}

// Shared tail of every guidance state: wrap the heading into [0,360), then
// rebuild the shot velocity from scratch as polar(heading, projectile_speed)
// clamped componentwise to the projectile speed.
void WrapHeadingAndRebuildVelocity(const Weapon &weapon, ActiveShot &shot) {
  shot.heading_deg = std::fmod(shot.heading_deg, 360.0F);
  if (shot.heading_deg < 0.0F) {
    shot.heading_deg += 360.0F;
  }
  const float speed = weapon.projectile_speed / 100.0F;
  float vel_x = 0.0F;
  float vel_y = 0.0F;
  AddPolarVelocity(static_cast<float>(RoundHeadingDeg(shot.heading_deg)),
                   speed,
                   vel_x,
                   vel_y);
  ClampVelocityComponents(vel_x, vel_y, speed);
  shot.vel_x = vel_x;
  shot.vel_y = vel_y;
}

// One guidance turn step (shared by states 0/1/999): rotate the shot heading
// toward `bearing` by at most turn_rate degrees, choosing the shorter wrap
// direction (the original splits at 181 degrees).
void TurnShotToward(ActiveShot &shot,
                    int bearing,
                    float turn_rate,
                    float frame_scale) {
  const int heading = RoundHeadingDeg(shot.heading_deg);
  const int delta = ShortestAngleDeltaDeg(bearing, heading);
  if (static_cast<int>(std::abs(turn_rate)) >= delta) {
    return; // within one tick's turn of the bearing; hold
  }
  int forward = bearing - heading;
  forward %= 360;
  if (forward < 0) {
    forward += 360;
  }
  if (forward < 181) {
    shot.heading_deg += turn_rate * frame_scale;
  } else {
    shot.heading_deg -= turn_rate * frame_scale;
  }
}

// NovaRandom_Range([0,n)) stand-in on the GameState LCG.

} // namespace

void NovaWeapon_ClearTransientCombatState(GameState &state) {
  // The original raises a group of one-frame "clear transient sprites" latches
  // at every system/stellar boundary and lets the transition-frame auxiliary
  // pass wipe the matching pools while they are set:
  //   DAT_00596d29 -> shots + beam records + fading fragments
  //   DAT_00596d2a -> freeflight objects (resource-boxes, jettisoned pods)
  //   DAT_00596d2b -> impact effects
  //   DAT_00596d2d -> weapon smoke puffs
  //   g_no_asteroids_latch -> asteroid / drift-debris records (handled by
  //                          NovaAsteroid_InitSystem /
  //                          NovaAsteroid_UpdateSprites)
  // The latches are pure one-shot clear requests (every reader just
  // deactivates and skips the normal update), so the port performs the wipe
  // synchronously at the same transition boundaries instead of modelling four
  // extra flags. The original also retires every ShotState directly during
  // Stellar_RunDockAndLaunchSequence and marks all live beam records inactive
  // during the landing transition. Ship slots (including the player's) get
  // their jamming-score cache reseeded to -1 at allocation; the port resets the
  // persistent player ship here and on outfit changes
  // (NovaOutfit_RecomputeOutfitDerivedState) instead.
  state.player.jamming_score.fill(-1);
  state.active_shots.clear();
  for (BeamHit &beam : state.beam_hit_queue) {
    beam = BeamHit{};
  }
  for (ImpactEffectInstance &effect : state.impact_effect_instances) {
    effect = ImpactEffectInstance{};
  }
  for (FadingEffectInstance &fragment : state.fading_effect_instances) {
    fragment = FadingEffectInstance{};
  }
  for (FreeflightObjectState &object : state.freeflight_objects) {
    object = FreeflightObjectState{};
  }
  state.sw_particles.clear();
  state.sw_particle_tick_accumulator = 0.0F;
  state.shot_trail_tick_accumulator = 0.0F;
  state.pending_fire_sounds.clear();
  state.pending_impact_sounds.clear();
}

int NovaWeapon_SpawnProjectile(GameState &state,
                               std::int16_t owner_ship_slot,
                               std::int16_t target_ship_slot,
                               std::int16_t weapon_id,
                               bool spawn_without_owner,
                               bool apply_random_spread) {
  if (weapon_id < 0 || weapon_id >= 0x100) {
    return -1;
  }
  const Weapon *w = WeaponAt(state, weapon_id);
  if (w == nullptr) {
    return -1;
  }

  const bool owner_in_range =
      owner_ship_slot >= 0 &&
      owner_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips);
  if (!spawn_without_owner && !owner_in_range) {
    return -1;
  }

  ActiveShot shot;
  shot.weapon_id = weapon_id;
  // Shot_SpawnShotFromWeapon stores the owner slot even for ownerless
  // (submunition) spawns; spawn_without_owner only suppresses the
  // owner-relative position/velocity/heading/muzzle/turret setup below, which
  // Shot_SpawnLinkedShotsOnImpact overwrites with the impact context.
  shot.owner_ship_slot = owner_ship_slot;
  shot.target_ship_slot = target_ship_slot;
  shot.system_id = owner_in_range
                       ? state.ShipAt(static_cast<std::size_t>(owner_ship_slot))
                             .current_system_id
                       : state.player.current_system_id;
  shot.damage_decay_elapsed_ticks =
      w->damage_decay_interval_ticks < 1 ? -1.0F : 0.0F;
  shot.damage_decay_points = 0;
  shot.guidance_state = 0;
  shot.point_defense_durability =
      std::max<std::int16_t>(0, w->point_defense_durability);

  const int mode = w->weapon_mode_code;
  // Sprite-container selection, fixed at spawn (Shot_SpawnShotFromWeapon
  // 0x0041fd30): a mode-4 shot from a ship whose class sets Flags3 0x0040 goes
  // to the layer-12 mode4_alt container above ships; every other shot uses the
  // layers-7-9 containers below ships. The owner slot range (-1/0x40+) falls
  // back to the default container.
  if (mode == 4 && owner_in_range) {
    const ShipClass *owner_cls = ShipClassFor(
        state, state.ShipAt(static_cast<std::size_t>(owner_ship_slot)));
    shot.draws_above_ships =
        owner_cls != nullptr && (owner_cls->availability_flags & 0x0040U) != 0U;
  }
  constexpr float kDegPerRad = 180.0F / 3.14159265358979323846F;
  // Shot heading in game degrees throughout; the muzzle geometry converts back
  // to radians locally (Ship.heading is stored in radians).
  float heading = 0.0F;
  // Shot_SpawnShotFromWeapon local_36: modes 4/7/8 fired with a target are
  // "aimed" (bearing to the target, then overwritten by the predictive lead).
  bool aim_led = false;
  // Shot_SpawnShotFromWeapon local_30: parallel-launch side for negative
  // spread weapons (0 = fire along the aim heading; +/-1 = hull heading
  // +/- |spread|, see the velocity block).
  int parallel_side = 0;
  // Weapon_SelectTurretQuadrant result (-1 when the weapon has no turret
  // group or no owner context).
  int turret_quadrant = -1;
  if (owner_in_range && !spawn_without_owner) {
    Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    shot.pos_x = owner.pos_x;
    shot.pos_y = owner.pos_y;
    shot.vel_x = owner.vel_x;
    shot.vel_y = owner.vel_y;
    heading = owner.heading * kDegPerRad;
    const bool target_in_range =
        target_ship_slot >= 0 &&
        target_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips);
    if ((mode == 4 || mode == 7 || mode == 8) && target_in_range) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(target_ship_slot));
      heading = BearingDeg(shot.pos_x, shot.pos_y, target.pos_x, target.pos_y);
      aim_led = true;
    }
    // Player-launched freefall bombs keep only 80% of the ship's velocity
    // (k_bomb_launch_vel_scale_f64 0x005752a0); NPC bays fire a full polar
    // vector instead (see the velocity rules below).
    if (owner_ship_slot == 0 && mode == 5) {
      shot.vel_x *= 0.8F;
      shot.vel_y *= 0.8F;
    }

    // Shot_SpawnShotFromWeapon: Weapon_SelectTurretQuadrant (0x0046c320)
    // picks/advances the barrel quadrant and offsets the spawn position to
    // the muzzle; with a target slot it also feeds the target position for
    // the flags_tertiary 0x10 target-nearest quadrant choice.
    const float *target_pos =
        target_ship_slot >= 0 && target_ship_slot < static_cast<std::int16_t>(
                                                        GameState::kMaxShips)
            ? &state.ShipAt(static_cast<std::size_t>(target_ship_slot)).pos_x
            : nullptr;
    turret_quadrant = NovaWeapon_SelectTurretQuadrant(
        state, owner, weapon_id, shot.pos_x, shot.pos_y, target_pos);
    // Negative shot_random_spread marks parallel multi-barrel launch; the
    // sign of the selected barrel's lateral offset picks the side of the
    // hull heading the volley leaves on (used by the velocity block below).
    if (w->inaccuracy < 0 && turret_quadrant >= 0) {
      const ShipClass *owner_cls = ShipClassFor(state, owner);
      const int group = static_cast<int>(w->turret_group_id);
      if (owner_cls != nullptr && owner_cls->muzzle_ready && group >= 0 &&
          group < 4) {
        const std::int16_t lateral =
            owner_cls->muzzle_lateral[static_cast<std::size_t>(
                group)][static_cast<std::size_t>(turret_quadrant)];
        if (lateral < 0) {
          parallel_side = -1;
        } else if (lateral > 0) {
          parallel_side = 1;
        }
      }
    }
  }

  shot.heading_deg = static_cast<float>(RoundHeadingDeg(heading));

  // Interference confusion (mode 1 + Seeker 0x0008): at launch the system's
  // Interference stat gates a Random(k_interference_scale / frame_scale) roll;
  // a hit latches the 999 weave state for the whole flight.
  if (mode == 1 && (w->flags_quaternary & 0x0008U) != 0U && owner_in_range) {
    const System *system =
        state.scenario.System(static_cast<std::int16_t>(shot.system_id + 0x80));
    if (system != nullptr && system->interference > 0) {
      // 0x0041fd30 truncates 100/frame_scale toward zero (x87 FIST +
      // residual/sign correction), then floors the result at 1.
      const int roll_range =
          std::max(1,
                   static_cast<int>(
                       100.0F / std::max(0.01F, state.last_frame_tick_scale)));
      if (RandomBelow(state, roll_range) + 1 <= system->interference) {
        shot.guidance_state = 999;
      }
    }
  }

  if (aim_led && owner_in_range) {
    // Ship_AimWeaponPredictive overwrites the plain bearing with the intercept
    // lead for every owner (player turrets included). The original measures
    // from the muzzle position (fourth argument), already quadrant-offset.
    const Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    const Ship &target =
        state.ShipAt(static_cast<std::size_t>(target_ship_slot));
    heading = static_cast<float>(NovaAi_AimWeaponPredictiveFrom(
        state, owner, target, weapon_id, shot.pos_x, shot.pos_y));
    shot.heading_deg = static_cast<float>(RoundHeadingDeg(heading));
  }

  // Positive shot_random_spread jitters the firing bearing by
  // [-spread, spread) (NovaRandom_Range(spread*2) - spread), wrapped into
  // [0,360). Mode 5 jitters AFTER the velocity block instead (below).
  auto apply_spread = [&]() {
    if (w->inaccuracy > 0) {
      shot.heading_deg = static_cast<float>(
          static_cast<int>(shot.heading_deg) +
          RandomBelow(state, static_cast<int>(w->inaccuracy) * 2) -
          w->inaccuracy);
      shot.heading_deg =
          std::fmod(std::fmod(shot.heading_deg, 360.0F) + 360.0F, 360.0F);
    }
  };
  if (apply_random_spread && mode != 5) {
    apply_spread();
  }

  // Velocity rules: modes other than 5/6 (and mode 5/6 launched by an NPC bay,
  // owner slot > 0) rebuild the vector as owner velocity + polar(heading,
  // speed). Ownerless or player-launched mode 5/6 keep the inherited velocity
  // only: bombs fall, rockets accelerate onto their heading in the guidance
  // pass. Negative shot_random_spread (parallel_side != 0) fires along the
  // hull heading +/- |spread| from the selected muzzle side. Original quirk:
  // the hull heading is stored in RADIANS but |spread| is added to it and the
  // sum is consumed as DEGREES by Math_AddPolarVelocity -- reproduced as-is.
  const float speed = w->projectile_speed / 100.0F;
  if ((mode != 5 && mode != 6) || shot.owner_ship_slot > 0) {
    if (owner_ship_slot < 0 ||
        owner_ship_slot >= static_cast<std::int16_t>(GameState::kMaxShips) ||
        parallel_side == 0) {
      AddPolarVelocity(static_cast<float>(RoundHeadingDeg(shot.heading_deg)),
                       speed,
                       shot.vel_x,
                       shot.vel_y);
    } else {
      const Ship &owner =
          state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
      const float abs_spread = std::fabs(static_cast<float>(w->inaccuracy));
      float launch_deg = parallel_side > 0 ? abs_spread + owner.heading
                                           : owner.heading - abs_spread;
      launch_deg = std::fmod(std::fmod(launch_deg, 360.0F) + 360.0F, 360.0F);
      AddPolarVelocity(static_cast<float>(RoundRangeEnvelope(launch_deg)),
                       speed,
                       shot.vel_x,
                       shot.vel_y);
    }
  }
  if (apply_random_spread && mode == 5) {
    apply_spread();
  }

  shot.life_ticks_remaining =
      std::max(1.0F, static_cast<float>(w->lifetime_ticks));
  shot.life_frames = static_cast<int>(std::ceil(shot.life_ticks_remaining));
  shot.collision_radius_px = 2.0F;
  // Weapon_GetShotImpactVariant (0x0046c2f0): Flags2 bit 0x1000 makes a
  // weapon disable but not destroy. Ship_ApplyDamageToShip preserves
  // one armor point for that variant.
  shot.impact_variant =
      (w->flags_secondary & 0x1000U) != 0U ? static_cast<std::int8_t>(1) : 0;
  // Shot_SpawnShotFromWeapon (0x0041fd30) marks two non-lethal (leave-one-
  // armor) cases: the owner is locked on a live target (Ship_IsShipLockedOn
  // Target 0x004124f0, target not disabled), or the owner itself is in AI
  // state 0x0D (Ship_IsShipInAiState0x0D 0x004115a0). The locked-on branch
  // applies only to an unvarianted weapon with a real, non-disabled target;
  // the state-0x0D branch is unconditional.
  if (shot.impact_variant == 0 && owner_ship_slot > 0 && owner_in_range &&
      target_ship_slot >= 0 &&
      target_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips) &&
      !NovaAiShip_IsDisabled(
          state, state.ShipAt(static_cast<std::size_t>(target_ship_slot))) &&
      NovaAiShip_IsShipLockedOnTarget(
          state.ShipAt(static_cast<std::size_t>(owner_ship_slot)),
          state.ShipAt(static_cast<std::size_t>(target_ship_slot)))) {
    shot.impact_variant = 1;
  }
  if (owner_ship_slot > 0 && owner_in_range &&
      state.ShipAt(static_cast<std::size_t>(owner_ship_slot)).ai_state_code ==
          0x0D) {
    shot.impact_variant = 1;
  }
  // Suicide weapons (ammo cost -999) consume the firing ship entirely.
  if (w->ammo_type == -999 && owner_in_range && !spawn_without_owner) {
    Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    owner.shield_points = 0.0F;
    owner.armor_points = 0.0F;
  }
  // Seek-channel seeding: each channel rolls a jam vulnerability in
  // [0, JamVuln] (0 when the channel is unused); Shot_UpdateShotGuidance
  // checks these rolls against the target's jamming score per frame.
  for (std::size_t channel = 0; channel < shot.lock_quality.size(); ++channel) {
    const std::int16_t vuln = w->jam_vuln[channel];
    shot.lock_quality[channel] =
        vuln < 1 ? 0 : static_cast<std::int16_t>(RandomBelow(state, vuln + 1));
  }
  state.active_shots.push_back(shot);
  return static_cast<int>(state.active_shots.size() - 1);
}

// Ghidra 0x0043BA30 Shot_AimStellarBatteryShot. The battery's muzzle is its
// map_x/map_y; the intercept uses the target's absolute velocity (the battery
// does not move). Mode 6 freeflight rockets reuse the same two-regime spool-up
// model as Ship_AimWeaponPredictive. Shot speed is the port's
// projectile_speed/100 (pixels/frame), matching the fired velocity.
std::int16_t NovaWeapon_AimStellarBatteryShot(const GameState &state,
                                              const Stellar &battery,
                                              const Ship &target) {
  constexpr float kMode6ThresholdFactor = 19.59F;
  constexpr float kMode6NearSpeedFactor = 0.316F;
  constexpr float kMode6FarTimeBonus = 2.06667F;
  const float origin_x = static_cast<float>(battery.pos_x);
  const float origin_y = static_cast<float>(battery.pos_y);
  std::int16_t bearing = static_cast<std::int16_t>(
      BearingDeg(origin_x, origin_y, target.pos_x, target.pos_y));
  const Weapon *w = state.scenario.Weapon(battery.weapon_id);
  if (w == nullptr) {
    return bearing;
  }
  const float dx = target.pos_x - origin_x;
  const float dy = target.pos_y - origin_y;
  const float dist = std::sqrt(dx * dx + dy * dy);
  const float shot_speed = w->projectile_speed / 100.0F;
  if (shot_speed <= 0.0F) {
    return bearing;
  }
  float time_to_intercept;
  if (w->weapon_mode_code == 6) {
    const float threshold = shot_speed * kMode6ThresholdFactor;
    if (threshold < dist) {
      time_to_intercept = (dist - threshold) / shot_speed + kMode6FarTimeBonus;
    } else {
      time_to_intercept = dist / (shot_speed * kMode6NearSpeedFactor);
    }
  } else {
    time_to_intercept = dist / shot_speed;
  }
  const float intercept_x = target.pos_x + target.vel_x * time_to_intercept;
  const float intercept_y = target.pos_y + target.vel_y * time_to_intercept;
  return static_cast<std::int16_t>(
      BearingDeg(origin_x, origin_y, intercept_x, intercept_y));
}

// Ghidra Stellar_TickStellarDefenseBatteries (0x0042D890) inline shot
// construction. The original indexes g_weapon_defs[StellarDef.field_0x2c] with
// the value the loader stored there, which is a weapon BANK slot, not the raw
// resource id: the loader (0x004bd3c0) reads the raw spob Weapon word
// (+0x23a), maps values < 0x80 to -1 and otherwise subtracts 0x80, so
// field_0x2c == resource_id - 0x80 and the tick's g_weapon_defs index is
// consistent with the bank-indexed table. The port keeps the raw resource id on
// Stellar.weapon_id and resolves it through ScenarioData::Weapon (which
// re-applies the same -0x80), then stores the resulting bank slot on the shot,
// matching the original shot's weapon_id. ShotState +0x24 visibility and +0x40
// retarget_timer are not modelled on ActiveShot. The sound variant (5) is
// likewise not carried by pending_fire_sounds.
int NovaWeapon_SpawnStellarBatteryShot(GameState &state,
                                       const Stellar &battery,
                                       std::int16_t target_ship_slot,
                                       std::int16_t weapon_resource_id) {
  // The original reserves one of the shared 128 ShotState records before
  // initializing it. ActiveShot is a vector in the clean-room model, but this
  // producer must still fail (and leave its battery cooldown unreloaded) when
  // the original pool would be full.
  if (state.active_shots.size() >= 0x80) {
    return -1;
  }
  const Weapon *w = state.scenario.Weapon(weapon_resource_id);
  if (w == nullptr) {
    return -1;
  }
  ActiveShot shot;
  shot.weapon_id = static_cast<std::int16_t>(weapon_resource_id - 0x80);
  shot.owner_ship_slot = -1;
  shot.target_ship_slot = target_ship_slot;
  shot.system_id = state.player.current_system_id;
  shot.pos_x = static_cast<float>(battery.pos_x);
  shot.pos_y = static_cast<float>(battery.pos_y);
  shot.vel_x = 0.0F;
  shot.vel_y = 0.0F;
  shot.impact_variant = (w->flags_secondary & 0x1000U) != 0U
                            ? static_cast<std::int8_t>(1)
                            : static_cast<std::int8_t>(0);
  shot.life_ticks_remaining =
      std::max(1.0F, static_cast<float>(w->lifetime_ticks));
  shot.life_frames = static_cast<int>(std::ceil(shot.life_ticks_remaining));
  shot.collision_radius_px = 2.0F;
  shot.damage_decay_elapsed_ticks =
      w->damage_decay_interval_ticks < 1 ? -1.0F : 0.0F;
  shot.damage_decay_points = 0;
  shot.guidance_state = 0;
  shot.linked_shot_generation = 0;
  // Original frame_cycle_index seed: Random(0x24) unless flags_primary 0x4
  // (a fixed/heading sprite set) is set.
  shot.frame_cycle_index =
      (w->flags & 0x0004U) != 0U ? 0 : RandomBelow(state, 0x24);
  shot.anim_elapsed = 0.0F;
  const Ship *target = nullptr;
  if (target_ship_slot >= 0 &&
      target_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips)) {
    target = &state.ShipAt(static_cast<std::size_t>(target_ship_slot));
  }
  if (target == nullptr) {
    return -1;
  }
  float heading = static_cast<float>(
      NovaWeapon_AimStellarBatteryShot(state, battery, *target));
  if (w->inaccuracy > 0) {
    heading = static_cast<float>(
        static_cast<int>(heading) +
        RandomBelow(state, static_cast<int>(w->inaccuracy) * 2) -
        w->inaccuracy);
    heading = std::fmod(std::fmod(heading, 360.0F) + 360.0F, 360.0F);
  }
  shot.heading_deg = heading;
  const float speed = w->projectile_speed / 100.0F;
  AddPolarVelocity(static_cast<float>(RoundHeadingDeg(shot.heading_deg)),
                   speed,
                   shot.vel_x,
                   shot.vel_y);
  for (std::size_t channel = 0; channel < shot.lock_quality.size(); ++channel) {
    const std::int16_t vuln = w->jam_vuln[channel];
    shot.lock_quality[channel] =
        vuln < 1 ? 0 : static_cast<std::int16_t>(RandomBelow(state, vuln + 1));
  }
  if (w->fire_sound >= 0) {
    state.pending_fire_sounds.push_back({w->fire_sound,
                                         shot.pos_x,
                                         shot.pos_y,
                                         /*priority_width=*/5,
                                         (w->flags & 0x0010U) != 0U});
  }
  state.active_shots.push_back(shot);
  return static_cast<int>(state.active_shots.size() - 1);
}

// Ghidra 0x0046BA30 Ship_FindNearestHittableWeaponTarget. Returns the slot of
// the nearest active ship in the player's system that this shot's weapon can
// hit (Weapon_CanWeaponHitTarget 0x00426ef0 ->
// NovaWeapon_CanProjectileHitShip), minimizing the integer-rounded squared
// distance from the shot position. -1 when no candidate qualifies. Used by
// Shot_SpawnLinkedShotsOnImpact for the Bible Flags2 0x0010 "submunitions fire
// toward nearest valid target" arm.
[[nodiscard]] std::int16_t
FindNearestHittableWeaponTarget(const GameState &state,
                                const ActiveShot &shot) {
  std::int16_t best_slot = -1;
  std::int16_t best_dist_sq = 0;
  for (std::int16_t slot = 0;
       slot < static_cast<std::int16_t>(GameState::kMaxShips);
       ++slot) {
    const Ship &candidate = state.ShipAt(static_cast<std::size_t>(slot));
    if (!candidate.is_active ||
        candidate.current_system_id != state.player.current_system_id) {
      continue;
    }
    if (!NovaWeapon_CanProjectileHitShip(state, shot, slot)) {
      continue;
    }
    // The original narrows each axis to a short before squaring, so the sum
    // wraps at 16 bits; preserve that squared-distance comparison exactly.
    // 0x0046ba30 truncates positions toward zero (x87 FIST + residual/sign).
    const auto dx = static_cast<std::int16_t>(std::abs(
        static_cast<int>(candidate.pos_x) - static_cast<int>(shot.pos_x)));
    const auto dy = static_cast<std::int16_t>(std::abs(
        static_cast<int>(candidate.pos_y) - static_cast<int>(shot.pos_y)));
    const auto dist_sq = static_cast<std::int16_t>(dx * dx + dy * dy);
    if (best_slot == -1 || dist_sq < best_dist_sq) {
      best_slot = slot;
      best_dist_sq = dist_sq;
    }
  }
  return best_slot;
}

// Ghidra 0x00420D30 Shot_SpawnLinkedShotsOnImpact. Spawns the impacting
// weapon's submunitions (Bible SubCount/SubType/SubTheta/SubLimit) at the
// impact position, inheriting owner/target context and incrementing the
// recursion generation for the SubLimit guard. Called by
// Shot_ResolveShotCollisionHit (0x00437780) with allow_linked=1 on the
// blast-proximity path. `impacting_shot` is read only before the first child is
// appended, so a reference into GameState::active_shots is safe here.
void NovaWeapon_SpawnLinkedShotsOnImpact(
    GameState &state,
    const ActiveShot &impacting_shot,
    std::int16_t fallback_target_ship_slot) {
  const Weapon *impacting_weapon = WeaponAt(state, impacting_shot.weapon_id);
  if (impacting_weapon == nullptr) {
    return;
  }
  const std::int16_t linked_weapon_id = impacting_weapon->range_link_weapon_id;
  if (linked_weapon_id == -1 || impacting_weapon->range_link_gate < 1) {
    return;
  }
  if (impacting_weapon->range_link_extra_count > 0 &&
      impacting_weapon->range_link_extra_count <=
          impacting_shot.linked_shot_generation) {
    return;
  }

  // The original resolves the linked weapon's preloaded fire-sound handle and
  // plays it spatialized at the impact against the player as listener. The
  // clean-room queues it for the spaceflight loop, which owns SDL audio; its
  // consumer applies the flags_primary 0x10 "don't retrigger while active"
  // gate.
  const Weapon *linked_weapon = WeaponAt(state, linked_weapon_id);
  if (linked_weapon != nullptr && linked_weapon->fire_sound >= 0 &&
      linked_weapon->fire_sound < 0x100) {
    state.pending_fire_sounds.push_back(
        {linked_weapon->fire_sound,
         impacting_shot.pos_x,
         impacting_shot.pos_y,
         /*priority_width=*/4,
         (linked_weapon->flags & 0x0010U) != 0U});
  }

  for (std::int16_t index = 0; index < impacting_weapon->range_link_gate;
       ++index) {
    const int spawned =
        NovaWeapon_SpawnProjectile(state,
                                   impacting_shot.owner_ship_slot,
                                   fallback_target_ship_slot,
                                   linked_weapon_id,
                                   /*spawn_without_owner=*/true,
                                   /*apply_random_spread=*/true);
    if (spawned < 0) {
      return;
    }
    ActiveShot &child = state.active_shots[static_cast<std::size_t>(spawned)];
    child.pos_x = impacting_shot.pos_x;
    child.pos_y = impacting_shot.pos_y;
    child.vel_x = 0.0F;
    child.vel_y = 0.0F;
    child.linked_shot_generation = impacting_shot.linked_shot_generation + 1;
    child.heading_deg = impacting_shot.heading_deg;
    if (impacting_weapon->weapon_mode_code == 9) {
      child.target_ship_slot = impacting_shot.target_ship_slot;
    } else if ((impacting_weapon->flags_secondary & 0x0010U) != 0U) {
      child.target_ship_slot = -1;
      child.target_ship_slot = FindNearestHittableWeaponTarget(state, child);
      if (child.target_ship_slot == -1) {
        child.target_ship_slot = fallback_target_ship_slot;
      }
      if (child.target_ship_slot == -1) {
        child.heading_deg = impacting_shot.heading_deg;
      } else {
        const Ship &target =
            state.ShipAt(static_cast<std::size_t>(child.target_ship_slot));
        child.heading_deg = BearingDeg(impacting_shot.pos_x,
                                       impacting_shot.pos_y,
                                       target.pos_x,
                                       target.pos_y);
      }
    }
    // Bible SubTheta: positive randomizes each heading within +/-spread;
    // negative fans the rounds deterministically across
    // abs(spread) * (SubCount-1) degrees centred on the parent heading.
    const std::int16_t spread = impacting_weapon->range_link_spread;
    if (spread < 0) {
      const int step = std::abs(static_cast<int>(spread));
      const int total = step * (impacting_weapon->range_link_gate - 1);
      child.heading_deg -= static_cast<float>(total / 2);
      child.heading_deg += static_cast<float>(step * index);
    } else if (spread > 0) {
      const int roll = RandomBelow(state, spread * 2 + 1);
      child.heading_deg += static_cast<float>(spread - roll);
    }
    if (child.heading_deg < 0.0F) {
      child.heading_deg += 360.0F;
    }
    if (child.heading_deg >= 360.0F) {
      child.heading_deg -= 360.0F;
    }
    // Mode 6 keeps the parent's velocity and accelerates under guidance; every
    // other mode rebuilds from the (spread) heading at the linked weapon speed.
    const Weapon *child_weapon = WeaponAt(state, child.weapon_id);
    if (child_weapon != nullptr && child_weapon->weapon_mode_code == 6) {
      child.vel_x = impacting_shot.vel_x;
      child.vel_y = impacting_shot.vel_y;
    } else if (child_weapon != nullptr) {
      AddPolarVelocity(static_cast<float>(RoundHeadingDeg(child.heading_deg)),
                       child_weapon->projectile_speed / 100.0F,
                       child.vel_x,
                       child.vel_y);
    }
  }
}

// Ghidra 0x00431530 Shot_UpdateShotGuidance. Normal homing consumes normalized
// 30 Hz time; asteroid-decoy tracking, interference weaving, random
// opportunities, rocket acceleration, and bomb weathervaning consume raw
// spaceflight calls replayed by the caller at the original 21 ms cadence.
void NovaWeapon_UpdateShotGuidance(GameState &state,
                                   ActiveShot &shot,
                                   float elapsed_ticks,
                                   int raw_call_count) {
  const Weapon *w = WeaponAt(state, shot.weapon_id);
  if (w == nullptr) {
    return;
  }
  const int mode = w->weapon_mode_code;
  if (mode == 9) {
    // Point-defense shots fly straight; the lead was applied at fire time.
    return;
  }
  const float frame_scale = std::max(0.0F, elapsed_ticks);
  const float shot_age =
      static_cast<float>(w->lifetime_ticks) - shot.life_ticks_remaining;
  raw_call_count = std::max(0, raw_call_count);

  // The random parts of normal homing are raw-call opportunities. Perform
  // them before the continuous turn, as in the original call ordering.
  for (int call = 0; call < raw_call_count && shot.guidance_state == 0 &&
                     mode == 1 && frame_scale * kGuidanceAgeGateF64 < shot_age;
       ++call) {
    const std::int16_t target_slot = shot.target_ship_slot;
    if (target_slot < 0 ||
        !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
      continue;
    }
    Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    bool jammed = false;
    for (int channel = 0; channel < 4; ++channel) {
      const int lock = shot.lock_quality[channel];
      if (lock > 0 &&
          NovaAi_GetShipJammingScore(state, target, channel) > 100 - lock) {
        jammed = true;
        break;
      }
    }
    if (jammed && (w->flags_quaternary & 0x8000U) != 0U &&
        RandomBelow(state, 500) == 0 && shot.owner_ship_slot >= 0 &&
        shot.owner_ship_slot <
            static_cast<std::int16_t>(GameState::kMaxShips)) {
      shot.target_ship_slot = shot.owner_ship_slot;
      shot.owner_ship_slot = -1;
      continue;
    }
    if (shot.owner_ship_slot >= 0 &&
        shot.owner_ship_slot <
            static_cast<std::int16_t>(GameState::kMaxShips) &&
        !NovaAiShip_CanEngageTargetUnderCloakRules(
            state,
            state.ShipAt(static_cast<std::size_t>(shot.target_ship_slot)),
            state.ShipAt(static_cast<std::size_t>(shot.owner_ship_slot))) &&
        (w->flags_quaternary & 0x8000U) != 0U &&
        RandomBelow(state, 1000) == 0) {
      shot.target_ship_slot = shot.owner_ship_slot;
      shot.owner_ship_slot = -1;
    }
  }

  if (shot.guidance_state == 0 && mode == 1) {
    // The target must be gone (-1) or active in the shot's system.
    std::int16_t target_slot = shot.target_ship_slot;
    bool target_valid = target_slot == -1;
    if (target_slot != -1) {
      if (state.SlotInRange(static_cast<std::size_t>(target_slot))) {
        Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
        target_valid =
            target.is_active && target.current_system_id == shot.system_id;
      }
    }
    if (target_valid) {
      int bearing = RoundHeadingDeg(shot.heading_deg);
      if (target_slot != -1) {
        Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
        bearing = static_cast<int>(
            BearingDeg(shot.pos_x, shot.pos_y, target.pos_x, target.pos_y));
      }
      float effective_turn = w->guided_turn_rate;
      // Jamming: the first seek channel whose vulnerability roll loses to the
      // target's jamming score dulls or reverses the turn. Owner-retarget RNG
      // was replayed above on the raw-call cadence.
      if (target_slot != -1) {
        Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
        for (int channel = 0; channel < 4; ++channel) {
          const int lock = shot.lock_quality[channel];
          if (lock <= 0) {
            continue;
          }
          if (NovaAi_GetShipJammingScore(state, target, channel) > 100 - lock) {
            if (effective_turn > 0.0F) {
              effective_turn = ((w->flags_quaternary & 0x0010U) != 0U)
                                   ? effective_turn * kJamTurnSignF32
                                   : 0.0F;
            }
            break;
          }
        }
      }
      // A target the owner cannot legitimately engage (cloak rules) makes the
      // missile go dumb; Seeker 0x8000 may retarget the owner instead.
      target_slot = shot.target_ship_slot;
      if (target_slot != -1 && shot.owner_ship_slot >= 0 &&
          shot.owner_ship_slot <
              static_cast<std::int16_t>(GameState::kMaxShips)) {
        const Ship &target =
            state.ShipAt(static_cast<std::size_t>(target_slot));
        const Ship &owner =
            state.ShipAt(static_cast<std::size_t>(shot.owner_ship_slot));
        if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, target, owner)) {
          effective_turn = 0.0F;
        }
      }
      // Seeker 0x4000 (loses lock if target not directly ahead): inside 250 px
      // on both axes with the target more than 45 deg off the nose, drop it.
      if (target_slot != -1 && (w->flags_quaternary & 0x4000U) != 0U) {
        const Ship &target =
            state.ShipAt(static_cast<std::size_t>(target_slot));
        if (std::abs(target.pos_x - shot.pos_x) < 250.0F &&
            std::abs(target.pos_y - shot.pos_y) < 250.0F) {
          const int to_target = static_cast<int>(
              BearingDeg(shot.pos_x, shot.pos_y, target.pos_x, target.pos_y));
          const int off_nose =
              std::abs(to_target - RoundHeadingDeg(shot.heading_deg)) % 360;
          if (off_nose > 45) {
            shot.target_ship_slot = -1;
          }
        }
      }
      if (frame_scale * kGuidanceAgeGateF64 < shot_age) {
        TurnShotToward(shot, bearing, effective_turn, frame_scale);
      }
      WrapHeadingAndRebuildVelocity(*w, shot);
    }
  }

  for (int call = 0; call < raw_call_count; ++call) {
    bool acquired_asteroid_decoy = false;
    if (shot.guidance_state == 0 && mode == 1 &&
        (w->flags_quaternary & 0x0002U) != 0U && RandomBelow(state, 10) == 0) {
      for (std::size_t i = 0; i < state.asteroid_pool.size(); ++i) {
        const AsteroidState &asteroid = state.asteroid_pool[i];
        if (!asteroid.active) {
          continue;
        }
        if (std::abs(asteroid.target_pos_x - shot.pos_x) >= 200.0F ||
            std::abs(asteroid.target_pos_y - shot.pos_y) >= 200.0F) {
          continue;
        }
        const int to_asteroid =
            static_cast<int>(BearingDeg(shot.pos_x,
                                        shot.pos_y,
                                        asteroid.target_pos_x,
                                        asteroid.target_pos_y));
        if (ShortestAngleDeltaDeg(to_asteroid,
                                  RoundHeadingDeg(shot.heading_deg)) < 16) {
          shot.guidance_state = 1;
          shot.target_ship_slot = static_cast<std::int16_t>(i);
          acquired_asteroid_decoy = true;
          break;
        }
      }
    }

    if (shot.guidance_state == 999) {
      if (kGuidanceAgeGateF64 < shot_age) {
        const int phase_counter =
            static_cast<int>(state.shot_guidance_frame_counter) -
            (raw_call_count - 1 - call);
        if (phase_counter % 300 < 150) {
          shot.heading_deg -= w->guided_turn_rate;
        } else {
          shot.heading_deg += w->guided_turn_rate;
        }
      }
      WrapHeadingAndRebuildVelocity(*w, shot);
      if ((w->flags_quaternary & 0x8000U) != 0U &&
          RandomBelow(state, 1000) == 0 && shot.owner_ship_slot >= 0 &&
          shot.owner_ship_slot <
              static_cast<std::int16_t>(GameState::kMaxShips)) {
        shot.guidance_state = 0;
        shot.target_ship_slot = shot.owner_ship_slot;
        shot.owner_ship_slot = -1;
      }
    } else if (!acquired_asteroid_decoy && shot.guidance_state != 0 &&
               shot.guidance_state != 998) {
      int bearing = RoundHeadingDeg(shot.heading_deg);
      if (shot.guidance_state == 1) {
        const std::int16_t asteroid_slot = shot.target_ship_slot;
        if (asteroid_slot >= 0 &&
            asteroid_slot <
                static_cast<std::int16_t>(state.asteroid_pool.size()) &&
            state.asteroid_pool[static_cast<std::size_t>(asteroid_slot)]
                .active) {
          const AsteroidState &asteroid =
              state.asteroid_pool[static_cast<std::size_t>(asteroid_slot)];
          bearing = static_cast<int>(BearingDeg(shot.pos_x,
                                                shot.pos_y,
                                                asteroid.target_pos_x,
                                                asteroid.target_pos_y));
        } else {
          shot.target_ship_slot = -1;
        }
      }
      if (kGuidanceAgeGateF64 < shot_age) {
        TurnShotToward(shot, bearing, w->guided_turn_rate, 1.0F);
      }
      WrapHeadingAndRebuildVelocity(*w, shot);
    }

    if (mode == 6) {
      const float speed = w->projectile_speed / 100.0F;
      float polar_x = 0.0F;
      float polar_y = 0.0F;
      AddPolarVelocity(static_cast<float>(RoundHeadingDeg(shot.heading_deg)),
                       speed,
                       polar_x,
                       polar_y);
      shot.vel_x =
          (shot.vel_x * kRocketBlendOldF32 + polar_x * kRocketBlendNewF32) *
          kOnePercentF64;
      shot.vel_y =
          (shot.vel_y * kRocketBlendOldF32 + polar_y * kRocketBlendNewF32) *
          kOnePercentF64;
    }
    if (mode == 5) {
      const int vel_bearing = static_cast<int>(
          BearingDeg(0.0F, 0.0F, shot.vel_x * 1000.0F, shot.vel_y * 1000.0F));
      if (ShortestAngleDeltaDeg(vel_bearing,
                                RoundHeadingDeg(shot.heading_deg)) > 0) {
        int forward = vel_bearing - RoundHeadingDeg(shot.heading_deg);
        forward %= 360;
        if (forward < 0) {
          forward += 360;
        }
        shot.heading_deg +=
            forward < 181 ? kBombNoseTurnRate : -kBombNoseTurnRate;
      }
    }
  }
}

namespace {
// Ghidra 0x0042f270 beam-scan eligibility. This is the beam path's own inline
// test, NOT the projectile Weapon_CanWeaponHitTarget 0x00426ef0 gate: the beam
// scan does not reject same-government non-squad ships. Conditions, in the
// decompile's order: active, same system, not the owner, ship_class != 0x2ff,
// weapon flags_primary 0x400 == candidate class capability_flags 0x400, not the
// owner's direct subordinate / own squad leader / player-squad mate, and the
// mission-critical (dude booty_flags 0x100) owner/candidate exclusions.
[[nodiscard]] bool BeamCandidateEligible(const GameState &state,
                                         const Ship &owner,
                                         std::int16_t owner_slot,
                                         const Ship &candidate,
                                         std::int16_t candidate_slot,
                                         const Weapon &weapon) {
  constexpr std::int16_t kInvalidShipClass = 0x2ff;
  // 0x0042f270's first contact clause is candidate_slot != owner_slot; a ship
  // never intercepts its own beam.
  if (candidate_slot == owner_slot || !candidate.is_active ||
      candidate.current_system_id != owner.current_system_id ||
      candidate.ship_class_id == kInvalidShipClass) {
    return false;
  }
  const ShipClass *candidate_class = ShipClassFor(state, candidate);
  if (candidate_class == nullptr ||
      (weapon.flags & 0x0400U) !=
          (candidate_class->capability_flags & 0x0400U)) {
    return false;
  }
  // local_42: exclude the owner's direct subordinate, the owner's own squad
  // leader, and a fellow player-squad member.
  bool not_friendly = true;
  if (candidate.squad_leader_ship_slot != -1) {
    not_friendly = candidate.squad_leader_ship_slot != owner_slot;
    if (NovaShip_IsInPlayerSquad(state, candidate) &&
        NovaShip_IsInPlayerSquad(state, owner)) {
      not_friendly = false;
    }
  }
  if (candidate_slot == owner.squad_leader_ship_slot) {
    not_friendly = false;
  }
  if (!not_friendly) {
    return false;
  }
  // bVar8: mission-critical dude exclusions.
  const auto dude_for = [&state](const Ship &ship) -> const DudeDef * {
    if (ship.dude_class_id < 0) {
      return nullptr;
    }
    return state.scenario.Dude(
        static_cast<std::int16_t>(ship.dude_class_id + 0x80));
  };
  bool owner_chain_to_player = false;
  for (std::int16_t current = owner_slot, step = 0;
       step < static_cast<std::int16_t>(GameState::kMaxShips);
       ++step) {
    if (current == 0) {
      owner_chain_to_player = true;
      break;
    }
    if (current < 0 ||
        current >= static_cast<std::int16_t>(GameState::kMaxShips)) {
      break;
    }
    current =
        state.ShipAt(static_cast<std::size_t>(current)).squad_leader_ship_slot;
    if (current == -1) {
      break;
    }
  }
  const DudeDef *candidate_dude = dude_for(candidate);
  if (owner_chain_to_player && candidate_dude != nullptr &&
      (candidate_dude->booty_flags & 0x0100U) != 0U) {
    return false;
  }
  const bool candidate_player_squad =
      candidate_slot == 0 || NovaShip_IsInPlayerSquad(state, candidate);
  if (candidate_player_squad) {
    const DudeDef *owner_dude = dude_for(owner);
    if (owner_dude == nullptr && owner.squad_leader_ship_slot != -1) {
      const Ship &leader =
          state.ShipAt(static_cast<std::size_t>(owner.squad_leader_ship_slot));
      owner_dude = dude_for(leader);
    }
    if (owner_dude != nullptr && (owner_dude->booty_flags & 0x0100U) != 0U) {
      return false;
    }
  }
  return true;
}
} // namespace

// Ghidra 0x00427a90 Shot_QueueBeamHit.
bool NovaWeapon_QueueBeamHit(GameState &state,
                             std::int16_t owner_ship_slot,
                             std::int16_t target_ship_slot,
                             std::int16_t weapon_id,
                             std::int16_t forced_targeting,
                             std::int16_t firing_bearing_deg) {
  if (owner_ship_slot < 0 ||
      owner_ship_slot >= static_cast<std::int16_t>(GameState::kMaxShips) ||
      weapon_id < 0 || weapon_id >= 0x100) {
    return false;
  }
  const Weapon *weapon = WeaponAt(state, weapon_id);
  if (weapon == nullptr) {
    return false;
  }
  for (BeamHit &beam : state.beam_hit_queue) {
    if (beam.lifetime_ticks >= -1) {
      continue;
    }
    Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    const bool target_in_range =
        target_ship_slot >= 0 &&
        target_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips);

    // Shot_QueueBeamHit (0x00427a90): pick the turret-exit quadrant for the
    // weapon's group and advance the ship's per-group rotation exactly once.
    // Every port beam callsite passes the original's -1 sentinel, so the
    // explicit-quadrant arm is not modelled. TODO(decomp(0x00427a90)) skipped:
    // the original indexes g_ship_states+0xc8fe by the raw group id with no
    // bounds check (group < 0 or > 3 reads/advances the neighbouring field
    // and consumes one RNG roll); the port bounds-guards instead, leaving
    // turret_quadrant = -1 so the beam stays at the owner centre, which is the
    // visible original result.
    const int turret_group = weapon->turret_group_id;
    std::int16_t turret_quadrant = -1;
    if (turret_group >= 0 && turret_group < 4) {
      int stored =
          owner.muzzle_quadrant[static_cast<std::size_t>(turret_group)];
      if (stored < 0 || stored > 3) {
        stored = RandomBelow(state, 4);
      }
      if ((weapon->flags_tertiary & 0x0010U) != 0U && forced_targeting != 1 &&
          target_in_range) {
        if (const ShipClass *owner_cls = ShipClassFor(state, owner)) {
          const Ship &target =
              state.ShipAt(static_cast<std::size_t>(target_ship_slot));
          // Disasm 0x00427c98 (FLD dword [owner+0x44]) truncates the raw
          // hull heading (radians) to an int; 0x00427cd9 MOVSX ECX,AX then
          // 0x00427ce2 PUSH ECX passes it to
          // Weapon_ChooseBestTurretQuadrantForTarget (0x0046c4e0) as the
          // bearing. The original feeds radians where degrees are expected;
          // preserved.
          turret_quadrant =
              static_cast<std::int16_t>(ChooseBestTurretQuadrantForTarget(
                  *owner_cls,
                  owner.pos_x,
                  owner.pos_y,
                  static_cast<std::int16_t>(static_cast<int>(owner.heading)),
                  turret_group,
                  target.pos_x,
                  target.pos_y));
        }
      } else {
        turret_quadrant = static_cast<std::int16_t>(stored);
      }
      // The advance is always driven by the stored state, not by the
      // target-nearest choice above (original Shot_QueueBeamHit).
      owner.muzzle_quadrant[static_cast<std::size_t>(turret_group)] =
          static_cast<std::int8_t>((stored + 1) & 3);
    }

    beam.source_x = owner.pos_x;
    beam.source_y = owner.pos_y;
    // Shot_UpdateBeamHitQueue (0x0042f270) re-derives the turret-exit offset
    // every frame from the live owner; apply the same transform at queue time
    // so the first frame is not drawn from the owner centre.
    const ShipClass *owner_cls = ShipClassFor(state, owner);
    if (owner_cls != nullptr && turret_group >= 0 && turret_group < 4 &&
        turret_quadrant >= 0) {
      ApplyTurretSpreadVelocity(
          *owner_cls,
          beam.source_x,
          beam.source_y,
          TurretBearingDegForShip(owner, owner_cls->frames_per_rotation),
          turret_group,
          turret_quadrant);
    }
    beam.target_x = owner.pos_x;
    beam.target_y = owner.pos_y;
    beam.firing_bearing_deg = firing_bearing_deg;
    bool aimed_at_target = false;
    // Shot_UpdateBeamHitQueue (0x0042f270) only uses the target position for
    // turreted/PD modes (3/10); a mode-0 beam keeps the straight heading
    // bearing and ends at its BeamLength.
    if (weapon->weapon_mode_code != 0 && target_in_range) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(target_ship_slot));
      beam.target_x = target.pos_x;
      beam.target_y = target.pos_y;
      aimed_at_target = true;
    }
    if (!aimed_at_target) {
      // No target endpoint: lay the beam downrange along the firing bearing.
      // The original derives endpoints per frame in Shot_UpdateBeamHitQueue
      // (0x0042f270); with no ship in reach the visible length is exactly
      // BeamLength (the +0x20 term is only the AI fire-reach gate).
      const float reach = static_cast<float>(weapon->beam_length_px);
      const float rad = static_cast<float>(firing_bearing_deg) *
                        (3.14159265358979323846F / 180.0F);
      beam.target_x = beam.source_x + std::sin(rad) * reach;
      beam.target_y = beam.source_y - std::cos(rad) * reach;
    }
    beam.lifetime_ticks = std::max<std::int16_t>(1, weapon->lifetime_ticks);
    beam.animation_counter = 0;
    beam.weapon_id = weapon_id;
    beam.owner_ship_slot = owner_ship_slot;
    beam.target_ship_slot = target_ship_slot;
    beam.forced_targeting = forced_targeting;
    beam.turret_quadrant = turret_quadrant;
    beam.turret_group_id = weapon->turret_group_id;
    beam.impact_variant = (weapon->flags_secondary & 0x1000U) != 0U ? 1 : 0;
    // Shot_QueueBeamHit (0x00427a90) also marks locked-on non-player beams
    // non-lethal (leave one armor): owner is a ship, the caller did not force
    // targeting, a live (non-disabled) target exists, and the owner is locked
    // on it (Ship_IsShipLockedOnTarget 0x004124f0).
    if (beam.impact_variant == 0 && owner_ship_slot > 0 &&
        forced_targeting != 1 && target_ship_slot >= 0 &&
        target_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips) &&
        !NovaAiShip_IsDisabled(
            state, state.ShipAt(static_cast<std::size_t>(target_ship_slot))) &&
        NovaAiShip_IsShipLockedOnTarget(
            owner, state.ShipAt(static_cast<std::size_t>(target_ship_slot)))) {
      beam.impact_variant = 1;
    }
    beam.impact_resolved = false;
    return true;
  }
  return false;
}

void NovaWeapon_TickBeamHitQueue(GameState &state, float elapsed_ticks) {
  const float ticks = std::max(0.0F, elapsed_ticks);
  for (BeamHit &beam : state.beam_hit_queue) {
    if (beam.lifetime_ticks < -1) {
      continue;
    }
    const Weapon *beam_weapon = WeaponAt(state, beam.weapon_id);
    // Shot_UpdateBeamHitQueue (0x0042f270) re-derives the source from the live
    // owner each frame, so a beam follows a moving owner rather than freezing
    // at the queue-time muzzle position.
    const bool owner_valid =
        beam.owner_ship_slot >= 0 &&
        beam.owner_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips);
    if (owner_valid) {
      const Ship &owner =
          state.ShipAt(static_cast<std::size_t>(beam.owner_ship_slot));
      beam.source_x = owner.pos_x;
      beam.source_y = owner.pos_y;
      // Shot_UpdateBeamHitQueue (0x0042f270) recomputes the turret-exit
      // offset from the live owner every frame (rotation, near/far scale and
      // drop), so the source tracks a moving/rotating ship instead of
      // freezing at the queue-time muzzle.
      const ShipClass *owner_cls = ShipClassFor(state, owner);
      if (owner_cls != nullptr && beam.turret_group_id >= 0 &&
          beam.turret_group_id < 4 && beam.turret_quadrant >= 0 &&
          beam.turret_quadrant < 4) {
        ApplyTurretSpreadVelocity(
            *owner_cls,
            beam.source_x,
            beam.source_y,
            TurretBearingDegForShip(owner, owner_cls->frames_per_rotation),
            beam.turret_group_id,
            beam.turret_quadrant);
      }
    }
    if (beam.forced_targeting == 1 && beam.target_shot_slot >= 0) {
      const auto shot_slot = static_cast<std::size_t>(beam.target_shot_slot);
      if (shot_slot >= state.active_shots.size() ||
          state.active_shots[shot_slot].consumed ||
          !(state.active_shots[shot_slot].life_ticks_remaining >= 0.0F)) {
        beam.lifetime_ticks = -1;
        beam.target_shot_slot = -1;
      } else {
        ActiveShot &shot = state.active_shots[shot_slot];
        beam.target_x = shot.pos_x;
        beam.target_y = shot.pos_y;
        if (shot.point_defense_durability < 1) {
          shot.consumed = true;
          shot.life_ticks_remaining = 0.0F;
        } else if (beam_weapon != nullptr) {
          const int damage =
              static_cast<int>(beam_weapon->mass_damage) +
              (static_cast<int>(beam_weapon->energy_damage) + 1) / 2;
          shot.point_defense_durability =
              static_cast<std::int16_t>(shot.point_defense_durability - damage);
        }
      }
    } else if (beam_weapon != nullptr && beam_weapon->weapon_mode_code == 0) {
      // Shot_UpdateBeamHitQueue (0x0042f270) keeps a mode-0 beam on the owner's
      // current heading (falling back to the queued bearing when there is no
      // live owner) and scans for the nearest active ship inside a narrow
      // forward cone within BeamLength + ceil(trunc(frame_span*0.66)/2). A
      // hit truncates the visible endpoint to distance - frame_span*0.2;
      // with nothing in reach the beam ends exactly at BeamLength.
      // frame_span = Sprite_GetShipClassEscortFrameWidth (0x004624c0), the
      // FULL frame width (right - left) with fallback 0x4b = 75; 0.66 and 0.2
      // are the doubles DAT_005753d0 / DAT_005753d8.
      // The 0.66 (DAT_005753d0) and 0.2 (DAT_005753d8) scales are IEEE
      // doubles; the original multiplies with FMUL double, then the x87 FIST+
      // residual/sign correction truncates toward zero.
      // CONFIRMED-BUG(original): this is a center-in-sector test, not a hull
      // test, so the original contact is very imprecise. The half-angle is
      // fixed per ship, so its lateral tolerance d*tan(cone) is narrower than
      // the hull at close range (the beam passes through the ship) and wider
      // at long range (phantom hits, with the endpoint truncated in empty
      // space); frame_span is the sprite's full width, so elongated hulls are
      // sized by their longest dimension and over-hit edge-on. Nearest-center
      // selection also ignores occlusion. Kept faithful for fidelity; see
      // "Beam collision detection has holes" in docs/known_original_bugs.md.
      // Deliberately not gated by kApplyOriginalBugFixes.
      constexpr double kBeamReachFrameScale = 0.66;
      constexpr double kBeamTruncateFrameScale = 0.2;
      float bearing_deg = static_cast<float>(beam.firing_bearing_deg);
      if (owner_valid) {
        const Ship &owner =
            state.ShipAt(static_cast<std::size_t>(beam.owner_ship_slot));
        bearing_deg = owner.heading * (180.0F / 3.14159265358979323846F);
      }
      const float beam_length = static_cast<float>(beam_weapon->beam_length_px);
      float visible_length = beam_length;
      std::int16_t hit_slot = -1;
      float best_distance = 0.0F;
      if (owner_valid) {
        // Ghidra 0x0042f270 eligibility (BeamCandidateEligible). This is the
        // beam path's own inline test; it deliberately does NOT reject
        // same-government non-squad targets, matching the original.
        const Ship &owner =
            state.ShipAt(static_cast<std::size_t>(beam.owner_ship_slot));
        for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
          if (!BeamCandidateEligible(state,
                                     owner,
                                     beam.owner_ship_slot,
                                     state.ShipAt(slot),
                                     static_cast<std::int16_t>(slot),
                                     *beam_weapon)) {
            continue;
          }
          const Ship &candidate = state.ShipAt(slot);
          const float frame_height =
              candidate.collision_mask.HasMask()
                  ? static_cast<float>(candidate.collision_mask.mask->height)
                  : 75.0F;
          const int scaled_extent = static_cast<int>(
              static_cast<double>(frame_height) * kBeamReachFrameScale);
          const float dx = candidate.pos_x - beam.source_x;
          const float dy = candidate.pos_y - beam.source_y;
          const float distance = std::sqrt(dx * dx + dy * dy);
          if (distance >
              beam_length + static_cast<float>((scaled_extent + 1) / 2)) {
            continue;
          }
          const float target_bearing = BearingDeg(
              beam.source_x, beam.source_y, candidate.pos_x, candidate.pos_y);
          const float delta =
              std::remainder(target_bearing - bearing_deg, 360.0F);
          if (std::abs(delta) >
              static_cast<float>(scaled_extent) * 10.0F / 32.0F) {
            continue;
          }
          if (hit_slot == -1 || distance < best_distance) {
            best_distance = distance;
            // The original stores the contact-truncated length as a truncated
            // int (x87 FIST + residual/sign correction), not a float.
            const double truncated =
                static_cast<double>(distance) -
                static_cast<double>(frame_height) * kBeamTruncateFrameScale;
            visible_length =
                std::max(0.0F, static_cast<float>(std::trunc(truncated)));
            hit_slot = static_cast<std::int16_t>(slot);
          }
        }
      }
      const float rad = bearing_deg * (3.14159265358979323846F / 180.0F);
      beam.target_x = beam.source_x + std::sin(rad) * visible_length;
      beam.target_y = beam.source_y - std::cos(rad) * visible_length;
      if (!beam.impact_resolved && hit_slot >= 0) {
        NovaWeapon_ResolveDirectWeaponHit(state,
                                          beam.owner_ship_slot,
                                          hit_slot,
                                          beam.weapon_id,
                                          beam.impact_variant);
        beam.impact_resolved = true;
      }
    } else if (beam.target_ship_slot >= 0 &&
               beam.target_ship_slot <
                   static_cast<std::int16_t>(GameState::kMaxShips)) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(beam.target_ship_slot));
      beam.target_x = target.pos_x;
      beam.target_y = target.pos_y;
      if (!beam.impact_resolved && target.is_active) {
        NovaWeapon_ResolveDirectWeaponHit(state,
                                          beam.owner_ship_slot,
                                          beam.target_ship_slot,
                                          beam.weapon_id,
                                          beam.impact_variant);
        beam.impact_resolved = true;
      }
    }
    beam.lifetime_remainder += ticks;
    const std::int16_t whole_ticks =
        static_cast<std::int16_t>(beam.lifetime_remainder);
    if (whole_ticks <= 0) {
      continue;
    }
    beam.lifetime_remainder -= static_cast<float>(whole_ticks);
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(beam.weapon_id + 0x80));
    for (std::int16_t tick = 0; tick < whole_ticks && beam.lifetime_ticks >= 0;
         ++tick) {
      // Decay phase (Bible "Decay"): once the lifetime reaches 0, a beam with
      // a positive Decay value holds on screen while
      // animation_counter + falloff < 0x10, counting animation_counter up;
      // the renderer shrinks the corona / fades the beam with it. The
      // original only ever increments animation_counter in this branch.
      if (beam.lifetime_ticks == 0 && weapon != nullptr &&
          weapon->damage_decay_interval_ticks > 0) {
        beam.animation_counter =
            static_cast<std::int16_t>(beam.animation_counter + 1);
        if (beam.animation_counter + weapon->beam_falloff < 0x10) {
          continue;
        }
      }
      beam.lifetime_ticks = static_cast<std::int16_t>(beam.lifetime_ticks - 1);
    }
    if (beam.lifetime_ticks < 0) {
      beam = BeamHit{};
    }
  }
}

// Ghidra Shot_HandleShot (0x00435830) time-animated shot-frame branch: for a
// weapon with flags_primary bit 0 set, each frame accumulates the real frame
// time into ShotState.anim_elapsed and, when it crosses the weapon's
// beam_width_or_animation_frame_delay (Bible BeamWidth, in 30ths of a second;
// a delay < 1 advances every frame), steps ShotState.frame_cycle_index,
// wrapping at the shot sprite-set's frame count (0, or frame_count-1 when the
// weapon's flags_secondary bit 1 is set). The Light Blaster and the other
// unguided projectiles take the static/heading branch instead, so this only
// drives genuinely time-animated weapon shots.
void NovaWeapon_StepShotAnimation(GameState &state,
                                  ActiveShot &shot,
                                  float elapsed_ticks) {
  const Weapon *w = WeaponAt(state, shot.weapon_id);
  if (!w) {
    return;
  }
  // flags_primary bit 0 clear -> static/heading shot-frame path (no animation).
  if ((w->flags & 0x0001U) == 0) {
    return;
  }
  const std::int16_t frame_delay = w->beam_width_or_animation_frame_delay;
  shot.anim_elapsed += elapsed_ticks;
  if (frame_delay < 1 || shot.anim_elapsed >= static_cast<float>(frame_delay)) {
    shot.frame_cycle_index += 1;
    shot.anim_elapsed = 0.0F;
  }
  // The caller (DrawShots) clamps the displayed frame to the sprite set's frame
  // count, mirroring Shot_HandleShot's wrap. The reverse-wrap (flags_secondary
  // bit 1 -> frame_count - 1) needs the frame count, which lives on the SDL
  // side; DrawShots owns that clamp once the set is resolved.
}

void NovaWeapon_TickShots(GameState &state,
                          float elapsed_ticks,
                          const NovaPreferences *prefs) {
  // Advance shots per Shot_HandleShot (0x00435830): sanitize ownership/target
  // latches, count lifetime down, run the guidance pass, then integrate the
  // position with the (possibly rebuilt) velocity. Shots that cross zero are
  // retired with the weapon's expiry impact; collision-resolved shots are
  // consumed earlier in the frame by the collision pass.
  auto &shots = state.active_shots;
  const float tick_scale = std::max(0.0F, elapsed_ticks);
  state.last_frame_tick_scale = tick_scale;
  // Shot_HandleShot's trail arm is a raw flight-call effect, unlike movement
  // and guidance which integrate elapsed ticks. Bank the presentation delta
  // so a 60 Hz port does not emit twice as many trail particles as the
  // original's 21 ms outer-loop cadence.
  state.shot_trail_tick_accumulator += tick_scale / kOriginalRawCallTicks;
  const int trail_raw_call_count =
      static_cast<int>(std::floor(state.shot_trail_tick_accumulator));
  state.shot_trail_tick_accumulator -= static_cast<float>(trail_raw_call_count);
  const std::uint16_t display_counter_bits =
      std::bit_cast<std::uint16_t>(state.spaceflight_frame_counter);
  state.spaceflight_frame_counter = std::bit_cast<std::int16_t>(
      static_cast<std::uint16_t>(display_counter_bits + 1));
  state.shot_guidance_frame_counter_accumulator +=
      tick_scale / kOriginalRawCallTicks;
  const int raw_call_count = static_cast<int>(
      std::floor(state.shot_guidance_frame_counter_accumulator));
  state.shot_guidance_frame_counter_accumulator -=
      static_cast<float>(raw_call_count);
  const std::uint16_t counter_bits =
      std::bit_cast<std::uint16_t>(state.shot_guidance_frame_counter);
  state.shot_guidance_frame_counter = std::bit_cast<std::int16_t>(
      static_cast<std::uint16_t>(counter_bits + raw_call_count));
  // Linked submunitions append to `shots` when their parent expires. Process
  // only the frame's original shots; the original's fixed pool could revisit a
  // newly allocated earlier slot, while this compacted vector lets children
  // begin on the next tick.
  const std::size_t initial_shot_count = shots.size();
  for (std::size_t shot_index = 0; shot_index < initial_shot_count;
       ++shot_index) {
    ActiveShot &shot = shots[shot_index];
    if (shot.consumed) {
      continue;
    }
    const Weapon *weapon = WeaponAt(state, shot.weapon_id);
    if (weapon == nullptr) {
      shot.consumed = true;
      continue;
    }
    // Owner sanitation and the target latches. Point-defense shots are always
    // targetless (they were lead-aimed at fire time); a mode-1 shot whose
    // recorded target died latches 998 and flies inert for the rest of its
    // life.
    if (shot.owner_ship_slot < -1 ||
        shot.owner_ship_slot >=
            static_cast<std::int16_t>(GameState::kMaxShips)) {
      shot.owner_ship_slot = -1;
    }
    if (weapon->weapon_mode_code == 9) {
      shot.target_ship_slot = -1;
    } else {
      const std::int16_t target = shot.target_ship_slot;
      if (target < 0 ||
          target >= static_cast<std::int16_t>(GameState::kMaxShips)) {
        shot.target_ship_slot = -1;
      } else if (shot.guidance_state == 0 &&
                 !state.ShipAt(static_cast<std::size_t>(target)).is_active) {
        shot.guidance_state = 998;
        shot.target_ship_slot = -1;
      }
    }

    shot.life_ticks_remaining -= tick_scale;
    shot.life_frames =
        static_cast<int>(std::ceil(std::max(0.0F, shot.life_ticks_remaining)));
    if (shot.life_ticks_remaining <= 0.0F) {
      // Ghidra Shot_HandleShot (0x00435830) emits the weapon's expiry/fuse
      // impact for shots in the player's system (gated in the original by
      // life > k_shot_expiry_min_life -63000, which a -1 expiry satisfies).
      if (shot.system_id == state.player.current_system_id) {
        // The expiry path launches linked submunitions unless Flags2 0x20
        // explicitly suppresses that behavior. Snapshot first because the
        // spawner appends to active_shots and may invalidate this reference.
        if (weapon->range_link_gate > 0 &&
            (weapon->flags_secondary & 0x0020U) == 0U) {
          const ActiveShot expiring_shot = shots[shot_index];
          NovaWeapon_SpawnLinkedShotsOnImpact(
              state, expiring_shot, expiring_shot.target_ship_slot);
        }
        NovaEffects_SpawnAreaImpact(state,
                                    shots[shot_index].pos_x,
                                    shots[shot_index].pos_y,
                                    weapon->impact_effect_id,
                                    weapon->splash_radius,
                                    true);
      }
      shots[shot_index].consumed = true;
      continue;
    }
    // Guidance runs before movement: a homing shot turns and rebuilds its
    // velocity, then the (possibly new) vector integrates this frame.
    NovaWeapon_UpdateShotGuidance(state, shot, tick_scale, raw_call_count);
    shot.pos_x += shot.vel_x * tick_scale;
    shot.pos_y += shot.vel_y * tick_scale;
    if (trail_raw_call_count > 0 && tick_scale > 0.0F &&
        (prefs == nullptr || !prefs->smoke_trails) &&
        weapon->trail_particle_count > 0) {
      // Sprite_GetFrameFullWidth(shot->sprite_ref) / 2, rounded up, is the
      // rear-edge anchor used by 0x0043609d. A prepared collision mask carries
      // the exact frame width; otherwise the original's missing/default shot
      // sprite is represented by its 32px full-width default (half = 16).
      const float anchor_offset_px =
          weapon->trail_particle_count > 0 && shot.collision_mask.HasMask()
              ? static_cast<float>((shot.collision_mask.mask->width + 1) / 2)
              : 16.0F;
      for (int raw_call = 0; raw_call < trail_raw_call_count; ++raw_call) {
        // TODO(decomp(0x00436170)) skipped: ActiveShot does not model the
        // original sprite's fade intensity, so the blend weight stays 0x20
        // instead of 0x20 - sprite_intensity.
        NovaEffects_SpawnWeaponTrailParticles(state,
                                              shot.pos_x,
                                              shot.pos_y,
                                              *weapon,
                                              shot.heading_deg,
                                              anchor_offset_px);
      }
    }
    NovaWeapon_StepShotAnimation(state, shot, tick_scale);
    // Bible Decay is measured in 30ths of a second. The original adds
    // g_avg_frame_tick_scale, advances only when elapsed strictly exceeds the
    // interval, resets to zero, and performs at most one decay step per call.
    if (weapon->damage_decay_interval_ticks > 0) {
      shot.damage_decay_elapsed_ticks += tick_scale;
      if (static_cast<float>(weapon->damage_decay_interval_ticks) <
          shot.damage_decay_elapsed_ticks) {
        shot.damage_decay_elapsed_ticks = 0.0F;
        shot.damage_decay_points =
            static_cast<std::int16_t>(shot.damage_decay_points + 1);
      }
    }
  }
  shots.erase(std::remove_if(shots.begin(),
                             shots.end(),
                             [](const ActiveShot &s) { return s.consumed; }),
              shots.end());
  // Cooldown decay moved to NovaWeapon_TickPlayerWeaponBankCooldowns (the
  // faithful PlayerTick_WeaponCommands tail: ammo>0 gate + ionization pin),
  // called from the spaceflight loop's player tick.
}
} // namespace game
