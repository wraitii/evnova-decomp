#include "weapon.hpp"
#include "weapon_internal.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "collision.hpp"
#include "game_state.hpp"
#include "impact_effects.hpp"
#include "nova_math.hpp"
#include "nova_random.hpp"
#include "outfit.hpp"
#include "preferences.hpp"
#include "ship_ai.hpp"
#include "spaceflight.hpp"
#include "spaceflight_internal.hpp"
#include "targeting.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <random>

namespace game {
using weapon_detail::BankAmmo;
using weapon_detail::BankSecondary;
using weapon_detail::kBankStride;
using weapon_detail::RoundRangeEnvelope;
using weapon_detail::WeaponAt;

// @port 0x00413810 100%
// Ghidra 0x00413810 Weapon_InitShipWeaponBursts. For every mounted bank whose
// weapon has both a burst cycle and a reset cooldown, zeroes the burst counter
// and preloads the bank cooldown to the reset cooldown. The original tests the
// ship class's default_weapon_ammo table; the clean-room uses the live mounted
// count, which EnsureNpcWeaponBanks seeds from the same class data.
void NovaWeapon_InitShipWeaponBursts(GameState &state, Ship &ship) {
  if (ship.ship_instance_id == 0) {
    return; // player hull carries no NPC bank state
  }
  for (std::size_t bank = 0; bank < 0x100; ++bank) {
    const Weapon *w =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (w != nullptr && ship.npc_weapon_count_by_class[bank] > 0 &&
        w->burst_cycle_ticks > 0 && w->burst_reset_cooldown > 0) {
      ship.npc_weapon_bank_burst_counter[bank] = 0;
      ship.npc_weapon_bank_cooldown[bank] =
          static_cast<float>(w->burst_reset_cooldown);
    }
  }
}

// The ship-class default_weapon_ammo/secondary 0x100-entry tables collapse
// here to the flat bank = weapon id - 0x80 model used by the fire path. This
// only overwrites the two per-bank count tables; the caller owns the cached
// loadout marker and any cooldown/burst reset. Shared by the NPC bank
// initializer and Ship_ResetShipToDefaultCombatState's refill arm, whose
// original form (0x0041e240) copies just these two tables.
void NovaWeapon_CopyShipClassStockBanks(const GameState &state, Ship &ship) {
  ship.npc_weapon_count_by_class.fill(0);
  ship.npc_weapon_secondary_count_by_class.fill(0);
  const ShipClass *cls = ShipClassFor(state, ship);
  if (cls == nullptr) {
    return;
  }
  for (const ShipDefaultWeaponBank &stock : cls->stock_weapons) {
    if (stock.weapon_id < 0x80 || stock.weapon_id >= 0x180) {
      continue;
    }
    const auto bank = static_cast<std::size_t>(stock.weapon_id - 0x80);
    ship.npc_weapon_count_by_class[bank] =
        std::max<std::int16_t>(stock.count, 0);
    // -1 is the original unlimited-secondary sentinel.
    ship.npc_weapon_secondary_count_by_class[bank] = stock.ammo_load;
  }
}

// Ghidra Weapon_InitShipWeaponBanksFromShipClass-side initializer used by the
// NPC spawn/AI paths (ship_ai.cpp EnsureNpcWeaponBanks): copies the class
// stock into the per-bank counters, clears the cooldown/burst state, and
// preloads burst cooldowns (Weapon_InitShipWeaponBursts 0x00413810).
void NovaWeapon_EnsureNpcWeaponBanks(GameState &state, Ship &ship) {
  if (ship.ship_instance_id == 0 ||
      ship.npc_weapon_banks_ship_class == ship.ship_class_id) {
    return;
  }
  ship.npc_weapon_bank_cooldown.fill(0.0F);
  ship.npc_weapon_bank_burst_counter.fill(0);
  NovaWeapon_CopyShipClassStockBanks(state, ship);
  if (ShipClassFor(state, ship) != nullptr) {
    NovaWeapon_InitShipWeaponBursts(state, ship);
  }
  ship.npc_weapon_banks_ship_class = ship.ship_class_id;
}

// @port 0x004138a0 100%
// Ghidra 0x004138a0 Weapon_ClassifyShipWeaponAmmoReadiness.
int NovaWeapon_ClassifyAmmoReadiness(const GameState &state, const Ship &ship) {
  int armed = 0;
  int usable = 0;
  int depleted = 0;
  for (std::size_t bank = 0; bank < ship.npc_weapon_count_by_class.size();
       ++bank) {
    if (ship.npc_weapon_count_by_class[bank] <= 0) {
      continue;
    }
    ++armed;
    const Weapon *weapon = state.scenario.Weapon(
        static_cast<std::int16_t>(static_cast<std::int16_t>(bank) + 0x80));
    if (weapon == nullptr) {
      // Missing defs cannot occur for stock-armed banks; the original reads
      // the raw def table, which decodes as a free-energy weapon here.
      continue;
    }
    const std::int16_t cost = weapon->ammo_type;
    if (cost >= 0) {
      // Secondary-ammo weapon: ready while rounds remain.
      ++usable;
      if (ship.npc_weapon_secondary_count_by_class[bank] < 1) {
        ++depleted;
      }
    } else if (cost < -1000) {
      // Fuel weapon: |cost| - 1000 fuel per shot. The original's FCOMP treats
      // fuel == required as depleted (strict > passes).
      ++usable;
      const float required = static_cast<float>(-cost - 1000);
      if (!(ship.fuel_points > required)) {
        ++depleted;
      }
    }
    // cost in [-1000, -1]: free-energy; armed and never depleted.
  }
  if (armed == 0 || (depleted > 0 && depleted == armed)) {
    return 2;
  }
  if (depleted > 0 && depleted == usable) {
    return 1;
  }
  return 0;
}

namespace {

// One axis of the 0x00411600 envelope. The original rounds |delta| with FISTP
// (nearest-even) then backs the integer off by one when it rounded up -- a
// net floor() for the non-negative magnitude (disasm 0x00411620..0x00411659).
[[nodiscard]] int FloorAbsDelta(float delta) {
  return static_cast<int>(std::abs(delta));
}

} // namespace

// @port 0x00411600 100%
// Ghidra 0x00411600 Weapon_IsShipWithinWeaponRangeOfTarget (disasm
// 0x00411773..0x00411800): the reach envelope is
//   floor(|dx|)^2 + floor(|dy|)^2 <= reach^2
// with reach = beam_length_px + 32 for weapon modes 0 and 3 (exact integer
// add), and floor(range_scalar + 32.0f) for every other mode. Disassembly shows
// that mode 10 follows the non-beam range_scalar path.
// weapon_slot >= 0 checks that bank; -1 scans the class's stock-armed banks
// (default ammo > 0, weapon_mode_code < 9) and accepts on the first hit.
bool NovaWeapon_ShipWithinWeaponRangeOfTarget(const GameState &state,
                                              const Ship &ship,
                                              const Ship &target,
                                              std::int16_t weapon_slot) {
  const int dx = FloorAbsDelta(ship.pos_x - target.pos_x);
  const int dy = FloorAbsDelta(ship.pos_y - target.pos_y);
  const int distance_sq = dx * dx + dy * dy;
  auto reach_squared = [](const Weapon &weapon) {
    const int reach =
        weapon.weapon_mode_code == 0 || weapon.weapon_mode_code == 3
            ? weapon.beam_length_px + 32
            : static_cast<int>(std::floor(weapon.range_scalar + 32.0F));
    return reach * reach;
  };
  if (weapon_slot < 0) {
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    if (cls == nullptr) {
      return false;
    }
    // Stock-armed bank lookup: the original walks g_ship_class_defs[class].
    // default_weapon_ammo[0..0xff] > 0; the port's collapsed stock_weapons
    // table is equivalent (one entry per armed bank).
    for (const ShipDefaultWeaponBank &stock : cls->stock_weapons) {
      if (stock.weapon_id < 0x80 || stock.weapon_id >= 0x180 ||
          stock.count <= 0) {
        continue;
      }
      const Weapon *weapon = state.scenario.Weapon(stock.weapon_id);
      if (weapon == nullptr || weapon->weapon_mode_code >= 9) {
        continue;
      }
      if (distance_sq <= reach_squared(*weapon)) {
        return true;
      }
    }
    return false;
  }
  if (weapon_slot >= 0x100) {
    return false;
  }
  const Weapon *weapon =
      state.scenario.Weapon(static_cast<std::int16_t>(weapon_slot + 0x80));
  if (weapon == nullptr) {
    return false;
  }
  return distance_sq <= reach_squared(*weapon);
}

void NovaWeapon_SeedBanksFromShipStock(GameState &state,
                                       std::int16_t ship_class_id) {
  // Seeds the player's 0x100 weapon-bank ammo/secondary counters from a ship
  // class's mounted stock weapons (Ghidra default_weapon_ammo / default_weapon_
  // secondary). For each stock weapon triple {weapon_id, count, ammo_load} the
  // mounted-count goes into weapon_count_by_class[weapon_id-0x80] and any
  // carried rounds (ammo_load, when > 0) into the matching secondary/ammo
  // counter. The original seeds the new ship's banks this way in
  // Menu_RunNewGameFlow and Player_ReplaceShipWithCapturedHull, then calls
  // Weapon_ReconcileOutfitPoolWithWeaponBanks to register the mounted stock
  // guns as owned outfits.
  const ShipClass *ship =
      state.scenario.Ship(static_cast<std::int16_t>(ship_class_id + 0x80));
  if (ship == nullptr) {
    return;
  }
  for (const ShipDefaultWeaponBank &stock : ship->stock_weapons) {
    if (stock.weapon_id < 0x80 || stock.weapon_id > 0x17f) {
      continue; // unmounted bank (weapon_id -1) or out-of-range
    }
    const std::size_t bank = static_cast<std::size_t>(stock.weapon_id - 0x80);
    state.weapon_count_by_class[bank * kBankStride] =
        static_cast<std::int16_t>(stock.count > 0 ? stock.count : 0);
    if (stock.ammo_load > 0) {
      // The original seeds the secondary counter at the mounted weapon's
      // ammo_or_energy_cost_code (out-of-range codes fall back to the bank);
      // see Menu_RunNewGameFlow 0x00489d70. BUGFIX(original), currently
      // ungated: for a mode-99 carried-ship bay AmmoType is the carried ship
      // class id, and the original writes the count into that class slot even
      // though every read/spend path reads the bay's own counter, leaving the
      // bay empty. See docs/known_original_bugs.md ("Hxxx/Exxx ship changes
      // omit carried fighters"). The same fix is applied in
      // NovaWeapon_AddShipClassStockBanks for the C/E/H mission operators.
      std::size_t secondary_bank = bank;
      const Weapon *weapon = state.scenario.Weapon(stock.weapon_id);
      if (weapon != nullptr && weapon->weapon_mode_code != 99 &&
          weapon->ammo_type >= 0 && weapon->ammo_type < 0x100) {
        secondary_bank = static_cast<std::size_t>(weapon->ammo_type);
      }
      state.weapon_secondary_count_by_class[secondary_bank * kBankStride] =
          static_cast<std::int16_t>(stock.ammo_load);
    }
  }
}

void NovaWeapon_AddShipClassStockBanks(GameState &state,
                                       std::int16_t ship_class_id) {
  // Ghidra 0x00449370 'C'/'E'/'H': add the new class's mounted stock weapons
  // on top of the retained loadout. Like NovaWeapon_SeedBanksFromShipStock it
  // keeps a mode-99 carried-ship count in the bay's own counter instead of the
  // class slot the original writes: BUGFIX(original), currently ungated (see
  // the sibling helper and docs/known_original_bugs.md "Hxxx/Exxx ship changes
  // omit carried fighters").
  const ShipClass *ship =
      state.scenario.Ship(static_cast<std::int16_t>(ship_class_id + 0x80));
  if (ship == nullptr) {
    return;
  }
  for (const ShipDefaultWeaponBank &stock : ship->stock_weapons) {
    if (stock.weapon_id < 0x80 || stock.weapon_id > 0x17f || stock.count <= 0) {
      continue;
    }
    const auto bank = static_cast<std::int16_t>(stock.weapon_id - 0x80);
    BankAmmo(state, bank) =
        static_cast<std::int16_t>(BankAmmo(state, bank) + stock.count);
    if (stock.ammo_load > 0) {
      std::int16_t secondary_bank = bank;
      const Weapon *weapon = state.scenario.Weapon(stock.weapon_id);
      if (weapon != nullptr && weapon->weapon_mode_code != 99 &&
          weapon->ammo_type >= 0 && weapon->ammo_type < 0x100) {
        secondary_bank = weapon->ammo_type;
      }
      BankSecondary(state, secondary_bank) = static_cast<std::int16_t>(
          BankSecondary(state, secondary_bank) + stock.ammo_load);
    }
  }
}

// @port 0x00463260 95% correctness
// Ghidra 0x00463260 Weapon_RebuildWeaponBankPoolsFromOwnedOutfits.
void NovaWeapon_RebuildBanksFromOwnedOutfits(GameState &state) {
  // Zero-sweep all 0x100 banks (the original clears both counters).
  for (std::size_t b = 0; b < 0x100; ++b) {
    state.weapon_count_by_class[b * kBankStride] = 0;
    state.weapon_secondary_count_by_class[b * kBankStride] = 0;
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

// @port 0x00462EC0 90% correctness
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
  for (std::size_t i = 0;
       i < state.inventory.outfit_owned_count.size() && i < outfits.size();
       ++i) {
    const std::int16_t owned = state.inventory.outfit_owned_count[i];
    if (owned <= 0) {
      continue;
    }
    const Outfit &o = outfits[i];
    const auto consume = [](std::int16_t &balance, std::int16_t &owned_count) {
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
      consume(bank_secondary[o.mod_val], state.inventory.outfit_owned_count[i]);
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
      for (std::size_t i = 0;
           i < state.inventory.outfit_owned_count.size() && i < outfits.size();
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
      for (std::size_t i = 0;
           i < state.inventory.outfit_owned_count.size() && i < outfits.size();
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
    NovaOutfit_RecomputeOutfitDerivedState(state);
  }
}

// @port 0x00464670 100%
// Ghidra 0x00464670 Weapon_HasLoadedLaunchBayAmmo (disasm
// 0x00464670..0x004646f6): true when the ship's class has a KeyCarried
// fighter and one of its banks holds a mode-99 bay weapon with mounted
// ammo > 0 and loaded secondary > 0 whose ammo_type - 0x80 equals the
// KeyCarried id. -1 KeyCarried short-circuits to false.
bool NovaWeapon_HasLoadedLaunchBayAmmo(const GameState &state,
                                       const Ship &ship) {
  const ShipClass *cls = ShipClassFor(state, ship);
  if (cls == nullptr || cls->key_carried_ship_class < 0) {
    return false;
  }
  const bool is_player = ship.ship_instance_id == 0;
  auto bank_ammo = [&](std::int16_t bank) -> std::int16_t {
    return is_player
               ? BankAmmo(state, bank)
               : ship.npc_weapon_count_by_class[static_cast<std::size_t>(bank)];
  };
  auto bank_secondary = [&](std::int16_t bank) -> std::int16_t {
    return is_player ? BankSecondary(state, bank)
                     : ship.npc_weapon_secondary_count_by_class
                           [static_cast<std::size_t>(bank)];
  };
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const Weapon *w = WeaponAt(state, bank);
    if (w == nullptr || w->weapon_mode_code != 99) {
      continue;
    }
    if (bank_ammo(bank) <= 0 || bank_secondary(bank) <= 0) {
      continue;
    }
    if (static_cast<std::int16_t>(w->ammo_type - 0x80) ==
        cls->key_carried_ship_class) {
      return true;
    }
  }
  return false;
}

namespace {

// Shared predicate of Weapon_HasLaunchBayWeapon (0x00464520) and
// Weapon_FindLaunchBayWeaponBank (0x00464600): a bank is a loaded launch bay
// when its def is mode 99, the bank's mounted counter is > 0, and the carried
// ship class (ammo_type) sets capability Flags 0x8000 (Bible: escape-ship
// type). The original indexes g_ship_class_defs[ammo_type - 0x80] without a
// range check; scenario.Ship() returning nullptr for bad ids is the safe
// superset (mode-99 weapons always carry a valid class id).
constexpr std::int16_t kBayWeaponModeCode = 99;
constexpr std::uint16_t kEscapeShipClassFlag = 0x8000;

[[nodiscard]] bool IsLoadedLaunchBayBank(const GameState &state,
                                         const Ship &ship,
                                         std::int16_t bank) {
  const Weapon *def = WeaponAt(state, bank);
  if (def == nullptr || def->weapon_mode_code != kBayWeaponModeCode) {
    return false;
  }
  const bool is_player = ship.ship_instance_id == 0;
  const std::int16_t mounted =
      is_player
          ? BankAmmo(state, bank)
          : ship.npc_weapon_count_by_class[static_cast<std::size_t>(bank)];
  if (mounted < 1) {
    return false;
  }
  const ShipClass *carried = state.scenario.Ship(def->ammo_type);
  return carried != nullptr &&
         (carried->capability_flags & kEscapeShipClassFlag) != 0;
}

} // namespace

// @port 0x00464520 100%
// Ghidra 0x00464520 Weapon_HasLaunchBayWeapon (disasm 0x00464520..0x0046458e):
// true when any of the ship's 0x100 banks is a loaded launch bay
// (IsLoadedLaunchBayBank). Reads the bank's MOUNTED counter, not the loaded
// secondary, so a stocked-but-unloaded bay still counts. Gates the eject
// transform (Outfit_HasEscapePodOrLaunchBay 0x004644a0) and ship handling.
bool NovaWeapon_HasLaunchBayWeapon(const GameState &state, const Ship &ship) {
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    if (IsLoadedLaunchBayBank(state, ship, bank)) {
      return true;
    }
  }
  return false;
}

// @port 0x00464600 100%
// Ghidra 0x00464600 Weapon_FindLaunchBayWeaponBank (disasm
// 0x00464600..0x0046466e): the first loaded launch-bay bank, or -1.
// Called from Ship_HandleShip for carrier-bay launches.
std::int16_t NovaWeapon_FindLaunchBayWeaponBank(const GameState &state,
                                                const Ship &ship) {
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    if (IsLoadedLaunchBayBank(state, ship, bank)) {
      return bank;
    }
  }
  return -1;
}

// @port 0x00415c10 100%
// Ghidra 0x00415C10 Weapon_HasAnyFireableNonSecondaryWeapon (disasm
// 0x00415c10..0x00415cb2): true when some bank with mounted ammo > 0 holds a
// damaging (MassDmg > 0), non-secondary (Flags2 0x1000 clear) weapon in a
// straight-flight mode the AI can drive (-1/0/3/4/6/7/8 -- the original's
// (mode - 6) < 3 unsigned test includes 6, 7 and 8) that also passes
// Weapon_CanFireWeaponBank (0x00468990).
bool NovaWeapon_HasAnyFireableNonSecondaryWeapon(const GameState &state,
                                                 const Ship &ship) {
  const bool is_player = ship.ship_instance_id == 0;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    const std::int16_t mounted =
        is_player
            ? BankAmmo(state, bank)
            : ship.npc_weapon_count_by_class[static_cast<std::size_t>(bank)];
    if (mounted <= 0) {
      continue;
    }
    const Weapon *def = WeaponAt(state, bank);
    if (def == nullptr || def->mass_damage <= 0) {
      continue;
    }
    if ((def->flags_secondary & 0x1000) != 0) {
      continue;
    }
    switch (def->weapon_mode_code) {
    case -1:
    case 0:
    case 3:
    case 4:
    case 6:
    case 7:
    case 8:
      break;
    default:
      continue;
    }
    if (NovaWeapon_CanFireWeaponBank(state, ship, bank)) {
      return true;
    }
  }
  return false;
}

namespace {

// DAT_00575850 (== DAT_005750b8, the AI call site's scale): mode-1 guided.
inline constexpr float kGuidedMaxRangeScale = 0.85F;
// DAT_005757c8: mode-6 rocket.
inline constexpr float kRocketMaxRangeScale = 0.5F;

} // namespace

// @port 0x0046CEC0 100%
// Ghidra 0x0046CEC0 Weapon_GetShipMaxWeaponRange (disasm
// 0x0046cec0..0x0046d081): the furthest effective reach over the ship's
// armed (bank ammo > 0), fireable (Weapon_CanFireWeaponBank) banks, used by
// the AI state machine to size approach/keep-distance envelopes (the
// 0x00406a4e call site scales it by 0.85). Reach per weapon mode:
//   0/3: beam_length_px (signed); -1/4/7: RoundRangeEnvelope(range_scalar);
//   1: RoundRangeEnvelope(range_scalar * 0.85);
//   6: RoundRangeEnvelope(range_scalar * 0.5); else 0. The original's case
//   list tests mode 7 twice and omits mode 8, so turret projectiles
//   contribute 0 (quirk kept). Max is clamped to 0x7fff.
int NovaWeapon_GetShipMaxWeaponRange(const GameState &state, const Ship &ship) {
  const bool is_player = ship.ship_instance_id == 0;
  auto bank_ammo = [&](std::int16_t bank) -> std::int16_t {
    return is_player
               ? BankAmmo(state, bank)
               : ship.npc_weapon_count_by_class[static_cast<std::size_t>(bank)];
  };
  int max_range = 0;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    if (bank_ammo(bank) <= 0) {
      continue;
    }
    if (!NovaWeapon_CanFireWeaponBank(state, ship, bank)) {
      continue;
    }
    const Weapon *w = WeaponAt(state, bank);
    if (w == nullptr) {
      // The original reads the raw def table; missing defs cannot occur for
      // armed banks in the clean-room model either.
      continue;
    }
    const std::int16_t mode = w->weapon_mode_code;
    int reach = 0;
    if (mode == 0 || mode == 3) {
      reach = w->beam_length_px;
    } else if (mode == -1 || mode == 4 || mode == 7) {
      reach = RoundRangeEnvelope(w->range_scalar);
    } else if (mode == 1) {
      reach = RoundRangeEnvelope(w->range_scalar * kGuidedMaxRangeScale);
    } else if (mode == 6) {
      reach = RoundRangeEnvelope(w->range_scalar * kRocketMaxRangeScale);
    }
    if (reach > max_range) {
      max_range = reach;
    }
  }
  if (max_range >= 0x7fff) {
    max_range = 0x7fff;
  }
  return max_range;
}

