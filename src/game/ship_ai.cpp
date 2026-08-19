// Clean-room reconstruction of the NPC ship AI decision layer. See ship_ai.hpp
// for the module overview and the Ghidra-address mapping of each helper.
//
// Porting notes:
//  * Ships carry no outfit inventory, so "has gravity shield" uses the NPC
//    branch of Outfit_ShipHasGravityShieldOutfit (0x0046df70) via
//    NovaShip_HasGravityShield, and "effective stats" are the class base
//    values (the player's outfit/ionization damping is not modelled yet).
//  * Several per-mode turn/thrust scale constants (the _DAT_005750xx globals)
//    were provisional; they have since been decoded from the raw bytes and
//    typed + pre-commented in the Ghidra DB (2025-08-09). The movement modes
//    (1/2/3/4/0xd) now read the real values; combat-mode constants are only
//    referenced in comments until those modes are ported.
//  * HUD/mission flavor side-effects (overlay messages, extortion prompts,
//    fuel-transfer chatter, carrier-bay launch) are documented no-ops until
//    those systems land; combat-heavy branches that depend on the not-yet-
//    reconstructed disable/weapon systems are conservatively gated.
//  * "Wander" is the reintegrated value of Phase 3/4: Behavior 0x01 + state 0/1
//    makes a ship pick a random adjacent travel stellar and steer toward it,
//    which is the visible "make them move around the system" milestone.

#include "ship_ai.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "government.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "spaceflight.hpp"
#include "targeting.hpp"
#include "travel.hpp"

namespace game {

namespace {

// --- Decoded movement constants (Ghidra _DAT_00575xxx). The original reads
// these from byte-mislabeled globals at 0x00575000..0x00575200; the values
// below were decoded from the raw bytes (float vs double from the actual
// instruction widths) on 2025-08-09 and are typed + pre-commented in the
// Ghidra DB. See docs/npc_ship_behavior_plan.md "Diagnosis" section. ---
// "Moving" / arrival-stopped velocity threshold, px/tick (0x575080, double).
constexpr float kVerySlowSpeed = 0.35F;
// State-1 travel arrival velocity damp (0x575088, double).
constexpr float kArrivalDamp = 0.98F;
// State-1 travel arrive-range: (9.0 - min(turn_rate_deg, 8.0)) * 8.0 + 32.0
// px per axis (0x575070 / 0x575068 / 0x575078, doubles).
constexpr float kArriveRangeBase = 9.0F;
constexpr float kArriveRangeTurnCap = 8.0F;
constexpr float kArriveRangeScale = 8.0F;
constexpr float kArriveRangeOffset = 32.0F;
// Mode-1 (damp/brake) ladder: fast threshold 0.35 (0x575080), "still fast"
// 1.75 (0x575118), half-thrust factor 0.5 (0x575038), aligned damp 0.94
// (0x5750f8), nearly-stopped damp 0.95 (0x5750f0), alignment addend 1.0
// (0x575018).
constexpr float kMode1FastThreshold = 0.35F;
constexpr float kMode1StillFastThreshold = 1.75F;
constexpr float kMode1SlowThrustFactor = 0.5F;
constexpr float kMode1Damp = 0.94F;
constexpr float kMode1StopDamp = 0.95F;
constexpr float kMode1AlignAddend = 1.0F;
// Mode-2 (travel): alignment addend 5.0 (0x575100, float), "close to stellar"
// gate 500 px/axis (0x575104, float), arrival cruise fraction 0.25
// (0x575108, double).
constexpr float kMode2AlignAddend = 5.0F;
constexpr float kMode2CloseGatePx = 500.0F;
constexpr float kMode2ArriveFraction = 0.25F;
// Mode-3 (approach centre): alignment addend 3.0 (0x575120, float).
constexpr float kMode3AlignAddend = 3.0F;
// Engagement distance within which a ship can fire on or engage a target
// (0x5750b0, float).
constexpr float kEngageDist = 165.0F;
// Assist/response keep-distance thresholds.
constexpr float kAssistClose = 300.0F;
constexpr float kAssistFar = 600.0F;
// Squared-distance threshold for "at system centre" (states 2/3 station-keep);
// 0x575098 (double) = 1,000,000 px^2 (1000 px radius).
constexpr float kCentreRangeSq = 1000000.0F;
// Gravity-shield approach multipliers (state 0xd/0xf).
constexpr float kShieldKeepMult = 4.0F;
// Combat turn-radius constants from DAT_005750a0/a4/a8/ac. The class turn
// value is converted to the runtime degrees-per-tick value before these
// branches use it.
constexpr float kTurnRadiusBase = 10.0F;
constexpr float kTurnRadiusScale50 = 50.0F;
constexpr float kAssistTurnRadiusScale = 30.0F;
constexpr float kAssistInnerTurnRadiusScale = 15.0F;
constexpr float kCombatCloseRange = 165.0F;
constexpr float kCombatStationRange = 251.0F; // 0xfb

// --- Combat control-mode constants (Ship_ApplyShipAiControls, decoded from
// the typed _DAT_00575xxx globals; same source table as the travel block). ---
// Proximity gates: 165 px/axis for the tight-approach / boost-return gate
// (0x5750b0), 123 px for the evasive-break order (0x575124, decoded from the
// raw bytes: 00 00 f6 42), 82 px for the long-axis break-off (0x575128),
// 200 px for the hold/approach velocity-lerp (0x575160).
constexpr float kEvasiveOrderGatePx = 123.0F;
constexpr float kBreakOuterGatePx = 82.0F;
constexpr float kHoldApproachGatePx = 200.0F;
// Mode-0xc/0xf velocity-match tolerance, px/tick per axis (0x575158).
constexpr float kVelocityMatchTol = 0.525F;
// Mode-0x10 evasive break: 1.5x thrust (0x575130, double), 135 deg heading
// offset (0x575138, float), sign from ship_instance_id parity.
constexpr float kEvasiveThrustFactor = 1.5F;
constexpr float kEvasiveHeadingOffsetDeg = 135.0F;
// Mode-0x11 boost: 2.75x thrust (0x575140), 1.8x max-speed cruise (0x575148).
constexpr float kBoostThrustFactor = 2.75F;
constexpr float kBoostCruiseFactor = 1.8F;
// Mode-0xc brake while outrunning the target: 0.66x thrust (0x575168, double).
constexpr float kVelMatchBrakeFactor = 0.66F;
// Position-creep factor shared by mode 8 (escort follow) and the mode-0xc
// formation move: eff_thrust * 10 (0x575170, double) per frame-time unit.
constexpr float kFormationCreepFactor = 10.0F;
// Mode-0xc formation position offset radius (0x575178, float = 48 px) applied
// in front of the leader's heading+135 when positioning (phase-8 area).
constexpr float kFormationOffsetRadiusPx = 48.0F;
// Mode-0x14 scripted velocity-match creep half-spans: 150 px x / 80 px y
// per axis (0x57517c / 0x575180), merge gate turn+10 (0x5750a0 = 10.0).
constexpr float kScriptPosXSpan = 150.0F;
constexpr float kScriptPosYSpan = 80.0F;
constexpr float kScriptAlignAddend = 10.0F;
// Control-mode 10 (stationary cleanup reverse): desired -5.75 px/tick with a
// -3.67 thrust command (raw float bits 0xC0B80000 / 0xC06AE148).
constexpr float kMode10ReverseSpeed = -5.75F;
constexpr float kMode10ReverseThrust = -3.67F;
// Escort-follow half-span stand-in: Sprite_GetShipClassEscortShotHalfSpan
// (0x004624c0) returns the class escort sprite's shot half-span or its 0x4B =
// 75 px debug fallback; the clean-room sprite tables are not modelled, so the
// debug fallback stands in (TODO(decomp)).
constexpr float kEscortHalfSpanPx = 75.0F;

constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
constexpr float kFullCircleDeg = 360.0F;
// The reference simulation cadence (30 Hz): the original's
// _g_avg_frame_time_ms at 30 fps, used by the control-mode creeps and the
// velocity-match/scripted ramps (modes 8/0xc/0xf/0x14 read the measured frame
// time in milliseconds, not the normalized tick unit).
constexpr float kReferenceFrameTimeMs = 1000.0F / 30.0F;

// Game-convention angle winding helper (0..360, 0 = up / -y).
float WrapDeg(float d) {
  d = std::fmod(d, kFullCircleDeg);
  return d < 0.0F ? d + kFullCircleDeg : d;
}

// Mirrors Math_SquaredDistance (0x0043b710) for two float points.
float SquaredDistance(float x1, float y1, float x2, float y2) {
  const float dx = x2 - x1;
  const float dy = y2 - y1;
  return dx * dx + dy * dy;
}

// Mirrors Math_BearingFromPointToPoint: heading (degrees, 0 = up, clockwise)
// from (x1,y1) to (x2,y2). The codebase's heading = atan2(dx, -dy).
float BearingDeg(float x1, float y1, float x2, float y2) {
  const float dx = x2 - x1;
  const float dy = y2 - y1;
  return WrapDeg(std::atan2(dx, -dy) / kDegToRad);
}

// The current system's resource id (nav/stellar tables are indexed by resource
// id >= 0x80, matching the original g_system_defs_ptr / g_stellar_defs).
const System *CurrentSystem(const GameState &state) {
  return state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
}

const Stellar *StellarByResourceId(const GameState &state, int resource_id) {
  if (resource_id < 0) {
    return nullptr;
  }
  return state.scenario.Stellar(static_cast<std::int16_t>(resource_id));
}

} // namespace

// Ghidra 0x004688e0 Ship_IsShipDestroyed.
bool NovaAiShip_IsDestroyed(const Ship &ship) {
  if (ship.death_timer_active > 0.0F) {
    return true;
  }
  return ship.armor_points <= 0.0F;
}

// Ghidra 0x00410670 Ship_EnterShipAiState0x02_ClearPrimaryTarget.
void NovaAi_EnterState2ClearPrimaryTarget(Ship &ship, std::uint32_t now_ms) {
  ship.ai_state_code = 2;
  if (ship.ai_station_hold_timer < 0.0F) {
    ship.ai_station_hold_timer = 0.0F;
  }
  ship.primary_target_ship_slot = -1;
  ship.ai_mode_start_time_ms = now_ms;
}

// Ghidra 0x004687b0 Ship_IsShipFireRestricted. True when the ship must not
// fire/act this frame: derelict government (flags_primary 0x800), docked to a
// stellar (target_stellar_object_id set) for non-player ships, or critically
// damaged (armor below a government/aggression-dependent fraction of max). The
// mission-fleet escort-without-flight gate is deferred (no mission fleets yet).
bool NovaAiShip_IsFireRestricted(const GameState &state, const Ship &ship) {
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
  // Non-player ships attached to a stellar (docked/landed target) hold fire.
  if (ship.ship_instance_id > 0 && ship.target_stellar_object_id != -1) {
    return true;
  }
  // Critically-damaged gate: armor below a fraction of max armor. The Bible
  // threshold is one third, reduced to one tenth by Ship Flags 0x0010.
  // This gate is also what suppresses NPC shield/armor regeneration in
  // Ship_HandleShip, so keep it separate from the destruction predicate.
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const float max_armor = cls ? static_cast<float>(cls->base_armor) : 0.0F;
  if (max_armor > 0.0F) {
    const float ratio =
        (cls && (cls->capability_flags & 0x10) != 0) ? 0.1F : (1.0F / 3.0F);
    if (ship.armor_points < max_armor * ratio) {
      return true;
    }
  }
  return false;
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
      NovaAiShip_IsFireRestricted(state, ship)) {
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
      if (ship.npc_weapon_bank_ammo[bank_index] <= 0) {
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
  if (ship.ai_behavior_code > 4 && ship.ai_target_ship_slot >= 0 &&
      ship.ai_control_mode == 0xc &&
      state.SlotInRange(static_cast<std::size_t>(ship.ai_target_ship_slot))) {
    const Ship &target =
        state.ShipAt(static_cast<std::size_t>(ship.ai_target_ship_slot));
    should_enter =
        should_enter || NovaTargeting_ShipAtCloakVisibilityThreshold(target);
  }
  if (should_enter) {
    NovaAi_OnShipCloakStateEntered(state, ship);
  } else {
    NovaAi_OnShipCloakStateCleared(ship);
  }
}

namespace {

constexpr float kInterceptSlowVelocity = 0.35F; // DAT_00575080
constexpr float kInterceptDistanceScale = 0.8F; // DAT_00575190

[[nodiscard]] const ShipClass *ShipClassFor(const GameState &state,
                                            const Ship &ship) {
  return state.scenario.Ship(
      static_cast<std::int16_t>(ship.ship_class_id + 0x80));
}

[[nodiscard]] float
RoundedDistanceSquared(float x1, float y1, float x2, float y2) {
  return static_cast<float>(std::lround(SquaredDistance(x1, y1, x2, y2)));
}

void EnsureNpcWeaponBanks(GameState &state, Ship &ship) {
  if (ship.ship_instance_id == 0 ||
      ship.npc_weapon_banks_ship_class == ship.ship_class_id) {
    return;
  }
  ship.npc_weapon_bank_ammo.fill(0);
  ship.npc_weapon_bank_secondary.fill(0);
  ship.npc_weapon_bank_cooldown.fill(0.0F);
  ship.npc_weapon_bank_burst_counter.fill(0);
  const ShipClass *cls = ShipClassFor(state, ship);
  if (cls != nullptr) {
    for (const ShipDefaultWeaponBank &stock : cls->stock_weapons) {
      if (stock.weapon_id < 0x80 || stock.weapon_id >= 0x180) {
        continue;
      }
      const auto bank = static_cast<std::size_t>(stock.weapon_id - 0x80);
      ship.npc_weapon_bank_ammo[bank] = std::max<std::int16_t>(stock.count, 0);
      // -1 is the original unlimited-secondary sentinel.
      ship.npc_weapon_bank_secondary[bank] = stock.ammo_load;
    }
    // Weapon_InitShipWeaponBursts (0x00413810): a configured burst weapon
    // (burst_cycle AND burst_reset_cooldown) starts with a zeroed burst
    // counter and its cooldown preloaded to the reset cooldown.
    for (std::size_t bank = 0; bank < 0x100; ++bank) {
      const Weapon *w =
          state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
      if (w != nullptr && ship.npc_weapon_bank_ammo[bank] > 0 &&
          w->burst_cycle_ticks > 0 && w->burst_reset_cooldown > 0) {
        ship.npc_weapon_bank_burst_counter[bank] = 0;
        ship.npc_weapon_bank_cooldown[bank] =
            static_cast<float>(w->burst_reset_cooldown);
      }
    }
  }
  ship.npc_weapon_banks_ship_class = ship.ship_class_id;
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
    return {state.weapon_bank_ammo[index * 100],
            state.weapon_bank_secondary[index * 100],
            state.weapon_bank_cooldown[index]};
  }
  return {ship.npc_weapon_bank_ammo[index],
          ship.npc_weapon_bank_secondary[index],
          ship.npc_weapon_bank_cooldown[index]};
}

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

[[nodiscard]] bool WeaponCanTrackTarget(const ShipClass &ship_class,
                                        const Weapon &weapon) {
  // Weapon_WeaponCanTrackTarget rejects flagged tracking weapons when the
  // caller's maximum turn rate exceeds three degrees. The remaining
  // field_0x58 threshold is not yet represented in the clean Weapon record;
  // unflagged weapons pass this gate exactly as in the original.
  return (weapon.flags & 0x0008U) == 0 ||
         static_cast<float>(ship_class.turn_rate) * 0.1F <= 3.0F;
}

[[nodiscard]] bool IsWeaponInTargetRange(const Weapon &weapon,
                                         float distance_sq) {
  if (!(weapon.range_scalar > 0.0F)) {
    return false;
  }
  return distance_sq * kInterceptDistanceScale <=
         weapon.range_scalar * weapon.range_scalar;
}

[[nodiscard]] bool IsTurretWeaponInTargetRange(const Weapon &weapon,
                                               const Ship &ship,
                                               const Ship &target) {
  // Weapon_IsShipWithinWeaponRangeOfTarget (0x00411600) rounds each absolute
  // axis delta, then compares the squared sum against (range + 32)^2. Beam
  // modes use BeamLength; projectile turret modes use the post-load +0x5c
  // effective range. This is intentionally not the generic intercept estimate.
  const float dx = std::abs(ship.pos_x - target.pos_x);
  const float dy = std::abs(ship.pos_y - target.pos_y);
  const bool beam_mode = weapon.weapon_mode_code == 0 ||
                         weapon.weapon_mode_code == 3 ||
                         weapon.weapon_mode_code == 10;
  const float reach = (beam_mode ? static_cast<float>(weapon.beam_length_px)
                                 : weapon.range_scalar) +
                      32.0F;
  const float rounded_dx = static_cast<float>(std::lround(dx));
  const float rounded_dy = static_cast<float>(std::lround(dy));
  return reach > 0.0F &&
         rounded_dx * rounded_dx + rounded_dy * rounded_dy <= reach * reach;
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

// Ghidra 0x0043b740 Ship_AimWeaponPredictive. See ship_ai.hpp for the
// algorithm. Returns the leading intercept bearing when the given weapon is a
// lead-capable mode, else the straight bearing to the target.
std::int16_t NovaAi_AimWeaponPredictive(const GameState &state,
                                        const Ship &ship,
                                        const Ship &target,
                                        std::int16_t weapon_id) {
  // Straight bearing fallback (Math_BearingFromPointToPoint(ship, target)).
  std::int16_t bearing = static_cast<std::int16_t>(
      BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y));
  if (weapon_id < 0 || weapon_id >= 0x100) {
    return bearing;
  }
  const Weapon *w =
      state.scenario.Weapon(static_cast<std::int16_t>(weapon_id + 0x80));
  if (w == nullptr) {
    return bearing;
  }
  const int mode = w->weapon_mode_code;
  const bool lead_capable = mode == -1 || mode == 4 || (mode >= 6 && mode <= 9);
  if (!lead_capable) {
    return bearing;
  }
  const float dx = target.pos_x - ship.pos_x;
  const float dy = target.pos_y - ship.pos_y;
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
  const float intercept_x = target.pos_x + (target.vel_x - ship.vel_x) * t;
  const float intercept_y = target.pos_y + (target.vel_y - ship.vel_y) * t;
  return static_cast<std::int16_t>(
      BearingDeg(ship.pos_x, ship.pos_y, intercept_x, intercept_y));
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
  if (std::abs(std::remainder(relative_bearing - target_to_ship_bearing,
                              kFullCircleDeg)) < 90.0F) {
    return false;
  }

  // Keep the original bank walk observable in the clean-room model. NPC bank
  // counters are seeded by the auto-selection path; an unseeded NPC simply has
  // no available guided bank yet, which does not change this helper's final
  // strict speed comparison.
  const float distance_sq =
      SquaredDistance(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);
  bool has_intercept_bank = false;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const Weapon *weapon = state.scenario.Weapon(bank + 0x80);
    if (weapon == nullptr || weapon->weapon_mode_code != 1 ||
        !WeaponBankCanFire(state, ship, bank) ||
        !WeaponCanTrackTarget(*ship_class, *weapon) ||
        !IsWeaponInTargetRange(*weapon, distance_sq)) {
      continue;
    }
    has_intercept_bank = true;
    break;
  }
  (void)has_intercept_bank;
  return ship_class->speed < target_class->speed;
}

// Ghidra 0x00412030 / 0x00412090. The decompiler exposes the second argument
// as a ShipState pointer, but the callsites pass the literal short ranges
// 0x226 and -1; score_flags is the useful semantic type.
std::int16_t NovaAi_FindBestAssistTargetForShip(const GameState &state,
                                                const Ship &ship,
                                                std::int16_t score_flags) {
  const std::int16_t self = ship.ship_instance_id;
  std::int32_t best_score = std::numeric_limits<std::int32_t>::max();
  std::int16_t best_slot = -1;
  for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
    const Ship &candidate = state.ShipAt(slot);
    if (candidate.ship_instance_id == self ||
        candidate.ship_instance_id == candidate.ai_target_ship_slot ||
        candidate.ship_instance_id == ship.ai_target_ship_slot ||
        candidate.ai_target_ship_slot == ship.ai_target_ship_slot ||
        !candidate.is_active || candidate.ai_state_code == 0x15 ||
        NovaAiShip_IsDestroyed(candidate) ||
        !NovaAiShip_CanEngageTargetUnderCloakRules(state, ship, candidate)) {
      continue;
    }
    if (NovaAiShip_IsFireRestricted(state, candidate) &&
        candidate.escort_command_code != 2) {
      continue;
    }

    bool target_context_ok = false;
    if (candidate.ai_target_ship_slot == 0) {
      target_context_ok = NovaAiShip_ShouldKeepPressingTarget(state, candidate);
    } else if (ship.ai_target_ship_slot >= 0 &&
               state.SlotInRange(
                   static_cast<std::size_t>(ship.ai_target_ship_slot))) {
      target_context_ok = NovaTargeting_IsShipAcquirableAsTarget(
          state,
          state.ShipAt(static_cast<std::size_t>(ship.ai_target_ship_slot)),
          candidate);
    }
    if (!target_context_ok) {
      continue;
    }

    const float target_distance_sq = RoundedDistanceSquared(
        state.ShipAt(static_cast<std::size_t>(ship.ai_target_ship_slot)).pos_x,
        state.ShipAt(static_cast<std::size_t>(ship.ai_target_ship_slot)).pos_y,
        candidate.pos_x,
        candidate.pos_y);
    if (score_flags > 0 &&
        target_distance_sq > static_cast<float>(score_flags) * score_flags) {
      continue;
    }

    std::int32_t score = static_cast<std::int32_t>(std::lround(
        RoundedDistanceSquared(
            ship.pos_x, ship.pos_y, candidate.pos_x, candidate.pos_y) +
        (score_flags > 0 ? target_distance_sq : 0.0F)));
    const ShipClass *candidate_class = ShipClassFor(state, candidate);
    const ShipClass *ship_class = ShipClassFor(state, ship);
    if (candidate_class != nullptr && ship_class != nullptr &&
        candidate_class->class_category != ship_class->class_category) {
      score = (score + 1) / 2;
      if (candidate_class->class_category == 0 &&
          ship_class->class_category == 2) {
        score = (score + 1) / 2;
      }
    }
    score = std::max(score, 1);
    if (score < best_score) {
      best_score = score;
      best_slot = static_cast<std::int16_t>(slot);
    }
  }
  return best_slot;
}

