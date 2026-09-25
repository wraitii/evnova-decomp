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
using weapon_detail::BankAmmo;
using weapon_detail::BankSecondary;
using weapon_detail::ChooseBestTurretQuadrantForTarget;
using weapon_detail::RoundHeadingDeg;
using weapon_detail::TurretBearingDegForShip;
using weapon_detail::WeaponAt;

namespace {
// Ghidra Weapon_FireShipWeapons (0x00414550) combat-rating cooldown scales
// (k_npc_fire_cooldown_scale_1p75/1p5/1p25/1p1; double constants at
// 0x00575118/0x00575130/0x005751d0/0x005751c0). Larger cooldown = slower fire,
// so hostile NPCs are gentle on weak players and full-rate on veterans.
constexpr double kNpcFireCooldownScale1p75 = 1.75;
constexpr double kNpcFireCooldownScale1p5 = 1.5;
constexpr double kNpcFireCooldownScale1p25 = 1.25;
constexpr double kNpcFireCooldownScale1p1 = 1.1;

[[nodiscard]] std::int16_t PlayerFireSoundPriorityWidth(const Weapon &weapon) {
  // Weapon_FirePlayerWeaponBank 0x00455150: beam modes 0/3 and launch bays
  // use 6; other secondary-trigger banks use 6; ordinary primary fire uses 5.
  if (weapon.weapon_mode_code == 0 || weapon.weapon_mode_code == 3 ||
      weapon.weapon_mode_code == 99 || (weapon.flags & 0x0002U) != 0U) {
    return 6;
  }
  return 5;
}

} // namespace

// Ghidra 0x0046c320 Weapon_SelectTurretQuadrant: pick and advance the firing
// barrel quadrant for the weapon's turret group, offsetting muzzle_pos to the
// barrel. The bearing is the DISPLAYED rotation frame scaled back to degrees
// (frame * 360 / frames_per_rotation, floor idiom). When the weapon has
// flags_tertiary 0x10 and a target position is supplied, the quadrant is
// replaced by the target-nearest barrel (0x0046c4e0); an out-of-range stored
// quadrant re-rolls randomly in [0,4). Returns the quadrant used, or -1 when
// the weapon has no valid turret group. The quadrant advances modulo 4 after
// every shot (per-ship state, Ship.muzzle_quadrant).
std::int16_t NovaWeapon_SelectTurretQuadrant(GameState &state,
                                             Ship &ship,
                                             std::int16_t weapon_id,
                                             float &muzzle_x,
                                             float &muzzle_y,
                                             const float *target_pos) {
  const Weapon *w = WeaponAt(state, weapon_id);
  const ShipClass *cls = ShipClassFor(state, ship);
  if (w == nullptr || cls == nullptr) {
    return -1;
  }
  const int group = static_cast<int>(w->turret_group_id);
  if (group < 0 || group >= 4) {
    return -1;
  }
  const int frames =
      cls->frames_per_rotation > 0 ? cls->frames_per_rotation : 36;
  const std::int16_t ship_bearing_deg = TurretBearingDegForShip(ship, frames);
  auto &quadrant_state = ship.muzzle_quadrant[static_cast<std::size_t>(group)];
  if ((w->flags_tertiary & 0x10) != 0 && target_pos != nullptr) {
    quadrant_state = static_cast<std::int8_t>(
        ChooseBestTurretQuadrantForTarget(*cls,
                                          muzzle_x,
                                          muzzle_y,
                                          ship_bearing_deg,
                                          group,
                                          *target_pos,
                                          *(target_pos + 1)));
  }
  if (quadrant_state < 0 || quadrant_state > 3) {
    quadrant_state = static_cast<std::int8_t>(RandomBelow(state, 4));
  }
  const int quadrant = quadrant_state;
  ApplyTurretSpreadVelocity(
      *cls, muzzle_x, muzzle_y, ship_bearing_deg, group, quadrant);
  quadrant_state = static_cast<std::int8_t>((quadrant + 1) & 3);
  // The original re-validates after the advance (unreachable with the mod-4
  // update) and re-rolls randomly when still invalid.
  if (quadrant_state < 0 || quadrant_state > 3) {
    quadrant_state = static_cast<std::int8_t>(RandomBelow(state, 4));
  }
  return static_cast<std::int16_t>(quadrant);
}

