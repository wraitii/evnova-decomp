#include "weapon.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "game_state.hpp"
#include "outfit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace game {
namespace {

// The clean-room weapon banks are kept in the GameState strided arrays with the
// original's 100-element stride (see game_state.hpp). These helpers read/write
// the ammo / secondary counters for one zero-based weapon bank.
constexpr std::size_t kBankStride = 100;

std::int16_t &BankAmmo(GameState &state, std::int16_t bank) {
  return state.weapon_bank_ammo[static_cast<std::size_t>(bank) * kBankStride];
}

const std::int16_t &BankAmmo(const GameState &state, std::int16_t bank) {
  return state.weapon_bank_ammo[static_cast<std::size_t>(bank) * kBankStride];
}

std::int16_t &BankSecondary(GameState &state, std::int16_t bank) {
  return state
      .weapon_bank_secondary[static_cast<std::size_t>(bank) * kBankStride];
}

const std::int16_t &BankSecondary(const GameState &state, std::int16_t bank) {
  return state
      .weapon_bank_secondary[static_cast<std::size_t>(bank) * kBankStride];
}

// The weapon loaded in a bank, or nullptr when the bank holds nothing valid.
const Weapon *WeaponAt(const GameState &state, std::int16_t bank) {
  // Banks are indexed by zero-based weapon id; the scenario Weapon() lookup
  // uses the 0x80.. residue. Bank slot b == weapon resource id -- but our
  // clean Weapon table is indexed by (weapon id - 0x80). Resolve via the
  // resource-id offset: bank slot `b` corresponds to weapon resource `b+0x80`.
  return state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
}

} // namespace

void NovaWeapon_SeedBanksFromShipStock(GameState &state,
                                       std::int16_t ship_class_id) {
  // Seeds the player's 0x100 weapon-bank ammo/secondary counters from a ship
  // class's mounted stock weapons (Ghidra default_weapon_ammo / default_weapon_
  // secondary). For each stock weapon triple {weapon_id, count, ammo_load} the
  // mounted-count goes into weapon_bank_ammo[weapon_id-0x80] and any carried
  // rounds (ammo_load, when > 0) into the matching secondary/ammo counter. The
  // original seeds the new ship's banks this way in Menu_RunNewGameFlow and
  // Outfit_SwapPlayerShipWithEscort, then calls
  // Weapon_ReconcileOutfitPoolWithWeaponBanks to register the mounted stock
  // guns as owned outfits.
  const ShipClass *ship = state.scenario.Ship(
      static_cast<std::int16_t>(ship_class_id + 0x80));
  if (ship == nullptr) {
    return;
  }
  for (const ShipDefaultWeaponBank &stock : ship->stock_weapons) {
    if (stock.weapon_id < 0x80 || stock.weapon_id > 0x17f) {
      continue; // unmounted bank (weapon_id -1) or out-of-range
    }
    const std::size_t bank = static_cast<std::size_t>(stock.weapon_id - 0x80);
    state.weapon_bank_ammo[bank * kBankStride] =
        static_cast<std::int16_t>(stock.count > 0 ? stock.count : 0);
    if (stock.ammo_load > 0) {
      state.weapon_bank_secondary[bank * kBankStride] =
          static_cast<std::int16_t>(stock.ammo_load);
    }
  }
}

void NovaWeapon_RebuildBanksFromOwnedOutfits(GameState &state) {
  // Zero-sweep all 0x100 banks (the original clears both counters).
  for (std::size_t b = 0; b < 0x100; ++b) {
    state.weapon_bank_ammo[b * kBankStride] = 0;
    state.weapon_bank_secondary[b * kBankStride] = 0;
  }
  // Accumulate from owned outfits. outfit_owned_count is indexed by outfit id
  // - 0x80; scenario.outfits is indexed the same way, so index i maps to both.
  const auto &outfits = state.scenario.outfits;
  for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size(); ++i) {
    const std::int16_t owned = state.inventory.outfit_owned_count[i];
    if (owned <= 0 || i >= outfits.size()) {
      continue;
    }
    const Outfit &o = outfits[i];
    // ModType 1 (kWeapon): mod_val is the zero-based weapon bank slot
    // (resource id minus 0x80) the owned count loads into; owned count > 0
    // gates firing.
    if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kWeapon)) {
      if (o.mod_val >= 0 && o.mod_val < 0x100) {
        BankAmmo(state, o.mod_val) =
            static_cast<std::int16_t>(BankAmmo(state, o.mod_val) + owned);
      }
    }
    // ModType 3 (kAmmo): mod_val is the zero-based weapon bank slot whose
    // carried-round (secondary) counter this ammo outfit backs.
    if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kAmmo)) {
      if (o.mod_val >= 0 && o.mod_val < 0x100) {
        BankSecondary(state, o.mod_val) =
            static_cast<std::int16_t>(BankSecondary(state, o.mod_val) + owned);
      }
    }
  }
}

void NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(GameState &state) {
  // Ghidra Weapon_ReconcileOutfitPoolWithWeaponBanks (0x00462ec0) reconciles
  // the outfit-pool counts with the live weapon-bank counters in both
  // directions:
  //   1. snapshot each bank's ammo/secondary counter;
  //   2. for every owned outfit, subtract the count it already explains from
  //      the matching snapshot (weapon outfits from ammo, ammo outfits from
  //      secondary), clamping the owned count down if more is owned than the
  //      bank carries;
  //   3. clamp any negative owned count to zero;
  //   4. convert leftover positive bank balance back into owned outfits: for
  //      each bank with ammo > 0 still unaccounted, add that amount to the
  //      first owned-eligible weapon outfit (ModType 1) whose mod_val is the
  //      bank slot; likewise leftover secondary -> an ammo outfit (ModType 3).
  // Menu_RunNewGameFlow calls this right after seeding weapon banks from the
  // starting ship class's stock weapons, so a mounted weapon that has no
  // DefaultItem entry (e.g. the Shuttle's Light Blaster) is registered as an
  // owned outfit and becomes sellable in the Outfitter.
  std::array<std::int16_t, 0x100> bank_ammo{};
  std::array<std::int16_t, 0x100> bank_secondary{};
  for (std::int16_t b = 0; b < 0x100; ++b) {
    bank_ammo[b] = BankAmmo(state, b);
    bank_secondary[b] = BankSecondary(state, b);
  }

  const auto &outfits = state.scenario.outfits;
  for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size() &&
                      i < outfits.size();
       ++i) {
    const std::int16_t owned = state.inventory.outfit_owned_count[i];
    if (owned <= 0) {
      continue;
    }
    const Outfit &o = outfits[i];
    const auto consume = [](std::int16_t &balance,
                            std::int16_t &owned_count) {
      if (owned_count < balance) {
        balance = static_cast<std::int16_t>(balance - owned_count);
      } else {
        owned_count = balance;
        balance = 0;
      }
    };
    if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kWeapon) &&
        o.mod_val >= 0 && o.mod_val < 0x100) {
      consume(bank_ammo[o.mod_val], state.inventory.outfit_owned_count[i]);
    } else if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kAmmo) &&
               o.mod_val >= 0 && o.mod_val < 0x100) {
      consume(bank_secondary[o.mod_val],
              state.inventory.outfit_owned_count[i]);
    }
  }
  for (std::int16_t &count : state.inventory.outfit_owned_count) {
    if (count < 0) {
      count = 0;
    }
  }

  // Leftover positive bank ammo that no owned outfit explains becomes an owned
  // weapon outfit; leftover secondary becomes an owned ammo outfit.
  bool materialized = false;
  for (std::int16_t b = 0; b < 0x100; ++b) {
    if (bank_ammo[b] > 0) {
      for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size() &&
                          i < outfits.size();
           ++i) {
        const Outfit &o = outfits[i];
        if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kWeapon) &&
            o.mod_val == b) {
          state.inventory.outfit_owned_count[i] = static_cast<std::int16_t>(
              state.inventory.outfit_owned_count[i] + bank_ammo[b]);
          bank_ammo[b] = 0;
          materialized = true;
          break;
        }
      }
    }
    if (bank_secondary[b] > 0) {
      for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size() &&
                          i < outfits.size();
           ++i) {
        const Outfit &o = outfits[i];
        if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kAmmo) &&
            o.mod_val == b) {
          state.inventory.outfit_owned_count[i] = static_cast<std::int16_t>(
              state.inventory.outfit_owned_count[i] + bank_secondary[b]);
          bank_secondary[b] = 0;
          materialized = true;
          break;
        }
      }
    }
  }

  if (materialized) {
    OutfitMarkStatsDirty(state);
  }
}

