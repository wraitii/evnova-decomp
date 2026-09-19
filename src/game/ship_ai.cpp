// Clean-room reconstruction of the NPC ship AI decision layer. See ship_ai.hpp
// for the module overview and the Ghidra-address mapping of each helper.
//
// Porting notes:
//  * Ships carry no outfit inventory, so "is inertialess" uses the NPC
//    branch of Outfit_ShipIsInertialess (0x0046df70) via
//    NovaShip_IsInertialess, and "effective stats" are the class base
//    values (the player's outfit/ionization damping is not modelled yet).
//  * The per-mode turn/thrust scale constants (the _DAT_005750xx globals) are
//    decoded from the raw bytes and typed + pre-commented in the Ghidra DB.
//    The movement and combat modes read the real values; still-unused
//    freeflight-anchor constants remain commented-only.
//  * HUD/mission flavor side-effects (overlay messages, extortion prompts,
//    fuel-transfer chatter, carrier-bay launch) are documented no-ops until
//    those systems land; combat-heavy branches that depend on the not-yet-
//    reconstructed disable systems are conservatively gated. NPC weapon-bank
//    selection and point-defense auto-fire are ported; formation-offset
//    mirroring remains TODO(decomp).
//  * "Wander": Behavior 0x01 + state 0/1 makes a ship pick a random adjacent
//    travel stellar and steer toward it -- the visible "make them move around
//    the system" baseline.

#include "ship_ai.hpp"

#include "ship_ai_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "boarding_plunder.hpp"
#include "escort_formation.hpp"
#include "frame_timing.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "log.hpp"
#include "mission.hpp"
#include "nova_math.hpp"
#include "nova_random.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "ship_spawn.hpp"
#include "spaceflight.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "weapon.hpp"

