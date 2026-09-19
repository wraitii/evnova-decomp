// Clean-room reconstruction of NPC weapon aiming, targeting, and bank
// selection.

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

namespace {

constexpr float kInterceptSlowVelocity = 0.35F; // DAT_00575080
constexpr float kInterceptDistanceScale = 0.8F; // DAT_00575190

// Squared distance between two points, truncated toward zero. Used by
// Ship_ScoreAssistTargetForShip (0x00412090), whose x87 FIST + residual/sign
// correction turns the round-to-nearest FIST into a truncation.
[[nodiscard]] float
TruncatedDistanceSquared(float x1, float y1, float x2, float y2) {
  return static_cast<float>(static_cast<int>(SquaredDistance(x1, y1, x2, y2)));
}

struct WeaponBankState {
  std::int16_t ammo = 0;
  std::int16_t secondary = 0;
  float cooldown = 0.0F;
};

[[nodiscard]] WeaponBankState
ReadWeaponBank(const GameState &state, const Ship &ship, std::int16_t bank) {
  const auto index = static_cast<std::size_t>(bank);
  if (ship.ship_instance_id == 0) {
    return {state.weapon_count_by_class[index * 100],
            state.weapon_secondary_count_by_class[index * 100],
            state.weapon_bank_cooldown[index]};
  }
  return {ship.npc_weapon_count_by_class[index],
          ship.npc_weapon_secondary_count_by_class[index],
          ship.npc_weapon_bank_cooldown[index]};
}

} // namespace

[[nodiscard]] bool
WeaponBankCanFire(const GameState &state, const Ship &ship, std::int16_t bank) {
  const WeaponBankState bank_state = ReadWeaponBank(state, ship, bank);
  if (bank_state.ammo <= 0 || bank_state.cooldown > 0.0F) {
    return false;
  }
  const Weapon *weapon =
      state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
  if (weapon == nullptr) {
    return false;
  }
  // Weapon_CanFireWeaponBank (0x00468990) only consults the secondary
  // counter for ammo-backed weapons and carrier-bay weapons.  Energy weapons
  // use ammo_type == -1 and remain fireable with a zero secondary counter;
  // requiring secondary > 0 here incorrectly disables NPC energy guns whose
  // stock record carries no ammunition load.
  if (weapon->weapon_mode_code == 99) {
    return bank_state.secondary >= 1;
  }
  if (weapon->ammo_type >= 0 && weapon->ammo_type <= 0xff) {
    return bank_state.secondary >= 1;
  }
  return true;
}

namespace {

// Guided-turn threshold (Ghidra DAT_00575780 = 2.0f deg/tick).
constexpr float kGuidedTrackMinTurnRate = 2.0F;

// Ghidra 0x00463dc0 Weapon_WeaponCanTrackTarget. `turn_rate_deg_per_tick` is
// the TARGET ship's Ship_ComputeShipMaxTurnRateDeg result: the original is
// called as Weapon_WeaponCanTrackTarget(weapon_bank, target_ship) from both
// 0x00410f20 and 0x0040d220 (disasm 0x00411122-0x00411137 / 0x0040d369-
// 0x0040d37e), i.e. the gate depends on how hard the target can turn. Once the
// target turns harder than 3 deg/tick, a flags_primary 0x0008 weapon is
// rejected outright, and any weapon whose guided_turn_rate is at or below 2.0
// deg/tick (DAT_00575780) is rejected. Targets at or below 3 deg/tick pass the
// gate without consulting either field. The original int-converts the rate
// with C truncation toward zero; an unordered (NaN) guided_turn_rate counts as
// trackable (the FCOMP unordered path returns true).
[[nodiscard]] bool WeaponCanTrackTarget(float target_turn_rate_deg_per_tick,
                                        const Weapon &weapon) {
  if (static_cast<int>(target_turn_rate_deg_per_tick) <= 3) {
    return true;
  }
  if ((weapon.flags & 0x0008U) != 0U) {
    return false;
  }
  return !(weapon.guided_turn_rate <= kGuidedTrackMinTurnRate);
}

[[nodiscard]] bool IsWeaponInTargetRange(const Weapon &weapon,
                                         float distance_sq) {
  // Ghidra 0x00411162/0x00411168: distance^2 * 0.8 <= range_scalar^2. The
  // original has no positive-range guard, so a zero range still passes when
  // the squared distance is zero (same position).
  return distance_sq * kInterceptDistanceScale <=
         weapon.range_scalar * weapon.range_scalar;
}

} // namespace

// Mode-6 freeflight-rocket lead constants (Ghidra k_mode6_rocket_* doubles,
// 0x005754e0/e8/f0, pre-commented). The rocket is still accelerating to speed,
// so its flight time is computed in two regimes rather than the naive
// dist/speed: near target (dist <= speed*19.59) it is ~3.16x the naive lead
// (0.316 speed factor); far target it is (dist - speed*19.59)/speed plus a
// small 2.06667 spool bonus.
constexpr float kMode6LeadThresholdFactor = 19.59F;
constexpr float kMode6LeadNearSpeedFactor = 0.316F;
constexpr float kMode6LeadFarTimeBonus = 2.06667F;