// @port 0x00455150 88% gameplay,audio
// Ghidra 0x00455150 Weapon_FirePlayerWeaponBank.
void NovaWeapon_FirePlayerWeaponBank(GameState &state,
                                     std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return;
  }
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (w == nullptr) {
    return;
  }
  Ship &player = state.player;

  // Cloak gate: while the player is cloaked past the visibility threshold only
  // weapons with flags_secondary 0x4000 may fire.
  if (NovaTargeting_ShipAtCloakVisibilityThreshold(player) &&
      (w->flags_secondary & 0x4000U) == 0U) {
    return;
  }
  // NovaTime_GetTickCount60Hz() records the fire time and, for Flags 0x02|0x80
  // classes with a non-zero unfold marker, folds the ship and returns before
  // any projectile spawns (Weapon_FirePlayerWeaponBank 0x00455150, before the
  // cooldown check).
  player.last_weapon_fire_time_ms = state.tick_60hz;
  if (const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(player.ship_class_id + 0x80))) {
    if ((cls->sprite_behavior_flags & 0x0002U) != 0U &&
        (cls->sprite_behavior_flags & 0x0080U) != 0U &&
        player.waypoint_arrival_marker_b > 0) {
      player.waypoint_arrival_marker_a = -1;
      return;
    }
  }
  if (state.weapon_bank_cooldown[weapon_bank] > 0.0F) {
    return; // still cooling down
  }
  if (player.ai_station_hold_timer != 0.0F) {
    return; // the original requires an exactly-zero station-hold timer
  }

  const std::int16_t mode = w->weapon_mode_code;
  const std::int16_t target_slot = player.primary_target_ship_slot;
  const bool has_target =
      target_slot >= 0 &&
      target_slot < static_cast<std::int16_t>(GameState::kMaxShips) &&
      state.ShipAt(static_cast<std::size_t>(target_slot)).is_active;

  const int burst_attempts =
      NovaWeapon_GetWeaponBurstAttempts(state, player, weapon_bank);
  int volley_fired = 0;
  const float heading_deg = player.heading * (180.0F / 3.14159265358979323846F);

  for (int attempt = 0; attempt < burst_attempts; ++attempt) {
    if (!NovaWeapon_CanFireWeaponBank(state, player, weapon_bank)) {
      continue;
    }
    bool fired = false;
    if (mode == -1 || mode == 1 || mode == 5 || mode == 6) {
      // Straight projectile (-1/6), homing (1), freefall (5).
      fired = NovaWeapon_SpawnProjectile(state,
                                         0,
                                         target_slot,
                                         weapon_bank,
                                         /*spawn_without_owner=*/false,
                                         /*apply_random_spread=*/true) >= 0;
    } else if (mode == 0) {
      // Fixed beam along the current heading (the original queues the record
      // with the live target slot; the endpoint stays heading-driven).
      fired = NovaWeapon_QueueBeamHit(state,
                                      0,
                                      target_slot,
                                      weapon_bank,
                                      /*forced_targeting=*/-1,
                                      static_cast<std::int16_t>(heading_deg));
    } else if (mode == 3 || mode == 4) {
      // Turreted beam (3) / turreted projectile (4): fire only while the
      // target is NOT inside a turret blind-spot sector.
      if (has_target) {
        const Ship &target =
            state.ShipAt(static_cast<std::size_t>(target_slot));
        const std::int16_t bearing =
            static_cast<std::int16_t>(std::lround(BearingDeg(
                player.pos_x, player.pos_y, target.pos_x, target.pos_y)));
        if (!NovaAi_WeaponIsTargetBearingInTurretBlindSpot(
                *state.scenario.Ship(
                    static_cast<std::int16_t>(player.ship_class_id + 0x80)),
                *w,
                static_cast<std::int16_t>(heading_deg),
                bearing)) {
          if (mode == 3) {
            fired = NovaWeapon_QueueBeamHit(state,
                                            0,
                                            target_slot,
                                            weapon_bank,
                                            /*forced_targeting=*/-1,
                                            bearing);
          } else {
            fired = NovaWeapon_SpawnProjectile(
                        state, 0, target_slot, weapon_bank, false, true) >= 0;
          }
        }
      }
    } else if (mode == 7 || mode == 8) {
      // Guided launch gate: mode 7 requires the target within 46 deg of the
      // nose, mode 8 of the tail (heading + 180). Outside the gate, mode 7
      // dumb-fires without a target; mode 8 holds fire.
      if (has_target) {
        const Ship &target =
            state.ShipAt(static_cast<std::size_t>(target_slot));
        const float tb =
            BearingDeg(player.pos_x, player.pos_y, target.pos_x, target.pos_y);
        const float reference =
            (mode == 8) ? std::remainder(heading_deg + 180.0F, 360.0F)
                        : heading_deg;
        const float delta = std::abs(std::remainder(tb - reference, 360.0F));
        if (delta < 46.0F) {
          fired = NovaWeapon_SpawnProjectile(
                      state, 0, target_slot, weapon_bank, false, true) >= 0;
        } else if (mode == 7) {
          fired = NovaWeapon_SpawnProjectile(
                      state, 0, -1, weapon_bank, false, true) >= 0;
        }
      } else if (mode == 7) {
        fired = NovaWeapon_SpawnProjectile(
                    state, 0, -1, weapon_bank, false, true) >= 0;
      }
    } else if (mode == 99) {
      // Carrier-bay launch (Weapon_SpawnShipFromCarrierBayWeapon): the port
      // does not spawn bay ships from the fire path yet.
      // TODO(decomp(0x00455150)) skipped: launch-bay ship spawn.
    }

    if (!fired) {
      continue;
    }
    ++volley_fired;
    // flags_secondary 0x200: muzzle sprite flash to level 32. The renderer
    // draws the weapon-effects layer at this brightness and
    // NovaShip_TickWeaponSpriteAndRunningLights fades it (Ghidra
    // Weapon_FirePlayerWeaponBank 0x00455150).
    if ((w->flags_secondary & 0x200U) != 0U) {
      player.weapon_sprite_flash_level = 32.0F;
    }

    // Per-shot cost (skipped for burst-counted weapons, flags_tertiary 0x1;
    // they pay once per burst-cycle wrap below). Energy weapons (cost -1) are
    // free per shot; codes in [-999,-1] spend nothing; codes < -999 draw fuel
    // at (|cost| - 1000) * 0.1 units; [0,255] consumes one round from that
    // bank's secondary counter. Mode-99 bays spend from their own counter.
    if ((w->flags_tertiary & 0x0001U) == 0U) {
      const int cost = w->ammo_type;
      if (cost < -999) {
        player.fuel_points = std::max(
            0.0F, player.fuel_points - static_cast<float>(-cost - 1000) * 0.1F);
      } else {
        std::int16_t spend_slot = -1;
        if (cost >= 0 && cost <= 0xff) {
          spend_slot = static_cast<std::int16_t>(cost);
        } else if (mode == 99) {
          spend_slot = weapon_bank;
        }
        if (spend_slot != -1) {
          std::int16_t &counter = BankSecondary(state, spend_slot);
          counter =
              static_cast<std::int16_t>(std::max<std::int16_t>(0, counter - 1));
        }
      }
    }
  }

  if (volley_fired < 1) {
    return;
  }
  // A volley actually fired: queue the fire sound (the port plays it through
  // the spaceflight loop's sound queue; the original's beam modes 0/3 play
  // theirs through a separate immediate NovaAudio_PlaySpatialByDistance call,
  // approximated here by the same queued path).
  if (w->fire_sound >= 0) {
    state.pending_fire_sounds.push_back({w->fire_sound,
                                         player.pos_x,
                                         player.pos_y,
                                         PlayerFireSoundPriorityWidth(*w),
                                         (w->flags & 0x0010U) != 0U});
  }

  // Kickback (resource "recoil"): rearward polar impulse of kickback /
  // hull_mass, clamped per axis to the class base speed
  // (Math_AddPolarVelocityWithClamp 0x0043b4e0).
  if (w->kickback_impulse > 0) {
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(player.ship_class_id + 0x80));
    if (cls != nullptr && cls->mass_tons > 0) {
      const float rear_bearing = std::remainder(heading_deg + 180.0F, 360.0F);
      Math_AddPolarVelocityWithClamp(rear_bearing *
                                         (3.14159265358979323846F / 180.0F),
                                     static_cast<float>(w->kickback_impulse) /
                                         static_cast<float>(cls->mass_tons),
                                     cls->speed,
                                     player.vel_x,
                                     player.vel_y);
    }
  }

  // Bank cooldown: flags_primary 0x40 weapons reload for the fixed interval;
  // the rest scale by the volley and the mount count (a second identical
  // weapon doubles the fire rate).
  if ((w->flags & 0x0040U) == 0U) {
    const int mount_count =
        std::max(1, static_cast<int>(BankAmmo(state, weapon_bank)));
    state.weapon_bank_cooldown[weapon_bank] =
        static_cast<float>(volley_fired) *
        static_cast<float>(std::max(1, static_cast<int>(w->reload_ticks))) /
        static_cast<float>(mount_count);
  } else {
    state.weapon_bank_cooldown[weapon_bank] =
        static_cast<float>(w->reload_ticks);
  }

  // flags_tertiary 0x20 (linked fire): every other bank's cooldown rises to
  // at least this bank's cooldown + 2.0 ticks (_DAT_00575698), so bank groups
  // fire in sequence instead of simultaneously.
  if ((w->flags_tertiary & 0x0020U) != 0U) {
    const float floor = state.weapon_bank_cooldown[weapon_bank] + 2.0F;
    for (std::int16_t b = 0; b < 0x100; ++b) {
      if (b != weapon_bank) {
        float &cd = state.weapon_bank_cooldown[b];
        if (cd < floor) {
          cd = floor;
        }
      }
    }
  }

  // Burst cycle: advance the per-bank counter; on the wrap edge of a
  // burst-counted weapon (flags_tertiary 0x1) pay one burst's worth of
  // ammo/fuel; when the counter reaches Weapon_GetWeaponFireIntervalTicks
  // (0x0046f270) it resets and the bank preloads burst_reset_cooldown.
  if (w->burst_cycle_ticks > 0) {
    std::int16_t &cycle = state.weapon_bank_burst_counter[weapon_bank];
    cycle = static_cast<std::int16_t>(cycle + 1);
    if ((w->flags_tertiary & 0x0001U) != 0U &&
        cycle % w->burst_cycle_ticks == 0) {
      const int pay = (w->flags & 0x0040U) != 0U ? volley_fired : 1;
      if (mode == 99) {
        std::int16_t &counter = BankSecondary(state, weapon_bank);
        counter = static_cast<std::int16_t>(std::max<std::int16_t>(
            0, counter - static_cast<std::int16_t>(pay)));
      } else {
        const int cost = w->ammo_type;
        if (cost < -999) {
          player.fuel_points =
              std::max(0.0F,
                       player.fuel_points -
                           static_cast<float>(pay * (-cost - 1000)) * 0.1F);
        } else if (cost >= 0 && cost <= 0xff) {
          std::int16_t &counter =
              BankSecondary(state, static_cast<std::int16_t>(cost));
          counter = static_cast<std::int16_t>(std::max<std::int16_t>(
              0, counter - static_cast<std::int16_t>(pay)));
        }
      }
    }
    const int interval =
        (w->flags & 0x0040U) != 0U
            ? w->burst_cycle_ticks
            : std::max(1, static_cast<int>(BankAmmo(state, weapon_bank))) *
                  w->burst_cycle_ticks;
    if (cycle >= interval) {
      cycle = 0;
      state.weapon_bank_cooldown[weapon_bank] =
          static_cast<float>(w->burst_reset_cooldown);
    }
  }
}

