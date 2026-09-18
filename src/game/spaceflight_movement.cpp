#include "spaceflight.hpp"

#include "../log.hpp"
#include "frame_timing.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"
#include "spaceflight_internal.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace game {

// ---------------------------------------------------------------------------
// Player ship movement (free flight)
// ---------------------------------------------------------------------------
// GHIDRA 0x0044aa70 Ship_HandlePlayerShipCore drives the player ship; the
// actual per-frame integration lives in 0x00433050 Ship_HandleShip with the
// polar helpers Math_AddPolarVelocity (0x0043b4a0) and
// Math_AddPolarVelocityWithClamp (0x0043b4e0). Reconstructs that movement
// model for the player instead of the old fixed-constant stand-in:
//
//  * Stats come from the ship class (ShipClassDef). The scenario loader scales
//    the raw resource shorts exactly as the original (NovaData_LoadScenario-
//    ResourceTables 0x004bd3c0):
//        accel    -> thrust (px/tick^2):   raw_accel / 10000.0 (DAT_00575e68)
//                                                 then *2.0 runtime
//                                                 (DAT_005757a8)
//        speed    -> top speed (px/tick):  raw_speed / 100.0 (DAT_00575e48)
//        maneuver -> turn rate (deg/tick): raw_maneuver * 0.1 (DAT_00575e58)
//    Starter (sh.x9an 0x80): accel 0.05, turn 4.0 deg/tick, top speed 4
//    px/tick. The original scales these values by its frame-time-derived tick
//    count.
//
//  * Turn follows the held key at round(effective turn rate) integer degrees
//    per tick. Outfit opcode-9 is folded into the effective stats before this
//    integrator; the original's ionization damping awaits reconstruction of
//    that combat state. Heading 0 points 'up', increases clockwise.
//
//  * Thrust accelerates along the heading as a polar velocity step clamped
//    per-AXIS to the projection of the class top speed (Math_AddPolarVelocity-
//    WithClamp 0x0043b4e0). The clamp only caps each axis's additional thrust
//    at its polar max projection; it is NOT a vector-magnitude governor, so a
//    ship turning at full thrust can build a small off-axis component that
//    pushes its net speed modestly past the nominal top speed (the authentic
//    EVN drift). Separately, the final velocity vector is then hard-capped
//    each frame to +/-the effective max-speed component (DAT_005997bc/c0 in
//    Ship_HandlePlayerShipControl 0x0044e019), pulling back any excess built
//    up off-axis, so the cap bounds the net speed. The original's throttle is
//    high enough to top out within a few frames.
//
//  * Inertia: most ships preserve momentum when throttle is released -- there
//    is NO continuous velocity drag in the original's free-flight path, so a
//    released ship keeps most of its momentum. The old stand-in's per-frame
//    kDrag multiply was the main fidelity bug. Inertia-less ships are the
//    special case where base_accel == 0 && base_speed == 0: Ship_HandleShip
//    (0x00433050) zeroes their velocity every frame, pinning them in place.
//
//  * REVERSE ('s'/down) turns the ship toward the heading opposite its current
//    velocity, then continues to coast; it does not apply retro-thrust. This is
//    Ship_HandlePlayerShipControl's early Ship_TurnShipTowardHeading path.
//
// NOTE(decomp) scale/cadence: the original integrates over g_avg_frame_time_ms
// (0x00735448, Frame_MeasureFrameTiming 0x00432ea0), so ship motion is
// cadence-independent. The reimplementation normalizes measured SDL elapsed
// time to the original's 30 Hz simulation-tick basis.
// Pure tick-scaled movement integration (unit-tested in
// tests/movement_test.cpp).

// One per-axis step of Ghidra Math_AddPolarVelocityWithClamp (0x0043b4e0), the
// original's single forward-thrust pathway (Ship_HandleShip calls it with
// `ai_forward_thrust_cmd * frame_time` as the base speed). For each velocity
// axis it combines the polar projection of the class top speed (max_proj) with
// the polar projection of the per-frame thrust step (delta) and the current
// velocity, exactly as decoded:
//
//   if (delta <= 0 || max_proj <= 0) {
//       if (delta < 0 && max_proj < 0) { if (max_proj < cur) cur += delta; }
//       else                             cur += delta;
//   } else if (cur < max_proj)          cur += delta;
//
// The clamp therefore only CAPS additional thrust on an axis at its polar max
// projection; it never pulls an existing (e.g. drift-built) component back to
// enforce a vector-magnitude limit. Heading uses the polar convention from
// Math_AddPolarVelocity (0x0043b4a0): vel_x += sin(h)*s ; vel_y -= cos(h)*s.
// Declared in spaceflight.hpp so both the player and NPC integrators share one
// faithful port.
void Math_AddPolarVelocityWithClamp(float heading_rad,
                                    float thrust_step,
                                    float max_speed,
                                    float &vel_x,
                                    float &vel_y) {
  const float sin_h = std::sin(heading_rad);
  const float cos_h = std::cos(heading_rad);
  auto axis_step = [](float max_proj, float delta, float cur) -> float {
    if (delta <= 0.0F || max_proj <= 0.0F) {
      if (delta < 0.0F && max_proj < 0.0F) {
        if (max_proj < cur) {
          cur += delta;
        }
      } else {
        cur += delta;
      }
    } else if (cur < max_proj) {
      cur += delta;
    }
    return cur;
  };
  vel_x = axis_step(sin_h * max_speed, sin_h * thrust_step, vel_x);
  vel_y = axis_step(-cos_h * max_speed, -cos_h * thrust_step, vel_y);
}