namespace game {

using namespace ship_ai_detail;

// Ghidra 0x004688e0 Ship_IsShipDestroyed (see game_state.hpp); named AI entry
// point for its existing callers.
bool NovaAiShip_IsDestroyed(const Ship &ship) { return IsShipDestroyed(ship); }

// Ghidra 0x00410670 Ship_EnterShipAiState0x02_ClearPrimaryTarget.
void NovaAi_EnterState2ClearPrimaryTarget(Ship &ship, std::uint32_t now_ms) {
  ship.ai_state_code = 2;
  if (ship.ai_station_hold_timer < 0.0F) {
    ship.ai_station_hold_timer = 0.0F;
  }
  ship.primary_target_ship_slot = -1;
  ship.ai_mode_start_time_ms = now_ms;
}

// Ghidra 0x00410dd0 Ship_ResetShipPrimaryAndSecondaryTargets.
void NovaAi_ResetShipPrimaryAndSecondaryTargets(Ship &ship) {
  if (ship.ai_state_code != 9 && ship.ai_state_code != 0xf) {
    ship.ai_state_code = 0;
    ship.ai_control_mode = 0;
  }
  ship.ai_secondary_target_slot = -1;
  ship.ai_station_hold_timer = 0.0F;
  ship.ai_forward_thrust_cmd = 0.0F;
  ship.ai_desired_speed = 0.0F;
}

// Ghidra 0x00410e20. This is the ordinary new-NPC arrival/slowdown entry used
// when no adjacent restricted stellar was selected. The spawn caller supplies
// the position; the original helper owns these state and animation latches.
void NovaAi_EnterState8Slowdown(GameState &state, Ship &ship) {
  ship.ai_state_code = 8;
  ship.ai_station_hold_timer = -999.0F;
  ship.waypoint_arrival_marker_a = 1;
  ship.waypoint_arrival_marker_b = 0;
  ship.turn_bank_animation_phase =
      -static_cast<float>(std::uniform_int_distribution<int>{0, 9}(state.rng));
  ship.weapon_sprite_flash_level = 0.0F;

  // Port-only probe instrumentation (no gameplay effect): the clean-room
  // arrival monitor has no counterpart in the original, which arms it only
  // through its spawn placements. See game_state.hpp:267.
  ship.arrival_monitor_elapsed_ticks = 0.0F;
  ship.arrival_monitor_active = true;
  ship.arrival_monitor_warning_logged = false;
  NovaLog::Info(
      "NPC arrival monitor armed: slot={} class={} behavior={} state={} "
      "control={} speed={:.2f} station_hold={:.2f}",
      ship.ship_instance_id,
      ship.ship_class_id,
      ship.ai_behavior_code,
      ship.ai_state_code,
      ship.ai_control_mode,
      std::hypot(ship.vel_x, ship.vel_y),
      ship.ai_station_hold_timer);
}

// Ghidra 0x004159e0 Ship_EnterShipAiState0x15_EmergeFromHypergate.
void NovaAi_EnterState15EmergeFromHypergate(GameState &state,
                                            Ship &ship,
                                            std::int16_t stellar_id) {
  ship.ai_state_code = 0x15;
  ship.ai_control_mode = 0;
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = stellar_id;
  ship.ai_maneuver_timer_ms = 60.0F;
  ship.ai_station_hold_timer = -1.0F;

  if (stellar_id >= 0 && stellar_id < 0x800) {
    // Ghidra gates on Ship_ComputeShipFuelCapacity(ship) < 1 (0x00463a20): a
    // hull able to carry any fuel at all records the emergence stellar. This
    // is a much lower bar than the jump-sequence cost (kJumpFuelCost) checked
    // by NovaTravel_CanShipInitiateJumpSequence.
    ship.jump_destination_stellar_id =
        NovaAi_ComputeShipFuelCapacity(state, ship) >= 1.0 ? stellar_id : -2;

    const Stellar *stellar = state.scenario.Stellar(stellar_id);
    if (stellar != nullptr) {
      // Ghidra reads StellarDef +0x28 directly. Valid entries are integer
      // degrees [0, 359]; invalid resource values use NovaRandom_Range(360).
      // Scenario loading preserves the signed raw word at sp\x9ab +0x1a.
      const int heading_deg =
          stellar->emergence_angle_deg.has_value() &&
                  *stellar->emergence_angle_deg >= 0 &&
                  *stellar->emergence_angle_deg <= 359
              ? *stellar->emergence_angle_deg
              : std::uniform_int_distribution<int>(0, 359)(state.rng);
      // The original ShipState stores this field in degrees. Clean-room Ship
      // stores heading in radians, so convert only at this boundary.
      ship.heading = static_cast<float>(heading_deg) * kDegToRad;
    }
  }

  ship.ai_desired_speed = ship.squad_leader_ship_slot == 0 ? -15.0F : -30.0F;
  ship.ai_forward_thrust_cmd = -3.0F;
}

bool NovaAi_CompleteNpcJump(GameState &state, Ship &ship) {
  if (ship.ai_state_code != 0x14 || ship.ai_secondary_target_slot < 0) {
    return false;
  }
  const std::int16_t source_system = ship.current_system_id;
  const System *source =
      state.scenario.System(static_cast<std::int16_t>(source_system + 0x80));
  if (source == nullptr) {
    return false;
  }

  std::int16_t destination_resource = -1;
  for (std::size_t slot = 0; slot < source->nav_defs.size(); ++slot) {
    if (source->nav_defs[slot] == ship.ai_secondary_target_slot) {
      destination_resource = source->links[slot];
      break;
    }
  }
  if (destination_resource < 0x80 ||
      destination_resource == source_system + 0x80) {
    return false;
  }
  const std::int16_t destination_system =
      static_cast<std::int16_t>(destination_resource - 0x80);
  const System *destination = state.scenario.System(destination_resource);
  if (destination == nullptr ||
      !NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
    return false;
  }

  ship.fuel_points = std::max(0.0F, ship.fuel_points - kJumpFuelCost);
  ship.current_system_id = destination_system;
  ship.primary_target_ship_slot = -1;
  ship.squad_leader_ship_slot = -1;
  ship.ai_hostility_accumulator = 0;
  ship.defense_fleet_home_stellar_id = -1;
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;
  ship.speed = 0.0F;

  // Prefer the reverse hyperlink's entry stellar. If the scenario has no
  // paired NavDef, the system centre remains a valid conservative fallback.
  std::int16_t entry_stellar = -1;
  for (std::size_t slot = 0; slot < destination->links.size(); ++slot) {
    if (destination->links[slot] == source_system + 0x80 &&
        destination->nav_defs[slot] >= 0x80 &&
        state.scenario.Stellar(destination->nav_defs[slot]) != nullptr) {
      entry_stellar = destination->nav_defs[slot];
      break;
    }
  }
  if (entry_stellar >= 0) {
    const Stellar *stellar = state.scenario.Stellar(entry_stellar);
    ship.pos_x = static_cast<float>(stellar->pos_x);
    ship.pos_y = static_cast<float>(stellar->pos_y);
  } else {
    ship.pos_x = static_cast<float>(destination->pos_x);
    ship.pos_y = static_cast<float>(destination->pos_y);
  }
  NovaAi_EnterState15EmergeFromHypergate(state, ship, entry_stellar);
  return true;
}

// Ghidra 0x004687b0 Ship_IsShipDisabled. True when the ship must not
// fire/act this frame: derelict government (flags_primary 0x800); a mission
// ShipGoal-5 special ship that is still active and unprovoked; or critically
// damaged (armor below 1/3 of max, 1/10 with capability flags 0x10). The
// damage threshold uses the outfit/personality-inclusive
// Ship_ComputeShipMaxArmor (0x004637a0). Non-player ships with a stellar
// target are exempt -- the original returns not-disabled without reaching the
// armor check (0x00468856 XOR AL,AL early-out).
bool NovaAiShip_IsDisabled(const GameState &state, const Ship &ship) {
  if (ship.faction_or_government_id >= 0) {
    // faction_or_government_id is a zero-based government id (indexes
    // g_government_defs directly in the original / scenario.governments here).
    const auto gid = static_cast<std::size_t>(ship.faction_or_government_id);
    if (gid < state.scenario.governments.size() &&
        (state.scenario.governments[gid].flags_primary & 0x800) != 0) {
      // Derelict: never acts.
      return true;
    }
  }
  // Mission-fleet ShipGoal-5 ships sit disabled until provoked: the runtime
  // ship-objective-complete latch (Ghidra MisnRuntimeFlags.special_ship_
  // attacking, +0x02 == the port's objective_complete) must still be clear
  // and the target must not have been boarded.
  if (ship.ship_instance_id > 0 && ship.mission_fleet_slot >= 0 &&
      static_cast<std::size_t>(ship.mission_fleet_slot) <
          state.active_missions.size()) {
    const std::size_t fleet_slot =
        static_cast<std::size_t>(ship.mission_fleet_slot);
    const MissionRuntimeFlags &runtime =
        state.active_mission_runtime_flags[fleet_slot];
    if (runtime.is_active && state.active_missions[fleet_slot].ship_goal == 5 &&
        !runtime.objective_complete && ship.boarded_target_latch == 0) {
      return true;
    }
  }
  // Ships attached to a stellar (landing/jump-out approach) are never
  // disabled, even when crippled; the armor gate below is skipped.
  if (ship.ship_instance_id > 0 && ship.defense_fleet_home_stellar_id != -1) {
    return false;
  }
  // Critically-damaged gate: armor*100.0f below max_armor*33.333 (double
  // 0x575808) or max_armor*10.0 (double 0x5757f8) with capability flags 0x10.
  // This gate is also what suppresses NPC shield/armor regeneration in
  // Ship_HandleShip, so keep it separate from the destruction predicate.
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const double max_armor =
      static_cast<double>(NovaAi_ComputeMaxArmorPoints(state, ship));
  if (max_armor > 0.0) {
    const double threshold =
        (cls != nullptr && (cls->capability_flags & 0x10) != 0U) ? 10.0
                                                                 : 33.333;
    if (static_cast<double>(ship.armor_points * 100.0F) <
        max_armor * threshold) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x00463550 Ship_ComputeShipMaxShieldPoints. Player outfit aggregation
// runs in Outfit_ComputePlayerEffectiveStats. Keep NPC capacity wide for the
// original x87 threshold comparisons; behavior 5 explicitly rounds to float.
double NovaAi_ComputeMaxShieldPoints(const GameState &state, const Ship &ship) {
  if (ship.ship_instance_id == 0) {
    return static_cast<double>(
        Outfit_ComputePlayerEffectiveStats(state).max_shield_points);
  }

  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  double max_shield =
      cls != nullptr ? static_cast<double>(cls->base_shield) : 0.0;
  if (ship.pers_def_slot >= 0 && static_cast<std::size_t>(ship.pers_def_slot) <
                                     state.scenario.pers_defs.size()) {
    const double scale = static_cast<double>(
        state.scenario.pers_defs[static_cast<std::size_t>(ship.pers_def_slot)]
            .shield_armor_scale);
    if (scale > 0.0) {
      max_shield *= scale;
    }
  }
  if (ship.ai_behavior_code == 5) {
    // 0x004635c9..d2 stores the behavior-5 product to float before the
    // caller reloads it; retain that one intermediate rounding step.
    max_shield = static_cast<double>(static_cast<float>(max_shield * 1.333));
  }
  return max_shield;
}

// Ghidra 0x004637a0 Ship_ComputeShipMaxArmor. Player outfit aggregation runs
// in Outfit_ComputePlayerEffectiveStats.
float NovaAi_ComputeMaxArmorPoints(const GameState &state, const Ship &ship) {
  if (ship.ship_instance_id == 0) {
    return Outfit_ComputePlayerEffectiveStats(state).max_armor_points;
  }
  const ShipClass *ship_class = ShipClassFor(state, ship);
  double max_armor =
      ship_class != nullptr ? static_cast<double>(ship_class->base_armor) : 0.0;
  if (ship.pers_def_slot >= 0 && static_cast<std::size_t>(ship.pers_def_slot) <
                                     state.scenario.pers_defs.size()) {
    const double scale = static_cast<double>(
        state.scenario.pers_defs[static_cast<std::size_t>(ship.pers_def_slot)]
            .shield_armor_scale);
    if (scale > 0.0) {
      max_armor *= scale;
    }
  }
  if (ship.ai_behavior_code == 5) {
    max_armor = static_cast<double>(static_cast<float>(max_armor * 1.333));
  }
  return static_cast<float>(max_armor);
}

// Ghidra 0x00463a20 Ship_ComputeShipFuelCapacity. The player's capacity folds
// in opcode-12 outfit bonuses through the effective-stats pass; NPC ships use
// the raw class value because the original skips the outfit loop for
// ship_instance_id != 0.
double NovaAi_ComputeShipFuelCapacity(const GameState &state,
                                      const Ship &ship) {
  if (ship.ship_instance_id == 0) {
    return static_cast<double>(
        Outfit_ComputePlayerEffectiveStats(state).fuel_capacity);
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  return cls != nullptr ? static_cast<double>(cls->base_fuel) : 0.0;
}

namespace {

void BeginCloakTransition(GameState &state, Ship &ship) {
  if (NovaTargeting_ShipAtCloakVisibilityThreshold(ship)) {
    return;
  }
  if (ship.cloak_fade_progress > 0.0F) {
    if (ship.cloak_transition_latch < 0) {
      ship.cloak_transition_latch = 0;
    }
  } else {
    ship.cloak_transition_latch = 1;
  }
  if (ship.shield_points > 0.0F &&
      NovaOutfit_HasCloakShieldDropOnActivation(state, ship)) {
    ship.shield_points = 0.0F;
  }
}

void ClearCloakTransition(Ship &ship) {
  if (!NovaTargeting_ShipAtCloakVisibilityThreshold(ship)) {
    return;
  }
  if (ship.cloak_fade_progress > 0.0F) {
    ship.cloak_transition_latch = -1;
  } else if (ship.cloak_transition_latch > 0) {
    ship.cloak_transition_latch = 0;
  }
}

} // namespace

// Ghidra 0x004680d0 Ship_OnShipCloakStateEntered.
void NovaAi_OnShipCloakStateEntered(GameState &state, Ship &ship) {
  BeginCloakTransition(state, ship);
}

// Ghidra 0x00468190 Ship_OnShipCloakStateCleared.
void NovaAi_OnShipCloakStateCleared(Ship &ship) { ClearCloakTransition(ship); }

// Ghidra 0x00411d00 Ship_UpdateShipCloakStateFromTraits.
void NovaAi_UpdateShipCloakStateFromTraits(GameState &state, Ship &ship) {
  constexpr float kCloakTargetDistance = 165.0F; // DAT_005750b0
  if (!NovaOutfit_HasCloakingDevice(state, ship) ||
      NovaAiShip_IsDisabled(state, ship)) {
    if (ship.cloak_fade_progress > 0.0F) {
      NovaAi_OnShipCloakStateCleared(ship);
    }
    return;
  }
  if (NovaOutfit_GetCloakFuelDrainFlags(state, ship) > 0 &&
      ship.fuel_points <= 0.0F) {
    NovaAi_OnShipCloakStateCleared(ship);
    return;
  }
  if (NovaOutfit_GetCloakShieldDrainFlags(state, ship) > 0 &&
      ship.shield_points <= 0.0F) {
    NovaAi_OnShipCloakStateCleared(ship);
    return;
  }

  const ShipClass *ship_class =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (ship_class == nullptr) {
    NovaAi_OnShipCloakStateCleared(ship);
    return;
  }
  bool should_enter = false;
  const std::uint16_t flags = ship_class->flags_secondary;
  if ((flags & 0x0100U) != 0U) {
    for (std::int16_t bank = 0; bank < 0x100; ++bank) {
      const std::size_t bank_index = static_cast<std::size_t>(bank);
      if (ship.npc_weapon_count_by_class[bank_index] <= 0) {
        continue;
      }
      const Weapon *weapon =
          state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
      if (weapon != nullptr && static_cast<float>(weapon->reload_ticks) <
                                   ship.npc_weapon_bank_cooldown[bank_index]) {
        should_enter = true;
        break;
      }
    }
  }
  if ((flags & 0x0200U) != 0U &&
      (ship.ai_state_code == 3 ||
       (ship.ai_state_code == 0x10 && ship.ai_control_mode == 0x13))) {
    should_enter = true;
  }
  if ((flags & 0x0400U) != 0U &&
      (ship.ai_control_mode == 4 || ship.ai_state_code == 8 ||
       ship.ai_state_code == 0xb)) {
    should_enter = true;
  }
  if ((flags & 0x0800U) != 0U &&
      (ship.ai_state_code == 1 || ship.ai_state_code == 6 ||
       ship.ai_state_code == 0x14 || ship.ai_state_code == 7 ||
       ship.ai_state_code == 10)) {
    should_enter = true;
  }
  if ((flags & 0x1000U) != 0U &&
      NovaTargeting_ShipAtCloakVisibilityThreshold(ship)) {
    if (ship.ai_state_code == 4 && ship.primary_target_ship_slot >= 0 &&
        state.SlotInRange(
            static_cast<std::size_t>(ship.primary_target_ship_slot))) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot));
      should_enter =
          should_enter ||
          std::abs(ship.pos_x - target.pos_x) > kCloakTargetDistance ||
          std::abs(ship.pos_y - target.pos_y) > kCloakTargetDistance;
    }
    if (ship.ai_state_code == 0x10 && ship.ai_control_mode != 0x14) {
      should_enter = true;
    }
  }
  if ((flags & 0x2000U) != 0U && ship.ai_state_code == 0) {
    should_enter = true;
  }
  if (ship.ai_behavior_code > 4 && ship.squad_leader_ship_slot >= 0 &&
      ship.ai_control_mode == 0xc &&
      state.SlotInRange(
          static_cast<std::size_t>(ship.squad_leader_ship_slot))) {
    const Ship &target =
        state.ShipAt(static_cast<std::size_t>(ship.squad_leader_ship_slot));
    should_enter =
        should_enter || NovaTargeting_ShipAtCloakVisibilityThreshold(target);
  }
  if (should_enter) {
    NovaAi_OnShipCloakStateEntered(state, ship);
  } else {
    NovaAi_OnShipCloakStateCleared(ship);
  }
}

// Ghidra 0x00412330 Ship_SelectNearestDisabledShipForBoarding: find the
// nearest boardable ship and lock it in as the primary target with AI state
// 0x0d. Candidate gates, in original order: not self, not one of this ship's
// own carried fighters (squad leader == this ship's instance), active,
// boarded-target latch clear, disabled, not a mission-fleet ship, and one of
// -- is the player, has a non-negative post-hit mode hint, has low AI
// behavior (< 3), its class has low default AI (< 3), or this ship still has
// a fireable non-secondary weapon (Weapon_HasAnyFireableNonSecondaryWeapon
// 0x00415c10, sampled once at entry). Then the candidate class must have
// capture_power > 0 (Bible Crew: 0 crew = cannot be boarded nor capture),
// and candidates whose squad leader is not the player skip allied govts.
// Distance is the truncated squared distance (the original's FISTP +
// sign-correction idiom nets to truncation toward zero).
void NovaAi_SelectNearestDisabledShipForBoarding(GameState &state, Ship &ship) {
  const bool has_fireable_weapon =
      NovaWeapon_HasAnyFireableNonSecondaryWeapon(state, ship);
  std::int16_t best_slot = -1;
  int best_distance = 0;
  for (std::size_t i = 0; i < GameState::kMaxShips; ++i) {
    const std::int16_t slot = static_cast<std::int16_t>(i);
    const Ship &candidate = state.ShipAt(i);
    if (slot == ship.ship_instance_id ||
        ship.ship_instance_id == candidate.squad_leader_ship_slot ||
        !candidate.is_active || candidate.boarded_target_latch != 0 ||
        !NovaAiShip_IsDisabled(state, candidate) ||
        candidate.mission_fleet_slot != -1) {
      continue;
    }
    const ShipClass *candidate_cls = state.scenario.Ship(
        static_cast<std::int16_t>(candidate.ship_class_id + 0x80));
    const bool low_ai =
        candidate.ai_behavior_code < 3 ||
        (candidate_cls != nullptr && candidate_cls->default_ai_behavior < 3);
    if (!(slot == 0 || candidate.post_hit_mode_hint >= 0 || low_ai ||
          has_fireable_weapon)) {
      continue;
    }
    if (candidate_cls == nullptr || candidate_cls->crew <= 0) {
      continue;
    }
    if (candidate.squad_leader_ship_slot != 0 &&
        NovaGovernment_AreGovtsAllied(state.scenario,
                                      ship.faction_or_government_id,
                                      candidate.faction_or_government_id)) {
      continue;
    }
    const int distance = static_cast<int>(SquaredDistance(
        ship.pos_x, ship.pos_y, candidate.pos_x, candidate.pos_y));
    if (best_slot == -1 || distance < best_distance) {
      best_slot = slot;
      best_distance = distance;
    }
  }
  if (best_slot != -1) {
    ship.primary_target_ship_slot = best_slot;
    ship.ai_state_code = 0x0d;
    ship.ai_control_mode = 0;
  }
}

// Ghidra 0x004133F0 Ship_UpdateShipCombatOddsScore.
void NovaAi_UpdateShipCombatOddsScore(GameState &state, Ship &ship) {
  const ShipClass *ship_class = ShipClassFor(state, ship);
  std::int16_t allied_strength =
      ship_class != nullptr ? ship_class->strength : 0;
  std::int16_t hostile_strength = 0;

  if (NovaTargeting_IsShipAcquirableAsTarget(state, state.player, ship)) {
    const ShipClass *player_class = ShipClassFor(state, state.player);
    if (player_class != nullptr) {
      // Base unit pinned (GameState::kCombatRatingBaseStrength); the original
      // divides by class 0's unindexed Strength at 0x0041343c.
      const std::int32_t divisor =
          GameState::kCombatRatingBaseStrength * 0x1900;
      float rating_scale =
          static_cast<float>(state.player_combat_rating_points / divisor);
      rating_scale = std::clamp(rating_scale, 1.0F, 2.0F);
      hostile_strength = static_cast<std::int16_t>(
          static_cast<float>(player_class->strength) * rating_scale);
    }
  }

  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &other = state.ShipAt(slot);
    if (!other.is_active || other.ship_instance_id == ship.ship_instance_id ||
        NovaAiShip_IsDisabled(state, other)) {
      continue;
    }
    const ShipClass *other_class = ShipClassFor(state, other);
    if (other_class == nullptr) {
      continue;
    }
    if (NovaGovernment_AreGovtsAllied(state.scenario,
                                      other.faction_or_government_id,
                                      ship.faction_or_government_id)) {
      allied_strength =
          static_cast<std::int16_t>(allied_strength + other_class->strength);
      continue;
    }
    if (NovaGovernment_AreGovtsHostileOrXenophobic(
            state.scenario,
            other.faction_or_government_id,
            ship.faction_or_government_id) ||
        (other.primary_target_ship_slot == ship.ship_instance_id &&
         other.ai_state_code == 4)) {
      hostile_strength =
          static_cast<std::int16_t>(hostile_strength + other_class->strength);
    }
  }

