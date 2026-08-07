#include "weapon.hpp"

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
  return state
      .weapon_bank_ammo[static_cast<std::size_t>(bank) * kBankStride];
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
    // ModType 1 (kWeapon): mod_val names the mounted weapon; its owned count
    // is the number we hold (so weapon_bank_ammo[b] > 0 gates firing).
    if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kWeapon)) {
      if (o.mod_val >= 0 && o.mod_val < 0x100) {
        BankAmmo(state, o.mod_val) =
            static_cast<std::int16_t>(BankAmmo(state, o.mod_val) + owned);
      }
    }
    // ModType 3 (kAmmo): mod_val names the ammo outfit id whose carried rounds
    // back this weapon; accumulate into the secondary (ammo) counter.
    if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kAmmo)) {
      if (o.mod_val >= 0 && o.mod_val < 0x100) {
        BankSecondary(state, o.mod_val) =
            static_cast<std::int16_t>(BankSecondary(state, o.mod_val) + owned);
      }
    }
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
  // World position: the ship's centre (TODO(decomp): navigate to the gun-exit
  // point on the ship sprite when ShipClassDef.gun_exit_pos is decoded).
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

void NovaWeapon_TickShots(GameState &state) {
  // Advance shots by velocity and lifetime; drop expired rounds.
  auto &shots = state.active_shots;
  for (auto &shot : shots) {
    shot.pos_x += shot.vel_x;
    shot.pos_y += shot.vel_y;
    if (shot.life_frames > 0) {
      --shot.life_frames;
    }
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

} // namespace game