[[nodiscard]] PlayerMovementStats
NovaPlayer_IntegrateMovement(PlayerShip &ship,
                             const FlightInput &input,
                             const ShipClass &ship_class,
                             float elapsed_ticks,
                             const PlayerMovementOptions &opts) {
  constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
  constexpr float kTwoPi = 6.283185307179586F;

  PlayerMovementStats stats;
  elapsed_ticks = std::max(0.0F, elapsed_ticks);
  // Ship_HandlePlayerShipControl rounds Ship_ComputeShipMaxTurnRateDeg before
  // multiplying by frame time. Our caller supplies the effective raw
  // maneuver (base + opcode-9 bonuses), so preserve that integer gate here.
  stats.turn_rate_deg_per_tick =
      std::round(static_cast<float>(ship_class.turn_rate) * 0.1F);
  // Loader-verified scales (NovaData_LoadScenarioResourceTables 0x004bd3c0):
  //   accel (offset 0x04) -> base_accel via /DAT_00575e68=10000.0;
  //   speed (offset 0x06) -> base_speed  via /DAT_00575e48=100.0   (NOT 640);
  //   turn  (offset 0x08) -> base_turn_rate_deg via *DAT_00575e58=0.1.
  stats.max_speed_px_per_tick = static_cast<float>(ship_class.speed) / 100.0F;
  // Ship_ComputeShipEffectiveThrust (0x004640a0) multiplies the loaded accel
  // by DAT_005757a8 = 2.0 before the player applies it as the thrust step, so
  // the effective thrust is 2*accel/10000 (the x2 is applied here).
  stats.thrust_px_per_tick2 =
      static_cast<float>(ship_class.accel) / 10000.0F * 2.0F;
  const float turn_rad =
      stats.turn_rate_deg_per_tick * kDegToRad * elapsed_ticks;

  ship.engine_thrust = input.thrust && !input.reverse;

  // Reverse uses the original's automatic turn-toward-velocity path instead
  // of also applying manual steering in the same tick. The face-target arm
  // likewise suppresses keyboard steering (the parent's local_265 latch gates
  // PlayerTick_TurnInput); when reverse and face-target are held together the
  // original's block ordering is unresolved (TODO(decomp)) and reverse wins
  // here.
  if (!input.reverse && !opts.face_target_armed) {
    if (input.turn_left && !input.turn_right) {
      ship.heading -= turn_rad;
      stats.turn_dir = -1;
    }
    if (input.turn_right && !input.turn_left) {
      ship.heading += turn_rad;
      stats.turn_dir = 1;
    }
    ship.heading = std::fmod(ship.heading + kTwoPi, kTwoPi);
    if (ship.heading < 0.0F) {
      ship.heading += kTwoPi;
    }
  }

  if (input.reverse) {
    // Ghidra 0x0044e019: the reverse command finds the current velocity's
    // bearing, adds 180 degrees, and calls Ship_TurnShipTowardHeading. It
    // turns the hull around while preserving its velocity; it is not braking.
    const float speed = std::hypot(ship.vel_x, ship.vel_y);
    if (speed > 1e-4F) {
      const float reverse_heading =
          std::atan2(ship.vel_x, -ship.vel_y) + 3.14159265358979323846F;
      const float desired = std::fmod(reverse_heading + kTwoPi, kTwoPi);
      float delta = std::remainder(desired - ship.heading, kTwoPi);
      stats.turn_dir = delta > 0.0F ? 1 : -1;
      delta = std::clamp(delta, -turn_rad, turn_rad);
      ship.heading = std::fmod(ship.heading + delta + kTwoPi, kTwoPi);
    }
  } else if (opts.face_target_armed) {
    // Ghidra 0x0044aa70 manual-flight auto-turn continuation: with the
    // face-target arm latched, one turn step per frame toward the stored
    // integer heading, stopping (without snapping) once the shortest delta is
    // within a single step -- the original's |delta| <= step exit rejoins the
    // keyboard path for the frame (with sVar8 = 0, i.e. no bank this frame).
    const float desired_rad =
        static_cast<float>(ship.ai_desired_heading_deg) * kDegToRad;
    const float delta = std::remainder(desired_rad - ship.heading, kTwoPi);
    if (std::abs(delta) > turn_rad) {
      ship.heading = std::fmod(
          ship.heading + std::copysign(turn_rad, delta) + kTwoPi, kTwoPi);
      stats.turn_dir = delta > 0.0F ? 1 : -1;
    }
  } else if (input.thrust) {
    if (opts.inertialess) {
      // Ghidra 0x0044c9ab thrust arm, inertialess variant: thrust
      // accumulates the scalar speed (+0x48), clamped to the effective max
      // speed; the steering block below converts it into velocity.
      ship.speed =
          std::min(ship.speed + stats.thrust_px_per_tick2 * elapsed_ticks,
                   stats.max_speed_px_per_tick);
    } else {
      // Forward thrust: polar step toward the heading, per-axis clamped to
      // the class top speed projection (Math_AddPolarVelocityWithClamp
      // semantics; the original clamps to effective max speed here and to
      // max * 1.8 while the afterburner runs -- see the cap tail in
      // PlayerTick_ManualFlightAndRegeneration for the overspeed mechanics).
      Math_AddPolarVelocityWithClamp(ship.heading,
                                     stats.thrust_px_per_tick2 * elapsed_ticks,
                                     stats.max_speed_px_per_tick,
                                     ship.vel_x,
                                     ship.vel_y);
    }
  }

  // Inertia-less ships (base_accel == 0 && base_speed == 0) are stationary: the
  // original Ship_HandleShip (0x00433050) zeroes their velocity every frame, so
  // they act as pinned/immovable objects rather than coasting forever.
  if (ship_class.accel == 0.0F && ship_class.speed == 0.0F) {
    ship.vel_x = 0.0F;
    ship.vel_y = 0.0F;
    ship.speed = 0.0F;
    return stats;
  }

  if (opts.inertialess) {
    // Ghidra 0x0044cffe inertialess steering block
    // (PlayerTick_InertialessSteering). The scalar speed is clamped to the
    // cap global, decays by 33/34 (DAT_005755e0, a double) while
    // fire-restricted, and the velocity rotates toward heading * speed at the
    // thrust-scaled rate. The block's engine-glow ramp toward round(speed * 32
    // * 0.75 / max) capped 24 is owned by the port's glow drive (TODO(decomp)).
    const float scalar_cap = opts.speed_cap_x >= 0.0F
                                 ? opts.speed_cap_x
                                 : stats.max_speed_px_per_tick;
    if (ship.speed > scalar_cap) {
      ship.speed = scalar_cap;
    }
    if (opts.fire_restricted) {
      constexpr float kInertialessFireRestrictedSpeedDamp = 33.0F / 34.0F;
      ship.speed *= kInertialessFireRestrictedSpeedDamp;
    }
    NovaShip_SteerVelocityTowardShipHeading(
        ship, stats.thrust_px_per_tick2, elapsed_ticks);
  }

  // Per-axis velocity cap. Beyond the per-axis clamp applied *during* thrust
  // (Math_AddPolarVelocityWithClamp), the original player path
  // (velocity-cap block 0x0044d05b) clamps the resulting velocity vector to
  // +/-g_player_speed_cap_x/y every frame before integrating position. The
  // caps are always >= the effective max speed (see the afterburner tail in
  // PlayerTick_ManualFlightAndRegeneration); bare integrator calls fall back to
  // it.
  const float cap_x =
      opts.speed_cap_x >= 0.0F ? opts.speed_cap_x : stats.max_speed_px_per_tick;
  const float cap_y =
      opts.speed_cap_y >= 0.0F ? opts.speed_cap_y : stats.max_speed_px_per_tick;
  ship.vel_x = std::clamp(ship.vel_x, -cap_x, cap_x);
  ship.vel_y = std::clamp(ship.vel_y, -cap_y, cap_y);

  ship.pos_x += ship.vel_x * elapsed_ticks;
  ship.pos_y += ship.vel_y * elapsed_ticks;
  // Inertialess ships keep the scalar speed (+0x48) the thrust/steering
  // blocks maintained; the velocity is derived from it, not the reverse.
  if (!opts.inertialess) {
    ship.speed = std::sqrt(ship.vel_x * ship.vel_x + ship.vel_y * ship.vel_y);
  }
  return stats;
}

