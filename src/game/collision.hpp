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
// Deferred scope: stellar/asteroid contact branches, sprite pixel masks (the
// clean-room uses explicit circle envelopes on Ship/ActiveShot), impact
// particle bursts, linked shots, kill chatter, and the mission disable
// bookkeeping inside the ship-hit path.

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
// driver: resolves direct shot-vs-ship contacts (circle overlap stand-in for
// the original's bounding-circle/pixel-mask tests) and dispatches
// Shot_ResolveShotCollisionHit with linked shots disallowed.
void NovaWeapon_ResolveDirectShotCollisions(GameState &state);

// Ghidra Shot_ResolveCollisions (0x00437e20): the blast-proximity pass over
// live shots. Resolves at most one proximity ship hit per shot when the
// weapon has a blast radius; the stellar and asteroid contact branches remain
// deferred (TODO(decomp)).
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
