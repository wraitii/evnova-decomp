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

// Ghidra 0x004112C0 Ship_ShowPlayerInterceptTauntIfEligible.
void NovaAi_ShowPlayerInterceptTauntIfEligible(GameState &state, Ship &ship);

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

// Same algorithm with the 0x0043b740 fourth argument (float *ship_pos_xy):
// the straight bearing and intercept distance are measured from the given
// origin (the muzzle position after quadrant geometry) instead of the hull
// centre. Ship velocity for the lead term still comes from the ship.
[[nodiscard]] std::int16_t
NovaAi_AimWeaponPredictiveFrom(const GameState &state,
                               const Ship &ship,
                               const Ship &target,
                               std::int16_t weapon_id,
                               float origin_x,
                               float origin_y);

// Ghidra 0x0043b8c0 Ship_AimWeaponLeadVelocity. The same intercept lead as
// Ship_AimWeaponPredictive, but the target is a raw position/velocity (a
// scripted asteroid or station rather than a ShipState). Mode 6 (freeflight
// rocket) is excluded from the lead gate, matching the original, so a rocket
// falls back to the straight bearing; the original's own mode-6 branch is
// unreachable dead code. The firing hull still supplies its own velocity.
[[nodiscard]] std::int16_t NovaAi_AimWeaponLeadVelocity(const GameState &state,
                                                        const Ship &ship,
                                                        float target_pos_x,
                                                        float target_pos_y,
                                                        float target_vel_x,
                                                        float target_vel_y,
                                                        std::int16_t weapon_id,
                                                        float origin_x,
                                                        float origin_y);

// Ghidra 0x00464810 Ship_GetShipJammingScore. Electronic-warfare jamming score
// (0..100) of `ship` for one seek channel (0..3), lazily computed and cached in
// Ship.jamming_score (the original's ShipState +0xC926). Base value is the
// owning government's inherent jam (InhJam1-4); outfit ModType opcodes
// 0x21..0x24 (Jamming Type 1-4) add their ModVal per owned instance for the
// player or per mounted stock outfit for an NPC. NPCs whose government has
// flags_primary 0x80 get half credit. Returns 0 for disabled ships.
// The player's cache is invalidated on outfit changes and at system
// transitions; the original only reseeds it when a ship slot is allocated.
[[nodiscard]] int NovaAi_GetShipJammingScore(const GameState &state,
                                             Ship &ship,
                                             int seek_channel);

// Ghidra 0x004688e0 Ship_IsShipDestroyed. True when the death timer is active
// (death_timer_active > 0) or armor_points <= 0.
[[nodiscard]] bool NovaAiShip_IsDestroyed(const Ship &ship);

// Ghidra 0x0040c790 Stellar_SelectRandomAdjacentTravelStellar. Selects one
// eligible travel stellar in the ship's current system, applying the
// government ScanMask preference pools and hostility filters. `strict_mode`
// (the 1-in-3 roll from Stellar_SelectRandomAdjacentDestination) and
// `unrestricted_only` (true for the behavior-0x04 interceptor caller
// 0x00403de0, false elsewhere) match the original selector's second and third
// arguments. Returns the stellar resource id (>= 0x80) or -1.
[[nodiscard]] std::int16_t
NovaAi_SelectRandomAdjacentTravelStellar(GameState &state,
                                         const Ship &ship,
                                         bool strict_mode,
                                         bool unrestricted_only);

// Ghidra 0x0046e9e0 Stellar_SelectRandomAdjacentDestination. Runs the
// selector with a NovaRandom_Range(3) strict_mode roll and returns the
// chosen id only when its availability_flags & 0x3000 marks a hypergate /
// wormhole; ordinary travel points yield -1.
[[nodiscard]] std::int16_t
NovaAi_SelectRandomAdjacentDestination(GameState &state, const Ship &ship);

// Ghidra 0x0040cc10 Stellar_FindNearestAdjacentTravelStellar. Returns the
// nearest adjacent, non-restricted, non-hostile travel stellar to `ship` in
// its current system, excluding `excluded_stellar_id`; -1 when none.
[[nodiscard]] std::int16_t NovaAi_FindNearestAdjacentTravelStellar(
    const GameState &state, const Ship &ship, std::int16_t excluded_stellar_id);

