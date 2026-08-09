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

// Mirrors Ship_AllocateShipSlotInSystem (0x004254b0): finds the first inactive
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
// TODO(decomp) ignore_ship_availability: the original forwards this to
// EncounterFleet_SpawnRandomEncounterFleet to gate the arrival overlay banner;
// our lead-only spawner does not model the banner yet, so the flag is accepted
// for signature parity but not yet acted on.
[[nodiscard]] int NovaEncounter_TrySpawnRandomFleet(
    GameState &state, std::int16_t system_id, bool ignore_ship_availability);

// PARTIAL reconstruction of the lead-ship spawn of
// EncounterFleet_SpawnRandomEncounterFleet (Ghidra 0x004259b0). Allocates one
// ship slot in system_id for the random-encounter fleet template at
// fleet_def_index (0-based index into ScenarioData.fleets), applying the def's
// lead ship class (if any) and identity: zero-based ship_class_id, government,
// AI behavior
// (or ship-class default when the requested code is -1), base shield/armor,
// mission slots cleared, mining-scoop flag, zero credits. Returns the allocated
// slot
// or -1 when the def has no lead ship, is unavailable at spawn time
// (is_available_runtime clear), or no slot is free.
//
// TODO(decomp) deferred (see ship_spawn.cpp): the original also positions the
// lead (spin-out at a random polar offset via AI state 0x08, or jump-in at an
// adjacent stellar via AI state 0x15), seeds random cargo for carry_cargo_flag
// fleets, copies the 8-bank weapon loadout, and spawns/links the escorts + the
// arrival overlay banner. Those are not yet reconstructed (they sit on the
// deferred AI-state / DudeDef / weapon-bank code); this function leaves the
// ship at a neutral origin heading (ai_state_code 0) so it is visible and
// positioned for rendering but not yet animated or armed.
[[nodiscard]] int NovaEncounter_SpawnFleetLeadShip(
    GameState &state, std::int16_t system_id, std::int16_t fleet_def_index);

} // namespace game
