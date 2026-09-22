// Clean-room reconstruction of NPC ship behavior supervisors and travel/target
// policy.

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

// Ghidra 0x0040c790 Stellar_SelectRandomAdjacentTravelStellar. Selects a
// random adjacent travel stellar in the ship's current system and returns the
// stellar *resource id* (>= 0x80) or -1. One pass builds five 16-slot per-nav
// masks:
//   sprite_active: nav slot valid AND NovaTargeting_StellarTargetsSpriteSet-
//                  Active(st) AND map pos < 1000 on both axes.
//   travel_usable: (travel_flags & 0x20) != 0 AND availability_flags & 0x3000
//                  == 0 (a restricted-range travel point).
//   hostile:       ship and stellar both have a government and
//                  NovaGovernment_AreGovtsHostileOrXenophobic is true.
//   avail_1000 / avail_2000: availability_flags bit 0x1000 / 0x2000.
// A second phase selects among ScanMask-gated pools (GovtDef +0x22 = Bible
// ScanMask, payload +0x04): bit 0x80 prefers avail_2000, 0x40 prefers
// avail_1000, and 0x20 forces the plain pool in strict mode. `strict_mode`
// (the 1-in-3 roll from Stellar_SelectRandomAdjacentDestination) restricts
// selection to the plain pool unless ScanMask 0x20 applies; `unrestricted_only`
// (passed true by the behavior-0x04 interceptor caller 0x00403de0) selects
// only the plain pool. Rejection sampling mirrors the original draw-for-draw;
// the original's unbounded loops are capped because malformed availability
// data can make the strict pool empty (the original would spin forever).
std::int16_t NovaAi_SelectRandomAdjacentTravelStellar(GameState &state,
                                                      const Ship &ship,
                                                      bool strict_mode,
                                                      bool unrestricted_only) {
  const System *sys = state.scenario.System(
      static_cast<std::int16_t>(ship.current_system_id + 0x80));
  if (sys == nullptr) {
    return -1;
  }

  std::array<bool, 16> sprite_active{};
  std::array<bool, 16> travel_usable{};
  std::array<bool, 16> hostile{};
  std::array<bool, 16> avail_1000{};
  std::array<bool, 16> avail_2000{};
  int eligible_count = 0; // original local_1c
  int plain_count = 0;    // original local_20
  int count_1000 = 0;     // original local_14
  int count_2000 = 0;     // original sVar4

  for (std::size_t i = 0; i < sys->nav_defs.size(); ++i) {
    const std::int16_t nav = sys->nav_defs[i];
    if (nav < 0) {
      continue; // empty nav slot (original tests nav_stellar_ids[i] != -1)
    }
    const Stellar *st = StellarByResourceId(state, nav);
    if (st == nullptr) {
      continue;
    }
    sprite_active[i] = NovaTargeting_StellarTargetsSpriteSetActive(*st) &&
                       st->pos_x < 1000 && st->pos_y < 1000;
    travel_usable[i] =
        (st->flags & 0x20U) != 0U && (st->availability_flags & 0x3000U) == 0U;
    hostile[i] =
        ship.faction_or_government_id >= 0 && st->government_id >= 0 &&
        NovaGovernment_AreGovtsHostileOrXenophobic(
            state.scenario, ship.faction_or_government_id, st->government_id);
    if ((st->availability_flags & 0x1000U) != 0U) {
      avail_1000[i] = true;
      if (sprite_active[i] && !hostile[i]) {
        ++count_1000;
      }
    }
    if ((st->availability_flags & 0x2000U) != 0U) {
      avail_2000[i] = true;
      if (sprite_active[i] && !hostile[i]) {
        ++count_2000;
      }
    }
    if (sprite_active[i] && !travel_usable[i] && !hostile[i]) {
      ++eligible_count;
      if (!avail_1000[i] && !avail_2000[i]) {
        ++plain_count;
      }
    }
  }

  bool mask20 = false;
  bool prefer_1000 = false;
  bool prefer_2000 = false;
  if (ship.faction_or_government_id >= 0) {
    if (const Government *govt =
            state.scenario.GovernmentByIndex(ship.faction_or_government_id);
        govt != nullptr) {
      mask20 = (govt->flags_secondary & 0x20U) != 0U;
      prefer_1000 = (govt->flags_secondary & 0x40U) != 0U;
      prefer_2000 = (govt->flags_secondary & 0x80U) != 0U;
    }
  }

  // Rejection sampling over the 16 slots, exactly as the original. The bound
  // plus deterministic fallback guards against the original's latent infinite
  // loop on contradictory availability/ScanMask data; the first 256 draws keep
  // the original RNG cadence in every reachable case.
  const auto pick_from = [&sys, &state](const auto &accept) -> std::int16_t {
    for (int attempt = 0; attempt < 0x100; ++attempt) {
      const std::size_t slot = static_cast<std::size_t>(
          std::uniform_int_distribution<int>(0, 15)(state.rng));
      if (accept(slot)) {
        return static_cast<std::int16_t>(slot);
      }
    }
    for (std::size_t slot = 0; slot < sys->nav_defs.size(); ++slot) {
      if (accept(slot)) {
        return static_cast<std::int16_t>(slot);
      }
    }
    return -1;
  };

  std::int16_t picked = -1;
  if (unrestricted_only) {
    if (plain_count < 1) {
      return -1;
    }
    picked = pick_from([&](std::size_t s) {
      return sprite_active[s] && !avail_1000[s] && !avail_2000[s] &&
             !hostile[s];
    });
  } else if (prefer_2000 && count_2000 > 0) {
    picked = pick_from([&](std::size_t s) {
      return sprite_active[s] && avail_2000[s] && !hostile[s];
    });
  } else if (prefer_1000 && count_1000 > 0) {
    picked = pick_from([&](std::size_t s) {
      return sprite_active[s] && avail_1000[s] && !hostile[s];
    });
  } else if (eligible_count < 1 || !strict_mode || mask20 ||
             (plain_count < 1 && count_1000 < 1)) {
    // Preference-respecting fallback pool.
    if (eligible_count < 1 || (plain_count < 1 && (count_1000 < 1 || mask20))) {
      return -1;
    }
    picked = pick_from([&](std::size_t s) {
      return sprite_active[s] && !travel_usable[s] && !hostile[s] &&
             (!avail_2000[s] || prefer_2000) && (!avail_1000[s] || !mask20);
    });
  } else {
    // Strict-mode pool: eligible candidates without the avail_2000 bit.
    picked = pick_from([&](std::size_t s) {
      return sprite_active[s] && !travel_usable[s] && !hostile[s] &&
             !avail_2000[s];
    });
  }

  if (picked < 0) {
    return -1;
  }
  return sys->nav_defs[static_cast<std::size_t>(picked)];
}

// Ghidra 0x0046e9e0 Stellar_SelectRandomAdjacentDestination. Picks a random
// adjacent travel stellar for an NPC spawn, but returns the id only when
// availability_flags & 0x3000 marks it a hypergate/wormhole; otherwise -1.
// The NovaRandom_Range(3) roll supplies the selector's strict_mode argument
// (it is NOT a direct 1-in-3 emergence gate): it is 1 only when the roll is 0.
std::int16_t NovaAi_SelectRandomAdjacentDestination(GameState &state,
                                                    const Ship &ship) {
  const bool strict_mode =
      std::uniform_int_distribution<int>(0, 2)(state.rng) == 0;
  const std::int16_t stellar_id = NovaAi_SelectRandomAdjacentTravelStellar(
      state, ship, strict_mode, /*unrestricted_only=*/false);
  if (stellar_id < 0 || stellar_id >= 0x800) {
    return -1;
  }
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr || (stellar->availability_flags & 0x3000U) == 0U) {
    return -1;
  }
  return stellar_id;
}

// Ghidra 0x0040cc10 Stellar_FindNearestAdjacentTravelStellar. Returns the
// nearest adjacent travel stellar to `ship` in its current system, or -1.
// Scans the 16 nav slots and skips empty slots, restricted travel points
// (availability_flags & 0x3000), stellars hostile to the ship's government
// (when both governments are valid), and `excluded_stellar_id` (its sole
// caller passes jump_destination_stellar_id). Unlike the random selector it
// ignores travel_flags and ScanMask preferences and minimises the plain
// squared distance between the ship and the stellar map position.
std::int16_t
NovaAi_FindNearestAdjacentTravelStellar(const GameState &state,
                                        const Ship &ship,
                                        std::int16_t excluded_stellar_id) {
  const System *sys = state.scenario.System(
      static_cast<std::int16_t>(ship.current_system_id + 0x80));
  if (sys == nullptr) {
    return -1;
  }
  std::int16_t best = -1;
  float best_distance_sq = 0.0F;
  for (const std::int16_t nav : sys->nav_defs) {
    if (nav < 0) {
      continue;
    }
    const Stellar *st = StellarByResourceId(state, nav);
    if (st == nullptr) {
      continue;
    }
    if ((st->availability_flags & 0x3000U) != 0U) {
      continue; // restricted travel point
    }
    if (ship.faction_or_government_id >= 0 && st->government_id >= 0 &&
        NovaGovernment_AreGovtsHostileOrXenophobic(
            state.scenario, ship.faction_or_government_id, st->government_id)) {
      continue;
    }
    if (excluded_stellar_id == nav) {
      continue;
    }
    const float distance_sq = SquaredDistance(ship.pos_x,
                                              ship.pos_y,
                                              static_cast<float>(st->pos_x),
                                              static_cast<float>(st->pos_y));
    if (best < 0 || distance_sq < best_distance_sq) {
      best = nav;
      best_distance_sq = distance_sq;
    }
  }
  return best;
}