namespace {

// Shared lead-intercept core for Ship_AimWeaponPredictive (0x0043b740) and
// Ship_AimWeaponLeadVelocity (0x0043b8c0). Both compute the straight bearing
// first and only replace it with an intercept when the weapon mode is
// lead-capable; they differ solely in that gate. `include_mode6` selects the
// predictive gate {-1, 4, 6..9}; the LeadVelocity gate is {-1, 4, 7..9}, so
// the mode-6 branch below is unreachable for that caller (the original's
// copied mode-6 branch is likewise dead code).
std::int16_t AimWeaponInterceptBearing(const GameState &state,
                                       float ship_vel_x,
                                       float ship_vel_y,
                                       float target_pos_x,
                                       float target_pos_y,
                                       float target_vel_x,
                                       float target_vel_y,
                                       float origin_x,
                                       float origin_y,
                                       std::int16_t weapon_id,
                                       bool include_mode6) {
  // Straight bearing fallback (Math_BearingFromPointToPoint(origin, target)).
  std::int16_t bearing = static_cast<std::int16_t>(
      BearingDeg(origin_x, origin_y, target_pos_x, target_pos_y));
  if (weapon_id < 0 || weapon_id >= 0x100) {
    return bearing;
  }
  const Weapon *w =
      state.scenario.Weapon(static_cast<std::int16_t>(weapon_id + 0x80));
  if (w == nullptr) {
    return bearing;
  }
  const int mode = w->weapon_mode_code;
  const int min_lead_mode = include_mode6 ? 6 : 7;
  const bool lead_capable =
      mode == -1 || mode == 4 || (mode >= min_lead_mode && mode <= 9);
  if (!lead_capable) {
    return bearing;
  }
  const float dx = target_pos_x - origin_x;
  const float dy = target_pos_y - origin_y;
  const float dist = std::sqrt(dx * dx + dy * dy);
  const float shot_speed = w->projectile_speed / 100.0F;
  if (shot_speed <= 0.0F) {
    return bearing;
  }
  float t; // flight time (ticks) to the intercept
  if (mode == 6) {
    const float threshold = shot_speed * kMode6LeadThresholdFactor;
    if (threshold < dist) {
      t = (dist - threshold) / shot_speed + kMode6LeadFarTimeBonus;
    } else {
      t = dist / (shot_speed * kMode6LeadNearSpeedFactor);
    }
  } else {
    t = dist / shot_speed;
  }
  const float intercept_x = target_pos_x + (target_vel_x - ship_vel_x) * t;
  const float intercept_y = target_pos_y + (target_vel_y - ship_vel_y) * t;
  return static_cast<std::int16_t>(
      BearingDeg(origin_x, origin_y, intercept_x, intercept_y));
}

} // namespace

// Ghidra 0x0043b740 Ship_AimWeaponPredictive. See ship_ai.hpp for the
// algorithm. Returns the leading intercept bearing when the given weapon is a
// lead-capable mode, else the straight bearing to the target.
std::int16_t NovaAi_AimWeaponPredictive(const GameState &state,
                                        const Ship &ship,
                                        const Ship &target,
                                        std::int16_t weapon_id) {
  return NovaAi_AimWeaponPredictiveFrom(
      state, ship, target, weapon_id, ship.pos_x, ship.pos_y);
}

// Ghidra 0x0043b740 Ship_AimWeaponPredictive with the fourth argument
// (float *ship_pos_xy) supplied: the muzzle position after quadrant geometry.
std::int16_t NovaAi_AimWeaponPredictiveFrom(const GameState &state,
                                            const Ship &ship,
                                            const Ship &target,
                                            std::int16_t weapon_id,
                                            float origin_x,
                                            float origin_y) {
  return AimWeaponInterceptBearing(state,
                                   ship.vel_x,
                                   ship.vel_y,
                                   target.pos_x,
                                   target.pos_y,
                                   target.vel_x,
                                   target.vel_y,
                                   origin_x,
                                   origin_y,
                                   weapon_id,
                                   /*include_mode6=*/true);
}

// Ghidra 0x0043b8c0 Ship_AimWeaponLeadVelocity. Same intercept math as
// Ship_AimWeaponPredictive, but the target arrives as explicit
// position/velocity pointers so it can lead a non-Ship object (the scripted
// asteroid / station target); the firing hull's own velocity still comes from
// `ship`. Degree bearing. Mode 6 is excluded from the gate, so a rocket on
// this path falls back to the straight bearing.
std::int16_t NovaAi_AimWeaponLeadVelocity(const GameState &state,
                                          const Ship &ship,
                                          float target_pos_x,
                                          float target_pos_y,
                                          float target_vel_x,
                                          float target_vel_y,
                                          std::int16_t weapon_id,
                                          float origin_x,
                                          float origin_y) {
  return AimWeaponInterceptBearing(state,
                                   ship.vel_x,
                                   ship.vel_y,
                                   target_pos_x,
                                   target_pos_y,
                                   target_vel_x,
                                   target_vel_y,
                                   origin_x,
                                   origin_y,
                                   weapon_id,
                                   /*include_mode6=*/false);
}