namespace {

bool QueuePointDefenseBeamHit(GameState &state,
                              std::int16_t owner_ship_slot,
                              std::int16_t shot_slot,
                              std::int16_t weapon_bank,
                              std::int16_t bearing_deg) {
  std::size_t free_slot = state.beam_hit_queue.size();
  for (std::size_t i = 0; i < state.beam_hit_queue.size(); ++i) {
    if (state.beam_hit_queue[i].lifetime_ticks < -1) {
      free_slot = i;
      break;
    }
  }
  if (free_slot == state.beam_hit_queue.size() || shot_slot < 0 ||
      static_cast<std::size_t>(shot_slot) >= state.active_shots.size() ||
      !NovaWeapon_QueueBeamHit(state,
                               owner_ship_slot,
                               -1,
                               weapon_bank,
                               /*forced_targeting=*/1,
                               bearing_deg)) {
    return false;
  }
  BeamHit &beam = state.beam_hit_queue[free_slot];
  const ActiveShot &shot =
      state.active_shots[static_cast<std::size_t>(shot_slot)];
  beam.target_shot_slot = shot_slot;
  beam.target_x = shot.pos_x;
  beam.target_y = shot.pos_y;
  return true;
}

} // namespace

// @port 0x0043A310 100%
// Ghidra 0x0043a310 Weapon_SelectTurretTargetWithinArc.
void NovaWeapon_SelectTurretTargetWithinArc(GameState &state, Ship &ship) {
  if (ship.ai_station_hold_timer > 0.0F) {
    return;
  }
  const bool player = ship.ship_instance_id == 0;
  auto ammo = [&](std::int16_t bank) -> std::int16_t {
    return player
               ? BankAmmo(state, bank)
               : ship.npc_weapon_count_by_class[static_cast<std::size_t>(bank)];
  };
  auto secondary = [&](std::int16_t bank) -> std::int16_t & {
    return player ? BankSecondary(state, bank)
                  : ship.npc_weapon_secondary_count_by_class
                        [static_cast<std::size_t>(bank)];
  };
  auto cooldown = [&](std::int16_t bank) -> float & {
    return player
               ? state.weapon_bank_cooldown[static_cast<std::size_t>(bank)]
               : ship.npc_weapon_bank_cooldown[static_cast<std::size_t>(bank)];
  };
  auto burst_counter = [&](std::int16_t bank) -> std::int16_t & {
    return player
               ? state.weapon_bank_burst_counter[static_cast<std::size_t>(bank)]
               : ship.npc_weapon_bank_burst_counter[static_cast<std::size_t>(
                     bank)];
  };

  std::int16_t bank = -1;
  const Weapon *weapon = nullptr;
  for (std::int16_t candidate = 0; candidate < 0x100; ++candidate) {
    const Weapon *w = WeaponAt(state, candidate);
    if (w == nullptr ||
        (w->weapon_mode_code != 9 && w->weapon_mode_code != 10) ||
        ammo(candidate) <= 0 || cooldown(candidate) > 0.0F ||
        !NovaWeapon_CanFireWeaponBank(state, ship, candidate)) {
      continue;
    }
    bank = candidate;
    weapon = w;
    break;
  }
  if (bank < 0 || weapon == nullptr) {
    return;
  }

  const int reach =
      weapon->weapon_mode_code == 9
          ? static_cast<int>(static_cast<int>(weapon->range_scalar) * 1.5)
          : static_cast<int>(weapon->beam_length_px);
  const int reach_sq = reach * reach;
  const ShipClass *ship_class = ShipClassFor(state, ship);
  if (ship_class == nullptr) {
    return;
  }
  const std::int16_t ship_heading = static_cast<std::int16_t>(
      ship.heading * (180.0F / 3.14159265358979323846F));
  auto eligible_geometry =
      [&](float x, float y, int &distance_sq, std::int16_t &bearing) {
        const float dx = x - ship.pos_x;
        const float dy = y - ship.pos_y;
        distance_sq = static_cast<int>(dx * dx + dy * dy);
        if (distance_sq > reach_sq) {
          return false;
        }
        bearing =
            static_cast<std::int16_t>(BearingDeg(ship.pos_x, ship.pos_y, x, y));
        return !NovaAi_WeaponIsTargetBearingInTurretBlindSpot(
            *ship_class, *weapon, ship_heading, bearing);
      };

  std::int16_t target_slot = -1;
  std::int16_t target_kind = -1; // 0 shot, 1 ship
  std::int16_t target_bearing = 0;
  int best_distance_sq = 0;
  const std::size_t shot_count =
      std::min<std::size_t>(state.active_shots.size(), 0x80);
  for (std::size_t i = 0; i < shot_count; ++i) {
    const ActiveShot &shot = state.active_shots[i];
    const Weapon *shot_weapon = WeaponAt(state, shot.weapon_id);
    const bool protects_leader =
        ship.squad_leader_ship_slot != -1 &&
        shot.target_ship_slot == ship.squad_leader_ship_slot;
    if (shot.consumed || !(shot.life_ticks_remaining > 0.0F) ||
        shot_weapon == nullptr || shot_weapon->weapon_mode_code != 1 ||
        shot.guidance_state != 0 || (shot_weapon->flags & 0x0080U) != 0U ||
        (shot.target_ship_slot != ship.ship_instance_id && !protects_leader)) {
      continue;
    }
    int distance_sq = 0;
    std::int16_t bearing = 0;
    if (eligible_geometry(shot.pos_x, shot.pos_y, distance_sq, bearing) &&
        (target_slot == -1 || distance_sq < best_distance_sq)) {
      target_slot = static_cast<std::int16_t>(i);
      target_kind = 0;
      target_bearing = bearing;
      best_distance_sq = distance_sq;
    }
  }

  if (target_slot == -1) {
    for (std::size_t i = 0; i < GameState::kMaxShips; ++i) {
      Ship &candidate = state.ShipAt(i);
      const ShipClass *candidate_class = ShipClassFor(state, candidate);
      if (!candidate.is_active ||
          candidate.ship_instance_id == ship.ship_instance_id ||
          candidate.ship_instance_id == ship.squad_leader_ship_slot ||
          candidate_class == nullptr ||
          (candidate_class->flags_secondary & 0x0008U) == 0U ||
          NovaAiShip_IsDisabled(state, candidate) ||
          !NovaAiShip_CanEngageTargetUnderCloakRules(state, candidate, ship)) {
        continue;
      }
      bool pressing = false;
      if (i == 0) {
        pressing = NovaAiShip_ShouldKeepPressingTarget(state, ship);
      } else {
        pressing = NovaAiShip_IsShipLockedOnAttackerInState4(candidate, ship);
        if (!pressing && ship.squad_leader_ship_slot >= 0 &&
            state.SlotInRange(
                static_cast<std::size_t>(ship.squad_leader_ship_slot))) {
          pressing = NovaAiShip_IsShipLockedOnAttackerInState4(
              candidate,
              state.ShipAt(
                  static_cast<std::size_t>(ship.squad_leader_ship_slot)));
        }
      }
      int distance_sq = 0;
      std::int16_t bearing = 0;
      if (pressing &&
          eligible_geometry(
              candidate.pos_x, candidate.pos_y, distance_sq, bearing) &&
          (target_slot == -1 || distance_sq < best_distance_sq)) {
        target_slot = static_cast<std::int16_t>(i);
        target_kind = 1;
        target_bearing = bearing;
        best_distance_sq = distance_sq;
      }
    }
  }
  if (target_slot == -1) {
    return;
  }

  bool fired = false;
  if (weapon->weapon_mode_code == 9) {
    const int spawned = NovaWeapon_SpawnProjectile(
        state, ship.ship_instance_id, -1, bank, false, false);
    if (spawned >= 0) {
      ActiveShot &pd = state.active_shots[static_cast<std::size_t>(spawned)];
      const float target_x =
          target_kind == 0
              ? state.active_shots[static_cast<std::size_t>(target_slot)].pos_x
              : state.ShipAt(static_cast<std::size_t>(target_slot)).pos_x;
      const float target_y =
          target_kind == 0
              ? state.active_shots[static_cast<std::size_t>(target_slot)].pos_y
              : state.ShipAt(static_cast<std::size_t>(target_slot)).pos_y;
      pd.pos_x = ship.pos_x;
      pd.pos_y = ship.pos_y;
      pd.vel_x = ship.vel_x;
      pd.vel_y = ship.vel_y;
      const float target_pos[2]{target_x, target_y};
      NovaWeapon_SelectTurretQuadrant(
          state, ship, bank, pd.pos_x, pd.pos_y, target_pos);
      float heading =
          target_kind == 0
              ? BearingDeg(pd.pos_x, pd.pos_y, target_x, target_y)
              : static_cast<float>(NovaAi_AimWeaponPredictiveFrom(
                    state,
                    ship,
                    state.ShipAt(static_cast<std::size_t>(target_slot)),
                    bank,
                    pd.pos_x,
                    pd.pos_y));
      if (weapon->inaccuracy > 0) {
        heading += static_cast<float>(
            RandomBelow(state, weapon->inaccuracy * 2) - weapon->inaccuracy);
      }
      pd.heading_deg = static_cast<float>(RoundHeadingDeg(heading));
      AddPolarVelocity(pd.heading_deg,
                       weapon->projectile_speed / 100.0F,
                       pd.vel_x,
                       pd.vel_y);
      fired = true;
    }
  } else if (target_kind == 0) {
    fired = QueuePointDefenseBeamHit(
        state, ship.ship_instance_id, target_slot, bank, target_bearing);
  } else {
    fired = NovaWeapon_QueueBeamHit(state,
                                    ship.ship_instance_id,
                                    target_slot,
                                    bank,
                                    /*forced_targeting=*/-1,
                                    target_bearing);
  }
  if (!fired) {
    return;
  }

  cooldown(bank) +=
      static_cast<float>(weapon->reload_ticks) / static_cast<float>(ammo(bank));
  if (weapon->fire_sound >= 0) {
    state.pending_fire_sounds.push_back({weapon->fire_sound,
                                         ship.pos_x,
                                         ship.pos_y,
                                         /*priority_width=*/4,
                                         (weapon->flags & 0x0010U) != 0U});
  }
  if (weapon->ammo_type < -999) {
    ship.fuel_points =
        std::max(0.0F,
                 ship.fuel_points -
                     static_cast<float>(-weapon->ammo_type - 1000) * 0.1F);
  } else if (weapon->ammo_type >= 0 &&
             (weapon->flags_secondary & 0x0001U) == 0U) {
    const std::int16_t spend = player ? weapon->ammo_type : bank;
    if (spend >= 0 && spend < 0x100) {
      secondary(spend) = static_cast<std::int16_t>(
          std::max(0, static_cast<int>(secondary(spend)) - 1));
    }
  }
  if (weapon->burst_cycle_ticks > 0) {
    std::int16_t &cycle = burst_counter(bank);
    cycle = static_cast<std::int16_t>(cycle + 1);
    const std::int16_t interval =
        (weapon->flags & 0x0040U) != 0U
            ? weapon->burst_cycle_ticks
            : static_cast<std::int16_t>(ammo(bank) * weapon->burst_cycle_ticks);
    if (cycle >= interval) {
      cycle = 0;
      cooldown(bank) = static_cast<float>(weapon->burst_reset_cooldown);
      if ((weapon->flags_secondary & 0x0001U) != 0U) {
        const std::int16_t spend = player ? weapon->ammo_type : bank;
        if (spend >= 0 && spend < 0x100) {
          secondary(spend) = static_cast<std::int16_t>(
              std::max(0, static_cast<int>(secondary(spend)) - 1));
        }
      }
    }
  }
}