namespace {

// Squared-distance metric used by Ship_AcquirePrimaryTargetForShip
// (0x0040e020, x sequence 0x0040f4a7, y sequence 0x0040f50e). The original
// takes FABS of each axis, stores it with FIST, then runs a residual/sign
// correction (0x0040f4b7..0x0040f4f6 and the analogous y sequence) that turns
// the round-to-nearest FIST into a truncation toward zero. Because the axis is
// already absolute, the net result is floor(|axis|); it is not round-to-nearest
// and must not be approximated with std::lround.
[[nodiscard]] std::int32_t
RoundedAxisDistanceSquared(float x1, float y1, float x2, float y2) {
  const std::int32_t dx = static_cast<std::int32_t>(std::fabs(x1 - x2));
  const std::int32_t dy = static_cast<std::int32_t>(std::fabs(y1 - y2));
  return dx * dx + dy * dy;
}

} // namespace

// Ghidra 0x00411800 Ship_ComputePerceivedCombatStrengthAgainstShip.
int NovaAiShip_ComputePerceivedCombatStrength(const GameState &state,
                                              const Ship &ship) {
  // FIST at 0x0041187f and 0x00411a35 is followed by a residual/sign
  // correction (0x00411891..0x004118b7, 0x00411a41..0x00411a69) that turns
  // the round-to-nearest store into a truncation toward zero. Every ratio here
  // is non-negative, so the net effect is floor().
  const auto trunc_to_int = [](float value) -> int {
    return static_cast<int>(value);
  };
  const auto clamp_ratio = [](float ratio) -> float {
    if (ratio > 1.0F) {
      ratio = 1.0F;
    }
    if (ratio < 0.25F) {
      ratio = 0.25F;
    }
    return ratio;
  };

  const ShipClass *ship_class =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  // 0x00411a90: a non-positive max shield seeds the initial ratio with the
  // subject's raw shield points (then the same [0.25, 1.0] clamp), not 0.25.
  const float ship_max_shield =
      static_cast<float>(NovaAi_ComputeMaxShieldPoints(state, ship));
  float shield_ratio =
      clamp_ratio(ship_max_shield > 0.0F ? ship.shield_points / ship_max_shield
                                         : ship.shield_points);
  int total = trunc_to_int(
      static_cast<float>(ship_class != nullptr ? ship_class->strength : 0) *
      shield_ratio);

  float candidate_ratio = shield_ratio;
  for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
    const Ship &candidate = state.ShipAt(slot);
    if (static_cast<std::int16_t>(slot) == ship.ship_instance_id ||
        !candidate.is_active || NovaAiShip_IsDestroyed(candidate)) {
      continue;
    }
    const ShipClass *candidate_class = state.scenario.Ship(
        static_cast<std::int16_t>(candidate.ship_class_id + 0x80));
    const int candidate_strength =
        candidate_class != nullptr ? candidate_class->strength : 0;
    int support = 0;
    if (candidate.squad_leader_ship_slot == ship.ship_instance_id) {
      support = candidate_strength;
    } else if (ship.ship_instance_id == 0) {
      if (ship_class != nullptr &&
          NovaGovernment_AreGovtsAllied(state.scenario,
                                        ship_class->inherent_combat_govt,
                                        candidate.faction_or_government_id)) {
        support = candidate_strength;
      }
    } else if (NovaGovernment_AreGovtsAllied(
                   state.scenario,
                   ship.faction_or_government_id,
                   candidate.faction_or_government_id)) {
      support = candidate_strength;
    }
    // The original reuses one scratch across the base and candidate ratios; a
    // candidate with a non-positive max shield keeps the previous scratch.
    const float candidate_max_shield =
        static_cast<float>(NovaAi_ComputeMaxShieldPoints(state, candidate));
    if (candidate_max_shield > 0.0F) {
      candidate_ratio = candidate.shield_points / candidate_max_shield;
    }
    candidate_ratio = clamp_ratio(candidate_ratio);
    if (support > 0 &&
        NovaAiShip_IsThreatenedByEnemyOfShip(state, candidate, ship)) {
      // The original doubles support in a 16-bit short (ADD EDI,EDI then
      // MOVSX at 0x00411a25) and sign-extends the running total's low 16 bits
      // before each add (MOVSX EAX,DI at 0x00411a1a).
      support = static_cast<std::int16_t>(support * 2);
    }
    const auto total_short = static_cast<std::int16_t>(total);
    total = trunc_to_int(static_cast<float>(total_short) +
                         static_cast<float>(support) * candidate_ratio);
  }
  return total;
}