// Ghidra 0x00410670 Ship_EnterShipAiState0x02_ClearPrimaryTarget. Enters AI
// state 0x02 (local jump-departure staging), clears the current primary target,
// clamps the station-hold timer below zero, and records the current 60 Hz tick
// count (GameState.tick_60hz) in ai_mode_start_time_ms. The state brakes while
// moving, then selects the centre-outward mode-3/mode-4 departure path.
void NovaAi_EnterState2ClearPrimaryTarget(GameState &state, Ship &ship);

// Ghidra 0x00410dd0 Ship_ResetShipPrimaryAndSecondaryTargets. Resets the
// movement/target state to idle unless the ship is in states 9 or 0xf; this is
// the per-ship exit used by state 8 after control mode 0x0a's reverse step.
void NovaAi_ResetShipPrimaryAndSecondaryTargets(Ship &ship);

// Ghidra 0x00410e20. Initializes the short NPC arrival/slowdown phase used
// when a new ship has no adjacent
// restricted stellar from which to emerge. The caller supplies the initial
// polar position/velocity; this helper arms the state and its visual latches.
void NovaAi_EnterState8Slowdown(GameState &state, Ship &ship);

// Ghidra 0x004159e0 Ship_EnterShipAiState0x15_EmergeFromHypergate. Places an
// NPC at a destination hypergate/wormhole stellar's emergence point, seeds its
// emergence heading, and arms a 60-tick hold before the slower arrival
// override: 30 px/tick normally, or 15 when squad_leader_ship_slot is the
// player. This is not normally a persistent state-0x17 successor in the NPC
// path.
void NovaAi_EnterState15EmergeFromHypergate(GameState &state,
                                            Ship &ship,
                                            std::int16_t stellar_id);

// Completes a cross-system jump for an NPC already in state 0x14: performs
// the gameplay-visible system transfer. The original's larger hyperspace
// presentation path is not shared with this per-ship AI tick.
bool NovaAi_CompleteNpcJump(GameState &state, Ship &ship);

// Ghidra 0x004687b0 Ship_IsShipDisabled. True when the ship must not
// fire/act this frame. Sources: derelict government (flags_primary 0x800;
// Bible: "ships of this govt start out disabled (derelicts)"), mission
// ShipGoal-5 special ships not yet attacking (TODO(decomp): the
// special_ship_attacking runtime flag is not modelled yet), or critically
// damaged (armor below 1/3 of max, or 1/10 with Ship capability flags 0x10).
// Non-player ships with a stellar target are exempt: the original returns
// not-disabled for them without reaching the armor check (0x00468856), so
// ships on a landing/jump-out approach never count as disabled.
// Shared with the spawn-maintenance cleanup
// (NovaShip_DeactivateVacantShipsAndTally 0x0041ad50), which spares
// non-disabled ships actively engaging the player.
[[nodiscard]] // Ghidra 0x0046b360 Weapon_IsTargetBearingInTurretBlindSpot:
              // whether the bearing lies in one of the weapon's turret
              // blind-spot sectors (front <46 deg
              // / side <136 deg / rear; weapon flags_primary
              // 0x1000/0x2000/0x4000, force-overridden by the matching
              // ShipClass capability flags). Turreted fire/selection paths
              // reject the bank while the target is in a blind spot.
[[nodiscard]] bool
NovaAi_WeaponIsTargetBearingInTurretBlindSpot(const ShipClass &ship_class,
                                              const Weapon &weapon,
                                              std::int16_t heading_deg,
                                              std::int16_t target_bearing_deg);

bool NovaAiShip_IsDisabled(const GameState &state, const Ship &ship);

// Computes the current maximum shield capacity using the player outfit cache
// or the NPC class / personality values used by the original AI.
[[nodiscard]] double NovaAi_ComputeMaxShieldPoints(const GameState &state,
                                                   const Ship &ship);

// Ghidra 0x004637a0 Ship_ComputeShipMaxArmor: player via the cached outfit
// aggregate, NPC = class base * positive personality shield_armor_scale *
// behavior-5 difficulty. Returned as float because the original stores the
// behavior-5 product to float.
[[nodiscard]] float NovaAi_ComputeMaxArmorPoints(const GameState &state,
                                                 const Ship &ship);

// Ghidra 0x00463a20 Ship_ComputeShipFuelCapacity. Player capacity folds in
// opcode-12 (kFuelCapacity) outfit bonuses via the effective-stats pass; an
// NPC's is the raw class value (the original skips the outfit loop for
// ship_instance_id != 0).
[[nodiscard]] double NovaAi_ComputeShipFuelCapacity(const GameState &state,
                                                    const Ship &ship);

