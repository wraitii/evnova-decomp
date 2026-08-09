#pragma once

// Clean-room reconstruction of the player ship's weapon banks and the primary
// firing path, mirroring the Ghidra weapon subsystem:
//   Weapon_RebuildWeaponBankPoolsFromOwnedOutfits 0x00463260
//   Weapon_ReconcileOutfitPoolWithWeaponBanks      0x00462ec0
//   Weapon_CanFireWeaponBank                      0x00468990
//   Weapon_FirePlayerWeaponBank                   0x00455150
//   Weapon_GetWeaponFireIntervalTicks             0x0046f270
// The original stores 0x100 weapon banks (one per zero-based weapon id),
// each carrying two counters: `weapon_bank_ammo[bank]` (the number of the
// weapon mounted / shots per volley, >0 gates whether the primary-fire loop
// touches the bank) and `weapon_bank_secondary[bank]` (carried ammunition
// rounds). The starter Shuttle mounts a single Light Blaster (stock weapon
// {id 0x80, count 1, ammo -1=unlimited}); `weapon_bank_ammo[0]=1` so it is
// fireable.
//
// Only the unguided-projectile branch of the firing routine is reconstructed
// (weapon_mode_code -1/1/5/6 -> Shot_SpawnShotFromWeapon). The carrier-bay,
// beam (mode 0), turret (3/4), guided (7/8) and launcher (99) branches are
// not yet implemented (they need the full shot/turret/targeting systems).

#include "game_state.hpp"
#include "scenario_data.hpp"

#include <cstdint>
#include <string>

namespace game {

// Ghidra Weapon_RebuildWeaponBankPoolsFromOwnedOutfits (0x00463260): rebuilds
// the player's 0x100 weapon-bank ammo/secondary counters from the currently
// owned outfits. Every owned outfit with ModType 1 (kWeapon) contributes its
// owned count to weapon_bank_ammo[mod_val] (mod_val is the zero-based weapon
// bank slot); every owned ModType 3 (kAmmo) outfit contributes to
// weapon_bank_secondary[mod_val]. All banks are zeroed first. Mirrors the
// original's zero-sweep + owned-outfit accumulation. Used when outfit
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

// Ghidra Weapon_CanFireWeaponBank (0x00468990): whether the given weapon bank
// may fire right now. The clean-room player model omits the ship-disable, NPC
// ammo and launch-bay-dependency gates (no NPC/disable state yet); only the
// ammo/energy sufficiency checks that apply to the player's owned banks are
// reproduced.
[[nodiscard]] bool NovaWeapon_CanFireBank(const GameState &state,
                                          std::int16_t weapon_bank);

// Ghidra Weapon_FirePlayerWeaponBank (0x00455150): fires one player weapon
// bank for the current frame. Reconstructed for the unguided-projectile mode:
// consumes ammo/energy as the weapon requires, spawns one shot toward the
// ship's current heading, then sets the bank's cooldown to the weapon's fire
// interval (reload_ticks) so the bank cannot fire again until it elapses.
// Once a round actually spawns, the weapon's fire_sound slot (when >= 0) is
// appended to GameState.pending_fire_sound_slots so the spaceflight loop (which
// owns the SdlAudio device) can play the cached sound -- mirroring the
// original's volley_fired > 0 gate around NovaAudio_PlaySpatialByDistance.
// The visual side-effects, burst-cycle bookkeeping and carrier-bay / beam /
// guided modes are not yet implemented (TODO(decomp)).
void NovaWeapon_FirePlayerWeaponBank(GameState &state,
                                     std::int16_t weapon_bank);

// Primary-fire dispatch mirroring the Ship_HandlePlayerShipControl loop: for
// every bank with ammo > 0 and whose weapon is NOT a secondary (Weapon.flags
// bit 0x2 unset), attempts to fire it. The player's main (Light Blaster) bank
// is such a bank, so holding fire fires it on a cadence.
void NovaWeapon_FirePlayerPrimary(GameState &state);

// Per-frame shot + cooldown bookkeeping for the firing path. Advances each
// active shot by its velocity, counts down its remaining life, and steps the
// time-animated shot-frame cycle (frame_cycle_index / anim_elapsed at the
// weapon's shot_anim_frame_dwell cadence, mirroring Shot_HandleShot's animated
// branch), then removes expired rounds and decrements every weapon-bank
// cooldown toward zero. `frame_time_ms` is the real elapsed frame time used to
// accumulate the animated-short dwell (ignored for static/heading shot sets).
// Called once per spaceflight frame for the player's shots.
void NovaWeapon_TickShots(GameState &state, float frame_time_ms = 1.0F);

// Diagnostic: the human-readable weapon name of the given bank's weapon, or
// "?" when the bank is unmounted/invalid. Used by the HUD weapon readout.
[[nodiscard]] std::string NovaWeapon_BankDisplayName(const GameState &state,
                                                     std::int16_t weapon_bank);

// The ammo/secondary counter shown for a weapon bank in the HUD weapon/ammo
// panel, mirroring NovaUi_DrawActiveWeaponAmmoPanel (0x00460ec0). The original
// reads `weapon_bank_secondary_counter_0[bank*100]` for a special weapon
// (weapon_mode_code 99 / out-of-range ammo_type), else the ammo counter of the
// weapon whose id equals this weapon's ammo_type (weapon_bank_secondary
// counter `ammo_type`). The panel hides the count for energy-based weapons
// (ammo_type == -1 or flags_secondary & 0x40), and returns `-1` to signal that
// case. Returns -1 when the bank/weapon is invalid.
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

} // namespace game