// Ghidra 0x0040e020 Ship_AcquirePrimaryTargetForShip. Full NPC primary-target
// acquisition. In order: the retention gate for an active engagement; the
// 0x3fe forced-hostility / pers grudge arms; the mission-fleet ShipBehav 0/1
// arms; the ammo-readiness early return; the non-xenophobic behavior<5 ally
// join; then the government target passes and common tail -- the near-player
// + reputation legal-record gate (Flags 0x0002 nosy, 0x0040 never-attacks,
// doubled/1.5x CrimeTol), the 1-in-50 inherent-combat-govt roll, the
// xenophobic (Flags 0x0001) aggressive scan, the IFF-scrambler/policy player
// shield, the MaxOdds perceived-strength filter, the squadron-leader Mass
// preference, and the nearest-acquirable fallback; finally the behavior-6
// escort re-selection. The original sets ai_state 4 only in the ally join and
// behavior-6 paths (and via SetShipHostileToPlayer); the behavior supervisors
// promote the common-tail/fallback result.
// TODO(decomp(0x0040e020)) skipped: the leading five-pair license-seed
// integrity check (see the first comment inside the function).
void NovaAi_AcquirePrimaryTarget(GameState &state, Ship &ship) {
  // TODO(decomp(0x0040e020)) skipped: the leading five-pair license-seed
  // integrity check on g_ship_states[5].license_seed. The original calls
  // Ship_SetShipHostileToPlayer when any stored pair-comparison boolean is
  // FALSE (a matching pair), not on an arbitrary mismatch; the purpose is
  // provisional. The clean-room model has no license-seed field.
  //
  // 0x0040e149 early retention: keep an existing primary target while the ship
  // is in an attack state (3/4) and the target slot is still active. The
  // original has NO same-system and NO destroyed check in this gate.
  if (ship.primary_target_ship_slot >= 0 &&
      state.SlotInRange(
          static_cast<std::size_t>(ship.primary_target_ship_slot)) &&
      (ship.ai_state_code == 3 || ship.ai_state_code == 4) &&
      state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot))
          .is_active) {
    return;
  }

  // Personality overrides at 0x0040e1d0: the sentinel ambusher always turns
  // hostile; other Flags-1 personalities do so after their persistent grudge
  // latch has been set by a player weapon hit, subject to cloak visibility.
  if (ship.pers_def_slot >= 0 && static_cast<std::size_t>(ship.pers_def_slot) <
                                     state.scenario.pers_defs.size()) {
    if (ship.pers_def_slot == 0x3fe) {
      NovaAi_SetShipHostileToPlayer(state, ship);
      return;
    }
    const PersDef &pers =
        state.scenario.pers_defs[static_cast<std::size_t>(ship.pers_def_slot)];
    if ((static_cast<std::uint16_t>(pers.flags_primary) & 0x0001U) != 0U &&
        pers.grudge &&
        NovaAiShip_CanEngageTargetUnderCloakRules(state, state.player, ship)) {
      NovaAi_SetShipHostileToPlayer(state, ship);
      return;
    }
  }

  // 0x0040e202 active mission-fleet arms. Goal 0 forces hostility to the
  // player (when the player is engageable under the cloak rules); goal 1 drops
  // a player primary, hands off to the random-combat-candidate selector, and
  // otherwise parks in state 0x0c with the player as secondary.
  if (ship.mission_fleet_slot != -1 &&
      static_cast<std::size_t>(ship.mission_fleet_slot) <
          state.active_missions.size() &&
      state
          .active_mission_runtime_flags[static_cast<std::size_t>(
              ship.mission_fleet_slot)]
          .is_active) {
    const ActiveMission &mission =
        state
            .active_missions[static_cast<std::size_t>(ship.mission_fleet_slot)];
    if (mission.ship_behavior == 0 &&
        NovaAiShip_CanEngageTargetUnderCloakRules(state, state.player, ship)) {
      NovaAi_SetShipHostileToPlayer(state, ship);
      return;
    }
    if (mission.ship_behavior == 1) {
      if (ship.primary_target_ship_slot == 0) {
        ship.primary_target_ship_slot = -1;
      }
      if (ship.primary_target_ship_slot == -1) {
        NovaAi_EnterState4TargetRandomCombatCandidate(state, ship);
        if (ship.primary_target_ship_slot == -1) {
          ship.ai_state_code = 0xc;
          ship.ai_secondary_target_slot = 0;
        }
      }
      return;
    }
  }

  // 0x0040e2c0: a ship with no ready weapon (readiness bucket 2) never
  // acquires a target.
  if (NovaWeapon_ClassifyAmmoReadiness(state, ship) == 2) {
    return;
  }

  const Government *ship_govt =
      ship.faction_or_government_id >= 0
          ? state.scenario.GovernmentByIndex(ship.faction_or_government_id)
          : nullptr;

  // 0x0040e3c0: a non-xenophobic behavior-1..4 ship joins an allied ship's
  // current fight when the ally's target is engageable and its perceived
  // strength fits this government's MaxOdds. The executable deliberately
  // does not require the allied ship to be in the same system.
  if (ship_govt != nullptr && (ship_govt->flags_primary & 0x0001U) == 0U &&
      ship.ai_behavior_code < 5) {
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      const Ship &ally = state.ShipAt(slot);
      if (static_cast<std::int16_t>(slot) == ship.ship_instance_id ||
          !ally.is_active || ally.faction_or_government_id < 0 ||
          ally.primary_target_ship_slot < 0 ||
          !state.SlotInRange(
              static_cast<std::size_t>(ally.primary_target_ship_slot)) ||
          (ally.ai_state_code != 3 && ally.ai_state_code != 4) ||
          !NovaGovernment_AreGovtsAllied(state.scenario,
                                         ship.faction_or_government_id,
                                         ally.faction_or_government_id)) {
        continue;
      }
      const Ship &ally_target =
          state.ShipAt(static_cast<std::size_t>(ally.primary_target_ship_slot));
      if ((ally.primary_target_ship_slot == 0 ||
           ally_target.squad_leader_ship_slot == 0) &&
          NovaGovernment_GetPolicyFlag(
              state.scenario, ship.faction_or_government_id, 0)) {
        continue;
      }
      if (!NovaAiShip_CanEngageTargetUnderCloakRules(
              state, ally_target, ship)) {
        continue;
      }
      const int target_strength =
          NovaAiShip_ComputePerceivedCombatStrength(state, ally_target);
      const int own_strength =
          NovaAiShip_ComputePerceivedCombatStrength(state, ship);
      if (static_cast<float>(target_strength) >
          static_cast<float>(own_strength) * ship_govt->max_odds) {
        continue;
      }
      ship.primary_target_ship_slot = ally.primary_target_ship_slot;
      ship.ai_state_code = 4;
      return;
    }
  }

  // Ghidra 0x0040e2d8 Weapon_HasAnyFireableNonSecondaryWeapon. Captured here
  // because the common-tail candidate gate admits a disabled target when the
  // attacker has any fireable non-secondary weapon.
  const bool has_fireable_weapon =
      NovaWeapon_HasAnyFireableNonSecondaryWeapon(state, ship);

  // Ghidra 0x0040e020 government target passes. `flagged` is the per-slot
  // candidate array (the original's local_70); the strength and
  // squared-distance scratch arrays are populated only for flagged slots and
  // are shared by the aggressive scan, the common tail and the fallback,
  // exactly as the original reuses its stack arrays.
  if (ship_govt != nullptr) {
    const Government &govt = *ship_govt;
    const std::uint16_t govt_flags = govt.flags_primary;
    const float max_odds = govt.max_odds;
    const std::int16_t faction = ship.faction_or_government_id;
    // The legal-record and system-government tests use the PLAYER's current
    // system, not the ship's: the original reads g_ship_states[0]
    // (0x0040e4f2/0x0040e4f8 in the reputation gate, 0x0040e909 in the
    // xenophobic player arm) and indexes g_system_reputation by that id.
    const std::int16_t player_system_id = state.player.current_system_id;
    const System *player_system = state.scenario.System(
        static_cast<std::int16_t>(player_system_id + 0x80));
    const std::int16_t system_govt =
        player_system != nullptr ? player_system->government_id : -1;
    const std::int16_t system_rep =
        player_system_id >= 0 && static_cast<std::size_t>(player_system_id) <
                                     state.system_reputation.size()
            ? state
                  .system_reputation[static_cast<std::size_t>(player_system_id)]
            : 0;
    // Government Flags 0x0002 (Bible): attacks the player in non-allied
    // systems if he is a criminal there; the original doubles CrimeTol for it.
    const bool nosy = (govt_flags & 0x0002U) != 0U;

    std::array<bool, GameState::kMaxShips> flagged{};
    std::array<std::int16_t, GameState::kMaxShips> candidate_strength{};
    std::array<std::int32_t, GameState::kMaxShips> candidate_dist{};

    if ((govt_flags & 0x0001U) == 0U) {
      // ---- non-xenophobic government (0x0040e3ee..0x0040e61f) ----
      // Near-player gate: within random_ai_render_cadence * 600 on both axes
      // (the product wraps through a short like the original), not policy
      // suppressed, engageable and alive.
      const std::int16_t near_radius =
          static_cast<std::int16_t>(ship.random_ai_render_cadence * 600);
      const bool player_near =
          !NovaGovernment_GetPolicyFlag(state.scenario, faction, 0) &&
          std::fabs(ship.pos_x - state.player.pos_x) <=
              static_cast<float>(near_radius) &&
          std::fabs(ship.pos_y - state.player.pos_y) <=
              static_cast<float>(near_radius) &&
          NovaAiShip_CanEngageTargetUnderCloakRules(
              state, state.player, ship) &&
          !NovaAiShip_IsDestroyed(state.player);

      // Reputation gate: the legal-record ladder. Government Flags 0x0040
      // (Bible: never attacks player) blocks it outright. The threshold
      // depends on the relationship between the ship's government and the
      // system government; the 0x0002 "nosy" path doubles CrimeTol, and the
      // allied path uses the undocumented 1.5x CrimeTol.
      if (player_near && (govt_flags & 0x0040U) == 0U &&
          NovaAiShip_CanEngageTargetUnderCloakRules(
              state, state.player, ship)) {
        if (faction == system_govt) {
          if (static_cast<int>(system_rep) + govt.crime_tol < 0) {
            flagged[0] = true;
          }
        } else if (system_govt < 0) {
          if (nosy && static_cast<int>(system_rep) + 2 * govt.crime_tol < 0) {
            flagged[0] = true;
          }
        } else if (NovaGovernment_AreGovtsHostileOrXenophobic(
                       state.scenario, faction, system_govt)) {
          if (govt.crime_tol < system_rep) {
            flagged[0] = true;
          }
        } else if (!NovaGovernment_AreGovtsAllied(
                       state.scenario, faction, system_govt)) {
          if (nosy && static_cast<int>(system_rep) + 2 * govt.crime_tol < 0) {
            flagged[0] = true;
          }
        } else if (static_cast<float>(system_rep) <
                   -static_cast<float>(govt.crime_tol) * 1.5F) {
          flagged[0] = true;
        }
      }

      // Inherent-combat-government roll (0x0040e57f): an idle government
      // warship has a 1-in-50 chance to flag the player when the PLAYER's
      // current hull carries an inherent combat govt hostile to the ship's
      // government. The original reads g_ship_class_defs[g_ship_states->
      // ship_class_id] -- slot 0, the player's hull -- as the player's combat
      // identity (the same convention as
      // Government_IsCandidateHostileToTargeter 0x004629e0 and
      // Ui_InstallGameplayInterfaceLayout 0x004cda50).
      const ShipClass *player_class = state.scenario.Ship(
          static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
      if (ship.squad_leader_ship_slot != 0 && faction != -1 &&
          ship.mission_fleet_slot == -1 && ship.mission_owner_slot == -1 &&
          player_class != nullptr && player_class->inherent_combat_govt != -1 &&
          NovaGovernment_AreGovtsHostileOrXenophobic(
              state.scenario, faction, player_class->inherent_combat_govt) &&
          RandomBelow(state, 0x32) == 0 &&
          !NovaAiShip_IsDestroyed(state.player) &&
          NovaAiShip_CanEngageTargetUnderCloakRules(
              state, state.player, ship)) {
        flagged[0] = true;
      }
    } else {
      // ---- xenophobic government aggressive scan (0x0040e710) ----
      int count = 0;
      for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
        const Ship &candidate = state.ShipAt(slot);
        if (NovaAiShip_IsDestroyed(candidate) ||
            NovaAiShip_IsDisabled(state, candidate) ||
            !NovaAiShip_CanEngageTargetUnderCloakRules(
                state, candidate, ship) ||
            candidate.ship_class_id == 0x2ff) {
          continue;
        }
        if (slot == 0) {
          // Player special case: aggressive governments flag the player when
          // the local record is not good (< 1) and policy/IFF permits.
          if ((govt_flags & 0x0040U) == 0U &&
              NovaAiShip_CanEngageTargetUnderCloakRules(
                  state, state.player, ship) &&
              !NovaGovernment_GetPolicyFlag(state.scenario, faction, 0) &&
              (faction != system_govt || system_rep < 1)) {
            ++count;
            flagged[0] = true;
          }
          continue;
        }
        if (static_cast<std::int16_t>(slot) == ship.ship_instance_id ||
            !candidate.is_active ||
            ship.ship_instance_id == candidate.squad_leader_ship_slot ||
            ship.current_system_id != candidate.current_system_id ||
            candidate.defense_fleet_home_stellar_id != -1 ||
            candidate.pers_def_slot == 0x3ff ||
            candidate.ship_class_id == 0x2ff ||
            NovaGovernment_AreGovtsAllied(
                state.scenario, faction, candidate.faction_or_government_id) ||
            !NovaAiShip_IsEnemyOfShip(state, ship, candidate)) {
          continue;
        }
        ++count;
        flagged[slot] = true;
      }

      if (count > 0) {
        for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
          if (!flagged[slot]) {
            continue;
          }
          candidate_strength[slot] = static_cast<std::int16_t>(
              NovaAiShip_ComputePerceivedCombatStrength(state,
                                                        state.ShipAt(slot)));
          candidate_dist[slot] =
              RoundedAxisDistanceSquared(ship.pos_x,
                                         ship.pos_y,
                                         state.ShipAt(slot).pos_x,
                                         state.ShipAt(slot).pos_y);
        }
        const auto own_strength = static_cast<std::int16_t>(
            NovaAiShip_ComputePerceivedCombatStrength(state, ship));
        for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
          if (flagged[slot] &&
              static_cast<float>(own_strength) * max_odds <
                  static_cast<float>(candidate_strength[slot])) {
            flagged[slot] = false;
            --count;
          }
        }
      }
      if (count > 0) {
        std::int32_t best_dist = -1;
        for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
          if (flagged[slot] &&
              (best_dist < 0 || candidate_dist[slot] < best_dist)) {
            ship.primary_target_ship_slot = static_cast<std::int16_t>(slot);
            best_dist = candidate_dist[slot];
          }
        }
        if (ship.primary_target_ship_slot == 0) {
          NovaAi_SetShipHostileToPlayer(state, ship);
        }
      }
    }

    // 0x0040ece3 IFF-scrambler / policy player shield: clear a flagged player
    // before the common tail.
    if (govt.iff_scrambler_active ||
        NovaGovernment_GetPolicyFlag(state.scenario, faction, 0)) {
      flagged[0] = false;
    }

    // ---- common tail (0x0040eea0) ----
    int count = flagged[0] ? 1 : 0;
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      const Ship &candidate = state.ShipAt(slot);
      if (static_cast<std::int16_t>(slot) != ship.ship_instance_id &&
          NovaAiShip_CanEngageTargetUnderCloakRules(state, candidate, ship) &&
          candidate.is_active &&
          (has_fireable_weapon || !NovaAiShip_IsDisabled(state, candidate)) &&
          NovaAiShip_IsEnemyOfShip(state, ship, candidate)) {
        ++count;
        flagged[slot] = true;
      }
    }

    if (count > 0) {
      for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
        if (!flagged[slot]) {
          continue;
        }
        candidate_strength[slot] =
            static_cast<std::int16_t>(NovaAiShip_ComputePerceivedCombatStrength(
                state, state.ShipAt(slot)));
        candidate_dist[slot] =
            RoundedAxisDistanceSquared(ship.pos_x,
                                       ship.pos_y,
                                       state.ShipAt(slot).pos_x,
                                       state.ShipAt(slot).pos_y);
      }
      const auto own_strength = static_cast<std::int16_t>(
          NovaAiShip_ComputePerceivedCombatStrength(state, ship));
      // A squadron leader prefers the heaviest valid enemy (maximum Mass
      // among the candidates that pass MaxOdds), ties broken by distance.
      std::int32_t mass_threshold = -1;
      for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
        if (!flagged[slot]) {
          continue;
        }
        if (static_cast<float>(own_strength) * max_odds <
            static_cast<float>(candidate_strength[slot])) {
          --count;
          flagged[slot] = false;
          continue;
        }
        const ShipClass *cls = state.scenario.Ship(
            static_cast<std::int16_t>(state.ShipAt(slot).ship_class_id + 0x80));
        const std::int32_t mass = cls != nullptr ? cls->mass_tons : 0;
        if (ship.is_any_ships_squad_leader &&
            (mass_threshold < 0 || mass_threshold < mass)) {
          mass_threshold = mass;
        }
      }

      if (count > 0) {
        std::int32_t best_dist = -1;
        for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
          if (!flagged[slot]) {
            continue;
          }
          if (mass_threshold >= 0) {
            const ShipClass *cls =
                state.scenario.Ship(static_cast<std::int16_t>(
                    state.ShipAt(slot).ship_class_id + 0x80));
            const std::int32_t mass = cls != nullptr ? cls->mass_tons : 0;
            if (mass != mass_threshold) {
              continue;
            }
          }
          if (best_dist < 0 || candidate_dist[slot] < best_dist) {
            ship.primary_target_ship_slot = static_cast<std::int16_t>(slot);
            best_dist = candidate_dist[slot];
          }
        }
        if (ship.primary_target_ship_slot == 0) {
          NovaAi_SetShipHostileToPlayer(state, ship);
        }
      }
    }

    // Fallback (0x0040f1c8): no primary yet, take the nearest acquirable
    // target. The original passes (ship, candidate) to
    // Ship_IsShipAcquirableAsTarget, i.e. `ship` as the candidate and the
    // scanned slot as the acquirer -- the same ordering as the behavior-6
    // re-selection below. The original sets only the primary slot here; the
    // supervisor promotes to state 4.
    if (ship.primary_target_ship_slot == -1) {
      std::int32_t best_dist = -1;
      for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
        if (static_cast<std::int16_t>(slot) == ship.ship_instance_id) {
          continue;
        }
        const Ship &candidate = state.ShipAt(slot);
        if (!candidate.is_active ||
            !NovaAiShip_CanEngageTargetUnderCloakRules(
                state, candidate, ship) ||
            !NovaTargeting_IsShipAcquirableAsTarget(state, ship, candidate)) {
          continue;
        }
        if (!flagged[slot]) {
          candidate_dist[slot] = RoundedAxisDistanceSquared(
              ship.pos_x, ship.pos_y, candidate.pos_x, candidate.pos_y);
        }
        if (best_dist < 0 || candidate_dist[slot] < best_dist) {
          best_dist = candidate_dist[slot];
          ship.primary_target_ship_slot = static_cast<std::int16_t>(slot);
        }
      }
    }
  }

  // 0x0040f293 behavior-6 escort re-selection: with no primary target, pick
  // the nearest same-system ship that is acquirable as a target, excluding
  // self and the squad leader. The predicate call keeps the original's order
  // Ship_IsShipAcquirableAsTarget(ship, candidate): its first parameter is the
  // candidate-to-be-acquired, so `ship` is the candidate here and the scanned
  // slot is the acquirer. The original's final loop reads the distance array
  // for every slot although only scanned candidates populate it (an
  // uninitialised-stack quirk); the port selects among scanned candidates.
  // The port also bounds-checks squad_leader_ship_slot where the original
  // indexes g_ship_states[-1] when the leader is unset.
  if (ship.ai_behavior_code == 6 && ship.primary_target_ship_slot == -1) {
    std::int16_t escort_best = -1;
    std::int32_t escort_best_dist = -1;
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      const Ship &candidate = state.ShipAt(slot);
      if (!candidate.is_active ||
          static_cast<std::int16_t>(slot) == ship.ship_instance_id) {
        continue;
      }
      if (ship.squad_leader_ship_slot >= 0 &&
          state.SlotInRange(
              static_cast<std::size_t>(ship.squad_leader_ship_slot)) &&
          candidate.ship_instance_id ==
              state
                  .ShipAt(static_cast<std::size_t>(ship.squad_leader_ship_slot))
                  .ship_instance_id) {
        continue;
      }
      if (!NovaTargeting_IsShipAcquirableAsTarget(state, ship, candidate) ||
          ship.current_system_id != candidate.current_system_id) {
        continue;
      }
      const std::int32_t distance_sq = RoundedAxisDistanceSquared(
          ship.pos_x, ship.pos_y, candidate.pos_x, candidate.pos_y);
      if (escort_best_dist < 0 || distance_sq < escort_best_dist) {
        escort_best_dist = distance_sq;
        escort_best = static_cast<std::int16_t>(slot);
      }
    }
    if (escort_best != -1) {
      ship.primary_target_ship_slot = escort_best;
      ship.ai_state_code = 4;
    }
  }
}