// Ghidra 0x00463680 Ship_ComputeShipShieldRegenRate. NPC branch: class
// ShieldRech (points/frame) plus ModType-5 outfit bonuses from the class
// default loadout, clamped >= 0, then multiplied by 1.333 for behavior 5.
// The player branch is Outfit_ComputePlayerEffectiveStats().shield_recharge.
[[nodiscard]] float NovaAi_ComputeShipShieldRegenRate(const GameState &state,
                                                      const Ship &ship);

// Ghidra 0x004638e0 Ship_ComputeShipArmorRegenRate. NPC branch: class
// ArmorRech plus ModType-29 outfit bonuses from the class default loadout,
// clamped >= 0, then multiplied by 1.333 for behavior 5. Returns 0 while the
// ship is disabled; the Ship_HandleShip caller is already gated on that.
[[nodiscard]] float NovaAi_ComputeShipArmorRegenRate(const GameState &state,
                                                     const Ship &ship);

// Ghidra 0x00463b30 Ship_ComputeShipFuelRechargeRate. NPC branch: fuel
// scoop rate (fuel points/frame) from the class FuelRegen (1/FuelRegen) plus
// ModType-18 class default outfits (count * 1/ModVal). Returns true and sets
// `out_rate` when any recharge source is active; false (out_rate untouched)
// otherwise.
[[nodiscard]] bool NovaAi_ComputeShipFuelRechargeRate(const GameState &state,
                                                      const Ship &ship,
                                                      float &out_rate);

// Ghidra 0x004680d0 Ship_OnShipCloakStateEntered. Starts the signed cloak
// transition, drops shields when ModType 17 requests it, and queues the
// player cloak-enter cue (snd 381).
void NovaAi_OnShipCloakStateEntered(GameState &state, Ship &ship);

// Ghidra 0x00468190 Ship_OnShipCloakStateCleared. Starts the negative cloak
// transition when the ship becomes visible enough to leave cloak, and queues
// the player cloak-clear cue (snd 380).
void NovaAi_OnShipCloakStateCleared(GameState &state, Ship &ship);

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

// Ghidra 0x00412090 Ship_ScoreAssistTargetForShip. Scores `candidate` as a
// potential target for `helper`; returns 0 when the candidate is not eligible.
// The original's own parameter names (`ship`, `target_ship`) are inverted
// relative to these roles.
[[nodiscard]] std::int32_t
NovaAi_ScoreAssistTargetForShip(const GameState &state,
                                const Ship &candidate,
                                const Ship &helper,
                                std::int16_t score_flags);

// Ghidra 0x00412030 Ship_FindBestAssistTargetForShip. `score_flags` is the
// original short argument: 0x226 limits the first range term, while -1 means
// no range limit. Returns the slot with the lowest positive score.
[[nodiscard]] std::int16_t NovaAi_FindBestAssistTargetForShip(
    const GameState &state, const Ship &ship, std::int16_t score_flags);

// Ghidra 0x00411c20 Ship_FindSwarmMate. Resets the
// cached swarm-mate slot to -1, then scans strictly lower ship slots for an
// active swarming-behavior hull (ShipClass Flags2 0x0001) that shares this
// ship's primary target and either its faction (when not -1) or its squad
// leader (when not -1). Caches and returns the first match, else -1. This is
// the only producer of a positive swarm_mate_ship_slot.
[[nodiscard]] std::int16_t NovaAi_FindSwarmMate(const GameState &state,
                                                Ship &ship);

// Ghidra 0x00411b40 Ship_IsSwarmMateStillValid. True for a
// non-swarming hull (nothing to maintain), or when the cached lower-indexed
// swarm mate is still active, swarming, and sharing the same primary target
// plus faction / squad-leader context. False means the cache must be re-found.
[[nodiscard]] bool NovaAiShip_IsSwarmMateStillValid(const GameState &state,
                                                    const Ship &ship);

// Ghidra 0x00411ae0 Ship_ShouldFollowSwarmMate. For a swarming
// hull outside a defense fleet, when the cached swarm-mate slot is valid and
// is not this ship's own squad leader, forces ai_control_mode 0x12 (chase the
// swarm mate) and returns true. The side effect is committed before returning.
[[nodiscard]] bool NovaAiShip_ShouldFollowSwarmMate(const GameState &state,
                                                    Ship &ship);

// Ghidra 0x00411540 Ship_EscortFireAtUnprovokedTarget. Refreshes the
// active NPC weapon bank for behavior >4 ships; ships with a lower behavior
// return untouched (behavior 3/4 arm banks in the combat control modes).
// Clears the primary target whenever it is inactive or disabled, else
// delegates to the turret/point-defense selector Weapon_FireTurretAtTarget.
void NovaAi_EscortFireAtUnprovokedTarget(GameState &state, Ship &ship);