int NovaAi_GetShipJammingScore(const GameState &state,
                               Ship &ship,
                               int seek_channel) {
  if (seek_channel < 0 || seek_channel > 3) {
    return 0;
  }
  if (NovaAiShip_IsDisabled(state, ship)) {
    return 0;
  }
  const int cached = ship.jamming_score[static_cast<std::size_t>(seek_channel)];
  if (cached >= 0) {
    return cached;
  }

  int score = 0;
  // Base: the ship class's inherent-attributes government InhJam value.
  const ShipClass *cls = ShipClassFor(state, ship);
  // InherentGovt is normalized to a zero-based def index at load.
  const std::int16_t inherent_govt =
      cls != nullptr ? cls->inherent_attributes_govt : -1;
  if (inherent_govt < 0) {
    score = 0;
  } else if (const Government *govt =
                 state.scenario.GovernmentByIndex(inherent_govt);
             govt != nullptr) {
    score = govt->inherent_jam[static_cast<std::size_t>(seek_channel)];
  }

  // Outfit bonuses: ModType opcodes 0x21..0x24 (Jamming Type 1-4) contribute
  // their ModVal. The player sums every owned outfit; an NPC sums each mounted
  // stock outfit once (not per count), matching 0x00464810.
  auto contributes = [seek_channel, &score](const Outfit &outfit) {
    std::array<std::int16_t, 4> types{outfit.mod_type,
                                      outfit.alt_mod_types[0],
                                      outfit.alt_mod_types[1],
                                      outfit.alt_mod_types[2]};
    std::array<std::int16_t, 4> vals{outfit.mod_val,
                                     outfit.alt_mod_vals[0],
                                     outfit.alt_mod_vals[1],
                                     outfit.alt_mod_vals[2]};
    for (std::size_t i = 0; i < types.size(); ++i) {
      if (types[i] ==
          static_cast<std::int16_t>(seek_channel +
                                    static_cast<int>(OutfitEffect::kJam1))) {
        score += vals[i];
      }
    }
  };
  if (ship.ship_instance_id == 0) {
    const auto &outfits = state.scenario.outfits;
    for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size();
         ++i) {
      if (state.inventory.outfit_owned_count[i] > 0 && i < outfits.size()) {
        contributes(outfits[i]);
      }
    }
  } else if (cls != nullptr) {
    for (std::size_t slot = 0; slot < cls->default_outfit_ids.size(); ++slot) {
      if (cls->default_outfit_counts[slot] <= 0) {
        continue;
      }
      const std::int16_t outfit_id = cls->default_outfit_ids[slot];
      if (outfit_id < 0 || outfit_id >= static_cast<std::int16_t>(
                                            state.scenario.outfits.size())) {
        continue;
      }
      contributes(state.scenario.outfits[static_cast<std::size_t>(outfit_id)]);
    }
    // Fraction-specific government flag 0x80 halves the NPC's score.
    // faction_or_government_id indexes g_government_defs directly (zero-based).
    if (ship.faction_or_government_id >= 0) {
      const auto gid = static_cast<std::size_t>(ship.faction_or_government_id);
      if (gid < state.scenario.governments.size() &&
          (state.scenario.governments[gid].flags_primary & 0x80U) != 0U) {
        score = score >> 1;
      }
    }
  }

  score = std::clamp(score, 0, 100);
  ship.jamming_score[static_cast<std::size_t>(seek_channel)] =
      static_cast<std::int16_t>(score);
  return score;
}

// Ghidra 0x00410f20 Ship_CanShipInterceptCurrentPrimaryTarget. The original's
// final comparison is deliberately a strict base-speed comparison; the
// preceding weapon-bank walk only establishes the guided/intercept context.
bool NovaAiShip_CanInterceptCurrentPrimaryTarget(const GameState &state,
                                                 const Ship &ship) {
  const std::int16_t target_slot = ship.primary_target_ship_slot;
  if (target_slot < 0 ||
      !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
    return false;
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  if (!target.is_active || target.current_system_id != ship.current_system_id ||
      (target.ship_instance_id != 0 && target.ai_state_code != 3)) {
    return false;
  }
  const ShipClass *ship_class = ShipClassFor(state, ship);
  const ShipClass *target_class = ShipClassFor(state, target);
  if (ship_class == nullptr || target_class == nullptr ||
      ship_class->mass_tons < 100) {
    return false;
  }

  const float relative_x = target.vel_x - ship.vel_x;
  const float relative_y = target.vel_y - ship.vel_y;
  if (std::abs(relative_x) <= kInterceptSlowVelocity &&
      std::abs(relative_y) <= kInterceptSlowVelocity) {
    return false;
  }
  const float relative_bearing = BearingDeg(0.0F, 0.0F, relative_x, relative_y);
  const float target_to_ship_bearing =
      BearingDeg(target.pos_x, target.pos_y, ship.pos_x, ship.pos_y);
  // Ghidra 0x0041117d: the original subtracts the two integer bearings and
  // takes the absolute difference WITHOUT wrapping into [-180, 180], so a
  // pair straddling the 0/360 seam (e.g. 5 vs 355) yields 350 and passes this
  // bail-out gate. Deliberately not wrapped to preserve the original quirk.
  if (std::abs(static_cast<int>(relative_bearing) -
               static_cast<int>(target_to_ship_bearing)) < 90) {
    return false;
  }

  // Original bank walk (0x004110f0-0x0041117d). Both of its exit paths return
  // the identical strict base_speed comparison (disasm 0x00411183 vs
  // 0x004111e0), so the walk is dead with respect to the result; it is
  // reproduced for fidelity and `has_intercept_bank` is intentionally unused.
  const float distance_sq =
      SquaredDistance(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);
  // Ghidra 0x00411122-0x00411137 passes the TARGET ship to
  // Weapon_WeaponCanTrackTarget, so this gate uses the target's max turn rate
  // (Ship_ComputeShipMaxTurnRateDeg 0x00463e70), not the shooter's.
  const float target_turn_rate_deg_per_tick =
      NovaShip_ComputeEffectiveStats(state, target, *target_class)
          .turn_rate_deg_per_tick;
  bool has_intercept_bank = false;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const WeaponBankState bs = ReadWeaponBank(state, ship, bank);
    // Ghidra 0x004110f0/0x00411107: ammo counter > 0 and secondary counter
    // > 0 or the -1 infinite-ammo marker.
    if (bs.ammo <= 0 || (bs.secondary <= 0 && bs.secondary != -1)) {
      continue;
    }
    const Weapon *weapon = state.scenario.Weapon(bank + 0x80);
    if (weapon == nullptr || weapon->weapon_mode_code != 1 ||
        !WeaponCanTrackTarget(target_turn_rate_deg_per_tick, *weapon) ||
        !NovaWeapon_CanFireWeaponBank(state, ship, bank) ||
        !IsWeaponInTargetRange(*weapon, distance_sq)) {
      continue;
    }
    has_intercept_bank = true;
    break;
  }
  (void)has_intercept_bank; // result discarded by the original in both paths
  return ship_class->speed < target_class->speed;
}