// Shared travel fallback used by behavior 0x02/0x03. This is the common
// `state 0 -> adjacent stellar -> state 1/2/6` ladder visible in both Ghidra
// supervisors; mission and special-loadout branches remain deferred.
void NovaAi_ReacquireTravelOrSettle(GameState &state, Ship &ship) {
  const System *sys = CurrentSystem(state, ship);
  const bool still_at_point = sys && ship.jump_destination_stellar_id >= 0 &&
                              NovaTargeting_IsStellarAdjacentToSystem(
                                  *sys, ship.jump_destination_stellar_id);
  if (!still_at_point) {
    ship.ai_secondary_target_slot = -1;
    ship.ai_secondary_target_slot = NovaAi_SelectRandomAdjacentTravelStellar(
        state, ship, /*strict_mode=*/false, /*unrestricted_only=*/false);
  }
  if (ship.ai_secondary_target_slot == -1) {
    if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
      NovaAi_EnterState2ClearPrimaryTarget(state, ship);
    } else {
      ship.ai_state_code = 6;
    }
  } else if (!still_at_point) {
    ship.travel_transfer_mode = 2;
    ship.ai_state_code = 1;
  } else if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
    NovaAi_EnterState2ClearPrimaryTarget(state, ship);
  } else {
    ship.ai_state_code = 6;
  }
}