  if (allied_strength == 0) {
    allied_strength = 1;
  }
  ship.ai_odds_score = static_cast<float>(hostile_strength) /
                       static_cast<float>(allied_strength);
}

// Ghidra 0x004152E0 Ship_IssueEscortOrders.
// Commands are Formation=0, Defend=1, Attack=2, and Return=3.
void NovaAi_IssueEscortOrders(GameState &state, Ship &ship) {
  // Deliberate, ungated divergence from the original range probes. 0x004152E0
  // calls Weapon_IsShipWithinWeaponRangeOfTarget (0x00411600) with the literal
  // weapon slot 1 for BOTH directions, which reads global wëap resource 0x81
  // ("Medium Blaster", reach 350; the helper adds 32 for non-beam modes ->
  // 382 px) no matter what either ship carries. The two probes are therefore
  // symmetric and the intended outranging check is dead; the threshold is an
  // undocumented artifact of the compiled index. The port substitutes each
  // side's own stock-armed reach for categories 1..3 and a fixed 382 px
  // reference for category-0 fighters. Because the whole reconstruction is a
  // divergence it is not routed through kApplyOriginalBugFixes; that policy no
  // longer restores the literal index.
  //
  // Fixed category-0 fighter reference radius: the shipped wëap 0x81 envelope
  // (350 + 32). Pinned rather than read back from the weapon table so a mod
  // editing the unrelated wëap 0x81 cannot move fighter escort orders.
  constexpr int kEscortFighterDecisionRangePx = 382;
  std::array<std::int16_t, 4> commands{3, 3, 3, 3};
  bool target_disabled = false;
  bool target_can_hit_leader = false;
  bool leader_can_hit_target = false;
  bool target_locked = false;
  bool target_beyond_fighter_decision_range = false;

  if (ship.ai_behavior_code < 3) {
    const long double max_shield =
        static_cast<long double>(NovaAi_ComputeMaxShieldPoints(state, ship));
    // DOUBLE_00575168 is 0.66. Keep the binary64 literal and the original
    // x87 comparison width at the boundary.
    if (static_cast<long double>(ship.shield_points) < max_shield * 0.66) {
      commands[0] = 1;
    } else {
      commands[0] = 2;
    }
    commands[1] = 1;
    commands[2] = 1;
    commands[3] = 0;
  } else {
    if (ship.primary_target_ship_slot >= 0 &&
        state.SlotInRange(
            static_cast<std::size_t>(ship.primary_target_ship_slot))) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot));
      target_locked = NovaAiShip_IsShipLockedOnAttackerInState4(target, ship);
      if (target_locked) {
        // Divergence (see the function comment): read each side's own
        // stock-armed reach instead of the original's literal slot 1. The scan
        // ignores current ammo, matching the original helper's stock-weapon
        // (weapon_slot == -1) arm. Feeds categories 1..3 only; category 0 uses
        // the fixed reference below.
        target_can_hit_leader = NovaWeapon_ShipWithinWeaponRangeOfTarget(
            state, target, ship, /*weapon_slot=*/-1);
        leader_can_hit_target = NovaWeapon_ShipWithinWeaponRangeOfTarget(
            state, ship, target, /*weapon_slot=*/-1);
        // Replicate the 0x00411600 envelope's floor(|dx|)^2 + floor(|dy|)^2
        // comparison against the fixed reference reach.
        const int dx = static_cast<int>(std::abs(ship.pos_x - target.pos_x));
        const int dy = static_cast<int>(std::abs(ship.pos_y - target.pos_y));
        target_beyond_fighter_decision_range =
            dx * dx + dy * dy >
            kEscortFighterDecisionRangePx * kEscortFighterDecisionRangePx;
      }
      target_disabled = NovaAiShip_IsDisabled(state, target);
    }

    if (target_disabled && ship.ai_state_code == 0xd) {
      commands = {3, 3, 3, 0};
    } else {
      const long double max_shield =
          static_cast<long double>(NovaAi_ComputeMaxShieldPoints(state, ship));
      if (static_cast<long double>(ship.shield_points) < max_shield * 0.33) {
        if (!target_disabled) {
          if (!target_can_hit_leader || leader_can_hit_target) {
            commands[0] = 1;
            commands[1] = 1;
          } else {
            commands[0] = 2;
            commands[1] = 2;
          }
        } else {
          commands[0] = 2;
          commands[1] = 2;
        }
        commands[2] = 1;
      } else if (static_cast<long double>(ship.shield_points) <
                 max_shield * 0.66) {
        if (!target_disabled) {
          commands[0] =
              (!target_can_hit_leader || leader_can_hit_target) ? 1 : 2;
        } else {
          commands[0] = 2;
        }
        commands[1] = 2;
        commands[2] = 1;
      } else {
        commands[0] = (!target_disabled && leader_can_hit_target) ? 1 : 2;
        commands[1] = 2;
        commands[2] =
            (ship.ai_odds_score >= 0.0F && ship.ai_odds_score < 0.5F) ? 2 : 0;
      }
      commands[3] = 0;
      // Divergence: category-0 fighters use the fixed 382 px reference instead
      // of the tier result. They hold station only while the target is engaging
      // the leader from inside that radius; otherwise they attack (a target not
      // engaging us, a disabled target, or a locked target beyond the radius).
      // This is the original high-shield arm (!disabled && R) ? Defend :
      // Attack, with R = locked && within 382, applied at every shield fraction
      // -- the original ordered Defend at all fractions below 0.66. It keeps a
      // long-range carrier's fighters attacking at range where the per-class
      // stock-weapon scan above would have them Defend. Categories 1..3 keep
      // the tier logic.
      commands[0] = (target_locked && !target_disabled &&
                     !target_beyond_fighter_decision_range)
                        ? 1
                        : 2;
    }
  }

  // State/control overrides are applied after the health/target tiers.
  if (ship.ai_state_code != 3 && ship.ai_state_code != 4 &&
      ship.ai_state_code != 0xd && ship.ai_state_code != 0x13) {
    commands = {3, 3, 3, 3};
  }
  if (ship.ai_state_code == 3) {
    if (ship.ai_control_mode == 4 || ship.ai_control_mode == 1) {
      commands = {3, 3, 3, 3};
    } else {
      commands[0] = 1;
      commands[1] = 1;
    }
  }
  if (ship.ai_state_code == 2) {
    commands = {3, 3, 3, 3};
  }

  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    if (static_cast<std::int16_t>(slot) == ship.ship_instance_id) {
      continue;
    }
    Ship &escort = state.ShipAt(slot);
    if (escort.squad_leader_ship_slot != ship.ship_instance_id ||
        escort.ai_behavior_code <= 4) {
      continue;
    }
    const ShipClass *escort_class = state.scenario.Ship(
        static_cast<std::int16_t>(escort.ship_class_id + 0x80));
    if (escort_class == nullptr || escort_class->class_category < 0 ||
        escort_class->class_category >= 4) {
      continue;
    }
    escort.escort_command_code =
        commands[static_cast<std::size_t>(escort_class->class_category)];
    escort.escort_command_pending = 1;
  }
}