// Ghidra 0x00411540 plus the weapon-bank chooser at 0x0040ce00. This is the
// target-validity side faithfully; bank ranking is the available clean-room
// subset (mode, ammo, cooldown, target capability, range, and damage class).
void NovaAi_UpdateAutoWeaponSelectionFromTarget(GameState &state, Ship &ship) {
  // Combat behaviors 3/4 acquire hostile contacts directly. The original's
  // post-state weapon refresh still arms their selected bank; restricting
  // this to escort/mission behaviors (>4) left ordinary hostile NPCs with no
  // active weapon at all.
  // Ship_IsShipDestroyed (0x004688e0) is a separate gate from the
  // fire-restricted/disabled predicate.  A lethal hit leaves the ship slot
  // active during its destruction window, but it must not acquire a fresh
  // weapon bank in the post-state refresh.
  if (ship.ai_behavior_code < 3 || NovaAiShip_IsDestroyed(ship)) {
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
  if (!target.is_active || NovaAiShip_IsFireRestricted(state, target)) {
    ship.primary_target_ship_slot = -1;
    return;
  }
  const ShipClass *target_class = ShipClassFor(state, target);
  if (target_class == nullptr) {
    return;
  }
  EnsureNpcWeaponBanks(state, ship);

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
      if (!IsTurretWeaponInTargetRange(*weapon, ship, target)) {
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

// Ghidra 0x0040c790 Stellar_SelectRandomAdjacentTravelStellar. Selects a
// random adjacent travel stellar in the ship's current system, applying the
// government scan-mask/hostility filters; returns the stellar *resource id*
// (>= 0x80) or -1 when none qualifies. Provisional: the original builds
// weighted candidate pools from several scan-mask bits; we reconstruct the
// deterministic eligibility (adjacent + sprite-active + travel-usable + govt
// not hostile) and pick uniformly from the survivors.
std::int16_t NovaAi_SelectRandomAdjacentTravelStellar(GameState &state,
                                                      const Ship &ship) {
  const System *sys = CurrentSystem(state);
  if (!sys) {
    return -1;
  }
  std::vector<std::int16_t> candidates;
  for (const auto nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue; // empty nav slot
    }
    const Stellar *st = StellarByResourceId(state, nav);
    if (!st || !st->is_available || (st->flags & 1U) == 0U) {
      continue;
    }
    // Original culls stella with map coords >= 1000 (off the usable playfield).
    if (st->pos_x >= 1000 || st->pos_y >= 1000) {
      continue;
    }
    // Hostile neighbouring government is skipped. Stellar government_id is a
    // resource id (>= 0x80); the relation helper wants zero-based ids.
    if (ship.faction_or_government_id >= 0 && st->government_id >= 0x80 &&
        NovaGovernment_AreGovtsHostileOrXenophobic(
            state.scenario,
            ship.faction_or_government_id,
            static_cast<std::int16_t>(st->government_id - 0x80))) {
      continue;
    }
    candidates.push_back(nav);
  }
  if (candidates.empty()) {
    return -1;
  }
  std::uniform_int_distribution<std::size_t> pick(0, candidates.size() - 1);
  return candidates[pick(state.rng)];
}

// Ghidra 0x0040e020 Ship_AcquirePrimaryTargetForShip. The original has
// mission/scripted target arms and several combat-strength filters. Those data
// sources are not reconstructed here, so this clean-room slice keeps the
// executable core: same-system active contacts, hostile governments or
// player-directed combat contacts, nearest-first selection, and the player as
// a valid target when hostility has already been established.
void NovaAi_AcquirePrimaryTarget(GameState &state, Ship &ship) {
  if (ship.primary_target_ship_slot >= 0 &&
      state.SlotInRange(
          static_cast<std::size_t>(ship.primary_target_ship_slot))) {
    const Ship &current =
        state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot));
    if (current.is_active && !NovaAiShip_IsDestroyed(current) &&
        current.current_system_id == ship.current_system_id) {
      return;
    }
  }

  std::int16_t best_slot = -1;
  bool best_is_engaged = false;
  float best_distance_sq = 0.0F;
  for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
    if (slot == static_cast<std::size_t>(ship.ship_instance_id)) {
      continue;
    }
    const Ship &candidate = state.ShipAt(slot);
    if (!candidate.is_active || NovaAiShip_IsDestroyed(candidate) ||
        candidate.current_system_id != ship.current_system_id ||
        candidate.ai_state_code == 0x15 ||
        candidate.target_stellar_object_id != -1 ||
        NovaAiShip_IsFireRestricted(state, candidate) ||
        NovaTargeting_ShipAtCloakVisibilityThreshold(candidate)) {
      continue;
    }

    const bool player_contact = slot == 0;
    const bool candidate_is_engaged = candidate.ai_target_ship_slot == 0 ||
                                      candidate.primary_target_ship_slot == 0;
    bool hostile_contact = candidate_is_engaged;
    if (!hostile_contact && ship.faction_or_government_id >= 0 &&
        candidate.faction_or_government_id >= 0) {
      hostile_contact = NovaGovernment_AreGovtsHostileOrXenophobic(
          state.scenario,
          ship.faction_or_government_id,
          candidate.faction_or_government_id);
    }
    if (player_contact) {
      // The player has no stable government identity in the clean-room state;
      // retain the original's already-hostile path instead of making every
      // behavior-0x03 ship attack the player on sight.
      hostile_contact = hostile_contact || ship.ai_hostility_accumulator > 0;
    }
    if (!hostile_contact) {
      continue;
    }

    const float dx = candidate.pos_x - ship.pos_x;
    const float dy = candidate.pos_y - ship.pos_y;
    const float distance_sq = dx * dx + dy * dy;
    if (best_slot == -1 || (candidate_is_engaged && !best_is_engaged) ||
        (candidate_is_engaged == best_is_engaged &&
         distance_sq < best_distance_sq)) {
      best_slot = static_cast<std::int16_t>(slot);
      best_is_engaged = candidate_is_engaged;
      best_distance_sq = distance_sq;
    }
  }

  ship.primary_target_ship_slot = best_slot;
  if (best_slot != -1) {
    ship.ai_state_code = 4;
    if (best_slot == 0) {
      ship.ai_hostility_accumulator = std::max<std::int16_t>(
          ship.ai_hostility_accumulator, static_cast<std::int16_t>(1));
    }
  }
}

// Shared travel fallback used by behavior 0x02/0x03. This is the common
// `state 0 -> adjacent stellar -> state 1/2/6` ladder visible in both Ghidra
// supervisors; mission and special-loadout branches remain deferred.
void NovaAi_ReacquireTravelOrSettle(GameState &state,
                                    Ship &ship,
                                    std::uint32_t now_ms) {
  const System *sys = CurrentSystem(state);
  const bool still_at_point = sys && ship.jump_destination_stellar_id >= 0 &&
                              NovaTargeting_IsStellarAdjacentToSystem(
                                  *sys, ship.jump_destination_stellar_id);
  if (!still_at_point) {
    ship.ai_secondary_target_slot = -1;
    ship.ai_secondary_target_slot =
        NovaAi_SelectRandomAdjacentTravelStellar(state, ship);
  }
  if (ship.ai_secondary_target_slot == -1) {
    if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
      NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
    } else {
      ship.ai_state_code = 6;
    }
  } else if (!still_at_point) {
    ship.travel_transfer_mode = 2;
    ship.ai_state_code = 1;
  } else if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
    NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
  } else {
    ship.ai_state_code = 6;
  }
}