// Ghidra 0x004112C0 Ship_ShowPlayerInterceptTauntIfEligible. Generic
// (non-personality) ships entering intercept state may challenge the player
// when no HUD message is already visible. The stock challenges are the 20
// entries in STR# 0x138b; transition-table cue 4 accompanies the message.
void NovaAi_ShowPlayerInterceptTauntIfEligible(GameState &state, Ship &ship) {
  if (ship.pers_def_slot != -1 || state.hud_overlay.active) {
    return;
  }
  const auto govt_suppresses = [&state](std::int16_t govt_index) {
    const Government *govt = state.scenario.GovernmentByIndex(govt_index);
    return govt != nullptr && (govt->flags_secondary & 0x08U) != 0U;
  };
  if (govt_suppresses(ship.faction_or_government_id)) {
    return;
  }
  const ShipClass *ship_class =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (ship_class == nullptr ||
      govt_suppresses(ship_class->inherent_attributes_govt) ||
      !NovaShip_DoesShipLikePlayer(state, ship) ||
      NovaTargeting_IsShipAcquirableAsTarget(state, ship, state.player) ||
      NovaAiShip_IsDestroyed(state.player) ||
      NovaTargeting_ShipAtCloakVisibilityThreshold(ship)) {
    return;
  }

  std::string speaker = ship_class->display_name;
  if (ship.mission_fleet_slot >= 0 &&
      static_cast<std::size_t>(ship.mission_fleet_slot) <
          state.active_missions.size() &&
      !state.active_missions[static_cast<std::size_t>(ship.mission_fleet_slot)]
           .mission_fleet_name.empty()) {
    speaker =
        state.active_missions[static_cast<std::size_t>(ship.mission_fleet_slot)]
            .mission_fleet_name;
  }
  const auto challenge_index =
      std::uniform_int_distribution<std::uint16_t>{1, 20}(state.rng);
  const std::string challenge = NovaHud_LoadStringEntry(0x138b, challenge_index)
                                    .value_or("Prepare to be destroyed!");
  NovaHud_ShowOverlayMessage(
      state, speaker + ":  " + challenge, 0xe0, 0xe0, 0xe0, 0xf0U);
  state.pending_ui_sounds.push_back({4, 1});
}

// Ghidra 0x00402860 Ship_UpdateShipAiBehavior0x01_WimpyTrader. The "normal
// travel / wander" supervisor. Reacquires a travel stellar when idle (state 0)
// -- picking a random adjacent travel stellar and entering state 1 (travel to
// it), else falling back to state 2 via NovaAi_EnterState2ClearPrimaryTarget
// (or state 6 when no jump route exists). Escalates into hostile attack (state
// 3, or state 10 against the player) once a hostility accumulator + primary
// target exist.
void NovaAi_UpdateBehavior0x01(GameState &state,
                               Ship &ship,
                               std::uint32_t now_ms) {
  if (NovaAiShip_IsDisabled(state, ship)) {
    return;
  }
  const std::int16_t st = ship.ai_state_code;
  if (st == 9 || st == 0xf || st == 0x16) {
    // Combat/disabled/capture-handled elsewhere.
    return;
  }
  if (st == 0) {
    // Idle: either sitting at a valid travel point or needing a new one.
    // Stellar_IsStellarAdjacentToCurrentSystem checks whether the ship's
    // recorded jump destination (jump_destination_stellar_id) is still one of
    // the current system's nav points. If it is, the ship is parked/at a
    // valid stellar and tries to jump away; if not, it picks a fresh random
    // adjacent travel stellar to wander toward.
    const System *sys = CurrentSystem(state, ship);
    const bool still_at_point = sys && ship.jump_destination_stellar_id >= 0 &&
                                NovaTargeting_IsStellarAdjacentToSystem(
                                    *sys, ship.jump_destination_stellar_id);
    if (still_at_point) {
      // Already at a valid travel stellar: settle / try to jump away. The jump
      // gate uses THIS ship's class fuel (NovaTravel_CanShipInitiateJump-
      // Sequence), not the player's.
      if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
        NovaAi_EnterState2ClearPrimaryTarget(state, ship);
      } else {
        ship.ai_state_code = 6;
      }
    } else {
      // Pick a fresh travel destination to wander toward.
      ship.ai_secondary_target_slot = -1;
      const std::int16_t travel = NovaAi_SelectRandomAdjacentTravelStellar(
          state, ship, /*strict_mode=*/false, /*unrestricted_only=*/false);
      ship.ai_secondary_target_slot = travel;
      if (ship.ai_secondary_target_slot == -1) {
        // No route remains: try to jump, else settle into idle-template.
        if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
          NovaAi_EnterState2ClearPrimaryTarget(state, ship);
        } else {
          ship.ai_state_code = 6;
        }
      } else {
        ship.travel_transfer_mode = 2;
        ship.ai_state_code = 1;
      }
    }
  }
  // Hostility escalation: once the ship feels threatened and has a target.
  if (ship.ai_hostility_accumulator > 0 &&
      ship.primary_target_ship_slot != -1) {
    if (ship.squad_leader_ship_slot == 0) {
      ship.ai_state_code = 10;
      ship.ai_secondary_target_slot = 0;
    } else {
      ship.ai_state_code = 3;
    }
  }
  if (ship.ai_state_code == 3) {
    NovaAi_ShowPlayerInterceptTauntIfEligible(state, ship);
  }
  (void)now_ms;
}

namespace {

// g_has_active_freeflight_objects: true while any freeflight pool slot is live
// (lifetime_ticks >= 0; < 0 is the inactive sentinel). Consumed by the mining
// supervisor's debris-scoop decision.
[[nodiscard]] bool HasActiveFreeflightObjects(const GameState &state) {
  for (const FreeflightObjectState &object : state.freeflight_objects) {
    if (object.lifetime_ticks >= 0.0F) {
      return true;
    }
  }
  return false;
}

} // namespace

// Ghidra 0x00402980 Ship_UpdateShipAiAsteroidMinerBehavior. Supervisor for
// ships whose class Flags3 has bit 0x1 (Bible "ship destroys asteroids") or
// 0x2 ("ship scoops asteroid debris"). The dispatcher runs it in preference
// to the normal ai_behavior_code branch when Flags3 & 3 and the ship has no
// squad leader. Arms the scripted asteroid manoeuvre (state 0x10), the
// freeflight-anchor cargo pick-up (state 0x11), or the nearest-adjacent-
// travel-stellar wander ladder; a hostile contact escalates to state 3.
void NovaAi_UpdateAsteroidMinerBehavior(GameState &state, Ship &ship) {
  if (NovaAiShip_IsDisabled(state, ship)) {
    ship.ai_state_code = 0;
    ship.ai_control_mode = 0;
    ship.ai_secondary_target_slot = -1;
    ship.primary_target_ship_slot = -1;
    return;
  }
  if (ship.ai_state_code == 0x16) {
    return;
  }

  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const bool destroys_asteroids =
      cls != nullptr && (cls->flags3 & ShipClass::kDestroysAsteroids) != 0U;
  const bool scoops_debris =
      cls != nullptr && (cls->flags3 & ShipClass::kScoopsAsteroidDebris) != 0U;

  if (ship.ai_hostility_accumulator < 1 ||
      ship.primary_target_ship_slot == -1) {
    bool wander = false;
    if (!destroys_asteroids && !scoops_debris) {
      wander = true;
    }
    if (destroys_asteroids && ship.ai_state_code != 2) {
      if (state.asteroid_pool[0].active) {
        ship.ai_state_code = 0x10;
      } else {
        ship.primary_target_ship_slot = -1;
        ship.ai_secondary_target_slot = -1;
        ship.ai_state_code = 6;
      }
    }
    if (scoops_debris) {
      if (!ship.mining_scoop_active) {
        wander = true;
      } else {
        // Original sums the ship's own 6 cargo bins (the player branch's
        // mission/junk terms are skipped for instance != 0). The clean-room
        // Ship has no per-NPC cargo model, so this is 0 and a newly spawned
        // miner always passes. TODO(decomp): model per-NPC cargo so a full
        // miner stops scooping.
        const int cargo_total = 0;
        const int cargo_capacity = cls != nullptr ? cls->cargo_holds : 0;
        if (cargo_total < cargo_capacity) {
          if (!HasActiveFreeflightObjects(state)) {
            if (destroys_asteroids) {
              ship.ai_state_code = 0x10;
            } else {
              wander = true;
            }
          } else {
            ship.ai_state_code = 0x11;
          }
        } else {
          NovaAi_EnterState2ClearPrimaryTarget(state, ship);
        }
      }
    }
    if (wander) {
      if (ship.ai_maneuver_timer_ms > 0.0F) {
        ship.ai_secondary_target_slot = -1;
        ship.ai_state_code = 0;
      } else {
        if (ship.ai_secondary_target_slot == -1) {
          ship.ai_secondary_target_slot =
              NovaAi_FindNearestAdjacentTravelStellar(
                  state, ship, ship.jump_destination_stellar_id);
        }
        if (ship.ai_secondary_target_slot == -1 || ship.ai_state_code == 2 ||
            ship.ai_state_code == 3) {
          if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
            NovaAi_EnterState2ClearPrimaryTarget(state, ship);
          } else {
            ship.ai_state_code = 6;
          }
        } else {
          ship.ai_state_code = 1;
          ship.jump_destination_stellar_id = ship.ai_secondary_target_slot;
        }
      }
    }
  } else {
    ship.ai_state_code = 3;
  }

  if (ship.ai_state_code == 3) {
    NovaAi_ShowPlayerInterceptTauntIfEligible(state, ship);
  }
}