bool NovaWeapon_CanFireBank(const GameState &state, std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return false;
  }
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (!w) {
    return false;
  }
  // Carrier-bay weapons (mode 99) need a loaded ship in the secondary counter.
  if (w->weapon_mode_code == 99) {
    return BankSecondary(state, weapon_bank) >= 1;
  }
  // Ammo/energy requirement. ammo_type (Ghidra ammo_or_energy_cost_code) <
  // -999 means the weapon draws energy from fuel (not implemented; the Light
  // Blaster and most gun weapons are free). In [0,255] it needs carried
  // ammunition; otherwise (-1 = unlimited) no ammo is required.
  const int cost = w->ammo_type;
  if (cost >= 0 && cost <= 0xff) {
    if (BankSecondary(state, static_cast<std::int16_t>(cost)) < 1) {
      return false;
    }
  }
  return true;
}

void NovaWeapon_FirePlayerWeaponBank(GameState &state,
                                     std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return;
  }
  PlayerShip &ship = state.player;
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (!w || state.weapon_bank_cooldown[weapon_bank] > 0.0F) {
    return; // unmounted bank, or still cooling down this frame
  }
  if (!NovaWeapon_CanFireBank(state, weapon_bank)) {
    return;
  }

  // Spawn the shot for the unguided-projectile weapon modes (guidance -1/1/5/6
  // in Ghidra Weapon_FirePlayerWeaponBank -> Shot_SpawnShotFromWeapon). Beams
  // (mode 0), turrets (3/4), guided (7/8) and launchers (99) are not yet
  // reconstructed; this build only reproduces the direct-fire projectile.
  if ((w->weapon_mode_code != -1) && (w->weapon_mode_code != 1) &&
      !((w->weapon_mode_code - 5U) < 2U)) {
    return;
  }

  ActiveShot shot;
  shot.weapon_id = weapon_bank;
  // World position: the ship's centre. The per-frame sprite anchors in the
  // sprite world (Sprite_AnchorToScreen + DrawSprite's opts.anchor_*) make the
  // gun-fire-point placement available; wiring it needs the fired round to exit
  // at the ship sprite's gun-exit point, which awaits ShipClassDef.gun_exit_pos
  // being decoded (TODO(decomp)).
  shot.pos_x = ship.pos_x;
  shot.pos_y = ship.pos_y;
  // Velocity = heading-projected projectile speed + the ship's own velocity
  // (Math_AddPolarVelocity convention: heading 0 = up/-y, vel=(sin,-cos)*s).
  // WeaponDef.Speed is stored as pixels/frame * 100 (see scenario_data.hpp).
  const float spd = static_cast<float>(w->projectile_speed) / 100.0F;
  shot.vel_x = std::sin(ship.heading) * spd + ship.vel_x;
  shot.vel_y = -std::cos(ship.heading) * spd + ship.vel_y;
  shot.life_frames = std::max(1, static_cast<int>(w->lifetime_ticks));
  state.active_shots.push_back(shot);

  // A round actually spawned: mirror Weapon_FirePlayerWeaponBank's
  // `volley_fired > 0` gate and queue this weapon's fire sound (slot, not
  // resource id) for the spaceflight loop to play through the cached sound.
  if (w->fire_sound >= 0) {
    state.pending_fire_sound_slots.push_back(w->fire_sound);
  }

  // Set the bank cooldown to the fire interval (the original computes this
  // from the weapon's burst/reload fields; here reload_ticks, in reference
  // cadence frames, is used. TODO(decomp): reproduce the exact
  // speed_scalar / burst bookkeeping). The bank cannot fire again until this
  // elapses (NovaWeapon_TickShots counts it down).
  state.weapon_bank_cooldown[weapon_bank] =
      static_cast<float>(std::max(1, static_cast<int>(w->reload_ticks)));
}

void NovaWeapon_FirePlayerPrimary(GameState &state) {
  // Ghidra Ship_HandlePlayerShipControl's primary-fire loop fires every bank
  // that has ammo (weapon_bank_ammo[b] > 0) and is NOT a secondary weapon
  // (Weapon.flags bit 0x2 unset). The player's main Light Blaster bank is such
  // a bank.
  for (std::int16_t b = 0; b < 0x100; ++b) {
    if (BankAmmo(state, b) <= 0) {
      continue;
    }
    const Weapon *w = WeaponAt(state, b);
    if (!w || (w->flags & 0x0002U) != 0) {
      continue; // unmounted or a secondary weapon
    }
    NovaWeapon_FirePlayerWeaponBank(state, b);
  }
}