// @port 0x00414550 68% gameplay,moddata
// Ghidra Weapon_FireShipWeapons (0x00414550), ported for NPCs as
// NovaWeapon_FireNpcWeaponBank (the player path is separate).
void NovaWeapon_FireNpcWeaponBank(GameState &state, Ship &ship) {
  const std::int16_t bank = ship.active_weapon_bank_slot;
  auto consume_fire_request = [&]() {
    ship.ai_fire_trigger_latch = 0;
    ship.active_weapon_bank_slot = -1;
  };
  // Weapon_FireShipWeapons (0x00414550) records the fire time and folds a
  // Flags-0x02|0x80 ship with a live unfold marker (and a multi-set animation
  // length) at the very top of the function, before any freeze/eligibility
  // gate or projectile spawn. The fold return deliberately leaves the latched
  // fire request in place, matching the original while the unfold marker is
  // live.
  ship.last_weapon_fire_time_ms = state.tick_60hz;
  if (const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80))) {
    if ((cls->sprite_behavior_flags & 0x0002U) != 0U &&
        (cls->sprite_behavior_flags & 0x0080U) != 0U &&
        ship.waypoint_arrival_marker_b > 0 && cls->animation_cycle_count > 1) {
      ship.waypoint_arrival_marker_a = -1;
      return;
    }
  }
  // Ship_HandleShip (0x00433050) keeps destroyed slots around long enough for
  // their death/debris handling, but Weapon_FireShipWeapons must not launch a
  // bank that was latched before the disabling/lethal hit. The original AI
  // and control paths suppress fire-restricted ships before this handoff; keep
  // the firing boundary defensive so a same-frame hit cannot leave a
  // continuous-fire bank reasserting the weapon sprite flash indefinitely.
  const bool fire_restricted = NovaAiShip_IsDisabled(state, ship);
  if (ship.ship_instance_id == 0 || bank < 0 || bank >= 0x100 ||
      ship.ai_fire_trigger_latch == 0 || fire_restricted ||
      ship.death_timer_active > 0.0F || ship.armor_points <= 0.0F) {
    if (fire_restricted || ship.death_timer_active > 0.0F ||
        ship.armor_points <= 0.0F) {
      consume_fire_request();
    }
    return;
  }
  const std::size_t index = static_cast<std::size_t>(bank);
  const Weapon *weapon =
      state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
  if (weapon == nullptr) {
    consume_fire_request();
    return;
  }
  // Ship_HandleShip (0x004351de..0x0043530d) retains +0x72 and +0xba after
  // handing a flags_primary 0x2 continuous-fire bank to Weapon_FireShipWeapons.
  // Ordinary banks are one-frame requests and are cleared after the handoff,
  // whether or not the bank was ready. Keeping this distinction is also what
  // leaves a live weapon id available to the mode-6/7 predictive-aim path.
  const bool continuous_fire = (weapon->flags & 0x0002U) != 0U;
  auto finish_fire_handoff = [&]() {
    if (!continuous_fire) {
      consume_fire_request();
    }
  };
  if (ship.npc_weapon_count_by_class[index] <= 0 ||
      ship.npc_weapon_bank_cooldown[index] > 0.0F) {
    finish_fire_handoff();
    return;
  }
  // Weapon_CanFireWeaponBank is the authoritative ammo/energy gate.  In
  // particular, energy weapons (ammo_type == -1) can have secondary == 0;
  // that counter is not an ammunition requirement for them.
  if (weapon->weapon_mode_code == 99 &&
      ship.npc_weapon_secondary_count_by_class[index] < 1) {
    finish_fire_handoff();
    return;
  }
  const std::int16_t mode = weapon->weapon_mode_code;

  const std::int16_t target_slot = ship.primary_target_ship_slot;
  const bool has_target =
      target_slot >= 0 &&
      state.SlotInRange(static_cast<std::size_t>(target_slot)) &&
      state.ShipAt(static_cast<std::size_t>(target_slot)).is_active;
  const Ship *target =
      has_target ? &state.ShipAt(static_cast<std::size_t>(target_slot))
                 : nullptr;

  // Weapon_GetWeaponBurstAttempts (0x0046f2c0): how many shots this trigger
  // fires. Non-burst weapons (flags_primary 0x40 clear) get exactly one; a
  // burst bank starts from the mounted ammo count and is capped by the loaded
  // secondary ammo of its cost bank. Fuel-cost (ammo_type < -999) burst capping
  // is deferred (the port does not model fuel-on-weapons).
  int burst_attempts = 1;
  if ((weapon->flags & 0x0040U) != 0) {
    burst_attempts = ship.npc_weapon_count_by_class[index];
    if ((weapon->flags_tertiary & 0x0001U) != 0) {
      const int cost = weapon->ammo_type;
      if (cost >= 0 && cost <= 0xff) {
        burst_attempts =
            std::min(burst_attempts,
                     static_cast<int>(ship.npc_weapon_secondary_count_by_class
                                          [static_cast<std::size_t>(cost)]));
      }
    }
    burst_attempts = std::max(0, burst_attempts);
  }
  if (burst_attempts < 1) {
    finish_fire_handoff();
    return;
  }

  // Weapon_IsTargetBearingInTurretBlindSpot (0x0046b360): fixed
  // forward/side/rear sector test.
  // Front <46 deg, side <136 deg, else rear; each sector is BLIND when the
  // weapon's flags_primary bit 0x1000/0x2000/0x4000 (or the ship class
  // capability flags) is set. Turret weapons usually clear all three bits, so
  // blind_spot() is false and turreted modes fire through the reach branch.
  const ShipClass *ship_cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  auto blind_spot = [&](float target_bearing_deg) {
    const float delta = std::abs(std::remainder(
        target_bearing_deg - ship.heading * (180.0F / 3.14159265358979323846F),
        360.0F));
    if (delta < 46.0F) {
      return (weapon->flags & 0x1000U) != 0U ||
             (ship_cls != nullptr &&
              (ship_cls->capability_flags & 0x1000U) != 0U);
    }
    if (delta < 136.0F) {
      return (weapon->flags & 0x2000U) != 0U ||
             (ship_cls != nullptr &&
              (ship_cls->capability_flags & 0x2000U) != 0U);
    }
    return (weapon->flags & 0x4000U) != 0U ||
           (ship_cls != nullptr &&
            (ship_cls->capability_flags & 0x4000U) != 0U);
  };
  // Weapon_FireShipWeapons (0x00414550) uses BeamLength + 32 for mode 3 and
  // the post-load projectile range (+0x5c) + 32 for mode 4. Modes 7/8 use the
  // same projectile range envelope for their quadrant checks.
  const float projectile_turret_reach = weapon->range_scalar + 32.0F;
  const float beam_turret_reach =
      static_cast<float>(weapon->beam_length_px) + 32.0F;

  const std::size_t cost_index = [&]() -> std::size_t {
    const int cost = weapon->ammo_type;
    return (cost >= 0 && cost <= 0xff) ? static_cast<std::size_t>(cost) : index;
  }();

  // ---- burst loop (Weapon_FireShipWeapons 0x00414550) ----
  int shots_fired = 0;
  for (int attempt = 0; attempt < burst_attempts; ++attempt) {
    // Weapon_CanFireWeaponBank (0x00468990) per-burst gate on THIS SHIP's own
    // banks. Cooldown is already 0 at entry and stays 0 through the burst;
    // the firing bank must still carry loaded ammo. Cloak gating
    // (flags_secondary 0x4000) is deferred.
    if (!NovaWeapon_CanFireWeaponBank(state, ship, bank)) {
      continue;
    }
    bool fired = false;
    if (mode == 0) {
      // Beam: no arc gate (original's mode -1/0/6 block fires regardless).
      fired = NovaWeapon_QueueBeamHit(
          state,
          ship.ship_instance_id,
          ship.primary_target_ship_slot,
          bank,
          -1,
          static_cast<std::int16_t>(ship.heading *
                                    (180.0F / 3.14159265358979323846F)));
    } else if (mode == 3 || mode == 4) {
      // Turreted beam (3) / turreted unguided (4): fire only when the target
      // is NOT in the fixed arc (the turret's relief role) but within reach.
      if (has_target && target != nullptr) {
        const float tb =
            BearingDeg(ship.pos_x, ship.pos_y, target->pos_x, target->pos_y);
        const float reach =
            (mode == 3) ? beam_turret_reach : projectile_turret_reach;
        if (!blind_spot(tb) && std::abs(ship.pos_x - target->pos_x) < reach &&
            std::abs(ship.pos_y - target->pos_y) < reach) {
          if (mode == 3) {
            fired = NovaWeapon_QueueBeamHit(
                state,
                ship.ship_instance_id,
                ship.primary_target_ship_slot,
                bank,
                -1,
                static_cast<std::int16_t>(std::lround(tb)));
          } else {
            fired = NovaWeapon_SpawnProjectile(state,
                                               ship.ship_instance_id,
                                               ship.primary_target_ship_slot,
                                               bank,
                                               false,
                                               true) >= 0;
          }
        }
      }
    } else if (mode == 7 || mode == 8) {
      // Front (7) / rear (8) quadrant turret: within 46 deg of the nose/tail
      // AND within reach (original's unaff_EBP < 0x2e gate).
      if (has_target && target != nullptr) {
        const float tb =
            BearingDeg(ship.pos_x, ship.pos_y, target->pos_x, target->pos_y);
        const float cur_deg = ship.heading * (180.0F / 3.14159265358979323846F);
        const float reference =
            (mode == 8) ? std::remainder(cur_deg + 180.0F, 360.0F) : cur_deg;
        const float delta = std::abs(std::remainder(tb - reference, 360.0F));
        if (delta < 46.0F &&
            std::abs(ship.pos_x - target->pos_x) < projectile_turret_reach &&
            std::abs(ship.pos_y - target->pos_y) < projectile_turret_reach) {
          fired = NovaWeapon_SpawnProjectile(state,
                                             ship.ship_instance_id,
                                             ship.primary_target_ship_slot,
                                             bank,
                                             false,
                                             true) >= 0;
        }
      }
    } else if (mode == -1 || mode == 1 || mode == 5 || mode == 6 || mode == 9) {
      // Straight projectile (-1/6), homing (1), freefall (5): fire toward the
      // primary target. Mode 1 requires a live target like the original. Mode
      // 9 (point defense) keeps firing at the primary target -- its dedicated
      // targeting (Weapon_SelectTurretTargetWithinArc 0x0043a310) is deferred.
      if (mode == 1 && !has_target) {
        continue;
      }
      fired = NovaWeapon_SpawnProjectile(state,
                                         ship.ship_instance_id,
                                         ship.primary_target_ship_slot,
                                         bank,
                                         false,
                                         true) >= 0;
    } else {
      finish_fire_handoff();
      return; // unsupported weapon mode in this bank
    }
    if (!fired) {
      continue;
    }
    ++shots_fired;
    // Per-burst ammo consumption (one per successful shot, mirroring the
    // original's loop).
    const std::int16_t secondary =
        ship.npc_weapon_secondary_count_by_class[index];
    if (secondary > 0 && ship.pers_def_slot != 0x3ff &&
        (weapon->flags_tertiary & 0x0001U) == 0U) {
      ship.npc_weapon_secondary_count_by_class[index] =
          static_cast<std::int16_t>(secondary - 1);
    }
  }

  if (shots_fired < 1) {
    finish_fire_handoff();
    return;
  }
  // flags_secondary 0x200: muzzle sprite flash to level 32 (Ghidra
  // Weapon_FireShipWeapons 0x00414550, once per volley after the burst loop).
  if ((weapon->flags_secondary & 0x200U) != 0U) {
    ship.weapon_sprite_flash_level = 32.0F;
  }
  // A volley fired (sVar9 >= 1 in Weapon_FireShipWeapons): queue the fire
  // sound, sourced at this ship, for the spaceflight loop. The original plays
  // it via NovaAudio_PlaySpatialByDistance with the player ship as listener,
  // so NPC fire fades with distance; flags_primary bit 0x10 marks sounds that
  // must not stack (NovaAudio_CountActiveByHandle gate).
  if (weapon->fire_sound >= 0) {
    state.pending_fire_sounds.push_back({weapon->fire_sound,
                                         ship.pos_x,
                                         ship.pos_y,
                                         /*priority_width=*/4,
                                         (weapon->flags & 0x0010U) != 0U});
  }
  const int mount_count =
      std::max(1, static_cast<int>(ship.npc_weapon_count_by_class[index]));
  float fire_cooldown;
  if ((weapon->flags & 0x0040U) == 0) {
    // Original: local_1c = sVar9 * (speed_scalar / mount_count).
    fire_cooldown = static_cast<float>(
                        std::max(1, static_cast<int>(weapon->reload_ticks))) *
                    static_cast<float>(std::max(1, shots_fired)) /
                    static_cast<float>(mount_count);
  } else {
    fire_cooldown = static_cast<float>(weapon->reload_ticks);
  }
  // Ghidra Weapon_FireShipWeapons (0x00414550): when the target is the player
  // (slot 0), scale the cooldown 1.75/1.5/1.25/1.1 as the rating passes
  // base*100/400/800/1600 (full rate above). Base is pinned to the shipped
  // class-0 Strength (2) rather than read live; see game_state.hpp. Applied
  // before the burst block, so a burst wrap still overwrites it unscaled.
  if (ship.primary_target_ship_slot == 0) {
    const std::int32_t reference_strength =
        GameState::kCombatRatingBaseStrength;
    const std::int32_t rating = state.player_combat_rating_points;
    if (rating < reference_strength * 100) {
      fire_cooldown = static_cast<float>(static_cast<double>(fire_cooldown) *
                                         kNpcFireCooldownScale1p75);
    } else if (rating < reference_strength * 400) {
      fire_cooldown = static_cast<float>(static_cast<double>(fire_cooldown) *
                                         kNpcFireCooldownScale1p5);
    } else if (rating < reference_strength * 800) {
      fire_cooldown = static_cast<float>(static_cast<double>(fire_cooldown) *
                                         kNpcFireCooldownScale1p25);
    } else if (rating < reference_strength * 1600) {
      fire_cooldown = static_cast<float>(static_cast<double>(fire_cooldown) *
                                         kNpcFireCooldownScale1p1);
    }
  }

  // Burst cycle (Weapon_FireShipWeapons): count a cycle tick; on the wrap
  // edge (flags_tertiary & 1) consume one round from the secondary ammo bank;
  // when Weapon_GetWeaponFireIntervalTicks is reached, reset the counter and
  // preload the reset cooldown.
  if (weapon->burst_cycle_ticks > 0) {
    ship.npc_weapon_bank_burst_counter[index] = static_cast<std::int16_t>(
        ship.npc_weapon_bank_burst_counter[index] + 1);
    if ((weapon->flags_tertiary & 0x0001U) != 0 &&
        (ship.npc_weapon_bank_burst_counter[index] %
         weapon->burst_cycle_ticks) == 0) {
      auto &cost_secondary =
          ship.npc_weapon_secondary_count_by_class[cost_index];
      cost_secondary = static_cast<std::int16_t>(
          std::max(0, static_cast<int>(cost_secondary) - 1));
    }
    const std::int16_t interval =
        (weapon->flags & 0x0040U) != 0
            ? weapon->burst_cycle_ticks
            : static_cast<std::int16_t>(mount_count *
                                        weapon->burst_cycle_ticks);
    if (interval > 0 && ship.npc_weapon_bank_burst_counter[index] >= interval) {
      ship.npc_weapon_bank_burst_counter[index] = 0;
      fire_cooldown = static_cast<float>(weapon->burst_reset_cooldown);
    }
  }
  ship.npc_weapon_bank_cooldown[index] = fire_cooldown;
  // Weapon_FireShipWeapons (0x00414550) records the bank it just served as the
  // last lead-fired bank, but only for the lead-capable weapon modes {-1, 6}.
  // The ship-AI aim blocks use this when the active bank is unset.
  if (weapon->weapon_mode_code == -1 || weapon->weapon_mode_code == 6) {
    ship.last_fired_weapon_bank_slot = ship.active_weapon_bank_slot;
  }
  finish_fire_handoff();
}