// Ghidra 0x00405120 Ship_DefenseFleetPrioritizePlayerThreat. Per-frame
// supervisor run by Ship_UpdateShipAI (0x00401000) INSTEAD of a behavior
// supervisor when the ship holds a stellar assignment
// (defense_fleet_home_stellar_id != -1). In the original that field is only
// ever set to a real stellar by Stellar_SpawnDefenseFleetShip (0x00421fd0),
// which garrisons a ship AT a stellar, gives it ai_behavior_code 3 and
// Ship_SetShipHostileToPlayer -- the Bible's stellar defense fleet (spöb
// DefenseDude / DefCount). So this is the defense fleet's player-threat
// override, not something every NPC runs. It
// searches for the nearest player-side ship -- slot 0 (the player) or an active
// ship whose squad_leader_ship_slot == 0 (a player escort) -- that may be
// engaged under the cloak rules, preferring candidates within a
// squared-distance bound DAT_00575060 = 36,000,000 (= 6000^2) on the first pass
// and falling back to the nearest anywhere on a second pass. If the ship
// already holds a primary target the search result is ignored unless that
// primary is an NPC attached to a non-player leader, in which case the best
// player-side candidate replaces it. With no primary it either locks the best
// candidate (AI state 4) or, when none exists, returns to its stellar
// (AI state 1 with ai_secondary_target_slot = defense_fleet_home_stellar_id).
// Search distance is Math_SquaredDistance (Euclidean squared) truncated toward
// zero.
void NovaAi_DefenseFleetPrioritizePlayerThreat(GameState &state, Ship &ship) {
  if (NovaAiShip_IsDestroyed(ship) || ship.ai_state_code == 0x16) {
    return;
  }

  // DAT_00575060 (0x00575060) = 36000000.0f = 6000^2.
  constexpr float kPlayerThreatRangeSq = 36000000.0F;

  std::int16_t best_slot = -1;
  int best_distance = -1;
  // Two passes: the first keeps only candidates within 6000 px (the original
  // still rejects unordered/greater distances), the second accepts any.
  for (int pass = 0; pass < 2 && best_slot == -1; ++pass) {
    for (std::size_t i = 0; i < GameState::kMaxShips; ++i) {
      const std::int16_t slot = static_cast<std::int16_t>(i);
      const Ship &candidate = state.ShipAt(i);
      if (!candidate.is_active) {
        continue;
      }
      // Only the player (slot 0) or a ship attached to the player qualifies.
      if (slot != 0 && candidate.squad_leader_ship_slot != 0) {
        continue;
      }
      if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, candidate, ship)) {
        continue;
      }
      const float distance_sq = SquaredDistance(
          ship.pos_x, ship.pos_y, candidate.pos_x, candidate.pos_y);
      if (pass == 0 && !(distance_sq <= kPlayerThreatRangeSq)) {
        continue;
      }
      // Strictly closer wins; the first candidate wins ties. The negated
      // comparison also rejects an unordered (NaN) distance, matching the
      // original FCOMP flags.
      if (best_slot != -1 &&
          !(distance_sq < static_cast<float>(best_distance))) {
        continue;
      }
      best_distance = static_cast<int>(distance_sq);
      best_slot = slot;
    }
  }

  ship.ai_maneuver_timer_ms = 0.0F;
  ship.ai_secondary_target_slot = -1;

  // The original indexes the candidate pool with the exhausted search-loop
  // counter (index 0x40, one past the heap-allocated 64-ship array) in this
  // revalidation -- an out-of-bounds read with no defined value. The port
  // instead revalidates the ship's existing primary target, the evident
  // intent (clear a primary that can no longer be engaged under cloak rules).
  // TODO(decomp(0x00405120)) divergence: original reads ships[0x40] here.
  if (ship.primary_target_ship_slot != -1) {
    const std::size_t primary =
        static_cast<std::size_t>(ship.primary_target_ship_slot);
    if (!state.SlotInRange(primary) ||
        !NovaAiShip_CanEngageTargetUnderCloakRules(
            state, state.ShipAt(primary), ship)) {
      ship.primary_target_ship_slot = -1;
    }
  }

  if (ship.primary_target_ship_slot == -1) {
    if (best_slot == -1) {
      ship.ai_secondary_target_slot = ship.defense_fleet_home_stellar_id;
      ship.ai_state_code = 1;
    } else {
      ship.primary_target_ship_slot = best_slot;
      ship.ai_state_code = 4;
    }
  }

  const std::int16_t primary = ship.primary_target_ship_slot;
  if (primary != -1 && primary != 0 &&
      state.SlotInRange(static_cast<std::size_t>(primary)) &&
      state.ShipAt(static_cast<std::size_t>(primary)).squad_leader_ship_slot !=
          0) {
    if (best_slot == -1) {
      ship.primary_target_ship_slot = -1;
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
    } else {
      ship.primary_target_ship_slot = best_slot;
    }
  }
}

// Ghidra 0x00402bd0 Ship_UpdateShipAiBehavior0x02_BraveTrader. Local/dude
// behavior shares the travel fallback with behavior 0x01, but promotes an
// established hostile contact once it is within the original 0x4e3-pixel
// per-axis gate. The player-target arm enters state 10 (assist/response); other
// contacts enter state 3. Government chatter and assistance encounter side
// effects remain
// TODO(decomp).
void NovaAi_UpdateBehavior0x02(GameState &state, Ship &ship) {
  if (NovaAiShip_IsDisabled(state, ship)) {
    return;
  }
  if (ship.ai_state_code == 9 || ship.ai_state_code == 0xf ||
      ship.ai_state_code == 0x16) {
    return;
  }

  if (ship.ai_state_code == 0 && ship.primary_target_ship_slot == -1) {
    NovaAi_ReacquireTravelOrSettle(state, ship);
  }

  if (ship.ai_hostility_accumulator > 0 &&
      ship.primary_target_ship_slot != -1 &&
      state.SlotInRange(
          static_cast<std::size_t>(ship.primary_target_ship_slot))) {
    const Ship &target =
        state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot));
    const float dx = std::abs(ship.pos_x - target.pos_x);
    const float dy = std::abs(ship.pos_y - target.pos_y);
    if (dx < 1251.0F && dy < 1251.0F && ship.ai_station_hold_timer <= 0.0F) {
      ship.ai_state_code = 4;
    } else if (ship.squad_leader_ship_slot == 0) {
      ship.ai_state_code = 10;
      ship.ai_secondary_target_slot = 0;
    } else {
      ship.ai_state_code = 3;
    }
  }
  if (ship.ai_state_code == 3) {
    NovaAi_ShowPlayerInterceptTauntIfEligible(state, ship);
    (void)NovaGovernment_TryTriggerAssistanceEncounter(
        state, ship, /*force=*/false);
  } else if (ship.ai_state_code == 4 && ship.primary_target_ship_slot > 0 &&
             state.SlotInRange(
                 static_cast<std::size_t>(ship.primary_target_ship_slot)) &&
             state.ShipAt(
                      static_cast<std::size_t>(ship.primary_target_ship_slot))
                     .ai_behavior_code > 2) {
    NovaAi_ShowPlayerInterceptTauntIfEligible(state, ship);
  }
}

