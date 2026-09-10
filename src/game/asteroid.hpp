#pragma once

// Clean-room asteroid / drift-debris pool operations (the r\xf6id resource
// family). The original keeps 16 AsteroidState records in the global
// `g_asteroid_states` and spawns them with Asteroid_SpawnRecord (0x00421e60).
// The pool itself lives on GameState.asteroid_pool (see game_state.hpp); this
// module reconstructs the spawn path (Asteroid_SpawnRecord) plus the asteroid
// allocators that also touch the pool (Asteroid_InitSystem 0x004216B0 and
// Asteroid_Spawn 0x00421830, Steps 3/4) and the per-tick drift simulation
// (Asteroid_UpdateSprites 0x00436910); the drift's SDL sprite bind, frame
// selection and viewport wrap live on SpaceflightView.
//
// NOTE: the records this pool holds are ASTEROID / drift-debris chars (the
// r\xf6id family), NOT NPC ships. Population of real AI ships in a system is a
// separate concern driven by AvgShips + DudeTypes/% Prob (see ship_spawn.cpp
// / System_TickNpcSpawnMaintenance 0x0041d6e0).

#include <cstdint>

#include "game_state.hpp"

namespace game {

// Base sp\x9an resource id for the 16 asteroid / drift-debris sprite sets the
// original loads into DAT_00596c00 (FUN_004ad960 loads spin ids 800..815).
// Record `wander_type` (0..15) indexes it as kAsteroidSpinBase + type: the
// Metal/Ice/Silicates/Metal-rich x Small/Medium/Large/Huge tumble sheets.
inline constexpr std::uint16_t kAsteroidSpinBase = 800;

// Ghidra 0x00421e60 Asteroid_SpawnRecord: finds the first inactive pool
// slot, marks it active, records the asteroid type `type` (a 0-based index
// into ScenarioData.asteroid_defs), copies the spawn position, scatters a
// target velocity, and seeds the wander radius / speed / table value from the
// per-type asteroid row plus the original's fixed random factors. Returns the
// spawned slot index, or -1 when no slot is free.
//
// The original scatters position/velocity and picks the wander radius from the
// sprite descriptor's frame count (+0x54); here the per-type lifetime row
// field stands in for that value (see AsteroidDef.lifetime TODO(decomp)).
[[nodiscard]] int NovaAsteroid_SpawnRecord(GameState &state,
                                           float pos_x,
                                           float pos_y,
                                           std::int16_t type);

// Ghidra 0x00421830 Asteroid_Spawn: allocates one asteroid / drift-debris
// record into a free pool slot for the current system. `place_in_ring == false`
// scatters the target around the player and `true` parks it on an axis-aligned
// ring; the record's wander type is a 0..15 asteroid-type index (r\xf6id
// asteroid type) permitted by the system's ast_types mask. Returns the
// allocated pool slot, or -1 on any no-op/guard condition.
[[nodiscard]] int NovaAsteroid_Spawn(GameState &state, bool place_in_ring);

// Ghidra 0x004216B0 Asteroid_InitSystem: restores the current system's
// asteroid / drift-debris population on spaceflight entry / cross-system
// travel. Spawns `asteroid_count` scattered records, pre-warms all 16 pool
// slots with random wander targets around the player, and sets
// GameState.no_asteroids_latch when the system declares no asteroids.
void NovaAsteroid_InitSystem(GameState &state);

// Ghidra 0x00436910 Asteroid_UpdateSprites (simulation half): advances each
// live asteroid / drift-debris record one tick. Deactivates records whose
// active flag is clear, whose integrity counter (integrity) has
// fallen to the -32000 hidden sentinel, or while the system's no-asteroids
// latch is set; otherwise integrates target_pos by target_vel and advances
// the wander accumulator by wander_speed. The original's sprite-set bind,
// frame selection, distance intensity and viewport wrap run in
// SpaceflightView (WrapAsteroids / DrawAsteroids), which owns the SDL frame
// spans the wrap needs. `elapsed_ticks` is in 30 Hz simulation ticks
// (the original's g_avg_frame_tick_scale).
void NovaAsteroid_UpdateSprites(GameState &state, float elapsed_ticks);

} // namespace game
