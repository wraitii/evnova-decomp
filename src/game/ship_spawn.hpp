#pragma once

// Clean-room NPC ship lifecycle primitives. The original keeps all ships in the
// single `g_ship_states` array (64 slots; index 0 = the player) and allocates +
// resets a slot via Ship_AllocateShipSlotInSystem (0x004254b0), then the
// higher-level spawners (Dude_SpawnShipFromDudeDefInSystem,
// EncounterFleet_SpawnShipFromFleetDef, Mission_SpawnShipFrom*) lay a loaded
// ShipClassDef/DudeDef/fleet-def on top of it.
//
// This module reconstructs the allocation primitive against GameState.ships_.
// The deep combat/AI residual fields the original zero-resets here are not yet
// modelled (they only carry weight once the ship AI/combat systems are
// reconstructed); Ship's defaults already match the net zero/-1 reset, and the
// intentional omissions are logged as TODO(decomp) in ship_spawn.cpp.

#include "game_state.hpp"

namespace game {

// Ghidra 0x004254b0 Ship_AllocateShipSlotInSystem: finds the first inactive
// ship slot in [1, kMaxShips - reserved_tail), marks it active in system_id and
// resets it to baseline defaults. Returns the allocated slot or -1 when none is
// free (or the reserved tail consumes the whole array).
[[nodiscard]] int NovaShip_AllocateShipSlot(GameState &state,
                                            std::int16_t system_id,
                                            std::int16_t reserved_tail);

// Mirrors EncounterFleet_SelectRandomEncounterFleetDefWeighted (Ghidra
// 0x0046b6d0): weighted-random selection among a system's bound random-
// encounter fleet defs. Guards on System.encounter_fleet_count; builds a
// candidate list from the System.encounter_fleet_ids whose FleetDef has a
// valid lead ship (lead_ship_class_id != -1) AND is_available_runtime set,
// accumulating System.encounter_fleet_weights into a cumulative-weight bucket;
// then draws a uniform value in [0, total_weight) and returns the fleet id
// whose cumulative bucket is first reached (heavier-weighted defs are
// likelier). Returns -1 when there are no eligible candidates (or no total
// weight). The returned value is a 0-based fleet-def index into
// ScenarioData.fleets (i.e. resource id minus 0x80), matching the parameter
// conventions of NovaEncounter_SpawnFleetLeadShip.
[[nodiscard]] int NovaEncounter_SelectFleetDefWeighted(
    const System &system, const ScenarioData &scenario, std::mt19937 &rng);

// Mirrors Dude_SelectRandomSystemDudeClassIndex (Ghidra 0x0046b600): weighted-
// random pick among a system's eight bound dude slots. Builds a cumulative-
// weight bucket over the slots whose System.dude_class_ids entry is a valid
// class id (0..0x1ff), draws a uniform value in [0, total_weight), and returns
// the lowest-index slot whose cumulative bucket first reaches the (draw+1)
// threshold (heavier-weighted slots are likelier). Returns -1 when no slot has
// a valid non-zero weight. The returned value is a 0-based dude slot index
// (0-7), i.e. an index into System.dude_class_ids / dude_class_weights; the
// caller resolves that to the actuate dude-class id. This is the clean-room
// analogue of the already-restored fleet weighted-select
// (NovaEncounter_SelectFleetDefWeighted) and feeds
// EncounterFleet_SpawnRandomSystemDudeShip (0x0041ba80, deferred).
[[nodiscard]] int
NovaDude_SelectRandomSystemDudeClassIndex(const System &system,
                                          std::mt19937 &rng);

// Mirrors EncounterFleet_TrySpawnRandomEncounterFleet (Ghidra 0x00425280):
// scans the ScenarioData.fleets table, marking each def that may spawn in
// system_id based on its spawn_system_filter and current availability, then
// with per-eligible-def odds spawns one eligible def via
// NovaEncounter_SpawnFleetLeadShip. Returns the spawned ship slot, or -1 when
// no eligible def was picked (or none was available).
//
// Filter decode (0-based def): -1 -> anywhere; 0x80..9999 -> that exact system
// id; 10000..14999 -> system government == (filter-10000); 15000..19999 ->
// system govt allied to (filter-15000); 20000..24999 -> system govt !=
// (filter-20000); 25000..29999 -> system govt hostile/xenophobic to
// (filter-25000). Only defs with a valid lead (lead_ship_class_id != -1) and
// is_available_runtime set are candidates. Selection draws uniformly over the
// full 0x100-def space and only spawns when the drawn slot is marked eligible
// (the original's effective per-eligible-def odds, lower when few defs fit).
//
// ignore_ship_availability is forwarded to the fleet spawner and gates its
// arrival overlay banner; it does not bypass the fleet or escort availability
// cache checks.
[[nodiscard]] int NovaEncounter_TrySpawnRandomFleet(
    GameState &state, std::int16_t system_id, bool ignore_ship_availability);

// Mirrors Dude_SelectShipTypeIndexFromDudeDef (Ghidra 0x0046b4b0): weighted-
// random pick of one ship slot (0..15) from a DudeDef's ship_types /
// ship_probabilities tables, skipping entries whose ShipClass runtime
// availability bit is clear. ignore_ship_availability lifts that filter (the
// caller accepts any listed class). Returns the chosen slot index or -1 when
// no valid entry is selectable.
[[nodiscard]] int NovaDude_SelectShipTypeIndex(const DudeDef &dude,
                                               const ScenarioData &scenario,
                                               bool ignore_ship_availability,
                                               std::mt19937 &rng);

// Reimplementation of EncounterFleet_SpawnRandomEncounterFleet
// (Ghidra 0x004259b0). Allocates and shapes the lead, rolls all four escort
// ranges, skips escorts whose cached ShipClass availability is clear, copies
// stock loadouts, seeds optional cargo, links escorts to the lead, and applies
// the original state-0x08/state-0x15 arrival placement. Returns the lead slot,
// or -1 when the fleet is absent/unavailable or its lead cannot be allocated.
// The ignore_ship_availability argument in the original is presentation-only
// for this function (it gates the arrival banner); the fleet and escort cache
// checks remain authoritative here.
[[nodiscard]] int
NovaEncounter_SpawnFleetLeadShip(GameState &state,
                                 std::int16_t system_id,
                                 std::int16_t fleet_def_index,
                                 std::int16_t ai_behavior_code = -1,
                                 bool ignore_ship_availability = false);

// Ghidra 0x0043A020 System_UpdateRandomEncounterCountdown. Advances the
// current system's armed reinforcement timer and spawns its fleet on expiry.
void NovaSystem_UpdateReinforcementCountdown(GameState &state,
                                             float elapsed_ticks);

// Mirrors EncounterFleet_SpawnRandomSystemDudeShip (Ghidra 0x0041ba80):
// spawns a single random system-bound NPC (dude) ship. Scans the first inactive
// ship slot, picks a random dude class bound to the system
// (NovaDude_SelectRandomSystemDudeClassIndex), then a weighted ship type from
// that dude def (NovaDude_SelectShipTypeIndex), and lays it onto the slot:
// ship class, government, AI behavior (dude ai_type or the class default when
// < 1), dude_class_id, base shield/armor/fuel, a heading/radial and a position
// (at the first stellar for ai_behavior 3 speed-locked ships, else random
// [-750,750) scatter). Returns the allocated slot, or -1 when no dude/ship is
// selectable or no slot is free.
//
// The selected class's complete stock weapon-bank loadout is copied onto the
// NPC. Deferred (see ship_spawn.cpp): remaining deep combat/AI residual fields
// (jamming, combat_state, voice_type, escort-eligibility). The NPC is left at
// a visible heading/position (ai_state_code 0) so it renders without the
// AI-state entry.
[[nodiscard]] int NovaEncounter_SpawnRandomSystemDudeShip(
    GameState &state, std::int16_t system_id, std::uint16_t reserved_slots);

// Ghidra 0x004235c0 Pers_SpawnShipFromPersDef. Spawns an NPC ship from a
// përs personality (ScenarioData.pers_defs): builds the eligibility set for
// system_id (present + AI type > 0 + ActiveOn satisfied, LinkSyst filter match
// incl. the 10000/15000/20000/25000 government codes, and — when
// exclude_derelict_govts is set — non-derelict government), drops candidates
// already active in the ship table (same-name dedup via display-name id), then
// picks a random eligible slot (or the forced one; forcing also marks the def
// present) and lays the personality onto a freshly allocated slot: identity,
// per-weapon count/ammo deltas over the class stock loadout, booty credits,
// shield/armor scale, afterburner latch, LinkMission resolution, and the
// derelict-wreck kinematics. Returns the spawned slot or -1.
[[nodiscard]] int NovaPers_SpawnShipFromPersDef(GameState &state,
                                                std::int16_t system_id,
                                                bool exclude_derelict_govts,
                                                std::int16_t forced_pers_slot);

// Mirrors Dude_SpawnRandomDudeShipInSystem (Ghidra 0x0041c710): the
// high-level random wandering NPC spawn dispatcher. Rolls 1-in-7 for a përs
// personality/linked-mission ship, else 1-in-7 for a random-encounter fleet,
// else spawns a random system dude ship (discarded when its computed fuel
// capacity < 1), then positions the spawned ship at a random polar offset from
// system centre and faces it toward the origin. Returns the spawned ship slot
// or -1.
//
// The original's "mission ship" arm is the përs call above; there is no
// separate active-mission fleet arm in this dispatcher. Successful single
// ships share the slowdown / restricted-stellar jump-in placement tail.
[[nodiscard]] int NovaDude_SpawnRandomDudeShipInSystem(GameState &state,
                                                       std::int16_t system_id);

// Reconstructs the initial ambient-population slice at the end of
// System_RebuildInitialNpcAndMissionPopulation (Ghidra 0x0041af90). On
// system/stellar entry it performs System.avg_ships spawn attempts immediately.
// Ordinary dude ships
// use EncounterFleet_SpawnRandomSystemDudeShip directly, retaining its
// [-750,750) scatter, and receive their class base velocity along their random
// heading. Encounter and mission/personality rolls are kept in their original
// 1-in-7 order; the latter is the përs spawner.
void NovaSystem_PopulateInitialNpcShips(GameState &state,
                                        std::int16_t system_id);

// Mirrors System_TickNpcSpawnMaintenance (Ghidra 0x0041d6e0). Per-tick NPC
// population maintenance, in the original's order:
//
//  1. Mission-fleet stepper over the 16 active-mission slots:
//     * aux-fleet arm: when the acceptance roll clock (+0x69) has expired and
//       the aux dude def is set, top the aux fleet up to its remaining
//       budget (mission_ship_count_active - mission_fleet_metric_c) when the
//       mission's spawn locator matches this system; flags 0x0010 fleets
//       never drain their budget (indefinite respawns).
//     * main-fleet arm: missions whose spawn system matches (or -6 follow)
//       count down the spawn/rearm timer (+0x4b) and, when it expires,
//       respawn the fleet toward its target count (Bible ShipStart 1
//       keeps the alive count at target via goal_count_remaining). Ships
//       arrive on a ±256 scatter around the shared ~2100-unit polar radius,
//       oriented on the bearing toward the player's previous system.
//     * flags 0x0001 missions resolve right after their fleet spawns.
//  2. Ambience: while the active ambient count lies below System.avg_ships,
//     roll a 1-in-500 encounter pick gated by encounter_chance_percent: on a
//     hit select a weighted encounter-fleet def
//     (NovaEncounter_SelectFleetDefWeighted) and spawn its lead
//     (NovaEncounter_SpawnFleetLeadShip), otherwise spawn a random
//     system-bound dude ship (NovaDude_SpawnRandomDudeShipInSystem).
//  3. Stellar defense-fleet trickle: scan the 16 nav stellars for the first
//     with field_0x47 set and a positive present_ship_count whose live
//     defenders number below max_ship_count % 10; spawn one
//     (NovaStellar_SpawnDefenseFleetShip) and decrement the budget. At most
//     one defense spawn per tick.
//
// Deferred (TODO(decomp), see ship_spawn.cpp): the roaming-NPC population cap,
// the <200-traffic ambient-fleet escalation and the ambient-mission-ship
// respawn latch.
void NovaSystem_TickNpcSpawnMaintenance(GameState &state,
                                        std::int16_t system_id,
                                        std::uint32_t now_ms);

// Ghidra 0x00422400 ShipClass_SpawnEscortShipFromClass. Allocates a ship slot
// in the player's current system (reserved tail 8) and populates it as a
// player fleet escort: identity from the zero-based ship class id, AI
// behavior 6 attached to the player (squad_leader_ship_slot 0), base shield /
// armor, afterburner + mining-scoop latches, hired-escort origin mark
// (+0xBB), no mission linkage, jamming caches cold, stock weapon banks, then
// AI runtime reset and the leader-return AI entry. `spawn_stellar_id` -1
// places it at the player with a random polar drift velocity; otherwise at
// that stellar's map position. Used by the shipyard hire path and saved-escort
// restore. Returns the allocated slot, or -1 when no slot is free.
[[nodiscard]] int
NovaShipClass_SpawnEscortShipFromClass(GameState &state,
                                       std::int16_t ship_class_id,
                                       std::int16_t spawn_stellar_id);

// Mirrors Dude_SpawnShipFromDudeDefInSystem (Ghidra 0x0041c9f0): allocates one
// ship slot in system_id (reserving slot_pool tail slots), picks a weighted
// ship type from the dude def and lays on identity, government, AI behavior
// (dude ai_type or the class default when < 1) and base shield/armor. Returns
// the slot or -1. The original also copies the class's per-weapon ammo tables
// into the ship state here; the clean-room builds them lazily on the first
// AI/fire tick (NovaWeapon_EnsureNpcWeaponBanks).
[[nodiscard]] int
NovaDude_SpawnShipFromDudeDefInSystem(GameState &state,
                                      std::int16_t dude_def_index,
                                      std::int16_t system_id,
                                      std::int16_t slot_pool,
                                      bool ignore_ship_availability);

// Ghidra 0x00421fd0 Stellar_SpawnDefenseFleetShip. Spawns one ship for a
// stellar's Bible defense fleet (spöb DefenseDude): allocates via
// NovaDude_SpawnShipFromDudeDefInSystem (slot pool 2), stamps it as the
// stellar's defender (Ship.defense_fleet_home_stellar_id), forces behavior-3
// warship, seeds it at the stellar's map position with a random heading and an
// initial velocity at the effective max speed, makes it hostile to the player,
// and latches the stellar's field_0x47 so the per-tick defense trickle in
// NovaSystem_TickNpcSpawnMaintenance can replace losses. `stellar_id` is the
// stellar resource id; returns the slot or -1.
[[nodiscard]] int NovaStellar_SpawnDefenseFleetShip(GameState &state,
                                                    std::int16_t stellar_id);

// Ghidra 0x0041cf40 Mission_SpawnMissionShipFromDudeDef. Spawns one mission-
// fleet ship for active-mission slot mission_fleet_slot from its dude def:
// allocates a slot (reserve 8), locks the fleet's special-ship-type index when
// the caller forces a class for a single-ship 0x800-flagged fleet, then lays
// on identity (mission_fleet_slot + dude_class_id), government, AI behavior,
// class vitals, skill variance and the waypoint marker. Fleets with behavior
// 0 (attack) spawn hostile to the player. Returns the slot or -1.
[[nodiscard]] int
NovaMission_SpawnMissionShipFromDudeDef(GameState &state,
                                        std::int16_t dude_class_id,
                                        std::int16_t forced_ship_class_id,
                                        std::int16_t spawn_system_id,
                                        std::int16_t mission_fleet_slot);

// Ghidra mission-fleet restore slice of System_RebuildInitialNpcAndMission-
// Population (0x0041af90, runs between Ship_DeactivateVacantShipsAndTally and
// the ambient avg_ships spawns on system/landing entry). For every active
// mission whose spawn system matches system_id (or follows the player, -6)
// and whose spawn/rearm timer is idle, respawns the mission fleet via
// NovaMission_SpawnMissionShipFromDudeDef, applying the spawn-behavior
// placement arms (3 = center scatter, 5 = derelict wreck, negative
// ShipStart = arrive at a linked system's nav point, 2 = cloak) and the
// escort-behavior link. Missions flagged 0x0001 resolve (auto-abort) right
// after their fleet is placed. copy_player_heading mirrors the original's
// flag==1 callers (not wired yet; all current call sites pass false).
void NovaSystem_RestoreMissionFleets(GameState &state,
                                     std::int16_t system_id,
                                     bool copy_player_heading,
                                     std::uint32_t now_ms);

// Mirrors the ship-slot cleanup of Ship_DeactivateVacantShipsAndTally
// (Ghidra 0x0041ad50): scans every NPC slot and deactivates the "vacant" ones,
// tallying them into their spawn-quota bucket first. A slot is SPARED only when
// it is actively engaging the player -- ai_behavior_code > 4,
// squad_leader_ship_slot
// == 0, not docked at a stellar (defense_fleet_home_stellar_id == -1), not in a
// mission fleet -- AND is not disabled AND `keep_player_engaged` is
// false (the original's flag==0). Everything else (idle wanderers/dudes,
// parked-at-stellar ships, mission ships, disabled ships) is
// deactivated: parked ships increment their stellar's present_ship_count
// (capped at max_ship_count), mission ships would increment their mission
// fleet's current-ship count (mission fleets not reconstructed; logged no-op),
// then is_active and the targeting/mission/system slots are cleared.
//
// The original runs this with flag==0 on travel/landing arrival
// (Stellar_HandleStellarEntryAndExit 0x00457580) and system entry
// (NovaMainLoop_Run 0x00486880);
// System_RebuildInitialNpcAndMissionPopulation then immediately makes
// System.avg_ships spawn attempts through its scattered initial-population
// path. Per-tick maintenance only replenishes later losses. Note the original
// scans ALL slots regardless of system; the clean-room's ships only
// ever live in the current system, so this matches in practice.
void NovaShip_DeactivateVacantShipsAndTally(GameState &state,
                                            bool keep_player_engaged);

// Mirrors NovaRandom_Reseed (Ghidra 0x004ab970 -> NovaRandom_Range(0)), which
// mixes NovaTime_GetTickCount60Hz() into the global LCG. The original calls
// this once at game-session bootstrap (NovaGameSession_Run 0x00416100) so each
// session's NovaRandom draws differ. The clean-room GameState keeps its own
// mt19937 in `state.rng` (seeded 42 by default); this reseeds it with fresh
// entropy so the new-game flow spawns a different,
// time-varying set of ships/positions instead of the deterministic pause.
void NovaGame_ReseedRandom(GameState &state);

// Ghidra 0x0041e640 Weapon_SpawnShipFromCarrierBayWeapon: allocate and
// populate one carried fighter launched from `launcher`'s carrier-bay weapon
// bank. The fighter is a behavior-5 ship attached to the carrier via
// squad_leader_ship_slot, spawned at the carrier's position/heading/velocity,
// class = bay weapon ammo_type - 0x80, with launch spread (weapon
// Inaccuracy10) and launch velocity (weapon Speed_a/100 clamped per-axis to
// the fighter's effective max speed). Mirrors the sibling escort-command and
// squad bookkeeping, then seeds the stock 8-bank loadout. Divergence: the
// original returns a plain bool in AL (true = spawned; XOR AL,AL /
// MOV AL,1 rets, verified 0x0041e674/0x0041f321) and discards the slot;
// returning the slot is a deliberate clean-room superset. Returns -1 on a
// bad bank / no free slot.
[[nodiscard]] int NovaWeapon_SpawnShipFromCarrierBayWeapon(
    GameState &state, const Ship &launcher, std::int16_t weapon_bank);

// Ghidra 0x0040d9a0 Ship_LaunchShipFromCarrierBay: per-frame launch driver.
// Requires an active primary target in-system, then scans the banks for the
// first loaded mode-99 bay weapon (mounted ammo > 0, loaded secondary > 0,
// NPC-mount gate Flags2 0x100 clear) whose cooldown has expired, makes it the
// active bank, spawns the fighter, queues the fire sound, sets the bank
// cooldown to reload / MOUNTED ammo (original quirk -- the divisor is the
// mounted count, not the loaded secondary), decrements the secondary, and
// applies the shared-cooldown arm (Flags3 0x20: every other bank's cooldown
// rises to new_cooldown + 2.0 (DAT_00575040) when smaller). Returns true when
// a fighter launched.
bool NovaShip_LaunchShipFromCarrierBay(GameState &state, Ship &launcher);

// Ghidra 0x00415ea0 Ship_LaunchCarriedShipFromBay (bay-RECOVERY arm; called
// when a carried fighter reaches its carrier in AI control mode 8): finds the
// carrier's mode-99 bay weapon matching the fighter's class id (fallback:
// the class's base-sprite clone-source id), tops the bay back up (+1 loaded
// secondary, priming the bank cooldown to reload when it was empty), and
// deactivates the fighter with its squad/AI linkage cleared. The original's
// g_shipAvailabilityCachesDirty set on a player carrier stays deferred
// (TODO(decomp)).
void NovaShip_RecoverCarriedShipToBay(GameState &state, Ship &fighter);

// Ghidra 0x004694a0 ShipClass_HasPlayerBayCapacityFor: whether the player
// can capture/retain another ship of the given (zero-based) class. With both
// outfit_slot and weapon_bank == -1 the holding is resolved: the first armed
// mode-99 bank whose class (or base-sprite clone family) matches the class,
// then the ModType-3 fighter-bay outfit bound to that bank; no bay/outfit ->
// false. Occupancy = the bay's loaded secondary count (or the outfit's owned
// count when both slots are explicit); capacity = weapon MaxAmmo x mounted
// count, or the outfit Max field when MaxAmmo is 0/-1. Also counts active
// behavior-5 fighters of this class with squad leader 0 and no mission fleet.
// Returns occupancy_count < capacity.
[[nodiscard]] bool
NovaShipClass_HasPlayerBayCapacityFor(GameState &state,
                                      std::int16_t ship_class_id,
                                      std::int16_t outfit_slot = -1,
                                      std::int16_t weapon_bank = -1);

} // namespace game
