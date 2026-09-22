// Clean-room reconstruction of the NPC ship AI state-to-control decision layer.

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

#include "../util/math.hpp"

namespace game {

using namespace ship_ai_detail;

namespace {
constexpr float kArrivalDamp = 0.98F;
// State-1 travel arrive-range: (9.0 - min(turn_rate_deg, 8.0)) * 8.0 + 32.0
// px per axis (0x575070 / 0x575068 / 0x575078, doubles).
constexpr float kArriveRangeBase = 9.0F;
constexpr float kArriveRangeTurnCap = 8.0F;
constexpr float kArriveRangeScale = 8.0F;
constexpr float kArriveRangeOffset = 32.0F;
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
// Frame_MeasureFrameTiming multiplies elapsed milliseconds by 0.03 before
// publishing _g_avg_frame_time_ms, so the AI consumes normalized simulation
// ticks (about 1.0 at 30 Hz), not literal milliseconds.
// Inertialess approach multipliers (state 0xd/0xf).
constexpr float kInertialessKeepMult = 4.0F;
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
constexpr float kCombatStationRange = 251.0F;
// State-4 standoff weapon-range scale (0x5750b8 double, aliased at 0x575850);
// the reach is multiplied in x87 and the result truncated toward zero.
constexpr double kStandoffRangeScale = 0.85;
// Disabled standoff target halves the already-truncated reach (0x575038
// double 0.5), also truncated toward zero.
constexpr double kStandoffDisabledScale = 0.5;

} // namespace

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
        NovaAi_EnterState2ClearPrimaryTarget(state, ship);
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
          // (Flags3 bit 2) ships.
          std::uniform_int_distribution<std::int32_t> dist(
              (sc2 && (sc2->flags3 & 2) != 0) ? 100 : 300,
              (sc2 && (sc2->flags3 & 2) != 0) ? 174 : 499);
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
    const System *sys = CurrentSystem(state, ship);
    const std::int16_t half_span =
        sys ? static_cast<std::int16_t>(sys->pos_x > 0 ? 0x96 : 0x96)
            : 0x96; // System_GetCurrentSystemLinkSpriteWidth fallback
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
        other.defense_fleet_home_stellar_id = -1;
        other.ai_hostility_accumulator = -1;
        other.primary_target_ship_slot = -1;
        other.ai_secondary_target_slot = ship.ai_secondary_target_slot;
        other.ai_state_code = 0x14;
        other.ai_control_mode = 0;
      }
      // Gameplay-visible NPC gate/wormhole transfer. Mode 0x17 is only the
      // handoff marker in the AI/control switch; the original continues
      // through a surrounding presentation/transfer path, and the
      // gameplay-visible transfer completes once state 0x14 reaches the entry
      // point.
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
    // Ship_CheckSpecialLoadoutCapability (0x0046d080) bypasses the brake for
    // classes flagged Flags2 0x0020 or carrying the ModType-37 fast-jumping
    // outfit (owned inventory for the player; class default loadout for NPCs).
    const bool special_departure =
        NovaOutfit_HasFastJumpCapability(state, ship);
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
      ship.defense_fleet_home_stellar_id == -1) {
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
      ship.defense_fleet_home_stellar_id == -1) {
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
        return;
      }
      if (ship.pers_def_slot == 0x3ff) {
        // Player-controlled ship: the shareware/licence nag arm is not
        // reconstructed (see the TODO(decomp(0x00405590)) at this function's
        // entry). Nothing else runs for this branch.
        return;
      }
      // Within escort distance an NPC clears its target and (warships /
      // interceptors) scans the player for contraband.
      ship.ai_secondary_target_slot = -1;
      ship.primary_target_ship_slot = -1;
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      NovaShip_ScanPlayerForContraband(state, ship, now_ms);
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
  // within (10 - turn) * 30 + 50 px (x4 inertialess) of the cripple,
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
    // Inertialess ships scale the keeping range up (they drift at a larger
    // turn radius).
    const bool inertialess_0f = cls && NovaShip_IsInertialess(ship, *cls);
    const float full = inertialess_0f ? range * kInertialessKeepMult : range;
    ship.ai_control_mode = (std::abs(ship.pos_x - tgt.pos_x) > full ||
                            std::abs(ship.pos_y - tgt.pos_y) > full)
                               ? 0xb
                               : 0xf;
    return;
  }

  // ---- Scripted asteroid manoeuvre (state 0x10): the far approach (0x13)
  // or the close merge (0x14), picked by the class-turn-scaled distance gate
  // (Ghidra 0x00405590). Static hold (state 0x11) drives the anchor pick-up.
  // ----
  if (ship.ai_state_code == 0x10) {
    const AsteroidState &target = state.asteroid_pool[0];
    if (!target.active) {
      ship.ai_control_mode = 1;
    } else {
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      // ShipClassDef +0x38 is stored 10x in this port (see
      // NovaShip_ComputeEffectiveStats), so scale to degrees/tick.
      const float base_turn = cls != nullptr ? cls->turn_rate * 0.1F : 0.0F;
      // threshold = trunc((10.0 - base_turn) * 15.0) (x87 FIST + residual/sign
      // correction); outside it is 0x13.
      const float threshold = static_cast<float>(static_cast<int>(
          (kScriptAlignAddend - base_turn) * kScriptTurnAddend));
      if (threshold < std::abs(ship.pos_x - target.target_pos_x) ||
          threshold < std::abs(ship.pos_y - target.target_pos_y)) {
        ship.ai_control_mode = 0x13;
      } else {
        ship.ai_control_mode = 0x14;
      }
    }
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
      } else if (NovaOutfit_HasFastJumpCapability(state, ship) ||
                 (std::abs(ship.vel_x) < kVerySlowSpeed &&
                  std::abs(ship.vel_y) < kVerySlowSpeed)) {
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
      // Flags2 0x0002 (Bible "prefers standoff attacks"; carriers carry 0x82)
      // replaces the intercept test with a max-weapon-range envelope.
      const ShipClass *ship_class = ShipClassFor(state, ship);
      const bool standoff =
          ship_class != nullptr && (ship_class->flags_secondary & 0x0002U) != 0;
      if (dx > kCombatCloseRange || dy > kCombatCloseRange) {
        if (!standoff) {
          if (NovaAiShip_CanTargetOutrunShooter(state, ship)) {
            if (ship.ai_behavior_code < 3) {
              ship.ai_state_code = 3;
              ship.ai_control_mode = 5;
            } else {
              ship.ai_control_mode = 0xe;
            }
          } else if (!NovaAiShip_ShouldFollowSwarmMate(state, ship) &&
                     ship.ai_control_mode != 0x11) {
            // ShouldFollowSwarmMate commits mode 0x12 itself; only fall back
            // to the ordinary strafe mode when it declined.
            ship.ai_control_mode = 7;
          }
        } else {
          // Reach is (short)Weapon_GetShipMaxWeaponRange * 0.85 truncated
          // toward zero and narrowed to signed short, then halved and
          // truncated again for a disabled target (x87 FIST + sign backoff,
          // 0x00406a54..0x00406b4d). The range formatter clamps to 0x7fff, so
          // the 16-bit narrowing cannot bite.
          const std::int16_t range = static_cast<std::int16_t>(
              NovaWeapon_GetShipMaxWeaponRange(state, ship));
          const std::int16_t scaled = static_cast<std::int16_t>(
              evnova::util::TruncateToInt32(static_cast<float>(
                  static_cast<double>(range) * kStandoffRangeScale)));
          std::int16_t standoff_range = scaled;
          if (NovaAiShip_IsDisabled(state, target)) {
            standoff_range = static_cast<std::int16_t>(
                evnova::util::TruncateToInt32(static_cast<float>(
                    static_cast<double>(scaled) * kStandoffDisabledScale)));
          }
          // The enclosing far arm guarantees a >165 px axis, so the original's
          // redundant near check always takes the mode-0xe branch.
          ship.ai_control_mode = (static_cast<float>(standoff_range) < dx ||
                                  static_cast<float>(standoff_range) < dy)
                                     ? 7
                                     : 0xe;
        }
      } else if (!standoff) {
        if (ship.ai_control_mode != 0x10) {
          ship.ai_control_mode = 6;
        }
      } else {
        ship.ai_control_mode = 5;
      }
      // LAB_00406bc2 tail: while a state-4 target stays valid and the ship is
      // not disabled, every frame runs the carrier-bay launch driver, then
      // Government_TryTriggerGovtAssistanceEncounter (0x00413610).
      if (!NovaAiShip_IsDisabled(state, ship)) {
        NovaShip_LaunchShipFromCarrierBay(state, ship);
        (void)NovaGovernment_TryTriggerAssistanceEncounter(
            state, ship, /*force=*/false);
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
        if (NovaAiShip_CanTargetOutrunShooter(state, ship)) {
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
      if (cls != nullptr && NovaShip_IsInertialess(ship, *cls)) {
        range *= kInertialessKeepMult;
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

} // namespace game