// The per-mode weapon-bank selectors called by the AI control modes
// (Ghidra 0x00408150). Each is the faithful port of one original selector:
// direct-fire and guided pick a primary-target weapon, current-target fires
// the turret/point-defense selector, and general is the broad fallback.

// Ghidra 0x0040ce00 Weapon_FireTurretAtTarget (Carbon symbol AIFireTurret).
// Runs the point-defense auto-fire prologue, then scans fireable mode-3/4/7/8
// turret banks in arc/range, scores by mass/energy damage, and arms the best
// one (energy preferred while the target still has shields).
void NovaAi_FireTurretAtTarget(GameState &state, Ship &ship);

// Ghidra 0x004221d0 Ship_IsInboundThreatExceedingDefenses. True when the
// signed inbound damage tally is at least 105% of the ship's current shields
// plus armor. The original comparison is unordered-false.
[[nodiscard]] bool NovaAi_IsInboundThreatExceedingDefenses(const Ship &ship);

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

// Ghidra 0x00412330 Ship_SelectNearestDisabledShipForBoarding. Finds the
// nearest boardable ship (disabled, unboarded, non-mission, capture_power >
// 0, not allied unless it is a player-dependent) and enters AI state 0x0d
// with it as the primary target. See the implementation comment for the full
// gate chain.
void NovaAi_SelectNearestDisabledShipForBoarding(GameState &state, Ship &ship);

// Ghidra 0x004133F0 Ship_UpdateShipCombatOddsScore.
void NovaAi_UpdateShipCombatOddsScore(GameState &state, Ship &ship);

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
// reconstructed. TODO(decomp(0x00405590)): the state-7 player-controlled
// (pers_def_slot 0x3ff) shareware/licence nag arm and the g_ai_misc_event_flag
// write are not reconstructed.
void NovaAi_UpdateShipState(GameState &state,
                            Ship &ship,
                            std::uint32_t now_ms,
                            float elapsed_ticks = 1.0F);

// Ghidra 0x00408150 Ship_ApplyShipAiControls. Transforms the ship's
// ai_control_mode (written each frame by the state machine) into the concrete
// movement fields the integrator consumes: ai_desired_heading_deg,
// ai_desired_speed, ai_forward_thrust_cmd. This is the bridge that makes the
// AI state machine actually move ships. Also latches ai_fire_trigger_latch for
// the firing path. `elapsed_ticks` is the normalized
// cadence used by the position/velocity creeps (the original's misleadingly
// named _g_avg_frame_time_ms is elapsed milliseconds * 0.03). The mode-4/0xd
// jump/formation bookkeeping reads GameState.tick_60hz directly and stamps
// ai_mode_start_time_ms in those 1/60 s ticks.
// The per-mode turn/thrust polynomials come from the decoded _DAT_00575xxx
// globals. Weapon selection is wired in via the four NovaAi_Select* helpers;
// formation-offset mirroring, predictive-aim-when-bank-live, carrier-bay
// launches and the +0xBD boost latch remain deferred (TODO(decomp)).
void NovaAi_ApplyControls(GameState &state, Ship &ship, float elapsed_ticks);

// Ghidra 0x00401000 Ship_UpdateShipAI. The top-level per-ship AI entry: applies
// the dispatcher's arm selection, recomputes some stat caches, dispatches to
// the behavior supervisor selected by ship.ai_behavior_code, then runs the
// state machine and applies controls.
//
// Deliberate divergence: the original throttles the heavy decision on slow
// machines via the g_ai_update_period stagger (Frame_MeasureFrameTiming
// 0x00432ea0 grades a divisor from the average frame time; a non-idle ship then
// skips both the supervisor and the state machine on frames where
// frame_counter % divisor != instance_id % divisor). The port targets the
// maximum cadence (divisor 1, i.e. every frame) and does not model the
// throttle. The original's second argument, which only bypasses that throttle,
// is therefore not carried either.
void NovaAi_UpdateShipAI(GameState &state,
                         Ship &ship,
                         std::uint32_t now_ms,
                         float elapsed_ticks = 1.0F);

