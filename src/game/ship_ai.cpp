// Clean-room reconstruction of the NPC ship AI decision layer. See ship_ai.hpp
// for the module overview and the Ghidra-address mapping of each helper.
//
// Porting notes:
//  * Ships carry no outfit inventory, so "has gravity shield" uses the NPC
//    branch of Outfit_ShipHasGravityShieldOutfit (0x0046df70) via
//    NovaShip_HasGravityShield, and "effective stats" are the class base
//    values (the player's outfit/ionization damping is not modelled yet).
//  * The per-mode turn/thrust scale constants (the _DAT_005750xx globals) are
//    decoded from the raw bytes and typed + pre-commented in the Ghidra DB.
//    The movement and combat modes read the real values; still-unused
//    freeflight-anchor constants remain commented-only.
//  * HUD/mission flavor side-effects (overlay messages, extortion prompts,
//    fuel-transfer chatter, carrier-bay launch) are documented no-ops until
//    those systems land; combat-heavy branches that depend on the not-yet-
//    reconstructed disable systems are conservatively gated. NPC weapon-bank
//    selection is ported (the NovaAi_Select* helpers); point-defense
//    auto-select, inbound-threat gating, and formation-offset mirroring remain
//    TODO(decomp).
//  * "Wander": Behavior 0x01 + state 0/1 makes a ship pick a random adjacent
//    travel stellar and steer toward it -- the visible "make them move around
//    the system" baseline.

#include "ship_ai.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "boarding_plunder.hpp"
#include "escort_formation.hpp"
#include "government.hpp"
#include "log.hpp"
#include "mission.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "ship_spawn.hpp"
#include "spaceflight.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "weapon.hpp"

namespace game {

namespace {

// --- Decoded movement constants (Ghidra _DAT_00575xxx). The original reads
// these from byte-mislabeled globals at 0x00575000..0x00575200; the values
// below were decoded from the raw bytes (float vs double from the actual
// instruction widths) on 2025-08-09 and are typed + pre-commented in the
// Ghidra DB. See docs/npc_ship_behaviour.md "Decoded constants" section. ---
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
// Mode-3 (depart from centre): alignment addend 3.0 (0x575120, float).
constexpr float kMode3AlignAddend = 3.0F;
// Engagement distance within which a ship can fire on or engage a target
// (0x5750b0, float).
constexpr float kEngageDist = 165.0F;
// Assist/response keep-distance thresholds.
constexpr float kAssistClose = 300.0F;
constexpr float kAssistFar = 600.0F;
// Squared-distance threshold for the centre envelope used by departure and
// attack staging (states 2/3); 0x575098 (double) = 1,000,000 px^2 (1000 px
// radius).
constexpr float kCentreRangeSq = 1000000.0F;
// Stellar_GetJumpSequenceDuration60Hz (0x0046EFB0) returns 350 for the
// engine-enabled path used by the NPC spin-up. ShipClassDef's duration
// multiplier is not decoded into ShipClass yet, so retain the original base
// duration until that field is represented here. NOTE: the original compares
// this against 60 Hz tick elapsed time, i.e. a ~5.8 s NPC spin-up; the port's
// NPC path measures wall-clock ms, so the port's spin-up is 350 ms --
// TODO(decomp) unify on the 60 Hz tick unit.
constexpr float kNpcJumpSpinupDurationMs = 350.0F;
// Gravity-shield approach multipliers (state 0xd/0xf).
constexpr float kShieldKeepMult = 4.0F;
// Combat turn-radius constants from DAT_005750a0/a4/a8/ac. The class turn
// value is converted to the runtime degrees-per-tick value before these
// branches use it.
constexpr float kTurnRadiusBase = 10.0F;
constexpr float kTurnRadiusScale50 = 50.0F;
constexpr float kAssistTurnRadiusScale =
    30.0F; // FLOAT_005750c0 (state 0xc outer band)
// FLOAT_005750d0 = 60.0: state-0xc inner band (mode 9 beyond it, mode 0xb
// between outer and inner). 15.0 (FLOAT_005750d4) belongs to state 9's range.
constexpr float kAssistInnerTurnRadiusScale = 60.0F;
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
// Control-mode 10 arrival slowdown: start at 50 px/tick along the heading and
// reduce that override by 1.165 each tick (raw bits 0xC2480000 / 0xBF951EB8).
constexpr float kMode10ReverseSpeed = -50.0F;
constexpr float kMode10ReverseThrust = -1.165F;
// Escort-follow half-span stand-in: Sprite_GetShipClassEscortFrameHeight
// (0x004624c0) returns the class escort sprite's shot half-span or its 0x4B =
// 75 px debug fallback; the clean-room sprite tables are not modelled, so the
// debug fallback stands in (TODO(decomp)).
constexpr float kEscortHalfSpanPx = 75.0F;

constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
constexpr float kFullCircleDeg = 360.0F;

// Frame_MeasureFrameTiming multiplies elapsed milliseconds by 0.03 before
// publishing _g_avg_frame_time_ms, so the AI consumes normalized simulation
// ticks (about 1.0 at 30 Hz), not literal milliseconds.

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
  // ShipState +0xC8E8 (weapon-sprite flash) is not represented in the
  // clean-room Ship yet; allocation already supplies its zero default.
}

// Ghidra 0x004159e0 Ship_EnterShipAiState0x15_JumpOutToSystem.
void NovaAi_EnterState15JumpOutToSystem(GameState &state,
                                        Ship &ship,
                                        std::int16_t stellar_id) {
  ship.ai_state_code = 0x15;
  ship.ai_control_mode = 0;
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = stellar_id;
  ship.ai_maneuver_timer_ms = 60.0F;
  ship.ai_station_hold_timer = -1.0F;

  if (stellar_id >= 0 && stellar_id < 0x800) {
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    const bool can_jump =
        cls != nullptr && static_cast<float>(cls->base_fuel) >= kJumpFuelCost;
    ship.jump_destination_stellar_id = can_jump ? stellar_id : -2;

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
  ship.target_stellar_object_id = -1;
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
  NovaAi_EnterState15JumpOutToSystem(state, ship, entry_stellar);
  return true;
}

// Ghidra 0x004687b0 Ship_IsShipDisabled. True when the ship must not
// fire/act this frame: derelict government (flags_primary 0x800), mission
// spawn_behavior-5 special ships not yet attacking (TODO(decomp):
// special_ship_attacking runtime flag unmodelled), or critically damaged
// (armor below 1/3 of max, 1/10 with capability flags 0x10). Non-player ships
// with a stellar target are exempt -- the original returns not-disabled
// without reaching the armor check (0x00468856 XOR AL,AL early-out).
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
  // Ships attached to a stellar (landing/jump-out approach) are never
  // disabled, even when crippled; the armor gate below is skipped.
  if (ship.ship_instance_id > 0 && ship.target_stellar_object_id != -1) {
    return false;
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
  // Straight bearing fallback (Math_BearingFromPointToPoint(origin, target)).
  std::int16_t bearing = static_cast<std::int16_t>(
      BearingDeg(origin_x, origin_y, target.pos_x, target.pos_y));
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
  const float dx = target.pos_x - origin_x;
  const float dy = target.pos_y - origin_y;
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
      BearingDeg(origin_x, origin_y, intercept_x, intercept_y));
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
        candidate.ship_instance_id == candidate.squad_leader_ship_slot ||
        candidate.ship_instance_id == ship.squad_leader_ship_slot ||
        candidate.squad_leader_ship_slot == ship.squad_leader_ship_slot ||
        !candidate.is_active || candidate.ai_state_code == 0x15 ||
        NovaAiShip_IsDestroyed(candidate) ||
        !NovaAiShip_CanEngageTargetUnderCloakRules(state, ship, candidate)) {
      continue;
    }
    if (NovaAiShip_IsDisabled(state, candidate) &&
        candidate.escort_command_code != 2) {
      continue;
    }