// Ghidra 0x00402860 Ship_UpdateShipAiBehavior0x01. The "normal travel / wander"
// supervisor. Reacquires a travel stellar when idle (state 0) -- picking a
// random adjacent travel stellar and entering state 1 (travel to it), else
// falling back to state 2 via NovaAi_EnterState2ClearPrimaryTarget (or state 6
// when no jump route exists). Escalates into hostile attack (state 3, or state
// 10 against the player) once a hostility accumulator + primary target exist.
void NovaAi_UpdateBehavior0x01(GameState &state,
                               Ship &ship,
                               std::uint32_t now_ms) {
  if (NovaAiShip_IsFireRestricted(state, ship)) {
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
    const System *sys = CurrentSystem(state);
    const bool still_at_point = sys && ship.jump_destination_stellar_id >= 0 &&
                                NovaTargeting_IsStellarAdjacentToSystem(
                                    *sys, ship.jump_destination_stellar_id);
    if (still_at_point) {
      // Already at a valid travel stellar: settle / try to jump away. The jump
      // gate uses THIS ship's class fuel (NovaTravel_CanShipInitiateJump-
      // Sequence), not the player's.
      if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
        NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
      } else {
        ship.ai_state_code = 6;
      }
    } else {
      // Pick a fresh travel destination to wander toward.
      ship.ai_secondary_target_slot = -1;
      const std::int16_t travel =
          NovaAi_SelectRandomAdjacentTravelStellar(state, ship);
      ship.ai_secondary_target_slot = travel;
      if (ship.ai_secondary_target_slot == -1) {
        // No route remains: try to jump, else settle into idle-template.
        if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
          NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
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
    if (ship.ai_target_ship_slot == 0) {
      ship.ai_state_code = 10;
      ship.ai_secondary_target_slot = 0;
    } else {
      ship.ai_state_code = 3;
    }
  }
  // State 3's intercept taunt (Ship_ShowPlayerInterceptTauntIfEligible) is a
  // HUD flavor no-op (TODO: mission/HUD chatter).
  (void)now_ms;
}

// Ghidra 0x00402bd0 Ship_UpdateShipAiBehavior0x02. Local/dude behavior shares
// the travel fallback with behavior 0x01, but promotes an established hostile
// contact once it is within the original 0x4e3-pixel per-axis gate. The
// player-target arm enters state 10 (assist/response); other contacts enter
// state 3. Government chatter and assistance encounter side effects remain
// TODO(decomp).
void NovaAi_UpdateBehavior0x02(GameState &state,
                               Ship &ship,
                               std::uint32_t now_ms) {
  if (NovaAiShip_IsFireRestricted(state, ship)) {
    return;
  }
  if (ship.ai_state_code == 9 || ship.ai_state_code == 0xf ||
      ship.ai_state_code == 0x16) {
    return;
  }

  if (ship.ai_state_code == 0 && ship.primary_target_ship_slot == -1) {
    NovaAi_ReacquireTravelOrSettle(state, ship, now_ms);
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
    } else if (ship.ai_target_ship_slot == 0) {
      ship.ai_state_code = 10;
      ship.ai_secondary_target_slot = 0;
    } else {
      ship.ai_state_code = 3;
    }
  }
}

// Ghidra 0x00402e50 Ship_UpdateShipAiBehavior0x03. Hostile behavior acquires
// a nearby contact when idle, preserves an active primary target, and falls
// back to the normal travel/jump ladder when combat has no target. The
// government flee, weapon-readiness, mission-fleet, and capture-variant arms
// depend on data not represented by the current clean-room Ship model.
void NovaAi_UpdateBehavior0x03(GameState &state,
                               Ship &ship,
                               std::uint32_t now_ms) {
  if (NovaAiShip_IsFireRestricted(state, ship)) {
    return;
  }
  if (ship.ai_state_code == 9 || ship.ai_state_code == 0xf ||
      ship.ai_state_code == 0x16) {
    return;
  }

  if (ship.primary_target_ship_slot != -1) {
    if (!state.SlotInRange(
            static_cast<std::size_t>(ship.primary_target_ship_slot)) ||
        !state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot))
             .is_active ||
        NovaAiShip_IsDestroyed(state.ShipAt(
            static_cast<std::size_t>(ship.primary_target_ship_slot)))) {
      ship.primary_target_ship_slot = -1;
      ship.ai_state_code = 0;
    }
  }

  if (ship.ai_state_code == 0 && ship.primary_target_ship_slot == -1) {
    NovaAi_AcquirePrimaryTarget(state, ship);
    if (ship.primary_target_ship_slot == -1) {
      NovaAi_ReacquireTravelOrSettle(state, ship, now_ms);
    }
  }

  if (ship.ai_hostility_accumulator > 0 &&
      ship.primary_target_ship_slot != -1 && ship.ai_state_code != 3 &&
      ship.ai_state_code != 7 && ship.ai_station_hold_timer <= 0.0F) {
    ship.ai_state_code = 4;
  }

  if (ship.ai_state_code == 4 && ship.primary_target_ship_slot != -1) {
    const auto target_slot =
        static_cast<std::size_t>(ship.primary_target_ship_slot);
    if (!state.SlotInRange(target_slot) ||
        !state.ShipAt(target_slot).is_active ||
        NovaAiShip_IsDestroyed(state.ShipAt(target_slot))) {
      ship.primary_target_ship_slot = -1;
      ship.ai_state_code = 0;
    }
  }
}

