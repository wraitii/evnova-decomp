#pragma once

// Public interface for the clean-room spaceflight mode. The implementation
// mirrors Ghidra 0x00489210 Ship_RunSpaceflightMode and its contained
// 0x00417600 Frame_SpaceflightLoop / 0x004186b0 Frame_TickSystems. See the
// audited Ship_HandlePlayerShipCore region inventory in spaceflight.cpp for
// internal PlayerTick_* boundaries; those labels are navigation metadata, not
// original source-level functions or part of this public API.
//
// The reimplementation's escape back to the menu diverges from the original,
// which latches DAT_00596d38 on the primary mouse command through the pause
// menu; see the loop body in spaceflight.cpp.

#include "../sdl_audio.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "preferences.hpp"

class SdlPlatform;

namespace game {

// Enters in-system spaceflight mode. On the pilot's first entry (state
// intro_played == false) it first plays the intro cinematic, then sets
// intro_played = true immediately after it returns -- mirroring
// Ship_RunSpaceflightMode setting g_intro_played = 0x01 right after
// IntroCinematic_Run(). Then it runs the in-game main loop until the player
// returns to the menu, at which point it returns. Mirrors
// Ship_RunSpaceflightMode's gating (g_intro_played) and clean return to the
// menu shell.
void NovaSpaceflight_Run(SdlPlatform &platform,
                         SdlAudio &audio,
                         GameState &state,
                         const NovaPreferences &prefs);

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
  // Signed rotation applied this frame (-1 left / +1 right / 0 none), the
  // original's sVar8 in the manual-flight turn block. Feeds the turn-bank
  // animation for keyboard steering AND the auto-turn continuations (reverse,
  // face-target, jump alignment all bank in the original).
  int turn_dir = 0;
};

// Optional movement context threaded from
// PlayerTick_ManualFlightAndRegeneration (Ghidra 0x0044aa70 manual-flight
// region). speed_cap_* < 0 falls back to the class max speed so bare integrator
// calls keep the plain clamp.
struct PlayerMovementOptions {
  // Face-target auto-turn arm (0x0044c0b1 command); suppresses keyboard
  // steering and turns one step per frame toward ai_desired_heading_deg.
  bool face_target_armed = false;
  // Inertialess movement model (Outfit_ShipIsInertialess
  // 0x0046df70 player branch): thrust accumulates the scalar speed, which the
  // steering block converts into velocity via
  // NovaShip_SteerVelocityTowardShipHeading; ship.speed stays a scalar.
  bool inertialess = false;
  // Fire-restricted flag (the port's NovaAiShip_IsDisabled approximation of
  // the parent's local fire-restriction latch); drives the inertialess speed
  // decay.
  bool fire_restricted = false;
  // Per-axis velocity clamp targets (g_player_speed_cap_x/y). Always >= the
  // effective max speed while maintained by the afterburner tail.
  float speed_cap_x = -1.0F;
  float speed_cap_y = -1.0F;
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
                             float elapsed_ticks,
                             const PlayerMovementOptions &opts = {});

// Applies the live flight controls to the player ship: integrates the heading/
// throttle from an already-polled flight-input snapshot into GameState.player
// so the ship flies during flight. The caller supplies the snapshot so it can
// also feed the travel/other channels without polling the keyboard twice.
// face_target_armed is the manual-flight auto-turn arm set by
// PlayerTick_FaceTargetCommand (Ghidra 0x0044c0b1 face-target command):
// while armed, keyboard steering is suppressed and the hull turns one step per
// frame toward the stored ai_desired_heading_deg.
extern void
PlayerTick_ManualFlightAndRegeneration(GameState &state,
                                       const FlightInput &input,
                                       float elapsed_ticks,
                                       bool face_target_armed = false);

// Ghidra 0x0044aa70 face-target command (0x0044c0b1 -> 0x0044c18a, binding
// slot 7; the port binds R -- see FlightInput::face_target in
// sdl_platform.hpp). While held with the station-hold timer idle, stores the
// integer heading to face in player.ai_desired_heading_deg and returns true
// to arm the manual-flight auto-turn: the primary ship target wins unless the
// 0x38/0x6f arm modifier is held, which -- like having no ship target --
// faces the selected travel stellar instead.
extern bool PlayerTick_FaceTargetCommand(GameState &state,
                                         const FlightInput &input,
                                         bool arm_modifier_held);

// Per-frame player status/outfit maintenance from Ship_HandlePlayerShipCore
// 0x0044aa70 (internal label PlayerTick_StatusAndOutfitEvents): fire-restricted
// velocity damping, disabled auto-repair, the periodic distress-call cue, and
// carried-bomb countdown/detonation. Must run before the flight input pass each
// frame (the
// original dispatches it ahead of the manual-flight block). `eject_command`
// carries the eject key state for the escape-pod block (Ghidra 0x00451024;
// reachable while the hull is disabled or destroyed). Returns true when
// death/inactive bookkeeping consumed the player tick; the caller must skip
// timed actions and every later player-command region for that frame.
extern bool PlayerTick_StatusAndOutfitEvents(GameState &state,
                                             float elapsed_ticks,
                                             bool eject_command);

// Ghidra 0x00431480 Frame_JitterPlayerStatModifiers: random-walks the first
// two persisted player stat modifiers (state.player_stat_modifier_pct[0]/[1],
// the DAT_007353f6/f8 pair) by +-1 with 2-in-3 probability, clamped to
// [0x55,0x73] = [85,115] percent. Runs at the launch tail (Stellar_TravelTo-
// System 0x00456038) and the in-flight jump arrival (0x0044fb39).
extern void NovaFrame_JitterPlayerStatModifiers(GameState &state);