// @port 0x00468990 100%
// Ghidra 0x00468990 Weapon_CanFireWeaponBank. See the .hpp contract; the
// 0x004138a0 fuel classifier is kept UNSCALED, matching the original's
// inconsistency.
bool NovaWeapon_CanFireWeaponBank(const GameState &state,
                                  const Ship &ship,
                                  std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return false;
  }
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (!w) {
    return false;
  }
  const bool is_player = ship.ship_instance_id == 0;
  // NPC-only mount gate: a bank whose weapon flags_secondary 0x100 is set
  // never fires on NPC ships; the player is exempt.
  if (!is_player && (w->flags_secondary & 0x0100) != 0) {
    return false;
  }
  // Cloak gate: at the cloak-visibility threshold only weapons flagged
  // 0x4000 (Bible "Weapon can be fired while cloaked") fire.
  if (NovaTargeting_ShipAtCloakVisibilityThreshold(ship) &&
      (w->flags_secondary & 0x4000) == 0) {
    return false;
  }
  // Launch-bay dependency: flags_secondary 0x80 weapons fire only while the
  // class's KeyCarried fighter sits loaded in a bay.
  if ((w->flags_secondary & 0x0080) != 0 &&
      !NovaWeapon_HasLoadedLaunchBayAmmo(state, ship)) {
    return false;
  }
  // Read the ship's own loaded secondary ammo: the player's banks live in the
  // GameState strided arrays (BankSecondary), an NPC's on
  // Ship.npc_weapon_bank_*
  // -- the original reads one ShipState array either way, branching only on
  // which counter index to use (Weapon_CanFireWeaponBank 0x00468990).
  auto loaded_secondary = [&](std::int16_t slot) -> std::int16_t {
    return is_player ? BankSecondary(state, slot)
                     : ship.npc_weapon_secondary_count_by_class
                           [static_cast<std::size_t>(slot)];
  };
  if (w->weapon_mode_code == 99) {
    // Carrier-bay weapon: needs a loaded ship in the firing bank's counter.
    return loaded_secondary(weapon_bank) >= 1;
  }
  // Ammo/energy requirement. ammo_type (Ghidra ammo_or_energy_cost_code) in
  // [0,255] needs carried ammunition; [-999,-1] is free-energy (no check);
  // <= -1000 draws fuel. The original reads the COST bank's counter for the
  // player, but the FIRING bank's for an NPC (0x00468990's
  // ship_instance_id==0 branch) -- reproduced below.
  const int cost = w->ammo_type;
  if (cost >= 0 && cost <= 0xff) {
    const std::int16_t slot =
        is_player ? static_cast<std::int16_t>(cost) : weapon_bank;
    if (loaded_secondary(slot) < 1) {
      return false;
    }
  } else if (cost <= -1000) {
    // Fuel-drawn: per-shot fuel = (|cost| - 1000) * 0.1 (DAT_00575810);
    // the original's FCOMPP passes on equality and unordered. Note the
    // readiness classifier 0x004138a0 compares against |cost| - 1000
    // UNSCALED -- an original inconsistency preserved on both sides.
    const float required = static_cast<float>(-cost - 1000) * 0.1F;
    if (ship.fuel_points < required) {
      return false;
    }
  }
  return true;
}