// Ghidra 0x00412090 Ship_ScoreAssistTargetForShip. The original's first
// argument is the candidate and its second argument is the assisting helper.
// In particular, the cloak predicate is asymmetric and must receive those
// roles in this order.
std::int32_t NovaAi_ScoreAssistTargetForShip(const GameState &state,
                                             const Ship &candidate,
                                             const Ship &helper,
                                             std::int16_t score_flags) {
  if (candidate.ship_instance_id == helper.ship_instance_id ||
      candidate.ship_instance_id == candidate.squad_leader_ship_slot ||
      candidate.ship_instance_id == helper.squad_leader_ship_slot ||
      candidate.squad_leader_ship_slot == helper.squad_leader_ship_slot ||
      !candidate.is_active ||
      !NovaAiShip_CanEngageTargetUnderCloakRules(state, candidate, helper)) {
    return 0;
  }
  if (NovaAiShip_IsDisabled(state, candidate) &&
      candidate.escort_command_code != 2) {
    return 0;
  }

  bool target_context_ok = false;
  // The original tests the HELPER's leader slot here (0x00412113, EBP =
  // target_ship/helper) before calling ShouldKeepPressingTarget(candidate)
  // (0x00412280, ESI = ship/candidate).
  if (helper.squad_leader_ship_slot == 0) {
    target_context_ok = NovaAiShip_ShouldKeepPressingTarget(state, candidate);
  } else if (helper.squad_leader_ship_slot >= 0 &&
             state.SlotInRange(
                 static_cast<std::size_t>(helper.squad_leader_ship_slot))) {
    target_context_ok = NovaTargeting_IsShipAcquirableAsTarget(
        state,
        state.ShipAt(static_cast<std::size_t>(helper.squad_leader_ship_slot)),
        candidate);
  }
  if (!target_context_ok) {
    return 0;
  }

  const Ship &helper_leader =
      state.ShipAt(static_cast<std::size_t>(helper.squad_leader_ship_slot));
  const float candidate_to_leader_sq =
      TruncatedDistanceSquared(helper_leader.pos_x,
                               helper_leader.pos_y,
                               candidate.pos_x,
                               candidate.pos_y);
  if (score_flags > 0 &&
      candidate_to_leader_sq > static_cast<float>(score_flags) * score_flags) {
    return 0;
  }

  std::int32_t score = static_cast<std::int32_t>(
      TruncatedDistanceSquared(
          helper.pos_x, helper.pos_y, candidate.pos_x, candidate.pos_y) +
      (score_flags > 0 ? candidate_to_leader_sq : 0.0F));
  const ShipClass *candidate_class = ShipClassFor(state, candidate);
  const ShipClass *helper_class = ShipClassFor(state, helper);
  if (candidate_class != nullptr && helper_class != nullptr &&
      candidate_class->class_category != helper_class->class_category) {
    score = (score + 1) / 2;
    if (candidate_class->class_category == 0 &&
        helper_class->class_category == 2) {
      score = (score + 1) / 2;
    }
  }
  return std::max(score, 1);
}

// Ghidra 0x00412030 Ship_FindBestAssistTargetForShip. The decompiler exposes
// the second argument as a ShipState pointer, but the callsites pass the
// literal short ranges 0x226 and -1; score_flags is the useful semantic type.
std::int16_t NovaAi_FindBestAssistTargetForShip(const GameState &state,
                                                const Ship &ship,
                                                std::int16_t score_flags) {
  std::int32_t best_score = std::numeric_limits<std::int32_t>::max();
  std::int16_t best_slot = -1;
  for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
    const std::int32_t score = NovaAi_ScoreAssistTargetForShip(
        state, state.ShipAt(slot), ship, score_flags);
    if (score > 0 && score < best_score) {
      best_score = score;
      best_slot = static_cast<std::int16_t>(slot);
    }
  }
  return best_slot;
}