// Ghidra 0x004038b0 Ship_UpdateShipAiBehavior0x03_WarshipCapture. The
// plunder-flavored hostile-behavior variant for factions whose government
// flags_primary carries 0x1000: hunts disabled boardable victims, arbitrates
// capture (0xd) versus attack (4), performs the AI boarding handoff from
// control mode 0xf via Boarding_BoardShipAndTransferCargo, and abandons
// targets it cannot press (depleted ammo / no fireable weapon).
void NovaAi_UpdateBehavior0x03CaptureVariant(GameState &state,
                                             Ship &ship,
                                             std::uint32_t now_ms);

// Ghidra 0x004053c0 Mission_UpdateShipMissionStellarAttackDirective. Selects
// the first hostile destroyable stellar in the ship's current system when the
// ship has a fireable stellar-damage weapon, otherwise falls back to behavior
// 0x03. Dispatched for active mission fleets whose ShipGoal is 2.
void Mission_UpdateShipMissionStellarAttackDirective(GameState &state,
                                                     Ship &ship);

// Ghidra 0x004048a0 Ship_UpdateEscortAI. Per-frame supervisor
// for behavior > 4 ships: releases squads whose leader vanished, arms the
// leader-jump-prep sync into AI state 0x0B (disengage + hold formation while
// the leader charges a jump; inertialess ships detach and travel on their
// own), validates the primary target, decodes the escort command (player
// category table / sub-leader mirror), and runs the command arms (1 assist,
// 2 attack, 3 return-to-hangar, 4 cease fire, 0/default formation). Deferred:
// the government voice override and the pending-latch reset for chatter (the
// chatter consumer pass is TODO(decomp)).
void NovaAi_UpdateEscortAI(GameState &state, Ship &ship, std::uint32_t now_ms);

// Assigns autonomous commands to dependent escorts on the leader's cadence.
// Commands are 0 Formation, 1 Defend, 2 Attack, and 3 Return.
void NovaAi_IssueEscortOrders(GameState &state, Ship &ship);

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
// enter) its cloaking state: not disabled, carries a ModType 17 cloaking
// device (player: owned outfits; NPC: the ship class's default outfit list;
// last match wins), and the device's drain nibbles are satisfiable from
// current resources. The original reads those nibbles from the ModType word
// instead of ModVal; see the BUGFIX(original) note in the definition.
[[nodiscard]] bool NovaAiShip_CanMaintainCloakState(const GameState &state,
                                                    const Ship &ship);

// Ghidra 0x0040f780 Ship_ShouldShipKeepPressingTarget. True when a pursuing
// ship should keep its primary target (or the player under mutual targeting):
// active, not disabled, holding an AI target and a primary target,
// passing the cloak-aware engagement predicate, not coasting through a reversal
// (ai_maneuver_timer_ms <= 0), and in a non-disengage AI state (not in
// {7,9,15,10,11,5,12,18}) -- either directly on the player, on a ship that
// targets the player, or pressed by a third ship that itself holds the player
// or a player-targeting primary. Mirrors the original's literal
// primary_target_ship_slot == ship_instance_id comparison in the third-ship
// scan (instance ids equal slots in this build).
[[nodiscard]] bool NovaAiShip_ShouldKeepPressingTarget(const GameState &state,
                                                       const Ship &ship);

// Ghidra 0x0040fc00 Ship_IsThreatened. True when some
// other active ship is positioned to come to `ship`'s aid: iterates the other
// slots and returns true for the first one in a non-disengage state
// (ai_state_code not in {7,9,15,10,11,5,12}) while `ship`'s primary target
// slot equals its own instance id (a literal port of the original's
// comparison; see the implementation comment).
[[nodiscard]] bool NovaAiShip_IsThreatened(const GameState &state,
                                           const Ship &ship);

// Ghidra 0x0040fca0 / 0x0040fce0 Ship_IsShipAssistingPlayerInAiState0x09/0x0F.
// True when an active, non-disabled ship targets the player in the assist
// states 9 / 0xf (entered from the comm-window Request Assistance / Beg For
// Mercy action). Gates the ship-comm dialog's "on my way" vs "I'm busy"
// prompt pick and re-requesting assistance from a ship already helping.
[[nodiscard]] bool
NovaAiShip_IsShipAssistingPlayerState9(const GameState &state,
                                       const Ship &ship);
[[nodiscard]] bool
NovaAiShip_IsShipAssistingPlayerState0xF(const GameState &state,
                                         const Ship &ship);

// Ghidra 0x004102b0 Ship_IsShipLockedOnAttackerInAiState0x04. True when the
// ship is in AI state 0x04 and holds `attacker`'s instance id as its primary
// target. Gates the distress-response tiers and the shot-hit aggro logic.
[[nodiscard]] bool
NovaAiShip_IsShipLockedOnAttackerInState4(const Ship &ship,
                                          const Ship &attacker);