// Ghidra 0x00405590 Ship_UpdateShipAiState. The per-frame state machine. This
// is a substantial function; the reconstruction below covers the core
// movement/control-mode decision for the states the reimplementation drives
// (travel, wander, escort-follow, hold, drift, disengage, defunct) and writes
// Ship.ai_control_mode accordingly. The high-level attack states now enter
// pursuit/engagement control modes using the reconstructed target slots;
// weapon selection, cloak engagement, and HUD/mission flavor remain deferred.
void NovaAi_UpdateShipState(GameState &state,
                            Ship &ship,
                            std::uint32_t now_ms) {
  (void)now_ms;
  auto *scn = &state.scenario;

  // ---- Defunct (0x16) unwind. ----
  if (ship.ai_state_code == 0x16) {
    if (ship.reverse_speed_bias <= 0.0F) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      return;
    }
    ship.ai_fire_trigger_latch = 0;
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = -1;
    ship.ai_control_mode = 1;
    return;
  }

  // Coast-through-reversal (reverse_speed_bias > 0) only suspends the machine
  // for states other than 10 and 0xe (those keep running through the coast).
  if (ship.reverse_speed_bias > 0.0F && ship.ai_state_code != 10 &&
      ship.ai_state_code != 0xe) {
    return;
  }

  // Clear a dead primary target.
  if (ship.primary_target_ship_slot != -1 &&
      !state.SlotInRange(
          static_cast<std::size_t>(ship.primary_target_ship_slot))) {
    ship.primary_target_ship_slot = -1;
  } else if (ship.primary_target_ship_slot != -1 &&
             !NovaAiShip_IsDestroyed(state.ShipAt(
                 static_cast<std::size_t>(ship.primary_target_ship_slot)))) {
    // keep
  } else if (ship.primary_target_ship_slot != -1) {
    ship.primary_target_ship_slot = -1;
  }

  // The station-hold timer (states 0xb/2/3) is the only one that persists it.
  {
    const std::int16_t st = ship.ai_state_code;
    if (st != 0xb && st != 2 && st != 3) {
      ship.ai_station_hold_timer = 0.0F;
    }
  }
  if (ship.ai_station_hold_timer > 0.0F) {
    if (ship.ai_target_ship_slot == 0) {
      ship.ai_state_code = 0xb;
    } else if (ship.primary_target_ship_slot == -1) {
      ship.ai_state_code = 2;
    } else {
      ship.ai_state_code = 3;
    }
  }

  // Behavior-5 escort holding state 0xb: if the ship can't initiate a jump,
  // downgrade to pursue (5) and target the lead. Gated on this ship's own class
  // fuel.
  if (ship.ai_behavior_code == 5 && ship.ai_state_code == 0xb) {
    if (!NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
      ship.ai_state_code = 5;
      ship.primary_target_ship_slot = -1;
      ship.ai_secondary_target_slot = ship.ai_target_ship_slot;
    }
  }

  // Ghidra's state-4/state-0xd cloak-engagement gate. Once an attacker has
  // crossed its own cloak visibility threshold, it may only keep this target
  // if the target remains engageable at close range. Higher-behavior ships
  // brake and wait for a bounded patience interval; ordinary ships either hold
  // at the centre or fall back to their jump-capable idle template.
  if (ship.primary_target_ship_slot != -1 &&
      (ship.ai_state_code == 4 || ship.ai_state_code == 0xd) &&
      state.SlotInRange(
          static_cast<std::size_t>(ship.primary_target_ship_slot))) {
    const Ship &target =
        state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot));
    // The original tests the primary target's cloak threshold against this
    // ship's scanner/position context; the argument order is subject, ship.
    if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, target, ship)) {
      if (ship.ai_behavior_code > 2) {
        ship.ai_control_mode = 1;
        ship.ai_station_hold_timer = 0.0F;
        ship.ai_secondary_target_slot = -1;
        if (!std::isfinite(ship.target_engagement_patience_timer) ||
            ship.target_engagement_patience_timer <= 0.0F) {
          ship.target_engagement_patience_timer = static_cast<float>(
              std::uniform_int_distribution<int>{0, 99}(state.rng) + 100);
        } else {
          constexpr float kOriginalFrameTimeMs = 1000.0F / 30.0F;
          ship.target_engagement_patience_timer -= kOriginalFrameTimeMs;
          if (ship.target_engagement_patience_timer <= 0.0F) {
            ship.primary_target_ship_slot = -1;
            ship.ai_state_code = 0;
            ship.ai_control_mode = 0;
            ship.target_engagement_patience_timer = -1.0F;
          }
        }
        return;
      }
      if (!NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
        ship.ai_state_code = 6;
      } else {
        NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
      }
      return;
    }
    ship.target_engagement_patience_timer = -1.0F;
  }

  // ---- Travel to system (state 1). ----
  if (ship.ai_state_code == 1 && ship.ai_secondary_target_slot != -1) {
    const Stellar *target =
        StellarByResourceId(state, ship.ai_secondary_target_slot);
    if (!target || (target->availability_flags & 0x3000) != 0) {
      // Restricted travel stellar (hypergate/wormhole): begin a jump sequence.
      ship.ai_state_code = 0x14;
    } else {
      // Distances to the travel stellar.
      const float dx = static_cast<float>(target->pos_x) - ship.pos_x;
      const float dy = static_cast<float>(target->pos_y) - ship.pos_y;
      // Compute the turn-radius arrive range, mirroring Ship_UpdateShipAiState
      // (0x00405590): range = (9.0 - min(Ship_ComputeShipMaxTurnRateDeg,
      // 8.0)) * 8.0 + 32.0 px per axis. As long as the offset is larger than
      // it, keep steering toward the stellar (control mode 2 travel).
      const auto *cls =
          scn->Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      const float turn_deg =
          cls ? static_cast<float>(cls->turn_rate) * 0.1F
              : 0.0F; // Ship_ComputeShipMaxTurnRateDeg NPC branch
      const float arrive_range =
          (kArriveRangeBase - std::min(turn_deg, kArriveRangeTurnCap)) *
              kArriveRangeScale +
          kArriveRangeOffset;
      const bool outside_range =
          std::abs(dx) > arrive_range || std::abs(dy) > arrive_range;
      if (outside_range) {
        ship.ai_control_mode = 2; // travel: steer toward map coords
      } else {
        // Arrived: damp residual velocity, or stop and (re)arm the departure
        // coast-through-reversal timer to fly off once the ship settles. The
        // velocity damp and "stopped" threshold match the original's
        // _DAT_00575088 (0.98) / _DAT_00575080 (0.35 px/tick).
        const ShipClass *sc2 = cls;
        const bool has_arrival_marker =
            sc2 && (sc2->sprite_behavior_flags & 2) != 0;
        if (has_arrival_marker) {
          ship.waypoint_arrival_marker_a = -1;
        }
        if (std::abs(ship.vel_x) >= kVerySlowSpeed ||
            std::abs(ship.vel_y) >= kVerySlowSpeed) {
          ship.ai_control_mode = 1; // damp velocity toward stop
          ship.vel_x *= kArrivalDamp;
          ship.vel_y *= kArrivalDamp;
          ship.speed *= kArrivalDamp;
        } else {
          // Fully stopped: record the arrival target (for a potential jump)
          // and re-enter idle so the wander supervisor picks a new destination.
          ship.vel_x = 0.0F;
          ship.vel_y = 0.0F;
          ship.speed = 0.0F;
          ship.jump_destination_stellar_id = ship.ai_secondary_target_slot;
          ship.ai_control_mode = 0;
          ship.ai_state_code = 0;
          // Arm the coast-through-reversal timer so the next wander leg starts
          // with the ship "breaking away". Ghidra draws
          // NovaRandom_Range(200)+300 (300..499) for open-space ships, or
          // NovaRandom_Range(0x4b)+100 (100..174) for route-limited
          // (availability_flags bit 2) ships.
          std::uniform_int_distribution<std::int32_t> dist(
              (sc2 && (sc2->availability_flags & 2) != 0) ? 100 : 300,
              (sc2 && (sc2->availability_flags & 2) != 0) ? 174 : 499);
          ship.reverse_speed_bias = static_cast<float>(dist(state.rng));
        }
      }
    }
  }

  // ---- Jumping to a system (state 0x14). ----
  if (ship.ai_state_code == 0x14 && ship.ai_secondary_target_slot >= 0 &&
      ship.ai_secondary_target_slot < 0x800) {
    const Stellar *target =
        StellarByResourceId(state, ship.ai_secondary_target_slot);
    if (!target) {
      return;
    }
    ship.jump_destination_stellar_id = ship.ai_secondary_target_slot;
    ship.ai_station_hold_timer = 0.0F;
    const float dx = static_cast<float>(target->pos_x) - ship.pos_x;
    const float dy = static_cast<float>(target->pos_y) - ship.pos_y;
    const System *sys = CurrentSystem(state);
    const std::int16_t half_span =
        sys ? static_cast<std::int16_t>(sys->pos_x > 0 ? 0x96 : 0x96)
            : 0x96; // System_GetCurrentSystemLinkHalfSpan fallback
                    // (provisional)
    const std::int16_t span_q = static_cast<std::int16_t>(half_span / 4);
    const bool far = std::abs(dx) > span_q || std::abs(dy) > span_q;
    if (far) {
      ship.ai_control_mode = 2;
      ship.reverse_speed_bias = -1.0F;
    } else {
      // Arrived at the jump point: hold, clear velocity, and propagate the jump
      // to any ships actively escorting/tracking this one (Phase 7 wiring).
      ship.vel_x = 0.0F;
      ship.vel_y = 0.0F;
      ship.ai_control_mode = 0x17;
      if (!(ship.reverse_speed_bias >= 0.0F) ||
          ship.reverse_speed_bias > 16.0F) {
        ship.reverse_speed_bias = 16.0F;
      }
      // Escort propagation (Ship_UpdateShipAiState state 0x14 successor loop):
      // any active ship whose ai_target_ship_slot == this ship is ordered to
      // jump with it. Provisionally wired here (Phase 7).
      for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
        Ship &other = state.ShipAt(slot);
        if (!other.is_active ||
            other.ship_instance_id == ship.ship_instance_id) {
          continue;
        }
        if (other.ai_target_ship_slot != ship.ship_instance_id) {
          continue;
        }
        other.ai_target_ship_slot = -1;
        other.ai_behavior_code = ship.ai_behavior_code;
        if (other.ai_behavior_code > 3) {
          other.ai_behavior_code = 3;
        }
        other.target_stellar_object_id = -1;
        other.ai_hostility_accumulator = -1;
        other.primary_target_ship_slot = -1;
        other.ai_secondary_target_slot = ship.ai_secondary_target_slot;
        other.ai_state_code = 0x14;
        other.ai_control_mode = 0;
      }
    }
    return;
  }

  // ---- Disengage (0x15) -> stationary cleanup (8). ----
  if (ship.ai_state_code == 0x15) {
    ship.ai_control_mode = 0;
    ship.primary_target_ship_slot = -1;
    if (ship.reverse_speed_bias <= 0.0F) {
      ship.reverse_speed_bias = -1.0F;
      ship.ai_state_code = 8;
      ship.ai_secondary_target_slot = -1;
    }
    return;
  }

  // ---- Idle-template / approach station-keeping (state 2). ----
  if (ship.ai_state_code == 2) {
    // At system centre -> approach/stop; else station-keep or drift to it.
    // The "stopped" test uses the same 0.35 px/tick threshold as the original
    // (_DAT_00575080); the special-loadout arm of the original's mode-4 branch
    // is not modelled (TODO(decomp): Ship_CheckSpecialLoadoutCapability).
    if (SquaredDistance(0.0F, 0.0F, ship.pos_x, ship.pos_y) <= kCentreRangeSq) {
      ship.ai_control_mode = 3;
    } else if (std::abs(ship.vel_x) < kVerySlowSpeed &&
               std::abs(ship.vel_y) < kVerySlowSpeed) {
      ship.ai_control_mode = 4;
    } else {
      ship.ai_control_mode = 1;
    }
    return;
  }

  // ---- Hold-station / follow target (state 0xb). ----
  if (ship.ai_state_code == 0xb) {
    const std::int16_t target = ship.ai_target_ship_slot;
    if (target == -1) {
      ship.primary_target_ship_slot = -1;
      ship.ai_secondary_target_slot = -1;
      ship.ai_state_code = 2;
      ship.ai_control_mode = 4;
    } else if (target == 0) {
      // Hold near the player.
      if (state.player.ai_station_hold_timer <= 0.0F) {
        ship.primary_target_ship_slot = -1;
        ship.ai_secondary_target_slot = -1;
        ship.ai_state_code = 0;
        ship.ai_control_mode = 0;
      } else {
        ship.ai_secondary_target_slot = state.player.ai_secondary_target_slot;
        ship.primary_target_ship_slot = -1;
        ship.ai_control_mode = 0xd; // hold formation position
      }
    } else {
      // Follow the target at hold distance.
      const Ship &tgt = state.ShipAt(static_cast<std::size_t>(target));
      if (tgt.ai_state_code != 0 &&
          (tgt.ai_control_mode == 1 || tgt.ai_control_mode == 4)) {
        ship.ai_secondary_target_slot = tgt.ai_secondary_target_slot;
        ship.primary_target_ship_slot = -1;
        ship.ai_control_mode = 0xd;
      } else {
        ship.primary_target_ship_slot = -1;
        ship.ai_secondary_target_slot = -1;
        ship.ai_state_code = 0;
        ship.ai_control_mode = 0;
      }
    }
    return;
  }

  // ---- Drift / evade (state 0xe). ----
  if (ship.ai_state_code == 0xe) {
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = -1;
    ship.ai_control_mode = 0;
    // Continue drifting along the current heading at a small polar-velocity
    // step until the coast-through-reversal timer expires.
    ship.vel_x += std::sin(ship.heading) * (2.0F);
    ship.vel_y -= std::cos(ship.heading) * (2.0F);
    if (ship.reverse_speed_bias <= 0.0F) {
      ship.ai_state_code = 0;
    }
    return;
  }

  // ---- Pursuit (state 5) toward ai_target_ship_slot. ----
  if (ship.ai_state_code == 5 && ship.ai_target_ship_slot != -1 &&
      ship.target_stellar_object_id == -1) {
    ship.ai_secondary_target_slot = ship.ai_target_ship_slot;
    const auto *cls =
        scn->Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    const float base_turn = cls ? cls->turn_rate * 0.1F : 0.0F;
    const std::int16_t range = static_cast<std::int16_t>(std::max(
        100,
        static_cast<int>((kTurnRadiusBase - base_turn) * kTurnRadiusScale50)));
    const Ship &tgt =
        state.ShipAt(static_cast<std::size_t>(ship.ai_target_ship_slot));
    if (std::abs(ship.pos_x - tgt.pos_x) > range ||
        std::abs(ship.pos_y - tgt.pos_y) > range) {
      ship.ai_control_mode = 0xb;
    } else {
      ship.ai_control_mode = 8;
    }
    // Cloak-engagement downgrade: without the cloak-aware predicate the ship
    // boosts straight in (TODO: Ship_CanShipEngageTargetUnderCloakRules).
    return;
  }

  // ---- Assistance/response (state 10) toward ai_target_ship_slot. ----
  if (ship.ai_state_code == 10 && ship.ai_target_ship_slot != -1 &&
      ship.target_stellar_object_id == -1) {
    ship.ai_secondary_target_slot = ship.ai_target_ship_slot;
    const Ship &tgt =
        state.ShipAt(static_cast<std::size_t>(ship.ai_target_ship_slot));
    if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, tgt, ship)) {
      const bool far_from_target =
          std::abs(ship.pos_x - tgt.pos_x) > kAssistFar ||
          std::abs(ship.pos_y - tgt.pos_y) > kAssistFar;
      ship.ai_control_mode = far_from_target ? 9 : 1;
      return;
    }
    const bool far = std::abs(ship.pos_x - tgt.pos_x) > kAssistFar ||
                     std::abs(ship.pos_y - tgt.pos_y) > kAssistFar;
    const bool close = std::abs(ship.pos_x - tgt.pos_x) > kAssistClose ||
                       std::abs(ship.pos_y - tgt.pos_y) > kAssistClose;
    if (far) {
      ship.ai_control_mode = 9; // pursue at long range
    } else if (close) {
      ship.ai_control_mode = 1; // approach
    } else {
      ship.ai_control_mode = 0xc; // engage
    }
    return;
  }

  // ---- Follow/hold (state 6): damp toward stop. ----
  if (ship.ai_state_code == 6) {
    ship.ai_station_hold_timer = 0.0F;
    ship.ai_control_mode = 1;
    return;
  }

  // ---- Escort/follow primary at range (state 7). ----
  if (ship.ai_state_code == 7) {
    if (ship.primary_target_ship_slot == -1 || ship.reverse_speed_bias > 0.0F) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      return;
    }
    const std::int16_t target = ship.primary_target_ship_slot;
    if (!state.SlotInRange(static_cast<std::size_t>(target)) ||
        !state.ShipAt(static_cast<std::size_t>(target)).is_active) {
      ship.primary_target_ship_slot = -1;
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      return;
    }
    const Ship &tgt = state.ShipAt(static_cast<std::size_t>(target));
    if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, tgt, ship)) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      ship.primary_target_ship_slot = -1;
      return;
    }
    if (target == 0) {
      // Escorting the player: match hold distance, else pursue.
      if (std::abs(ship.pos_x - tgt.pos_x) > kEngageDist ||
          std::abs(ship.pos_y - tgt.pos_y) > kEngageDist) {
        ship.ai_control_mode = 9;
      } else {
        // Within escort distance: hold formation position.
        ship.ai_secondary_target_slot = -1;
        ship.primary_target_ship_slot = -1;
        ship.ai_state_code = 0;
        ship.ai_control_mode = 0;
      }
      return;
    }
    if (std::abs(ship.pos_x - tgt.pos_x) > kEngageDist ||
        std::abs(ship.pos_y - tgt.pos_y) > kEngageDist) {
      ship.ai_control_mode = 9;
    } else {
      ship.ai_state_code = 0;
      ship.primary_target_ship_slot = -1;
    }
    return;
  }

  // ---- Stationary cleanup (state 8). ----
  if (ship.ai_state_code == 8) {
    ship.ai_station_hold_timer = -999.0F;
    ship.ai_control_mode = 10;
    return;
  }

  // ---- Disabled/hidden pursuer-flee at turn radius (state 0xf). ----
  if (ship.ai_state_code == 0xf && ship.primary_target_ship_slot != -1) {
    if (!state.SlotInRange(
            static_cast<std::size_t>(ship.primary_target_ship_slot)) ||
        !NovaAiShip_IsFireRestricted(state,
                                     state.ShipAt(static_cast<std::size_t>(
                                         ship.primary_target_ship_slot)))) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      ship.primary_target_ship_slot = -1;
      ship.ai_secondary_target_slot = -1;
      return;
    }
    ship.ai_secondary_target_slot = ship.primary_target_ship_slot;
    const auto *cls =
        scn->Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    const float base_turn = cls ? cls->turn_rate * 0.1F : 0.0F;
    const std::int16_t range = static_cast<std::int16_t>(
        (kTurnRadiusBase - base_turn) * kTurnRadiusScale50 + 50.0F);
    const Ship &tgt =
        state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot));
    // Gravity-shield ships scale the keeping range up (they drift at a larger
    // turn radius).
    const bool gravity_shield_0f = cls && NovaShip_HasGravityShield(ship, *cls);
    const float full = gravity_shield_0f ? range * kShieldKeepMult : range;
    ship.ai_control_mode = (std::abs(ship.pos_x - tgt.pos_x) > full ||
                            std::abs(ship.pos_y - tgt.pos_y) > full)
                               ? 0xb
                               : 0xf;
    return;
  }

  // ---- Scripted/invulnerable maneuver (state 0x10) and static hold (0x11)
  // reference the asteroid/scripted targets; without those systems we keep
  // them static as control modes 0x13/0x14 (unhittable drift). ----
  if (ship.ai_state_code == 0x10) {
    ship.ai_control_mode = 0x13;
    return;
  }
  if (ship.ai_state_code == 0x11) {
    ship.ai_control_mode = 0x15;
    return;
  }

  // ---- Attack a stellar system (state 0x12): keep an engagement distance.
  // ----
  if (ship.ai_state_code == 0x12) {
    // No weapon banks are reconstructed yet (Phase 5), so the ship holds its
    // current range and lets the player/combat systems advance it later.
    if (std::abs(ship.vel_x) >= kVerySlowSpeed ||
        std::abs(ship.vel_y) >= kVerySlowSpeed) {
      ship.ai_control_mode = 1;
    } else {
      ship.ai_control_mode = 0x16;
    }
    ship.vel_x *= kArrivalDamp;
    ship.vel_y *= kArrivalDamp;
    ship.speed *= kArrivalDamp;
    return;
  }

  // ---- Attack / engagement states (3/4). ----
  if (ship.ai_state_code == 3 || ship.ai_state_code == 4) {
    if (ship.primary_target_ship_slot == -1 ||
        !state.SlotInRange(
            static_cast<std::size_t>(ship.primary_target_ship_slot))) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      ship.ai_hostility_accumulator = 0;
      return;
    }
    const Ship &target =
        state.ShipAt(static_cast<std::size_t>(ship.primary_target_ship_slot));
    if (!target.is_active || NovaAiShip_IsDestroyed(target)) {
      ship.primary_target_ship_slot = -1;
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      ship.ai_hostility_accumulator = 0;
      return;
    }

    const float dx = std::abs(ship.pos_x - target.pos_x);
    const float dy = std::abs(ship.pos_y - target.pos_y);
    ship.ai_secondary_target_slot = ship.primary_target_ship_slot;
    if (ship.ai_state_code == 3) {
      if (ship.ai_station_hold_timer > 0.0F) {
        if (SquaredDistance(0.0F, 0.0F, ship.pos_x, ship.pos_y) <=
            kCentreRangeSq) {
          ship.ai_station_hold_timer = 0.0F;
          ship.ai_control_mode = 3;
        } else {
          ship.ai_control_mode = 4;
        }
      } else if (dx < kCombatStationRange && dy < kCombatStationRange &&
                 ship.ai_control_mode != 4) {
        ship.ai_control_mode = 5;
        if (ship.faction_or_government_id >= 0 &&
            ship.ai_target_ship_slot != 0 &&
            target.faction_or_government_id >= 0 &&
            NovaGovernment_AreGovtsAllied(state.scenario,
                                          ship.faction_or_government_id,
                                          target.faction_or_government_id) &&
            target.ai_target_ship_slot == 0) {
          ship.ai_state_code = 0;
          ship.ai_control_mode = 0;
          ship.primary_target_ship_slot = -1;
          return;
        }
      } else if (SquaredDistance(0.0F, 0.0F, ship.pos_x, ship.pos_y) <=
                     kCentreRangeSq ||
                 !NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
        ship.ai_control_mode = 3;
      } else if (std::abs(ship.vel_x) < kVerySlowSpeed &&
                 std::abs(ship.vel_y) < kVerySlowSpeed) {
        ship.ai_control_mode = 4;
      } else {
        ship.ai_control_mode = 1;
      }
    } else {
      if (ship.reverse_speed_bias > 0.0F) {
        ship.ai_state_code = 0;
        ship.ai_control_mode = 0;
        return;
      }
      if (ship.ai_target_ship_slot != -1 &&
          ship.ai_target_ship_slot == ship.primary_target_ship_slot) {
        ship.ai_state_code = 0;
        ship.ai_control_mode = 0;
        ship.primary_target_ship_slot = -1;
        return;
      }
      if (dx > kCombatCloseRange || dy > kCombatCloseRange) {
        if (NovaAiShip_CanInterceptCurrentPrimaryTarget(state, ship)) {
          if (ship.ai_behavior_code < 3) {
            ship.ai_state_code = 3;
            ship.ai_control_mode = 5;
          } else {
            ship.ai_control_mode = 0xe;
          }
        } else if (ship.ai_control_mode != 0x11) {
          ship.ai_control_mode = 7;
        }
      } else if (ship.ai_control_mode != 0x10) {
        ship.ai_control_mode = 6;
      }
    }
    return;
  }

  // ---- Assist/response combat approach (state 0xc). ----
  if (ship.ai_state_code == 0xc) {
    ship.ai_station_hold_timer = 0.0F;
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = 0;
    const Ship &player = state.player;
    const auto *cls = ShipClassFor(state, ship);
    const float turn = cls ? cls->turn_rate * 0.1F : 0.0F;
    const float outer = (kTurnRadiusBase - turn) * kAssistTurnRadiusScale;
    const float inner = (kTurnRadiusBase - turn) * kAssistInnerTurnRadiusScale;
    const float dx = std::abs(ship.pos_x - player.pos_x);
    const float dy = std::abs(ship.pos_y - player.pos_y);
    if (state.player.ai_station_hold_timer > 0.0F) {
      ship.ai_state_code = 0xb;
    } else if (!NovaAiShip_CanEngageTargetUnderCloakRules(
                   state, state.player, ship)) {
      ship.ai_control_mode = 1;
    } else if (dx > outer || dy > outer) {
      ship.ai_control_mode = (dx > inner || dy > inner) ? 9 : 0xb;
    } else {
      ship.ai_control_mode = 0xc;
    }
    return;
  }

  // ---- Boarding / disabled-target pursuit (state 0xd). ----
  if (ship.ai_state_code == 0xd) {
    ship.ai_station_hold_timer = 0.0F;
    const std::int16_t target_slot = ship.primary_target_ship_slot;
    if (target_slot < 0 || ship.reverse_speed_bias > 0.0F ||
        !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      return;
    }
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    if (ship.ai_target_ship_slot != -1 &&
        (target_slot == ship.ai_target_ship_slot ||
         target.ai_target_ship_slot == ship.ai_target_ship_slot)) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      ship.primary_target_ship_slot = -1;
      return;
    }
    if (!target.is_active || NovaAiShip_IsDestroyed(target)) {
      ship.primary_target_ship_slot = -1;
      ship.ai_secondary_target_slot = -1;
      ship.ai_hostility_accumulator = 0;
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      return;
    }
    ship.ai_secondary_target_slot = -1;
    const float dx = std::abs(ship.pos_x - target.pos_x);
    const float dy = std::abs(ship.pos_y - target.pos_y);
    if (!NovaAiShip_IsFireRestricted(state, target)) {
      if (dx > kCombatCloseRange || dy > kCombatCloseRange) {
        if (NovaAiShip_CanInterceptCurrentPrimaryTarget(state, ship)) {
          ship.ai_control_mode = ship.ai_behavior_code < 3 ? 5 : 0xe;
        } else if (ship.ai_control_mode != 0x11) {
          ship.ai_control_mode = 7;
        }
      } else if (ship.ai_control_mode != 0x10 && ship.ai_control_mode != 0x11) {
        ship.ai_control_mode = 6;
      }
    } else if (target.escort_rehired_mark == 0 ||
               ship.reverse_speed_bias > 0.0F) {
      ship.ai_secondary_target_slot = target_slot;
      const auto *cls = ShipClassFor(state, ship);
      const float turn = cls ? cls->turn_rate * 0.1F : 0.0F;
      float range = (kTurnRadiusBase - turn) * kAssistTurnRadiusScale;
      if (cls != nullptr && NovaShip_HasGravityShield(ship, *cls)) {
        range *= kShieldKeepMult;
      }
      if (dx > range || dy > range) {
        ship.ai_control_mode =
            (dx > range * 2.0F || dy > range * 2.0F) ? 9 : 0xb;
      } else {
        ship.ai_control_mode = 0xf;
      }
    } else {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      ship.primary_target_ship_slot = -1;
      ship.ai_secondary_target_slot = -1;
    }
    return;
  }

  if (ship.ai_state_code == 0) {
    ship.ai_control_mode = 0;
    return;
  }
}