namespace {

// Eligibility of bank `b` as a selectable secondary: loaded, flagged secondary
// (flags bit 0x2), a mode below 9 or the carrier-bay mode 99, and -- when the
// weapon's flags_secondary bit 0x800 (must-stay-fireable) is set -- still
// passing Weapon_CanFireWeaponBank. Shared by the eligibility count and the
// wrapped cycle walk (0x0044eab4).
[[nodiscard]] bool IsEligibleSecondaryBank(const GameState &state,
                                           const Ship &player,
                                           std::int16_t bank) {
  if (BankAmmo(state, bank) <= 0) {
    return false;
  }
  const Weapon *w = WeaponAt(state, bank);
  if (w == nullptr || (w->flags & 0x0002U) == 0) {
    return false;
  }
  if (!(w->weapon_mode_code < 9 || w->weapon_mode_code == 99)) {
    return false;
  }
  if ((w->flags_secondary & 0x0800U) != 0 &&
      !NovaWeapon_CanFireWeaponBank(state, player, bank)) {
    return false;
  }
  return true;
}

} // namespace

// @port 0x0044BEB0 75% gameplay,verify,synthetic
// Ghidra PlayerTick_WeaponCommands, internal region of
// Ship_HandlePlayerShipCore 0x0044AA70. Synthetic CFG: 0x0044BEB7 ->
// 0x0044C0B1. Starting at the 0x0044BEB0 label is invalid because 0x0044BEB7
// is also entered from 0x0044BE2B; the preceding 0x0044BE15 -> 0x0044BEB7
// region is the separate nearest-target command. The weapon region includes
// PlayerTick_WeaponCycleContinuation at 0x0044EAB4 but excludes the face-target
// command. Dispatch order matches the decompile:
// primary fire, selected-secondary fire, unfirable-bank auto-clear, the
// secondary-bank cycle, and the clear-selection arm. The cooldown-decay tail
// runs unconditionally in NovaWeapon_TickPlayerWeaponBankCooldowns.
void NovaWeapon_TickPlayerWeaponCommands(GameState &state,
                                         const PlayerWeaponCommandInput &input,
                                         float /*elapsed_ticks*/) {
  Ship &player = state.player;
  NovaWeapon_SelectTurretTargetWithinArc(state, player);
  // Fire arms require the station-hold and maneuver timers to be expired and
  // the ship not disabled (disabled / derelict-government gate).
  const bool controls_live = player.ai_station_hold_timer <= 0.0F;
  const bool maneuver_done = player.ai_maneuver_timer_ms <= 0.0F;
  const bool fire_restricted = NovaAiShip_IsDisabled(state, player);

  // Primary fire: every bank with ammo whose weapon is not a secondary
  // (flags bit 0x2 clear).
  if (input.fire_primary_held && controls_live && maneuver_done &&
      !fire_restricted) {
    for (std::int16_t b = 0; b < 0x100; ++b) {
      if (BankAmmo(state, b) <= 0) {
        continue;
      }
      const Weapon *w = WeaponAt(state, b);
      if (w == nullptr || (w->flags & 0x0002U) != 0) {
        continue;
      }
      NovaWeapon_FirePlayerWeaponBank(state, b);
    }
  }

  // Secondary fire: the currently selected bank.
  if (input.fire_secondary_held && controls_live &&
      player.active_weapon_bank_slot != -1 && maneuver_done &&
      !fire_restricted) {
    NovaWeapon_FirePlayerWeaponBank(state, player.active_weapon_bank_slot);
  }

  // Auto-clear the selection when the selected weapon carries flags_primary
  // 0x800 and can no longer fire (e.g. its ammo ran out).
  if (player.active_weapon_bank_slot != -1) {
    const Weapon *w = WeaponAt(state, player.active_weapon_bank_slot);
    if (w != nullptr && (w->flags & 0x0800U) != 0 &&
        !NovaWeapon_CanFireWeaponBank(
            state, player, player.active_weapon_bank_slot)) {
      player.active_weapon_bank_slot = -1;
      // g_shipAvailabilityCachesDirty = 1: the port has no availability
      // cache; availability is recomputed per query.
    }
  }

  // @port 0x0044EAB4 90% gameplay,synthetic
  // Ghidra 0x0044eab4 PlayerTick_WeaponCycleContinuation (synthetic region of
  // 0x0044aa70): the reordered secondary-bank cycle. g_shipAvailability-
  // CachesDirty has no port counterpart (no cache; recomputed per query).
  // Secondary-bank cycle (edge-resolved by the caller against
  // g_playerSecondaryCycleCommandLatch). Counts eligible banks; with none the
  // denial cue plays, otherwise the accept cue plays and the selection walks
  // forward/backward with wrap, skipping ineligible banks, until one
  // qualifies. The original sets g_shipAvailabilityCachesDirty here as well.
  if (input.cycle_secondary) {
    std::int16_t eligible_count = 0;
    for (std::int16_t b = 0; b < 0x100; ++b) {
      if (IsEligibleSecondaryBank(state, player, b)) {
        ++eligible_count;
      }
    }
    if (eligible_count < 1) {
      state.pending_ui_sounds.push_back({3, 1});
    } else {
      state.pending_ui_sounds.push_back({2, 1});
      const std::int16_t direction = input.cycle_secondary_backwards
                                         ? static_cast<std::int16_t>(-1)
                                         : static_cast<std::int16_t>(1);
      std::int16_t slot = player.active_weapon_bank_slot;
      bool found = false;
      do {
        // The original wraps via [0,0x100) correction loops after a signed
        // step; equivalent to modulo on the int16 range including -1.
        slot = static_cast<std::int16_t>((slot + direction + 0x100) & 0xff);
        found = IsEligibleSecondaryBank(state, player, slot);
      } while (!found);
      player.active_weapon_bank_slot = slot;
    }
  }

  // Clear-selection arm (binding slot 1).
  if (input.clear_secondary && player.active_weapon_bank_slot != -1) {
    player.active_weapon_bank_slot = -1;
    state.pending_ui_sounds.push_back({2, 1});
  }
}

