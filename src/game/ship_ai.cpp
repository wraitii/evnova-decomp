// Clean-room reconstruction of the NPC ship AI decision layer. See ship_ai.hpp
// for the module overview and the Ghidra-address mapping of each helper.
//
// Porting notes:
//  * Ships carry no outfit inventory, so "has gravity shield" uses the NPC
//    branch of Outfit_ShipHasGravityShieldOutfit (0x0046df70) via
//    NovaShip_HasGravityShield, and "effective stats" are the class base
//    values (the player's outfit/status damping is not modelled yet).
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
#include <random>
#include <vector>

#include "government.hpp"
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
// Engagement distance within which a ship can fire / apply disable pressure
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
// PROVISIONAL turn-radius range used by the port's own pursuit/assist range
// gates (states 5/0xf; the original uses Ship_CanShipInterceptCurrent-
// PrimaryTarget instead). NOT the decoded state-1 travel arrive range above.
constexpr float kTurnRadiusBase = 10.0F;
constexpr float kTurnRadiusScale50 = 50.0F;

constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
constexpr float kFullCircleDeg = 360.0F;

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
  // Critically-damaged gate: armor below a fraction of max armor. Class
  // capability_flags & 0x10 chooses the stricter/laxer ratio.
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const float max_armor = cls ? static_cast<float>(cls->base_armor) : 0.0F;
  if (max_armor > 0.0F) {
    const float ratio =
        (cls && (cls->capability_flags & 0x10) != 0) ? 0.2F : 0.25F;
    if (ship.armor_points < max_armor * ratio) {
      return true;
    }
  }
  return false;
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