// ---- Ghidra 0x00401000 Ship_UpdateShipAI : the top-level dispatcher. ----
// Applies the global "heavy AI" cadence gating/skips, recomputes the effective
// movement stats cached on the ship when its class is the 0x2ff sentinel
// (player-side; not relevant to NPCs), dispatches to the behavior supervisor
// selected by ship.ai_behavior_code, then runs the state machine and applies
// the controls. `skip_heavy_ai` forces the reduced (non-heavy) path.
void NovaAi_UpdateShipAI(GameState &state,
                         Ship &ship,
                         bool skip_heavy_ai,
                         std::uint32_t now_ms,
                         float elapsed_ticks) {
  // Rare "defunct / retired" global abort (DAT_00596d3d set): skip AI.
  // (Kept as a structural no-op; the latch is not modelled.)

  // Fire-restricted ships skip the heavy behavior selection this frame.
  const bool restricted = NovaAiShip_IsDisabled(state, ship);
  // Ship_CanShipInterceptCurrentPrimaryTarget reads the ship-local weapon
  // rows before the post-state auto-selector runs. Seed those rows once from
  // the assigned class so the intercept walk sees NPC guided banks too.
  NovaWeapon_EnsureNpcWeaponBanks(state, ship);
  // Ghidra calls the cloak-trait producer before the state supervisor, but
  // skips it during the coast-through-reversal interval (+0x4c > 0).
  if (ship.ai_maneuver_timer_ms <= 0.0F) {
    NovaAi_UpdateShipCloakStateFromTraits(state, ship);
  }

  // Ship_UpdateShipAI (0x00401000, call site 0x0040119e): a ship resolved as
  // someone's formation leader refreshes its followers' wedge offsets every
  // frame (smooth mode; the system-entry rebuild uses the snap variant). The
  // player's equivalent pass runs in the player core (0x00451003).
  if (ship.ai_selected_as_resolved_target) {
    Ship_UpdateEscortFormations(state, ship, /*snap=*/false);
  }

  // Ghidra 0x00401000 at 0x004011ca: mission-fleet jump-in placement does
  // not call NovaAi_EnterState8Slowdown. It writes -999 to the station-hold
  // timer instead, and this per-frame sentinel gate promotes the ship into
  // the ordinary arrival slowdown before behavior dispatch. The threshold is
  // FLOAT_00575004 (-900.0f), not the general negative-timer test.
  const bool arrival_slowdown_sentinel = ship.ai_station_hold_timer < -900.0F;
  if (arrival_slowdown_sentinel) {
    ship.ai_state_code = 8;
    ship.ai_control_mode = 10;
  } else if (ship.ai_state_code == 8) {
    // State 8 is valid only while its arrival sentinel is present. The
    // original clears an orphaned state 8 before cadence/behavior dispatch;
    // this is also the normal state observed immediately after the movement
    // integrator completes the negative-speed slowdown reset.
    ship.ai_state_code = 0;
  }

  // Ghidra 0x00401000 slice (disasm 0x004012c0..0x00401340): while parked in
  // the formation control modes, state 0x0B (squad jump hold) exits when its
  // reason disappears -- the player leader went disabled, or an NPC leader
  // left the hold (timer <= 1.0) into combat (state 4). Same reset writes as
  // the disabled path.
  const std::int16_t entry_control_mode = ship.ai_control_mode;
  if (ship.ai_control_mode == 4 || ship.ai_control_mode == 0xd) {
    auto exit_jump_hold = [&]() {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      ship.primary_target_ship_slot = -1;
      ship.ai_secondary_target_slot = -1;
      ship.ai_station_hold_timer = -1.0F;
    };
    if (ship.ai_state_code == 0xb && ship.squad_leader_ship_slot == 0 &&
        NovaAiShip_IsDisabled(state, state.player)) {
      exit_jump_hold();
    }
    if (ship.ai_state_code == 0xb && ship.squad_leader_ship_slot > 0 &&
        ship.ai_station_hold_timer <= 1.0F) {
      const Ship &leader =
          state.ShipAt(static_cast<std::size_t>(ship.squad_leader_ship_slot));
      if (!NovaAiShip_IsDisabled(state, leader) && leader.ai_state_code == 4) {
        exit_jump_hold();
      }
    }
  }

  // Ghidra 0x00401000 calls Ship_IssueEscortOrders only after the arrival
  // sentinel and entry/hold control-mode bypasses, and before the disabled
  // auto-guard clears the leader's transient state. The frame phase is the
  // original signed remainder of the signed 16-bit global counter.
  const int frame_phase = static_cast<int>(state.spaceflight_frame_counter) % 8;
  const int leader_phase = ship.ship_instance_id >> 3;
  if (!arrival_slowdown_sentinel && entry_control_mode != 4 &&
      entry_control_mode != 0xd && ship.is_any_ships_squad_leader &&
      frame_phase == leader_phase) {
    NovaAi_IssueEscortOrders(state, ship);
  }

  // Auto-guard: a disabled ship ignores the whole AI selection and just
  // holds its current state/controls; mirrors the original clearing the target
  // slots and control to 0 first.
  if (restricted && !arrival_slowdown_sentinel) {
    ship.squad_leader_ship_slot = -1;
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = -1;
    ship.ai_hostility_accumulator = 0;
    ship.ai_state_code = 0;
    ship.ai_control_mode = 0;
    // TODO(decomp): the original also clears defense_fleet_home_stellar_id
    // here (Ship_UpdateShipAI 0x00401000); the port leaves it set. Investigate
    // whether dropping it is a deliberate divergence or a gap.
    // Fall through to the state machine so it (re)parks the ship.
  }

  // Half of the heavy cadence: the original skips the heavy AI block for some
  // ships each frame (g_render_quality_setting / skill-variance staggering).
  // We preserve the cadence structure: heavy AI runs unless skip_heavy_ai or
  // a per-ship skill-variance gate says otherwise.
  bool run_heavy = !skip_heavy_ai;
  if (run_heavy) {
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    if (cls && (cls->sprite_behavior_flags & 2) != 0) {
      // Ships with fold/unfold animation frames (Flags 2) can afford the
      // heavy decision every frame; basic ones are rate-limited by the
      // animation cycle length (provisional cadence, from +0xA00).
      const std::int16_t variance = cls->animation_cycle_count;
      if (variance > 0 || ship.ship_instance_id % 3 == 0) {
        // heavy path allowed
      } else {
        run_heavy = false;
      }
    }
  }

  // The original sentinel arm bypasses the entire behavior/disabled branch,
  // then runs only the state and control tail. State 8 restores -999 each
  // frame, keeping behavior supervisors from stealing the arrival until the
  // negative-speed integrator completes it.
  if (run_heavy && !restricted && !arrival_slowdown_sentinel) {
    // Dispatch precedence (Ship_UpdateShipAI 0x00401000): a ship holding a
    // stellar assignment runs Ship_DefenseFleetPrioritizePlayerThreat and
    // skips the behavior supervisors entirely. Otherwise availability-driven
    // hulls (class Flags3 bit 0x1/0x2) with no squad leader run the
    // mining/destroyer supervisor, and everything else dispatches on
    // ai_behavior_code.
    const std::int16_t behavior = ship.ai_behavior_code;
    const ShipClass *dispatch_class = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    const bool availability_hull =
        dispatch_class != nullptr &&
        (dispatch_class->availability_flags & 3U) != 0U;
    bool mission_stellar_attack = false;
    if (ship.mission_fleet_slot >= 0 &&
        static_cast<std::size_t>(ship.mission_fleet_slot) <
            state.active_missions.size()) {
      const std::size_t mission_slot =
          static_cast<std::size_t>(ship.mission_fleet_slot);
      mission_stellar_attack =
          state.active_mission_runtime_flags[mission_slot].is_active &&
          state.active_missions[mission_slot].ship_behavior == 2;
    }
    if (mission_stellar_attack) {
      Mission_UpdateShipMissionStellarAttackDirective(state, ship, now_ms);
    } else if (ship.defense_fleet_home_stellar_id != -1) {
      // Ghidra 0x00405120 Ship_DefenseFleetPrioritizePlayerThreat.
      NovaAi_DefenseFleetPrioritizePlayerThreat(state, ship);
    } else if (availability_hull && ship.squad_leader_ship_slot == -1) {
      // Ghidra 0x00402980 Ship_UpdateShipAiAvailabilityBehavior.
      NovaAi_UpdateAvailabilityBehavior(state, ship, now_ms);
    } else if (behavior == 1) {
      NovaAi_UpdateBehavior0x01(state, ship, now_ms);
    } else if (behavior == 2) {
      NovaAi_UpdateBehavior0x02(state, ship, now_ms);
    } else if (behavior == 3) {
      // Ship_UpdateShipAI (0x00401000) dispatch: hostile behavior 0x03 has a
      // plunder/capture variant selected by the faction's government
      // flags_primary 0x1000 (Bible: "warships will plunder non-mission,
      // trader-type enemies").
      bool capture_variant = false;
      if (ship.faction_or_government_id != -1) {
        // Ship.faction_or_government_id is a zero-based def index (the
        // original reads g_government_defs[faction] directly); the 0x80-based
        // Government() expects a resource id; this field is already a
        // zero-based definition index, so use GovernmentByIndex directly.
        if (const Government *govt =
                state.scenario.GovernmentByIndex(ship.faction_or_government_id);
            govt != nullptr && (govt->flags_primary & 0x1000U) != 0U) {
          capture_variant = true;
        }
      }
      if (capture_variant) {
        NovaAi_UpdateBehavior0x03CaptureVariant(state, ship, now_ms);
      } else {
        // Ship_UpdateShipAiBehavior0x03_Warship (0x00402e50) has the hostile
        // target acquisition/travel fallback.
        NovaAi_UpdateBehavior0x03(state, ship, now_ms);
      }
    } else if (behavior == 4) {
      // Ship_UpdateShipAiBehavior0x04_Interceptor (0x00403de0): reuse the
      // reconstructed hostile path so an existing target is also promoted into
      // state 4.
      NovaAi_UpdateBehavior0x03(state, ship, now_ms);
    } else if (behavior > 4) {
      // Ship_UpdateEscortAI (0x004048a0), now ported as
      // NovaAi_UpdateEscortAI (replaces the former force-state-10
      // divergence glue; see that function for the deferred slices). The
      // State 9/0xf exclusions live inside the supervisor, matching the
      // original. A valid state-8 arrival never reaches this branch because
      // its -999 station-hold sentinel takes the dispatcher arm above.
      NovaAi_UpdateEscortAI(state, ship, now_ms);
    }
  }

  // Always run the state machine + controls (the original does so after the
  // heavy block even when bVar6 skipped the heavy decision). elapsed_ticks is
  // the normalized cadence published by the original's misleadingly named
  // _g_avg_frame_time_ms EMA (about 1.0 at 30 Hz).
  NovaAi_UpdateShipState(state, ship, now_ms, elapsed_ticks);
  NovaAi_ApplyControls(state, ship, elapsed_ticks, now_ms);
  // TODO(decomp(0x00408150)): the original calls
  // Ship_EscortFireAtUnprovokedTarget only inside Ship_ApplyShipAiControls, at
  // the tail of control modes 0/1/9/0xb/0xc. This post-state call substitutes
  // for the missing mode-0 call and runs for every control mode. The correct
  // shape is to call it in ApplyControls' case 0 (split from 0x17) and remove
  // this call; see docs/npc_ship_behaviour.md and the case-0 comment.
  if (ship.ai_state_code != 0x12) {
    NovaAi_EscortFireAtUnprovokedTarget(state, ship);
  }
}

// ---------------------------------------------------------------------------

