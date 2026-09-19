#pragma once

// Clean-room reconstruction of the player ship's weapon banks and the primary
// firing path, mirroring the Ghidra weapon subsystem:
//   Weapon_RebuildWeaponBankPoolsFromOwnedOutfits 0x00463260
//   Weapon_ReconcileOutfitPoolWithWeaponBanks      0x00462ec0
//   Weapon_CanFireWeaponBank                      0x00468990
//   Weapon_FirePlayerWeaponBank                   0x00455150
//   Weapon_GetWeaponFireIntervalTicks             0x0046f270
// The original stores 0x100 weapon banks (one per zero-based weapon id),
// each carrying two counters: `weapon_count_by_class[bank]` (the number of the
// weapon mounted / shots per volley, >0 gates whether the primary-fire loop
// touches the bank) and `weapon_secondary_count_by_class[bank]` (carried
// ammunition rounds). The starter Shuttle mounts a single Light Blaster (stock
// weapon {id 0x80, count 1, ammo -1=unlimited}); `weapon_count_by_class[0]=1`
// so it is fireable.
//
// The player still only fires the basic projectile branch here. The shared
// projectile spawn record carries the ownership/lifetime data consumed by
// the first collision slice; carrier-bay, beam, turret, guided, and launcher
// branches remain deferred.

#include "game_state.hpp"
#include "scenario_data.hpp"

#include <cstdint>
#include <string>