// Ghidra Shot_HandleShot (0x00435830) time-animated shot-frame branch: for a
// weapon with flags_primary bit 0 set, each frame accumulates the real frame
// time into ShotState.anim_elapsed and, when it crosses the weapon's
// shot_anim_frame_dwell (Ghidra WeaponDef.homing_strength_or_turn_rate; a
// dwell < 1 advances every frame), steps ShotState.frame_cycle_index, wrapping
// at the shot sprite-set's frame count (0, or frame_count-1 when the weapon's
// flags_secondary bit 1 is set). The Light Blaster and the other unguided
// projectiles take the static/heading branch instead, so this only drives
// genuinely time-animated weapon shots.
void NovaWeapon_StepShotAnimation(GameState &state,
                                  ActiveShot &shot,
                                  float frame_time_ms) {
  const Weapon *w = WeaponAt(state, shot.weapon_id);
  if (!w) {
    return;
  }
  // flags_primary bit 0 clear -> static/heading shot-frame path (no animation).
  if ((w->flags & 0x0001U) == 0) {
    return;
  }
  const std::int16_t dwell = w->shot_anim_frame_dwell;
  shot.anim_elapsed += frame_time_ms;
  if (dwell < 1 || shot.anim_elapsed >= static_cast<float>(dwell)) {
    shot.frame_cycle_index += 1;
    shot.anim_elapsed = 0.0F;
  }
  // The caller (DrawShots) clamps the displayed frame to the sprite set's frame
  // count, mirroring Shot_HandleShot's wrap. The reverse-wrap (flags_secondary
  // bit 1 -> frame_count - 1) needs the frame count, which lives on the SDL
  // side; DrawShots owns that clamp once the set is resolved.
}

void NovaWeapon_TickShots(GameState &state, float frame_time_ms) {
  // Advance shots by velocity and lifetime; drop expired rounds. Also step the
  // time-animated shot-frame cycle (Shot_HandleShot animated branch) for
  // weapons that use it; static/heading shot sets are untouched.
  auto &shots = state.active_shots;
  for (auto &shot : shots) {
    shot.pos_x += shot.vel_x;
    shot.pos_y += shot.vel_y;
    if (shot.life_frames > 0) {
      --shot.life_frames;
    }
    NovaWeapon_StepShotAnimation(state, shot, frame_time_ms);
  }
  shots.erase(
      std::remove_if(shots.begin(),
                     shots.end(),
                     [](const ActiveShot &s) { return s.life_frames <= 0; }),
      shots.end());
  // Count every weapon-bank cooldown down toward zero.
  for (float &cd : state.weapon_bank_cooldown) {
    if (cd > 0.0F) {
      --cd;
    }
  }
}

std::string NovaWeapon_BankDisplayName(const GameState &state,
                                       std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return "?";
  }
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (!w || BankAmmo(state, weapon_bank) <= 0) {
    return "?";
  }
  return w->name.empty() ? "?" : w->name;
}

void NovaWeapon_PreloadFireSound(GameState &state,
                                 std::int16_t fire_sound_slot) {
  if (fire_sound_slot < 0 || fire_sound_slot >= 36) {
    return; // no fire sound for this slot
  }
  if (state.weapon_fire_sounds[fire_sound_slot].has_value()) {
    return; // already cached
  }
  // Slot -> snd resource id 200 + slot (see NovaWeapon_FireSoundResourceId).
  const auto resource = NovaResource_LoadSndData(static_cast<std::uint16_t>(
      NovaWeapon_FireSoundResourceId(fire_sound_slot)));
  if (!resource) {
    return; // resource missing; weapon fires silently
  }
  if (auto decoded = NovaSound_Decode(*resource)) {
    state.weapon_fire_sounds[fire_sound_slot] = std::move(*decoded);
    NovaLog::Info("cached weapon fire sound slot {} (snd id {})",
                  fire_sound_slot,
                  NovaWeapon_FireSoundResourceId(fire_sound_slot));
  } else {
    NovaLog::Warn("weapon fire sound slot {} (snd id {}) failed to decode",
                  fire_sound_slot,
                  NovaWeapon_FireSoundResourceId(fire_sound_slot));
  }
}

void NovaWeapon_PreloadOwnedFireSounds(GameState &state) {
  // Scan the rebuilt primary weapon banks and preload each distinct owned
  // weapon's fire sound so a bank never misses its first shot (the original
  // preloads the whole g_gameplay_sound_handle_table at startup; here we only
  // load what the player's banks use).
  for (std::int16_t b = 0; b < 0x100; ++b) {
    const std::int16_t ammo = BankAmmo(state, b);
    if (ammo <= 0) {
      continue;
    }
    const Weapon *w = WeaponAt(state, b);
    if (!w || (w->flags & 0x0002U) != 0) {
      continue; // unmounted or a secondary weapon
    }
    NovaWeapon_PreloadFireSound(state, w->fire_sound);
  }
}

} // namespace game