    bool target_context_ok = false;
    if (candidate.squad_leader_ship_slot == 0) {
      target_context_ok = NovaAiShip_ShouldKeepPressingTarget(state, candidate);
    } else if (ship.squad_leader_ship_slot >= 0 &&
               state.SlotInRange(
                   static_cast<std::size_t>(ship.squad_leader_ship_slot))) {
      target_context_ok = NovaTargeting_IsShipAcquirableAsTarget(
          state,
          state.ShipAt(static_cast<std::size_t>(ship.squad_leader_ship_slot)),
          candidate);
    }
    if (!target_context_ok) {
      continue;
    }

    const float target_distance_sq = RoundedDistanceSquared(
        state.ShipAt(static_cast<std::size_t>(ship.squad_leader_ship_slot))
            .pos_x,
        state.ShipAt(static_cast<std::size_t>(ship.squad_leader_ship_slot))
            .pos_y,
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
  // disabled predicate.  A lethal hit leaves the ship slot
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
  // 0x00411540 gates the whole refresh on behavior >= 5, so its disabled-
  // target clear never applied to behavior-3 capture drives boarding a
  // disabled victim. The port runs the refresh for behaviors 3/4 (the
  // original arms their banks elsewhere, TODO(decomp)), so the clear is
  // restricted to behaviors >= 5 to keep the state-0xd boarding target alive.
  if (!target.is_active ||
      (ship.ai_behavior_code >= 5 && NovaAiShip_IsDisabled(state, target))) {
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
// (always false at the known call sites) selects only the plain pool.
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
  int count_2000 = 0;     // original local_98

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
      mask20 = (govt->scan_mask_short & 0x20U) != 0U;
      prefer_1000 = (govt->scan_mask_short & 0x40U) != 0U;
      prefer_2000 = (govt->scan_mask_short & 0x80U) != 0U;
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
             (!avail_2000[s] || !prefer_2000) && (!avail_1000[s] || !mask20);
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
  // Mirrors Ship_ComputeShipMaxShieldPoints (0x00463550): the player uses the
  // cached outfit-derived value, NPCs the class base scaled by the personality
  // shield/armor scale (when positive) and the behavior-5 difficulty factor.
  const auto max_shield_for = [&state](const Ship &subject) -> float {
    if (subject.ship_instance_id == 0) {
      return Outfit_ComputePlayerEffectiveStats(state).max_shield_points;
    }
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(subject.ship_class_id + 0x80));
    float value = cls != nullptr ? static_cast<float>(cls->base_shield) : 0.0F;
    if (subject.pers_def_slot != -1 &&
        static_cast<std::size_t>(subject.pers_def_slot) <
            state.scenario.pers_defs.size()) {
      const float scale =
          state.scenario
              .pers_defs[static_cast<std::size_t>(subject.pers_def_slot)]
              .shield_armor_scale;
      if (scale > 0.0F) {
        value *= scale;
      }
    }
    if (subject.ai_behavior_code == 5) {
      value *= 1.333F; // k_behavior5_difficulty_mult_f64 (0x00575760)
    }
    return value;
  };
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
  const float ship_max_shield = max_shield_for(ship);
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
    const float candidate_max_shield = max_shield_for(candidate);
    if (candidate_max_shield > 0.0F) {
      candidate_ratio = candidate.shield_points / candidate_max_shield;
    }
    candidate_ratio = clamp_ratio(candidate_ratio);
    if (support > 0 &&
        NovaAiShip_HasIncomingDistressSupport(state, candidate, ship)) {
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

// Ghidra 0x0040e020 Ship_AcquirePrimaryTargetForShip. PARTIAL reconstruction:
// the early retention gate, the active mission-fleet goal 0/1 arms, the
// weapon-readiness early return, the IFF-scrambler/policy player shield, and
// the behavior-6 escort re-selection. NOT implemented: the license/anti-tamper
// check, the pers_def personality arms, and every government target pass
// (ally-support 0x0040e3c0, flags_primary&1 aggressive 0x0040e710, near-player
// reputation/odds 0x0040ece3, inherent-combat roll 0x0040e57f, distress-
// responder rescans/common tail 0x0040eea0) with their perceived-combat-
// strength filtering, so the middle of the routine keeps the previous
// clean-room nearest-hostile slice. The iff_scrambler_active term below is
// inert until its writer is ported (see scenario_data.hpp).
void NovaAi_AcquirePrimaryTarget(GameState &state, Ship &ship) {
  // TODO(decomp(0x0040e020)) skipped: the leading five-pair license-seed
  // integrity check on g_ship_states[5].license_seed. The original calls
  // Ship_SetShipHostileToPlayer when any stored pair-comparison boolean is
  // FALSE (a matching pair), not on an arbitrary mismatch; the purpose is
  // provisional. The clean-room model has no license-seed field.
  //
  // TODO(decomp(0x0040e020)) skipped: the pers_def_slot arms -- slot 0x3fe
  // forces hostility, and (pers.flags_primary & 1) with the pers +0x621 grudge
  // latch and the cloak rules forces hostility. PersDef has no grudge-latch
  // field and its writer (Shot_ResolveShipHitFromWeapon 0x0041a2cb) is
  // unported.

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
    if (mission.fleet_spawn_goal == 0 &&
        NovaAiShip_CanEngageTargetUnderCloakRules(state, state.player, ship)) {
      NovaAi_SetShipHostileToPlayer(state, ship);
      return;
    }
    if (mission.fleet_spawn_goal == 1) {
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

  // TODO(decomp(0x0040e020)) skipped: the original's government
  // target passes (0x0040e3c0/0x0040e710 ally-support scan, flags_primary&1
  // aggressive scan, the 0x0040ece3 near-player reputation/odds gate, the
  // 0x0040e57f inherent-combat-government roll, and the 0x0040eea0 distress-
  // responder rescans) with their perceived-combat-strength filters are not
  // yet reconstructed. NovaAiShip_ComputePerceivedCombatStrength is ported but
  // unintegrated (no gameplay caller), so it filters nothing here. The region
  // below keeps the previous clean-room nearest-hostile selection, now
  // respecting the IFF-scrambler/policy player shield, followed by the
  // faithful behavior-6 escort re-selection.
  const Government *ship_govt =
      ship.faction_or_government_id >= 0
          ? state.scenario.GovernmentByIndex(ship.faction_or_government_id)
          : nullptr;
  const bool player_shielded =
      ship_govt != nullptr &&
      (ship_govt->iff_scrambler_active ||
       NovaGovernment_GetPolicyFlag(
           state.scenario, ship.faction_or_government_id, 0));

  std::int16_t best_slot = -1;
  bool best_is_engaged = false;
  float best_distance_sq = 0.0F;
  for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
    if (slot == static_cast<std::size_t>(ship.ship_instance_id) ||
        (slot == 0 && player_shielded)) {
      continue;
    }
    const Ship &candidate = state.ShipAt(slot);
    if (!candidate.is_active || NovaAiShip_IsDestroyed(candidate) ||
        candidate.current_system_id != ship.current_system_id ||
        candidate.ai_state_code == 0x15 ||
        candidate.target_stellar_object_id != -1 ||
        NovaAiShip_IsDisabled(state, candidate) ||
        NovaTargeting_ShipAtCloakVisibilityThreshold(candidate)) {
      continue;
    }

    const bool player_contact = slot == 0;
    // "Engaged with the player" means *attacking* the player
    // (primary_target_ship_slot == 0). The squad_leader_ship_slot == 0 marker
    // instead identifies ships ATTACHED to the player -- the player's hired
    // escorts -- so counting it here made every hostile behavior-0x03 NPC
    // acquire the nearest escort on sight (Ghidra 0x0040e020 never treats an
    // escort attach marker as hostility: escorts carry government -1 and
    // only become targets through the hit/hostility-propagation paths).
    const bool candidate_is_engaged = candidate.primary_target_ship_slot == 0;
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
void NovaAi_ReacquireTravelOrSettle(GameState &state,
                                    Ship &ship,
                                    std::uint32_t now_ms) {
  const System *sys = CurrentSystem(state);
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
      const std::int16_t travel = NovaAi_SelectRandomAdjacentTravelStellar(
          state, ship, /*strict_mode=*/false, /*unrestricted_only=*/false);
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
    if (ship.squad_leader_ship_slot == 0) {
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

// Ghidra 0x00402980 Ship_UpdateShipAiAvailabilityBehavior. Supervisor for
// ships whose class Flags3 has bit 0x1 (Bible "ship destroys asteroids") or
// 0x2 ("ship scoops asteroid debris"). The dispatcher runs it in preference
// to the normal ai_behavior_code branch when availability_flags & 3 and the
// ship has no squad leader. Arms the scripted asteroid manoeuvre (state 0x10),
// the freeflight-anchor cargo pick-up (state 0x11), or the nearest-adjacent-
// travel-stellar wander ladder; a hostile contact escalates to state 3.
void NovaAi_UpdateAvailabilityBehavior(GameState &state,
                                       Ship &ship,
                                       std::uint32_t now_ms) {
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
      cls != nullptr && (cls->availability_flags & 1U) != 0U;
  const bool scoops_debris =
      cls != nullptr && (cls->availability_flags & 2U) != 0U;

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
          NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
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
            NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
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

  // Ship_ShowPlayerInterceptTauntIfEligible (called when ai_state_code == 3)
  // is HUD/mission chatter, the same no-op as in NovaAi_UpdateBehavior0x01.
  // TODO(decomp): port the taunt.
}

// Ghidra 0x00402bd0 Ship_UpdateShipAiBehavior0x02_BraveTrader. Local/dude
// behavior shares the travel fallback with behavior 0x01, but promotes an
// established hostile contact once it is within the original 0x4e3-pixel
// per-axis gate. The player-target arm enters state 10 (assist/response); other
// contacts enter state 3. Government chatter and assistance encounter side
// effects remain
// TODO(decomp).
void NovaAi_UpdateBehavior0x02(GameState &state,
                               Ship &ship,
                               std::uint32_t now_ms) {
  if (NovaAiShip_IsDisabled(state, ship)) {
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
    } else if (ship.squad_leader_ship_slot == 0) {
      ship.ai_state_code = 10;
      ship.ai_secondary_target_slot = 0;
    } else {
      ship.ai_state_code = 3;
    }
  }
  if (ship.ai_state_code == 3) {
    (void)NovaGovernment_TryTriggerAssistanceEncounter(
        state, ship, /*force=*/false);
  }
}

// Ghidra 0x00402e50 Ship_UpdateShipAiBehavior0x03_Warship. Hostile behavior
// acquires a nearby contact when idle, preserves an active primary target, and
// falls back to the normal travel/jump ladder when combat has no target. The
// government flee, weapon-readiness, mission-fleet, and capture-variant arms
// depend on data not represented by the current clean-room Ship model.
void NovaAi_UpdateBehavior0x03(GameState &state,
                               Ship &ship,
                               std::uint32_t now_ms) {
  if (NovaAiShip_IsDisabled(state, ship)) {
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

// Ghidra 0x004038b0 Ship_UpdateShipAiBehavior0x03_WarshipCapture. The
// plunder-flavored variant of hostile behavior 0x03, selected by the
// dispatcher when the ship's faction has government flags_primary 0x1000
// (Bible: "warships will plunder non-mission, trader-type enemies"). Instead
// of only fighting, the ship scans for disabled boardable victims
// (Ship_SelectNearestDisabledShipForBoarding), marks already-boarded targets
// for the attack/yield decision, performs the actual boarding handoff
// (Outfit_BoardShipAndTransferCargo) from AI control mode 0xf, and abandons
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
              NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
            } else {
              ship.ai_state_code = 6;
            }
          } else {
            ship.travel_transfer_mode = 2;
            ship.ai_state_code = 1;
          }
        } else if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
          NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
        } else {
          ship.ai_state_code = 6;
        }
      } else if (NovaTravel_CanShipInitiateJumpSequence(state, ship)) {
        NovaAi_EnterState2ClearPrimaryTarget(ship, now_ms);
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
    ship.target_stellar_object_id = -1;
    NovaBoarding_BoardShipAndTransferCargo(state, ship, victim, now_ms);
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
    if ((target_class != nullptr && target_class->default_ai_behavior > 2) &&
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

// Ghidra 0x00405590 Ship_UpdateShipAiState. The per-frame state machine. This
// is a substantial function; the reconstruction below covers the core
// movement/control-mode decision for the states the reimplementation drives
// (travel, wander, escort-follow, hold, drift, disengage, defunct) and writes
// Ship.ai_control_mode accordingly. The high-level attack states now enter
// pursuit/engagement control modes using the reconstructed target slots;
// weapon selection, cloak engagement, and HUD/mission flavor remain deferred.
void NovaAi_UpdateShipState(GameState &state,
                            Ship &ship,
                            std::uint32_t now_ms,
                            float elapsed_ticks) {
  (void)now_ms;
  auto *scn = &state.scenario;

  // ---- Defunct (0x16) unwind. ----
  if (ship.ai_state_code == 0x16) {
    if (ship.ai_maneuver_timer_ms <= 0.0F) {
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

  // AI maneuver timer (>0) only suspends the machine
  // for states other than 10 and 0xe (those keep running through the coast).
  if (ship.ai_maneuver_timer_ms > 0.0F && ship.ai_state_code != 10 &&
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
    if (ship.squad_leader_ship_slot == 0) {
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
      ship.ai_secondary_target_slot = ship.squad_leader_ship_slot;
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
          ship.target_engagement_patience_timer -= elapsed_ticks;
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
          ship.ai_maneuver_timer_ms = static_cast<float>(dist(state.rng));
        }
      }
    }
  }

  // ---- Committed inter-system transfer (state 0x14). ----
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
            : 0x96; // System_GetCurrentSystemLinkSpriteHeight fallback
                    // (provisional)
    const std::int16_t span_q = static_cast<std::int16_t>(half_span / 4);
    const bool far = std::abs(dx) > span_q || std::abs(dy) > span_q;
    if (far) {
      // Unlike local state 2, this restricted-lane entry does not enter the
      // ordinary mode-4 hyperjump sequence. It approaches the gate/wormhole
      // with mode 2, then hands off through mode 0x17.
      ship.ai_control_mode = 2;
      ship.ai_maneuver_timer_ms = -1.0F;
    } else {
      // At the gate/wormhole entry point, the original clears velocity as part
      // of the handoff, then propagates entry to any active escorts. This is
      // not a visible mode-4 brake/spin-up phase: the NPC transfers/vanishes.
      ship.vel_x = 0.0F;
      ship.vel_y = 0.0F;
      ship.ai_control_mode = 0x17;
      if (!(ship.ai_maneuver_timer_ms >= 0.0F) ||
          ship.ai_maneuver_timer_ms > 16.0F) {
        ship.ai_maneuver_timer_ms = 16.0F;
      }
      // Escort propagation (Ship_UpdateShipAiState state 0x14 successor loop):
      // any active ship whose squad_leader_ship_slot == this ship is ordered to
      // jump with it. TODO(decomp): confirm formation-offset propagation.
      for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
        Ship &other = state.ShipAt(slot);
        if (!other.is_active ||
            other.ship_instance_id == ship.ship_instance_id) {
          continue;
        }
        if (other.squad_leader_ship_slot != ship.ship_instance_id) {
          continue;
        }
        other.squad_leader_ship_slot = -1;
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
      // Gameplay-visible NPC gate/wormhole transfer. Mode 0x17 is only the
      // handoff marker in the AI/control switch; the original continues
      // through a surrounding presentation/transfer path. This reconstruction
      // completes that path here once state 0x14 reaches the entry point.
      (void)NovaAi_CompleteNpcJump(state, ship);
    }
    return;
  }

  // ---- Gate emergence (0x15) -> arrival slowdown (8). ----
  if (ship.ai_state_code == 0x15) {
    ship.ai_control_mode = 0;
    ship.primary_target_ship_slot = -1;
    if (ship.ai_maneuver_timer_ms <= 0.0F) {
      ship.ai_maneuver_timer_ms = -1.0F;
      ship.ai_state_code = 8;
      ship.ai_secondary_target_slot = -1;
    } else {
      return;
    }
    // Do not return after the transition. The original continues through the
    // same state-machine invocation to the state-8 arm below, which writes
    // the -999 arrival sentinel and selects control mode 0x0a. Delaying that
    // work by a frame lets the next behavior-supervisor pass steal state 8.
  }

  // ---- Jump-departure staging (state 2). ----
  if (ship.ai_state_code == 2) {
    // State 2 does not seek the system centre. While moving, it brakes; once
    // stopped, mode 4 aligns from the centre through the ship and ramps the
    // departure outward. Inside the centre envelope mode 3 supplies the
    // outward thrust arm. The "stopped" test uses the same 0.35 px/tick
    // threshold as the original (_DAT_00575080); special-loadout classes can
    // bypass that brake as described below.
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    // Ship_CheckSpecialLoadoutCapability (0x0046d080) bypasses the brake for
    // classes flagged 0x20 in Flags2. Its outfit-based capability (load id
    // 0x25) is not represented in the NPC loadout model yet.
    const bool special_departure =
        cls != nullptr && (cls->flags_secondary & 0x0020U) != 0U;
    if (SquaredDistance(0.0F, 0.0F, ship.pos_x, ship.pos_y) <= kCentreRangeSq) {
      ship.ai_control_mode = 3;
    } else if (special_departure || (std::abs(ship.vel_x) < kVerySlowSpeed &&
                                     std::abs(ship.vel_y) < kVerySlowSpeed)) {
      ship.ai_control_mode = 4;
    } else {
      ship.ai_control_mode = 1;
    }
    if (!NovaAiShip_IsDisabled(state, ship)) {
      (void)NovaGovernment_TryTriggerAssistanceEncounter(
          state, ship, /*force=*/false);
    }
    return;
  }

  // ---- Hold-station / follow target (state 0xb). ----
  if (ship.ai_state_code == 0xb) {
    const std::int16_t target = ship.squad_leader_ship_slot;
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
    if (ship.ai_state_code == 3) {
      (void)NovaGovernment_TryTriggerAssistanceEncounter(
          state, ship, /*force=*/false);
    }
    return;
  }

  // ---- Drift / evade (state 0xe). ----
  if (ship.ai_state_code == 0xe) {
    ship.primary_target_ship_slot = -1;
    ship.ai_secondary_target_slot = -1;
    ship.ai_control_mode = 0;
    // The original advances position directly by g_avg_frame_time_ms * 0.7;
    // this state does not add that step to persistent velocity.
    ship.pos_x += std::sin(ship.heading) * (elapsed_ticks * 0.7F);
    ship.pos_y -= std::cos(ship.heading) * (elapsed_ticks * 0.7F);
    if (ship.ai_maneuver_timer_ms <= 0.0F) {
      ship.ai_state_code = 0;
    }
    return;
  }

  // ---- Pursuit (state 5) toward squad_leader_ship_slot. ----
  if (ship.ai_state_code == 5 && ship.squad_leader_ship_slot != -1 &&
      ship.target_stellar_object_id == -1) {
    ship.ai_secondary_target_slot = ship.squad_leader_ship_slot;
    const auto *cls =
        scn->Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    const float base_turn = cls ? cls->turn_rate * 0.1F : 0.0F;
    const std::int16_t range = static_cast<std::int16_t>(std::max(
        100,
        static_cast<int>((kTurnRadiusBase - base_turn) * kTurnRadiusScale50)));
    const Ship &tgt =
        state.ShipAt(static_cast<std::size_t>(ship.squad_leader_ship_slot));
    if (std::abs(ship.pos_x - tgt.pos_x) > range ||
        std::abs(ship.pos_y - tgt.pos_y) > range) {
      ship.ai_control_mode = 0xb;
    } else {
      ship.ai_control_mode = 8;
    }
    // A leader that cannot currently be engaged under the cloak rules always
    // downgrades the follow command to braking, after the distance selection.
    if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, tgt, ship)) {
      ship.ai_control_mode = 1;
    }
    return;
  }

  // ---- Assistance/response (state 10) toward squad_leader_ship_slot. ----
  if (ship.ai_state_code == 10 && ship.squad_leader_ship_slot != -1 &&
      ship.target_stellar_object_id == -1) {
    ship.ai_secondary_target_slot = ship.squad_leader_ship_slot;
    const Ship &tgt =
        state.ShipAt(static_cast<std::size_t>(ship.squad_leader_ship_slot));
    if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, tgt, ship)) {
      const bool far_from_target =
          std::abs(ship.pos_x - tgt.pos_x) > kAssistClose ||
          std::abs(ship.pos_y - tgt.pos_y) > kAssistClose;
      ship.ai_control_mode = far_from_target ? 9 : 1;
      return;
    }
    const bool far = std::abs(ship.pos_x - tgt.pos_x) > kAssistFar ||
                     std::abs(ship.pos_y - tgt.pos_y) > kAssistFar;
    const bool outside_close =
        std::abs(ship.pos_x - tgt.pos_x) > kAssistClose ||
        std::abs(ship.pos_y - tgt.pos_y) > kAssistClose;
    if (far) {
      ship.ai_control_mode = 9; // pursue at long range
    } else if (outside_close) {
      ship.ai_control_mode = 0xb; // formation approach
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
    if (ship.primary_target_ship_slot == -1 ||
        ship.ai_maneuver_timer_ms > 0.0F) {
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

  // ---- Arrival slowdown state (state 8). ----
  // The movement integrator exits it through
  // Ship_ResetShipPrimaryAndSecondaryTargets after control mode 0x0a's
  // reverse-speed threshold. If it survives that path, the outer
  // Ship_DeactivateVacantShipsAndTally sweep at system entry/landing (or the
  // escape-pod transition) removes the vacant NPC slot.
  if (ship.ai_state_code == 8) {
    ship.ai_station_hold_timer = -999.0F;
    ship.ai_control_mode = 10;
    return;
  }

  // ---- Assist approach to a disabled ship (state 0xf). Entered from the
  // comm-window Request Assistance / Beg For Mercy arm
  // (Ship_EnterShipAiState0x0F 0x00410c70, player as primary target): keep
  // within (10 - turn) * 30 + 50 px (x4 gravity shield) of the cripple,
  // velocity-matching (mode 0xf) when close; the repair itself happens in the
  // mode-0xf within-3 px arm of NovaAi_ApplyControls. Leaves the state when
  // the target is gone or no longer disabled.
  if (ship.ai_state_code == 0xf && ship.primary_target_ship_slot != -1) {
    const auto target_slot =
        static_cast<std::size_t>(ship.primary_target_ship_slot);
    if (!state.SlotInRange(target_slot) ||
        !state.ShipAt(target_slot).is_active ||
        !NovaAiShip_IsDisabled(state, state.ShipAt(target_slot))) {
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
        (kTurnRadiusBase - base_turn) * kAssistTurnRadiusScale + 50.0F);
    const Ship &tgt = state.ShipAt(target_slot);
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
    // Weapon banks are selected by the control modes; this state just holds
    // its engagement distance and lets the combat systems advance it later.
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
            ship.squad_leader_ship_slot != 0 &&
            target.faction_or_government_id >= 0 &&
            NovaGovernment_AreGovtsAllied(state.scenario,
                                          ship.faction_or_government_id,
                                          target.faction_or_government_id) &&
            target.squad_leader_ship_slot == 0) {
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
      if (ship.ai_maneuver_timer_ms > 0.0F) {
        ship.ai_state_code = 0;
        ship.ai_control_mode = 0;
        return;
      }
      if (ship.squad_leader_ship_slot != -1 &&
          ship.squad_leader_ship_slot == ship.primary_target_ship_slot) {
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
    if (target_slot < 0 || ship.ai_maneuver_timer_ms > 0.0F ||
        !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      return;
    }
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    if (ship.squad_leader_ship_slot != -1 &&
        (target_slot == ship.squad_leader_ship_slot ||
         target.squad_leader_ship_slot == ship.squad_leader_ship_slot)) {
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
    if (!NovaAiShip_IsDisabled(state, target)) {
      if (dx > kCombatCloseRange || dy > kCombatCloseRange) {
        if (NovaAiShip_CanInterceptCurrentPrimaryTarget(state, ship)) {
          ship.ai_control_mode = ship.ai_behavior_code < 3 ? 5 : 0xe;
        } else if (ship.ai_control_mode != 0x11) {
          ship.ai_control_mode = 7;
        }
      } else if (ship.ai_control_mode != 0x10 && ship.ai_control_mode != 0x11) {
        ship.ai_control_mode = 6;
      }
    } else if (target.boarded_target_latch == 0 ||
               ship.ai_maneuver_timer_ms > 0.0F) {
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
    // LAB_00406bc2 tail: while a state-2 target stays valid and the ship is
    // not disabled, every frame runs the carrier-bay launch driver (and in
    // the original Government_TryTriggerGovtAssistanceEncounter 0x00413610,
    // not yet reimplemented -- TODO(decomp)).
    if (!NovaAiShip_IsDisabled(state, ship)) {
      NovaShip_LaunchShipFromCarrierBay(state, ship);
    }
    return;
  }

  if (ship.ai_state_code == 0) {
    ship.ai_control_mode = 0;
    return;
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
    const ShipClass *rating_baseline = state.scenario.Ship(0x80);
    if (player_class != nullptr && rating_baseline != nullptr) {
      const std::int32_t divisor =
          static_cast<std::int32_t>(rating_baseline->strength) * 0x1900;
      float rating_scale =
          divisor != 0
              ? static_cast<float>(state.player_combat_rating_points / divisor)
              : 1.0F;
      rating_scale = std::clamp(rating_scale, 1.0F, 2.0F);
      hostile_strength = static_cast<std::int16_t>(std::lround(
          static_cast<float>(player_class->strength) * rating_scale));
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
                          float elapsed_ticks,
                          std::uint32_t now_ms) {
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

  // Ship_ApplyShipAiControls entry (0x00408150) also syncs
  // ai_desired_heading_deg to the rounded current heading every frame, before
  // the mode switch. Steering modes overwrite it immediately after; unsteered
  // modes (0 idle drift, 10 arrival slowdown) inherit "desired == current" and
  // therefore hold heading exactly -- this is what keeps the state-8 jump-in
  // and gate-emergence glides straight instead of wheeling toward a stale
  // steering target. ShipState stores the field in degrees while the clean-room
  // heading is radians, so convert at this boundary.
  ship.ai_desired_heading_deg =
      static_cast<std::int16_t>(WrapDeg(std::round(ship.heading / kDegToRad)));

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

  // Ship_IsShipDisabled gate: the original skips every combat/behavior
  // mode for disabled ships (they fall through with thrust 0).
  const bool fire_restricted = NovaAiShip_IsDisabled(state, ship);

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
    // Idle: the original has no mode-0 block (thrust stays 0, heading held),
    // and importantly does NOT touch ai_desired_speed. The entry reset above
    // only rewrites a non-negative desired to eff_max_speed; a negative value
    // (e.g. state-15's -30 or -15 emergence speed seeded by
    // NovaAi_EnterState15JumpOutToSystem) survives the hold intact and is
    // handed to the mode-0x0a arrival slowdown, which keeps an already-negative
    // value. Clobbering it here made gate-emergence ships reseed to -50 and
    // fly ~1.67x too fast / ~2.8x too far (Ghidra 0x00408150 mode 0 has no
    // desired-spec write; the auto-weapon select is the only body).
    // Mode 0x17 likewise has no steering block: state 0x14 owns the
    // gate/wormhole handoff and velocity bookkeeping; the surrounding jump
    // path performs the actual transfer.
    break;

  case 0x15:
    // Jump-in positioning: the original scans the 64 freeflight anchors for
    // the nearest, steers at it (thrust on align, else 1.75x-thrust damp), and
    // drops to control mode 0 when no anchor exists. No freeflight anchors are
    // modelled, so the original's no-anchor outcome (mode 0) holds.
    // TODO(decomp): reconstruct freeflight-anchor positioning. Like the
    // original, this mode never writes
    // ai_desired_speed (it transitions to mode 0, which preserves a seed).
    ship.ai_control_mode = 0;
    break;

  case 0x16:
    // Jump-out / retreat: steer at the target stellar's map coordinates and
    // select a general weapon once aligned. The original never thrusts here.
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
          // Ghidra 0x00408150 mode 0x16 calls Weapon_SelectGeneralWeaponBank
          // (0x0040d910) once aligned.
          NovaAi_SelectGeneralWeaponBank(state, ship);
        }
      }
    }
    ship.ai_desired_speed = 0.0F;
    break;

  case 0x12: {
    // Chase the formation leader: head at the point 15x max-speed in front
    // of the leader's heading (Math_AddPolarVelocity on the leader position)
    // and thrust within turn+20 deg. With no leader the original falls back
    // to control mode 0; state-4 guided-weapon selection arms a guided bank.
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
      // Ghidra 0x00408150 mode 0x12 calls
      // Weapon_SelectGuidedWeaponBankForPrimaryTarget (0x0040d220) for state
      // 4 with a primary target.
      NovaAi_SelectGuidedWeaponBankForPrimaryTarget(state, ship);
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
    // Depart from the system centre: point from the centre to the ship and
    // coast (desired = 0 -> max-speed clamp in the integrator), thrusting once
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
    // it. The population pass then replenishes the current system, matching
    // the original's departure lifecycle.
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
    ship.ai_station_hold_timer += elapsed_ticks;

    // Ghidra 0x00408150 compares elapsed 60 Hz tick time against
    // Stellar_GetJumpSequenceDuration60Hz() / jump_duration_multiplier. The
    // class multiplier is not decoded yet; use the faithful 350 base and
    // keep the lifecycle transition exact. (Port measures ms; see the
    // kNpcJumpSpinupDurationMs note.)
    if (static_cast<float>(now_ms - ship.ai_mode_start_time_ms) >=
        kNpcJumpSpinupDurationMs) {
      ship.ai_station_hold_timer = 0.0F;
      ship.is_active = false;
      ship.current_system_id = -1;
      ship.ai_forward_thrust_cmd = 0.0F;
      ship.ai_desired_speed = 0.0F;
    }
    break;

  case 0xd: {
    // Formation hold with leader release; squad_leader_ship_slot identifies
    // the followed leader. While the leader's station-hold timer runs (>1.0)
    // the ship
    // matches the leader's spin-up: damp to a standstill (0.95, desired -4.0,
    // timer +1) once the leader is within 11 deg of its own desired heading;
    // once the ship's own hold timer passes 30 and the leader is not the
    // player, the ship releases back to its class default behavior (state 2 /
    // mode 4). Otherwise it copies the leader's velocity and glow (formation
    // offset deferred).
    if (fire_restricted) {
      break;
    }
    const std::int16_t leader_slot = ship.squad_leader_ship_slot;
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
        ship.squad_leader_ship_slot = -1;
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
        ship.ai_maneuver_timer_ms = 180.0F; // normalized ticks, raw 0x43340000
      } else {
        ship.ai_desired_heading_deg = leader.ai_desired_heading_deg;
        ship.ai_station_hold_timer = -4.0F;
      }
    } else {
      ship.vel_x = leader.vel_x;
      ship.vel_y = leader.vel_y;
      // Ship_MoveShipTowardFormationOffset (0x00408150 mode-0xd block, snap=0)
      // + glow copy: the released hold path positions onto the wedge offset.
      if (ship.formation_leader_ship_slot > 0 &&
          state.SlotInRange(
              static_cast<std::size_t>(ship.formation_leader_ship_slot))) {
        NovaEscort_MoveTowardFormationOffset(
            state, ship, /*snap=*/false, elapsed_ticks);
      }
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
    // Formation-offset mirroring is deferred; the weapon-bank select for
    // states 3/4 arms the turret-ish current-target bank. Close (165 px/axis)
    // + the +0xBD latch breaks off to a boost (0x11); the latch has no
    // producer yet so the transition is inert.
    if (fire_restricted || ship.primary_target_ship_slot == -1) {
      break;
    }
    if (ship.formation_leader_ship_slot > 0 &&
        static_cast<std::size_t>(ship.formation_leader_ship_slot) <
            GameState::kMaxShips) {
      // Ship_MoveShipTowardFormationOffset (0x00408150 combat/hold mode
      // blocks) + glow copy: modes keep their wedge position while attacking.
      NovaEscort_MoveTowardFormationOffset(
          state, ship, /*snap=*/false, elapsed_ticks);
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
      // Ghidra 0x00408150 mode 5 calls Weapon_SelectWeaponBankForCurrentTarget
      // (0x0040ce00) for states 3/4.
      NovaAi_SelectWeaponBankForCurrentTarget(state, ship);
    }
    if (ship.afterburner_latch != 0 &&
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
    // arms a direct-fire bank and, for gravity-shield ships
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
    if (std::abs(heading_delta_deg()) < eff_turn_deg + 15.0F) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      ship.ai_desired_speed = 0.0F;
    }
    if (std::abs(heading_delta_deg()) < eff_turn_deg * 3.0F) {
      // Ghidra 0x00408150 mode 6 arms a direct-fire bank within turn*3.
      NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(state, ship, false);
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
    if (ship.afterburner_latch != 0 && dx > kBreakOuterGatePx &&
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
    // target is beyond 165 px). Arms the current-target bank while close.
    if (fire_restricted || ship.primary_target_ship_slot == -1) {
      break;
    }
    const std::int16_t target_slot = ship.primary_target_ship_slot;
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    ship.ai_desired_heading_deg = ship.ai_evasive_heading_deg;
    ship.ai_forward_thrust_cmd = eff_thrust * kEvasiveThrustFactor;
    ship.ai_desired_speed = 0.0F;
    if (std::abs(ship.pos_x - target.pos_x) < kCombatCloseRange &&
        std::abs(ship.pos_y - target.pos_y) < kCombatCloseRange) {
      // Ghidra 0x00408150 mode 0x10 arms the current-target bank only while
      // both axes stay within 165 px (the original uses AND, not OR).
      NovaAi_SelectWeaponBankForCurrentTarget(state, ship);
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
    // the straight bearing, weapon selection at alignment, and a
    // return to combat pursuit (6) inside 165 px (or a 1-in-100 roll further
    // out). TODO(decomp): formation-offset mirroring.
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
      // Ship_MoveShipTowardFormationOffset (0x00408150 combat/hold mode
      // blocks) + glow copy: modes keep their wedge position while attacking.
      NovaEscort_MoveTowardFormationOffset(
          state, ship, /*snap=*/false, elapsed_ticks);
      ship.engine_glow_level =
          state
              .ShipAt(static_cast<std::size_t>(ship.formation_leader_ship_slot))
              .engine_glow_level;
    }
    const float dx = std::abs(ship.pos_x - target.pos_x);
    const float dy = std::abs(ship.pos_y - target.pos_y);
    if (dx < kCombatCloseRange && dy < kCombatCloseRange) {
      // Ghidra 0x00408150 mode 0x11 selects the turret-ish current bank first,
      // then direct-fire within turn*3 while it is close.
      NovaAi_SelectWeaponBankForCurrentTarget(state, ship);
      if (std::abs(heading_delta_deg()) < eff_turn_deg * 3.0F) {
        NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(state, ship, false);
      }
    } else if (std::abs(heading_delta_deg()) < eff_turn_deg * 3.0F) {
      // Far: within turn*3 the original arms a guided bank.
      NovaAi_SelectGuidedWeaponBankForPrimaryTarget(state, ship);
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
    // live; deferred -> straight bearing), select direct-fire/current banks at
    // the turn*3 gate and a guided bank at the turn*4 thrust gate, then break
    // off to a boost (0x11)
    // beyond 82 px/axis when the 0xBD latch is set and the target bearing is
    // within 31 deg. TODO(decomp): formation-offset mirroring.
    if (fire_restricted || ship.primary_target_ship_slot == -1) {
      break;
    }
    if (ship.formation_leader_ship_slot > 0 &&
        static_cast<std::size_t>(ship.formation_leader_ship_slot) <
            GameState::kMaxShips) {
      // Ship_MoveShipTowardFormationOffset (0x00408150 combat/hold mode
      // blocks) + glow copy: modes keep their wedge position while attacking.
      NovaEscort_MoveTowardFormationOffset(
          state, ship, /*snap=*/false, elapsed_ticks);
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
      // Ghidra 0x00408150 mode 7 selects direct-fire then the current-target
      // bank within turn*3.
      NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(state, ship, false);
      NovaAi_SelectWeaponBankForCurrentTarget(state, ship);
    }
    if (std::abs(heading_delta_deg()) < eff_turn_deg * 4.0F) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      ship.ai_desired_speed = 0.0F;
      // Ghidra 0x00408150 mode 7 arms a guided bank within turn*4, alongside
      // the thrust/scalar-speed write.
      NovaAi_SelectGuidedWeaponBankForPrimaryTarget(state, ship);
    }
    if (ship.afterburner_latch != 0 &&
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
    // ship creeps its position toward squad_leader_ship_slot at 10x thrust per
    // frame and drops desired speed by the same step; inside, the original
    // launches/hands off the escort (TODO(decomp)) or clears the escort
    // for a non-capturable player escort. A destroyed/disabled ship or
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
      // Creep toward the followed lead (squad_leader_ship_slot) at 10x thrust.
      const float step = eff_thrust * kFormationCreepFactor * elapsed_ticks;
      const std::int16_t lead_slot = ship.squad_leader_ship_slot;
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
      // Inside the escort half-span: dock arrival. The original docks the
      // fighter into the carrier's bay (Ship_LaunchCarriedShipFromBay
      // 0x00415ea0) when the player can capture/retain that class
      // (ShipClass_HasPlayerBayCapacityFor 0x004694a0); otherwise it
      // clears the escort handoff.
      if (ship.ship_instance_id == 0) {
        if (NovaShipClass_HasPlayerBayCapacityFor(state, ship.ship_class_id)) {
          NovaShip_RecoverCarriedShipToBay(state, ship);
        } else {
          ship.ai_state_code = 0xc;
          ship.ai_control_mode = 0;
          ship.primary_target_ship_slot = -1;
          ship.ai_secondary_target_slot = -1;
        }
      } else {
        NovaShip_RecoverCarriedShipToBay(state, ship);
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
    // runs first. TODO(decomp): formation-offset mirroring.
    if (fire_restricted) {
      break;
    }
    if (ship.formation_leader_ship_slot > 0 &&
        static_cast<std::size_t>(ship.formation_leader_ship_slot) <
            GameState::kMaxShips) {
      // Ship_MoveShipTowardFormationOffset (0x00408150 combat/hold mode
      // blocks) + glow copy: modes keep their wedge position while attacking.
      NovaEscort_MoveTowardFormationOffset(
          state, ship, /*snap=*/false, elapsed_ticks);
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
    // is deferred (TODO(decomp)).
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
        const float step = eff_thrust * kFormationCreepFactor * elapsed_ticks;
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
        // Ship_MoveShipTowardFormationOffset (0x00408150 mode-0xc else arm,
        // snap=0): with no state-7 anchor the ship still keeps its assigned
        // wedge offset.
        NovaEscort_MoveTowardFormationOffset(
            state, ship, /*snap=*/false, elapsed_ticks);
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
    // one frame-time unit per frame.
    if (fire_restricted || ship.ai_secondary_target_slot == -1) {
      break;
    }
    const std::int16_t target_slot = ship.ai_secondary_target_slot;
    Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
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
          ship.speed = std::max(0.0F, ship.speed - eff_thrust * elapsed_ticks);
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
        add_polar_step(ship.pos_x, ship.pos_y, bearing, elapsed_ticks);
      } else {
        // Within 3 px (FLOAT_00575120): boarding/assist resolution. With the
        // maneuver timer spent and the ship not in the post-board drift (state
        // 0xe), latch the victim as a boarding target and arm the 100..179-tick
        // pause (random 0x50 + 100) that the capture supervisor's handoff
        // (Ship_UpdateShipAiBehavior0x03_WarshipCapture 0x004038b0) waits on
        // before Outfit_BoardShipAndTransferCargo. While the pause counts down
        // inside (0,100] in assist state 0xf (comm-window Request Assistance),
        // the arm instead clears the latch and repairs the victim back above
        // the disable threshold (+1.0 armor per tick, FLOAT_00575018).
        if (ship.ai_maneuver_timer_ms <= 0.0F) {
          if (ship.ai_state_code != 0xe) {
            target.boarded_target_latch = 1;
            ship.ai_maneuver_timer_ms = static_cast<float>(
                std::uniform_int_distribution<int>{0, 0x4f}(state.rng) + 100);
          }
        } else if (ship.ai_maneuver_timer_ms <= 100.0F &&
                   ship.ai_state_code == 0xf) {
          target.boarded_target_latch = 0;
          while (NovaAiShip_IsDisabled(state, target)) {
            target.armor_points += 1.0F;
          }
        }
      }
    }
    break;
  }

  case 0xe: {
    // Evade / break: while still moving (>= 0.35 px/tick) brake like mode 1
    // (reverse of the velocity bearing, or a predictive aim when a weapon
    // bank is live -- deferred to the straight bearing), thrust once within
    // turn+1 deg; gravity-shield ships reverse-thrust. Once slow, damp by 0.95
    // and steer at the target, arming direct-fire/guided/current banks within
    // turn*3 deg. The carrier-bay launch (Ship_LaunchShipFromCarrierBay) is
    // deferred.
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
        // Ghidra 0x00408150 mode 0xe arms direct-fire then guided within
        // turn*3 once the ship is slow enough.
        NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(state, ship, false);
        NovaAi_SelectGuidedWeaponBankForPrimaryTarget(state, ship);
      }
      // Ghidra 0x00408150 mode 0xe also runs the turret-ish current-target
      // select each slow frame.
      NovaAi_SelectWeaponBankForCurrentTarget(state, ship);
    }
    // Mode-0xe tail: the carrier-bay launch driver runs every frame while
    // the mode is active (original: after the fast/slow branches, guarded
    // only by the mode + target + fire-restriction gate above).
    NovaShip_LaunchShipFromCarrierBay(state, ship);
    break;
  }

  case 10:
    // Arrival slowdown: the negative desired speed is a heading-aligned
    // physics override, while the negative command supplies its per-tick
    // decay. Raw bits: 0xC2480000 (-50) / 0xBF951EB8 (about -1.165).
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
    // unguided weapon is armed at the alignment gate; the lead-velocity aim
    // (Ship_AimWeaponLeadVelocity) remains deferred, so the straight bearing
    // at the target stands in for the merge gate.
    if (!fire_restricted) {
      const AsteroidState &target = state.asteroid_pool[0];
      const float step = eff_thrust * kEvasiveThrustFactor * elapsed_ticks;
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
        // Ghidra 0x00408150 mode 0x14 clears the primary target and arms an
        // unguided bank once aligned at the asteroid target.
        NovaAi_SelectUnguidedWeaponBank(state, ship);
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
    NovaEscort_UpdateFormations(state, ship, /*snap=*/false);
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

  // The original sentinel arm bypasses the entire behavior/disabled branch,
  // then runs only the state and control tail. State 8 restores -999 each
  // frame, keeping behavior supervisors from stealing the arrival until the
  // negative-speed integrator completes it.
  if (run_heavy && !restricted && !arrival_slowdown_sentinel) {
    // Dispatch on behavior code. Availability-driven hulls (class Flags3 bit
    // 0x1/0x2) with no squad leader run the mining/destroyer supervisor
    // instead of the ai_behavior_code branch. The original checks a stellar
    // target first and would run Ship_ShouldShipPrioritizePlayerThreat
    // (0x00405120, still deferred); those ships stay on the behavior path for
    // now, so this gate requires target_stellar_object_id == -1 too.
    const std::int16_t behavior = ship.ai_behavior_code;
    const ShipClass *dispatch_class = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    const bool availability_driven =
        dispatch_class != nullptr &&
        (dispatch_class->availability_flags & 3U) != 0U &&
        ship.squad_leader_ship_slot == -1 &&
        ship.target_stellar_object_id == -1;
    if (availability_driven) {
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
      // Ship_UpdateShipAssistResponseBehavior (0x004048a0), now ported as
      // NovaAi_UpdateAssistResponseBehavior (replaces the former force-state-10
      // divergence glue; see that function for the deferred slices). The
      // State 9/0xf exclusions live inside the supervisor, matching the
      // original. A valid state-8 arrival never reaches this branch because
      // its -999 station-hold sentinel takes the dispatcher arm above.
      NovaAi_UpdateAssistResponseBehavior(state, ship, now_ms);
    }
  }

  // Always run the state machine + controls (the original does so after the
  // heavy block even when bVar6 skipped the heavy decision). elapsed_ticks is
  // the normalized cadence published by the original's misleadingly named
  // _g_avg_frame_time_ms EMA (about 1.0 at 30 Hz).
  NovaAi_UpdateShipState(state, ship, now_ms, elapsed_ticks);
  NovaAi_ApplyControls(state, ship, elapsed_ticks, now_ms);
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

// Ghidra 0x004048a0 Ship_UpdateShipAssistResponseBehavior. Per-frame supervisor
// for behavior > 4 ships (carried fighters = 5, escorts = 6): keeps the squad
// attached to its leader, decodes the escort command (player group command /
// sub-leader mirror), and arms the leader-jump-prep sync that enters AI state
// 0x0B. Deferred slices are marked TODO(decomp) inline.
void NovaAi_UpdateAssistResponseBehavior(GameState &state,
                                         Ship &ship,
                                         std::uint32_t now_ms) {
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
      ship.voice_type_mode = NovaAiRandomRange(state, 2);
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
    if (leader_slot != 0 && NovaShip_HasGravityShield(ship, *cls)) {
      // Gravity-shield ships detach from NPC leaders and travel on their own
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
  // g_pending_combat_chatter_kind == -1; the port's consumer pass (Frame_
  // UpdateCombatChatter) is TODO(decomp) and never resets the latch, so this
  // stays silent until that lands.
  auto queue_attack_chatter = [&]() {
    if (ship.squad_leader_ship_slot != 0 ||
        ship.primary_target_ship_slot == -1 ||
        state.pending_combat_chatter_kind != -1 ||
        state.pending_combat_chatter_variant != 0 || cls->class_category >= 2 ||
        (cls->flags_secondary & 0x10U) != 0U ||
        NovaAiRandomRange(state, 3) != 0) {
      return;
    }
    NovaFrame_QueueCombatChatter(
        state, 1, cls->inherent_attributes_govt, ship.voice_type_mode);
  };

  if (ship.ai_state_code == 0xb) {
    // State-0x0B maintenance: the hold state owns the ship until the leader's
    // jump fires (or the leader exits, handled by the 0x00401000 exits).
    ship.primary_target_ship_slot = -1;
    if (leader_slot != 0 && NovaShip_HasGravityShield(ship, *cls)) {
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
  if (ship.target_stellar_object_id != -1) {
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

// Ghidra 0x0040fc00 Ship_IsShipEligibleForCommAidInteraction.
bool NovaAiShip_IsShipEligibleForCommAidInteraction(const GameState &state,
                                                    const Ship &ship) {
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

// Ghidra 0x004112a0 Ship_IsShipInAiState0x02.
bool NovaAiShip_IsShipInAiState2(const Ship &ship) {
  return ship.ai_state_code == 2;
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
  if (distressed.pers_def_slot == 0x3ff) {
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

// Ghidra 0x00410110 Ship_HasIncomingDistressSupportForShip.
bool NovaAiShip_HasIncomingDistressSupport(const GameState &state,
                                           const Ship &ship,
                                           const Ship &context_ship) {
  if (ship.ship_instance_id == 0) {
    return NovaAiShip_HasShipDistressResponder(state, context_ship);
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
        NovaAiShip_CanShipRespondToDistressCall(
            state, context_ship, candidate)) {
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
        follower.target_stellar_object_id != -1) {
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
    const auto pick = static_cast<std::int16_t>(NovaAiRandomRange(state, 0x40));
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

namespace {

// Math_ShortestAngleDeltaDeg (0x0046b210): the [0,180] magnitude of the
// shortest angular separation between two game bearings.
std::int16_t ShortestAngleDeltaDeg(std::int16_t a, std::int16_t b) {
  int diff = static_cast<int>(a) - static_cast<int>(b);
  int mag = (diff ^ (diff >> 31)) - (diff >> 31); // abs
  if ((a > 0xb3) != (b > 0xb3)) {
    mag = 0x168 - mag;
  }
  if (mag > 0xb4) {
    mag = 0x168 - mag;
  }
  return static_cast<std::int16_t>(mag);
}

// Ghidra 0x0046b360 Math_ShortestAngleDeltaDeg helper stays file-local below;
// the blind-spot sector test is exported (see NovaAi_WeaponIsTargetBearingIn-
// TurretBlindSpot after this namespace).

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

// Ghidra 0x0046b360 Weapon_IsTargetBearingInTurretBlindSpot (formerly the
// misnamed Weapon_IsWeaponArcAllowed): whether the bearing lies in one of the
// weapon's turret blind-spot sectors. Front (<46 deg), side (<136 deg), rear;
// a sector is BLIND when the weapon's flags_primary 0x1000/0x2000/0x4000 is
// set, force-overridden ON by the matching ShipClass capability flags (Bible:
// "Turreted weapon has a blind spot to the front/sides/rear"). Callers reject
// the bank while the target is in a blind spot.
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
  // TODO(decomp(0x0043a310)) skipped: the head-of-function
  // Weapon_SelectTurretTargetWithinArc point-defense auto-selection (which
  // can fire a turret shot this frame and pick the target itself) is not
  // reconstructed; turret banks still fire through the normal firing path.
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
  const std::int16_t heading_deg =
      static_cast<std::int16_t>(std::lround(heading_deg_f));

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
        // Round the continuous |bearing - heading| difference before the
        // mod-360 wrap, exactly as the original.
        delta =
            std::lround(std::abs(static_cast<float>(bearing) - heading_deg_f));
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
  // TODO(decomp(0x004221d0)) skipped: Ship_IsInboundThreatExceedingDefenses
  // (decline guided selection when the target's inbound_weapon_threat exceeds
  // its shield+armor*1.05 budget) is not modelled -- no inbound-threat field
  // exists -- so the original's early-return under heavy incoming fire is not
  // reproduced.
  const ShipClass *ship_class = ShipClassFor(state, ship);
  const float distance_sq =
      SquaredDistance(ship.pos_x, ship.pos_y, target.pos_x, target.pos_y);

  std::int16_t chosen = -1;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const WeaponBankState bs = ReadWeaponBank(state, ship, bank);
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (weapon == nullptr || bs.ammo <= 0) {
      continue;
    }
    const bool track_ok =
        ship_class != nullptr && WeaponCanTrackTarget(*ship_class, *weapon);
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
    // Bible shïp Flags 0x0020: afterburner when the player has an advanced
    // combat rating. The original rolls NovaRandom_Range(0x540) and compares
    // roll + 0x100 against rating / class Strength (the original divides
    // unguarded; zero-strength classes would fault, so the division is
    // guarded here).
    const auto roll = static_cast<int>(
        std::uniform_int_distribution<int>{0, 0x53f}(state.rng));
    const auto threshold =
        static_cast<int>(state.player_combat_rating_points /
                         (cls->strength != 0 ? cls->strength : 1));
    return roll + 0x100 <= threshold;
  }
  return false;
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