// Ghidra 0x00402e50 Ship_UpdateShipAiBehavior0x03_Warship. Hostile warship
// supervisor: government hold/aggression policy, idle target acquisition,
// target retention/loss, the state-1/0x14/2 travel re-acquire arm, the state-6
// attached-fighter wait, and the low-shield / cowardice / ammo-out disengage
// arms. The capture/plunder government variant (flags_primary 0x1000) is the
// separate 0x004038b0 port. The random_ai_render_cadence aggression thresholds
// remain provisional.
void NovaAi_UpdateBehavior0x03(GameState &state, Ship &ship) {
  if (NovaAiShip_IsDisabled(state, ship)) {
    return;
  }
  if (ship.ai_state_code == 9 || ship.ai_state_code == 0xf ||
      ship.ai_state_code == 0x16) {
    return;
  }

  const auto ship_government = [&state, &ship]() -> const Government * {
    return ship.faction_or_government_id == -1
               ? nullptr
               : state.scenario.GovernmentByIndex(
                     ship.faction_or_government_id);
  };

  // Government hold window [-900, 0]: a non-threat-policy government drops a
  // player-squad threat; a government with Flags 0x0004 instead turns hostile
  // to an engageable player.
  if (ship.faction_or_government_id != -1 &&
      ship.ai_station_hold_timer > -900.0F &&
      ship.ai_station_hold_timer <= 0.0F) {
    if (const Government *govt = ship_government(); govt != nullptr) {
      if ((govt->flags_primary & 0x0004U) == 0U) {
        if ((govt->flags_primary & 0x0040U) != 0U &&
            NovaTargeting_IsThreatToPlayerSquad(state, ship)) {
          ship.primary_target_ship_slot = -1;
          ship.ai_state_code = 0;
        }
      } else if (!NovaTargeting_IsThreatToPlayerSquad(state, ship) &&
                 NovaAiShip_CanEngageTargetUnderCloakRules(
                     state, state.player, ship)) {
        NovaAi_SetShipHostileToPlayer(state, ship);
        ship.primary_target_ship_slot = 0;
        ship.ai_hostility_accumulator = 1;
      }
    }
  }

  // A control-mode 4 "hold position" command forces the ship back into the
  // state-2 centre/standoff manoeuvre unless it is already jumping (2) or
  // retreating (0xb).
  if (ship.ai_control_mode == 4 && ship.ai_state_code != 2 &&
      ship.ai_state_code != 0xb) {
    ship.ai_state_code = 2;
  }

  // State 0: idle acquisition. A newly acquired contact promotes to attack;
  // without one, fall through to the travel/jump ladder. The original runs the
  // ladder twice; the second pass only re-runs when the first left no target
  // and is retained for fidelity.
  if (ship.ai_state_code == 0) {
    if (ship.primary_target_ship_slot == -1) {
      if (ship.ai_station_hold_timer <= 0.0F) {
        NovaAi_AcquirePrimaryTarget(state, ship);
        if (ship.primary_target_ship_slot == -1) {
          NovaAi_ReacquireTravelOrSettle(state, ship);
        } else {
          ship.ai_state_code = 4;
        }
        if (ship.primary_target_ship_slot == -1) {
          NovaAi_ReacquireTravelOrSettle(state, ship);
        }
      }
    } else if (ship.ai_station_hold_timer <= 0.0F) {
      ship.ai_state_code = 4;
    }
  }

  // Threat escalation: any live hostility with a target attacks unless already
  // retreating (3/7) or holding station.
  if (ship.ai_hostility_accumulator > 0 &&
      ship.primary_target_ship_slot != -1 && ship.ai_state_code != 7 &&
      ship.ai_state_code != 3 && ship.ai_station_hold_timer <= 0.0F) {
    ship.ai_state_code = 4;
  }

  // Travel states re-scan for a contact each tick: 1/0x14/2 (wandering /
  // jumping / standoff) promote to attack on acquisition, keeping an already
  // held target only while not holding station. The original also tests
  // state != 3 and != 7 here, which are unreachable for these states.
  const std::int16_t travel_state = ship.ai_state_code;
  if (travel_state == 1 || travel_state == 0x14 || travel_state == 2) {
    if (ship.primary_target_ship_slot == -1) {
      if (ship.ai_station_hold_timer <= 0.0F) {
        NovaAi_AcquirePrimaryTarget(state, ship);
        if (ship.primary_target_ship_slot != -1) {
          ship.ai_state_code = 4;
        }
      }
    } else if (ship.ai_station_hold_timer <= 0.0F) {
      ship.ai_state_code = 4;
    }
  }

  // State 6: no jump route / no fuel. Re-scan once; if still targetless, wait
  // while behavior-5 fighter attachments with no stellar home remain,
  // otherwise release to state 0. Without this arm a parked warship never
  // leaves state 6.
  if (ship.ai_state_code == 6) {
    ship.primary_target_ship_slot = -1;
    NovaAi_AcquirePrimaryTarget(state, ship);
    if (ship.primary_target_ship_slot == -1) {
      int attached_fighters = 0;
      for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
        const Ship &other = state.ShipAt(slot);
        if (other.squad_leader_ship_slot == ship.ship_instance_id &&
            other.is_active && other.ai_behavior_code == 5 &&
            other.defense_fleet_home_stellar_id == -1 &&
            !NovaAiShip_IsDisabled(state, other)) {
          ++attached_fighters;
        }
      }
      if (attached_fighters == 0) {
        ship.ai_state_code = 0;
      }
    }
  }

  // Loss of an attack target: inactive/destroyed, or disabled with no
  // fireable non-secondary weapon left.
  if (ship.primary_target_ship_slot != -1 && ship.ai_state_code == 4) {
    const auto target_slot =
        static_cast<std::size_t>(ship.primary_target_ship_slot);
    if (!state.SlotInRange(target_slot) ||
        !state.ShipAt(target_slot).is_active ||
        NovaAiShip_IsDestroyed(state.ShipAt(target_slot)) ||
        (NovaAiShip_IsDisabled(state, state.ShipAt(target_slot)) &&
         !NovaWeapon_HasAnyFireableNonSecondaryWeapon(state, ship))) {
      ship.ai_state_code = 0;
      ship.primary_target_ship_slot = -1;
    }
  }

  // Low-shield government retreat (Govt Flags 0x0010): once engaged and below
  // half shields, a fleet leader whose odds exceed MaxOdds disengages, with
  // the threshold doubled while this system's own allied reinforcements are
  // still on cooldown.
  if (ship.primary_target_ship_slot != -1 &&
      ship.faction_or_government_id != -1 && ship.ai_state_code == 4) {
    if (const Government *govt = ship_government();
        govt != nullptr && (govt->flags_primary & 0x0010U) != 0U) {
      const double max_shield = NovaAi_ComputeMaxShieldPoints(state, ship);
      if (static_cast<double>(ship.shield_points) < max_shield * 0.5 &&
          ship.ai_odds_score >= 0.0F) {
        const System *sys = state.scenario.System(
            static_cast<std::int16_t>(ship.current_system_id + 0x80));
        if (sys != nullptr && sys->reinf_fleet >= 0) {
          const FleetDef *fleet = state.scenario.Fleet(
              static_cast<std::int16_t>(sys->reinf_fleet + 0x80));
          const bool allied =
              fleet != nullptr && fleet->government_id >= 0 &&
              NovaGovernment_AreGovtsAllied(state.scenario,
                                            ship.faction_or_government_id,
                                            fleet->government_id);
          bool retreat = false;
          if (allied) {
            const float cooldown =
                state.reinforcement_countdown[static_cast<std::size_t>(
                    ship.current_system_id)];
            retreat = cooldown <= 0.0F
                          ? govt->max_odds < ship.ai_odds_score
                          : govt->max_odds * 2.0F < ship.ai_odds_score;
          } else {
            retreat = govt->max_odds < ship.ai_odds_score;
          }
          if (retreat) {
            ship.ai_state_code = 3;
          }
        } else if (govt->max_odds < ship.ai_odds_score) {
          ship.ai_state_code = 3;
        }
      }
    }
  }

  // Cowardice / depletion disengage: outside states 2/3/0xb, an engaged
  // unled ship whose shields fall below its aggression or personality
  // cowardice threshold, or whose cost-bearing weapons are all depleted,
  // retreats. random_ai_render_cadence packs the personality aggression level:
  // 1 -> 30% shields, 2 -> 15%, 4 -> never retreats.
  if (ship.primary_target_ship_slot != -1 && ship.ai_state_code != 7 &&
      ship.ai_state_code != 3) {
    if (ship.ai_station_hold_timer <= 0.0F) {
      ship.ai_state_code = 4;
    }

    const double max_shield = NovaAi_ComputeMaxShieldPoints(state, ship);
    int shield_retreat_threshold = -0x7fff;
    if (ship.pers_def_slot == -1) {
      if (ship.random_ai_render_cadence == 1) {
        shield_retreat_threshold =
            static_cast<int>(std::lround(max_shield * 0.3));
      } else if (ship.random_ai_render_cadence == 2) {
        shield_retreat_threshold =
            static_cast<int>(std::lround(max_shield * 0.15));
      }
    } else if (static_cast<std::size_t>(ship.pers_def_slot) <
               state.scenario.pers_defs.size()) {
      const double cowardice =
          static_cast<double>(
              state.scenario
                  .pers_defs[static_cast<std::size_t>(ship.pers_def_slot)]
                  .cowardice_pct) *
          0.01 * max_shield;
      shield_retreat_threshold = static_cast<int>(std::lround(cowardice));
    }

    const Government *govt = ship_government();
    if (ship.shield_points < static_cast<float>(shield_retreat_threshold) &&
        ship.squad_leader_ship_slot == -1 && govt != nullptr &&
        (govt->flags_primary & 0x0010U) != 0U && ship.ai_state_code != 2 &&
        ship.ai_state_code != 3 && ship.ai_state_code != 0xb) {
      ship.ai_state_code = 3;
    }

    const ShipClass *ship_class = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    if (ship_class != nullptr &&
        (ship_class->flags_secondary & 0x0080U) != 0U &&
        NovaWeapon_ClassifyAmmoReadiness(state, ship) != 0 &&
        ship.ai_state_code != 2 && ship.ai_state_code != 3 &&
        ship.ai_state_code != 0xb) {
      ship.ai_state_code = 3;
    }
  }

  // State 2 without a jump route: re-pick a travel stellar, otherwise fall to
  // the no-route state 6. (The original re-tests the jump gate in the -1
  // branch, but the gate is false at entry to this arm, so state 6 is the only
  // outcome.)
  if (ship.ai_state_code == 2 &&
      !NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
    ship.ai_secondary_target_slot = -1;
    ship.ai_secondary_target_slot = NovaAi_SelectRandomAdjacentTravelStellar(
        state, ship, /*strict_mode=*/false, /*unrestricted_only=*/false);
    if (ship.ai_secondary_target_slot == -1) {
      ship.ai_state_code = 6;
    } else {
      ship.travel_transfer_mode = 2;
      ship.ai_state_code = 1;
    }
  }

  // Attack run out of ammo: stand down and clear all targets.
  if (ship.ai_state_code == 4 &&
      NovaWeapon_ClassifyAmmoReadiness(state, ship) == 2) {
    ship.ai_state_code = 0;
    ship.ai_control_mode = 0;
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = -1;
  }
}