// Ghidra 0x004053c0 Mission_UpdateShipMissionStellarAttackDirective.
void Mission_UpdateShipMissionStellarAttackDirective(GameState &state,
                                                     Ship &ship,
                                                     std::uint32_t now_ms) {
  if (NovaAiShip_IsDisabled(state, ship) || NovaAiShip_IsDestroyed(ship) ||
      ship.ai_state_code == 0x16) {
    return;
  }

  std::int16_t target_stellar = -1;
  const System *system =
      ship.current_system_id >= 0 &&
              static_cast<std::size_t>(ship.current_system_id) <
                  state.scenario.systems.size()
          ? &state.scenario
                 .systems[static_cast<std::size_t>(ship.current_system_id)]
          : nullptr;
  if (system != nullptr) {
    for (const std::int16_t resource_id : system->nav_defs) {
      const Stellar *stellar = state.scenario.Stellar(resource_id);
      if (stellar == nullptr || NovaTargeting_IsStellarActive(*stellar) ||
          stellar->strength_capacity <= 0 ||
          !NovaGovernment_AreGovtsHostileOrXenophobic(
              state.scenario,
              ship.faction_or_government_id,
              stellar->government_id)) {
        continue;
      }
      target_stellar = resource_id;
      break;
    }
  }

  std::int16_t stellar_weapon = -1;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (weapon == nullptr || (weapon->flags_secondary & 0x400U) == 0U ||
        !WeaponBankCanFire(state, ship, bank) ||
        weapon->weapon_mode_code == 0 || weapon->weapon_mode_code == 3 ||
        weapon->weapon_mode_code >= 8) {
      continue;
    }
    stellar_weapon = bank;
    break;
  }

  if (target_stellar == -1 || stellar_weapon == -1) {
    if (ship.ai_state_code == 0x12) {
      ship.primary_target_ship_slot = -1;
      ship.ai_secondary_target_slot = -1;
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
    }
    NovaAi_UpdateBehavior0x03(state, ship, now_ms);
    return;
  }

  ship.ai_secondary_target_slot = target_stellar;
  ship.primary_target_ship_slot = target_stellar;
  ship.ai_state_code = 0x12;
}

// ---------------------------------------------------------------------------
// Ship-comm / hail predicates and AI state entries. See ship_ai.hpp for the
// per-function Ghidra addresses and semantics.
// ---------------------------------------------------------------------------

namespace {

// The disengage/retreat AI state set shared by the keep-pressing predicate and
// the comm-aid scan (each uses its own literal set in the original; 0x12 = 18
// is only excluded by Ship_ShouldShipKeepPressingTarget).
[[nodiscard]] bool IsDisengageState(std::int16_t state, bool include_state18) {
  switch (state) {
  case 7:
  case 9:
  case 15:
  case 10:
  case 11:
  case 5:
  case 12:
    return true;
  case 18:
    return include_state18;
  default:
    return false;
  }
}

} // namespace

// Ghidra 0x004048a0 Ship_UpdateEscortAI. Per-frame supervisor
// for behavior > 4 ships (carried fighters = 5, escorts = 6): keeps the squad
// attached to its leader, decodes the escort command (player group command /
// sub-leader mirror), and arms the leader-jump-prep sync that enters AI state
// 0x0B. Deferred slices are marked TODO(decomp) inline.
void NovaAi_UpdateEscortAI(GameState &state, Ship &ship, std::uint32_t now_ms) {
  (void)now_ms;
  if (ship.ai_state_code == 0x16) {
    return; // destroyed: the destruction package owns this ship
  }
  const std::int16_t leader_slot = ship.squad_leader_ship_slot;
  if (leader_slot != 0 &&
      (ship.ai_state_code == 9 || ship.ai_state_code == 0xf)) {
    return; // combat/capture states are handled elsewhere (non-player leaders)
  }

  // Release-with-default-behavior, shared by the no-leader and
  // leader-lost exits.
  auto release_to_default = [&]() {
    ship.squad_leader_ship_slot = -1;
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    if (cls != nullptr) {
      ship.ai_behavior_code = cls->default_ai_behavior;
    }
    ship.ai_state_code = 0;
    ship.ai_control_mode = 0;
    ship.ai_station_hold_timer = -1.0F;
  };
  if (leader_slot == -1) {
    release_to_default();
    return;
  }

  const Ship *leader =
      state.SlotInRange(static_cast<std::size_t>(leader_slot))
          ? &state.ShipAt(static_cast<std::size_t>(leader_slot))
          : nullptr;
  if (leader == nullptr || !leader->is_active ||
      NovaAiShip_IsDestroyed(*leader)) {
    release_to_default();
    return;
  }

  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls == nullptr) {
    return;
  }

  if (leader_slot == 0) {
    // Player-leader bookkeeping: faction ownership resets for non-mission
    // escorts, and the voice roll only runs for unassigned voice modes.
    // TODO(decomp): the original govt voice override (g_government_defs[].
    // voice_type_mode) is skipped; the chatter consumer is not reconstructed.
    if (ship.mission_fleet_slot == -1) {
      ship.faction_or_government_id = -1;
    }
    if (ship.voice_type_mode != 0 && ship.voice_type_mode != 1) {
      ship.voice_type_mode = RandomBelow(state, 2);
    }
  }

  // Leader-jump-prep arm (disasm 0x00404a91..0x00404b00): an NPC leader in a
  // follow mode (4 / 0xd) or jump prep (state 2 + mode 1) arms it, and the
  // leader's station-hold timer being positive arms it for ANY leader row --
  // including the player (slot 0) during the jump-engage hold, whose timer is
  // seeded to 2.0 at hold-begin. The escort mirrors the leader's travel
  // destination and disengages into state 0x0B (hold formation until the
  // leader's jump fires).
  const bool leader_npc_follow_mode =
      leader_slot > 0 &&
      (leader->ai_control_mode == 4 || leader->ai_control_mode == 0xd);
  const bool leader_npc_jump_prep = leader_slot > 0 &&
                                    leader->ai_state_code == 2 &&
                                    leader->ai_control_mode == 1;
  if (leader_npc_follow_mode || leader_npc_jump_prep ||
      leader->ai_station_hold_timer > 0.0F) {
    ship.ai_secondary_target_slot = leader->ai_secondary_target_slot;
    ship.primary_target_ship_slot = -1;
    if (leader_slot != 0 && NovaShip_IsInertialess(ship, *cls)) {
      // Inertialess ships detach from NPC leaders and travel on their own
      // (state 2 + mode 4 toward the mirrored destination) instead of being
      // adopted at arrival.
      ship.squad_leader_ship_slot = -1;
      ship.ai_behavior_code = cls->default_ai_behavior;
      ship.ai_state_code = 2;
      ship.ai_control_mode = 4;
      ship.ai_station_hold_timer = 0.0F;
      return;
    }
    ship.ai_state_code = 0xb;
  }

  // Primary-target validation.
  if (ship.primary_target_ship_slot != -1) {
    const std::size_t target_slot =
        static_cast<std::size_t>(ship.primary_target_ship_slot);
    if (!state.SlotInRange(target_slot)) {
      ship.primary_target_ship_slot = -1;
    } else {
      const Ship &target = state.ShipAt(target_slot);
      if (!target.is_active || (NovaAiShip_IsDisabled(state, target) &&
                                ship.escort_command_code != 2)) {
        ship.primary_target_ship_slot = -1;
      }
    }
  }

  // Escort command decode.
  if (leader_slot == 0) {
    // The player's per-category group command (g_target_category_command).
    // The category table stays all -1 while the player-core writer (0x00450d88)
    // is unported; -1 coerces to the formation default, matching the original
    // startup state.
    const std::size_t category =
        cls->class_category >= 0 && cls->class_category < 4
            ? static_cast<std::size_t>(cls->class_category)
            : 0;
    ship.escort_command_code = state.target_category_command[category];
    if (ship.escort_command_code == -1) {
      ship.escort_command_code = 0;
    }
    if (ship.escort_command_code == 3) {
      if (ship.ai_behavior_code == 5) {
        if (ship.escort_command_pending == 0) {
          ship.escort_command_code = 0;
        }
      } else {
        // Non-carried ships cannot return to a hangar.
        ship.escort_command_code = 0;
      }
    }
  } else if (state.ShipAt(static_cast<std::size_t>(leader_slot))
                 .squad_leader_ship_slot == 0) {
    // Sub-leader of a player group: mirror the sub-leader's command; a
    // sub-leader in state 10 (following) promotes the wingman to return-to-
    // hangar.
    const Ship &sub_leader =
        state.ShipAt(static_cast<std::size_t>(leader_slot));
    if (sub_leader.escort_command_code == 1 ||
        sub_leader.escort_command_code == 2) {
      ship.escort_command_code =
          sub_leader.ai_state_code == 10 ? 3 : sub_leader.escort_command_code;
    } else {
      ship.escort_command_code = 3;
    }
  }

  // Ammo-readiness downgrade for attack commands on secondary-armed classes.
  if ((ship.escort_command_code == 1 || ship.escort_command_code == 2) &&
      (cls->flags_secondary & 0x80U) != 0U) {
    const int readiness = NovaWeapon_ClassifyAmmoReadiness(state, ship);
    if (readiness != 0) {
      if (ship.ai_behavior_code == 5) {
        ship.escort_command_code = 3;
      } else if (readiness == 2) {
        ship.escort_command_code = 0;
      }
    }
  }

  // Chatter gate shared by the attack commands: only player-attached ships
  // with a fresh assist target, low class category, and a non-mute class
  // roll 1-in-3. The pending-latch sentinel test follows the original's
  // g_pending_combat_chatter_kind == -1; NovaFrame_UpdateCombatChatter resets
  // the latch after consuming the queued request, so this
  // stays silent until that lands.
  auto queue_attack_chatter = [&]() {
    if (ship.squad_leader_ship_slot != 0 ||
        ship.primary_target_ship_slot == -1 ||
        state.pending_combat_chatter_kind != -1 ||
        state.pending_combat_chatter_variant != 0 || cls->class_category >= 2 ||
        (cls->flags_secondary & 0x10U) != 0U || RandomBelow(state, 3) != 0) {
      return;
    }
    NovaFrame_QueueCombatChatter(
        state, 1, cls->inherent_attributes_govt, ship.voice_type_mode);
  };

  if (ship.ai_state_code == 0xb) {
    // State-0x0B maintenance: the hold state owns the ship until the leader's
    // jump fires (or the leader exits, handled by the 0x00401000 exits).
    ship.primary_target_ship_slot = -1;
    if (leader_slot != 0 && NovaShip_IsInertialess(ship, *cls)) {
      ship.squad_leader_ship_slot = -1;
      ship.ai_behavior_code = cls->default_ai_behavior;
      ship.ai_state_code = 2;
      ship.ai_control_mode = 4;
      ship.ai_station_hold_timer = 0.0F;
      return;
    }
    return;
  }

  switch (ship.escort_command_code) {
  case 1: { // attack the leader's attacker (assist)
    ship.ai_station_hold_timer = -1.0F;
    ship.ai_maneuver_timer_ms = -1.0F;
    if (ship.primary_target_ship_slot != -1) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot));
      const float dx = target.pos_x - leader->pos_x;
      const float dy = target.pos_y - leader->pos_y;
      // Ghidra _DAT_00575058 = 408375.0 px^2 (~639 px): drop targets too far
      // from the squad leader.
      if (dx * dx + dy * dy > 408375.0F) {
        ship.primary_target_ship_slot = -1;
      } else {
        ship.ai_state_code = 4;
      }
    }
    if (ship.primary_target_ship_slot == -1) {
      ship.primary_target_ship_slot =
          NovaAi_FindBestAssistTargetForShip(state, ship, 0x226);
      queue_attack_chatter();
    }
    if (ship.primary_target_ship_slot == -1) {
      ship.ai_state_code = 10;
      ship.ai_secondary_target_slot = ship.squad_leader_ship_slot;
    } else {
      ship.ai_state_code = 4;
    }
    break;
  }
  case 2: { // attack the player's target
    ship.ai_station_hold_timer = -1.0F;
    ship.ai_maneuver_timer_ms = -1.0F;
    if (ship.primary_target_ship_slot == -1) {
      ship.primary_target_ship_slot =
          NovaAi_FindBestAssistTargetForShip(state, ship, -1);
      ship.ai_secondary_target_slot = -1;
      if (ship.primary_target_ship_slot == -1) {
        ship.ai_state_code = 10;
        ship.ai_secondary_target_slot = ship.squad_leader_ship_slot;
      } else {
        ship.ai_state_code = 4;
      }
      queue_attack_chatter();
    }
    if (ship.primary_target_ship_slot == -1) {
      ship.ai_state_code = 10;
      ship.ai_secondary_target_slot = ship.squad_leader_ship_slot;
    } else {
      ship.ai_state_code = 4;
    }
    break;
  }
  case 4: // cease fire
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = -1;
    ship.ai_state_code = 6;
    NovaAi_SelectWeaponBankForCurrentTarget(state, ship);
    break;
  case 3: // return to hangar (carried fighters only; others coerced to 0)
    if (ship.ai_behavior_code == 5) {
      ship.ai_state_code = 5;
      ship.primary_target_ship_slot = -1;
      ship.ai_secondary_target_slot = ship.squad_leader_ship_slot;
      break;
    }
    [[fallthrough]];
  default: { // formation (command 0 and any undecoded command)
    ship.ai_station_hold_timer = -1.0F;
    ship.ai_maneuver_timer_ms = -1.0F;
    if (ship.primary_target_ship_slot != -1 &&
        !NovaWeapon_ShipWithinWeaponRangeOfTarget(
            state,
            ship,
            state.ShipAt(
                static_cast<std::size_t>(ship.primary_target_ship_slot)),
            -1)) {
      ship.primary_target_ship_slot = -1;
    }
    if (ship.primary_target_ship_slot == -1) {
      // Candidate picker: the enter-state-4 call's target roll is reused for
      // its side effect; the state is overridden to 10 right after.
      NovaAi_EnterState4TargetRandomRelativeToSquadLeader(state, ship);
    }
    ship.ai_state_code = 10;
    ship.ai_secondary_target_slot = ship.squad_leader_ship_slot;
    if (ship.primary_target_ship_slot == ship.squad_leader_ship_slot) {
      ship.primary_target_ship_slot = -1;
    }
    if (ship.primary_target_ship_slot != -1 && leader_slot != 0) {
      NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(state,
                                                        ship,
                                                        /*allow_guided=*/false);
      NovaAi_SelectWeaponBankForCurrentTarget(state, ship);
    }
    break;
  }
  }
}

