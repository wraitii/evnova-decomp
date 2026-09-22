#include "spaceflight.hpp"

#include "../log.hpp"
#include "compatibility.hpp"
#include "frame_timing.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "landed_store.hpp"
#include "nova_random.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"
#include "spaceflight_internal.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "weapon.hpp"

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
//    each frame to +/-the effective max-speed component
//    (PlayerTick_ClampVelocity ToSpeedCaps 0x0044d05b, internal to
//    Ship_HandlePlayerShipCore 0x0044aa70), pulling back any excess built up
//    off-axis, so the cap bounds the net speed. The original's throttle is high
//    enough to top out within a few frames.
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
//    the PlayerTick_ReverseCommand arm (0x0044fff0), which arms the shared
//    auto-turn continuation with the opposite-velocity heading.
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

  // Every input-driven control arm below (keyboard turn, reverse, thrust and
  // the caller's afterburner) is gated on !Ship_IsShipDisabled(player) in the
  // original (Ship_HandlePlayerShipCore 0x0044aa70: the parent's local_251
  // latch). The auto-turn continuation (face-target / reverse's desired
  // heading) is NOT gated -- it lives in the else branch of the local_265
  // test -- so a disabled hull can still be swung around by the face-target
  // command, but holding a turn or thrust key does nothing.
  const bool input_enabled = !opts.fire_restricted;

  ship.engine_thrust = input_enabled && input.thrust;

  // Reverse uses the original's automatic turn-toward-velocity path instead
  // of also applying manual steering in the same tick. The face-target arm
  // likewise suppresses keyboard steering (the parent's local_265 latch gates
  // PlayerTick_TurnInput); when reverse and face-target are held together the
  // original's block ordering is unresolved (TODO(decomp)) and reverse wins
  // here.
  if (input_enabled && !input.reverse && !opts.face_target_armed) {
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

  if (input_enabled && input.reverse) {
    // Ghidra 0x0044fff0 PlayerTick_ReverseCommand (binding slot 0x16 / Down):
    // the reverse command has two arms, split by Outfit_ShipIsInertialess
    // (0x0044ffa8). It never applies forward thrust and never brakes via the
    // normal velocity path.
    if (opts.inertialess) {
      // Inertialess arm (Ghidra 0x0044ffa8): retro-decay the maintained scalar
      // speed (+0x48) by the effective thrust step, flooring at zero. The
      // inertialess steering block below then rotates the velocity toward
      // heading * the reduced speed; there is no turn and no turn_dir.
      ship.speed = std::max(
          0.0F, ship.speed - stats.thrust_px_per_tick2 * elapsed_ticks);
    } else if (std::abs(ship.vel_x) >= 0.05F || std::abs(ship.vel_y) >= 0.05F) {
      // Non-inertialess arm (Ghidra 0x0044fff0): derives the heading opposite
      // the current velocity -- Math_BearingFromPointToPoint(origin,
      // vel*100) + 180 deg -- stores it in ai_desired_heading_deg and latches
      // the manual-flight auto-turn arm. It is not braking and applies no
      // damping. The turn itself is the shared continuation at 0x00450086
      // (the same one the face-target arm uses): one rounded turn step per
      // frame, and no turn_dir once the shortest delta is within a step,
      // where the original leaves sVar8 = 0 (so the bank animation decays
      // instead of holding). DAT_005755b0 (0.05) is the per-axis gate.
      const float reverse_heading =
          std::atan2(ship.vel_x, -ship.vel_y) + 3.14159265358979323846F;
      const float desired = std::fmod(reverse_heading + kTwoPi, kTwoPi);
      const float delta = std::remainder(desired - ship.heading, kTwoPi);
      if (std::abs(delta) > turn_rad) {
        ship.heading = std::fmod(
            ship.heading + std::copysign(turn_rad, delta) + kTwoPi, kTwoPi);
        stats.turn_dir = delta > 0.0F ? 1 : -1;
      }
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
  }

  // Forward thrust is its own block in the original (Ghidra 0x004502f1, the
  // reordered thrust block of the joined afterburner/thrust/glow region
  // 0x0044C9AB -> 0x0044CA6B; g_nova_control_bits[0x44], binding slot 0x15): it
  // runs regardless of the reverse/face-target arm selected for the heading
  // above. Chaining it to those auto-turn branches silently dropped the
  // acceleration whenever the face-target key (A) was held -- the engine glow
  // showed (engine_thrust) but the hull never moved.
  if (input_enabled && input.thrust) {
    if (opts.inertialess) {
      // Ghidra 0x004502f1 thrust arm, inertialess variant: thrust
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
    // cap global, decays by the g_inertialess_fire_restricted_speed_damp
    // double while fire-restricted, and the velocity rotates toward
    // heading * speed at the thrust-scaled rate. The original disasm
    // (0x0044d02b, FMUL double ptr [0x005755e0]) applies the decay as a single
    // multiply per call with no tick scale; this port keeps that original
    // cadence. The block's engine-glow ramp toward round(speed * 32 * 0.75 /
    // max) capped 24 is owned by the port's glow drive (TODO(decomp)).
    const float scalar_cap = opts.speed_cap_x >= 0.0F
                                 ? opts.speed_cap_x
                                 : stats.max_speed_px_per_tick;
    if (ship.speed > scalar_cap) {
      ship.speed = scalar_cap;
    }
    if (opts.fire_restricted) {
      constexpr float kInertialessFireRestrictedSpeedDamp = 0.985F;
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
// ionization meter divided by the class capacity. Zero is returned only for a
// non-positive capacity; a crossed (negative) charge yields a negative
// intensity, which the callers' `intensity > 0` gate then declines to apply.
[[nodiscard]] float
spaceflight_detail::NovaShip_IonizationIntensity(const Ship &ship,
                                                 const ShipClass &ship_class) {
  return NovaOutfit_NormalizeIonizationIntensity(
      ship.ionization_points,
      static_cast<float>(ship_class.ionization_capacity));
}

// Shared ionization block of Ship_HandleShip (0x0043373f/0x00434394) and
// PlayerTick_IonizationAndFuelRegeneration (0x0045073f/0x00452304). See the
// declaration for the exact gate/decay/ramp contract. Both the 0.7 intensity
// cap (DAT_00575478/DAT_00575668) and the 0.025 ramp constant
// (DAT_00575480/DAT_00575670) are doubles in the original; this port keeps
// them as floats, an accepted precision divergence.
void spaceflight_detail::NovaShip_UpdateIonizationCharge(
    GameState &state,
    Ship &ship,
    float effective_max_speed_px_per_tick,
    float elapsed_ticks) {
  if (ship.ionization_points <= 0.0F) {
    ship.ionization_points = 0.0F;
    return;
  }
  // The original subtracts and stores the raw float result (x87 FSUBR, no
  // clamp); the next frame's gate re-normalizes a crossed negative value.
  const float decay_rate = NovaOutfit_ComputeIonizationDecayRate(state, ship);
  ship.ionization_points -= decay_rate * elapsed_ticks;
  const float intensity =
      std::min(0.7F, NovaOutfit_GetIonizationIntensity(state, ship));
  const float ionized_cap =
      (1.0F - intensity) * effective_max_speed_px_per_tick;
  const float damp_step = 0.025F * elapsed_ticks;
  // Two sequential per-axis tests, not if/else: the original re-tests the
  // updated component, so a component can be stepped and then stepped back in
  // the same frame (observable when the cap is exactly zero). No clamp to the
  // cap is applied -- a step may overshoot past it.
  if (ship.vel_x > ionized_cap) {
    ship.vel_x -= damp_step;
  }
  if (ship.vel_x < -ionized_cap) {
    ship.vel_x += damp_step;
  }
  if (ship.vel_y > ionized_cap) {
    ship.vel_y -= damp_step;
  }
  if (ship.vel_y < -ionized_cap) {
    ship.vel_y += damp_step;
  }
}

// Shared ModType-17 cloak shield upkeep. `shield_drain` is the ModVal nibble
// (bits 0x0100..0x0800) and kCloakDrainPerUnit is the original's 1/30 second
// x87 double.
void spaceflight_detail::NovaShip_ApplyCloakShieldDrain(
    Ship &ship, std::int16_t shield_drain, float elapsed_ticks) {
  if (shield_drain <= 0) {
    return;
  }
  // BUGFIX(original): the original only drains while the whole per-second rate
  // (1/2/4/8) is affordable, so the last `shield_drain` shields persist. Under
  // the policy, drain normally and clamp at zero.
  bool drain = true;
  if constexpr (!kApplyOriginalBugFixes) {
    drain = static_cast<float>(shield_drain) <= ship.shield_points;
  }
  if (!drain) {
    return;
  }
  ship.shield_points -=
      static_cast<float>(shield_drain) * kCloakDrainPerUnit * elapsed_ticks;
  if (ship.shield_points <= 0.0F) {
    ship.shield_points = 0.0F;
  }
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

// Ghidra 0x004642e0 Ship_ComputeShipEffectiveMaxSpeed tail applied to the
// port's split bodies: NovaShip_ComputeEffectiveStats is the NPC main body and
// Outfit_ComputePlayerEffectiveStats supplies the player opcode-8 aggregate.
// The common tail then applies the mission-slot 0x3ff x2 (DAT_005757a8), the
// player/direct-escort 1.5x (DAT_005757b8, when Strict Play is off) and the
// final negative clamp. The player branch resolves its class through
// state.player, so `ship_class` is only dereferenced for NPCs.
float NovaShip_ComputeEffectiveMaxSpeedPxPerTick(const GameState &state,
                                                 const Ship &ship,
                                                 const ShipClass &ship_class) {
  float max_speed;
  if (ship.ship_instance_id == 0) {
    max_speed = Outfit_ComputePlayerEffectiveStats(state).speed_raw / 100.0F;
    if (ship.pers_def_slot == 0x3ff) {
      max_speed *= 2.0F; // DAT_005757a8
    }
  } else {
    max_speed = NovaShip_ComputeEffectiveStats(state, ship, ship_class)
                    .max_speed_px_per_tick;
  }
  if ((ship.ship_instance_id == 0 || ship.squad_leader_ship_slot == 0) &&
      !state.pilot.strict_play) {
    max_speed *= 1.5F; // DAT_005757b8
  }
  return max_speed < 0.0F ? 0.0F : max_speed;
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
// block, matching the original's two-regime movement model. The
// velocity-match tug/cloak/fuel/aggro/derelict tail blocks are split into the
// helper functions above (NovaAi_Compute*RegenRate, NovaAi_ComputeShipFuel-
// RechargeRate) and the shared cloak helpers in ship_ai.cpp.
// Ghidra 0x00433050 Ship_HandleShip.
void NovaShip_IntegrateNpcMovement(GameState &state,
                                   Ship &ship,
                                   const ShipClass &ship_class,
                                   float elapsed_ticks) {
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
  // The movement clamp/threshold use Ship_ComputeShipEffectiveMaxSpeed
  // (0x004642e0), which includes the player-led-squad +1.5x tail
  // (DAT_005757b8) that the raw NovaShip_ComputeEffectiveStats aggregate does
  // not; the helper also applies the final negative clamp.
  const float eff_max_speed =
      NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, ship, ship_class);
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
  // The same gate also opens for the class-0x2ff sentinel (the escape pod),
  // which Ship_HandleShip admits even with an active maneuver timer.
  const bool holds_course = coasting && ship.ai_state_code != 0x16 &&
                            ship.ship_class_id != kEscapePodShipClassIndex;
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

    // Ghidra 0x00433050 disabled arm: a disabled direct escort of the player
    // (squad_leader_ship_slot 0, not a mission/defense-fleet ship) converts to
    // its default behavior and dumps its cargo share; every other disabled hull
    // rolls the probabilistic auto-repair. The original writes
    // g_playerInventoryAndLoadoutDirty; the port recomputes inventory-derived
    // state live, and the cargo-status panel redraws each frame.
    if (ship.mission_fleet_slot == -1 &&
        ship.defense_fleet_home_stellar_id == -1 &&
        ship.squad_leader_ship_slot == 0) {
      if (ship_class.default_ai_behavior < 3) {
        Player_TransferCargoAndJunkToEscortByRatio(state,
                                                   ship.ship_instance_id);
      }
      ship.squad_leader_ship_slot = -1;
      ship.escort_origin_mark = 0;
      ship.boarded_target_latch = 1;
      ship.ai_behavior_code = ship_class.default_ai_behavior;
      NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
    } else if (Frame_ShouldTriggerAutoRepairTick(state, ship)) {
      // Armor is restored to a fraction of the effective maximum: 1/3 for a
      // normal hull, 1/10 when capability Flags 0x10, plus one point
      // (DOUBLE_00575440 = 1.0).
      const float max_armor = NovaAi_ComputeMaxArmorPoints(state, ship);
      const float armor_fraction =
          (ship_class.capability_flags & 0x10U) != 0U ? 0.1F : (1.0F / 3.0F);
      ship.armor_points = max_armor * armor_fraction + 1.0F;
      // The original raises g_playerShipPresentationDirty when the player's
      // HUD target is this ship; the port's HUD is immediate-mode.
      if (ship.post_hit_mode_hint >= 0) {
        // Surrender conversion: the ship joins the player's squad (slot 0),
        // loses shields/targets, and either becomes a behavior-5 fighter or a
        // behavior-6 escort depending on how it was disabled. The hint selects
        // the STR# 0x7d2 overlay (0x80/0x7f) and the escort-origin mark.
        ship.squad_leader_ship_slot = 0;
        ship.shield_points = 0.0F;
        ship.ai_secondary_target_slot = -1;
        ship.primary_target_ship_slot = -1;
        if (ship.post_hit_mode_hint == 0) {
          ship.ai_behavior_code = 5;
          if (auto text = NovaHud_LoadStringEntry(0x7D2, 0x80)) {
            NovaHud_ShowOverlayMessage(state, *text, std::uint64_t{0xfa});
          }
        } else {
          ship.escort_origin_mark =
              static_cast<std::int8_t>(ship.post_hit_mode_hint == 1 ? 1 : 0);
          ship.ai_behavior_code = 6;
          if (auto text = NovaHud_LoadStringEntry(0x7D2, 0x7f)) {
            NovaHud_ShowOverlayMessage(state, *text, std::uint64_t{0xfa});
          }
        }
        state.pending_ui_sounds.push_back(GameState::PendingUiSound{8, 1});
      }
    }
  } else {
    // Ghidra 0x00433050: a live hull clears any stale post-hit hint every
    // frame, so a later disable starts from the neutral state.
    ship.post_hit_mode_hint = -1;
  }

  // Producer: Shot_UpdateBeamHitQueue (0x0042f270)'s negative-impact-impulse
  // arm, ported in NovaWeapon_ResolveDirectWeaponHit, arms the +0xC8DC lock and
  // +0xB4 stamp from a tractor/repulsor beam hit.
  //
  // Ghidra 0x00433050 velocity-match physical tug. The step size is the
  // PLAYER's effective thrust times g_death_puff_offset_scale_f64 (0.25) -- a
  // deliberate original quirk: the tug reads g_ship_states[0] regardless of
  // which ship carries the velocity-match lock. Each axis is stepped toward the
  // target's velocity and snaps when within one step. The original applies the
  // fixed step once per Ship_HandleShip call; the port replays that discrete
  // per-call step via RawSpaceflightCallTicks so the pull stays frame-rate
  // independent.
  if (ship.velocity_match_target_ship_slot != -1) {
    constexpr float kVelocityMatchTugScale =
        0.25F; // g_death_puff_offset_scale_f64
    const PlayerEffectiveStats player_eff =
        Outfit_ComputePlayerEffectiveStats(state);
    // Player branch of Ship_ComputeShipEffectiveThrust (0x004640a0): the
    // opcode-7 aggregate x2.0 (DAT_005757a8), the mission-ship x2.0 for pers
    // slot 0x3ff, ionization damping (min(intensity,0.7)) while charged, and a
    // final negative clamp. The step is a fixed per-raw-call value in the
    // original (no g_avg_frame_tick_scale), so replay it per 21 ms raw call.
    float player_thrust = player_eff.thrust_raw / 10000.0F * 2.0F;
    if (state.player.pers_def_slot == 0x3ff) {
      player_thrust *= 2.0F;
    }
    if (state.player.ionization_points > 0.0F) {
      const float intensity = std::min(
          0.7F, NovaOutfit_GetIonizationIntensity(state, state.player));
      player_thrust *= (1.0F - intensity);
    }
    player_thrust = std::max(0.0F, player_thrust);
    const float step = player_thrust * kVelocityMatchTugScale *
                       RawSpaceflightCallTicks(elapsed_ticks);
    float target_vel_x = 0.0F;
    float target_vel_y = 0.0F;
    const std::int16_t target_slot = ship.velocity_match_target_ship_slot;
    if (target_slot != ship.ship_instance_id &&
        state.SlotInRange(static_cast<std::size_t>(target_slot))) {
      const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
      target_vel_x = target.vel_x;
      target_vel_y = target.vel_y;
    }
    if (target_vel_x + step < ship.vel_x) {
      ship.vel_x -= step;
    }
    if (ship.vel_x < target_vel_x - step) {
      ship.vel_x += step;
    }
    if (target_vel_y + step < ship.vel_y) {
      ship.vel_y -= step;
    }
    if (ship.vel_y < target_vel_y - step) {
      ship.vel_y += step;
    }
    if (std::abs(ship.vel_x - target_vel_x) < step) {
      ship.vel_x = target_vel_x;
    }
    if (std::abs(ship.vel_y - target_vel_y) < step) {
      ship.vel_y = target_vel_y;
    }
  }

  // Ghidra 0x00433050 NPC cloak upkeep (the player equivalent lives in
  // PlayerTick_InteractionCloakAndStatus). While the hull is at the cloak
  // visibility threshold, an unmaintainable cloak is cleared and the ModType-17
  // fuel (bits 0x10..0x80) and shield (bits 0x100..0x800) drains are applied.
  // Drain scale is the shared kCloakDrainPerUnit (Ghidra 0x00575470, an x87
  // double = 1/30).
  if (NovaTargeting_ShipAtCloakVisibilityThreshold(ship)) {
    if (!NovaAiShip_CanMaintainCloakState(state, ship)) {
      NovaAi_OnShipCloakStateCleared(state, ship);
    }
    const std::int16_t fuel_drain =
        NovaOutfit_GetCloakFuelDrainFlags(state, ship);
    if (fuel_drain > 0) {
      ship.fuel_points -=
          static_cast<float>(fuel_drain) * kCloakDrainPerUnit * elapsed_ticks;
      if (ship.fuel_points <= 0.0F) {
        ship.fuel_points = 0.0F;
      }
    }
    spaceflight_detail::NovaShip_ApplyCloakShieldDrain(
        ship, NovaOutfit_GetCloakShieldDrainFlags(state, ship), elapsed_ticks);
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

  // Ghidra 0x00433050: the ionization decay/ramp runs AFTER position
  // integration and BEFORE the turn/regen and thrust blocks, so those read the
  // freshly decayed intensity. (The inertialess steer above intentionally uses
  // the pre-decay effective thrust: the original calls
  // Ship_ComputeShipEffectiveThrust before its ionization block, then again
  // inside the thrust block.)
  spaceflight_detail::NovaShip_UpdateIonizationCharge(
      state, ship, eff_max_speed, elapsed_ticks);
  const NpcEffectiveStats stats_post_ionization =
      NovaShip_ComputeEffectiveStats(state, ship, ship_class);
  float eff_turn_deg_post = stats_post_ionization.turn_rate_deg_per_tick;
  if (eff_turn_deg_post >= 1.0F) {
    eff_turn_deg_post = std::max(eff_turn_deg_post, 1.0F);
  }
  const float eff_thrust_post = stats_post_ionization.thrust_px_per_tick2;

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
  int turn_dir = 0;
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
    const float max_turn_deg = eff_turn_deg_post * elapsed_ticks;
    if (std::abs(delta_deg) <= max_turn_deg) {
      ship.heading =
          static_cast<float>(ship.ai_desired_heading_deg) * kDegToRad;
    } else {
      ship.heading =
          (cur_deg + std::copysign(max_turn_deg, delta_deg)) * kDegToRad;
      turn_dir = delta_deg > 0.0F ? 1 : -1;
    }
    ship.heading = std::fmod(ship.heading, kFullCircleDeg / kDegToRad);
    if (ship.heading < 0.0F) {
      ship.heading += kTwoPi;
    }

    // Regeneration shares the block's disabled gate. The capacities use the
    // personality-scaled effective maxima (Ship_ComputeShipMaxShieldPoints/
    // MaxArmor) and the regen rates the class + class-default-outfit ModType
    // 5/0x1d bonuses with the behavior-5 x1.333 scale; the original does not
    // clamp the summed value back to the maximum.
    const float max_shield =
        static_cast<float>(NovaAi_ComputeMaxShieldPoints(state, ship));
    if (ship.shield_points < max_shield) {
      ship.shield_points +=
          NovaAi_ComputeShipShieldRegenRate(state, ship) * elapsed_ticks;
    }
    const float max_armor = NovaAi_ComputeMaxArmorPoints(state, ship);
    if (ship.armor_points < max_armor) {
      ship.armor_points +=
          NovaAi_ComputeShipArmorRegenRate(state, ship) * elapsed_ticks;
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
    constexpr float kJumpVelocityDamp = 0.8F; // DAT_00575488
    // Unled ships subtract 35.0 (0x575498); player-led escorts
    // (squad_leader_ship_slot == 0) subtract 45.0 (0x575490). The player-led
    // arm only runs while the player's hold timer is above
    // k_unit_f32 (1.0, 0x575318), and it reads the player's own class
    // multiplier and mode-start stamp.
    constexpr float kJumpProgressSubtract = 35.0F;          // DAT_00575498
    constexpr float kJumpPlayerLedProgressSubtract = 45.0F; // DAT_00575490
    // k_unit_f32 (0x575318) is a shared 1.0 constant (unit decrement in the
    // fade countdowns elsewhere); here it is the player hold-timer threshold.
    constexpr float kUnitFloat = 1.0F;
    constexpr float kJumpProgressCap = 50.0F;   // DAT_00575388
    constexpr float kJumpDurationScale = 0.01F; // DOUBLE_00575368
    constexpr float kX2UnledScale = 0.5F;       // 0x3fe00000
    constexpr float kX2PlayerLedScale = 0.667F; // 0x3fe55810

    ship.vel_x *= kJumpVelocityDamp;
    ship.vel_y *= kJumpVelocityDamp;

    const float current_heading_deg = ship.heading / kDegToRad;
    const float heading_delta_deg = std::remainder(
        static_cast<float>(ship.ai_desired_heading_deg) - current_heading_deg,
        360.0F);
    const bool aligned =
        std::abs(heading_delta_deg) <= eff_turn_deg_post * elapsed_ticks;
    if (!aligned) {
      // Ghidra resets the mode-start timestamp while the ship is still
      // turning, so the jump-speed ramp begins only after alignment.
      ship.ai_mode_start_time_ms = state.tick_60hz;
    } else {
      // Ship_HandleShip 0x004347e8: progress = elapsed_60hz * scale *
      // jump_duration_multiplier / (duration_60hz * 0.01) - offset /
      // multiplier, where the class multiplier scales both the clock and the
      // offset. x2 mode halves the unled clock (0.5) and scales the
      // player-led clock by 0.667.
      const float duration_60hz = NovaTravel_JumpSequenceDuration60Hz(state);
      float jump_progress = 0.0F;
      if (ship.squad_leader_ship_slot == 0) {
        const Ship &leader = state.player;
        if (kUnitFloat < leader.ai_station_hold_timer) {
          const float player_multiplier =
              NovaTravel_PlayerJumpDurationMultiplier(state);
          const float elapsed_jump_60hz = static_cast<float>(
              state.tick_60hz - leader.ai_mode_start_time_ms);
          const float scale = state.x2_mode_active ? kX2PlayerLedScale : 1.0F;
          jump_progress = elapsed_jump_60hz * scale * player_multiplier /
                              (duration_60hz * kJumpDurationScale) -
                          kJumpPlayerLedProgressSubtract / player_multiplier;
        }
      } else {
        const float class_multiplier = ship_class.jump_duration_multiplier;
        const float elapsed_jump_60hz =
            static_cast<float>(state.tick_60hz - ship.ai_mode_start_time_ms);
        const float scale = state.x2_mode_active ? kX2UnledScale : 1.0F;
        jump_progress = elapsed_jump_60hz * scale * class_multiplier /
                            (duration_60hz * kJumpDurationScale) -
                        kJumpProgressSubtract / class_multiplier;
      }
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
  // Ghidra 0x00433050 maneuver-timer countdown is skipped for the class-0x2ff
  // escape pod, which never carries a coast timer (same sentinel as the thrust
  // gate above).
  if (ship.ai_maneuver_timer_ms > 0.0F &&
      ship.ship_class_id != kEscapePodShipClassIndex) {
    ship.ai_maneuver_timer_ms =
        std::max(0.0F, ship.ai_maneuver_timer_ms - elapsed_ticks);
  }

  // Ghidra 0x00433050 turn-bank animation ramp (ShipState +0xc8e4). Only
  // classes with sprite_behavior_flags bit 0 bank: the phase ramps toward +-8
  // while turning (DAT_005754b4 = 8.0, DAT_005754a8 = -8.0) and decays toward
  // 0 otherwise; ai_turn_bias_dir becomes +1 above FLOAT_005754ac = 4.0, -1
  // below DAT_005754b0 = -4.0, else 0. The ramp shares the thrust/coast gate.
  ship.ai_turn_bias_dir = 0;
  if (!holds_course && (ship_class.sprite_behavior_flags & 1U) != 0U) {
    float &phase = ship.turn_bank_animation_phase;
    if (turn_dir == 1) {
      if (phase < 8.0F) {
        phase += elapsed_ticks;
      }
    } else if (turn_dir == -1) {
      if (-8.0F < phase) {
        phase -= elapsed_ticks;
      }
    } else if (elapsed_ticks <= phase) {
      phase -= elapsed_ticks;
    } else if (-elapsed_ticks < phase) {
      phase = 0.0F;
    } else {
      phase += elapsed_ticks;
    }
    if (phase > 4.0F) {
      ship.ai_turn_bias_dir = 1;
    } else if (phase < -4.0F) {
      ship.ai_turn_bias_dir = -1;
    }
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
    } else if (ship.ai_forward_thrust_cmd < eff_thrust_post * 2.0F) {
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

  // Ghidra 0x00433050 fuel tail: add the class/outfit scoop rate, clamp to the
  // effective capacity (the original truncates the double capacity through a
  // 16-bit cast) and then floor at zero. The player's equivalent runs in
  // PlayerTick_IonizationAndFuelRegeneration.
  float fuel_recharge_rate = 0.0F;
  if (NovaAi_ComputeShipFuelRechargeRate(state, ship, fuel_recharge_rate)) {
    ship.fuel_points += fuel_recharge_rate * elapsed_ticks;
  }
  const float fuel_capacity = static_cast<float>(
      static_cast<std::int16_t>(NovaAi_ComputeShipFuelCapacity(state, ship)));
  if (fuel_capacity < ship.fuel_points) {
    ship.fuel_points = fuel_capacity;
  }
  if (ship.fuel_points < 0.0F) {
    ship.fuel_points = 0.0F;
  }

  // Ghidra 0x00433050 velocity-match expiry: clear the lock when the target is
  // gone/disabled/destroyed, and unconditionally retire it 30 ticks (0x1e)
  // after it was armed.
  const std::int16_t match_slot = ship.velocity_match_target_ship_slot;
  if (match_slot != -1) {
    if (match_slot != ship.ship_instance_id &&
        state.SlotInRange(static_cast<std::size_t>(match_slot))) {
      const Ship &match_target =
          state.ShipAt(static_cast<std::size_t>(match_slot));
      if (!match_target.is_active ||
          NovaAiShip_IsDisabled(state, match_target) ||
          NovaAiShip_IsDestroyed(match_target)) {
        ship.velocity_match_target_ship_slot = -1;
        ship.velocity_match_start_tick_60hz = 0;
      }
    }
    if (ship.velocity_match_start_tick_60hz + 0x1eU <= state.tick_60hz) {
      ship.velocity_match_target_ship_slot = -1;
    }
  }

  // Ghidra 0x00433050: a derelict-government hull never shows engine glow.
  // Applies to destroyed wrecks too (the original has no early return).
  if (NovaGovernment_IsGovernmentDerelict(state.scenario,
                                          ship.faction_or_government_id)) {
    ship.engine_glow_level = 0;
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
