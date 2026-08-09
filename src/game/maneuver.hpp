#pragma once

// Clean-room ScriptedManeuverState pool operations. The original keeps 16
// ScriptedManeuverState records in the global `g_scripted_maneuver_state_ptr`
// and spawns them with Frame_SpawnScriptedManeuverState (0x00421e60). The pool
// itself lives on GameState.maneuver_pool (see game_state.hpp); this module
// reconstructs the spawn path (Frame_SpawnScriptedManeuverState) plus the
// asteroid allocators that also touch the pool (System_InitAsteroids
// 0x004216B0 and Dude_SpawnAsteroid 0x00421830, Steps 3/4). The per-tick
// drift (Frame_UpdateScriptedManeuverSprites 0x00436910) remains deferred.
//
// NOTE: the records this pool holds are ASTEROID / drift-debris chars (the
// r\xf6id family), NOT NPC ships. Population of real AI ships in a system is a
// separate concern driven by AvgShips + DudeTypes/% Prob (see ship_spawn.cpp
// / System_TickNpcSpawnMaintenance 0x0041d6e0).

#include "game_state.hpp"

namespace game {

// Mirrors Frame_SpawnScriptedManeuverState (Ghidra 0x00421e60): finds the
// first inactive pool slot, marks it active, records the manoeuvre kind
// `type` (a 0-based manoeuvre-type index into ScenarioData.maneuver_types),
// copies the spawn position, scatters a target velocity, and seeds the
// wander radius / speed / table value from the per-type manoeuvre row plus the
// original's fixed random factors. Returns the spawned slot index, or -1 when
// no slot is free.
//
// The original scatters position/velocity and picks the wander radius from the
// sprite descriptor's frame count (+0x54); here the per-type lifetime row
// field stands in for that value (see ManeuverTypeDef.lifetime TODO(decomp)).
[[nodiscard]] int NovaManeuver_SpawnState(GameState &state,
                                          float pos_x,
                                          float pos_y,
                                          std::int16_t type);

// Mirrors Dude_SpawnAsteroid (Ghidra 0x00421830): allocates one asteroid /
// drift-debris manoeuvre record into a free pool slot for the current system.
// `place_in_ring == false` scatters the target around the player and `true`
// parks it on an axis-aligned ring; the record's wander type is a 0..15
// manoeuvre-type index (r\xf6id asteroid type) permitted by the system's
// ast_types mask. Returns the allocated pool slot, or -1 on any
// no-op/guard condition.
[[nodiscard]] int NovaDude_SpawnAsteroid(GameState &state, bool place_in_ring);

// Mirrors System_InitAsteroids (Ghidra 0x004216B0): restores the current
// system's asteroid / drift-debris population on spaceflight entry /
// cross-system travel. Spawns `asteroid_count` scattered records, pre-warms
// all 16 pool slots with random wander targets around the player, and sets
// GameState.no_roaming_ships_latch when the system declares no asteroids.
void NovaSystem_InitAsteroids(GameState &state);

} // namespace game