void NovaWeapon_PreloadFireSound(GameState &state,
                                 std::int16_t fire_sound_slot) {
  if (fire_sound_slot < 0 || fire_sound_slot >= 36) {
    return; // no fire sound for this slot
  }
  if (state.weapon_fire_sounds[fire_sound_slot].has_value()) {
    return; // already cached
  }
  // Slot -> snd resource id 200 + slot (see NovaWeapon_FireSoundResourceId).
  const auto resource = NovaResource_LoadSndData(static_cast<std::uint16_t>(
      NovaWeapon_FireSoundResourceId(fire_sound_slot)));
  if (!resource) {
    return; // resource missing; weapon fires silently
  }
  if (auto decoded = NovaSound_Decode(*resource)) {
    state.weapon_fire_sounds[fire_sound_slot] = std::move(*decoded);
    const auto resource_id = NovaWeapon_FireSoundResourceId(fire_sound_slot);
    if (resource_id >= GameState::kGameplaySoundFirstId &&
        static_cast<std::size_t>(resource_id) <
            static_cast<std::size_t>(GameState::kGameplaySoundFirstId) +
                GameState::kGameplaySoundCount) {
      state.gameplay_sounds[resource_id - GameState::kGameplaySoundFirstId] =
          *state.weapon_fire_sounds[fire_sound_slot];
    }
    NovaLog::Info("cached weapon fire sound slot {} (snd id {})",
                  fire_sound_slot,
                  NovaWeapon_FireSoundResourceId(fire_sound_slot));
  } else {
    NovaLog::Warn("weapon fire sound slot {} (snd id {}) failed to decode",
                  fire_sound_slot,
                  NovaWeapon_FireSoundResourceId(fire_sound_slot));
  }
}