// ---- Ghidra 0x00408150 Ship_ApplyShipAiControls : the movement bridge. ----
// Converts ship.ai_control_mode (written by the state machine each frame) into
// the concrete fields the integrator consumes: ai_desired_heading_deg (the
// bearing to steer), ai_desired_speed (scalar cruise speed) and
// ai_forward_thrust_cmd (whether to thrust this frame). The travel/arrive
// modes (0-3) are the ones that make NPCs wander; the attack/formation modes
// (5-0x17) are provisionally mapped onto the same steering primitive until
// the combat/formation systems land. The movement constants are now decoded
// from the Ghidra _DAT_00575xxx globals (see the constant block at the top).
void NovaAi_ApplyControls(GameState &state,
                          Ship &ship,
                          float frame_time_ms,
                          std::uint32_t now_ms) {
  (void)frame_time_ms;
  ship.ai_forward_thrust_cmd = 0.0F;
  ship.ai_fire_trigger_latch = 0;
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  // Effective stats via the shared helper (NPC branch of
  // Ship_ComputeShipEffectiveThrust / Ship_ComputeShipEffectiveMaxSpeed /
  // Ship_ComputeShipMaxTurnRateDeg): class base values, government
  // combat_rating_scale applied to speed/thrust, no outfit/status yet. Same
  // derivation as NovaShip_IntegrateNpcMovement.
  const NpcEffectiveStats eff =
      cls ? NovaShip_ComputeEffectiveStats(state, ship, *cls)
          : NpcEffectiveStats{};
  const float max_speed = eff.max_speed_px_per_tick;
  const float eff_thrust = eff.thrust_px_per_tick2;
  const float eff_turn_deg = eff.turn_rate_deg_per_tick;

  // Ship_ApplyShipAiControls entry: any non-negative ai_desired_speed carried
  // over from the previous frame is reset to the full cruise speed (the
  // original tests the 0x575008 zero sentinel). Modes that never write
  // desired themselves (mode-1 braking) rely on this reset to hand the
  // integrator an eff_max_speed cap.
  if (ship.ai_desired_speed >= 0.0F) {
    ship.ai_desired_speed = max_speed;
  }

  // Shortest signed angle (deg) from the current heading to the desired one.
  auto heading_delta_deg = [&]() {
    const float cur_deg = ship.heading / kDegToRad;
    return std::remainder(static_cast<float>(ship.ai_desired_heading_deg) -
                              cur_deg,
                          kFullCircleDeg);
  };

  // Math_AddPolarVelocity (0x0043b4a0): adds speed along a game-convention
  // bearing (0 = up, clockwise) to an XY pair.
  auto add_polar_step = [](float &x, float &y, float bearing_deg, float speed) {
    const float rad = bearing_deg * kDegToRad;
    x += std::sin(rad) * speed;
    y -= std::cos(rad) * speed;
  };

  // The combat modes read the primary target slot, falling back to the
  // companion AI slot (Ship_ApplyShipAiControls reads ship[0x1c] then
  // ship[0x1b]). Returns -1 when neither is set or the slot is out of range.
  auto combat_target_slot = [&]() {
    std::int16_t slot = ship.primary_target_ship_slot;
    if (slot == -1) {
      slot = ship.ai_secondary_target_slot;
    }
    if (slot < 0 || !state.SlotInRange(static_cast<std::size_t>(slot))) {
      return std::int16_t{-1};
    }
    return slot;
  };

  // Ship_IsShipFireRestricted gate: the original skips every combat/behavior
  // mode for fire-restricted ships (they fall through with thrust 0).
  const bool fire_restricted = NovaAiShip_IsFireRestricted(state, ship);

  // The mode-0x6 evasive-break player gate (NovaAi_PlayerCombatRatingGate
  // 0x0046b330: a 1-in-0x540 roll beaten by the player's combat-rating
  // points). The clean-room does not track the player's combat rating, so the
  // gate conservatively fails (TODO(decomp): g_player_combat_rating_points).
  auto player_combat_rating_gate = [&]() { return false; };

  // Wrap a heading into [0, 360) using the original's 0x168-degree arithmetic.
  auto wrap_deg_int = [](int deg) {
    deg %= 360;
    if (deg < 0) {
      deg += 360;
    }
    return static_cast<std::int16_t>(deg);
  };

  switch (ship.ai_control_mode) {
  case 0:
  case 0x17:
    // Idle: the original has no mode-0 block (thrust stays 0, heading held).
    // Mode 0x17 (jump-arrival hold) also has no block in the original --
    // thrust stays 0.
    ship.ai_desired_speed = 0.0F;
    break;

  case 0x15:
    // Jump-in positioning: the original scans the 64 freeflight anchors for
    // the nearest, steers at it (thrust on align, else 1.75x-thrust damp), and
    // drops to control mode 0 when no anchor exists. No freeflight anchors are
    // modelled, so the original's no-anchor outcome (mode 0) holds; real jump
    // positioning awaits Phase 7.
    ship.ai_control_mode = 0;
    ship.ai_desired_speed = 0.0F;
    break;

  case 0x16:
    // Jump-out / retreat: steer at the target stellar's map coordinates and
    // select a weapon when aligned (weapon selection deferred, Phase 5). The
    // original never thrusts in this mode.
    if (!fire_restricted && ship.ai_secondary_target_slot >= 0x80) {
      const Stellar *st =
          StellarByResourceId(state, ship.ai_secondary_target_slot);
      if (st) {
        ship.ai_desired_heading_deg = static_cast<std::int16_t>(
            BearingDeg(ship.pos_x,
                       ship.pos_y,
                       static_cast<float>(st->pos_x),
                       static_cast<float>(st->pos_y)));
        if (std::abs(heading_delta_deg()) < eff_turn_deg + 15.0F) {
          // Weapon_SelectGeneralWeaponBank (deferred, Phase 5).
        }
      }
    }
    ship.ai_desired_speed = 0.0F;
    break;

  case 0x12: {
    // Chase the formation leader: head at the point 15x max-speed in front
    // of the leader's heading (Math_AddPolarVelocity on the leader position)
    // and thrust within turn+20 deg. With no leader the original falls back
    // to control mode 0; state-4 guided-weapon selection is deferred.
    if (fire_restricted) {
      break;
    }
    const std::int16_t leader_slot = ship.formation_leader_ship_slot;
    if (leader_slot < 1 ||
        !state.SlotInRange(static_cast<std::size_t>(leader_slot))) {
      ship.ai_control_mode = 0;
      break;
    }
    const Ship &leader = state.ShipAt(static_cast<std::size_t>(leader_slot));
    float tx = leader.pos_x;
    float ty = leader.pos_y;
    add_polar_step(tx, ty, leader.heading / kDegToRad, max_speed * 15.0F);
    ship.ai_desired_heading_deg =
        static_cast<std::int16_t>(BearingDeg(ship.pos_x, ship.pos_y, tx, ty));
    if (std::abs(heading_delta_deg()) < eff_turn_deg + 20.0F) {
      ship.ai_forward_thrust_cmd = eff_thrust;
    }
    if (ship.ai_state_code == 4 && ship.primary_target_ship_slot != -1) {
      // Weapon_SelectGuidedWeaponBankForPrimaryTarget (deferred, Phase 5).
    }
    break;
  }

  case 1: {
    // Slow to a stop (original mode 1). While any velocity component is above
    // 0.35 px/tick, steer toward the reverse of the velocity bearing and brake
    // with the raw effective thrust; once aligned and below 1.75 px/tick brake
    // at half thrust while damping velocity by 0.94; once below 0.35 px/tick
    // damp by 0.95 and drop to idle control mode 0. Gravity-shield ships brake
    // with a NEGATIVE thrust command (the integrator's scalar-speed path). The
    // state-code-9 not-yet-aligned damp is kept for parity.
    if (fire_restricted) {
      break;
    }
    if (std::abs(ship.vel_x) < kMode1FastThreshold &&
        std::abs(ship.vel_y) < kMode1FastThreshold) {
      ship.vel_x *= kMode1StopDamp;
      ship.vel_y *= kMode1StopDamp;
      ship.speed *= kMode1StopDamp;
      ship.ai_control_mode = 0;
      break;
    }
    if (cls == nullptr || !NovaShip_HasGravityShield(ship, *cls)) {
      ship.ai_desired_heading_deg = static_cast<std::int16_t>(
          WrapDeg(BearingDeg(0.0F, 0.0F, ship.vel_x, ship.vel_y) + 180.0F));
      if (std::abs(heading_delta_deg()) < eff_turn_deg + kMode1AlignAddend) {
        if (std::abs(ship.vel_x) >= kMode1StillFastThreshold ||
            std::abs(ship.vel_y) >= kMode1StillFastThreshold) {
          ship.ai_forward_thrust_cmd = eff_thrust;
        } else {
          ship.ai_forward_thrust_cmd = eff_thrust * kMode1SlowThrustFactor;
          ship.vel_x *= kMode1Damp;
          ship.vel_y *= kMode1Damp;
          ship.speed *= kMode1Damp;
        }
      } else if (ship.ai_state_code == 9) {
        ship.vel_x *= kMode1Damp;
        ship.vel_y *= kMode1Damp;
        ship.speed *= kMode1Damp;
      }
    } else {
      ship.ai_forward_thrust_cmd = -eff_thrust * kMode1SlowThrustFactor;
    }
    NovaAi_UpdateAutoWeaponSelectionFromTarget(state, ship);
    break;
  }

  case 2: {
    // Travel / pursue a map or ship coordinate in ai_secondary_target_slot (a
    // stellar resource id, or a ship slot). Steer the heading toward the
    // target; thrust only while the hull is within eff_turn_deg + 5.0 deg of
    // the bearing (the original's alignment gate -- the ship turns first, then
    // applies thrust once roughly aligned). Far from the stellar cruise at the
    // max-speed clamp (desired = 0); within 500 px per axis throttle to 25% of
    // max speed for the arrival slowdown.
    if (fire_restricted) {
      break;
    }
    float tx = ship.pos_x, ty = ship.pos_y;
    if (ship.ai_secondary_target_slot == 0) {
      const Ship &p = state.ShipAt(0);
      tx = p.pos_x;
      ty = p.pos_y;
    } else if (ship.ai_secondary_target_slot >= 0x80) {
      const Stellar *st =
          StellarByResourceId(state, ship.ai_secondary_target_slot);
      if (st) {
        tx = static_cast<float>(st->pos_x);
        ty = static_cast<float>(st->pos_y);
      }
    } else if (ship.ai_secondary_target_slot > 0 &&
               static_cast<std::size_t>(ship.ai_secondary_target_slot) <
                   GameState::kMaxShips) {
      const Ship &t =
          state.ShipAt(static_cast<std::size_t>(ship.ai_secondary_target_slot));
      tx = t.pos_x;
      ty = t.pos_y;
    }
    ship.ai_desired_heading_deg =
        static_cast<std::int16_t>(BearingDeg(ship.pos_x, ship.pos_y, tx, ty));
    if (std::abs(heading_delta_deg()) < eff_turn_deg + kMode2AlignAddend) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      if (std::abs(tx - ship.pos_x) < kMode2CloseGatePx &&
          std::abs(ty - ship.pos_y) < kMode2CloseGatePx) {
        ship.ai_desired_speed = max_speed * kMode2ArriveFraction;
      } else {
        ship.ai_desired_speed = 0.0F;
      }
    }
    // Not aligned: thrust stays 0 this frame (turn in place).
    break;
  }

  case 3:
    // Approach the system centre: point away from the centre and coast
    // (desired = 0 -> max-speed clamp in the integrator), thrusting once
    // aligned within eff_turn_deg + 3.0 deg.
    if (fire_restricted) {
      break;
    }
    ship.ai_desired_heading_deg = static_cast<std::int16_t>(
        BearingDeg(0.0F, 0.0F, ship.pos_x, ship.pos_y));
    if (std::abs(heading_delta_deg()) < eff_turn_deg + kMode3AlignAddend) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      ship.ai_desired_speed = 0.0F;
    }
    break;

  case 4:
    // Jump spin-up: point away from the centre and accumulate the hold timer.
    // The original's completion block (hold timer past the class-scaled jump
    // duration) zeroes the timer, clears the ship's system id and deactivates
    // it (the actual system transition is Phase 7), so that arm is not wired
    // yet (TODO(decomp)); the heading + timer ramp below are faithful.
    if (fire_restricted) {
      break;
    }
    ship.ai_desired_heading_deg = static_cast<std::int16_t>(
        BearingDeg(0.0F, 0.0F, ship.pos_x, ship.pos_y));
    if (ship.ai_station_hold_timer <= 0.0F) {
      ship.ai_station_hold_timer = 1.0F;
      ship.ai_mode_start_time_ms = now_ms;
    }
    if (ship.ai_station_hold_timer > 1.0F &&
        now_ms < ship.ai_mode_start_time_ms) {
      ship.ai_mode_start_time_ms = now_ms;
    }
    ship.ai_station_hold_timer += 1.0F;
    break;

  case 0xd: {
    // Formation hold with leader release (Ship_MoveShipTowardFormationOffset
    // 0x00414390 remains Phase 8). ai_target_ship_slot is the followed leader.
    // While the leader's station-hold timer runs (>1.0) the ship matches the
    // leader's spin-up: damp to a standstill (0.95, desired -4.0, timer +1)
    // once the leader is within 11 deg of its own desired heading; once the
    // ship's own hold timer passes 30 and the leader is not the player, the
    // ship releases back to its class default behavior (state 2 / mode 4).
    // Otherwise it copies the leader's velocity and glow (formation offset
    // deferred).
    if (fire_restricted) {
      break;
    }
    const std::int16_t leader_slot = ship.ai_target_ship_slot;
    if (leader_slot < 0 ||
        !state.SlotInRange(static_cast<std::size_t>(leader_slot))) {
      break;
    }
    const Ship &leader = state.ShipAt(static_cast<std::size_t>(leader_slot));
    const float leader_heading_deg = leader.heading / kDegToRad;
    ship.ai_desired_heading_deg =
        static_cast<std::int16_t>(std::round(leader_heading_deg));
    if (leader.ai_station_hold_timer > 1.0F) {
      if (ship.ai_station_hold_timer > 1.0F &&
          now_ms < ship.ai_mode_start_time_ms) {
        ship.ai_mode_start_time_ms = now_ms;
      }
      if (ship.ai_station_hold_timer > 30.0F && leader_slot != 0) {
        ship.ai_desired_heading_deg = leader.ai_desired_heading_deg;
        ship.ai_target_ship_slot = -1;
        ship.ai_behavior_code =
            cls ? cls->default_ai_behavior : ship.ai_behavior_code;
        ship.ai_state_code = 2;
        ship.ai_control_mode = 4;
        break;
      }
      const float leader_delta = std::abs(
          std::remainder(static_cast<float>(leader.ai_desired_heading_deg) -
                             std::round(leader_heading_deg),
                         kFullCircleDeg));
      if (leader_delta < 11.0F) {
        ship.ai_secondary_target_slot = leader.ai_secondary_target_slot;
        if (ship.ai_station_hold_timer == 0.0F) {
          ship.ai_station_hold_timer = 1.0F;
          ship.ai_mode_start_time_ms = now_ms;
        }
        ship.vel_x *= kMode1StopDamp;
        ship.vel_y *= kMode1StopDamp;
        ship.speed *= kMode1StopDamp;
        ship.ai_forward_thrust_cmd = 0.0F;
        ship.ai_desired_speed = -4.0F;
        ship.ai_station_hold_timer += 1.0F;
      } else if (leader_delta <= eff_turn_deg) {
        ship.reverse_speed_bias = 180.0F; // raw float 0x43340000
      } else {
        ship.ai_desired_heading_deg = leader.ai_desired_heading_deg;
        ship.ai_station_hold_timer = -4.0F;
      }
    } else {
      ship.vel_x = leader.vel_x;
      ship.vel_y = leader.vel_y;
      // Ship_MoveShipTowardFormationOffset (Phase 8) + glow copy:
      ship.engine_glow_level = leader.engine_glow_level;
    }
    break;
  }

  case 5: {
    // Pursue / maintain distance on the primary target. NOTE: the original's
    // bearing call is Math_BearingFromPointToPoint(target_pos, ship_pos) --
    // the REVERSE of every other combat mode -- so mode 5 steers away from
    // the target (the close-range "attack while backing off" strafe; the
    // state machine only selects it within the 251 px combat station range).
    // Formation-offset mirroring and the weapon-bank select for states 3/4 are
    // deferred. Close (165 px/axis) + the +0xBD latch breaks off to a boost
    // (0x11); the latch has no producer yet so the transition is inert.
    if (fire_restricted || ship.primary_target_ship_slot == -1) {
      break;
    }
    if (ship.formation_leader_ship_slot > 0 &&
        static_cast<std::size_t>(ship.formation_leader_ship_slot) <
            GameState::kMaxShips) {
      // Ship_MoveShipTowardFormationOffset (Phase 8).
      ship.engine_glow_level =
          state
              .ShipAt(static_cast<std::size_t>(ship.formation_leader_ship_slot))
              .engine_glow_level;
    }
    const std::int16_t target_slot = ship.primary_target_ship_slot;
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    ship.ai_desired_heading_deg = static_cast<std::int16_t>(
        BearingDeg(target.pos_x, target.pos_y, ship.pos_x, ship.pos_y));
    if (std::abs(heading_delta_deg()) < eff_turn_deg + 20.0F) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      ship.ai_desired_speed = 0.0F;
    }
    if (ship.ai_state_code == 3 || ship.ai_state_code == 4) {
      // Weapon_SelectWeaponBankForCurrentTarget (deferred, Phase 5).
    }
    if (ship.ai_brake_to_boost_latch != 0 &&
        (std::abs(ship.pos_x - target.pos_x) < kCombatCloseRange ||
         std::abs(ship.pos_y - target.pos_y) < kCombatCloseRange)) {
      ship.ai_control_mode = 0x11;
    }
    break;
  }

  case 6: {
    // Combat pursuit / lead-in: steer at the primary target (predictive aim
    // when a weapon bank is live is deferred, so the straight bearing stands
    // in), thrust once within turn+15 deg. A second turn*3 alignment gate
    // selects direct-fire weapons (deferred) and, for gravity-shield ships
    // within 100 px, throttles the cruise down to the target's scalar speed.
    // Break-off: at/inside 165 px (or any distance for non-shield ships) the
    // evasive-break order fires -- a player target must beat the (unmodelled)
    // combat-rating gate, non-player targets skip it half the time -- when the
    // ship is a light fighter (class default behavior > 2, mass < 200 t)
    // closing on a lighter/earlier target within 123 px with both hulls facing
    // each other within 31 deg; the evasive heading is current +/-135 deg by
    // instance-id parity (0x10). The 0xBD-latched boost break-off (0x11)
    // beyond 82 px is gated as in the original.
    if (fire_restricted || ship.primary_target_ship_slot == -1) {
      break;
    }
    const std::int16_t target_slot = ship.primary_target_ship_slot;
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    const float target_bearing_deg =
        BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);
    // Ship_AimWeaponPredictive: steer at the predicted intercept of the live
    // weapon bank (the original leads with the active bank or the +0xc8da
    // candidate; a -1 active bank has no weapon context, so the helper falls
    // back to the straight bearing below). Non-lead weapon modes return the
    // straight bearing too, matching the original mode-6 branch.
    ship.ai_desired_heading_deg = NovaAi_AimWeaponPredictive(
        state, ship, target, ship.active_weapon_bank_slot);
    // Weapon_SelectWeaponBankForCurrentTarget (deferred, Phase 5).
    if (std::abs(heading_delta_deg()) < eff_turn_deg + 15.0F) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      ship.ai_desired_speed = 0.0F;
    }
    if (std::abs(heading_delta_deg()) < eff_turn_deg * 3.0F) {
      // Weapon_SelectDirectFireWeaponBankForPrimaryTarget (deferred).
      const bool gravity_shield =
          cls != nullptr && NovaShip_HasGravityShield(ship, *cls);
      if (gravity_shield && std::abs(ship.pos_x - target.pos_x) < 100.0F &&
          std::abs(ship.pos_y - target.pos_y) < 100.0F &&
          target.speed < ship.speed) {
        ship.ai_desired_speed = target.speed;
      }
    }
    const float dx = std::abs(ship.pos_x - target.pos_x);
    const float dy = std::abs(ship.pos_y - target.pos_y);
    const bool gravity_shield =
        cls != nullptr && NovaShip_HasGravityShield(ship, *cls);
    if (dx < kCombatCloseRange || dy < kCombatCloseRange || !gravity_shield) {
      bool allowed = false;
      if (target_slot == 0) {
        allowed = player_combat_rating_gate();
      } else {
        std::uniform_int_distribution<int> coin(0, 1);
        if (coin(state.rng) == 0) {
          allowed = player_combat_rating_gate();
        } else {
          allowed = true;
        }
      }
      if (allowed && cls != nullptr && cls->default_ai_behavior > 2 &&
          cls->mass_tons < 200 && dx < kEvasiveOrderGatePx &&
          dy < kEvasiveOrderGatePx) {
        const ShipClass *target_cls = state.scenario.Ship(
            static_cast<std::int16_t>(target.ship_class_id + 0x80));
        const bool smaller_first =
            target_slot < ship.ship_instance_id ||
            (ship.ship_instance_id < target_slot && target_cls != nullptr &&
             cls->mass_tons < target_cls->mass_tons);
        if (smaller_first && std::abs(std::remainder(
                                 target_bearing_deg - ship.heading / kDegToRad,
                                 kFullCircleDeg)) < 31.0F) {
          const float target_heading_deg = target.heading / kDegToRad;
          const float facing_back = WrapDeg(target_bearing_deg + 180.0F);
          if (std::abs(std::remainder(facing_back - target_heading_deg,
                                      kFullCircleDeg)) < 31.0F) {
            ship.ai_control_mode = 0x10;
            const float cur_deg = ship.heading / kDegToRad;
            const bool even_instance = (ship.ship_instance_id & 1) == 0;
            ship.ai_evasive_heading_deg = wrap_deg_int(
                static_cast<int>(std::round(cur_deg) +
                                 (even_instance ? kEvasiveHeadingOffsetDeg
                                                : -kEvasiveHeadingOffsetDeg)));
            ship.ai_desired_heading_deg =
                wrap_deg_int(static_cast<int>(std::round(cur_deg)));
          }
        }
      }
    }
    if (ship.ai_brake_to_boost_latch != 0 && dx > kBreakOuterGatePx &&
        dy > kBreakOuterGatePx &&
        std::abs(std::remainder(target_bearing_deg - ship.heading / kDegToRad,
                                kFullCircleDeg)) < 31.0F) {
      ship.ai_control_mode = 0x11;
    }
    break;
  }

  case 0x10: {
    // Evasive break: fly the stored evasive heading (+/-135 deg) at 1.5x
    // thrust with cruise 0, then drop back to combat pursuit (6) once the
    // hull is within turn*3 deg of it (or, for gravity-shield ships, once the
    // target is beyond 165 px). Weapon selection at close range is deferred.
    if (fire_restricted || ship.primary_target_ship_slot == -1) {
      break;
    }
    const std::int16_t target_slot = ship.primary_target_ship_slot;
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    ship.ai_desired_heading_deg = ship.ai_evasive_heading_deg;
    ship.ai_forward_thrust_cmd = eff_thrust * kEvasiveThrustFactor;
    ship.ai_desired_speed = 0.0F;
    if (std::abs(ship.pos_x - target.pos_x) < kCombatCloseRange ||
        std::abs(ship.pos_y - target.pos_y) < kCombatCloseRange) {
      // Weapon_SelectWeaponBankForCurrentTarget (deferred, Phase 5).
    }
    const bool gravity_shield =
        cls != nullptr && NovaShip_HasGravityShield(ship, *cls);
    if (!gravity_shield) {
      if (std::abs(
              std::remainder(static_cast<float>(ship.ai_evasive_heading_deg) -
                                 ship.heading / kDegToRad,
                             kFullCircleDeg)) < eff_turn_deg * 3.0F) {
        ship.ai_control_mode = 6;
      }
    } else if (std::abs(ship.pos_x - target.pos_x) > kCombatCloseRange ||
               std::abs(ship.pos_y - target.pos_y) > kCombatCloseRange) {
      ship.ai_control_mode = 6;
    }
    break;
  }

  case 0x11: {
    // Boost to target: over-speed cruise (2.75x thrust, 1.8x max speed) on
    // the straight bearing, weapon selection once aligned (deferred), and a
    // return to combat pursuit (6) inside 165 px (or a 1-in-100 roll further
    // out). Formation-offset mirroring is deferred (Phase 8).
    if (fire_restricted || ship.primary_target_ship_slot == -1) {
      break;
    }
    const std::int16_t target_slot = ship.primary_target_ship_slot;
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    ship.ai_forward_thrust_cmd = eff_thrust * kBoostThrustFactor;
    ship.ai_desired_speed = max_speed * kBoostCruiseFactor;
    ship.ai_desired_heading_deg = static_cast<std::int16_t>(
        BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y));
    if (ship.formation_leader_ship_slot > 0 &&
        static_cast<std::size_t>(ship.formation_leader_ship_slot) <
            GameState::kMaxShips) {
      // Ship_MoveShipTowardFormationOffset (Phase 8).
      ship.engine_glow_level =
          state
              .ShipAt(static_cast<std::size_t>(ship.formation_leader_ship_slot))
              .engine_glow_level;
    }
    const float dx = std::abs(ship.pos_x - target.pos_x);
    const float dy = std::abs(ship.pos_y - target.pos_y);
    if (dx < kCombatCloseRange && dy < kCombatCloseRange) {
      // Weapon_SelectWeaponBankForCurrentTarget + direct-fire select
      // (deferred, Phase 5).
      if (std::abs(heading_delta_deg()) < eff_turn_deg * 3.0F) {
        // Weapon_SelectDirectFireWeaponBankForPrimaryTarget (deferred).
      }
    } else if (std::abs(heading_delta_deg()) < eff_turn_deg * 3.0F) {
      // Weapon_SelectGuidedWeaponBankForPrimaryTarget (deferred).
    }
    if (dx < kCombatCloseRange && dy < kCombatCloseRange) {
      ship.ai_control_mode = 6;
    } else {
      std::uniform_int_distribution<int> roll(0, 99);
      if (roll(state.rng) == 0) {
        ship.ai_control_mode = 6;
      }
    }
    break;
  }

  case 7: {
    // Combat strafe / guided approach: aim (predictive when a weapon bank is
    // live; deferred -> straight bearing), select weapons at the turn*3 gate
    // (deferred), thrust at the turn*4 gate, and break off to a boost (0x11)
    // beyond 82 px/axis when the 0xBD latch is set and the target bearing is
    // within 31 deg. Formation-offset mirroring is deferred (Phase 8).
    if (fire_restricted || ship.primary_target_ship_slot == -1) {
      break;
    }
    if (ship.formation_leader_ship_slot > 0 &&
        static_cast<std::size_t>(ship.formation_leader_ship_slot) <
            GameState::kMaxShips) {
      // Ship_MoveShipTowardFormationOffset (Phase 8).
      ship.engine_glow_level =
          state
              .ShipAt(static_cast<std::size_t>(ship.formation_leader_ship_slot))
              .engine_glow_level;
    }
    const std::int16_t target_slot = ship.primary_target_ship_slot;
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    const float target_bearing_deg =
        BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);
    // Mode-7 combat strafe: the original aims at the predicted intercept
    // (Ship_AimWeaponPredictive with the active bank weapon) whenever a
    // weapon candidate is present. Mirror mode 6 using the live bank; the
    // helper returns the straight bearing when no bank is armed.
    ship.ai_desired_heading_deg = NovaAi_AimWeaponPredictive(
        state, ship, target, ship.active_weapon_bank_slot);
    if (std::abs(heading_delta_deg()) < eff_turn_deg * 3.0F) {
      // Weapon_SelectDirectFireWeaponBankForPrimaryTarget +
      // Weapon_SelectWeaponBankForCurrentTarget (deferred, Phase 5).
    }
    if (std::abs(heading_delta_deg()) < eff_turn_deg * 4.0F) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      ship.ai_desired_speed = 0.0F;
      // Weapon_SelectGuidedWeaponBankForPrimaryTarget (deferred).
    }
    if (ship.ai_brake_to_boost_latch != 0 &&
        std::abs(ship.pos_x - target.pos_x) > kBreakOuterGatePx &&
        std::abs(ship.pos_y - target.pos_y) > kBreakOuterGatePx &&
        std::abs(std::remainder(target_bearing_deg - ship.heading / kDegToRad,
                                kFullCircleDeg)) < 31.0F) {
      ship.ai_control_mode = 0x11;
    }
    break;
  }

  case 8: {
    // Escort follow: steer at the escorted ship (ai_secondary_target_slot)
    // and thrust within turn+1 deg. Outside the class escort half-span the
    // ship creeps its position toward ai_target_ship_slot at 10x thrust per
    // frame and drops desired speed by the same step; inside, the original
    // launches/hands off the escort (deferred, Phase 8) or clears the escort
    // for a non-capturable player escort. A destroyed/fire-restricted ship or
    // a missing secondary target falls back to idle (state/control 0).
    const std::int16_t target_slot = ship.ai_secondary_target_slot;
    if (target_slot == -1) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      break;
    }
    if (NovaAiShip_IsDestroyed(ship) || fire_restricted) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      break;
    }
    if (!state.SlotInRange(static_cast<std::size_t>(target_slot))) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      break;
    }
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    if (!target.is_active ||
        target.current_system_id != ship.current_system_id) {
      ship.ai_behavior_code =
          cls ? cls->default_ai_behavior : ship.ai_behavior_code;
      ship.ai_state_code = 0;
      break;
    }
    ship.ai_desired_heading_deg = static_cast<std::int16_t>(
        BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y));
    if (std::abs(heading_delta_deg()) < eff_turn_deg + 1.0F) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      ship.ai_desired_speed = 0.0F;
    }
    if (kEscortHalfSpanPx < std::abs(target.pos_x - ship.pos_x) ||
        kEscortHalfSpanPx < std::abs(target.pos_y - ship.pos_y)) {
      // Creep toward the followed lead (ai_target_ship_slot) at 10x thrust.
      const float step = eff_thrust * kFormationCreepFactor * frame_time_ms;
      const std::int16_t lead_slot = ship.ai_target_ship_slot;
      if (lead_slot > 0 &&
          static_cast<std::size_t>(lead_slot) < GameState::kMaxShips) {
        const Ship &lead = state.ShipAt(static_cast<std::size_t>(lead_slot));
        if (ship.pos_x <= lead.pos_x - step) {
          ship.pos_x += step;
        } else if (lead.pos_x + step <= ship.pos_x) {
          ship.pos_x -= step;
        }
        if (ship.pos_y <= lead.pos_y - step) {
          ship.pos_y += step;
        } else if (lead.pos_y + step <= ship.pos_y) {
          ship.pos_y -= step;
        }
      }
      ship.ai_desired_speed -= step;
    } else {
      // Inside the escort half-span: the original launches the escort from
      // the carrier bay (or, for the player's slot, clears the escort when
      // the class cannot be captured). Both arms are deferred (Phase 8).
      if (ship.ship_instance_id == 0) {
        ship.ai_state_code = 0xc;
        ship.ai_control_mode = 0;
        ship.primary_target_ship_slot = -1;
        ship.ai_secondary_target_slot = -1;
      }
    }
    break;
  }

  case 9: {
    // Hold at distance: steer straight at the target while farther than
    // 200 px/axis; inside that range, steer at the velocity correction toward
    // a max-speed approach when the current velocity direction differs from
    // the target bearing by more than 15 deg. Thrust within turn+1 deg.
    if (fire_restricted) {
      break;
    }
    const std::int16_t target_slot = combat_target_slot();
    if (target_slot == -1 ||
        !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
      break;
    }
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    const float target_bearing_deg =
        BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);
    const float dx = std::abs(ship.pos_x - target.pos_x);
    const float dy = std::abs(ship.pos_y - target.pos_y);
    if (dx > kHoldApproachGatePx || dy > kHoldApproachGatePx) {
      ship.ai_desired_heading_deg =
          static_cast<std::int16_t>(target_bearing_deg);
    } else {
      const float vel_bearing_deg =
          BearingDeg(0.0F, 0.0F, ship.vel_x, ship.vel_y);
      if (std::abs(vel_bearing_deg - target_bearing_deg) > 15.0F) {
        // Delta toward the max-speed approach point.
        float px = 0.0F;
        float py = 0.0F;
        add_polar_step(px, py, target_bearing_deg, max_speed);
        const float dvx = px - ship.vel_x;
        const float dvy = py - ship.vel_y;
        if (std::abs(dvx) > kVerySlowSpeed || std::abs(dvy) > kVerySlowSpeed) {
          ship.ai_desired_heading_deg =
              static_cast<std::int16_t>(BearingDeg(0.0F, 0.0F, dvx, dvy));
        }
      }
    }
    if (std::abs(heading_delta_deg()) < eff_turn_deg + 1.0F) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      ship.ai_desired_speed = 0.0F;
    }
    NovaAi_UpdateAutoWeaponSelectionFromTarget(state, ship);
    break;
  }

  case 0xb: {
    // Formation hold: like mode 9 but the arrival throttle is 0.5x max speed
    // within 100 px/axis (not 0), and the formation-leader glow/offset mirror
    // runs first (offset deferred, Phase 8).
    if (fire_restricted) {
      break;
    }
    if (ship.formation_leader_ship_slot > 0 &&
        static_cast<std::size_t>(ship.formation_leader_ship_slot) <
            GameState::kMaxShips) {
      // Ship_MoveShipTowardFormationOffset (Phase 8).
      ship.engine_glow_level =
          state
              .ShipAt(static_cast<std::size_t>(ship.formation_leader_ship_slot))
              .engine_glow_level;
    }
    const std::int16_t target_slot = combat_target_slot();
    if (target_slot == -1 ||
        !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
      break;
    }
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    const float target_bearing_deg =
        BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);
    const float dx = std::abs(ship.pos_x - target.pos_x);
    const float dy = std::abs(ship.pos_y - target.pos_y);
    if (dx > kHoldApproachGatePx || dy > kHoldApproachGatePx) {
      ship.ai_desired_heading_deg =
          static_cast<std::int16_t>(target_bearing_deg);
    } else {
      const float vel_bearing_deg =
          BearingDeg(0.0F, 0.0F, ship.vel_x, ship.vel_y);
      if (std::abs(vel_bearing_deg - target_bearing_deg) > 15.0F) {
        float px = 0.0F;
        float py = 0.0F;
        add_polar_step(px, py, target_bearing_deg, max_speed);
        const float dvx = px - ship.vel_x;
        const float dvy = py - ship.vel_y;
        if (std::abs(dvx) > kVerySlowSpeed || std::abs(dvy) > kVerySlowSpeed) {
          ship.ai_desired_heading_deg =
              static_cast<std::int16_t>(BearingDeg(0.0F, 0.0F, dvx, dvy));
        }
      }
    }
    if (std::abs(heading_delta_deg()) < eff_turn_deg + 1.0F) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      if (dx > 100.0F || dy > 100.0F) {
        ship.ai_desired_speed = 0.0F;
      } else {
        ship.ai_desired_speed = max_speed * 0.5F;
      }
    }
    NovaAi_UpdateAutoWeaponSelectionFromTarget(state, ship);
    break;
  }

  case 0xc: {
    // Velocity match: copy the target's velocity and ease the desired heading
    // toward the target's heading by 1 deg/frame once outside the 25/skill
    // window (state 7 escorts also creep position toward the target's
    // 135-deg offset anchor at 10x thrust). While the relative velocity is
    // above 0.525 px/tick the ship instead brakes on the relative velocity at
    // 0.66x thrust, damping it by 0.94 once slow. Formation-offset mirroring
    // is deferred (Phase 8).
    if (fire_restricted || ship.ai_secondary_target_slot == -1) {
      break;
    }
    const std::int16_t target_slot = ship.ai_secondary_target_slot;
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    const float rel_x = ship.vel_x - target.vel_x;
    const float rel_y = ship.vel_y - target.vel_y;
    const bool gravity_shield =
        cls != nullptr && NovaShip_HasGravityShield(ship, *cls);
    if (gravity_shield || (std::abs(rel_x) < kVelocityMatchTol &&
                           std::abs(rel_y) < kVelocityMatchTol)) {
      ship.vel_x = target.vel_x;
      ship.vel_y = target.vel_y;
      const float target_heading_deg = target.heading / kDegToRad;
      const float cur_deg = ship.heading / kDegToRad;
      const float delta_deg = std::abs(
          std::remainder(std::round(target_heading_deg) - std::round(cur_deg),
                         kFullCircleDeg));
      float window = 25.0F;
      if (ship.skill_variance_scale > 0.0F) {
        window = 25.0F / ship.skill_variance_scale;
      }
      if (delta_deg < 1.0F || window <= delta_deg) {
        ship.ai_desired_heading_deg =
            static_cast<std::int16_t>(std::round(target_heading_deg));
      } else {
        const float signed_delta =
            std::remainder(std::round(target_heading_deg) - std::round(cur_deg),
                           kFullCircleDeg);
        std::int16_t desired = static_cast<std::int16_t>(std::round(cur_deg));
        desired =
            static_cast<std::int16_t>(desired + (signed_delta < 0.0F ? -1 : 1));
        ship.ai_desired_heading_deg = wrap_deg_int(desired);
      }
      if (ship.ai_state_code == 7 && target_slot != -1) {
        const float step = eff_thrust * kFormationCreepFactor * frame_time_ms;
        float anchor_x = target.pos_x;
        float anchor_y = target.pos_y;
        add_polar_step(anchor_x,
                       anchor_y,
                       WrapDeg(target_heading_deg + 135.0F),
                       kFormationOffsetRadiusPx);
        if (ship.pos_x <= anchor_x - step) {
          ship.pos_x += step;
        } else if (anchor_x + step <= ship.pos_x) {
          ship.pos_x -= step;
        }
        if (ship.pos_y <= anchor_y - step) {
          ship.pos_y += step;
        } else if (anchor_y + step <= ship.pos_y) {
          ship.pos_y -= step;
        }
      } else {
        // Ship_MoveShipTowardFormationOffset (Phase 8).
      }
      ship.engine_glow_level = target.engine_glow_level;
    } else {
      ship.ai_desired_heading_deg = static_cast<std::int16_t>(
          WrapDeg(BearingDeg(0.0F, 0.0F, rel_x, rel_y) + 180.0F));
      if (std::abs(heading_delta_deg()) < eff_turn_deg + 1.0F) {
        ship.ai_forward_thrust_cmd = eff_thrust * kVelMatchBrakeFactor;
        ship.ai_desired_speed = 0.0F;
      }
      if (std::abs(rel_x) < kMode1StillFastThreshold ||
          std::abs(rel_y) <= kMode1StillFastThreshold) {
        ship.vel_x = target.vel_x + rel_x * kMode1Damp;
        ship.vel_y = target.vel_y + rel_y * kMode1Damp;
      }
    }
    NovaAi_UpdateAutoWeaponSelectionFromTarget(state, ship);
    break;
  }

  case 0xf: {
    // Velocity-match pursuit of a disabled/secondary target: brake on the
    // RELATIVE velocity (mode-1 style, threshold 0.525 px/tick); once matched,
    // copy the target's velocity and heading and creep position toward it at
    // one frame-time unit per frame. The capture/boarding resolution that the
    // original performs once within 3 px (state 0xd/0xf arms) is deferred
    // (TODO(decomp): Outfit_BoardShipAndTransferCargo).
    if (fire_restricted || ship.ai_secondary_target_slot == -1) {
      break;
    }
    const std::int16_t target_slot = ship.ai_secondary_target_slot;
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    const float rel_x = ship.vel_x - target.vel_x;
    const float rel_y = ship.vel_y - target.vel_y;
    const bool gravity_shield =
        cls != nullptr && NovaShip_HasGravityShield(ship, *cls);
    if (std::abs(rel_x) >= kVelocityMatchTol ||
        std::abs(rel_y) >= kVelocityMatchTol) {
      if (!gravity_shield) {
        ship.ai_desired_heading_deg = static_cast<std::int16_t>(
            WrapDeg(BearingDeg(0.0F, 0.0F, rel_x, rel_y) + 180.0F));
        if (std::abs(heading_delta_deg()) < eff_turn_deg + 1.0F) {
          ship.ai_forward_thrust_cmd = eff_thrust;
          ship.ai_desired_speed = 0.0F;
        }
        if (std::abs(rel_x) < kMode1StillFastThreshold ||
            std::abs(rel_y) <= kMode1StillFastThreshold) {
          ship.vel_x = target.vel_x + rel_x * kMode1StopDamp;
          ship.vel_y = target.vel_y + rel_y * kMode1StopDamp;
        }
      } else {
        ship.ai_forward_thrust_cmd = 0.0F;
        ship.ai_desired_speed = 0.0F;
        if (ship.speed > 0.0F) {
          ship.speed = std::max(0.0F, ship.speed - eff_thrust * frame_time_ms);
        }
      }
    } else {
      ship.ai_desired_heading_deg =
          static_cast<std::int16_t>(std::round(target.heading / kDegToRad));
      ship.vel_x = target.vel_x;
      ship.vel_y = target.vel_y;
      const float dx = std::abs(ship.pos_x - target.pos_x);
      const float dy = std::abs(ship.pos_y - target.pos_y);
      if (dx > 3.0F || dy > 3.0F) {
        ship.speed = 0.0F;
        const float bearing =
            BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);
        add_polar_step(ship.pos_x, ship.pos_y, bearing, frame_time_ms);
      } else {
        // Capture resolution: with the target disabled and within 3 px the
        // original arms a 100..179-ms reverse-speed-bias timer (random 0x50 +
        // 100) before entering state 0xe or boarding via
        // Outfit_BoardShipAndTransferCargo. Deferred (TODO(decomp)).
      }
    }
    break;
  }

  case 0xe: {
    // Evade / break: while still moving (>= 0.35 px/tick) brake like mode 1
    // (reverse of the velocity bearing, or a predictive aim when a weapon
    // bank is live -- deferred to the straight bearing), thrust once within
    // turn+1 deg; gravity-shield ships reverse-thrust. Once slow, damp by 0.95
    // and steer at the target, selecting weapons within turn*3 deg (deferred).
    // The carrier-bay launch (Ship_LaunchShipFromCarrierBay) is deferred.
    if (fire_restricted || ship.primary_target_ship_slot == -1) {
      break;
    }
    const std::int16_t target_slot = ship.primary_target_ship_slot;
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    const bool gravity_shield =
        cls != nullptr && NovaShip_HasGravityShield(ship, *cls);
    if (std::abs(ship.vel_x) >= kVerySlowSpeed ||
        std::abs(ship.vel_y) >= kVerySlowSpeed) {
      if (!gravity_shield) {
        ship.ai_desired_heading_deg = static_cast<std::int16_t>(
            WrapDeg(BearingDeg(0.0F, 0.0F, ship.vel_x, ship.vel_y) + 180.0F));
        if (std::abs(heading_delta_deg()) < eff_turn_deg + 1.0F) {
          ship.ai_forward_thrust_cmd = eff_thrust;
        }
      } else {
        ship.ai_forward_thrust_cmd = -eff_thrust;
      }
    } else {
      ship.vel_x *= kMode1StopDamp;
      ship.vel_y *= kMode1StopDamp;
      ship.speed *= kMode1StopDamp;
      ship.ai_desired_heading_deg = static_cast<std::int16_t>(
          BearingDeg(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y));
      if (std::abs(heading_delta_deg()) < eff_turn_deg * 3.0F) {
        // Weapon_SelectDirectFireWeaponBankForPrimaryTarget +
        // Weapon_SelectGuidedWeaponBankForPrimaryTarget (deferred, Phase 5).
      }
      // Weapon_SelectWeaponBankForCurrentTarget (deferred, Phase 5).
    }
    // Ship_LaunchShipFromCarrierBay (deferred).
    break;
  }

  case 10:
    // Stationary cleanup reverse: hold the ship parked by reversing against
    // its heading. Raw float commands: desired -5.75 px/tick, thrust -3.67
    // (bits 0xC0B80000 / 0xC06AE148).
    if (ship.ai_desired_speed >= 0.0F) {
      ship.ai_desired_speed = kMode10ReverseSpeed;
    }
    ship.ai_forward_thrust_cmd = kMode10ReverseThrust;
    break;

  case 0x13:
    // Scripted maneuver: steer at the asteroid-pool slot-0 target position
    // (g_asteroid_states->target_pos in the original) and thrust within
    // turn+15 deg. No desired-speed write (cruise clamp from the entry reset).
    if (!fire_restricted) {
      const AsteroidState &target = state.asteroid_pool[0];
      ship.ai_desired_heading_deg = static_cast<std::int16_t>(BearingDeg(
          ship.pos_x, ship.pos_y, target.target_pos_x, target.target_pos_y));
      if (std::abs(heading_delta_deg()) < eff_turn_deg + 15.0F) {
        ship.ai_forward_thrust_cmd = eff_thrust;
      }
    }
    break;

  case 0x14:
    // Scripted velocity-match: ramp velocity toward the asteroid-pool slot-0
    // target velocity at 1.5x thrust per frame-time unit, then creep position
    // within the 150/80 px/axis windows (the original's banded weave). The
    // lead-velocity aim + unguided-weapon merge are deferred, so the straight
    // bearing at the target stands in for the merge gate.
    if (!fire_restricted) {
      const AsteroidState &target = state.asteroid_pool[0];
      const float step = eff_thrust * kEvasiveThrustFactor * frame_time_ms;
      auto ramp_axis = [&](float &vel, float target_vel) {
        if (vel < target_vel + step) {
          if (target_vel - step < vel) {
            vel = target_vel;
          } else {
            vel += step;
          }
        } else {
          vel -= step;
        }
      };
      ramp_axis(ship.vel_x, target.target_vel_x);
      ramp_axis(ship.vel_y, target.target_vel_y);
      if (target.target_pos_x + kScriptPosXSpan < ship.pos_x) {
        ship.pos_x -= step;
      } else if (ship.pos_x < target.target_pos_x - kScriptPosXSpan) {
        ship.pos_x += step;
      }
      if (target.target_pos_y + kScriptPosXSpan < ship.pos_y) {
        ship.pos_y -= step;
      } else if (ship.pos_y < target.target_pos_y - kScriptPosXSpan) {
        ship.pos_y += step;
      }
      if (ship.pos_x <= target.target_pos_x ||
          target.target_pos_x + kScriptPosYSpan <= ship.pos_x) {
        if (ship.pos_x < target.target_pos_x &&
            target.target_pos_x - kScriptPosYSpan < ship.pos_x) {
          ship.pos_x -= step;
        }
      } else {
        ship.pos_x += step;
      }
      if (ship.pos_y <= target.target_pos_y ||
          target.target_pos_y + kScriptPosYSpan <= ship.pos_y) {
        if (ship.pos_y < target.target_pos_y &&
            target.target_pos_y - kScriptPosYSpan < ship.pos_y) {
          ship.pos_y -= step;
        }
      } else {
        ship.pos_y += step;
      }
      ship.ai_desired_heading_deg = static_cast<std::int16_t>(BearingDeg(
          ship.pos_x, ship.pos_y, target.target_pos_x, target.target_pos_y));
      if (std::abs(heading_delta_deg()) < eff_turn_deg + kScriptAlignAddend) {
        ship.primary_target_ship_slot = -1;
        // Weapon_SelectUnguidedWeaponBank (deferred, Phase 5).
      }
    }
    break;

  default:
    break;
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
                         std::uint32_t now_ms) {
  // Rare "defunct / retired" global abort (DAT_00596d3d set): skip AI.
  // (Kept as a structural no-op; the latch is not modelled.)

  // Fire-restricted ships skip the heavy behavior selection this frame.
  const bool restricted = NovaAiShip_IsFireRestricted(state, ship);
  // Ship_CanShipInterceptCurrentPrimaryTarget reads the ship-local weapon
  // rows before the post-state auto-selector runs. Seed those rows once from
  // the assigned class so the intercept walk sees NPC guided banks too.
  EnsureNpcWeaponBanks(state, ship);
  // Ghidra calls the cloak-trait producer before the state supervisor, but
  // skips it during the coast-through-reversal interval (+0x4c > 0).
  if (ship.reverse_speed_bias <= 0.0F) {
    NovaAi_UpdateShipCloakStateFromTraits(state, ship);
  }

  // Auto-guard: a fire-restricted ship ignores the whole AI selection and just
  // holds its current state/controls; mirrors the original clearing the target
  // slots and control to 0 first.
  if (restricted) {
    ship.ai_target_ship_slot = -1;
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = -1;
    ship.ai_hostility_accumulator = 0;
    ship.ai_state_code = 0;
    ship.ai_control_mode = 0;
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
      // Skilled/fast ships can afford the heavy decision every frame; basic
      // ones are rate-limited by skill variance (provisional cadence).
      const std::int16_t variance = cls->skill_variance_percent;
      if (variance > 0 || ship.ship_instance_id % 3 == 0) {
        // heavy path allowed
      } else {
        run_heavy = false;
      }
    }
  }

  if (run_heavy && !restricted) {
    // Dispatch on behavior code.
    const std::int16_t behavior = ship.ai_behavior_code;
    if (behavior == 1) {
      NovaAi_UpdateBehavior0x01(state, ship, now_ms);
    } else if (behavior == 2) {
      NovaAi_UpdateBehavior0x02(state, ship, now_ms);
    } else if (behavior == 3) {
      // Ship_UpdateShipAiBehavior0x03 (0x00402e50) now has the hostile target
      // acquisition/travel fallback. The capture variant (0x004038b0) still
      // falls through this path because capture_power and the disabled-ship
      // scan are not represented in ScenarioData yet.
      NovaAi_UpdateBehavior0x03(state, ship, now_ms);
    } else if (behavior == 4) {
      // Ship_UpdateShipAiCombatState (0x00403de0): reuse the reconstructed
      // hostile path so an existing target is also promoted into state 4.
      NovaAi_UpdateBehavior0x03(state, ship, now_ms);
    } else if (behavior > 4) {
      // Ship_UpdateShipAssistResponseBehavior (0x004048a0): mission/escort
      // command decoding is not complete, but an existing AI target can still
      // enter the state machine's assist path.
      if (ship.ai_target_ship_slot != -1) {
        ship.ai_state_code = 10;
      }
      NovaAi_UpdateBehavior0x02(state, ship, now_ms);
    } else {
      // Ship_UpdateShipAiAvailabilityBehavior (0x00402980): availability
      // driven cargo/scripted branches remain deferred.
      NovaAi_UpdateBehavior0x01(state, ship, now_ms);
    }
  }

  // Always run the state machine + controls (the original does so after the
  // heavy block even when bVar6 skipped the heavy decision). frame_time_ms is
  // the reference cadence (the original reads the _g_avg_frame_time_ms EMA,
  // ~33.3 ms at 30 fps) for the control-mode position/velocity creeps.
  NovaAi_UpdateShipState(state, ship, now_ms);
  NovaAi_ApplyControls(state, ship, kReferenceFrameTimeMs, now_ms);
  // Ship_UpdateAutoWeaponSelectionFromTarget (0x00411540) is a post-state
  // refresh. It must run after ApplyControls because that bridge clears the
  // per-frame fire latch before the bank chooser arms it.
  NovaAi_UpdateAutoWeaponSelectionFromTarget(state, ship);
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

