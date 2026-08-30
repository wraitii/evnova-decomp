#pragma once

// Clean-room reconstruction of the NPC ship AI decision layer, mirroring the
// Ghidra per-ship AI functions documented in the plates. On the movement side
// the AI writes the concrete fields already consumed by
// NovaShip_IntegrateNpcMovement (ai_desired_heading_deg / ai_desired_speed /
// ai_forward_thrust_cmd); the AI state machine (Ship_UpdateShipAiState
// 0x00405590) maintains Ship.ai_state_code, the behavior supervisors
// (0x00401000 and the 0x00402xxx/0x00403xxx set) pick primary targets and
// escalate between states, and Ship_ApplyShipAiControls (0x00408150) finally
// translates Ship.ai_control_mode into those concrete movement fields. Called
// once per active NPC ship per frame from the spaceflight loop's
// Stub_AiRoutines (Ghidra scope 6 part 2).
//
// Convention mirrors ship_spawn.cpp: each Ghidra function maps to one top-level
// helper with the address in a comment; NPC ships carry no outfit inventory, so
// the class base values stand in for the effective stats the original derives
// from outfits/ionization effects (TODO(decomp) markers below). Units are the
// simulation's reference cadence (30 Hz); rates passed in are already frame-
// scaled by the caller where the original does the same.

#include "game_state.hpp"
#include "scenario_data.hpp"