// Ghidra 0x00464a90 Ship_CanShipEngageTargetUnderCloakRules.
bool NovaAiShip_CanEngageTargetUnderCloakRules(const GameState &state,
                                               const Ship &subject_ship,
                                               const Ship &other_ship) {
  if (subject_ship.ai_state_code == 0x15) {
    return false; // jump/travel state: no engagement
  }
  if (other_ship.pers_def_slot == 0x3ff) {
    return true;
  }
  // The first argument is the subject whose cloak visibility is tested.
  if (!NovaTargeting_ShipAtCloakVisibilityThreshold(subject_ship)) {
    return true;
  }
  // A ship tracking this subject and carrying a cloaking device can still be
  // engaged, even though the subject is hidden.
  if (other_ship.squad_leader_ship_slot == subject_ship.ship_instance_id &&
      NovaOutfit_HasCloakingDevice(state, other_ship)) {
    return true;
  }
  constexpr float kCloakEngagementCloseRange = 200.0F; // DAT_005757c0
  if (other_ship.cloak_scanner_reveal_screen == 1 &&
      std::abs(other_ship.pos_x - subject_ship.pos_x) <=
          kCloakEngagementCloseRange &&
      std::abs(other_ship.pos_y - subject_ship.pos_y) <=
          kCloakEngagementCloseRange) {
    return true;
  }
  return false;
}

// Ghidra 0x00467e80 Ship_CanMaintainCloakState.
bool NovaAiShip_CanMaintainCloakState(const GameState &state,
                                      const Ship &ship) {
  if (NovaAiShip_IsDisabled(state, ship)) {
    return false;
  }
  // Locate a ModType 17 cloaking device and take its ModVal drain bits. The
  // player scans owned outfits; NPCs scan the ship class's default outfit
  // list (no escort exception here, unlike Ship_CanShipEngageTargetUnder-
  // CloakRules' helper).
  std::uint16_t mod_val = 0;
  bool has_cloak = false;
  const auto scan_outfit = [&mod_val, &has_cloak](const Outfit *outfit) {
    if (outfit == nullptr || has_cloak) {
      return;
    }
    if (outfit->mod_type == 0x11) {
      mod_val = static_cast<std::uint16_t>(outfit->mod_val);
      has_cloak = true;
      return;
    }
    for (std::size_t i = 0; i < outfit->alt_mod_types.size(); ++i) {
      if (outfit->alt_mod_types[i] == 0x11) {
        mod_val = static_cast<std::uint16_t>(outfit->alt_mod_vals[i]);
        has_cloak = true;
        return;
      }
    }
  };
  if (ship.ship_instance_id == 0) {
    for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size();
         ++id) {
      if (state.inventory.outfit_owned_count[id] > 0) {
        scan_outfit(
            state.scenario.Outfit(static_cast<std::int16_t>(id + 0x80)));
      }
    }
  } else {
    const auto *ship_class = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    if (ship_class != nullptr) {
      for (std::size_t i = 0; i < ship_class->default_outfit_ids.size(); ++i) {
        if (ship_class->default_outfit_counts[i] > 0) {
          scan_outfit(state.scenario.Outfit(ship_class->default_outfit_ids[i]));
        }
      }
    }
  }
  if (!has_cloak) {
    return false;
  }
  // ModVal bits 0x0010..0x0080 gate on fuel, 0x0100..0x0800 on shields.
  if (((mod_val & 0x00f0U) != 0U) && ship.fuel_points <= 0.0F) {
    return false;
  }
  if (((mod_val & 0x0f00U) != 0U) && ship.shield_points <= 0.0F) {
    return false;
  }
  if (ship.ship_instance_id == 0) {
    // The player-only tail (station-hold latch + class Flags2 0x400) is
    // decompiler-garbled in the original; TODO(decomp(0x00467e80)) provisional.
  }
  return true;
}

// Ghidra 0x0040f780 Ship_ShouldShipKeepPressingTarget.
bool NovaAiShip_ShouldKeepPressingTarget(const GameState &state,
                                         const Ship &ship) {
  if (!ship.is_active || NovaAiShip_IsDisabled(state, ship)) {
    return false;
  }
  // The original admits an unset (-1) AI target here; it specifically rejects
  // the player slot 0 because that path is handled by the primary-target and
  // mutual-target checks below.
  if (ship.squad_leader_ship_slot == 0 || ship.primary_target_ship_slot == -1) {
    return false;
  }
  if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, ship, state.player)) {
    return false;
  }
  if (ship.defense_fleet_home_stellar_id != -1) {
    return true; // docked/landed against a stellar: keeps pressing
  }
  const std::int16_t target_slot = ship.primary_target_ship_slot;
  const bool coasting_reversal = ship.ai_maneuver_timer_ms > 0.0F;
  const bool engaged =
      !IsDisengageState(ship.ai_state_code, /*include_state18=*/true);
  const Ship *target =
      target_slot >= 0 &&
              static_cast<std::size_t>(target_slot) < GameState::kMaxShips
          ? &state.ShipAt(static_cast<std::size_t>(target_slot))
          : nullptr;
  // Targeting the player (slot 0): press while not coasting through a
  // reversal and not in a disengage state.
  if (target_slot == 0) {
    if (!coasting_reversal && engaged) {
      return true;
    }
  }
  // The target ship points at the player (the original evaluates this block
  // and the identical sVar6 != -1 block below separately; both require the
  // target's squad_leader_ship_slot to be the player).
  if (target != nullptr && target->squad_leader_ship_slot == 0) {
    if (!coasting_reversal && engaged) {
      return true;
    }
    if (target_slot != -1 && !coasting_reversal && engaged) {
      return true;
    }
  }
  // A third ship pressing this ship: scan the other slots for one whose
  // squad_leader_ship_slot is this ship's instance id, is not coasting through
  // a reversal, is active, unrestricted, and (like the direct checks) in a
  // non-disengage state while itself holding the player or a player-targeting
  // ship as primary.
  for (std::size_t j = 1; j < GameState::kMaxShips; ++j) {
    const Ship &other = state.ShipAt(j);
    if (other.squad_leader_ship_slot != ship.ship_instance_id) {
      continue;
    }
    if (other.ai_maneuver_timer_ms > 0.0F || !other.is_active) {
      continue;
    }
    if (NovaAiShip_IsDisabled(state, other)) {
      continue;
    }
    const std::int16_t o_target = other.primary_target_ship_slot;
    if (o_target == 0) {
      if (engaged) {
        return true;
      }
    }
    if (o_target != -1 &&
        static_cast<std::size_t>(o_target) < GameState::kMaxShips) {
      const Ship &other_target =
          state.ShipAt(static_cast<std::size_t>(o_target));
      if (engaged && other_target.squad_leader_ship_slot == 0) {
        return true;
      }
    }
  }
  return false;
}