// Ghidra 0x004124f0 Ship_IsShipLockedOnTarget. True when the ship is in a
// boarding/capture lock (AI state 0x0D, or state 4 with control mode 0x0F) and
// holds `target`'s instance id as its primary target. Shot_SpawnShotFromWeapon
// and Shot_QueueBeamHit use it to mark locked-on shots non-lethal (leave one
// armor point). Confidence: high (57-byte predicate; state/control/offset
// fields verified in disassembly).
[[nodiscard]] bool NovaAiShip_IsShipLockedOnTarget(const Ship &ship,
                                                   const Ship &target);

// Ghidra 0x00410ce0 / 0x00410e80 / 0x00410ec0 / 0x00410ee0 / 0x00410f00 /
// 0x004112a0. Small AI state/control-mode predicates used by the supervisors,
// the escort command dispatch, and the disable-outfit logic:
//   behavior-5 ship in state 5 (deployed fighter returning);
//   state {2,3,0x0B} with control mode 4 or 0x0D (escort hold variants);
//   state 0x08 (arrival slowdown); control mode 0x0C (velocity match);
//   state 4 (combat engagement); state 2 (idle travel staging);
//   state 0x15 (hypergate/wormhole emergence hold).
[[nodiscard]] bool NovaAiShip_IsShipInAiBehavior5State5(const Ship &ship);
[[nodiscard]] bool
NovaAiShip_IsShipInHoldStateWithControlMode4Or0xD(const Ship &ship);
[[nodiscard]] bool NovaAiShip_IsShipInAiState8(const Ship &ship);
[[nodiscard]] bool NovaAiShip_IsShipInAiControlModeC(const Ship &ship);
[[nodiscard]] bool NovaAiShip_IsShipInAiState4(const Ship &ship);
[[nodiscard]] bool NovaAiShip_IsShipInAiState2(const Ship &ship);
[[nodiscard]] bool NovaAiShip_IsShipInAiState0x15(const Ship &ship);

// Ghidra Ship_IsInPlayerSquad (0x0046b8d0). Player-squad membership predicate:
// true for the player (ship_instance_id 0), a ship attached directly to the
// player (squad_leader_ship_slot 0), or a ship attached to a ship that is
// itself attached to the player. Any other leader returns false. Deliberately
// player-specific -- the inner slot is compared to 0 (the player), not to a
// generic root -- so this is not the squad-root walk of
// Ship_ShipsShareSquadRoot (0x0046d190).
[[nodiscard]] bool NovaShip_IsInPlayerSquad(const GameState &state,
                                            const Ship &ship);

// Ghidra 0x00411270 Ship_IsShipInNonIdleAiState. True when the ship's
// ai_state_code is not one of the idle/non-combat states {0 (track-parked), 1
// (docked), 2 (idle-template), 7 (escort-arrive), 0x14 (jump/travel)}.
[[nodiscard]] bool NovaAiShip_IsShipInNonIdleAiState(const Ship &ship);

// Ghidra 0x00410060 Ship_IsAnyShipThreatToPlayerSquad. Scans the NPC
// slots for any ship satisfying NovaTargeting_IsThreatToPlayerSquad.
[[nodiscard]] bool NovaAi_IsAnyShipThreatToPlayerSquad(const GameState &state);

// Ghidra 0x004101d0 Ship_IsEnemyOfShip. Pairwise enemy predicate: distinct,
// both active, `other` not a 0x3ff mission slot, and government relation rules.
// Hostile/xenophobic governments are enemies, same-government ships are not,
// and a xenophobic `other` government admits any non-allied ship.
[[nodiscard]] bool NovaAiShip_IsEnemyOfShip(const GameState &state,
                                            const Ship &ship,
                                            const Ship &other);

// Ghidra 0x004100a0 Ship_IsPlayerThreatenedByEnemyOfShip. True when at least
// one other ship (not `ship`) both keeps pressing its own target and is an
// enemy of `ship`. Used by the comm dialog to route the threat branch.
[[nodiscard]] bool
NovaAiShip_IsPlayerThreatenedByEnemyOfShip(const GameState &state,
                                           const Ship &ship);