// Ghidra 0x00411540 plus the weapon-bank chooser at 0x0040ce00. This is the
// target-validity side faithfully; bank ranking is the available clean-room
// subset (mode, ammo, cooldown, target capability, range, and damage class).
void NovaAi_EscortFireAtUnprovokedTarget(GameState &state, Ship &ship) {
  // Ship_EscortFireAtUnprovokedTarget (0x00411540) is an escort/mission
  // post-state refresh: behaviors below 5 return without touching the bank,
  // so only escorts/fighters/assists revalidate and re-arm here. Behaviors 3/4
  // arm their banks in the combat control-mode bodies instead.
  // Ship_IsShipDestroyed (0x004688e0) is a separate gate from the
  // disabled predicate.  A lethal hit leaves the ship slot
  // active during its destruction window, but it must not acquire a fresh
  // weapon bank in the post-state refresh.
  if (ship.ai_behavior_code < 5) {
    return;
  }
  if (NovaAiShip_IsDestroyed(ship)) {
    ship.active_weapon_bank_slot = -1;
    ship.ai_fire_trigger_latch = 0;
    return;
  }
  const std::int16_t target_slot = ship.primary_target_ship_slot;
  if (target_slot < 0 ||
      !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
    ship.primary_target_ship_slot = -1;
    return;
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  // The original clears the primary target whenever it is inactive or
  // disabled, so a disabled boarding victim is never re-armed here. Behavior
  // 3/4 capture drives return above and do not reach this clear.
  if (!target.is_active || NovaAiShip_IsDisabled(state, target)) {
    ship.primary_target_ship_slot = -1;
    return;
  }
  const ShipClass *target_class = ShipClassFor(state, target);
  if (target_class == nullptr) {
    return;
  }
  NovaWeapon_EnsureNpcWeaponBanks(state, ship);

  const float distance_sq =
      SquaredDistance(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);
  std::int16_t best_bank = -1;
  std::int32_t best_score = -1;
  std::int16_t best_turret_bank = -1;
  std::int32_t best_turret_score = -1;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const Weapon *weapon = state.scenario.Weapon(bank + 0x80);
    if (weapon == nullptr ||
        (weapon->weapon_mode_code != -1 && weapon->weapon_mode_code != 0 &&
         weapon->weapon_mode_code != 1 && weapon->weapon_mode_code != 3 &&
         weapon->weapon_mode_code != 4 && weapon->weapon_mode_code != 5 &&
         weapon->weapon_mode_code != 6 && weapon->weapon_mode_code != 7 &&
         weapon->weapon_mode_code != 8) ||
        !WeaponBankCanFire(state, ship, bank) ||
        (weapon->flags_secondary & 0x400U) !=
            (target_class->capability_flags & 0x400U)) {
      continue;
    }
    const bool turret_mode =
        weapon->weapon_mode_code == 3 || weapon->weapon_mode_code == 4 ||
        weapon->weapon_mode_code == 7 || weapon->weapon_mode_code == 8;
    if (turret_mode) {
      // Ghidra 0x0040ce00 gates turret banks through
      // Weapon_IsShipWithinWeaponRangeOfTarget (0x00411600) with the bank.
      if (!NovaWeapon_ShipWithinWeaponRangeOfTarget(
              state, ship, target, bank)) {
        continue;
      }
    } else if (weapon->range_scalar > 0.0F &&
               distance_sq * kInterceptDistanceScale >
                   weapon->range_scalar * weapon->range_scalar) {
      continue;
    }
    const float target_bearing =
        BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);
    const float heading_deg = WrapDeg(ship.heading / kDegToRad);
    const float reference = weapon->weapon_mode_code == 8
                                ? WrapDeg(heading_deg + 180.0F)
                                : heading_deg;
    if ((weapon->weapon_mode_code == 7 || weapon->weapon_mode_code == 8) &&
        std::abs(std::remainder(target_bearing - reference, kFullCircleDeg)) >=
            46.0F) {
      continue;
    }
    const std::int32_t score = target.shield_points > 0.0F
                                   ? weapon->energy_damage
                                   : weapon->mass_damage;
    if (best_bank == -1 || score > best_score) {
      best_bank = bank;
      best_score = score;
    }
    // Ghidra's Weapon_SelectWeaponBankForCurrentTarget (0x0040ce00) is a
    // separate turret/quadrant selection path from the guided/direct helpers.
    // Keep that distinction here: otherwise a higher-damage mode-1 hailgun
    // permanently wins the clean-room all-mode ranking over an Abomination's
    // mode-4 pulse cannon, even though the original arms turret banks on
    // their own control-mode passes.
    if (turret_mode && (best_turret_bank == -1 || score > best_turret_score)) {
      best_turret_bank = bank;
      best_turret_score = score;
    }
  }
  const std::int16_t selected_bank =
      best_turret_bank != -1 ? best_turret_bank : best_bank;
  if (selected_bank != -1) {
    ship.active_weapon_bank_slot = selected_bank;
    ship.ai_fire_trigger_latch = 1;
  }
}

namespace {

// Math_ShortestAngleDeltaDeg is shared from nova_math.hpp. The blind-spot
// sector test is exported (see NovaAi_WeaponIsTargetBearingInTurretBlindSpot
// after this namespace).

// Ghidra 0x0046c930 / 0x0046ca60 (shared scan). True when `ship` owns any
// cloak-scanner outfit (modType 0x1E) whose effect ModVal carries `flag`
// (0x04 = allow targeting of untargetable ships, 0x08 = allow targeting of
// cloaked ships). The player path mirrors the targeting.cpp ScannerCapabilities
// port (same 0x1e scan across primary+alt mod slots); this adds the NPC
// ship-class default-outfit scan the player-only targeting port omits.
bool NovaAi_OutfitHasCloakScannerCapability(const GameState &state,
                                            const Ship &ship,
                                            std::uint16_t flag) {
  const auto outfit_has = [flag](const Outfit &o) {
    const auto one = [flag](std::int16_t type, std::int16_t val) {
      return type == 0x1E && (static_cast<std::uint16_t>(val) & flag) != 0U;
    };
    return one(o.mod_type, o.mod_val) ||
           one(o.alt_mod_types[0], o.alt_mod_vals[0]) ||
           one(o.alt_mod_types[1], o.alt_mod_vals[1]) ||
           one(o.alt_mod_types[2], o.alt_mod_vals[2]);
  };
  if (ship.ship_instance_id == 0) {
    const auto &owned = state.inventory.outfit_owned_count;
    const auto &outfits = state.scenario.outfits;
    for (std::size_t id = 0; id < owned.size() && id < outfits.size(); ++id) {
      if (owned[id] > 0 && outfit_has(outfits[id])) {
        return true;
      }
    }
    return false;
  }
  const ShipClass *cls = ShipClassFor(state, ship);
  if (cls == nullptr) {
    return false;
  }
  for (std::size_t i = 0; i < cls->default_outfit_ids.size(); ++i) {
    if (cls->default_outfit_counts[i] <= 0) {
      continue;
    }
    const Outfit *o = state.scenario.Outfit(cls->default_outfit_ids[i]);
    if (o != nullptr && outfit_has(*o)) {
      return true;
    }
  }
  return false;
}

} // namespace