// Ghidra 0x00405590 Ship_UpdateShipAiState. The per-frame state machine. This
// is a substantial function; the reconstruction below covers the core
// movement/control-mode decision for the states the reimplementation drives
// (travel, wander, escort-follow, hold, drift, disengage, defunct) and writes
// Ship.ai_control_mode accordingly. The disable-pressure / board (0xd) and
// ship-attack (3/4/0xc) branches are conservatively gated: they need the
// disable/weapon systems (Phase 5) to be faithfully exercised, so those
// helpers (Ship_CanShipApplyDisablePressureToTarget, Ship_CanShipIntercept-
// CurrentPrimaryTarget, Ship_IsShipFireRestricted) are used where available
// and otherwise defaulted permissive with TODO(decomp). HUD/mission flavor
// (extortion messages, fuel-transfer chatter) is no-op.
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
      const float turn_deg = cls ? static_cast<float>(cls->turn_rate) * 0.1F
                                 : 0.0F; // Ship_ComputeShipMaxTurnRateDeg NPC branch
      const float arrive_range =
          (kArriveRangeBase -
           std::min(turn_deg, kArriveRangeTurnCap)) *
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
    const float base_turn = cls ? cls->turn_rate : 0.0F;
    const std::int16_t range = static_cast<std::int16_t>(
        std::max(100, static_cast<int>((kTurnRadiusBase - base_turn) * 80.0F)));
    const Ship &tgt =
        state.ShipAt(static_cast<std::size_t>(ship.ai_target_ship_slot));
    if (std::abs(ship.pos_x - tgt.pos_x) > range ||
        std::abs(ship.pos_y - tgt.pos_y) > range) {
      ship.ai_control_mode = 0xb;
    } else {
      ship.ai_control_mode = 8;
    }
    // Disable-pressure downgrade: without the disable system the ship boosts
    // straight in (TODO: Ship_CanShipApplyDisablePressureToTarget).
    return;
  }

  // ---- Assistance/response (state 10) toward ai_target_ship_slot. ----
  if (ship.ai_state_code == 10 && ship.ai_target_ship_slot != -1 &&
      ship.target_stellar_object_id == -1) {
    ship.ai_secondary_target_slot = ship.ai_target_ship_slot;
    const Ship &tgt =
        state.ShipAt(static_cast<std::size_t>(ship.ai_target_ship_slot));
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

  // ---- Disabled pursuer/flee at turn radius (state 0xf). ----
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
    const float base_turn = cls ? cls->turn_rate : 0.0F;
    const std::int16_t range = static_cast<std::int16_t>(
        (kTurnRadiusBase - base_turn) * kTurnRadiusScale50 + 16.0F);
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

  // Everything else (states 3/4/0xc/0xd attack/engagement) writes the neutral
  // "idle" control mode until the disable/weapon systems (Phase 5) let us
  // exercise those branches faithfully. Concrete combat control modes (5/6/7/
  // 0xe for attack/mutual-target chains) are TODO(decomp).
  if (ship.ai_state_code == 0 || ship.ai_state_code == 3 ||
      ship.ai_state_code == 4) {
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
void NovaAi_ApplyControls(GameState &state, Ship &ship, float frame_time_ms) {
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
    return std::remainder(
        static_cast<float>(ship.ai_desired_heading_deg) - cur_deg,
        kFullCircleDeg);
  };

  switch (ship.ai_control_mode) {
  case 0:
  case 0x15:
  case 0x16:
  case 0x17:
    // Idle / static: no thrust, hold heading. 0x15/0x16 are the jump-in /
    // jump-out modes (freeflight-object / stellar steering, Phase 7) and 0x17
    // the jump-arrival hold -- the original Ship_ApplyShipAiControls has no
    // block for 0x17 (thrust stays 0), and 0x15/0x16 need the jump systems, so
    // all three park the ship.
    ship.ai_forward_thrust_cmd = 0.0F;
    ship.ai_desired_speed = 0.0F;
    break;

  case 1: {
    // Slow to a stop (original mode 1). While any velocity component is above
    // 0.35 px/tick, steer toward the reverse of the velocity bearing and brake
    // with the raw effective thrust; once aligned and below 1.75 px/tick brake
    // at half thrust while damping velocity by 0.94; once below 0.35 px/tick
    // damp by 0.95 and drop to idle control mode 0. Gravity-shield ships brake
    // with a NEGATIVE thrust command (the integrator's scalar-speed path). The
    // state-code-9 not-yet-aligned damp is kept for parity.
    if (std::abs(ship.vel_x) < kMode1FastThreshold &&
        std::abs(ship.vel_y) < kMode1FastThreshold) {
      ship.vel_x *= kMode1StopDamp;
      ship.vel_y *= kMode1StopDamp;
      ship.speed *= kMode1StopDamp;
      ship.ai_control_mode = 0;
      break;
    }
    if (cls == nullptr || !NovaShip_HasGravityShield(ship, *cls)) {
      ship.ai_desired_heading_deg = static_cast<std::int16_t>(WrapDeg(
          BearingDeg(0.0F, 0.0F, ship.vel_x, ship.vel_y) + 180.0F));
      if (std::abs(heading_delta_deg()) <
          eff_turn_deg + kMode1AlignAddend) {
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
    break;
  }

  case 2:
  case 9:
  case 0xb: {
    // Travel / pursue a map or ship coordinate in ai_secondary_target_slot (a
    // stellar resource id, or a ship slot). Steer the heading toward the
    // target; thrust only while the hull is within eff_turn_deg + 5.0 deg of
    // the bearing (the original's alignment gate -- the ship turns first, then
    // applies thrust once roughly aligned). Far from the stellar cruise at the
    // max-speed clamp (desired = 0); within 500 px per axis throttle to 25% of
    // max speed for the arrival slowdown.
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
    if (std::abs(heading_delta_deg()) <
        eff_turn_deg + kMode2AlignAddend) {
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
    ship.ai_desired_heading_deg = static_cast<std::int16_t>(
        BearingDeg(0.0F, 0.0F, ship.pos_x, ship.pos_y));
    if (std::abs(heading_delta_deg()) <
        eff_turn_deg + kMode3AlignAddend) {
      ship.ai_forward_thrust_cmd = eff_thrust;
      ship.ai_desired_speed = 0.0F;
    }
    break;

  case 4:
  case 0xd:
    // Station-keep / hold. The original mode 4 is the jump spin-up timer
    // (Phase 7) and mode 0xd the formation-hold
    // (Ship_MoveShipTowardFormationOffset); neither is wired, so keep the
    // placeholder hold behavior but with the real thrust value.
    // TODO(decomp): jump timer + formation offset.
    ship.ai_desired_speed = max_speed * 0.5F;
    ship.ai_forward_thrust_cmd = ship.speed > 0.1F ? eff_thrust : 0.0F;
    break;

  case 5:
  case 6:
  case 7:
  case 8:
  case 0xc:
  case 0xe:
  case 0xf:
  case 0x10:
  case 0x11:
  case 0x12:
  case 0x13:
  case 0x14:
  default:
    // Combat/formation/capture modes: steer toward the current heading and
    // cruise, so ships in these states still move fluidly rather than
    // hanging. Faithful per-mode polynomials are TODO(decomp)/Phase 5.
    ship.ai_desired_heading_deg =
        static_cast<std::int16_t>(WrapDeg(ship.heading / kDegToRad));
    ship.ai_desired_speed = max_speed;
    ship.ai_forward_thrust_cmd = eff_thrust;
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
      // Ship_UpdateShipAiBehavior0x02 (0x00402bd0): local "dude"/travel
      // behavior. Reuses the wander supervisor as a faithful-ish stand-in:
      // dude ships travel/wander and escalate hostility the same way. TODO:
      // the player-taunt / govt-assistance flavor is deferred.
      NovaAi_UpdateBehavior0x01(state, ship, now_ms);
    } else if (behavior == 3) {
      // Ship_UpdateShipAiBehavior0x03 / _CaptureVariant (0x00402e50/004038b0):
      // hostile attack / capture. The disable/capture systems are Phase 5, so
      // these default to the wander supervisor (they still pick travel targets
      // when idle) with a DEBUG note. TODO(decomp): real aggression.
      NovaAi_UpdateBehavior0x01(state, ship, now_ms);
    } else if (behavior == 4) {
      // Ship_UpdateShipAiCombatState (0x00403de0) -- Phase 5; wander stand-in.
      NovaAi_UpdateBehavior0x01(state, ship, now_ms);
    } else if (behavior > 4) {
      // Ship_UpdateShipAssistResponseBehavior (0x004048a0) -- assist; the
      // target-slot assist selection needs combat; wander stand-in.
      NovaAi_UpdateBehavior0x01(state, ship, now_ms);
    } else {
      // Ship_UpdateShipAiAvailabilityBehavior (0x00402980): class availability
      // drives cargo/travel; wander stand-in.
      NovaAi_UpdateBehavior0x01(state, ship, now_ms);
    }
  }

  // Always run the state machine + controls (the original does so after the
  // heavy block even when bVar6 skipped the heavy decision).
  NovaAi_UpdateShipState(state, ship, now_ms);
  NovaAi_ApplyControls(state, ship, 0.0F);
}

} // namespace game
