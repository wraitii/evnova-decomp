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
//     ship sprite full frame width * 0.333), and asteroid splash.
// Both passes funnel into Shot_ResolveShotCollisionHit -> Shot_ResolveShip-
// HitFromWeapon (0x004192d0) for damage, impulse, and aggro.
//
//   - Asteroid_HandleSpritePairCollision (0x00436f70) is the parallel sprite
//     callback on g_asteroid_sprite_layer: the same shot containers are tested
//     against the 16 asteroid sprites, weapons with flags_quaternary 0x0001
//     (Seeker "passes over asteroids") are rejected, and a pixel-mask overlap
//     funnels through NovaUi_ResolveWeaponSplashImpact (0x00436ff0).
//
//   - Stellar_HandleShipStellarCrash (0x0043aed0) is the physical fatal-
//     stellar pass: availability_flags 0x100 bodies pixel-mask-overlap ships
//     and instant-kill non-immune hulls (see
//     NovaStellar_HandleShipStellarCrash).
//
// Deferred scope: kill chatter, the player-owned stellar faction-combat
// events, and the mission disable bookkeeping inside the ship-hit path. The
// linked-shot expiry callsite in Shot_HandleShot (0x00435830) is now wired.
// Weapon impact SWParticle bursts are emitted by the
// ported NovaEffects_SpawnWeaponImpactBurstForWeapon. Direct shot-vs-ship,
// shot-vs-asteroid and shot/ship-vs-stellar contacts test the decoded sprite
// pixel masks (Sprite_TestPixelMaskOverlap 0x00475c80) with the original
// bounding-circle fallback only where the original has one.

#include "game_state.hpp"