// Ghidra 0x0046b360 Weapon_IsTargetBearingInTurretBlindSpot: whether the
// bearing lies in one of the weapon's turret blind-spot sectors. Front (<46
// deg), side (<136 deg), rear; a sector is BLIND when the weapon's
// flags_primary 0x1000/0x2000/0x4000 is set, force-overridden ON by the
// matching ShipClass capability flags (Bible: "Turreted weapon has a blind spot
// to the front/sides/rear"). Callers reject the bank while the target is in a
// blind spot.
bool NovaAi_WeaponIsTargetBearingInTurretBlindSpot(
    const ShipClass &ship_class,
    const Weapon &weapon,
    std::int16_t heading_deg,
    std::int16_t target_bearing_deg) {
  const std::int16_t delta =
      ShortestAngleDeltaDeg(heading_deg, target_bearing_deg);
  bool blind;
  if (delta < 0x2e) {
    blind = (weapon.flags & 0x1000U) != 0U;
    if ((ship_class.capability_flags & 0x1000U) != 0U) {
      blind = true;
    }
  } else if (delta < 0x88) {
    blind = (weapon.flags & 0x2000U) != 0U;
    if ((ship_class.capability_flags & 0x2000U) != 0U) {
      blind = true;
    }
  } else {
    blind = (weapon.flags & 0x4000U) != 0U;
    if ((ship_class.capability_flags & 0x4000U) != 0U) {
      blind = true;
    }
  }
  return blind;
}

// Ghidra 0x0040ce00 Weapon_SelectWeaponBankForCurrentTarget. Turret-ish bank
// selection for the current primary target: scans fireable mode-3/4/7/8
// banks in the allowed arc and range, scores by mass/energy damage, and arms
// the best (energy-preferred when the target still has shields).
void NovaAi_SelectWeaponBankForCurrentTarget(GameState &state, Ship &ship) {
  if (NovaAiShip_IsDisabled(state, ship)) {
    return;
  }
  NovaWeapon_SelectTurretTargetWithinArc(state, ship);
  const std::int16_t target_slot = ship.primary_target_ship_slot;
  if (target_slot < 0 ||
      !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
    return;
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  if (!target.is_active) {
    return;
  }
  const ShipClass *target_class = ShipClassFor(state, target);
  if (target_class == nullptr || (target_class->flags_secondary & 4U) != 0U) {
    return; // class-untargetable
  }
  if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, target, ship)) {
    return;
  }
  const ShipClass *ship_class = ShipClassFor(state, ship);
  const float heading_deg_f = ship.heading / kDegToRad;
  const std::int16_t heading_deg = static_cast<std::int16_t>(heading_deg_f);

  std::int16_t best_mass_bank = -1;
  std::int16_t best_mass = 0;
  std::int16_t best_energy_bank = -1;
  std::int16_t best_energy = 0;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const WeaponBankState bs = ReadWeaponBank(state, ship, bank);
    if (bs.ammo <= 0 || bs.cooldown > 0.0F) {
      continue;
    }
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (weapon == nullptr) {
      continue;
    }
    const int mode = weapon->weapon_mode_code;
    const bool turret = mode == 3 || mode == 4 || mode == 7 || mode == 8;
    if (!turret) {
      continue;
    }
    if ((weapon->flags_secondary & 0x400U) !=
        (target_class->capability_flags & 0x400U)) {
      continue;
    }
    const std::int16_t bearing = static_cast<std::int16_t>(
        BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y));

    // The mode-7/8 rear/turret arc check sets the barrier; then
    // Weapon_IsWeaponArcAllowed CLEARS it when the bank is allowed in the
    // facing arc (the original's inversion for these turret weapons).
    bool barrier = false;
    if (mode == 7 || mode == 8) {
      int delta = 0;
      if (mode == 7) {
        // Truncate the continuous |bearing - heading| difference toward zero
        // (x87 FIST + residual/sign correction) before the mod-360 wrap.
        delta = static_cast<int>(
            std::abs(static_cast<float>(bearing) - heading_deg_f));
      } else { // mode 8: reject the bank behind the hull
        delta = std::abs(static_cast<int>(bearing) -
                         ((heading_deg + 0xb4) % 0x168));
      }
      barrier = (delta % 0x168) < 0x2e;
    } else {
      barrier = true;
    }
    // Ghidra Weapon_SelectWeaponBankForCurrentTarget (0x0040ce00): a target
    // in one of the weapon's turret blind-spot sectors disqualifies the bank;
    // otherwise the bank stays a candidate and still passes the range check.
    // (The former NovaAi_WeaponArcAllowed call had this polarity inverted.)
    if (ship_class != nullptr &&
        NovaAi_WeaponIsTargetBearingInTurretBlindSpot(
            *ship_class, *weapon, heading_deg, bearing)) {
      barrier = false;
    }
    if (barrier) {
      barrier =
          NovaWeapon_ShipWithinWeaponRangeOfTarget(state, ship, target, bank);
    }
    if (!barrier || !NovaWeapon_CanFireWeaponBank(state, ship, bank)) {
      continue;
    }
    const std::int16_t mass = weapon->mass_damage < 1 ? 1 : weapon->mass_damage;
    if (best_mass_bank == -1 || mass > best_mass) {
      best_mass_bank = bank;
      best_mass = mass;
    }
    const std::int16_t energy =
        weapon->energy_damage < 1 ? 1 : weapon->energy_damage;
    if (best_energy_bank == -1 || energy > best_energy) {
      best_energy_bank = bank;
      best_energy = energy;
    }
  }
  std::int16_t selected = best_mass_bank;
  if (target.shield_points >= 0.0F) {
    selected = best_energy_bank;
  }
  if (selected != -1) {
    ship.active_weapon_bank_slot = selected;
    ship.ai_fire_trigger_latch = 1;
  }
}