// -------------------------------------------------------------------------
// NPC ship movement
// -------------------------------------------------------------------------

// Ship_GetIonizationIntensity (0x0046c160): the NPC path has no outfit
// opcode-0x28 additions, so its normalized intensity is simply the status
// ionization meter divided by the class capacity. The original returns zero for
// a non-positive capacity and clamps the resulting stat multiplier below.
[[nodiscard]] float
spaceflight_detail::NovaShip_IonizationIntensity(const Ship &ship,
                                                 const ShipClass &ship_class) {
  if (ship.ionization_points <= 0.0F || ship_class.ionization_capacity <= 0) {
    return 0.0F;
  }
  return ship.ionization_points /
         static_cast<float>(ship_class.ionization_capacity);
}

// Ghidra 0x00463e70 Ship_ComputeShipMaxTurnRateDeg (NPC branch; the player
// path applies the same base*0.1 + opcode-9/rule inside NovaPlayer_Integrate-
// Movement).
NpcEffectiveStats NovaShip_ComputeEffectiveStats(const GameState &state,
                                                 const Ship &ship,
                                                 const ShipClass &ship_class) {
  NpcEffectiveStats stats;
  stats.turn_rate_deg_per_tick =
      static_cast<float>(ship_class.turn_rate) * 0.1F;
  stats.max_speed_px_per_tick = static_cast<float>(ship_class.speed) / 100.0F;
  stats.thrust_px_per_tick2 =
      static_cast<float>(ship_class.accel) / 10000.0F * 2.0F;
  // Ship_ComputeShipEffectiveThrust (0x004640a0) and
  // Ship_ComputeShipEffectiveMaxSpeed (0x004642e0) return zero for the NPC
  // capability flag 0x400 before applying any other modifier. The turn-rate
  // helper (0x00463e70) has no such check (disasm-verified), so only speed and
  // thrust are zeroed here; the turn rate still runs the full NPC branch below.
  if ((ship_class.capability_flags & 0x0400U) != 0U) {
    stats.max_speed_px_per_tick = 0.0F;
    stats.thrust_px_per_tick2 = 0.0F;
  }

  // Government SkillMult applies to thrust and max speed when the ship belongs
  // to a government. Turn rate is not government-scaled. The skill-variance
  // scale precedes the government scale in the original (0x004640a0 /
  // 0x004642e0 multiply the GovtDef 0x64 SkillMult field).
  stats.max_speed_px_per_tick *= ship.skill_variance_scale;
  stats.thrust_px_per_tick2 *= ship.skill_variance_scale;
  if (ship.faction_or_government_id >= 0) {
    const Government *g = state.scenario.Government(
        static_cast<std::int16_t>(ship.faction_or_government_id + 0x80));
    if (g != nullptr) {
      stats.max_speed_px_per_tick *= g->skill_mult;
      stats.thrust_px_per_tick2 *= g->skill_mult;
    }
  }

  // DAT_00575788 = 1/3. A non-self velocity-match lock damps all three
  // movement stats, including turn rate; a self-lock is deliberately neutral.
  const bool velocity_matched =
      ship.velocity_match_target_ship_slot != -1 &&
      ship.velocity_match_target_ship_slot != ship.ship_instance_id;
  if (velocity_matched) {
    constexpr float kVelocityMatchScale = 1.0F / 3.0F;
    stats.max_speed_px_per_tick *= kVelocityMatchScale;
    stats.thrust_px_per_tick2 *= kVelocityMatchScale;
    stats.turn_rate_deg_per_tick *= kVelocityMatchScale;
  }

  // Mission ships use DAT_005757a8 = 2.0 for thrust, speed and the special
  // low-turn-rate correction. The latter is applied below with the same
  // ordering as Ship_ComputeShipMaxTurnRateDeg.
  if (ship.pers_def_slot == 0x03ff) {
    stats.max_speed_px_per_tick *= 2.0F;
    stats.thrust_px_per_tick2 *= 2.0F;
  }

  // Ship_ComputeShipMaxTurnRateDeg applies the mission correction before the
  // general one-degree floor, and ionization damping only while the ship is not
  // thrusting. DAT_00575790 is 6.0 and DAT_00575784 is 1.0.
  if (ship.pers_def_slot == 0x03ff && stats.turn_rate_deg_per_tick < 6.0F) {
    stats.turn_rate_deg_per_tick += 1.0F;
  }
  if (stats.turn_rate_deg_per_tick >= 1.0F) {
    stats.turn_rate_deg_per_tick = std::max(stats.turn_rate_deg_per_tick, 1.0F);
  }
  const float intensity = std::min(
      0.7F, spaceflight_detail::NovaShip_IonizationIntensity(ship, ship_class));
  if (intensity > 0.0F) {
    // Effective thrust always carries the ionization multiplier. The turn
    // helper applies the same damping only in its non-thrusting branch; max
    // speed has no ionization term in the original helper.
    stats.thrust_px_per_tick2 *= (1.0F - intensity);
    if (ship.ai_forward_thrust_cmd <= 0.0F) {
      stats.turn_rate_deg_per_tick *= (1.0F - intensity);
    }
  }
  return stats;
}