namespace game {

// Ghidra Weapon_CanWeaponHitTarget (0x00426ef0). Engagement gates: valid
// ownership/system, active and non-destroyed target, mode-1 target matching,
// self/faction/leader/stellar-target exclusions, player-aligned owner exclusion
// gates plus a target-player-conditioned owner booty gate, scripted-maneuver
// exclusion, and the ship-class capability_flags 0x400 vs weapon
// flags_secondary 0x400 match.
[[nodiscard]] bool NovaWeapon_CanProjectileHitShip(const GameState &state,
                                                   const ActiveShot &shot,
                                                   std::int16_t target_slot);

// Public entry to the file-local Ship_ShipsShareSquadRoot walk (primary site
// SharesSquadRoot, // Ghidra 0x0046d190 Ship_ShipsShareSquadRoot.). Used by
// Shot_HandleShot's player target-chain arm.
[[nodiscard]] bool NovaShip_ShipsShareSquadRoot(const GameState &state,
                                                std::int16_t first_slot,
                                                std::int16_t second_slot);

// Ghidra Ship_ApplyDamageToShip (0x004192d0) core: shield-first/
// armor damage, disable-transition arms, aggro/hostility response. Exported
// for the hull-destruction blast in Ship_UpdateVisualState 0x00428340.
void Ship_ApplyDamageToShip(GameState &state,
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

// Shot_HandleShot's expiry block, split so the damage/on-hit helpers stay in
// this translation unit (primary site NovaWeapon_TickShots,
// // Ghidra 0x00435830 Shot_HandleShot.). Called by the outer expiry test only
// while the shot is in the player's current system; the caller suppresses the
// whole block on a system mismatch. A plain weapon (flags_primary 0x8000
// clear) spawns the single default impact sprite only when impact_effect_id
// is nonzero; a flags-0x8000 weapon takes the area helper plus the
// splash-damage pass over every active ship within splash_radius, suppressed
// by flags_secondary 0x400. NPC owners are always splash-immune; the player
// owner is immune only when flags_primary 0x100 is set.
void NovaWeapon_ResolveShotExpiryImpact(GameState &state,
                                        const ActiveShot &shot,
                                        const Weapon &weapon);

// Ghidra Ship_HandleSpritePairCollision (0x004374f0) plus its sprite-layer
// driver: resolves direct shot-vs-ship contacts with the original's bounding-
// circle / pixel-mask overlap and dispatches Shot_ResolveShotCollisionHit with
// linked shots disallowed. Also runs the Asteroid_HandleSpritePairCollision
// (0x00436f70) contact for each shot that did not hit a ship.
void NovaWeapon_ResolveDirectShotCollisions(GameState &state);

// Ghidra Ship_HandleSpritePairCollision (0x004374f0), freeflight-object arm:
// the mining-scoop collection pass. Every ship pixel-mask-overlaps the
// persistent freeflight resource-boxes; the player (mining_scoop_active) or an
// NPC in AI state 0x11 collects one unit of the object's cargo (extra 0..5)
// into its own hold, or junk (extra 1000..1127, player only) payload, retiring
// the object. The player pickup re-derives the scoop latch through the
// cargo-capacity gate.
void NovaWeapon_ResolveFreeflightScoop(GameState &state);

// Public test seam the direct-contact pass uses: resolves each live entity's
// current-frame pixel mask from the non-SDL SpriteMaskStore (ships via the
// class sh\x8an base sheet, shots via weapon sprite+3000, asteroids via
// kAsteroidSpinBase+type) using the same frame selection as the renderer.
void NovaCollision_RefreshCollisionMasks(GameState &state);

// Ghidra Shot_ResolveCollisions (0x00437e20): the blast-proximity pass over
// live shots. Runs the planet-type (flags_secondary 0x400) stellar contact arm
// first (ResolveShotStellarContact), then resolves at most one proximity ship
// hit per shot when the weapon has a blast radius, then scans the asteroid pool
// (weapons with flags_quaternary bit 0 clear) into
// NovaUi_ResolveWeaponSplashImpact.
void NovaWeapon_ResolveProjectileCollisions(GameState &state);

// Ghidra 0x0046f1e0 Frame_AddCombatRatingPoints. Adds one kill's combat value
// (ShipClass.strength) to the player's aggregate rating: sub-5 values add a
// single point, larger values add round(points * 0.2), and the total is capped
// at 10,000,000. Called from the destruction arm of Shot_ResolveShipHitFrom-
// Weapon (0x004192d0); Ship_HandlePlayerShipCore has a second caller.
void NovaFrame_AddCombatRatingPoints(GameState &state, float points);

// Ghidra Stellar_HandleShipStellarCrash (0x0043aed0): fatal-stellar
// (availability_flags 0x100) ship-crash pass. Runs in Frame_TickSystems scope
// 8 after Stellar_TickStellarGravityPull. Instant-kills non-immune ships whose
// current-frame mask overlaps the loaded stellar sprite.
void NovaStellar_HandleShipStellarCrash(GameState &state);

// Ghidra Stellar_TickStellarDefenseBatteries (0x0042D890): per-tick defense-
// battery fire pass. For each of the current system's 16 nav stellars that is
// available, owns a weapon (Stellar.weapon_id), and is not in its active
// (destroyed/engaged) state, ticks defense_battery_cooldown down by the frame
// tick scale. On expiry it scans the 64 ship slots for the nearest active
// non-cloaked candidate that Government_IsCandidateHostileToTargeter accepts,
// within the weapon's range, then spawns one battery shot and reloads (or
// applies the burst wrap). The original has its own gameplay-frozen guard;
// this port currently dispatches scope 8 only on full ticks because it has no
// separate freeze latch (see Stub_MiscHandlers).
void NovaStellar_TickStellarDefenseBatteries(GameState &state);

// Ghidra Shot_QueueBeamHit / Shot_UpdateBeamHitQueue (0x00427a90/0x0042f270)
// impact leg, exposed for the instantaneous beam queue. Applies one direct
// weapon impact using the same shield/armor, ionization, impulse, and aggro
// path as projectiles. `suppress_retarget_logic` is the original's argument of
// the same name: true when the beam contact is the owner's locked target (it
// bypasses the incidental-fire gates), false for a stray beam sweep. The
// 0x0042f270 incidental arm passes true only when the swept hit equals the
// beam's recorded target or the owner's current primary target.
void NovaWeapon_ResolveDirectWeaponHit(GameState &state,
                                       std::int16_t owner_ship_slot,
                                       std::int16_t target_ship_slot,
                                       std::int16_t weapon_id,
                                       std::int8_t impact_variant = 0,
                                       bool suppress_retarget_logic = true);

} // namespace game
