#pragma once

// Clean-room reconstruction of the in-game spaceflight mode, mirroring Ghidra
// 0x00489210 Ship_RunSpaceflightMode and its contained 0x00417600
// Frame_SpaceflightLoop / 0x004186b0 Frame_TickSystems. The full flight/AI/
// combat simulation is not reconstructed; this module provides the faithful
// *skeleton*: it plays the new-game intro cinematic on first entry, then runs
// the in-system loop with the original's phase/scope ordering
// (Frame_SpaceflightLoop pre-draw/sim/draw/post-draw and Frame_TickSystems's
// run_full_tick-gated scopes). Unimplemented scopes are no-ops until their
// subsystem is reconstructed.
//
// The reimplementation's escape back to the menu diverges from the original,
// which latches DAT_00596d38 on the primary mouse command through the pause
// menu; see the loop body in spaceflight.cpp.

#include "../sdl_audio.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"

class SdlPlatform;

namespace game {

// Enters in-system spaceflight mode. On the pilot's first entry (state
// intro_played == false) it first plays the intro cinematic, then sets
// intro_played = true immediately after it returns -- mirroring
// Ship_RunSpaceflightMode setting DAT_00596d35 = 0x01 right after
// IntroCinematic_Run(). Then it runs the in-game main loop until the player
// returns to the menu, at which point it returns. Mirrors
// Ship_RunSpaceflightMode's gating (DAT_00596d35) and clean return to the menu
// shell.
void NovaSpaceflight_Run(SdlPlatform &platform,
                         SdlAudio &audio,
                         GameState &state);

// Ship-class movement stats derived from the raw resource as the original
// loader (NovaData_LoadScenarioResourceTables 0x004bd3c0) derives
// ShipClassDef.base_turn_rate_deg / base_speed / accel(0x3c):
//   accel    = raw_accel    / 10000.0  * 2.0  (px/tick^2; runtime multiplier
//                                                 DAT_005757a8=2.0)
//   speed    = raw_speed    / 100.0           (px/tick; DAT_00575e48, NOT 640)
//   turn     = raw_maneuver * 0.1             (deg/tick; DAT_00575e58)
// See spaceflight.cpp for the source-constant references.
struct PlayerMovementStats {
  float turn_rate_deg_per_tick = 0.0F;
  float max_speed_px_per_tick = 0.0F;
  float thrust_px_per_tick2 = 0.0F;
};

// Pure free-flight physics integrator (unit-testable; no SDL). Derives stats
// from a ShipClass and advances the player ship for an elapsed interval
// according to the Input key latches. Faithful to the original movement model
// ("Player ship movement (free flight)" comment in spaceflight.cpp): bank
// continuously at the class turn rate while a turn key is held, thrust along
// heading as a per-AXIS-polar-clamped step (Math_AddPolarVelocityWithClamp
// 0x0043b4e0), inertia-preserving coast when thrust released, and the original
// reverse command (turn toward the direction opposite current velocity). All
// rates are scaled by `elapsed_ticks`, normalized to the original 30 Hz
// simulation cadence (the same role as g_avg_frame_time_ms in the original).
// Returns the same PlayerMovementStats it integrated with so the caller knows
// what was applied.
[[nodiscard]] PlayerMovementStats
NovaPlayer_IntegrateMovement(PlayerShip &ship,
                             const FlightInput &input,
                             const ShipClass &ship_class,
                             float elapsed_ticks);

// Applies the live flight controls to the player ship: integrates the heading/
// throttle from an already-polled flight-input snapshot into GameState.player
// so the ship flies during flight. The caller supplies the snapshot so it can
// also feed the travel/other channels without polling the keyboard twice.
extern void NovaPlayer_UpdateFromInput(GameState &state,
                                       const FlightInput &input,
                                       float elapsed_ticks);

// Per-frame player status/outfit maintenance (Ghidra
// PlayerTick_StatusAndOutfit- Events, internal label of
// Ship_HandlePlayerShipCore 0x0044aa70): fire- restricted velocity damping, the
// disabled auto-repair system, the periodic distress-call cue, and carried-bomb
// countdown/detonation. Must run before the flight input pass each frame (the
// original dispatches it ahead of the manual- flight block). `eject_command`
// carries the eject key state for the destroyed-ship escape-pod block
// (0x004510b9). Returns true when the player ship is no longer active (death
// bookkeeping consumed the frame).
extern bool NovaPlayer_TickStatusAndOutfitEvents(GameState &state,
                                                 float elapsed_ticks,
                                                 bool eject_command);

// Per-frame in-flight shield regeneration (the spaceflight loop calls this
// once a frame). Restores the player's shields toward the effective maximum at
// the recorded shield-recharge rate (class base + outfit opcode-18 bonuses,
// scaled by frame time), capped so it never exceeds max shield points.
// Mirrors the shield-regen portion of the original's per-frame player update.
// Armor does NOT regenerate in flight (the original only repairs armor while
// landed/at the shipyard or via the disabled auto-repair system), so this only
// grows shield_points.
extern void NovaPlayer_TickShieldRecharge(GameState &state,
                                          float frame_time_ms);

// Ghidra PlayerTick_TimedActionTransition (internal label of
// Ship_HandlePlayerShipCore 0x0044aa70; block 0x0044d490..0x0044da70). While
// the player's blocking timed action is armed (the escape-pod flight launched
// by the eject transform), moves the ship at the effective thrust/top speed
// along the current heading, decrements the countdown, and - on reaching zero -
// runs the death/escape-pod respawn transition: mission abort, player-ship
// reset, the fresh ship class's OnPurchase script, emergency-destination
// relocation, weapon-bank reload, meters refill, world re-population, the
// 15..44 elapsed-day catch-up and the registration-number reroll. The
// original returns from the player core while a timed action runs, so the
// caller must skip the remaining player command blocks. Returns true when the
// timed action was active this frame.
extern bool NovaPlayer_TickTimedActionTransition(GameState &state,
                                                 float elapsed_ticks);

// Effective NPC movement stats, ported from the NPC branch of Ghidra
// Ship_ComputeShipEffectiveThrust (0x004640a0) /
// Ship_ComputeShipEffectiveMaxSpeed (0x004642e0) for a clean NPC (no outfit /
// disable state):
//   thrust_px_per_tick2  = base_accel * govt_scale * 2.0  (base = accel/10000)
//   max_speed_px_per_tick = base_speed * govt_scale       (base = speed/100)
//   turn_rate_deg_per_tick = base_turn * 0.1              (NOT govt-scaled;
//                              Ship_ComputeShipMaxTurnRateDeg 0x00463e70)
// Government combat_rating_scale applies only when the ship has a faction
// (faction_or_government_id != -1). The per-ship skill_variance_scale (+0x40)
// NPC acceleration and speed also include the per-ship skill_variance_scale
// (+0x40), seeded by ShipClass_ComputeShipClassSkillVarianceScale (0x0046b870);
// turn rate does not. Capability flag 0x400 zeros all three stats; a
// non-self velocity-match lock multiplies all three by 1/3; mission slot
// 0x3ff doubles thrust/speed and supplies the matching low-turn correction;
// ionization intensity damps thrust and, while not thrusting, turn rate.
struct NpcEffectiveStats {
  float thrust_px_per_tick2 = 0.0F;
  float max_speed_px_per_tick = 0.0F;
  float turn_rate_deg_per_tick = 0.0F;
};

[[nodiscard]] NpcEffectiveStats NovaShip_ComputeEffectiveStats(
    const GameState &state, const Ship &ship, const ShipClass &ship_class);

// One per-axis step of Ghidra Math_AddPolarVelocityWithClamp (0x0043b4e0), the
// original's single forward-thrust pathway. For each velocity axis it combines
// the polar projection of the class top speed (max_proj) with the polar
// projection of the per-frame thrust step (delta) and the current velocity,
// exactly as decoded. Named NovaPlayer_* for continuity but used by the NPC
// integrator (NovaShip_IntegrateNpcMovement) too.
void NovaPlayer_AddPolarVelocityClamped(float heading_rad,
                                        float thrust_step,
                                        float max_speed,
                                        float &vel_x,
                                        float &vel_y);

// Ghidra Frame_QueueCombatChatter (0x00426ce0): latches one pending combat
// chatter request (kind, government voice, variant selector) onto GameState.
// Consumed by the deferred Frame_UpdateCombatChatter pass (TODO(decomp)).
void NovaFrame_QueueCombatChatter(GameState &state,
                                  std::int16_t kind,
                                  std::int16_t government_id,
                                  std::int16_t variant);

// NPC free-flight movement integrator, ported from the movement block of Ghidra
// Ship_HandleShip (0x00433050). The AI writes the ship's ai_desired_heading_deg
// / ai_desired_speed / ai_forward_thrust_cmd fields; this turns the ship toward
// the desired heading at the class turn rate (continuous, unlike the player's
// integer-rounded keyboard path), then applies forward or reverse thrust along
// the heading and integrates position. Honors the ai_maneuver_timer_ms
// coast-through-reversal timer by holding heading and coasting. Derives the
// effective thrust/max-speed/turn from the class, including the high-confidence
// NPC capability/velocity-match/mission/ionization branches, and scales all
// rates by `elapsed_ticks` normalized to the 30 Hz simulation cadence.
extern void NovaShip_IntegrateNpcMovement(GameState &state,
                                          Ship &ship,
                                          const ShipClass &ship_class,
                                          float elapsed_ticks,
                                          std::uint32_t now_ms = 0);

// Port of Ghidra Ship_SteerVelocityTowardShipHeading (0x0043b020), the momentum
// / turn integrator for gravity-shield ships (ShipClassDef.flags_secondary bit
// 0x40). These ships keep a scalar `speed` in Ship.speed; this converts it into
// an actual velocity each frame by snapping the velocity toward the
// forward-heading * speed vector, then letting it relax back toward the
// previous frame's velocity at a rate of `eff_thrust * 4.0 * frame_time` per
// axis (Math_AddPolarVelocity semantics + a per-axis clamp that never
// overshoots the prior velocity). Produces a smooth velocity rotation rather
// than an instant heading snap. Confidence: medium-high on the shape; the
// turn-scale constant 4.0 (_DAT_005754ac) is provisional.
extern void NovaShip_SteerVelocityTowardShipHeading(Ship &ship,
                                                    float eff_thrust,
                                                    float elapsed_ticks);

// Whether an NPC ship is on the gravity-shield movement model: the ship-class
// flags_secondary bit 0x40 gate (and not in ai_control_mode 0x0c). Ports the
// NPC branch of Outfit_ShipHasGravityShieldOutfit (0x0046df70); the player's
// owned-outfit branch is handled separately in the player path.
[[nodiscard]] inline bool NovaShip_HasGravityShield(const Ship &ship,
                                                    const ShipClass &cls) {
  return (cls.flags_secondary & 0x40) != 0 && ship.ai_control_mode != 0x0c;
}

// External test seam into the per-ship simulation pass (Ghidra scope 4/5 of
// Frame_TickSystems 0x004186b0 -> Ship_HandleShip 0x00433050 validation
// prologue). The per-frame stub Stub_HandleShips lives in an anonymous
// namespace in spaceflight.cpp; this wrapper gives unit tests a stable external
// entry point. Runs the deactivation/range-reset prologue then integrates each
// active, non-player ship of the current system through
// NovaShip_IntegrateNpcMovement.
void NovaShip_TickNpcShips(GameState &state, float elapsed_ticks);

} // namespace game
