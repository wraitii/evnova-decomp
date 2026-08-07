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
// owned count to weapon_bank_ammo[mod_val] (mod_val is the weapon resource
// id); every owned ModType 3 (kAmmo) outfit contributes to
// weapon_bank_secondary[mod_val]. All banks are zeroed first. Mirrors the
// original's zero-sweep + owned-outfit accumulation. Used when outfit
// ownership changes (the outfitter buy/sell path).
void NovaWeapon_RebuildBanksFromOwnedOutfits(GameState &state);

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
// The audio/visual side effects, burst-cycle bookkeeping and carrier-bay /
// beam / guided modes are not yet implemented (TODO(decomp)).
void NovaWeapon_FirePlayerWeaponBank(GameState &state,
                                     std::int16_t weapon_bank);

// Primary-fire dispatch mirroring the Ship_HandlePlayerShipControl loop: for
// every bank with ammo > 0 and whose weapon is NOT a secondary (Weapon.flags
// bit 0x2 unset), attempts to fire it. The player's main (Light Blaster) bank
// is such a bank, so holding fire fires it on a cadence.
void NovaWeapon_FirePlayerPrimary(GameState &state);

// Per-frame shot + cooldown bookkeeping for the firing path. Advances each
// active shot by its velocity and counts down its remaining life (removing
// expired rounds), and decrements every weapon-bank cooldown toward zero.
// Called once per spaceflight frame for the player's shots.
void NovaWeapon_TickShots(GameState &state);

// Diagnostic: the human-readable weapon name of the given bank's weapon, or
// "?" when the bank is unmounted/invalid. Used by the HUD weapon readout.
[[nodiscard]] std::string NovaWeapon_BankDisplayName(const GameState &state,
                                                     std::int16_t weapon_bank);

} // namespace game
