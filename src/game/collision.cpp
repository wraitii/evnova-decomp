#include "collision.hpp"

#include "scenario_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace game {
namespace {

[[nodiscard]] const Weapon *WeaponForShot(const GameState &state,
                                          const ActiveShot &shot) {
  if (shot.weapon_id < 0 || shot.weapon_id >= 0x100) {
    return nullptr;
  }
  return state.scenario.Weapon(
      static_cast<std::int16_t>(shot.weapon_id + 0x80));
}

[[nodiscard]] bool ValidShipSlot(std::int16_t slot) {
  return slot >= 0 && slot < static_cast<std::int16_t>(GameState::kMaxShips);
}

[[nodiscard]] bool IsDestroyed(const Ship &ship) {
  return ship.death_timer_active > 0.0F || ship.armor_points <= 0.0F;
}

[[nodiscard]] bool SharesTargetLeaderChain(const GameState &state,
                                           std::int16_t first_slot,
                                           std::int16_t second_slot) {
  // Ghidra Ship_ShipsShareTargetLeaderChain (0x0046d190) walks each ship's
  // ai_target_ship_slot to a root and excludes shots within one escort leader
  // chain. The current AI model has the same slot-based links, so this keeps
  // the walk local and bounded while tolerating malformed cycles.
  auto root = [&state](std::int16_t slot) {
    std::int16_t current = slot;
    for (std::size_t step = 0; step < GameState::kMaxShips; ++step) {
      if (!ValidShipSlot(current)) {
        return current;
      }
      const std::int16_t next =
          state.ShipAt(static_cast<std::size_t>(current)).ai_target_ship_slot;
      if (!ValidShipSlot(next) || next == current) {
        return current;
      }
      current = next;
    }
    return current;
  };
  return root(first_slot) == root(second_slot);
}

[[nodiscard]] bool HasGovernmentFlag(const GameState &state,
                                     const Ship &ship,
                                     std::uint16_t flag) {
  if (ship.faction_or_government_id < 0 ||
      ship.faction_or_government_id >=
          static_cast<std::int16_t>(state.scenario.governments.size())) {
    return false;
  }
  return (state.scenario
              .governments[static_cast<std::size_t>(
                  ship.faction_or_government_id)]
              .flags_primary &
          flag) != 0;
}

void ApplyWeaponOnHitEffects(Ship &target,
                             const Weapon &weapon,
                             float impact_x,
                             float impact_y) {
  int points = weapon.ionization_points;
  if (weapon.splash_radius > 0) {
    const float dx = target.pos_x - impact_x;
    const float dy = target.pos_y - impact_y;
    const float radius = static_cast<float>(weapon.splash_radius);
    const float distance_sq = dx * dx + dy * dy;
    const float radius_sq = radius * radius;
    if (distance_sq > radius_sq) {
      points = 0;
    } else if (distance_sq > 0.0F) {
      // Weapon_ApplyWeaponOnHitEffects (0x0046f3f0): splash ionization is
      // linearly attenuated by squared distance inside the blast radius.
      points = static_cast<int>(std::lround(
          static_cast<float>(points) * (1.0F - distance_sq / radius_sq)));
    }
  }
  if (points > 0) {
    target.status_effect_points += static_cast<float>(points);
    target.ionization_color |= weapon.ionization_color;
  }
}

void ApplyImpactImpulse(const GameState &state,
                        const ActiveShot &shot,
                        const Weapon &weapon,
                        Ship &target,
                        float impact_x,
                        float impact_y) {
  if (weapon.impact_impulse == 0 || target.ai_station_hold_timer > 0.0F) {
    return;
  }
  const ShipClass *target_class = state.scenario.Ship(
      static_cast<std::int16_t>(target.ship_class_id + 0x80));
  if (target_class == nullptr || target_class->mass_tons <= 0 ||
      (target_class->capability_flags & 0x0400U) != 0U) {
    return;
  }

  const float dx = target.pos_x - impact_x;
  const float dy = target.pos_y - impact_y;
  const float distance = std::hypot(dx, dy);
  if (distance <= 0.0F) {
    return;
  }

  // Math_AddPolarVelocityWithClamp (0x0043b4e0): add impact/mass in the
  // impact-to-target direction, then clamp each component to the class's
  // base-speed bound. Ship-class Speed is stored in hundredths of px/frame.
  const float impulse = static_cast<float>(weapon.impact_impulse) /
                        static_cast<float>(target_class->mass_tons);
  target.vel_x += dx / distance * impulse;
  target.vel_y += dy / distance * impulse;
  const float max_axis_speed = target_class->speed / 100.0F;
  if (max_axis_speed > 0.0F) {
    target.vel_x = std::clamp(target.vel_x, -max_axis_speed, max_axis_speed);
    target.vel_y = std::clamp(target.vel_y, -max_axis_speed, max_axis_speed);
  }
  (void)shot;
}

void ResolveShipHit(const GameState &state,
                    const ActiveShot &shot,
                    Ship &target,
                    std::int16_t target_slot,
                    bool allow_aggro_updates,
                    bool suppress_retarget_logic) {
  const Weapon *weapon = WeaponForShot(state, shot);
  if (weapon == nullptr) {
    return;
  }

  const int armor_damage = static_cast<int>(weapon->mass_damage);
  const int shield_damage = static_cast<int>(weapon->energy_damage);
  const bool bypass_shields = (weapon->flags & 0x0020U) != 0;
  const bool force_armor_only = shot.impact_variant != 0;

  ApplyWeaponOnHitEffects(target, *weapon, shot.pos_x, shot.pos_y);
  ApplyImpactImpulse(state, shot, *weapon, target, shot.pos_x, shot.pos_y);

  // Shot_ResolveShipHitFromWeapon (0x004192d0): shields are consumed first;
  // once they are down, the weapon's armor damage is applied. The original
  // passes shields for weapons with Flags bit 0x20 and applies armor directly.
  if (!bypass_shields) {
    if (shield_damage > 0) {
      target.shield_points -= static_cast<float>(shield_damage);
    }
    if (target.shield_points <= 0.0F) {
      if (armor_damage > 0) {
        if (force_armor_only && target.armor_points > 0.0F &&
            target.armor_points - static_cast<float>(armor_damage) <= 0.0F) {
          target.armor_points = 1.0F;
        } else {
          target.armor_points -= static_cast<float>(armor_damage);
        }
      }
    }
  } else {
    if (armor_damage > 0) {
      if (force_armor_only && target.armor_points > 0.0F &&
          target.armor_points - static_cast<float>(armor_damage) <= 0.0F) {
        target.armor_points = 1.0F;
      } else {
        target.armor_points -= static_cast<float>(armor_damage);
      }
    }
  }

  if (allow_aggro_updates && target_slot > 0) {
    target.ai_hostility_accumulator = static_cast<std::int16_t>(
        std::min(0x7fff,
                 static_cast<int>(target.ai_hostility_accumulator) +
                     armor_damage + shield_damage));
  }

  // Minimal aggro transition from the hit path. Mission reactions, faction
  // reputation, chatter, surrender/disable transitions, and player HUD
  // messages are deliberately outside this first slice.
  if (allow_aggro_updates && shot.owner_ship_slot > 0 && target_slot > 0) {
    target.primary_target_ship_slot = shot.owner_ship_slot;
    if (target.ai_target_ship_slot < 0) {
      target.ai_target_ship_slot = shot.owner_ship_slot;
    }
  } else if (allow_aggro_updates && shot.owner_ship_slot == 0 && target_slot > 0) {
    target.primary_target_ship_slot = 0;
    target.ai_target_ship_slot = 0;
    if (target.ai_state_code == 0) {
      target.ai_state_code = 3;
    }
  }

  // Shot_ResolveShipHitFromWeapon refreshes the non-bypass hit reaction timer.
  // The clean-room renderer does not consume this yet, but retaining the latch
  // prevents later status/AI work from losing the event.
  if (!bypass_shields) {
    target.hit_reaction_timer = 32.0F;
  }
  (void)suppress_retarget_logic;
}

} // namespace