// Uniform integer in [0, bound) from the GameState PRNG (the dialog and AI
// entries share the session PRNG like the original's NovaRandom_Range).
[[nodiscard]] std::int16_t NovaAiRandomRange(GameState &state, int bound) {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::int16_t>(
      std::uniform_int_distribution<int>{0, bound - 1}(state.rng));
}

} // namespace

// Ghidra 0x00464a90 Ship_CanShipEngageTargetUnderCloakRules.
bool NovaAiShip_CanEngageTargetUnderCloakRules(const GameState &state,
                                               const Ship &subject_ship,
                                               const Ship &other_ship) {
  if (subject_ship.ai_state_code == 0x15) {
    return false; // jump/travel state: no engagement
  }
  if (other_ship.mission_ship_slot == 0x3ff) {
    return true;
  }
  // The first argument is the subject whose cloak visibility is tested.
  if (!NovaTargeting_ShipAtCloakVisibilityThreshold(subject_ship)) {
    return true;
  }
  // A ship tracking this subject and carrying a cloaking device can still be
  // engaged, even though the subject is hidden.
  if (other_ship.ai_target_ship_slot == subject_ship.ship_instance_id &&
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

// Ghidra 0x0040f780 Ship_ShouldShipKeepPressingTarget.
bool NovaAiShip_ShouldKeepPressingTarget(const GameState &state,
                                         const Ship &ship) {
  if (!ship.is_active || NovaAiShip_IsFireRestricted(state, ship)) {
    return false;
  }
  // The original admits an unset (-1) AI target here; it specifically rejects
  // the player slot 0 because that path is handled by the primary-target and
  // mutual-target checks below.
  if (ship.ai_target_ship_slot == 0 || ship.primary_target_ship_slot == -1) {
    return false;
  }
  if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, ship, state.player)) {
    return false;
  }
  if (ship.target_stellar_object_id != -1) {
    return true; // docked/landed against a stellar: keeps pressing
  }
  const std::int16_t target_slot = ship.primary_target_ship_slot;
  const bool coasting_reversal = ship.reverse_speed_bias > 0.0F;
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
  // target's ai_target_ship_slot to be the player).
  if (target != nullptr && target->ai_target_ship_slot == 0) {
    if (!coasting_reversal && engaged) {
      return true;
    }
    if (target_slot != -1 && !coasting_reversal && engaged) {
      return true;
    }
  }
  // A third ship pressing this ship: scan the other slots for one whose
  // ai_target_ship_slot is this ship's instance id, is not coasting through a
  // reversal, is active, unrestricted, and (like the direct checks) in a
  // non-disengage state while itself holding the player or a player-targeting
  // ship as primary.
  for (std::size_t j = 1; j < GameState::kMaxShips; ++j) {
    const Ship &other = state.ShipAt(j);
    if (other.ai_target_ship_slot != ship.ship_instance_id) {
      continue;
    }
    if (other.reverse_speed_bias > 0.0F || !other.is_active) {
      continue;
    }
    if (NovaAiShip_IsFireRestricted(state, other)) {
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
      if (engaged && other_target.ai_target_ship_slot == 0) {
        return true;
      }
    }
  }
  return false;
}