// Ports the movement block of Ghidra Ship_HandleShip (0x00433050) for the
// AI-driven NPC ships. The AI layer (Ship_UpdateShipAiState / the behavior
// supervisors) writes ai_desired_heading_deg (degrees, 0 = up, clockwise),
// ai_desired_speed (scalar speed along the heading) and ai_forward_thrust_cmd;
// here the integrator turns the hull toward the desired heading at the class
// turn rate and applies thrust / position integration.
//
// The AI turn is CONTINUOUS (Ship_HandleShip uses the raw
// Ship_ComputeShipMaxTurnRateDeg rate) unlike the player keyboard path which
// rounds to integer degrees/frame before integrating. Thrust follows the
// original three-branch model keyed on ai_desired_speed; ai_forward_thrust_cmd
// carries the RAW effective thrust value (NovaAi_ApplyControls writes
// Ship_ComputeShipEffectiveThrust, NOT 1.0 -- writing 1.0 would make NPCs
// accelerate ~50x too fast):
//   desired == 0 : free-coast (the thrust command is applied as a per-axis
//                  clamped step toward the max-speed projection; gated on
//                  ai_station_hold_timer <= 0).
//   desired >  0 : forward thrust toward `desired` speed, per-axis clamped to
//                  the polar projection of `desired` (Math_AddPolarVelocity-
//                  WithClamp 0x0043b4e0).
//   desired <  0 : physics override; set velocity to heading * abs(desired)
//                  instead of integrating thrust, then decay desired toward
//                  zero by abs(ai_forward_thrust_cmd).
// ai_maneuver_timer_ms is a coast-through-reversal TIMER (not a brake): while
// >0 it suppresses both turning and thrust (the ship holds heading and coasts);
// it counts down by normalized elapsed ticks each frame and is re-set to a
// random 30..59 ticks when the AI decides to reverse into open space.
//
// Stats come from NovaShip_ComputeEffectiveStats (Ship_ComputeShipEffective-
// Thrust / EffectiveMaxSpeed NPC branch): turn = raw_maneuver*0.1 deg/tick,
// max speed = raw_speed/100 px/tick * government SkillMult,
// thrust = raw_accel/10000*2 px/tick^2 * government SkillMult. NPC
// ships carry no outfit inventory; ionization capacity comes from the class.
// TODO(decomp): opcode 7/8/9 outfit bonuses, the per-ship skill_variance_scale
// (+0x40) factor and disable/ionization damping when the NPC outfit/combat
// state is reconstructed.
//
// Inertialess ships (ShipClassDef.flags_secondary bit 0x40) keep a scalar
// `speed` (integrated in the thrust block below) and steer their velocity
// through NovaShip_SteerVelocityTowardShipHeading (0x0043b020) in the position
// block, matching the original's two-regime movement model.
void NovaShip_IntegrateNpcMovement(GameState &state,
                                   Ship &ship,
                                   const ShipClass &ship_class,
                                   float elapsed_ticks,
                                   std::uint32_t now_ms) {
  constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
  constexpr float kTwoPi = 6.283185307179586F;
  constexpr float kFullCircleDeg = 360.0F;
  elapsed_ticks = std::max(0.0F, elapsed_ticks);

  // Derived effective stats (see header/block comment above).
  // The turn-rate floor mirrors Ship_ComputeShipMaxTurnRateDeg's NPC branch:
  // the returned rate is clamped up to a minimum floor (gh.data _DAT_00575784
  // = 1.0 deg/frame) whenever the base class rate itself is already at/above
  // that floor (so genuinely sluggish classes keep their low rate). For a clean
  // NPC the computed rate equals the base, so this floor is currently inert; it
  // becomes active only once ionization / disable damping lowers the rate
  // below its base (TODO(decomp)). Kept to match the original's NPC branch.
  const NpcEffectiveStats stats =
      NovaShip_ComputeEffectiveStats(state, ship, ship_class);
  const float base_turn_deg = stats.turn_rate_deg_per_tick;
  float eff_turn_deg = base_turn_deg;
  if (base_turn_deg >= 1.0F) {
    eff_turn_deg = std::max(eff_turn_deg, 1.0F);
  }
  const float eff_max_speed = stats.max_speed_px_per_tick;
  const float eff_thrust = stats.thrust_px_per_tick2;
  // Inertialess ships (ShipClassDef.flags_secondary bit 0x40, and not in
  // ai_control_mode 0x0c) keep a scalar Ship.speed and steer their velocity
  // through Ship_SteerVelocityTowardShipHeading instead of vector thrust.
  const bool inertialess = NovaShip_IsInertialess(ship, ship_class);

  // The movement blocks are gated off only while the maneuver timer is active
  // AND the ship is not in AI state 0x16. Ghidra Ship_HandleShip 0x00433050
  // opens the turn/thrust/bank block when
  // `ai_maneuver_timer_ms <= 0 || Ship_IsShipInAiState0x16(ship)` (the
  // class-0x2ff sentinel arm is handled by the caller's acceptance guard), so
  // a yielding (0x16) ship keeps steering even with an active timer.
  const bool coasting = ship.ai_maneuver_timer_ms > 0.0F;
  // Ghidra 0x00416070 Ship_IsShipInAiState0x16 runs inline here.
  const bool holds_course = coasting && ship.ai_state_code != 0x16;
  const bool fire_restricted = NovaAiShip_IsDisabled(state, ship);

  if (ship.arrival_monitor_active) {
    ship.arrival_monitor_elapsed_ticks += elapsed_ticks;
    const float velocity = std::hypot(ship.vel_x, ship.vel_y);
    const float settled_speed = std::max(10.0F, eff_max_speed + 1.0F);
    const bool lost_slowdown_early =
        ship.arrival_monitor_elapsed_ticks > 2.0F && ship.ai_state_code != 8 &&
        velocity > settled_speed;
    const bool remained_fast_too_long =
        ship.arrival_monitor_elapsed_ticks > 60.0F && velocity > settled_speed;
    if (!ship.arrival_monitor_warning_logged &&
        (lost_slowdown_early || remained_fast_too_long)) {
      NovaLog::Warn(
          "NPC arrival anomaly: slot={} class={} behavior={} state={} "
          "control={} velocity={:.2f} desired={:.2f} thrust={:.3f} "
          "station_hold={:.2f} maneuver={:.2f} age={:.2f} disabled={}",
          ship.ship_instance_id,
          ship.ship_class_id,
          ship.ai_behavior_code,
          ship.ai_state_code,
          ship.ai_control_mode,
          velocity,
          ship.ai_desired_speed,
          ship.ai_forward_thrust_cmd,
          ship.ai_station_hold_timer,
          ship.ai_maneuver_timer_ms,
          ship.arrival_monitor_elapsed_ticks,
          fire_restricted);
      ship.arrival_monitor_warning_logged = true;
    }
    if (velocity <= settled_speed) {
      ship.arrival_monitor_active = false;
    }
  }

  // Ship_HandleShip (0x00433050) applies the disabled/derelict damping before
  // integrating position. g_fire_restricted_ship_velocity_damp (0x00575448) is
  // the double 0.995 (NOT the 0.94 mode-1 brake damp at 0x5750f8): a disabled
  // ship keeps almost all of its velocity and drifts to a stop slowly. The
  // original applies it once per raw spaceflight call; exponentiate by the
  // time-adjusted raw-call count so the stop is frame-rate independent. It
  // also covers the inertialess scalar speed.
  if (fire_restricted) {
    constexpr float kFireRestrictedVelocityDamp = 0.995F; // 0x00575448
    const float damp = std::pow(kFireRestrictedVelocityDamp,
                                RawSpaceflightCallTicks(elapsed_ticks));
    ship.vel_x *= damp;
    ship.vel_y *= damp;
    ship.speed *= damp;
  }

  // --- Position integration + inertia-less special case. ---
  // Ship_HandleShip integrates the velocity inherited from the previous frame
  // before calculating this frame's steering/thrust. This ordering matters for
  // arrivals: the first state-8 frame installs the emergence velocity but does
  // not move the ship away from the gate until the following frame.
  if (ship_class.accel == 0.0F && ship_class.speed == 0.0F) {
    ship.vel_x = 0.0F;
    ship.vel_y = 0.0F;
    ship.speed = 0.0F;
  } else {
    if (inertialess) {
      NovaShip_SteerVelocityTowardShipHeading(ship, eff_thrust, elapsed_ticks);
    }
    ship.pos_x += ship.vel_x * elapsed_ticks;
    ship.pos_y += ship.vel_y * elapsed_ticks;
    if (!inertialess) {
      ship.speed = std::sqrt(ship.vel_x * ship.vel_x + ship.vel_y * ship.vel_y);
    }
  }

  // --- Turn toward the desired heading (continuous AI turn rate). ---
  // The original computes the shortest signed angular delta in degrees from
  // the current heading to = ai_desired_heading_deg, stepping it (in either
  // direction) by eff_max_turn_deg each frame, snapping to the exact heading
  // once within one step. The snap-once branch writes the desired heading
  // directly; otherwise the ship rotates by a full turn step in the direction
  // that closes the angle fastest. ai_turn_bias_dir mirrors the original's
  // turn-bank signal (Ship_HandleShip writes +0xc8f8 from the
  // turn_bank_animation_phase field at +0xc8e4
  // tilt anim: +1 while banking one way, -1 the other, 0 otherwise), which the
  // engine-glow block below consumes for its turn-bias +2 bump.
  ship.ai_turn_bias_dir = 0;
  // Ghidra gates the whole turn/regen block on `!Ship_IsShipDisabled(ship) &&
  // !g_gameplay_time_frozen` (uVar10, 0x00433050): a disabled NPC holds its
  // current heading (no rotation) and does not regenerate. The inner arm then
  // additionally needs `ai_maneuver_timer_ms <= 0 || Ship_IsShipInAiState0x16`.
  if (!fire_restricted && !holds_course) {
    const float cur_deg = ship.heading / kDegToRad;
    // Shortest signed delta [-180, 180] degrees from current to desired.
    const float delta_deg = std::remainder(
        static_cast<float>(ship.ai_desired_heading_deg) - cur_deg,
        kFullCircleDeg);
    const float max_turn_deg = eff_turn_deg * elapsed_ticks;
    if (std::abs(delta_deg) <= max_turn_deg) {
      ship.heading =
          static_cast<float>(ship.ai_desired_heading_deg) * kDegToRad;
    } else {
      ship.heading =
          (cur_deg + std::copysign(max_turn_deg, delta_deg)) * kDegToRad;
      ship.ai_turn_bias_dir = delta_deg > 0.0F ? 1 : -1;
    }
    ship.heading = std::fmod(ship.heading, kFullCircleDeg / kDegToRad);
    if (ship.heading < 0.0F) {
      ship.heading += kTwoPi;
    }

    // Regeneration shares the block's disabled gate; lethal hits must not
    // resurrect a ship whose armor has reached zero.
    if (!NovaAiShip_IsDestroyed(ship)) {
      const float max_shield = static_cast<float>(ship_class.base_shield);
      if (ship.shield_points < max_shield) {
        ship.shield_points = std::min(
            max_shield,
            ship.shield_points +
                static_cast<float>(ship_class.shield_recharge) * elapsed_ticks);
      }
      const float max_armor = static_cast<float>(ship_class.base_armor);
      if (ship.armor_points < max_armor) {
        ship.armor_points = std::min(
            max_armor,
            ship.armor_points +
                static_cast<float>(ship_class.armor_recharge) * elapsed_ticks);
      }
    }
  }

  // --- Forward / reverse thrust along the heading. ---
  // When the ship is coasting (ai_maneuver_timer_ms > 0) or defunct it skips
  // thrust entirely and holds velocity. Otherwise, when a forward-thrust
  // command is present, apply the three-branch ai_desired_speed model (the
  // original gates this whole block on ai_forward_thrust_cmd != 0, so a
  // stopped ship with no thrust command drifts without applying any new
  // velocity). The coast case (desired == 0) is a clamped step toward the
  // class top speed; desired > 0 throttles toward it; desired < 0 is the
  // physics-override path. Non-inertialess ships apply heading-aligned
  // thrust; inertialess ships accumulate a scalar `speed` clamped at the
  // same caps.
  if (!holds_course && ship.ai_forward_thrust_cmd != 0.0F) {
    const float desired = ship.ai_desired_speed;
    auto add_polar = [&](float speed) {
      // Math_AddPolarVelocity (0x0043b4a0): vel_x += sin(h)*s ; vel_y -=
      // cos(h)*s (heading 0 = up / -y, increasing clockwise).
      ship.vel_x += std::sin(ship.heading) * speed;
      ship.vel_y -= std::cos(ship.heading) * speed;
    };
    auto add_polar_clamped = [&](float thrust_step, float cap) {
      // Math_AddPolarVelocityWithClamp (0x0043b4e0), via the shared
      // player/NPC helper -- Math_AddPolarVelocityWithClamp clamp semantics.
      Math_AddPolarVelocityWithClamp(
          ship.heading, thrust_step, cap, ship.vel_x, ship.vel_y);
    };
    const float thrust_step = ship.ai_forward_thrust_cmd * elapsed_ticks;

    if (desired == 0.0F) {
      // Coast branch. Non-inertialess: per-axis clamped step toward the class
      // top speed (with a zero command this is a no-op). Inertialess: scalar
      // `speed` clamped at the class top speed. The original gates this branch
      // on ai_station_hold_timer <= 0 (a ship parked at a hold point does not
      // coast-accelerate).
      if (ship.ai_station_hold_timer <= 0.0F) {
        if (!inertialess) {
          add_polar_clamped(thrust_step, eff_max_speed);
        } else {
          ship.speed =
              std::clamp(ship.speed + thrust_step, 0.0F, eff_max_speed);
        }
      }
    } else if (desired > 0.0F) {
      // Forward thrust toward the requested speed.
      if (!inertialess) {
        add_polar_clamped(thrust_step, desired);
      } else {
        ship.speed = std::clamp(ship.speed + thrust_step, 0.0F, desired);
      }
    } else {
      // Physics override: replace the velocity with abs(desired) along the
      // current heading. Negative throttle is not reverse acceleration; its
      // magnitude decays the signed desired value toward zero below.
      if (!inertialess) {
        ship.vel_x = 0.0F;
        ship.vel_y = 0.0F;
        add_polar(std::abs(desired));
      } else {
        ship.speed = std::abs(desired);
      }
      // NOTE(decomp) deliberate divergence: the original advances this decay
      // once per 21 ms outer call without g_avg_frame_tick_scale. The fixed
      // 30 Hz simulation scheduler supplies the normalized equivalent while
      // keeping the clean-room result independent of presentation FPS.
      ship.ai_desired_speed += std::abs(ship.ai_forward_thrust_cmd) *
                               elapsed_ticks / kOriginalMaxRateFrameTicks;
      // Ship_HandleShip (0x00433050) hands the ship to
      // Ship_ResetShipPrimaryAndSecondaryTargets once the desired speed has
      // risen above the negative effective max speed. The threshold uses
      // min(self, lead target) when a valid lead (0..0x3f) is followed
      // (0x00433b26-0x00433b8e). State 8 begins at -50 and advances ~1.165
      // per movement tick before the reset returns the ship to idle and arms
      // the 30..59-tick coast-through timer (armed once: the reset clears the
      // desired speed and thrust command, so the branch is not re-entered).
      float threshold_max_speed = eff_max_speed;
      if (ship.squad_leader_ship_slot >= 0 &&
          ship.squad_leader_ship_slot <= 0x3f &&
          state.SlotInRange(
              static_cast<std::size_t>(ship.squad_leader_ship_slot))) {
        const Ship &lead =
            state.ShipAt(static_cast<std::size_t>(ship.squad_leader_ship_slot));
        const ShipClass *lead_class = state.scenario.Ship(
            static_cast<std::int16_t>(lead.ship_class_id + 0x80));
        if (lead_class != nullptr) {
          const NpcEffectiveStats lead_stats =
              NovaShip_ComputeEffectiveStats(state, lead, *lead_class);
          threshold_max_speed =
              std::min(threshold_max_speed, lead_stats.max_speed_px_per_tick);
        }
      }
      if (ship.ai_desired_speed >= -threshold_max_speed) {
        NovaAi_ResetShipPrimaryAndSecondaryTargets(ship);
        if (inertialess) {
          // Inertialess completion quirk (0x00434f7e): the original zeroes
          // the vector velocity and re-commands the scalar override at the
          // threshold max speed instead of keeping the glide velocity.
          ship.ai_desired_speed = threshold_max_speed;
          ship.vel_x = 0.0F;
          ship.vel_y = 0.0F;
        }
        if (ship.squad_leader_ship_slot == -1 && ship.pers_def_slot != 0x3ff) {
          std::uniform_int_distribution<std::int32_t> dist(30, 59);
          ship.ai_maneuver_timer_ms = static_cast<float>(dist(state.rng));
        }
      }
    }
  }

  // Ghidra Ship_HandleShip (0x00433050), jump-spin-up departure block:
  // Ship_ApplyShipAiControls arms ai_station_hold_timer in control mode 4,
  // but the visible departure movement is applied here. The original first
  // damps the stopped ship, then advances its position along the already-
  // aligned heading with a time-ramped jump speed; this is a position step,
  // not ordinary thrust into vel_x/vel_y.
  const bool jump_spinup_control =
      ship.ai_station_hold_timer > 0.0F &&
      (ship.ai_state_code == 2 || ship.ai_state_code == 3 ||
       ship.ai_state_code == 0xb) &&
      (ship.ai_control_mode == 4 || ship.ai_control_mode == 0xd) &&
      !fire_restricted;
  bool jump_glow_active = false;
  if (jump_spinup_control) {
    constexpr float kJumpVelocityDamp = 0.8F;      // DAT_00575488
    constexpr float kJumpProgressSubtract = 35.0F; // DAT_00575490
    constexpr float kJumpProgressCap = 50.0F;      // DAT_00575388
    constexpr float kJumpDurationScale = 0.01F;    // DOUBLE_00575368
    constexpr float kJumpDurationMs = 350.0F;

    ship.vel_x *= kJumpVelocityDamp;
    ship.vel_y *= kJumpVelocityDamp;

    const float current_heading_deg = ship.heading / kDegToRad;
    const float heading_delta_deg = std::remainder(
        static_cast<float>(ship.ai_desired_heading_deg) - current_heading_deg,
        360.0F);
    const bool aligned = std::abs(heading_delta_deg) <=
                         stats.turn_rate_deg_per_tick * elapsed_ticks;
    if (!aligned) {
      // Ghidra resets the mode-start timestamp while the ship is still
      // turning, so the jump-speed ramp begins only after alignment.
      ship.ai_mode_start_time_ms = now_ms;
    } else {
      // The original uses elapsed wall-clock time multiplied by the ship-class
      // jump_duration_multiplier, divided by duration_ms * 0.01, then subtracts
      // 35. The class multiplier is decoded (ShipClassDef +0x44) but this NPC
      // ramp still uses 1.0; apply it here as a follow-up.
      // TODO(decomp(0x004347e8)) skipped: NPC jump ramp class multiplier.
      const float elapsed_jump_ms =
          static_cast<float>(now_ms - ship.ai_mode_start_time_ms);
      float jump_progress =
          elapsed_jump_ms / (kJumpDurationMs * kJumpDurationScale) -
          kJumpProgressSubtract;
      jump_progress = std::clamp(jump_progress, 0.0F, kJumpProgressCap);
      if (jump_progress > 0.0F) {
        // Ghidra's Math_AddPolarVelocity is passed &ship->pos_x here
        // (0x004347e8): the jump ramp directly moves the position. Keeping
        // this out of the ordinary velocity preserves the original launch
        // cadence and prevents mode 4 from turning the ramp into a slow
        // acceleration curve.
        ship.pos_x += std::sin(ship.heading) * jump_progress * elapsed_ticks;
        ship.pos_y -= std::cos(ship.heading) * jump_progress * elapsed_ticks;
        jump_glow_active = true;
      }
    }
  }

  // --- Coast-through-reversal timer countdown. ---
  // Despite its legacy field name, the original stores normalized simulation
  // ticks here: Frame_MeasureFrameTiming scales elapsed milliseconds by 0.03
  // before publishing g_avg_frame_time_ms.
  if (ship.ai_maneuver_timer_ms > 0.0F) {
    ship.ai_maneuver_timer_ms =
        std::max(0.0F, ship.ai_maneuver_timer_ms - elapsed_ticks);
  }

  // --- Engine glow level (Ghidra ShipState field_0xc8d4). ---
  // Mirrors Ship_HandleShip's glow drive: the level ramps toward 0x20 (32)
  // under a full burn, toward 0x18 (24) under low throttle (thrust command
  // below 2x the effective thrust, gh.data DAT_0057531c = 2.0), fades by one
  // decrement while not thrusting (LAB_00435197) and while coasting through a
  // reversal (the closed main gate and the tail decrement collapse to one),
  // and gets an extra +2 while banking into a turn (ai_turn_bias_dir set and
  // class sprite_behavior_flags bit 2), with no upper clamp. The renderer's
  // per-frame random flicker/hide threshold lives in
  // NovaShip_TickWeaponSpriteAndRunningLights (Ship_UpdateVisualState).
  // These are integer mutations made once per raw Ship_HandleShip call. Bank
  // fractional normalized time and replay the original ordered state machine
  // at the loop's 21 ms maximum-rate cadence. In particular, the jump +3
  // precedes the banking +2 and ordinary thrust/fade branch in each replay.
  ship.engine_glow_raw_tick_accumulator += elapsed_ticks;
  constexpr float kGlowCadenceEpsilon = 1e-6F;
  const int glow_tick_count = static_cast<int>(
      (ship.engine_glow_raw_tick_accumulator + kGlowCadenceEpsilon) /
      kOriginalMaxRateFrameTicks);
  ship.engine_glow_raw_tick_accumulator -=
      static_cast<float>(glow_tick_count) * kOriginalMaxRateFrameTicks;
  ship.engine_glow_raw_tick_accumulator =
      std::max(0.0F, ship.engine_glow_raw_tick_accumulator);
  for (int glow_tick = 0; glow_tick < glow_tick_count; ++glow_tick) {
    std::int16_t &glow = ship.engine_glow_level;
    if (jump_glow_active) {
      glow =
          static_cast<std::int16_t>(std::min(0x20, static_cast<int>(glow) + 3));
    }
    auto fade_to_zero = [&]() { // LAB_00435197: single decrement toward 0.
      if (glow > 0) {
        glow = static_cast<std::int16_t>(glow - 1);
      }
    };
    // Ship_HandleShip only derives the bank signal for classes with the
    // banking flag (bit 0), then applies the glow boost when the engine-glow
    // flag (bit 1) is also present.
    if (ship.ai_turn_bias_dir != 0 &&
        (ship_class.sprite_behavior_flags & 1U) != 0U &&
        (ship_class.sprite_behavior_flags & 2U) != 0U) {
      // Ghidra adds +2 with no upper clamp (0x004350ee), so the level can
      // overshoot 0x18; the normal thrust branch pulls it back one step on
      // later frames.
      if (glow < 0x18) {
        glow = static_cast<std::int16_t>(glow + 2);
      }
    }
    if (ship.ai_forward_thrust_cmd <= 0.0F) {
      fade_to_zero();
    } else if (holds_course) {
      // Thrust command present but the maneuver timer is coasting the ship
      // through a reversal: Ghidra's inner check `timer > 0 && !state0x16`
      // jumps to LAB_00435197, the same single decrement the closed main gate
      // would otherwise take through the tail block.
      fade_to_zero();
    } else if (ship.ai_forward_thrust_cmd < eff_thrust * 2.0F) {
      // Low throttle: settle the glow at the 0x18 cruise level.
      if (glow < 0x18) {
        glow = static_cast<std::int16_t>(glow + 1);
      } else if (glow > 0x18) {
        glow = static_cast<std::int16_t>(glow - 1);
      }
    } else {
      // Full burn: raise toward the 0x20 cap.
      if (glow < 0x20) {
        glow = static_cast<std::int16_t>(glow + 1);
      }
    }
  }
  ship.engine_glow_intensity = std::clamp(
      static_cast<float>(ship.engine_glow_level) / 24.0F, 0.0F, 1.0F);

  // Ship_HandleShip 0x00435464..0x004354ae: player-owned hits build this
  // retarget-pressure accumulator in Ship_ApplyDamageToShip. Its
  // passive decay is normalized simulation time (g_avg_frame_tick_scale),
  // unlike the nearby per-call engine-glow mutations: subtract 0.5 per tick
  // and clamp a crossing to zero.
  if (ship.player_aggro_accumulator > 0.0F) {
    ship.player_aggro_accumulator =
        std::max(0.0F, ship.player_aggro_accumulator - 0.5F * elapsed_ticks);
  }
}