// Ghidra 0x00410110 Ship_IsThreatenedByEnemyOfShip. Aggregate threat probe: a
// player-owned `ship` delegates to
// NovaAiShip_IsPlayerThreatenedByEnemyOfShip; otherwise true when `ship` itself
// is still pressing its target, or when some other active ship (excluding
// `ship` and `context_ship`) can acquire `ship` as a target and is an enemy of
// `context_ship`. Used to double perceived combat strength when an allied
// threat is present.
[[nodiscard]] bool NovaAiShip_IsThreatenedByEnemyOfShip(
    const GameState &state, const Ship &ship, const Ship &context_ship);

// Ghidra 0x00411800 Ship_ComputePerceivedCombatStrengthAgainstShip. Combat
// strength = class Strength * shield ratio (FIST truncated toward zero, then
// clamped to [0.25, 1.0]), plus the class Strength * shield ratio of every
// active, non-destroyed squad follower or (for a governmented ship) allied
// ship, doubled when that ally has incoming distress support. A non-positive
// max shield uses the subject's raw shield points for the initial ratio;
// later such candidates keep the previous ratio scratch. The player compares
// its class inherent-combat government instead of a faction. UNINTEGRATED in
// the port: the original calls this from the not-yet-ported government target
// passes of 0x0040e020 (0x0040e995/0x0040ea61/0x0040ec36/0x0040ecd1/
// 0x0040ef37/0x0040f03f); C++ has no gameplay caller yet.
[[nodiscard]] int
NovaAiShip_ComputePerceivedCombatStrength(const GameState &state,
                                          const Ship &ship);

// Ghidra 0x0040e020 Ship_AcquirePrimaryTargetForShip. Full NPC target
// acquisition: retention / pers / mission-fleet early arms, ally join, then
// the near-player + reputation legal-record gate, inherent-combat roll,
// xenophobic aggressive scan, IFF/policy player shield, MaxOdds strength
// filter, squadron-leader Mass preference, nearest-acquirable fallback, and
// the behavior-6 escort re-selection. Only the leading license-seed
// anti-tamper prologue is skipped (see the .cpp).
void NovaAi_AcquirePrimaryTarget(GameState &state, Ship &ship);

// Ghidra 0x00410c30 Ship_EnterShipAiState0x09_TargetPlayerForAssist. Enters AI
// state 0x09 targeting the player, resets hostility/hold-timer/control, and
// sets ai_maneuver_timer_ms to -1 (keeps the state machine live and arms the
// mode-0xf arrival resolution). Called from the comm window when the hail
// target agrees to come to a non-disabled player's aid (combat assist).
void NovaAi_EnterState9TargetPlayerForAssist(Ship &ship);

// Ghidra 0x00410c70 Ship_EnterShipAiState0x0F_TargetPlayerForAssist. Same as
// the 0x09 entry but for a disabled player (state 0x0F, the assist/hover
// approach: the helper velocity-matches and repairs the player back above the
// disable threshold via the mode-0xf within-3px arm in NovaAi_ApplyControls).
void NovaAi_EnterState0FTargetPlayerForAssist(Ship &ship);

// Ghidra 0x00410b00 Ship_EnterShipAiState0x04_TargetRandomUnengagedShip.
// Counts same-system ships that are not disabled, not destroyed, hold
// the player as primary target and are in AI state 0x03/0x04; picks one at
// random, stores it in primary_target_ship_slot and enters ai_state_code
// 0x04 (clears the target to -1 when none).
void NovaAi_EnterState4TargetRandomUnengagedShip(GameState &state, Ship &ship);

// Ghidra 0x004107e0 Ship_EnterShipAiState0x04_TargetRandomCombatCandidate.
// Same as TargetRandomUnengagedShip but the candidate scan additionally
// requires NovaTargeting_IsThreatToPlayerSquad.
void NovaAi_EnterState4TargetRandomCombatCandidate(GameState &state,
                                                   Ship &ship);

// Ghidra 0x00410900
// Ship_EnterShipAiState0x04_TargetRandomRelativeToSquadLeader. Enters AI state
// 0x04 with a random combat target chosen relative to the ship's squad leader
// (squad_leader_ship_slot, +0x9A): the player (slot 0) is admitted only while
// the squad leader still presses its own target, other candidates when the
// squad leader (or the candidate itself, when the squad leader is the player)
// would keep pressing / can acquire them. Clears the primary target without a
// state change when no candidate exists.
void NovaAi_EnterState4TargetRandomRelativeToSquadLeader(GameState &state,
                                                         Ship &ship);