// Ghidra 0x0044aa70 cooldown-decay tail of PlayerTick_WeaponCommands: per
// bank with ammo > 0, clamp expired cooldowns to zero, otherwise decay by the
// frame tick scale; and while the player is ionized, banks whose weapon has
// flags_quaternary 0x20 are pinned at a 1-tick cooldown (0x3f800000),
// disabling them until the charge decays.
void NovaWeapon_TickPlayerWeaponBankCooldowns(GameState &state,
                                              float elapsed_ticks) {
  for (std::int16_t b = 0; b < 0x100; ++b) {
    if (BankAmmo(state, b) <= 0) {
      continue;
    }
    float &cooldown = state.weapon_bank_cooldown[b];
    if (cooldown <= 0.0F) {
      cooldown = 0.0F;
    } else {
      cooldown = std::max(0.0F, cooldown - elapsed_ticks);
    }
    const Weapon *w = WeaponAt(state, b);
    if (w != nullptr && (w->flags_quaternary & 0x0020U) != 0 &&
        state.player.ionization_points > 0.0F) {
      cooldown = 1.0F;
    }
  }
}

// Ship_HandleShip 0x00433050 per-bank cooldown tail (primary site
// Stub_HandleShips, src/game/spaceflight.cpp). Mirrors the original loop: only
// banks with ammo > 0 decay, an ionized ship pins flags_quaternary 0x20 banks
// at a 1-tick cooldown, and a targetless ship reloads every mode-99 launch bay.
void NovaWeapon_TickNpcWeaponBanks(GameState &state,
                                   Ship &ship,
                                   float elapsed_ticks) {
  const float ticks = std::max(0.0F, elapsed_ticks);
  // The original gates the pin on trunc(Ship_GetIonizationIntensity) > 0, i.e.
  // a fully charged ionization pool. NovaShip_IonizationIntensity is the same
  // helper the ionization block uses.
  bool ionized = false;
  if (const ShipClass *cls = ShipClassFor(state, ship); cls != nullptr) {
    ionized =
        spaceflight_detail::NovaShip_IonizationIntensity(ship, *cls) >= 1.0F;
  }
  for (std::size_t bank = 0; bank < ship.npc_weapon_bank_cooldown.size();
       ++bank) {
    if (ship.npc_weapon_count_by_class[bank] <= 0) {
      continue;
    }
    float &cooldown = ship.npc_weapon_bank_cooldown[bank];
    if (cooldown <= 0.0F) {
      cooldown = 0.0F;
    } else {
      cooldown = std::max(0.0F, cooldown - ticks);
    }
    const std::int16_t bank_id = static_cast<std::int16_t>(bank);
    const Weapon *w = WeaponAt(state, bank_id);
    if (w != nullptr && (w->flags_quaternary & 0x0020U) != 0U && ionized) {
      cooldown = 1.0F;
    }
  }
  if (ship.primary_target_ship_slot == -1) {
    for (std::size_t bank = 0; bank < ship.npc_weapon_bank_cooldown.size();
         ++bank) {
      const std::int16_t bank_id = static_cast<std::int16_t>(bank);
      const Weapon *w = WeaponAt(state, bank_id);
      if (w != nullptr && w->weapon_mode_code == 99 &&
          ship.npc_weapon_count_by_class[bank] > 0) {
        ship.npc_weapon_bank_cooldown[bank] = w->reload_ticks;
      }
    }
  }
}