// Ghidra 0x00431500 Frame_RerollPlayerStatModifiers: rerolls the second pair
// (state.player_stat_modifier_pct[2]/[3], DAT_007353fa/fc) to
// rand(0x15)+0x5a = [90,114] percent. Same call sites as the jitter.
extern void NovaFrame_RerollPlayerStatModifiers(GameState &state);

// Ghidra Ship_HandlePlayerShipCore synthetic region
// PlayerTick_ShieldAndArmorRegeneration 0x0044CB99 -> 0x0044CCAF.
extern void PlayerTick_ShieldAndArmorRegeneration(GameState &state,
                                                  float frame_time_ms);

// Ghidra Ship_HandlePlayerShipCore synthetic region
// PlayerTick_IonizationAndFuelRegeneration 0x00450717 -> 0x004507B4.
extern void PlayerTick_IonizationAndFuelRegeneration(GameState &state,
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
extern bool PlayerTick_TimedActionTransition(GameState &state,
                                             float elapsed_ticks);

// Effective NPC movement stats, ported from the NPC branch of Ghidra
// Ship_ComputeShipEffectiveThrust (0x004640a0) /
// Ship_ComputeShipEffectiveMaxSpeed (0x004642e0) for a clean NPC (no outfit /
// disable state):
//   thrust_px_per_tick2  = base_accel * govt_scale * 2.0  (base = accel/10000)
//   max_speed_px_per_tick = base_speed * govt_scale       (base = speed/100)
//   turn_rate_deg_per_tick = base_turn * 0.1              (NOT govt-scaled;
//                              Ship_ComputeShipMaxTurnRateDeg 0x00463e70)
// Government SkillMult (GovtDef 0x64) applies only when the ship has a faction
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

// Ghidra 0x004642e0 Ship_ComputeShipEffectiveMaxSpeed. The port splits the
// main body between NovaShip_ComputeEffectiveStats (NPC) and
// Outfit_ComputePlayerEffectiveStats (player opcode-8 aggregate); this applies
// the shared tail: mission-slot 0x3ff x2 (DAT_005757a8), the player/direct-
// escort 1.5x (DAT_005757b8, when Strict Play is off), and the final negative
// clamp. Returns px/tick. Used by the impact-impulse clamp and the ionization
// ramp; the stepping integrators still read their own raw aggregate.
[[nodiscard]] float NovaShip_ComputeEffectiveMaxSpeedPxPerTick(
    const GameState &state, const Ship &ship, const ShipClass &ship_class);

// One per-axis step of Ghidra Math_AddPolarVelocityWithClamp (0x0043b4e0), the
// original's single forward-thrust pathway. For each velocity axis it combines
// the polar projection of the class top speed (max_proj) with the polar
// projection of the per-frame thrust step (delta) and the current velocity,
// exactly as decoded. Named NovaPlayer_* for continuity but used by the NPC
// integrator (NovaShip_IntegrateNpcMovement) too.
void Math_AddPolarVelocityWithClamp(float heading_rad,
                                    float thrust_step,
                                    float max_speed,
                                    float &vel_x,
                                    float &vel_y);

// Ghidra Frame_QueueCombatChatter (0x00426ce0): latches one pending combat
// chatter request (kind, government voice, variant selector) onto GameState.
// Consumed by Frame_UpdateCombatChatter.
void NovaFrame_QueueCombatChatter(GameState &state,
                                  std::int16_t kind,
                                  std::int16_t government_id,
                                  std::int16_t variant);

// Ghidra 0x004311f0 Frame_UpdateCombatChatter. Retires a drained chatter
// resource or starts the pending acknowledgement/targeting/victory line.
void NovaFrame_UpdateCombatChatter(GameState &state, SdlAudio &audio);

// Ghidra 0x004313c0 Frame_CancelCombatChatter. Clears the pending request but
// deliberately lets an already-playing line finish.
void NovaFrame_CancelCombatChatter(GameState &state, SdlAudio &audio);

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
// / turn integrator for inertialess ships (ShipClassDef.flags_secondary bit
// 0x40). These ships keep a scalar `speed` in Ship.speed; this commands a
// velocity of forward-heading * speed (Math_AddPolarVelocity) and then advances
// the previous frame's velocity toward that command at a rate of
// `eff_thrust * 4.0 * frame_time` per axis (per-axis clamp, no overshoot of the
// command). Produces a smooth velocity acceleration rather than an instant
// heading snap. The turn scale _DAT_005754ac = 4.0f is disasm-verified;
// `eff_thrust`/`elapsed_ticks` stand in for the original's internal
// Ship_ComputeShipEffectiveThrust and g_avg_frame_tick_scale.
extern void NovaShip_SteerVelocityTowardShipHeading(Ship &ship,
                                                    float eff_thrust,
                                                    float elapsed_ticks);

// Whether an NPC ship is on the inertialess movement model: the ship-class
// flags_secondary bit 0x40 gate (and not in ai_control_mode 0x0c). Ports the
// NPC branch of Outfit_ShipIsInertialess (0x0046df70); the player's
// owned-outfit branch is handled separately in the player path.
[[nodiscard]] inline bool NovaShip_IsInertialess(const Ship &ship,
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

// External test seam into the per-ship AI decision pass (Ghidra scope 6 of
// Frame_TickSystems 0x004186b0 -> Ship_UpdateShipAI 0x00401000). Mirrors
// NovaShip_TickNpcShips so tests can model the live AI-then-movement order.
void NovaShip_TickNpcAi(GameState &state, float elapsed_ticks);

} // namespace game