// Ghidra 0x004106b0 Ship_EnterShipAiState0x0B_ClearTargetsSeedHold. Enters AI
// state 0x0B with the primary target cleared; when the station-hold timer is
// not running, seeds the 1.0-tick hold and stamps the 60 Hz mode start.
// Called when a squad leader charges a jump: by Ship_SyncJumpStateToSquad
// (0x00422340) and the leader-jump-prep arm of
// Ship_UpdateEscortAI (0x004048a0).
void NovaAi_EnterStateBClearTargetsSeedHold(Ship &ship, std::uint32_t now_60hz);

// Ghidra 0x00422340 Ship_SyncJumpStateToSquad. During the squad leader's
// jump-engage hold, syncs the leader's hold clock into every active squadmate
// without a stellar attachment: forces the -2 primary-target sentinel and
// enters AI state 0x0B, so squadmates disengage and hold formation until the
// jump fires. The system transfer itself happens at arrival via escort
// adoption (System_RebuildInitialNpcAndMissionPopulation 0x0041af90), not
// here. Slot 0 is skipped by the original scan (the player is never a
// follower).
void NovaAi_SyncJumpStateToSquad(GameState &state,
                                 Ship &leader,
                                 std::uint32_t now_60hz);

// Ghidra 0x00410cb0 Ship_EnterShipAiState0x05_ReturnToSquadLeader. Clears
// the primary target, mirrors squad_leader_ship_slot into the secondary slot,
// and enters AI state 0x05 with control mode 0 (return to the squad leader).
void NovaAi_EnterState5ReturnToSquadLeader(Ship &ship);

// Ghidra 0x00410700 Ship_SetShipHostileToPlayer. Flips the ship hostile: sets
// ai_state_code 0x04, clears the secondary target, targets the player, and
// drops escort control modes 0x04/0x0D while parked in hold states
// 2/3/0x0B (Ship_IsShipInHoldStateWithControlMode4Or0x0D, 0x00410e80). The pers
// announcement arm runs first: a personality ship (pers_def_slot set, no
// mission fleet) whose pers Flags carry 0x10 plays
// Mission_ShowMissionShipAnnouncement once (speaker latched in
// state.mission_speaker_ship_slot, +0xBC hail latch set), unless disabled or
// destroyed or still pressing its previous target. BUGFIX(original): the
// original's destroyed test reads the player slot; the port tests the ship
// being flipped (see the definition).
void NovaAi_SetShipHostileToPlayer(GameState &state, Ship &ship);

// Ghidra 0x0046b260 Ship_CanShipUseAfterburner (DB name
// Ship_IsShipEligibleForEscortOrLaunchBehavior): computes the ShipState +0xBD
// afterburner latch seeded at spawn time. Zero when another active ship lists
// this ship's instance id as its swarm mate (followers stay with the mate)
// or when the class capability flags say never (0x0400 planet-type).
// Capability 0x0040 is always-afterburner; capability 0x0020 rolls against
// the player's combat rating (roll + 0x100 <= rating / class Strength). The
// rating producer is not yet reconstructed, so the roll currently fails
// (TODO(decomp)).
[[nodiscard]] bool NovaShip_CanShipUseAfterburner(GameState &state,
                                                  const Ship &ship);

// Ghidra 0x00423fa0 / 0x00415cb0 shared tail: roll voice_type_mode 0..1, then
// override it from the ship class's inherent_attributes_govt Government voice
// code when both the class government and the voice code are present.
void NovaShip_ApplyInherentGovernmentVoice(GameState &state, Ship &ship);

// Ghidra 0x00402810 Ship_ResetShipAiBehaviorRuntimeFields: resets the core
// per-behavior runtime slots (ai_state_code/ai_control_mode, travel target
// cache, swarm-mate cache, resolved-target slot).
void NovaShip_ResetAiBehaviorRuntimeFields(Ship &ship);

// Ghidra 0x00468920 Ship_CanPlayerHaveMoreEscorts is declared in
// boarding_plunder.hpp (its first reconstruction was the capture flow).

// Ghidra 0x00410d10 Ship_EnterSquadReturnState (with the behavior-5 squad
// sweep of 0x00410cb0 Ship_EnterShipAiState0x05_ReturnToSquadLeader running
// inline): clears the primary target, mirrors squad_leader_ship_slot into the
// secondary slot, and enters AI state 0x0c for ships attached to the player
// (behavior != 5) or 0x0a otherwise; every active behavior-5 (deployed
// fighter) ship whose squad leader is this ship re-enters state 0x05.
void NovaShip_EnterSquadReturnState(GameState &state, Ship &ship);

} // namespace game