bool NovaWeapon_CanProjectileHitShip(const GameState &state,
                                     const ActiveShot &shot,
                                     std::int16_t target_slot) {
  const Weapon *weapon = WeaponForShot(state, shot);
  if (weapon == nullptr || !ValidShipSlot(target_slot) || shot.consumed) {
    return false;
  }
  if (!ValidShipSlot(shot.owner_ship_slot) ||
      shot.owner_ship_slot == target_slot) {
    return false;
  }

  const Ship &owner =
      state.ShipAt(static_cast<std::size_t>(shot.owner_ship_slot));
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  if (!target.is_active || target.current_system_id != shot.system_id ||
      IsDestroyed(target)) {
    return false;
  }
  if (owner.current_system_id != shot.system_id || IsDestroyed(owner)) {
    return false;
  }
  if (target.mission_ship_slot == 0x3ff || target.ai_state_code == 0x10) {
    return false;
  }

  // Weapon_CanWeaponHitTarget's mode-1 branch rejects a contact other than the
  // shot's recorded target unless Flags2 bit 0x08 opts into the broader path.
  if (weapon->weapon_mode_code == 1 &&
      (weapon->flags_secondary & 0x0008U) == 0 &&
      shot.target_ship_slot != target_slot) {
    return false;
  }

  // The original avoids collisions between ships pursuing the same target or
  // sharing a stellar target. These fields are represented directly here,
  // unlike the original's packed ShipState offsets.
  if (owner.ai_target_ship_slot >= 0 && target.ai_target_ship_slot >= 0 &&
      owner.ai_target_ship_slot == target.ai_target_ship_slot) {
    return false;
  }
  if (owner.target_stellar_object_id != -1 &&
      owner.target_stellar_object_id == target.target_stellar_object_id) {
    return false;
  }

  if (owner.faction_or_government_id >= 0 &&
      owner.faction_or_government_id == target.faction_or_government_id) {
    return false;
  }
  if (SharesTargetLeaderChain(state, shot.owner_ship_slot, target_slot)) {
    return false;
  }
  if (HasGovernmentFlag(state, target, 0x0008U) ||
      HasGovernmentFlag(state, target, 0x0800U)) {
    return false;
  }

  // Weapon_CanWeaponHitTarget requires the target's planet-type capability
  // bit to agree with Weapon.Flags2 bit 0x400.
  const ShipClass *target_class = state.scenario.Ship(
      static_cast<std::int16_t>(target.ship_class_id + 0x80));
  if (target_class == nullptr ||
      ((target_class->capability_flags ^ weapon->flags_secondary) & 0x0400U) !=
          0) {
    return false;
  }
  return true;
}