// Ghidra 0x0040fc00 Ship_IsShipEligibleForCommAidInteraction.
bool NovaAiShip_IsShipEligibleForCommAidInteraction(const GameState &state,
                                                    const Ship &ship) {
  if (!ship.is_active || NovaAiShip_IsFireRestricted(state, ship)) {
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
bool NovaAiShip_IsShipBrakingOnPlayerState9(const GameState &state,
                                            const Ship &ship) {
  return ship.is_active && !NovaAiShip_IsFireRestricted(state, ship) &&
         ship.primary_target_ship_slot == 0 && ship.ai_state_code == 9;
}

bool NovaAiShip_IsShipBrakingOnPlayerState0xF(const GameState &state,
                                              const Ship &ship) {
  return ship.is_active && !NovaAiShip_IsFireRestricted(state, ship) &&
         ship.primary_target_ship_slot == 0 && ship.ai_state_code == 0x0F;
}

// Ghidra 0x00411270 Ship_IsShipInNonIdleAiState.
bool NovaAiShip_IsShipInNonIdleAiState(const Ship &ship) {
  const std::int16_t s = ship.ai_state_code;
  return s != 0 && s != 2 && s != 1 && s != 0x14 && s != 7;
}

// Ghidra 0x00410060 Ship_AreAnyShipsEligibleForDistressCall.
bool NovaAi_AreAnyShipsEligibleForDistressCall(const GameState &state) {
  for (std::size_t j = 1; j < GameState::kMaxShips; ++j) {
    const Ship &other = state.ShipAt(j);
    if (NovaTargeting_IsShipEligibleForDistressCall(state, other)) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x004101d0 Ship_CanShipRespondToDistressCall.
bool NovaAiShip_CanShipRespondToDistressCall(const GameState &state,
                                             const Ship &responder,
                                             const Ship &distressed) {
  if (responder.ship_instance_id == distressed.ship_instance_id) {
    return false;
  }
  if (!responder.is_active || !distressed.is_active) {
    return false;
  }
  if (distressed.mission_ship_slot == 0x3ff) {
    return false;
  }
  const std::int16_t govt_a = responder.faction_or_government_id;
  const std::int16_t govt_b = distressed.faction_or_government_id;
  if (govt_a != -1 && govt_b != -1) {
    if (govt_a == govt_b) {
      return false;
    }
    if (NovaGovernment_AreGovtsHostileOrXenophobic(
            state.scenario, govt_a, govt_b)) {
      return true;
    }
    // A xenophobic `distressed` government admits any non-allied responder
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

// Ghidra 0x004100a0 Ship_HasShipDistressResponder.
bool NovaAiShip_HasShipDistressResponder(const GameState &state,
                                         const Ship &ship) {
  for (std::size_t j = 1; j < GameState::kMaxShips; ++j) {
    const Ship &other = state.ShipAt(j);
    if (!other.is_active ||
        static_cast<std::int16_t>(j) == ship.ship_instance_id) {
      continue;
    }
    if (NovaAiShip_ShouldKeepPressingTarget(state, other) &&
        NovaAiShip_CanShipRespondToDistressCall(state, ship, other)) {
      return true;
    }
  }
  return false;
}

// Ghidra 0x00410c30 Ship_EnterShipAiState0x09_TargetPlayerAndBrake.
void NovaAi_EnterState9TargetPlayerAndBrake(Ship &ship) {
  ship.ai_hostility_accumulator = 0;
  ship.ai_station_hold_timer = 0.0F;
  ship.primary_target_ship_slot = 0;
  ship.ai_state_code = 9;
  ship.ai_control_mode = 0;
  ship.reverse_speed_bias = -1.0F;
}

// Ghidra 0x00410c70 Ship_EnterShipAiState0x0F_TargetPlayerAndBrake.
void NovaAi_EnterState0FTargetPlayerAndBrake(Ship &ship) {
  ship.ai_hostility_accumulator = 0;
  ship.ai_station_hold_timer = 0.0F;
  ship.primary_target_ship_slot = 0;
  ship.ai_state_code = 0x0F;
  ship.ai_control_mode = 0;
  ship.reverse_speed_bias = -1.0F;
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
  if (NovaAiShip_IsFireRestricted(state, candidate) ||
      NovaAiShip_IsDestroyed(candidate)) {
    return false;
  }
  if (candidate.current_system_id != system_id) {
    return false;
  }
  if (require_distress) {
    return NovaTargeting_IsShipEligibleForDistressCall(state, candidate);
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
      pick = static_cast<std::int16_t>(NovaAiRandomRange(state, 0x3f) + 1);
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
      pick = static_cast<std::int16_t>(NovaAiRandomRange(state, 0x3f) + 1);
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

// Ghidra 0x00410700 Ship_SetShipHostileToPlayer. The mission-announcement arm
// is deferred (TODO(decomp): g_mission_ship_defs + Mission_ShowMissionShip-
// Announcement are not modelled), so this is the escort-mode/state flip only.
void NovaAi_SetShipHostileToPlayer(GameState &state, Ship &ship) {
  (void)state;
  if (ship.ai_control_mode == 4 || ship.ai_control_mode == 0x0D) {
    ship.ai_control_mode = 0;
  }
  ship.ai_state_code = 4;
  ship.ai_secondary_target_slot = -1;
  ship.primary_target_ship_slot = 0;
}

} // namespace game