namespace game {

struct NovaPreferences;

// Ghidra 0x00422210 Ship_TallyInboundWeaponThreat. Resets the threat latch on
// each active ship, then sums half of each live, normal-target-state shot's
// mass + energy damage into the shot's recorded target. The original pool holds
// 128 shots and truncates toward zero after every individual contribution.
void NovaWeapon_TallyInboundWeaponThreat(GameState &state);

// Ghidra Weapon_RebuildWeaponBankPoolsFromOwnedOutfits (0x00463260): rebuilds
// the player's 0x100 weapon-bank ammo/secondary counters from the currently
// owned outfits. Every owned outfit with ModType 1 (kWeapon) contributes its
// owned count to weapon_count_by_class[mod_val] (mod_val is the zero-based
// weapon bank slot); every owned ModType 3 (kAmmo) outfit contributes to
// weapon_secondary_count_by_class[mod_val]. All banks are zeroed first. Mirrors
// the original's zero-sweep + owned-outfit accumulation. Used when outfit
// ownership changes (the outfitter buy/sell path).
void NovaWeapon_RebuildBanksFromOwnedOutfits(GameState &state);

// Seeds the player's 0x100 weapon-bank ammo/secondary counters from a ship
// class's mounted stock weapons (Ghidra default_weapon_ammo / secondary).
// Used by new-game seeding and the shipyard purchase path, followed by
// NovaWeapon_ReconcileOutfitPoolWithWeaponBanks so mounted stock guns become
// owned outfits.
void NovaWeapon_SeedBanksFromShipStock(GameState &state,
                                       std::int16_t ship_class_id);

// Ghidra Weapon_ReconcileOutfitPoolWithWeaponBanks (0x00462ec0): reconciles
// outfit-pool counts with the live weapon-bank ammo/secondary counters in both
// directions. After seeding banks from a ship class's stock weapons
// (Menu_RunNewGameFlow) or after any buy/sell, it converts leftover positive
// bank ammo not explained by an owned weapon outfit into an owned outfit
// (and leftover secondary into an owned ammo outfit), so a mounted stock
// weapon like the Shuttle's Light Blaster is registered as sellable ownership.
void NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(GameState &state);

// Ghidra Stellar_RunDockAndLaunchSequence (0x00455e10) clears transient combat
// state at a system/stellar boundary. This does not alter weapon ownership or
// ammo.
void NovaWeapon_ClearTransientCombatState(GameState &state);

// Ghidra Weapon_InitShipWeaponBanksFromShipClass-side initializer used by the
// NPC paths (ship_ai.cpp EnsureNpcWeaponBanks, citing Weapon_InitShipWeapon-
// Bursts 0x00413810 for the burst-counter preload): rebuilds an NPC ship's
// 0x100 weapon-bank counters from its ship class's stock weapons. No-op for
// the player ship and when the cached loadout already matches the class.
void NovaWeapon_EnsureNpcWeaponBanks(GameState &state, Ship &ship);

// Copies a ship class's decoded stock weapons (the original's
// default_weapon_ammo/secondary 0x100 tables) into an NPC ship's flat
// per-bank counters, zeroing both counters first. Unlike
// NovaWeapon_EnsureNpcWeaponBanks it leaves the per-bank cooldown and
// burst-counter arrays untouched, matching Ship_ResetShipToDefaultCombatState's
// refill arm (0x0041e240), which only overwrites the two count tables. No-op
// for the player ship. Callers own the npc_weapon_banks_ship_class cache.
void NovaWeapon_CopyShipClassStockBanks(const GameState &state, Ship &ship);

// Ghidra Weapon_InitShipWeaponBursts (0x00413810). For every mounted bank whose
// weapon has both a burst cycle and a reset cooldown, zeroes the burst counter
// and preloads the bank cooldown to the reset cooldown. Called when a loadout
// is (re)built and from the shot-hit cloak re-entry reset. No-op for the player
// ship (no NPC bank state).
void NovaWeapon_InitShipWeaponBursts(GameState &state, Ship &ship);

// Ghidra 0x004138a0 Weapon_ClassifyShipWeaponAmmoReadiness. Classifies the
// NPC ship's armed weapon banks into three readiness buckets:
//   2 = no armed banks, or every armed bank is depleted;
//   1 = armed banks exist and every usable (cost-bearing) bank is depleted;
//   0 = at least one armed bank is ready.
// A bank is armed when npc_weapon_count_by_class > 0. Its def's ammo_type
// (ammo_or_energy_cost_code) classifies it: >= 0 secondary-ammo (ready while
// npc_weapon_secondary_count_by_class > 0); < -1000 fuel weapon (ready while
// fuel_points is STRICTLY greater than |cost| - 1000, equality counts
// depleted); [-1000,-1] free-energy (armed, never depleted).
[[nodiscard]] int NovaWeapon_ClassifyAmmoReadiness(const GameState &state,
                                                   const Ship &ship);

// Ghidra 0x00411600 Weapon_IsShipWithinWeaponRangeOfTarget. Reach envelope
// floor(|dx|)^2 + floor(|dy|)^2 <= reach^2; beam reach (modes 0/3 only) is
// beam_length_px + 32 exactly, everything else floors range_scalar + 32.0f.
// weapon_slot >= 0 checks that bank; -1 scans stock-armed class banks with
// weapon_mode_code < 9 (first hit wins).
[[nodiscard]] bool
NovaWeapon_ShipWithinWeaponRangeOfTarget(const GameState &state,
                                         const Ship &ship,
                                         const Ship &target,
                                         std::int16_t weapon_slot);

// Ghidra 0x0046f2c0 Weapon_GetWeaponBurstAttempts: shots per trigger pull.
// Non-burst weapons (flags_primary 0x40 clear) fire exactly one; burst banks
// start from the mounted ammo count capped by the cost bank's loaded
// secondary (or fuel_points / per-shot fuel for ammo_type < -999).
[[nodiscard]] int NovaWeapon_GetWeaponBurstAttempts(const GameState &state,
                                                    const Ship &ship,
                                                    std::int16_t weapon_bank);

// Ghidra 0x00464670 Weapon_HasLoadedLaunchBayAmmo: true when the ship's
// class has a KeyCarried fighter (ShipClass.key_carried_ship_class) and one
// of its 0x100 banks holds a mode-99 bay weapon with mounted ammo > 0 and
// loaded secondary > 0 whose ammo_type - 0x80 equals the KeyCarried id.
[[nodiscard]] bool NovaWeapon_HasLoadedLaunchBayAmmo(const GameState &state,
                                                     const Ship &ship);

// Ghidra 0x00464520 Weapon_HasLaunchBayWeapon: true when any of the ship's
// banks is a mode-99 bay weapon with mounted ammo > 0 whose carried ship
// class (ammo_type) sets capability Flags 0x8000 (escape-ship type). Reads
// the mounted counter, not the loaded secondary.
[[nodiscard]] bool NovaWeapon_HasLaunchBayWeapon(const GameState &state,
                                                 const Ship &ship);

// Ghidra 0x00464600 Weapon_FindLaunchBayWeaponBank: the first loaded
// launch-bay bank (same predicate as NovaWeapon_HasLaunchBayWeapon), or -1.
[[nodiscard]] std::int16_t
NovaWeapon_FindLaunchBayWeaponBank(const GameState &state, const Ship &ship);

// Ghidra 0x00415C10 Weapon_HasAnyFireableNonSecondaryWeapon: true when some
// bank with mounted ammo > 0 holds a damaging, non-secondary (Flags2 0x1000
// clear) weapon in a straight-flight mode (-1/0/3/4/6/7/8) that passes
// NovaWeapon_CanFireWeaponBank.
[[nodiscard]] bool
NovaWeapon_HasAnyFireableNonSecondaryWeapon(const GameState &state,
                                            const Ship &ship);

// Ghidra 0x0046CEC0 Weapon_GetShipMaxWeaponRange: the furthest effective
// reach over the ship's armed (bank ammo > 0), fireable
// (NovaWeapon_CanFireWeaponBank) banks. Reach per weapon mode: 0/3 =
// beam_length_px; -1/4/7 = floor(range_scalar) (the original's case list
// tests mode 7 twice and omits mode 8, so turret projectiles contribute 0);
// 1 = floor(range_scalar * 0.85); 6 = floor(range_scalar * 0.5); else 0.
// Returns the maximum, clamped to 0x7fff. Feeds the AI state machine's
// approach/keep-distance envelope (scaled by 0.85 at 0x00406a4e).
[[nodiscard]] int NovaWeapon_GetShipMaxWeaponRange(const GameState &state,
                                                   const Ship &ship);

// Ghidra 0x00468990 Weapon_CanFireWeaponBank: whether the given weapon bank
// may fire right now for a specific ship (player = GameState strided banks;
// NPC = Ship.npc_weapon_bank_*). Faithful branching on ship_instance_id==0:
// for the player the secondary-read index is the COST bank (ammo_type), for an
// NPC it is the FIRING bank itself. Gates, in original order: NPC-only
// flags_secondary 0x100 mount gate; cloak-visibility threshold (0x4000 =
// Bible "can be fired while cloaked"); launch-bay dependency (0x80, resolved
// through Weapon_HasLoadedLaunchBayAmmo); carrier-bay mode-99 loaded counter;
// ammo_type [0,0xff] counter; fuel-drawn ammo_type <= -1000 with per-shot
// fuel (|cost| - 1000) * 0.1 (DAT_00575810; FCOMPP passes on equality -- the
// readiness classifier 0x004138a0 compares UNSCALED, an original
// inconsistency preserved on both sides). Returns a plain bool (AL only).
[[nodiscard]] bool NovaWeapon_CanFireWeaponBank(const GameState &state,
                                                const Ship &ship,
                                                std::int16_t weapon_bank);

// Ghidra 0x0046c320 Weapon_SelectTurretQuadrant: pick and advance the firing
// barrel quadrant for the weapon's turret group (per-ship rotation state,
// Ship.muzzle_quadrant), offsetting muzzle_x/muzzle_y to the barrel via
// Weapon_ApplyTurretSpreadVelocity (0x0046c5c0). The muzzle bearing is the
// displayed rotation frame scaled back to degrees. When the weapon has
// flags_tertiary 0x10 and target_pos is non-null, the quadrant is replaced by
// the target-nearest barrel (0x0046c4e0). Returns the quadrant used, or -1
// when the weapon has no valid turret group.
std::int16_t NovaWeapon_SelectTurretQuadrant(GameState &state,
                                             Ship &ship,
                                             std::int16_t weapon_id,
                                             float &muzzle_x,
                                             float &muzzle_y,
                                             const float *target_pos);

// Ghidra Shot_SpawnShotFromWeapon (0x0041fd30): allocate one clean-room shot
// record, initialize owner/target/system attribution, muzzle position,
// heading/spread, inherited velocity, lifetime, and the provisional collision
// envelope. The current implementation covers only straight-flight spawn;
// homing guidance and sprite-container setup remain deferred.
[[nodiscard]] int NovaWeapon_SpawnProjectile(GameState &state,
                                             std::int16_t owner_ship_slot,
                                             std::int16_t target_ship_slot,
                                             std::int16_t weapon_id,
                                             bool spawn_without_owner = false,
                                             bool apply_random_spread = true);

// Ghidra Shot_AimStellarBatteryShot (0x0043BA30): lead-aim a stellar defense
// battery's shot. The battery/weapon are stationary, so the intercept is
// target_pos + target_vel * flight_time (no shooter-velocity term, unlike
// Ship_AimWeaponPredictive). Mode 6 (freeflight rocket) uses the two-regime
// spool-up model. `battery` supplies the weapon id (StellarDef +0x2c) and the
// muzzle at map_x/map_y; returns the game-degree bearing.
[[nodiscard]] std::int16_t NovaWeapon_AimStellarBatteryShot(
    const GameState &state, const Stellar &battery, const Ship &target);

// Ghidra Stellar_TickStellarDefenseBatteries (0x0042D890) shot-construction
// inline block: allocate one shot at the battery (map_x/map_y), aimed/lead at
// `target_ship_slot` with the battery weapon `weapon_resource_id`, and queue
// its fire sound. Returns the active-shot index or -1 when the weapon is not
// resolvable. The caller owns the cooldown/burst bookkeeping.
[[nodiscard]] int
NovaWeapon_SpawnStellarBatteryShot(GameState &state,
                                   const Stellar &battery,
                                   std::int16_t target_ship_slot,
                                   std::int16_t weapon_resource_id);

// Ghidra Shot_SpawnLinkedShotsOnImpact (0x00420D30): spawn the impacting
// weapon's linked submunitions (Bible SubCount/SubType/SubTheta/SubLimit) at
// the impact position. Owner/target context and heading/velocity are inherited
// from `impacting_shot`; each child's linked_shot_generation is parent + 1.
// `impacting_shot` is read by value before any child is appended, so callers
// may pass a reference into GameState::active_shots.
void NovaWeapon_SpawnLinkedShotsOnImpact(
    GameState &state,
    const ActiveShot &impacting_shot,
    std::int16_t fallback_target_ship_slot);

// Ghidra Weapon_FirePlayerWeaponBank (0x00455150): fires one player weapon
// bank for the current frame. Reconstructed for the unguided-projectile mode:
// consumes ammo/energy as the weapon requires, spawns one shot toward the
// ship's current heading, then sets the bank's cooldown to the weapon's fire
// interval (reload_ticks) so the bank cannot fire again until it elapses.
// Once a round actually spawns, the weapon's fire_sound slot (when >= 0) is
// appended to GameState.pending_fire_sounds so the spaceflight loop (which
// owns the SdlAudio device) can play the cached sound -- mirroring the
// original's volley_fired > 0 gate around NovaAudio_PlaySpatialByDistance.
// The visual side-effects, burst-cycle bookkeeping and carrier-bay / beam /
// guided modes are not yet implemented (TODO(decomp)).
void NovaWeapon_FirePlayerWeaponBank(GameState &state,
                                     std::int16_t weapon_bank);

// PlayerTick_WeaponCommands (synthetic entry 0x0044BEB7) +
// PlayerTick_WeaponCycleContinuation
// (0x0044EAB4) of Ship_HandlePlayerShipCore (0x0044aa70): the player's weapon
// command inputs, edge/hold-resolved by the spaceflight loop (the original
// reads g_player_key_bindings through NovaInput_IsCommandActiveWithGameplay-
// Guards with its 0x38/0x6f direction-modifier pair).
struct PlayerWeaponCommandInput {
  // Binding slot 2 (default DIK 0x39 = space): fire every primary bank.
  bool fire_primary_held = false;
  // Binding slot 3 (default DIK 0x1d = Left Ctrl): fire the selected
  // secondary bank (active_weapon_bank_slot).
  bool fire_secondary_held = false;
  // Binding slot 0 (default DIK 0x11 = W): cycle the secondary bank. The
  // caller edge-resolves this against g_playerSecondaryCycleCommandLatch
  // (DAT_007cab42).
  bool cycle_secondary = false;
  // Direction modifier (original 0x38/0x6f = Shift pair): cycle backwards.
  bool cycle_secondary_backwards = false;
  // Binding slot 1 (default DIK 0x1f = S): deselect the secondary bank.
  bool clear_secondary = false;
};

// The weapon command dispatch: primary fire loop, selected-secondary fire,
// auto-clear of an unfirable flags-0x800 bank, the wrapped secondary-bank
// cycle (count eligible banks, denial/accept cue, skip ineligible banks) and
// the clear-selection arm. Firing arms are gated on ai_station_hold_timer /
// ai_maneuver_timer_ms expiry and the disabled (disabled) state.
void NovaWeapon_TickPlayerWeaponCommands(GameState &state,
                                         const PlayerWeaponCommandInput &input,
                                         float elapsed_ticks);

// Cooldown-decay tail of PlayerTick_WeaponCommands: per bank with ammo, decay
// the cooldown by the frame tick scale, and pin banks whose weapon has
// flags_quaternary 0x20 at a 1-tick cooldown while the player is ionized
// (Ship_GetIonizationIntensity > 0).
void NovaWeapon_TickPlayerWeaponBankCooldowns(GameState &state,
                                              float elapsed_ticks);

// Ghidra Weapon_FireShipWeapons (0x00414550): fire the selected NPC bank for
// one volley. This slice covers projectile modes -1, 1, 4, 6, 7, and 8;
// beams, turrets, and carrier-bay branches remain deferred. A successful
// dispatch queues the weapon's fire sound (spatial, sourced at this ship) via
// GameState.pending_fire_sounds, mirroring the original's sVar9 > 0 gate
// around NovaAudio_PlaySpatialByDistance. Ship_HandleShip's handoff semantics
// are included: flags_primary 0x2 banks retain their active-bank/trigger latch;
// ordinary banks consume both fields after one handoff.
void NovaWeapon_FireNpcWeaponBank(GameState &state, Ship &ship);

// Ghidra 0x0043a310 Weapon_SelectTurretTargetWithinArc: automatically fires
// the first ready point-defense bank at the nearest eligible inbound guided
// shot, falling back to an eligible attacking ship only when no shot exists.
void NovaWeapon_SelectTurretTargetWithinArc(GameState &state, Ship &ship);

// Ship_HandleShip (0x00433050): count down NPC-local bank cooldowns.
void NovaWeapon_TickNpcWeaponBanks(Ship &ship, float elapsed_ticks);

// Ghidra Shot_QueueBeamHit (0x00427A90): enqueue one immediate beam hit.
// The queue is gameplay-complete for direct target impacts; turret quadrant
// selection and the original's per-frame endpoint re-derivation
// (Shot_UpdateBeamHitQueue 0x0042F270) remain deferred. With a valid target
// the beam endpoint tracks the target; otherwise firing_bearing_deg aims the
// endpoint downrange at BeamLength + 32 (the original's beam reach constant).
[[nodiscard]] bool NovaWeapon_QueueBeamHit(GameState &state,
                                           std::int16_t owner_ship_slot,
                                           std::int16_t target_ship_slot,
                                           std::int16_t weapon_id,
                                           std::int16_t forced_targeting = -1,
                                           std::int16_t firing_bearing_deg = 0);

// Ghidra Shot_UpdateBeamHitQueue (0x0042F270): advance beam lifetimes and
// resolve each queued direct impact once.
void NovaWeapon_TickBeamHitQueue(GameState &state, float elapsed_ticks);

// Ghidra Shot_UpdateShotGuidance (0x00431530): per-frame projectile guidance.
// State machine on ActiveShot.guidance_state: 0 = mode-1 homing (target
// bearing chase at the weapon's guided_turn_rate, jamming lock defeat via
// Ship.jamming_score vs the shot's per-channel vulnerability rolls, cloak
// dumb-fire, 45-deg lock drop for Seeker 0x4000, 1-in-10 asteroid decoy scan
// for Seeker 0x0002, owner-retarget rolls for Seeker 0x8000), 999 =
// interference weave, 998 = inert (target lost), 1 = asteroid-decoy tracking.
// Mode 6 rockets blend their velocity onto the aim heading; mode 5 freefall
// bombs/mines turn their nose onto the velocity vector. Called before position
// integration; raw_call_count replays discrete 21 ms calls while elapsed_ticks
// carries the original normalized frame scale.
void NovaWeapon_UpdateShotGuidance(GameState &state,
                                   ActiveShot &shot,
                                   float elapsed_ticks,
                                   int raw_call_count = 1);

// Per-frame shot + cooldown bookkeeping for the firing path. Advances each
// active shot by its velocity, counts down its remaining life, and steps the
// time-animated shot-frame cycle (frame_cycle_index / anim_elapsed at the
// weapon's animation-frame interval, mirroring Shot_HandleShot's animated
// branch), then removes expired rounds and decrements every weapon-bank
// cooldown toward zero. `elapsed_ticks` is elapsed time in normalized 30 Hz
// ticks; it drives animation, movement, lifetime, and cooldowns independently
// of the current update rate. Called from Frame_TickSystems scope 7.
void NovaWeapon_TickShots(GameState &state,
                          float elapsed_ticks = 1.0F,
                          const NovaPreferences *prefs = nullptr);

// Diagnostic: the human-readable weapon name of the given bank's weapon, or
// "?" when the bank is unmounted/invalid. Used by the HUD weapon readout.
[[nodiscard]] std::string NovaWeapon_BankDisplayName(const GameState &state,
                                                     std::int16_t weapon_bank);

// The ammo/secondary counter shown for a weapon bank in the HUD weapon/ammo
// panel, mirroring NovaUi_DrawActiveWeaponAmmoPanel (0x00460ec0). The original
// reads `weapon_secondary_count_by_class_counter_0[bank*100]` for a special
// weapon (weapon_mode_code 99 / out-of-range ammo_type), else the ammo counter
// of the weapon whose id equals this weapon's ammo_type
// (weapon_secondary_count_by_class counter `ammo_type`). The panel hides the
// count for energy-based weapons (ammo_type == -1 or flags_secondary & 0x40),
// and returns `-1` to signal that case. Returns -1 when the bank/weapon is
// invalid.
[[nodiscard]] std::int16_t NovaWeapon_BankAmmoCount(const GameState &state,
                                                    std::int16_t weapon_bank);

// Weapon fire-sound slot mapping. A weapon's `fire_sound` field (Ghidra
// WeaponDef.fire_sound_slot) is a slot index 0..35 that the original resolves
// through the preloaded g_gameplay_sound_handle_table; in the shipped data the
// slot maps to the snd resource id 200 + slot (verified: Light Blaster slot 8
// -> "Light Blaster.sfil" id 208). -1 means the weapon has no fire sound.
[[nodiscard]] constexpr std::int16_t
NovaWeapon_FireSoundResourceId(std::int16_t fire_sound_slot) {
  return (fire_sound_slot >= 0 && fire_sound_slot < 36)
             ? static_cast<std::int16_t>(200 + fire_sound_slot)
             : -1;
}

// Preloads a weapon fire sound into the GameState cache from the snd resource
// id 200 + `slot` (NovaResource_LoadSndData + NovaSound_Decode), so the firing
// path only looks the slot up in memory. Idempotent; leaves the slot empty on
// a decode failure (the weapon then fires silently, as if fire_sound_slot were
// -1). Call at spaceflight entry.
void NovaWeapon_PreloadFireSound(GameState &state,
                                 std::int16_t fire_sound_slot);

// Preloads every distinct fire sound the player's currently owned primary
// weapons use. Iterates the rebuilt weapon banks, collects each owned weapon's
// fire_sound slot, and loads it into the cache. Call at spaceflight entry so a
// bank never silently misses its first shot's sound.
void NovaWeapon_PreloadOwnedFireSounds(GameState &state);

void NovaWeapon_PreloadGameplaySounds(GameState &state);

// Ghidra NovaAudio_PlaySpatialByDistance (0x004692e0) distance falloff, as a
// pure 0..1 attenuation (the sound-volume extent factors out; the caller's
// master volume carries the preference). Returns 1.0 within 200 px, then
// falls off with 1/d^2 (loud channel full at 850 px, quiet channel at the
// 200-px reference) with each channel floored at 1/8, averaged to the mono
// gain the original's mixer actually plays.
[[nodiscard]] float NovaWeapon_ComputeSpatialFireGain(float listener_x,
                                                      float listener_y,
                                                      float src_x,
                                                      float src_y);

} // namespace game