// Ghidra 0x004038b0 Ship_UpdateShipAiBehavior0x03_WarshipCapture. The
// plunder-flavored variant of hostile behavior 0x03, selected by the
// dispatcher when the ship's faction has government flags_primary 0x1000
// (Bible: "warships will plunder non-mission, trader-type enemies"). Instead
// of only fighting, the ship scans for disabled boardable victims
// (Ship_SelectNearestDisabledShipForBoarding), marks already-boarded targets
// for the attack/yield decision, performs the actual boarding handoff
// (Boarding_BoardShipAndTransferCargo) from AI control mode 0xf, and abandons
// targets it cannot plausibly capture (no fireable weapons / depleted ammo).
void NovaAi_UpdateBehavior0x03CaptureVariant(GameState &state,
                                             Ship &ship,
                                             std::uint32_t now_ms) {
  if (NovaAiShip_IsDisabled(state, ship)) {
    return;
  }
  if (ship.ai_state_code == 0x16) {
    return;
  }

  // Drop a destroyed/missing primary target.
  if (ship.primary_target_ship_slot != -1) {
    const auto slot = static_cast<std::size_t>(ship.primary_target_ship_slot);
    if (!state.SlotInRange(slot) || !state.ShipAt(slot).is_active ||
        NovaAiShip_IsDestroyed(state.ShipAt(slot))) {
      ship.primary_target_ship_slot = -1;
      ship.ai_state_code = 0;
    }
  }

  // Re-acquire while idle, or while targetless outside the boarding handoff
  // state 0xe (which keeps the secondary-target boarding victim alive).
  if (ship.ai_state_code == 0 ||
      (ship.primary_target_ship_slot == -1 && ship.ai_state_code != 0xe)) {
    ship.primary_target_ship_slot = -1;
    NovaAi_SelectNearestDisabledShipForBoarding(state, ship);
    if (ship.primary_target_ship_slot == -1) {
      NovaAi_AcquirePrimaryTarget(state, ship);
    }
    if (ship.primary_target_ship_slot == -1) {
      // No victim or hostile around: the travel ladder. Settles at a valid
      // stellar, wanders to a random adjacent one, or jumps away.
      const System *sys = state.scenario.System(
          static_cast<std::int16_t>(ship.current_system_id + 0x80));
      const bool at_stellar = sys != nullptr &&
                              ship.jump_destination_stellar_id >= 0 &&
                              NovaTargeting_IsStellarAdjacentToSystem(
                                  *sys, ship.jump_destination_stellar_id);
      if (!at_stellar) {
        if (ship.ai_secondary_target_slot == -1) {
          ship.ai_secondary_target_slot =
              NovaAi_SelectRandomAdjacentTravelStellar(
                  state,
                  ship,
                  /*strict_mode=*/false,
                  /*unrestricted_only=*/false);
          if (ship.ai_secondary_target_slot == -1) {
            if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
              NovaAi_EnterState2ClearPrimaryTarget(state, ship);
            } else {
              ship.ai_state_code = 6;
            }
          } else {
            ship.travel_transfer_mode = 2;
            ship.ai_state_code = 1;
          }
        } else if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
          NovaAi_EnterState2ClearPrimaryTarget(state, ship);
        } else {
          ship.ai_state_code = 6;
        }
      } else if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
        NovaAi_EnterState2ClearPrimaryTarget(state, ship);
      } else {
        ship.ai_state_code = 6;
      }
    } else {
      // A victim/hostile was acquired: capture-approach (0xd) only for the
      // player, a surrendering post-hit contact, or a low-AI class with
      // capture capability; otherwise it is just an attack (4).
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot));
      const ShipClass *target_class = state.scenario.Ship(
          static_cast<std::int16_t>(target.ship_class_id + 0x80));
      const bool capturable_kind =
          target.ship_instance_id == 0 || target.post_hit_mode_hint >= 0 ||
          (target_class != nullptr && target_class->default_ai_behavior < 3);
      if (capturable_kind && target_class != nullptr &&
          target_class->crew != 0) {
        ship.ai_state_code = 0xd;
      } else {
        ship.ai_state_code = 4;
      }
    }
  }

  // Attack/capture arbitration on the current primary target.
  if (ship.primary_target_ship_slot != -1 &&
      (ship.ai_state_code == 0xd || ship.ai_state_code == 4)) {
    const auto target_slot =
        static_cast<std::size_t>(ship.primary_target_ship_slot);
    const Ship &target = state.ShipAt(target_slot);
    const ShipClass *target_class = state.scenario.Ship(
        static_cast<std::int16_t>(target.ship_class_id + 0x80));
    const bool capturable_kind =
        ship.primary_target_ship_slot == 0 || target.post_hit_mode_hint >= 0 ||
        (target_class != nullptr && target_class->default_ai_behavior < 3);
    if (capturable_kind && target_class != nullptr && target_class->crew > 0) {
      if (target.boarded_target_latch == 0) {
        // Not yet boarded: press the capture approach.
        ship.ai_state_code = 0xd;
      } else {
        // Already boarded: fall back to attack, but yield (state 0x16) when
        // another able ship is actively boarding the same victim.
        ship.ai_state_code = 4;
        for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
          const std::int16_t index = static_cast<std::int16_t>(slot);
          const Ship &competitor = state.ShipAt(slot);
          if (!competitor.is_active || index == ship.ship_instance_id ||
              index == ship.primary_target_ship_slot ||
              competitor.squad_leader_ship_slot == 0 ||
              NovaAiShip_IsDisabled(state, competitor) ||
              competitor.primary_target_ship_slot !=
                  ship.primary_target_ship_slot ||
              (competitor.ai_state_code != 0xd &&
               competitor.ai_control_mode != 0xe)) {
            continue;
          }
          ship.ai_state_code = 0x16;
          ship.ai_control_mode = 0;
          ship.primary_target_ship_slot = -1;
          ship.ai_secondary_target_slot = -1;
          ship.ai_hostility_accumulator = 0;
          ship.ai_maneuver_timer_ms = 120.0F;
          break;
        }
      }
    } else {
      ship.ai_state_code = 4;
    }
  }

  // Boarding handoff: the approach ran to completion (control mode 0xf) with
  // a victim latched into the secondary slot and the maneuver timer spent.
  if (ship.ai_state_code == 4 && ship.ai_control_mode == 0xf &&
      ship.ai_secondary_target_slot != -1 &&
      ship.ai_maneuver_timer_ms <= 0.0F &&
      // Port plumbing: the original indexes g_ship_states raw; a stellar id
      // left in the secondary slot would read out of bounds, which the slot
      // range guard rejects instead.
      state.SlotInRange(
          static_cast<std::size_t>(ship.ai_secondary_target_slot))) {
    Ship &victim =
        state.ShipAt(static_cast<std::size_t>(ship.ai_secondary_target_slot));
    ship.ai_maneuver_timer_ms = 100.0F;
    ship.ai_state_code = 0xe;
    ship.ai_control_mode = 0;
    ship.defense_fleet_home_stellar_id = -1;
    Boarding_BoardShipAndTransferCargo(state, ship, victim, now_ms);
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = -1;
  }

  // Abandon an already-disabled victim when this ship has no way to press
  // the attack (no fireable non-secondary weapon, or every bank depleted).
  if (ship.ai_state_code == 4 && ship.primary_target_ship_slot != -1) {
    const auto target_slot =
        static_cast<std::size_t>(ship.primary_target_ship_slot);
    const Ship &target = state.ShipAt(target_slot);
    const ShipClass *target_class = state.scenario.Ship(
        static_cast<std::int16_t>(target.ship_class_id + 0x80));
    if (NovaAiShip_IsDisabled(state, target) &&
        (target_class != nullptr && target_class->default_ai_behavior > 2) &&
        ship.primary_target_ship_slot != 0 && target.post_hit_mode_hint < 0 &&
        (!NovaWeapon_HasAnyFireableNonSecondaryWeapon(state, ship) ||
         NovaWeapon_ClassifyAmmoReadiness(state, ship) == 2)) {
      ship.ai_state_code = 0;
      ship.primary_target_ship_slot = -1;
    }
  }

  // Out of ammunition during an attack/capture: stand down entirely.
  if ((ship.ai_state_code == 4 || ship.ai_state_code == 0xd) &&
      NovaWeapon_ClassifyAmmoReadiness(state, ship) == 2) {
    ship.ai_state_code = 0;
    ship.ai_control_mode = 0;
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = -1;
  }
}

} // namespace game
