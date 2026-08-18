#pragma once

// Clean-room first slice of the Ghidra projectile collision pipeline:
//   Weapon_CanWeaponHitTarget       0x00426ef0
//   Ship_HandleSpritePairCollision  0x004374f0
//   Shot_ResolveShotCollisionHit    0x00437780
//   Shot_ResolveShipHitFromWeapon   0x004192d0
//   Shot_ResolveCollisions           0x00437e20
//
// This module intentionally stops at direct projectile-vs-ship impacts. The
// original uses SpriteLayer AABB callbacks plus bounding-circle/pixel-mask
// tests; the clean-room model uses explicit circle envelopes on Ship and
// ActiveShot until sprite geometry is available to the simulation.

#include "game_state.hpp"

namespace game {

// Ghidra Weapon_CanWeaponHitTarget (0x00426ef0). Applies the subset of the
// engagement gates that are represented by the current clean-room state:
// valid ownership/system, active and non-destroyed target, mode-1 target
// matching, self/faction/leader/stellar-target exclusions, scripted
// invulnerability, mission-special exclusion, and the planet-type capability
// match.
[[nodiscard]] bool NovaWeapon_CanProjectileHitShip(const GameState &state,
                                                   const ActiveShot &shot,
                                                   std::int16_t target_slot);

// Ghidra Shot_ResolveCollisions (0x00437e20), limited to active projectile
// shots and ship candidates. Resolves at most one direct hit per shot, applies
// shield/armor damage, records the minimal aggro transition, and consumes the
// shot. Splash, proximity fuses, asteroids, stellars, and freeflight objects
// remain deferred.
void NovaWeapon_ResolveProjectileCollisions(GameState &state);

// Ghidra Shot_ResolveShipHitFromWeapon (0x004192d0), exposed for the
// instantaneous beam queue. Applies one direct weapon impact using the same
// shield/armor, ionization, impulse, and minimal aggro path as projectiles.
void NovaWeapon_ResolveDirectWeaponHit(GameState &state,
                                       std::int16_t owner_ship_slot,
                                       std::int16_t target_ship_slot,
                                       std::int16_t weapon_id,
                                       std::int8_t impact_variant = 0);

} // namespace game