// Port of Ghidra Ship_SteerVelocityTowardShipHeading (0x0043b020). Takes the
// inertialess ship's scalar `speed` + `heading`, builds the commanded
// velocity as heading*speed (Math_AddPolarVelocity), then moves the PREVIOUS
// velocity toward that command by at most eff_thrust * 4.0 * frame_time per
// axis, never overshooting the command. That is a smooth acceleration of the
// current velocity toward the commanded heading*speed vector, not a heading
// snap. The turn-scale is _DAT_005754ac = 4.0f (0x005754ac, disasm-verified).
// eff_thrust and elapsed_ticks stand in for the original's internal
// Ship_ComputeShipEffectiveThrust(ship) call and g_avg_frame_tick_scale read.
void NovaShip_SteerVelocityTowardShipHeading(Ship &ship,
                                             float eff_thrust,
                                             float elapsed_ticks) {
  const float prev_vel_x = ship.vel_x;
  const float prev_vel_y = ship.vel_y;
  // Commanded velocity = forward-heading * speed (Ship.speed is the
  // inertialess scalar), via Math_AddPolarVelocity semantics.
  const float new_vx = std::sin(ship.heading) * ship.speed;
  const float new_vy = -std::cos(ship.heading) * ship.speed;
  // Original disasm: x87 FIST truncation of heading to integer degrees, then
  // Math_AddPolarVelocity; the table lookup is reproduced here as sin/cos of
  // the port's radian heading (see the NPC polar helpers).
  constexpr float kSteerTurnScale = 4.0F; // 0x005754ac
  const float step = eff_thrust * kSteerTurnScale * elapsed_ticks;
  // prev + clamp(new - prev, -step, +step): advance the previous velocity
  // toward the command at most `step` per axis, never crossing it.
  ship.vel_x = prev_vel_x + std::clamp(new_vx - prev_vel_x, -step, step);
  ship.vel_y = prev_vel_y + std::clamp(new_vy - prev_vel_y, -step, step);
}

} // namespace game