void NovaWeapon_ResolveProjectileCollisions(GameState &state) {
  for (ActiveShot &shot : state.active_shots) {
    if (shot.life_ticks_remaining <= 0.0F && shot.life_frames > 0) {
      // Compatibility for records created by older callers/tests that only
      // populated the original integer lifetime view.
      shot.life_ticks_remaining = static_cast<float>(shot.life_frames);
    }
    if (shot.consumed || shot.life_ticks_remaining <= 0.0F) {
      continue;
    }
    if (shot.system_id != state.player.current_system_id) {
      // Shot_HandleShot 0x00435830 rejects shots whose system no longer
      // matches the player before doing any movement or impact work.
      shot.consumed = true;
      continue;
    }
    const Weapon *weapon = WeaponForShot(state, shot);
    if (weapon == nullptr) {
      shot.consumed = true;
      continue;
    }

    std::int16_t best_target = -1;
    bool best_target_used_proximity = false;
    float best_distance_sq = std::numeric_limits<float>::max();
    for (std::int16_t slot = 0;
         slot < static_cast<std::int16_t>(GameState::kMaxShips);
         ++slot) {
      if (!NovaWeapon_CanProjectileHitShip(state, shot, slot)) {
        continue;
      }
      const Ship &target = state.ShipAt(static_cast<std::size_t>(slot));
      const float dx = target.pos_x - shot.pos_x;
      const float dy = target.pos_y - shot.pos_y;
      const float direct_radius =
          std::max(0.0F, target.collision_radius_px) +
          std::max(0.0F, shot.collision_radius_px);
      const float proximity_radius =
          static_cast<float>(std::max(0, static_cast<int>(weapon->blast_radius))) +
          std::max(0.0F, target.collision_radius_px) * 0.5F;
      const float radius = std::max(direct_radius, proximity_radius);
      const float distance_sq = dx * dx + dy * dy;
      if (distance_sq <= radius * radius && distance_sq < best_distance_sq) {
        best_target = slot;
        best_distance_sq = distance_sq;
        best_target_used_proximity = distance_sq > direct_radius * direct_radius;
      }
    }

    if (best_target >= 0) {
      ResolveShipHit(state,
                     shot,
                     state.ShipAt(static_cast<std::size_t>(best_target)),
                     best_target,
                     /*allow_aggro_updates=*/true,
                     /*suppress_retarget_logic=*/!best_target_used_proximity);

      // Shot_ResolveShotCollisionHit (0x00437780): a blast damages every
      // additional active ship in the axis-aligned splash box, excluding the
      // owner unless Flags bit 0x0100 explicitly allows player hurt.
      if (weapon->splash_radius > 0) {
        for (std::int16_t slot = 0;
             slot < static_cast<std::int16_t>(GameState::kMaxShips); ++slot) {
          if (slot == best_target || !ValidShipSlot(slot)) {
            continue;
          }
          // The original's Flags bit 0x0100 carve-out is specifically about
          // the player: NPC owners may receive their own blast damage, while
          // the player's ship is protected unless that bit is set.
          if (slot == shot.owner_ship_slot && shot.owner_ship_slot == 0 &&
              (weapon->flags & 0x0100U) == 0U) {
            continue;
          }
          Ship &splash_target =
              state.ShipAt(static_cast<std::size_t>(slot));
          if (!splash_target.is_active ||
              splash_target.current_system_id != shot.system_id ||
              IsDestroyed(splash_target)) {
            continue;
          }
          const ShipClass *splash_class = state.scenario.Ship(
              static_cast<std::int16_t>(splash_target.ship_class_id + 0x80));
          if (splash_class == nullptr ||
              (splash_class->capability_flags & 0x0400U) != 0U) {
            continue;
          }
          if (std::abs(splash_target.pos_x - shot.pos_x) >
                  static_cast<float>(weapon->splash_radius) ||
              std::abs(splash_target.pos_y - shot.pos_y) >
                  static_cast<float>(weapon->splash_radius)) {
            continue;
          }
          ResolveShipHit(state,
                         shot,
                         splash_target,
                         slot,
                         /*allow_aggro_updates=*/false,
                         /*suppress_retarget_logic=*/false);
        }
      }
      shot.consumed = true;
    }
  }

  state.active_shots.erase(
      std::remove_if(state.active_shots.begin(),
                     state.active_shots.end(),
                     [](const ActiveShot &shot) { return shot.consumed; }),
      state.active_shots.end());
}

} // namespace game
