#pragma once

// Clean-room port of the original collision pipeline. The original splits
// weapon contact into two independent passes:
//   - Direct sprite-contact hits arrive through the sprite-layer overlap
//     callback Ship_HandleSpritePairCollision (0x004374f0), which validates
//     Weapon_CanWeaponHitTarget (0x00426ef0), applies a bounding-circle or
//     pixel-mask overlap test, rejects shots inside their late-collision
//     window, and dispatches Shot_ResolveShotCollisionHit (0x00437780).
//   - Shot_ResolveCollisions (0x00437e20) is the separate blast-proximity
//     pass: stellar-damage contacts (weapons with flags_secondary 0x400),
//     blast-radius proximity hits against ships (radius = blast_radius +
//     ship sprite half-span / 3), and asteroid splash.
// Both passes funnel into Shot_ResolveShotCollisionHit -> Shot_ResolveShip-
// HitFromWeapon (0x004192d0) for damage, impulse, and aggro.
//
//   - Asteroid_HandleSpritePairCollision (0x00436f70) is the parallel sprite
//     callback on g_asteroid_sprite_layer: the same shot containers are tested
//     against the 16 asteroid sprites, weapons with flags_quaternary 0x0001
//     (Seeker "passes over asteroids") are rejected, and a pixel-mask overlap
//     funnels through NovaUi_ResolveWeaponSplashImpact (0x00436ff0).
//
// Deferred scope: stellar contact branch, impact particle bursts, linked
// shots, kill chatter, and the mission disable bookkeeping inside the ship-hit
// path. Direct shot-vs-ship and shot-vs-asteroid contacts now test the decoded
// sprite pixel masks (Sprite_TestPixelMaskOverlap 0x00475c80) with the original
// bounding-circle fallback when a mask is unavailable.

#include "game_state.hpp"

namespace game {

// Ghidra Weapon_CanWeaponHitTarget (0x00426ef0). Engagement gates: valid
// ownership/system, active and non-destroyed target, mode-1 target matching,
// self/faction/leader/stellar-target exclusions, player-escort and booty-flag
// exclusions for player-aligned owners, scripted-maneuver exclusion, and the
// ship-class capability_flags 0x400 vs weapon flags_secondary 0x400 match.
[[nodiscard]] bool NovaWeapon_CanProjectileHitShip(const GameState &state,
                                                   const ActiveShot &shot,
                                                   std::int16_t target_slot);

// Ghidra Shot_ResolveShipHitFromWeapon (0x004192d0) core: shield-first/
// armor damage, disable-transition arms, aggro/hostility response. Exported
// for the hull-destruction blast in Ship_UpdateVisualState 0x00428340.
void ResolveShipHitFromWeapon(GameState &state,
                              std::int16_t target_slot,
                              Ship &target,
                              float impact_x,
                              float impact_y,
                              std::int16_t impact_impulse,
                              std::int16_t armor_damage,
                              std::int16_t shield_damage,
                              std::int16_t attacker_ship_slot,
                              bool allow_aggro_updates,
                              bool suppress_retarget_logic,
                              bool force_armor_only,
                              bool bypass_shields,
                              std::int16_t player_aggro_delta,
                              bool check_fire_restriction_transition = false);

// Ghidra Shot_ResolveShipHitFromWeapon (0x004192d0) wrapper: resolve a hit
// against a ship given by slot (validates the slot / active state).
void NovaCollision_ResolveShipHitFromWeaponSlot(
    GameState &state,
    std::int16_t target_slot,
    float impact_x,
    float impact_y,
    std::int16_t impact_impulse,
    std::int16_t armor_damage,
    std::int16_t shield_damage,
    std::int16_t attacker_ship_slot,
    bool allow_aggro_updates,
    bool suppress_retarget_logic,
    bool force_armor_only,
    bool bypass_shields,
    std::int16_t player_aggro_delta);

// Ghidra Ship_HandleSpritePairCollision (0x004374f0) plus its sprite-layer
// driver: resolves direct shot-vs-ship contacts with the original's bounding-
// circle / pixel-mask overlap and dispatches Shot_ResolveShotCollisionHit with
// linked shots disallowed. Also runs the Asteroid_HandleSpritePairCollision
// (0x00436f70) contact for each shot that did not hit a ship.
void NovaWeapon_ResolveDirectShotCollisions(GameState &state);

// Public test seam the direct-contact pass uses: resolves each live entity's
// current-frame pixel mask from the non-SDL SpriteMaskStore (ships via the
// class sh\x8an base sheet, shots via weapon sprite+3000, asteroids via
// kAsteroidSpinBase+type) using the same frame selection as the renderer.
void NovaCollision_RefreshCollisionMasks(GameState &state);

// Ghidra Shot_ResolveCollisions (0x00437e20): the blast-proximity pass over
// live shots. Resolves at most one proximity ship hit per shot when the
// weapon has a blast radius, then scans the asteroid pool (weapons with
// flags_quaternary bit 0 clear) into NovaUi_ResolveWeaponSplashImpact; the
// stellar contact branch remains deferred (TODO(decomp)).
void NovaWeapon_ResolveProjectileCollisions(GameState &state);

// Ghidra Shot_QueueBeamHit / Shot_UpdateBeamHitQueue (0x00427a90/0x0042f270)
// impact leg, exposed for the instantaneous beam queue. Applies one direct
// weapon impact using the same shield/armor, ionization, impulse, and aggro
// path as projectiles.
void NovaWeapon_ResolveDirectWeaponHit(GameState &state,
                                       std::int16_t owner_ship_slot,
                                       std::int16_t target_ship_slot,
                                       std::int16_t weapon_id,
                                       std::int8_t impact_variant = 0);

} // namespace game