namespace game {

// Ghidra 0x0043b740 Ship_AimWeaponPredictive. Predictive weapon lead-aim:
// returns the game-degree bearing toward the best intercept point for a
// straight-flight / fast-close weapon (mode -1/4/6/7/8/9) against a moving
// target. Leads the target by flight time using RELATIVE velocity
// (target_vel - ship_vel): intercept = target_pos + (target_vel - ship_vel)*t.
// t = dist / projectile_speed for straight guns; mode 6 (freeflight rocket)
// uses the two-regime model (k_mode6_rocket_* constants: threshold 19.59,
// near-speed factor 0.316, far-time bonus 2.06667) because the rocket is still
// spooling up. Non-lead weapons (and weapon_id < 0) fall back to the straight
// bearing to the target's current position, matching the original.
//
// Weapon is addressed by bank index (weapon_id + 0x80 in the scenario tables);
// shot speed is the port's projectile_speed/100 so the lead matches the actual
// fired shot velocity. Used by the AI control modes 6/7 (steer the hull at the
// lead) and at fire time for turret modes 4/7/8/9.
[[nodiscard]] std::int16_t NovaAi_AimWeaponPredictive(const GameState &state,
                                                      const Ship &ship,
                                                      const Ship &target,
                                                      std::int16_t weapon_id);

// Ghidra 0x00464810 Ship_GetShipJammingScore. Electronic-warfare jamming score
// (0..100) of `ship` for one seek channel (0..3), lazily computed and cached in
// Ship.jamming_score (the original's ShipState +0xC926). Base value is the
// owning government's inherent jam (InhJam1-4); outfit ModType opcodes
// 0x21..0x24 (Jamming Type 1-4) add their ModVal per owned instance for the
// player or per mounted stock outfit for an NPC. NPCs whose government has
// flags_primary 0x80 get half credit. Returns 0 for fire-restricted ships.
// The player's cache is invalidated on outfit changes and at system
// transitions; the original only reseeds it when a ship slot is allocated.
[[nodiscard]] int NovaAi_GetShipJammingScore(const GameState &state,
                                             Ship &ship,
                                             int seek_channel);

// Ghidra 0x004688e0 Ship_IsShipDestroyed. True when the death timer is active
// (death_timer_active > 0) or armor_points <= 0.
[[nodiscard]] bool NovaAiShip_IsDestroyed(const Ship &ship);

// Ghidra 0x0040c790 Stellar_SelectRandomAdjacentTravelStellar. Selects one
// eligible travel stellar in the ship's current system, applying the currently
// reconstructed availability and government-hostility filters.
[[nodiscard]] std::int16_t
NovaAi_SelectRandomAdjacentTravelStellar(GameState &state, const Ship &ship);

// Ghidra 0x00410670 Ship_EnterShipAiState0x02_ClearPrimaryTarget. Enters AI
// state 0x02 (local jump-departure staging), clears the current primary target,
// clamps the station-hold timer below zero, and records the current tick count
// in ai_mode_start_time_ms. The state brakes while moving, then selects the
// centre-outward mode-3/mode-4 departure path.
void NovaAi_EnterState2ClearPrimaryTarget(Ship &ship, std::uint32_t now_ms);

// Ghidra 0x00410dd0 Ship_ResetShipPrimaryAndSecondaryTargets. Resets the
// movement/target state to idle unless the ship is in states 9 or 0xf; this is
// the per-ship exit used by state 8 after control mode 0x0a's reverse step.
void NovaAi_ResetShipPrimaryAndSecondaryTargets(Ship &ship);

// Ghidra 0x00410e20. Initializes the short NPC arrival/slowdown phase used
// when a new ship has no adjacent
// restricted stellar from which to emerge. The caller supplies the initial
// polar position/velocity; this helper arms the state and its visual latches.
void NovaAi_EnterState8Slowdown(GameState &state, Ship &ship);

// Ghidra 0x004159e0 Ship_EnterShipAiState0x15_JumpOutToSystem. Places an NPC
// at a destination hypergate/wormhole stellar's emergence point, seeds its
// emergence heading, and arms a 60-tick hold before the slower arrival
// override: 30 px/tick normally, or 15 when ai_target_ship_slot is the player.
// This is not normally a persistent state-0x17 successor in the NPC path.
void NovaAi_EnterState15JumpOutToSystem(GameState &state,
                                        Ship &ship,
                                        std::int16_t stellar_id);

// Clean-room cross-system completion for an NPC already in state 0x14. The
// original's larger hyperspace presentation path is not shared with this
// per-ship AI tick; this helper performs its gameplay-visible system transfer.
bool NovaAi_CompleteNpcJump(GameState &state, Ship &ship);

// Ghidra 0x004687b0 Ship_IsShipFireRestricted. True when the ship must not
// fire/act this frame: derelict government (flags_primary 0x800), docked to a
// stellar (target_stellar_object_id set) for non-player ships, or critically
// damaged (armor below a government/aggression-dependent fraction of max).
// Shared with the spawn-maintenance cleanup
// (NovaShip_DeactivateVacantShipsAndTally 0x0041ad50), which spares
// non-fire-restricted ships actively engaging the player.
[[nodiscard]] bool NovaAiShip_IsFireRestricted(const GameState &state,
                                               const Ship &ship);

// Ghidra 0x004680d0 Ship_OnShipCloakStateEntered. Starts the signed cloak
// transition and drops shields when ModType 17 requests it.
void NovaAi_OnShipCloakStateEntered(GameState &state, Ship &ship);

// Ghidra 0x00468190 Ship_OnShipCloakStateCleared. Starts the negative cloak
// transition when the ship becomes visible enough to leave cloak.
void NovaAi_OnShipCloakStateCleared(Ship &ship);

// Ghidra 0x00411d00 Ship_UpdateShipCloakStateFromTraits. Re-evaluates the
// NPC's cloak transition from its ModType 17 loadout, resources, class Flags2,
// combat state, and target relationship. Weapon hits do not produce the fade.
void NovaAi_UpdateShipCloakStateFromTraits(GameState &state, Ship &ship);

// Ghidra 0x00410f20 Ship_CanShipInterceptCurrentPrimaryTarget. Validates the
// current target's activity/system, minimum hull mass, relative-velocity
// bearing, and the caller/target class-speed relation.
[[nodiscard]] bool
NovaAiShip_CanInterceptCurrentPrimaryTarget(const GameState &state,
                                            const Ship &ship);

// Ghidra 0x00412030 Ship_FindBestAssistTargetForShip. `score_flags` is the
// original short argument: 0x226 limits the first range term, while -1 means
// no range limit. Returns the slot with the lowest positive score.
[[nodiscard]] std::int16_t NovaAi_FindBestAssistTargetForShip(
    const GameState &state, const Ship &ship, std::int16_t score_flags);

// Ghidra 0x00411540 Ship_UpdateAutoWeaponSelectionFromTarget. Refreshes the
// active NPC weapon bank for higher-behavior ships and clears stale targets.
void NovaAi_UpdateAutoWeaponSelectionFromTarget(GameState &state, Ship &ship);

// The four per-mode weapon-bank selectors called by the AI control modes
// (Ghidra 0x00408150). Each is the faithful port of one original selector:
// direct-fire and guided pick a primary-target weapon, current-target picks a
// turret-ish bank, and general is the broad fallback.

// Ghidra 0x0040ce00 Weapon_SelectWeaponBankForCurrentTarget. Scans fireable
// mode-3/4/7/8 turret banks in arc/range, scores by mass/energy damage, and
// arms the best one.
void NovaAi_SelectWeaponBankForCurrentTarget(GameState &state, Ship &ship);

// Ghidra 0x0040d220 Weapon_SelectGuidedWeaponBankForPrimaryTarget. Arms the
// first guided (mode-1) bank that can track the primary target within range;
// applies scanner-untargetable / cloaked-target capability gates.
void NovaAi_SelectGuidedWeaponBankForPrimaryTarget(GameState &state,
                                                   Ship &ship);

// Ghidra 0x0040d470 Weapon_SelectDirectFireWeaponBankForPrimaryTarget. Arms
// the best direct-fire bank (modes -1/0/6, or mode-1 when allow_guided_mode)
// in range, with a mode-6 blast-radius placement gate; retries relaxed once
// with guided allowed when no bank was armed.
void NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(GameState &state,
                                                       Ship &ship,
                                                       bool allow_guided_mode);

// Ghidra 0x0040d910 Weapon_SelectGeneralWeaponBank. Broad fallback: arms the
// most recent fireable general weapon (non mode-0/3, mode < 8).
void NovaAi_SelectGeneralWeaponBank(GameState &state, Ship &ship);

// Ghidra 0x0040d7e0 Weapon_SelectUnguidedWeaponBank. Fallback that arms the
// highest-damage unguided bank (modes -1/0/6, or mode-7 with no primary
// target). Used by the scripted mode 0x14 merge gate.
void NovaAi_SelectUnguidedWeaponBank(GameState &state, Ship &ship);

// Ghidra 0x00405590 Ship_UpdateShipAiState. The per-frame AI state machine.
// Given the ship's current ai_state_code it maintains that state and writes
// the companion Ship.ai_control_mode field (consumed by
// Ship_ApplyShipAiControls to produce concrete steering/thrust). States 0..0x16
// as enumerated in the plate comment in ship_ai.cpp. Most states are faithful;
// the HUD/mission flavor side-effects (extortion messages, carrier-bay launch,
// fuel-transfer chatter) are documented no-ops until those systems are
// reconstructed.
void NovaAi_UpdateShipState(GameState &state,
                            Ship &ship,
                            std::uint32_t now_ms,
                            float elapsed_ticks = 1.0F);

// Ghidra 0x00408150 Ship_ApplyShipAiControls. Transforms the ship's
// ai_control_mode (written each frame by the state machine) into the concrete
// movement fields the integrator consumes: ai_desired_heading_deg,
// ai_desired_speed, ai_forward_thrust_cmd. This is the bridge that makes the
// AI state machine actually move ships. Also latches ai_fire_trigger_latch for
// the firing path (deferred to Phase 5). `elapsed_ticks` is the normalized
// cadence used by the position/velocity creeps (the original's misleadingly
// named _g_avg_frame_time_ms is elapsed milliseconds * 0.03); `now_ms` backs
// the mode-4/0xd wall-clock bookkeeping.
// The per-mode turn/thrust polynomials come from the decoded _DAT_00575xxx
// globals. Weapon selection is wired in via the four NovaAi_Select* helpers;
// formation-offset mirroring, predictive-aim-when-bank-live, carrier-bay
// launches and the +0xBD boost latch remain deferred (TODO(decomp)).
void NovaAi_ApplyControls(GameState &state,
                          Ship &ship,
                          float elapsed_ticks,
                          std::uint32_t now_ms);

// Ghidra 0x00401000 Ship_UpdateShipAI. The top-level per-ship AI entry: applies
// the global "heavy AI" cadence gating/skips, recomputes some stat caches,
// dispatches to the behavior supervisor selected by ship.ai_behavior_code, then
// runs the state machine and applies controls. `skip_heavy_ai` mirrors the
// original's parameter and forces the reduced path. Wire this once per active
// NPC ship per frame (it replaces Stub_AiRoutines scope-6 part 2).
void NovaAi_UpdateShipAI(GameState &state,
                         Ship &ship,
                         bool skip_heavy_ai,
                         std::uint32_t now_ms,
                         float elapsed_ticks = 1.0F);

// ---------------------------------------------------------------------------
// Ship-comm / hail predicates and AI state entries (added for the ship-comm
// dialog 0x0047e470). Each maps one Ghidra function; the comm dialog branches
// on these to pick its prompt and to order the target ship around.
// ---------------------------------------------------------------------------

// Ghidra 0x00464a90 Ship_CanShipEngageTargetUnderCloakRules. True when the
// first ship's cloak visibility can be engaged using the second ship's
// mission/scanner/position context. The neutral subject/other ordering is
// intentional: callers reuse this predicate in pursuit, hostility, and weapon
// selection paths with different attacker/target roles.
[[nodiscard]] bool NovaAiShip_CanEngageTargetUnderCloakRules(
    const GameState &state, const Ship &subject_ship, const Ship &other_ship);

// Ghidra 0x00467e80 Ship_CanMaintainCloakState. Whether the ship can keep (or
// enter) its cloaking state: not fire-restricted, carries a ModType 17
// cloaking device (player: owned outfits; NPC: the ship class's default
// outfit list), and the device's configured fuel/shield drain flags are
// satisfiable from current resources.
[[nodiscard]] bool NovaAiShip_CanMaintainCloakState(const GameState &state,
                                                    const Ship &ship);

// Ghidra 0x0040f780 Ship_ShouldShipKeepPressingTarget. True when a pursuing
// ship should keep its primary target (or the player under mutual targeting):
// active, not fire-restricted, holding an AI target and a primary target,
// passing the cloak-aware engagement predicate, not coasting through a reversal
// (ai_maneuver_timer_ms <= 0), and in a non-disengage AI state (not in
// {7,9,15,10,11,5,12,18}) -- either directly on the player, on a ship that
// targets the player, or pressed by a third ship that itself holds the player
// or a player-targeting primary. Mirrors the original's literal
// primary_target_ship_slot == ship_instance_id comparison in the third-ship
// scan (instance ids equal slots in this build).
[[nodiscard]] bool NovaAiShip_ShouldKeepPressingTarget(const GameState &state,
                                                       const Ship &ship);

// Ghidra 0x0040fc00 Ship_IsShipEligibleForCommAidInteraction. True when some
// other active ship is positioned to come to `ship`'s aid: iterates the other
// slots and returns true for the first one in a non-disengage state
// (ai_state_code not in {7,9,15,10,11,5,12}) while `ship`'s primary target
// slot equals its own instance id (a literal port of the original's
// comparison; see the implementation comment).
[[nodiscard]] bool
NovaAiShip_IsShipEligibleForCommAidInteraction(const GameState &state,
                                               const Ship &ship);

// Ghidra 0x0040fca0 / 0x0040fce0. True when the ship is braking/throttled
// while locked onto the player in AI state 0x09 (escort-pursue) / 0x0F
// (pursue-with-restrictions). Gates the ship-comm dialog's "on my way" vs
// "I'm busy" prompt pick.
[[nodiscard]] bool
NovaAiShip_IsShipBrakingOnPlayerState9(const GameState &state,
                                       const Ship &ship);
[[nodiscard]] bool
NovaAiShip_IsShipBrakingOnPlayerState0xF(const GameState &state,
                                         const Ship &ship);

// Ghidra 0x00411270 Ship_IsShipInNonIdleAiState. True when the ship's
// ai_state_code is not one of the idle/non-combat states {0 (track-parked), 1
// (docked), 2 (idle-template), 7 (escort-arrive), 0x14 (jump/travel)}.
[[nodiscard]] bool NovaAiShip_IsShipInNonIdleAiState(const Ship &ship);

// Ghidra 0x00410060 Ship_AreAnyShipsEligibleForDistressCall. Scans the NPC
// slots for any ship satisfying NovaTargeting_IsShipEligibleForDistressCall.
[[nodiscard]] bool
NovaAi_AreAnyShipsEligibleForDistressCall(const GameState &state);

// Ghidra 0x004101d0 Ship_CanShipRespondToDistressCall. Whether `responder`
// can serve as a responder for `distressed`'s distress-call flow: distinct,
// both active, `distressed` not a 0x3ff mission slot, and government relation
// rules -- hostile/xenophobic governments respond, same-government does not,
// and a xenophobic `distressed` government admits any non-allied responder.
[[nodiscard]] bool NovaAiShip_CanShipRespondToDistressCall(
    const GameState &state, const Ship &responder, const Ship &distressed);

// Ghidra 0x004100a0 Ship_HasShipDistressResponder. True when at least one
// other ship (not `ship`) both keeps pressing its own target and can respond
// to `ship`'s distress call. Used by the comm dialog to route the distress
// branch.
[[nodiscard]] bool NovaAiShip_HasShipDistressResponder(const GameState &state,
                                                       const Ship &ship);

// Ghidra 0x00410c30 Ship_EnterShipAiState0x09_TargetPlayerAndBrake. Enters AI
// state 0x09 targeting the player, resets hostility/hold-timer/control, and
// sets ai_maneuver_timer_ms -1 (coast through reversal). Called when the hail
// target agrees to come to the player's aid.
void NovaAi_EnterState9TargetPlayerAndBrake(Ship &ship);

// Ghidra 0x00410c70 Ship_EnterShipAiState0x0F_TargetPlayerAndBrake. Same as
// the 0x09 entry but for fire-restricted ships (state 0x0F).
void NovaAi_EnterState0FTargetPlayerAndBrake(Ship &ship);

// Ghidra 0x00410b00 Ship_EnterShipAiState0x04_TargetRandomUnengagedShip.
// Counts same-system ships that are not fire-restricted, not destroyed, hold
// the player as primary target and are in AI state 0x03/0x04; picks one at
// random, stores it in primary_target_ship_slot and enters ai_state_code
// 0x04 (clears the target to -1 when none).
void NovaAi_EnterState4TargetRandomUnengagedShip(GameState &state, Ship &ship);

// Ghidra 0x004107e0 Ship_EnterShipAiState0x04_TargetRandomCombatCandidate.
// Same as TargetRandomUnengagedShip but the candidate scan additionally
// requires NovaTargeting_IsShipEligibleForDistressCall.
void NovaAi_EnterState4TargetRandomCombatCandidate(GameState &state,
                                                   Ship &ship);

// Ghidra 0x00410700 Ship_SetShipHostileToPlayer. Flips the ship hostile: sets
// ai_state_code 0x04, clears the secondary target, targets the player, and
// drops escort control modes 0x04/0x0D. The mission-side announcement arm
// (Mission_ShowMissionShipAnnouncement for mission-ship slots) is deferred
// with TODO(decomp) -- mission ship defs are not modelled.
void NovaAi_SetShipHostileToPlayer(GameState &state, Ship &ship);

// Ghidra 0x0046b260 Ship_CanShipUseAfterburner (DB name
// Ship_IsShipEligibleForEscortOrLaunchBehavior): computes the ShipState +0xBD
// afterburner latch seeded at spawn time. Zero when another active ship lists
// this ship's instance id as its formation leader (escorts stay with the
// leader) or when the class capability flags say never (0x0400 planet-type).
// Capability 0x0040 is always-afterburner; capability 0x0020 rolls against
// the player's combat rating (roll + 0x100 <= rating / class Strength). The
// rating producer is not yet reconstructed, so the roll currently fails
// (TODO(decomp)).
[[nodiscard]] bool NovaShip_CanShipUseAfterburner(GameState &state,
                                                  const Ship &ship);

// Ghidra 0x00402810 Ship_ResetShipAiBehaviorRuntimeFields: resets the core
// per-behavior runtime slots (ai_state_code/ai_control_mode, travel target
// cache, escort mirror cache, resolved-target slot).
void NovaShip_ResetAiBehaviorRuntimeFields(Ship &ship);

} // namespace game