// Ghidra 0x004b0740 NovaAudio_PreloadGameplayData (partial: this covers the
// weapon-fire slice of the original's startup snd preload; effect/cloak
// ranges remain TODO(decomp)).
void NovaWeapon_PreloadOwnedFireSounds(GameState &state) {
  // Scan the rebuilt primary weapon banks and preload each distinct owned
  // weapon's fire sound so a bank never misses its first shot (the original
  // preloads the whole g_gameplay_sound_handle_table at startup; here we only
  // load what the player's banks use).
  for (std::int16_t b = 0; b < 0x100; ++b) {
    const std::int16_t ammo = BankAmmo(state, b);
    if (ammo <= 0) {
      continue;
    }
    const Weapon *w = WeaponAt(state, b);
    if (!w || (w->flags & 0x0002U) != 0) {
      continue; // unmounted or a secondary weapon
    }
    NovaWeapon_PreloadFireSound(state, w->fire_sound);
  }
}

void NovaWeapon_PreloadGameplaySounds(GameState &state) {
  std::size_t loaded = 0;
  for (std::size_t offset = 0; offset < GameState::kGameplaySoundCount;
       ++offset) {
    if (state.gameplay_sounds[offset].has_value()) {
      ++loaded;
      continue;
    }
    const auto resource_id =
        static_cast<std::uint16_t>(GameState::kGameplaySoundFirstId + offset);
    const auto resource = NovaResource_LoadSndData(resource_id);
    if (!resource) {
      continue;
    }
    if (auto decoded = NovaSound_Decode(*resource)) {
      state.gameplay_sounds[offset] = std::move(*decoded);
      ++loaded;
    } else {
      NovaLog::Warn("gameplay snd resource {} failed to decode", resource_id);
    }
  }
  NovaLog::Info("preloaded {} gameplay sound resources (ids {}..{})",
                loaded,
                GameState::kGameplaySoundFirstId,
                GameState::kGameplaySoundFirstId +
                    GameState::kGameplaySoundCount - 1);
}