// Ghidra 0x004221d0 Ship_IsInboundThreatExceedingDefenses.
bool NovaAi_IsInboundThreatExceedingDefenses(const Ship &ship) {
  // The original first rounds the defensive sum to float, then multiplies by
  // a binary64 1.05 without storing the x87 result. FMA recovers the rounding
  // residual when the binary64 product lands exactly on the integer threat,
  // preserving that boundary even where long double is only binary64.
  constexpr double kDefenseBudgetScale = 1.05;
  const double defenses = ship.shield_points + ship.armor_points;
  const double budget = defenses * kDefenseBudgetScale;
  const double threat = ship.inbound_weapon_threat;
  if (threat < budget) {
    return false;
  }
  if (threat > budget) {
    return true;
  }
  if (std::isnan(budget)) {
    return false;
  }
  return std::fma(defenses, kDefenseBudgetScale, -budget) <= 0.0;
}

// Ghidra 0x0040d220 Weapon_SelectGuidedWeaponBankForPrimaryTarget. Arms the
// first fireable guided (mode-1) bank that can track the primary target
// within the (0.95-scaled) intercept range; applies the scanner-untargetable
// and cloaked-target capability gates.
void NovaAi_SelectGuidedWeaponBankForPrimaryTarget(GameState &state,
                                                   Ship &ship) {
  const std::int16_t target_slot = ship.primary_target_ship_slot;
  if (target_slot < 0 ||
      !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
    return;
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  if (ship.current_system_id != target.current_system_id || !target.is_active) {
    return;
  }
  const ShipClass *target_class = ShipClassFor(state, target);
  if (target_class == nullptr) {
    NovaLog::Todo(
        "guided weapon selection (0x0040d220): skipped target slot {} "
        "with missing ship class {}",
        target_slot,
        target.ship_class_id);
    return;
  }
  if ((target_class->flags_secondary & 4U) != 0U &&
      !NovaAi_OutfitHasCloakScannerCapability(state, ship, 0x04U)) {
    return; // class-untargetable and no targeting-scanner outfit
  }
  if (NovaTargeting_ShipAtCloakVisibilityThreshold(target) &&
      !NovaAi_OutfitHasCloakScannerCapability(state, ship, 0x08U)) {
    return; // cloaked target, no cloak-scanner outfit
  }
  if (NovaAi_IsInboundThreatExceedingDefenses(target)) {
    return;
  }
  const float distance_sq =
      SquaredDistance(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);
  // Ghidra 0x0040d369-0x0040d37e passes the TARGET ship to
  // Weapon_WeaponCanTrackTarget, so the gate uses the target's max turn rate
  // (Ship_ComputeShipMaxTurnRateDeg 0x00463e70), not the shooter's.
  const float target_turn_rate_deg_per_tick =
      NovaShip_ComputeEffectiveStats(state, target, *target_class)
          .turn_rate_deg_per_tick;

  std::int16_t chosen = -1;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const WeaponBankState bs = ReadWeaponBank(state, ship, bank);
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (weapon == nullptr || bs.ammo <= 0) {
      continue;
    }
    const bool track_ok =
        WeaponCanTrackTarget(target_turn_rate_deg_per_tick, *weapon);
    if (weapon->weapon_mode_code != 1 || !track_ok) {
      continue;
    }
    // 0x400-capability match and Weapon_CanFireWeaponBank gate.
    if ((weapon->flags_secondary & 0x400U) !=
            (target_class->capability_flags & 0x400U) ||
        !NovaWeapon_CanFireWeaponBank(state, ship, bank)) {
      continue;
    }
    // Guided intercept-range gate: the original reuses the mode-1 0.95 damp
    // (DOUBLE_005750f0) as the scale against range_scalar^2.
    if (distance_sq * 0.95F > weapon->range_scalar * weapon->range_scalar) {
      continue;
    }
    chosen = bank;
    break; // original takes the first qualifying bank
  }
  if (chosen != -1) {
    const WeaponBankState bs = ReadWeaponBank(state, ship, chosen);
    if (bs.cooldown <= 0.0F) {
      ship.active_weapon_bank_slot = chosen;
      ship.ai_fire_trigger_latch = 1;
    }
  }
}

