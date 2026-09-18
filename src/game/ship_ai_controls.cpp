#include "ship_ai.hpp"

#include "ship_ai_internal.hpp"

#include <algorithm>
#include <cmath>
#include <random>

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

// --- Movement constants the original reads from _DAT_00575xxx globals at
// 0x00575000..0x00575200 (float vs double chosen by instruction width). See
// docs/npc_ship_behaviour.md "Decoded constants". ---
// Mode-1 (damp/brake) ladder: fast threshold 0.35 (0x575080), "still fast"
// 1.75 (0x575118), half-thrust factor 0.5 (0x575038), aligned damp 0.94
// (0x5750f8), nearly-stopped damp 0.95 (0x5750f0), alignment addend 1.0
// (0x575018).
constexpr float kMode1FastThreshold = 0.35F;
constexpr float kMode1SlowThrustFactor = 0.5F;
constexpr float kMode1AlignAddend = 1.0F;
// Mode-2 (travel): alignment addend 5.0 (0x575100, float), "close to stellar"
// gate 500 px/axis (0x575104, float), arrival cruise fraction 0.25
// (0x575108, double).
constexpr float kMode2AlignAddend = 5.0F;
constexpr float kMode2CloseGatePx = 500.0F;
constexpr float kMode2ArriveFraction = 0.25F;
// Mode-3 (depart from centre): alignment addend 3.0 (0x575120, float).
constexpr float kMode3AlignAddend = 3.0F;
// Stellar_GetJumpSequenceDuration60Hz (0x0046EFB0) returns 350 for the
// engine-enabled path used by the NPC spin-up; the original threshold is
// 350 / ShipClassDef.jump_duration_multiplier (decoded in the shp loader
// 0x004bd3c0). This NPC path still uses the 1.0 multiplier (a follow-up gap);
// NOTE: the original compares against 60 Hz tick elapsed time, i.e. a ~5.8 s
// NPC spin-up, while the port's NPC path measures wall-clock ms (350 ms).
// TODO(decomp) apply the class multiplier and unify on the 60 Hz tick unit.
constexpr float kNpcJumpSpinupDurationMs = 350.0F;

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
// Control-mode 10 arrival slowdown: start at 50 px/tick along the heading and
// reduce that override by 1.165 each tick (raw bits 0xC2480000 / 0xBF951EB8).
constexpr float kMode10ReverseSpeed = -50.0F;
constexpr float kMode10ReverseThrust = -1.165F;
// Escort-follow half-span stand-in: Sprite_GetShipClassEscortFrameHeight
// (0x004624c0) returns the class escort sprite's shot half-span or its 0x4B =
// 75 px debug fallback; the clean-room sprite tables are not modelled, so the
// debug fallback stands in (TODO(decomp)).
constexpr float kEscortHalfSpanPx = 75.0F;

} // namespace

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
  // Ship_ComputeShipMaxTurnRateDeg): class base values, government SkillMult
  // applied to speed/thrust, no outfit/status yet. Same
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
  // ai_desired_heading_deg to the truncated current heading every frame, before
  // the mode switch. Steering modes overwrite it immediately after; unsteered
  // modes (0 idle drift, 10 arrival slowdown) inherit "desired == current" and
  // therefore hold heading exactly -- this is what keeps the state-8 jump-in
  // and gate-emergence glides straight instead of wheeling toward a stale
  // steering target. ShipState stores the field in degrees while the clean-room
  // heading is radians, so convert at this boundary.
  ship.ai_desired_heading_deg =
      static_cast<std::int16_t>(WrapDeg(ship.heading / kDegToRad));

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

  // Ghidra 0x0046b330 NovaAi_PlayerCombatRatingGate. Rolls
  // NovaRandom_Range(0x540) and succeeds when the player's combat-rating
  // points reach roll + 0x100: the pass chance rises from 0 at <= 0x100
  // (256) points to 1 at >= 0x63f (1599). The original shares the global LCG;
  // the clean-room draws from GameState.rng like the other ports.
  auto player_combat_rating_gate = [&]() {
    const int roll = static_cast<int>(
        std::uniform_int_distribution<int>{0, 0x53f}(state.rng));
    return roll + 0x100 <= state.player_combat_rating_points;
  };

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
    // NovaAi_EnterState15EmergeFromHypergate) survives the hold intact and is
    // handed to the mode-0x0a arrival slowdown, which keeps an already-negative
    // value. Clobbering it here made gate-emergence ships reseed to -50 and
    // fly ~1.67x too fast / ~2.8x too far (Ghidra 0x00408150 mode 0 has no
    // desired-spec write; the auto-weapon select is the only body).
    // TODO(decomp(0x00408150)): the original calls
    // Ship_EscortFireAtUnprovokedTarget at the tail of control mode 0
    // (0x0040847d). The port omits it here and runs the refresh from the
    // post-state call in NovaAi_UpdateShipAI instead, which covers every
    // control mode (including 0x17 and modes where the original has no call).
    // See docs/npc_ship_behaviour.md.
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
    // damp by 0.95 and drop to idle control mode 0. Inertialess ships brake
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
    if (cls == nullptr || !NovaShip_IsInertialess(ship, *cls)) {
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
    NovaAi_EscortFireAtUnprovokedTarget(state, ship);
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
    // class multiplier is decoded (ShipClassDef +0x44) but this NPC path still
    // uses the 1.0 base; keep the lifecycle transition exact. (Port measures
    // ms; see the kNpcJumpSpinupDurationMs note.) TODO(decomp(0x00408150))
    // skipped: NPC spin-up class multiplier.
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
    // timer +1 raw-call unit) once the leader is within 11 deg of its own
    // desired heading;
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
    ship.ai_desired_heading_deg = static_cast<std::int16_t>(leader_heading_deg);
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
                             std::trunc(leader_heading_deg),
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
        ship.ai_station_hold_timer +=
            elapsed_ticks / kOriginalMaxRateFrameTicks;
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
        Ship_MoveShipTowardFormationOffset(
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
      Ship_MoveShipTowardFormationOffset(
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
    // arms a direct-fire bank and, for inertialess ships
    // within 100 px, throttles the cruise down to the target's scalar speed.
    // Break-off: at/inside 165 px (or any distance for non-inertialess ships)
    // the evasive-break order fires -- a player target must beat the
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
      const bool inertialess =
          cls != nullptr && NovaShip_IsInertialess(ship, *cls);
      if (inertialess && std::abs(ship.pos_x - target.pos_x) < 100.0F &&
          std::abs(ship.pos_y - target.pos_y) < 100.0F &&
          target.speed < ship.speed) {
        ship.ai_desired_speed = target.speed;
      }
    }
    const float dx = std::abs(ship.pos_x - target.pos_x);
    const float dy = std::abs(ship.pos_y - target.pos_y);
    const bool inertialess =
        cls != nullptr && NovaShip_IsInertialess(ship, *cls);
    if (dx < kCombatCloseRange || dy < kCombatCloseRange || !inertialess) {
      // Ghidra 0x00408150 mode 6: the primary target slot 0 (player) must
      // pass the rating gate; any other target rolls NovaRandom_Range(2) and
      // bypasses the gate on 0, otherwise requires the gate to pass.
      bool allowed = false;
      if (target_slot == 0) {
        allowed = player_combat_rating_gate();
      } else if (std::uniform_int_distribution<int>{0, 1}(state.rng) == 0) {
        allowed = true;
      } else {
        allowed = player_combat_rating_gate();
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
            ship.ai_evasive_heading_deg = wrap_deg_int(static_cast<int>(
                cur_deg + (even_instance ? kEvasiveHeadingOffsetDeg
                                         : -kEvasiveHeadingOffsetDeg)));
            ship.ai_desired_heading_deg =
                wrap_deg_int(static_cast<int>(cur_deg));
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
    // hull is within turn*3 deg of it (or, for inertialess ships, once the
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
    const bool inertialess =
        cls != nullptr && NovaShip_IsInertialess(ship, *cls);
    if (!inertialess) {
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
      Ship_MoveShipTowardFormationOffset(
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
      Ship_MoveShipTowardFormationOffset(
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
    NovaAi_EscortFireAtUnprovokedTarget(state, ship);
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
      Ship_MoveShipTowardFormationOffset(
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
    NovaAi_EscortFireAtUnprovokedTarget(state, ship);
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
    const bool inertialess =
        cls != nullptr && NovaShip_IsInertialess(ship, *cls);
    if (inertialess || (std::abs(rel_x) < kVelocityMatchTol &&
                        std::abs(rel_y) < kVelocityMatchTol)) {
      ship.vel_x = target.vel_x;
      ship.vel_y = target.vel_y;
      const float target_heading_deg = target.heading / kDegToRad;
      const float cur_deg = ship.heading / kDegToRad;
      const float delta_deg = std::abs(
          std::remainder(std::trunc(target_heading_deg) - std::trunc(cur_deg),
                         kFullCircleDeg));
      float window = 25.0F;
      if (ship.skill_variance_scale > 0.0F) {
        window = 25.0F / ship.skill_variance_scale;
      }
      if (delta_deg < 1.0F || window <= delta_deg) {
        ship.ai_desired_heading_deg =
            static_cast<std::int16_t>(target_heading_deg);
      } else {
        const float signed_delta =
            std::remainder(std::trunc(target_heading_deg) - std::trunc(cur_deg),
                           kFullCircleDeg);
        std::int16_t desired = static_cast<std::int16_t>(cur_deg);
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
        Ship_MoveShipTowardFormationOffset(
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
    NovaAi_EscortFireAtUnprovokedTarget(state, ship);
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
    const bool inertialess =
        cls != nullptr && NovaShip_IsInertialess(ship, *cls);
    if (std::abs(rel_x) >= kVelocityMatchTol ||
        std::abs(rel_y) >= kVelocityMatchTol) {
      if (!inertialess) {
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
          static_cast<std::int16_t>(target.heading / kDegToRad);
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
        // before Boarding_BoardShipAndTransferCargo. While the pause counts
        // down inside (0,100] in assist state 0xf (comm-window Request
        // Assistance), the arm instead clears the latch and repairs the victim
        // back above the disable threshold (+1.0 armor per tick,
        // FLOAT_00575018).
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
    // turn+1 deg; inertialess ships reverse-thrust. Once slow, damp by 0.95
    // and steer at the target, arming direct-fire/guided/current banks within
    // turn*3 deg. The carrier-bay launch (Ship_LaunchShipFromCarrierBay) is
    // deferred.
    if (fire_restricted || ship.primary_target_ship_slot == -1) {
      break;
    }
    const std::int16_t target_slot = ship.primary_target_ship_slot;
    const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
    const bool inertialess =
        cls != nullptr && NovaShip_IsInertialess(ship, *cls);
    if (std::abs(ship.vel_x) >= kVerySlowSpeed ||
        std::abs(ship.vel_y) >= kVerySlowSpeed) {
      if (!inertialess) {
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
      if (std::abs(heading_delta_deg()) < eff_turn_deg + kScriptTurnAddend) {
        ship.ai_forward_thrust_cmd = eff_thrust;
      }
    }
    break;

  case 0x14:
    // Scripted velocity-match: ramp velocity toward the asteroid-pool slot-0
    // target velocity at 1.5x thrust per frame-time unit, then creep position
    // within the 150/80 px/axis windows (the original's banded weave). The
    // heading is the lead-velocity intercept (Ship_AimWeaponLeadVelocity) and
    // the unguided weapon is armed at the alignment gate.
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
      // Ghidra 0x00408150 mode 0x14: lead-aim at the scripted asteroid with
      // the active weapon bank (Ship_AimWeaponLeadVelocity 0x0043b8c0).
      ship.ai_desired_heading_deg =
          NovaAi_AimWeaponLeadVelocity(state,
                                       ship,
                                       target.target_pos_x,
                                       target.target_pos_y,
                                       target.target_vel_x,
                                       target.target_vel_y,
                                       ship.active_weapon_bank_slot,
                                       ship.pos_x,
                                       ship.pos_y);
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

} // namespace game