// Ghidra NovaAudio_PlaySpatialByDistance (0x004692e0): the original computes
// left/right channel gains from the rounded source/listener displacement,
// clamps each channel to the [extent/8, extent] band, then averages the
// channels (NovaAudio_QueueCenteredSound -> Audio_AllocateVoiceSlot feeds
// a single mono gain to the mixer). The extent is the sound-volume scaled
// global (0..0x100, NovaAudio_UpdateCenteredGainFromPreference), so the
// *distance* behavior factors out into a pure 0..1 attenuation; the caller's
// master volume carries the original mixer-level preference mapping. Within 200
// px the sound plays at full volume; beyond that the loud channel falls as
// 722500/d^2 (full at 850 px) and the quiet channel as 40000/d^2, each floored
// at 1/8 of the extent. The integer channel truncation, clamp, and (L+R+1)>>1
// average are preserved so the gain matches the original at 1/256 resolution.
float NovaWeapon_ComputeSpatialFireGain(float listener_x,
                                        float listener_y,
                                        float src_x,
                                        float src_y) {
  const int dx = std::lround(src_x - listener_x);
  const int dy = std::lround(src_y - listener_y);
  const std::int64_t dist_sq =
      static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy;
  if (dist_sq <= 40000) {
    return 1.0F; // 200 px or closer (including src == listener): full volume
  }
  // Channel gain as the original computes it with the extent at full scale
  // (E = 0x100): (E * numerator) / dist_sq, truncated, clamped to [E/8, E].
  auto channel = [dist_sq](std::int64_t numerator) -> std::int64_t {
    const std::int64_t gain = (numerator * 256) / dist_sq;
    return std::clamp<std::int64_t>(gain, 32, 256);
  };
  const std::int64_t loud = channel(722500);
  const std::int64_t quiet = channel(40000);
  std::int64_t left = loud;
  std::int64_t right = loud;
  if (dx < 200) {
    if (dx < -200) {
      right = quiet; // source clearly to the left
    }
    // -200 <= dx < 200: horizontally centered, both channels loud
  } else {
    right = loud;
    left = quiet; // source clearly to the right
  }
  const std::int64_t average = (left + right + 1) >> 1;
  return static_cast<float>(average) / 256.0F;
}

} // namespace game