// Ghidra 0x0040d470 Weapon_SelectDirectFireWeaponBankForPrimaryTarget. Arms
// the best in-range direct-fire bank (modes -1/0/6, or mode 1 when
// allow_guided_mode) scored by mass/energy damage; mode 6 applies a
// blast-radius placement gate. When nothing was armed and no non-guided bank
// was seen, retries once with guided handling allowed.
//
// Port divergences: the original guards only `target_slot == -1` and then
// indexes g_ship_states / g_ship_class_defs / g_weapon_defs unguarded; the
// port's SlotInRange / ShipClassFor / Weapon null checks only change behavior
// for malformed data. The original also calls the pure Math_SquaredDistance
// (0x0043b710) at 0x0040d67c and discards the result (FABS + FSTP); the port
// omits that no-op call.
void NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(GameState &state,
                                                       Ship &ship,
                                                       bool allow_guided_mode) {
  const std::int16_t target_slot = ship.primary_target_ship_slot;
  if (target_slot < 0 ||
      !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
    return;
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  if (ship.current_system_id != target.current_system_id || !target.is_active) {
    return;
  }
  const ShipClass *target_class = ShipClassFor(state, target);
  if (target_class == nullptr ||
      !NovaAiShip_CanEngageTargetUnderCloakRules(state, target, ship)) {
    return;
  }

  bool has_non_guided = false;
  std::int16_t best_mass_bank = -1;
  std::int16_t best_mass = 0;
  std::int16_t best_energy_bank = -1;
  std::int16_t best_energy = 0;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const WeaponBankState bs = ReadWeaponBank(state, ship, bank);
    if (bs.ammo <= 0) {
      continue;
    }
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (weapon == nullptr) {
      continue;
    }
    const int mode = weapon->weapon_mode_code;
    const bool mode_ok = mode == -1 || mode == 0 || mode == 6 ||
                         (mode == 1 && allow_guided_mode);
    if (!mode_ok) {
      continue;
    }
    if ((weapon->flags_secondary & 0x400U) !=
        (target_class->capability_flags & 0x400U)) {
      continue;
    }
    if (!NovaWeapon_CanFireWeaponBank(state, ship, bank)) {
      continue;
    }
    if (mode != 1) {
      has_non_guided = true;
    }
    if (bs.cooldown > 0.0F) {
      continue;
    }
    if (!NovaWeapon_ShipWithinWeaponRangeOfTarget(state, ship, target, bank)) {
      continue;
    }
    const std::int16_t mass = weapon->mass_damage < 1 ? 1 : weapon->mass_damage;
    const std::int16_t energy =
        weapon->energy_damage < 1 ? 1 : weapon->energy_damage;
    if (mode == 6 && weapon->blast_radius > 0) {
      // Mode 6 freeflight rocket: the blast radius * 2.5 (DOUBLE_00575188)
      // placement gate requires both axis deltas to clear it.
      const float blast = static_cast<float>(weapon->blast_radius) * 2.5F;
      const float dx = std::abs(ship.pos_x - target.pos_x);
      const float dy = std::abs(ship.pos_y - target.pos_y);
      if (blast <= dx && blast <= dy) {
        if (best_mass_bank == -1 || mass > best_mass) {
          best_mass_bank = bank;
          best_mass = mass;
        }
        if (best_energy_bank == -1 || energy > best_energy) {
          best_energy_bank = bank;
          best_energy = energy;
        }
      }
    } else {
      if (best_mass_bank == -1 || mass > best_mass) {
        best_mass_bank = bank;
        best_mass = mass;
      }
      if (best_energy_bank == -1 || energy > best_energy) {
        best_energy_bank = bank;
        best_energy = energy;
      }
    }
  }
  std::int16_t selected = best_mass_bank;
  if (target.shield_points >= 0.0F) {
    selected = best_energy_bank;
  }
  if (selected != -1) {
    ship.active_weapon_bank_slot = selected;
  }
  if (ship.active_weapon_bank_slot == -1) {
    if (!has_non_guided && !allow_guided_mode) {
      NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(state, ship, true);
    }
  } else {
    ship.ai_fire_trigger_latch = 1;
  }
}

// Ghidra 0x0040d910 Weapon_SelectGeneralWeaponBank. Broad fallback: arms the
// most recent fireable general weapon (non mode-0/3, mode < 8).
void NovaAi_SelectGeneralWeaponBank(GameState &state, Ship &ship) {
  std::int16_t chosen = -1;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const WeaponBankState bs = ReadWeaponBank(state, ship, bank);
    if (bs.ammo <= 0 || bs.cooldown > 0.0F) {
      continue;
    }
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (weapon == nullptr) {
      continue;
    }
    const int mode = weapon->weapon_mode_code;
    if (mode == 0 || mode == 3 || mode >= 8) {
      continue;
    }
    if (!NovaWeapon_CanFireWeaponBank(state, ship, bank)) {
      continue;
    }
    chosen = bank; // last qualifying bank wins
  }
  if (chosen != -1) {
    ship.active_weapon_bank_slot = chosen;
    ship.ai_fire_trigger_latch = 1;
  }
}

// Ghidra 0x0040d7e0 Weapon_SelectUnguidedWeaponBank. Fallback that arms the
// highest-damage fireable unguided bank: modes -1/0/6, or mode 7 when there
// is no primary target. Mode 0 additionally passes only when the weapon's
// flags_quaternary bit 0 or the ship class's availability bit 0 is clear.
void NovaAi_SelectUnguidedWeaponBank(GameState &state, Ship &ship) {
  const ShipClass *cls = ShipClassFor(state, ship);
  std::int16_t best_bank = -1;
  std::int16_t best_score = 0;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const WeaponBankState bs = ReadWeaponBank(state, ship, bank);
    if (bs.ammo <= 0 || bs.cooldown > 0.0F) {
      continue;
    }
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (weapon == nullptr) {
      continue;
    }
    const int mode = weapon->weapon_mode_code;
    const bool has_target = ship.primary_target_ship_slot != -1;
    const bool mode_ok =
        mode == -1 || (mode == 6) || (mode == 7 && !has_target) ||
        (mode == 0 &&
         (((weapon->flags_quaternary & 1U) == 0U) ||
          (cls != nullptr && (cls->availability_flags & 1U) == 0U)));
    if (!mode_ok) {
      continue;
    }
    if ((weapon->flags_secondary & 0x400U) != 0U ||
        !NovaWeapon_CanFireWeaponBank(state, ship, bank)) {
      continue;
    }
    const std::int16_t score =
        weapon->mass_damage < 1 ? 1 : weapon->mass_damage;
    if (score > best_score) {
      best_score = score;
      best_bank = bank;
    }
  }
  if (best_bank != -1) {
    ship.active_weapon_bank_slot = best_bank;
  }
  if (ship.active_weapon_bank_slot != -1) {
    ship.ai_fire_trigger_latch = 1;
  }
}

} // namespace game