// Ghidra 0x0040fc00 Ship_IsThreatened.
bool NovaAiShip_IsThreatened(const GameState &state, const Ship &ship) {
  if (!ship.is_active || NovaAiShip_IsDisabled(state, ship)) {
    return false;
  }
  for (std::size_t j = 1; j < GameState::kMaxShips; ++j) {
    const Ship &other = state.ShipAt(j);
    if (!other.is_active) {
      continue;
    }
    // Literal port of the original's condition: the *target* ship's primary
    // target slot is compared with its own instance id (disasm 0x0040fc51:
    // CMP [EBX+0x70], [EBX+0x86]). With instance ids equal to slots in this
    // build this reads "targets its own slot", which is what the original
    // effectively tests; kept verbatim to preserve the quirk.
    if (ship.primary_target_ship_slot != ship.ship_instance_id) {
      continue;
    }
    if (static_cast<std::int16_t>(j) == ship.ship_instance_id) {
      continue;
    }
    if (!IsDisengageState(other.ai_state_code, /*include_state18=*/false)) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x0040fca0 / 0x0040fce0.
bool NovaAiShip_IsShipAssistingPlayerState9(const GameState &state,
                                            const Ship &ship) {
  return ship.is_active && !NovaAiShip_IsDisabled(state, ship) &&
         ship.primary_target_ship_slot == 0 && ship.ai_state_code == 9;
}

bool NovaAiShip_IsShipAssistingPlayerState0xF(const GameState &state,
                                              const Ship &ship) {
  return ship.is_active && !NovaAiShip_IsDisabled(state, ship) &&
         ship.primary_target_ship_slot == 0 && ship.ai_state_code == 0x0F;
}

// Ghidra 0x004102b0 Ship_IsShipLockedOnAttackerInAiState0x04.
bool NovaAiShip_IsShipLockedOnAttackerInState4(const Ship &ship,
                                               const Ship &attacker) {
  return ship.primary_target_ship_slot == attacker.ship_instance_id &&
         ship.ai_state_code == 4;
}

// Ghidra 0x004124f0 Ship_IsShipLockedOnTarget.
bool NovaAiShip_IsShipLockedOnTarget(const Ship &ship, const Ship &target) {
  const std::int16_t state = ship.ai_state_code;
  return (state == 0x0D || (state == 4 && ship.ai_control_mode == 0x0F)) &&
         ship.primary_target_ship_slot == target.ship_instance_id;
}

// Ghidra 0x00410ce0 Ship_IsShipInAiBehavior5State0x05.
bool NovaAiShip_IsShipInAiBehavior5State5(const Ship &ship) {
  return ship.ai_behavior_code == 5 && ship.ai_state_code == 5;
}

// Ghidra 0x00410e80 Ship_IsShipInHoldStateWithControlMode4Or0x0D.
bool NovaAiShip_IsShipInHoldStateWithControlMode4Or0xD(const Ship &ship) {
  const std::int16_t s = ship.ai_state_code;
  return (s == 2 || s == 3 || s == 0x0B) &&
         (ship.ai_control_mode == 4 || ship.ai_control_mode == 0x0D);
}

// Ghidra 0x00410ec0 Ship_IsShipInAiState0x08.
bool NovaAiShip_IsShipInAiState8(const Ship &ship) {
  return ship.ai_state_code == 8;
}

// Ghidra 0x00410ee0 Ship_IsShipInAiControlMode0x0C.
bool NovaAiShip_IsShipInAiControlModeC(const Ship &ship) {
  return ship.ai_control_mode == 0x0C;
}

// Ghidra 0x00410f00 Ship_IsShipInAiState0x04.
bool NovaAiShip_IsShipInAiState4(const Ship &ship) {
  return ship.ai_state_code == 4;
}

// Ghidra Ship_IsInPlayerSquad (0x0046b8d0). Returns true when ship is the
// player (ship_instance_id 0), is attached directly to the player
// (squad_leader_ship_slot 0), or is attached to a ship that is itself attached
// to the player. Deliberately player-specific: the inner slot is compared to 0
// (the player), not to a generic root, so this is not the squad-root walk of
// Ship_ShipsShareSquadRoot (0x0046d190). The ship's own active flag is not
// consulted; callers that care about liveness check it separately.
bool NovaShip_IsInPlayerSquad(const GameState &state, const Ship &ship) {
  if (ship.ship_instance_id == 0) {
    return true;
  }
  const std::int16_t leader = ship.squad_leader_ship_slot;
  if (leader == 0) {
    return true;
  }
  return leader > 0 &&
         leader < static_cast<std::int16_t>(GameState::kMaxShips) &&
         state.ShipAt(static_cast<std::size_t>(leader))
                 .squad_leader_ship_slot == 0;
}

// Ghidra 0x004112a0 Ship_IsShipInAiState0x02.
bool NovaAiShip_IsShipInAiState2(const Ship &ship) {
  return ship.ai_state_code == 2;
}

// Ghidra 0x00411270 Ship_IsShipInNonIdleAiState.
bool NovaAiShip_IsShipInNonIdleAiState(const Ship &ship) {
  const std::int16_t s = ship.ai_state_code;
  return s != 0 && s != 2 && s != 1 && s != 0x14 && s != 7;
}

// Ghidra 0x00410060 Ship_IsAnyShipThreatToPlayerSquad.
bool NovaAi_IsAnyShipThreatToPlayerSquad(const GameState &state) {
  for (std::size_t j = 1; j < GameState::kMaxShips; ++j) {
    const Ship &other = state.ShipAt(j);
    if (NovaTargeting_IsThreatToPlayerSquad(state, other)) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x004101d0 Ship_IsEnemyOfShip.
bool NovaAiShip_IsEnemyOfShip(const GameState &state,
                              const Ship &ship,
                              const Ship &other) {
  if (ship.ship_instance_id == other.ship_instance_id) {
    return false;
  }
  if (!ship.is_active || !other.is_active) {
    return false;
  }
  if (other.pers_def_slot == 0x3ff) {
    return false;
  }
  const std::int16_t govt_a = ship.faction_or_government_id;
  const std::int16_t govt_b = other.faction_or_government_id;
  if (govt_a != -1 && govt_b != -1) {
    if (govt_a == govt_b) {
      return false;
    }
    if (NovaGovernment_AreGovtsHostileOrXenophobic(
            state.scenario, govt_a, govt_b)) {
      return true;
    }
    // A xenophobic `other` government treats any non-allied ship as an enemy
    // (literal port of the decomp's goto LAB_0041029d).
    if ((state.scenario.governments[static_cast<std::size_t>(govt_b)]
             .flags_primary &
         0x1U) != 0 &&
        !NovaGovernment_AreGovtsAllied(state.scenario, govt_a, govt_b)) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x004100a0 Ship_IsPlayerThreatenedByEnemyOfShip.
bool NovaAiShip_IsPlayerThreatenedByEnemyOfShip(const GameState &state,
                                                const Ship &ship) {
  for (std::size_t j = 1; j < GameState::kMaxShips; ++j) {
    const Ship &other = state.ShipAt(j);
    if (!other.is_active ||
        static_cast<std::int16_t>(j) == ship.ship_instance_id) {
      continue;
    }
    if (NovaAiShip_ShouldKeepPressingTarget(state, other) &&
        NovaAiShip_IsEnemyOfShip(state, ship, other)) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x00410110 Ship_IsThreatenedByEnemyOfShip.
bool NovaAiShip_IsThreatenedByEnemyOfShip(const GameState &state,
                                          const Ship &ship,
                                          const Ship &context_ship) {
  if (ship.ship_instance_id == 0) {
    return NovaAiShip_IsPlayerThreatenedByEnemyOfShip(state, context_ship);
  }
  if (NovaAiShip_ShouldKeepPressingTarget(state, ship)) {
    return true;
  }
  for (std::size_t j = 1; j < GameState::kMaxShips; ++j) {
    const Ship &candidate = state.ShipAt(j);
    if (!candidate.is_active ||
        static_cast<std::int16_t>(j) == context_ship.ship_instance_id ||
        static_cast<std::int16_t>(j) == ship.ship_instance_id) {
      continue;
    }
    if (NovaTargeting_IsShipAcquirableAsTarget(state, candidate, ship) &&
        NovaAiShip_IsEnemyOfShip(state, context_ship, candidate)) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x00410c30 Ship_EnterShipAiState0x09_TargetPlayerForAssist.
void NovaAi_EnterState9TargetPlayerForAssist(Ship &ship) {
  ship.ai_hostility_accumulator = 0;
  ship.ai_station_hold_timer = 0.0F;
  ship.primary_target_ship_slot = 0;
  ship.ai_state_code = 9;
  ship.ai_control_mode = 0;
  ship.ai_maneuver_timer_ms = -1.0F;
}

// Ghidra 0x00410c70 Ship_EnterShipAiState0x0F_TargetPlayerForAssist.
void NovaAi_EnterState0FTargetPlayerForAssist(Ship &ship) {
  ship.ai_hostility_accumulator = 0;
  ship.ai_station_hold_timer = 0.0F;
  ship.primary_target_ship_slot = 0;
  ship.ai_state_code = 0x0F;
  ship.ai_control_mode = 0;
  ship.ai_maneuver_timer_ms = -1.0F;
}

namespace {

// Shared candidate predicate of the two state-0x04 target-random entries. When
// `require_distress` is set the candidate must additionally be distress-
// eligible (Ship_EnterShipAiState0x04_TargetRandomCombatCandidate).
[[nodiscard]] bool IsState4Candidate(const GameState &state,
                                     const Ship &candidate,
                                     std::int16_t self_id,
                                     std::int16_t system_id,
                                     bool require_distress) {
  if (candidate.ship_instance_id == self_id) {
    return false;
  }
  if (NovaAiShip_IsDisabled(state, candidate) ||
      NovaAiShip_IsDestroyed(candidate)) {
    return false;
  }
  if (candidate.current_system_id != system_id) {
    return false;
  }
  if (require_distress) {
    return NovaTargeting_IsThreatToPlayerSquad(state, candidate);
  }
  return candidate.primary_target_ship_slot == 0 &&
         (candidate.ai_state_code == 3 || candidate.ai_state_code == 4);
}

} // namespace

// Ghidra 0x00410b00 Ship_EnterShipAiState0x04_TargetRandomUnengagedShip.
void NovaAi_EnterState4TargetRandomUnengagedShip(GameState &state, Ship &ship) {
  std::int16_t count = 0;
  for (std::size_t j = 1; j < GameState::kMaxShips; ++j) {
    if (IsState4Candidate(state,
                          state.ShipAt(j),
                          ship.ship_instance_id,
                          ship.current_system_id,
                          /*require_distress=*/false)) {
      ++count;
    }
  }
  if (count < 1) {
    ship.primary_target_ship_slot = -1;
    ship.ai_state_code = 4;
    return;
  }
  // Random pick over slots 1..0x3f until a valid candidate is found (the
  // original's nested do-while ladder).
  for (;;) {
    std::int16_t pick;
    do {
      pick = static_cast<std::int16_t>(RandomBelow(state, 0x3f) + 1);
    } while (pick == ship.ship_instance_id || pick == 0);
    if (IsState4Candidate(state,
                          state.ShipAt(static_cast<std::size_t>(pick)),
                          ship.ship_instance_id,
                          ship.current_system_id,
                          /*require_distress=*/false)) {
      ship.primary_target_ship_slot = pick;
      break;
    }
  }
  ship.ai_state_code = 4;
}

// Ghidra 0x004107e0 Ship_EnterShipAiState0x04_TargetRandomCombatCandidate.
void NovaAi_EnterState4TargetRandomCombatCandidate(GameState &state,
                                                   Ship &ship) {
  std::int16_t count = 0;
  for (std::size_t j = 1; j < GameState::kMaxShips; ++j) {
    if (IsState4Candidate(state,
                          state.ShipAt(j),
                          ship.ship_instance_id,
                          ship.current_system_id,
                          /*require_distress=*/true)) {
      ++count;
    }
  }
  if (count < 1) {
    ship.primary_target_ship_slot = -1;
    ship.ai_state_code = 4;
    return;
  }
  for (;;) {
    std::int16_t pick;
    do {
      pick = static_cast<std::int16_t>(RandomBelow(state, 0x3f) + 1);
    } while (pick == ship.ship_instance_id || pick == 0);
    if (IsState4Candidate(state,
                          state.ShipAt(static_cast<std::size_t>(pick)),
                          ship.ship_instance_id,
                          ship.current_system_id,
                          /*require_distress=*/true)) {
      ship.ai_secondary_target_slot = -1;
      ship.primary_target_ship_slot = pick;
      break;
    }
  }
  ship.ai_state_code = 4;
}

// Ghidra 0x004106b0 Ship_EnterShipAiState0x0B_ClearTargetsSeedHold.
void NovaAi_EnterStateBClearTargetsSeedHold(Ship &ship,
                                            std::uint32_t now_60hz) {
  ship.ai_state_code = 0x0B;
  ship.ai_hostility_accumulator = 0;
  ship.primary_target_ship_slot = -1;
  if (ship.ai_station_hold_timer <= 0.0F) {
    ship.ai_station_hold_timer = 1.0F;
    ship.ai_mode_start_time_ms = now_60hz;
  }
}

// Ghidra 0x00422340 Ship_SyncJumpStateToSquad. During the squad leader's
// jump-engage hold, copies the leader's hold clock (ai_station_hold_timer +
// ai_mode_start_time_ms) into every active squadmate with no stellar
// attachment, marks its primary target with the -2 sentinel, and enters AI
// state 0x0B (clear targets / seed hold): squadmates disengage and hold
// formation in lockstep while the leader charges the jump. Escorts transfer
// systems at arrival via the escort-adoption slice of
// System_RebuildInitialNpcAndMissionPopulation (0x0041af90), not here. Slot 0
// (the player) is never a follower and is skipped by the original's slot-1..63
// scan. Sole caller: Ship_HandlePlayerShipCore 0x0044c705 (jump-engage hold).
void NovaAi_SyncJumpStateToSquad(GameState &state,
                                 Ship &leader,
                                 std::uint32_t now_60hz) {
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &follower = state.ShipAt(slot);
    if (!follower.is_active ||
        follower.squad_leader_ship_slot != leader.ship_instance_id ||
        follower.defense_fleet_home_stellar_id != -1) {
      continue;
    }
    follower.ai_station_hold_timer = leader.ai_station_hold_timer;
    follower.ai_mode_start_time_ms = leader.ai_mode_start_time_ms;
    follower.primary_target_ship_slot = -2;
    NovaAi_EnterStateBClearTargetsSeedHold(follower, now_60hz);
  }
}

// Ghidra 0x00410cb0 Ship_EnterShipAiState0x05_ReturnToSquadLeader.
void NovaAi_EnterState5ReturnToSquadLeader(Ship &ship) {
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = ship.squad_leader_ship_slot;
  ship.ai_state_code = 5;
  ship.ai_control_mode = 0;
}

// Ghidra 0x00410900
// Ship_EnterShipAiState0x04_TargetRandomRelativeToSquadLeader.
void NovaAi_EnterState4TargetRandomRelativeToSquadLeader(GameState &state,
                                                         Ship &ship) {
  const std::int16_t leader_slot = ship.squad_leader_ship_slot;
  const bool leader_valid =
      leader_slot >= 0 &&
      static_cast<std::size_t>(leader_slot) < GameState::kMaxShips;
  const Ship *leader =
      leader_valid ? &state.ShipAt(static_cast<std::size_t>(leader_slot))
                   : nullptr;

  // Per-candidate eligibility, relative to the ship's squad leader. Slot 0
  // (the player) is special: the gate tests the SQUAD LEADER's pressing
  // state, not the candidate's (disasm 0x00410993 / 0x00410ab6). Other
  // candidates are admitted by the leader's pressing state when the leader is
  // the player, otherwise by pairwise acquirability from the leader. The
  // original dereferences g_ship_states + squad_leader_ship_slot without a
  // bounds check; a negative/unset leader (-1) fails the gate here.
  // TODO(decomp(0x00410900)): confirm no caller reaches this without a leader.
  auto candidate_gate = [&](const Ship &candidate, std::int16_t slot) {
    if (slot == 0) {
      return leader != nullptr &&
             NovaAiShip_ShouldKeepPressingTarget(state, *leader);
    }
    if (leader_slot == 0) {
      return NovaAiShip_ShouldKeepPressingTarget(state, candidate);
    }
    return leader != nullptr &&
           NovaTargeting_IsShipAcquirableAsTarget(state, candidate, *leader);
  };

  // Count pass: no is_active gate (disabled/system checks only, matching the
  // original's 0x00410910 ladder).
  std::int16_t count = 0;
  for (std::size_t j = 0; j < GameState::kMaxShips; ++j) {
    const auto slot = static_cast<std::int16_t>(j);
    if (slot == ship.ship_instance_id || slot == leader_slot) {
      continue;
    }
    const Ship &candidate = state.ShipAt(j);
    if (NovaAiShip_IsDisabled(state, candidate) ||
        candidate.current_system_id != ship.current_system_id) {
      continue;
    }
    if (candidate_gate(candidate, slot)) {
      ++count;
    }
  }
  if (count < 1) {
    // Clears the primary target only; no state change (disasm 0x004109d0).
    ship.primary_target_ship_slot = -1;
    return;
  }

  // Random pick over slots 0..0x3f (NovaRandom_Range(0x40), so the player IS
  // an admittable pick) until a valid candidate is found; the count pass
  // guarantees at least one exists. The pick pass adds the is_active gate.
  for (;;) {
    const auto pick = static_cast<std::int16_t>(RandomBelow(state, 0x40));
    if (pick == ship.ship_instance_id || pick == leader_slot) {
      continue;
    }
    const Ship &candidate = state.ShipAt(static_cast<std::size_t>(pick));
    if (!candidate.is_active ||
        candidate.current_system_id != ship.current_system_id ||
        NovaAiShip_IsDisabled(state, candidate)) {
      continue;
    }
    if (candidate_gate(candidate, pick)) {
      ship.ai_secondary_target_slot = -1;
      ship.primary_target_ship_slot = pick;
      ship.ai_state_code = 4;
      return;
    }
  }
}

// Ghidra 0x00410700 Ship_SetShipHostileToPlayer. Ports the escort-mode/state
// flip plus the pers announcement arm: a personality ship (no mission fleet)
// whose pers Flags carry 0x10 hails once when made hostile, unless it is
// already disabled/destroyed or still pressing its previous target.
void NovaAi_SetShipHostileToPlayer(GameState &state, Ship &ship) {
  if (!NovaAiShip_ShouldKeepPressingTarget(state, ship) &&
      ship.pers_def_slot >= 0 && ship.mission_fleet_slot == -1) {
    const auto &pers =
        state.scenario.pers_defs[static_cast<std::size_t>(ship.pers_def_slot)];
    if ((static_cast<std::uint16_t>(pers.flags_primary) & 0x10U) != 0U &&
        !NovaAiShip_IsDisabled(state, ship) && !NovaAiShip_IsDestroyed(ship)) {
      state.mission_speaker_ship_slot = ship.ship_instance_id;
      Mission_ShowMissionShipAnnouncement(state, pers.hail_quote_id);
      ship.mission_hail_latch = 1;
      state.mission_speaker_ship_slot = -1;
    }
  }
  if (ship.ai_control_mode == 4 || ship.ai_control_mode == 0x0D) {
    ship.ai_control_mode = 0;
  }
  ship.ai_state_code = 4;
  ship.ai_secondary_target_slot = -1;
  ship.primary_target_ship_slot = 0;
}

// Ghidra 0x0046b260 Ship_CanShipUseAfterburner. Returns the low byte the
// original stores into ShipState +0xBD.
bool NovaShip_CanShipUseAfterburner(GameState &state, const Ship &ship) {
  // Another active ship led by this ship's instance id blocks the latch.
  if (ship.ship_instance_id != 0) {
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      const Ship &other = state.ShipAt(slot);
      if (static_cast<std::int16_t>(slot) != ship.ship_instance_id &&
          other.is_active &&
          other.formation_leader_ship_slot == ship.ship_instance_id) {
        return false;
      }
    }
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls == nullptr) {
    return false;
  }
  const std::uint16_t capability = cls->capability_flags;
  if ((capability & 0x0400U) != 0U) {
    return false; // planet-type ship: never
  }
  if ((capability & 0x0040U) != 0U) {
    return true; // always afterburner (AI ships)
  }
  if ((capability & 0x0020U) != 0U) {
    // Bible shïp Flags 0x0020: afterburner once the rating passes a random
    // roll (NovaRandom_Range(0x540) + 0x100 <= rating / base). Base is the
    // pinned class-0 Strength, not this ship's own (0x0046b308); see
    // game_state.hpp.
    const auto roll = static_cast<int>(
        std::uniform_int_distribution<int>{0, 0x53f}(state.rng));
    const auto threshold = state.player_combat_rating_points /
                           GameState::kCombatRatingBaseStrength;
    return roll + 0x100 <= threshold;
  }
  return false;
}

// Ghidra 0x00423fa0 (replacement init) and 0x00415cb0 (captured escort reset)
// share this tail: random voice mode, then the class inherent-government
// override.
void NovaShip_ApplyInherentGovernmentVoice(GameState &state, Ship &ship) {
  ship.voice_type_mode = RandomBelow(state.rng, 2);
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls == nullptr || cls->inherent_attributes_govt == -1) {
    return;
  }
  const Government *govt =
      state.scenario.GovernmentByIndex(cls->inherent_attributes_govt);
  if (govt != nullptr && govt->voice_type_mode != -1) {
    ship.voice_type_mode = govt->voice_type_mode;
  }
}

// Ghidra 0x00402810 Ship_ResetShipAiBehaviorRuntimeFields.
void NovaShip_ResetAiBehaviorRuntimeFields(Ship &ship) {
  ship.ai_state_code = 0;
  ship.ai_control_mode = 0;
  ship.jump_destination_stellar_id = -2;
  ship.travel_target_cache = -1;
  ship.escort_command_code = -1;
  ship.formation_leader_ship_slot = -1;
  ship.resolved_squad_leader_ship_slot = -1;
}

// Ghidra 0x00410d10 Ship_EnterSquadReturnState; the behavior-5
// follower sweep at the tail is 0x00410cb0
// Ship_EnterShipAiState0x05_ReturnToSquadLeader run inline.
void NovaShip_EnterSquadReturnState(GameState &state, Ship &ship) {
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = ship.squad_leader_ship_slot;
  if (ship.ai_behavior_code == 5) {
    ship.ai_state_code = 10;
  } else if (ship.squad_leader_ship_slot == 0) {
    ship.ai_state_code = 0x0c;
  } else {
    ship.ai_state_code = 10;
  }
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &follower = state.ShipAt(slot);
    if (follower.is_active &&
        static_cast<std::int16_t>(slot) != ship.ship_instance_id &&
        follower.squad_leader_ship_slot == ship.ship_instance_id &&
        follower.ai_behavior_code == 5) {
      follower.primary_target_ship_slot = -1;
      follower.ai_secondary_target_slot = follower.squad_leader_ship_slot;
      follower.ai_state_code = 5;
      follower.ai_control_mode = 0;
    }
  }
}

} // namespace game