// Ghidra 0x0046f2c0 Weapon_GetWeaponBurstAttempts: how many shots one trigger
// pull fires. Non-burst weapons (flags_primary 0x40 clear) get exactly one; a
// burst bank starts from the mounted ammo count, capped by the loaded
// secondary of its cost bank (the firing bank's own counter for mode-99 bays)
// or, for fuel-drawn weapons (ammo_type < -999), by fuel_points divided by
// the per-shot fuel ((|cost| - 1000) * 0.1). The player's banks live in the
// GameState strided arrays; an NPC's on Ship.npc_weapon_bank_*
// (NovaWeapon_FireNpcWeaponBank keeps its own inline copy).
[[nodiscard]] int NovaWeapon_GetWeaponBurstAttempts(const GameState &state,
                                                    const Ship &ship,
                                                    std::int16_t weapon_bank) {
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (w == nullptr) {
    return 0;
  }
  const bool is_player = ship.ship_instance_id == 0;
  auto bank_ammo = [&](std::int16_t slot) -> std::int16_t {
    return is_player
               ? BankAmmo(state, slot)
               : ship.npc_weapon_count_by_class[static_cast<std::size_t>(slot)];
  };
  auto bank_secondary = [&](std::int16_t slot) -> std::int16_t {
    return is_player ? BankSecondary(state, slot)
                     : ship.npc_weapon_secondary_count_by_class
                           [static_cast<std::size_t>(slot)];
  };
  if ((w->flags & 0x0040U) == 0U) {
    return 1;
  }
  int attempts = std::max(0, static_cast<int>(bank_ammo(weapon_bank)));
  if ((w->flags_tertiary & 0x0001U) == 0U) {
    return attempts;
  }
  int cap;
  if (w->weapon_mode_code == 99) {
    cap = bank_secondary(weapon_bank);
  } else {
    const int cost = w->ammo_type;
    if (cost >= 0 && cost <= 0xff) {
      cap = bank_secondary(static_cast<std::int16_t>(cost));
    } else if (cost < -999) {
      if (ship.fuel_points <= 0.0F) {
        return 0;
      }
      const float per_shot =
          static_cast<float>(-cost - 1000) * 0.1F; // k fuel scale 0.1
      const float affordable = ship.fuel_points / per_shot;
      cap = static_cast<int>(affordable);
    } else {
      return attempts;
    }
  }
  return std::min(attempts, std::max(0, cap));
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

std::int16_t NovaWeapon_BankAmmoCount(const GameState &state,
                                      std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return -1;
  }
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (!w) {
    return -1;
  }
  // Energy/unlimited weapons show no count (NovaUi_DrawActiveWeaponAmmoPanel's
  // early cases).
  if (w->ammo_type == -1 || (w->flags_secondary & 0x40U) != 0U) {
    return -1;
  }
  // A special (mode 99) or out-of-range ammo_type weapon reads its own bank's
  // secondary counter; a normal weapon reads the ammo counter of the weapon
  // bank whose id equals its ammo_type. The original hands the counter straight
  // to PascalString_FromUInt (no display clamp), so return the raw value.
  const std::int16_t source_bank =
      (w->ammo_type < 0 || w->ammo_type > 0xff || w->weapon_mode_code == 99)
          ? weapon_bank
          : w->ammo_type;
  return BankSecondary(state, source_bank);
}
} // namespace game
