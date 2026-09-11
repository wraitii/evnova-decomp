#include "weapon.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "collision.hpp"
#include "game_state.hpp"
#include "impact_effects.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"
#include "spaceflight.hpp"
#include "targeting.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

namespace game {

namespace {

// Shot_UpdateShotGuidance constants (typed + pre-commented in the Ghidra DB).
inline constexpr float kGuidanceLifeGateF64 = 30.0F;      // 0x005754c0
inline constexpr float kGuidanceLifeGateLooseF64 = 15.0F; // 0x00575400
inline constexpr float kJamTurnSignF32 = -1.0F;           // 0x00575350
inline constexpr float kRocketBlendOldF32 = 94.0F;        // 0x0057540c
inline constexpr float kRocketBlendNewF32 = 5.0F;         // 0x00575408
inline constexpr float kOnePercentF64 = 0.01F;            // 0x00575368
inline constexpr float kBombNoseTurnRate = 1.0F;          // 0x00575318

[[nodiscard]] const ShipClass *ShipClassFor(const GameState &state,
                                            const Ship &ship) {
  return state.scenario.Ship(
      static_cast<std::int16_t>(ship.ship_class_id + 0x80));
}

} // namespace

// Ghidra 0x00413810 Weapon_InitShipWeaponBursts (burst-counter preload slice)
// plus the ship-class stock-loadout copy shared by the NPC spawn/AI paths
// (ship_ai.cpp EnsureNpcWeaponBanks). The original's per-class default
// weapon_ammo/secondary 0x100-entry tables collapse here to the flat
// bank = weapon id - 0x80 model used by the fire path.
void NovaWeapon_EnsureNpcWeaponBanks(GameState &state, Ship &ship) {
  if (ship.ship_instance_id == 0 ||
      ship.npc_weapon_banks_ship_class == ship.ship_class_id) {
    return;
  }
  ship.npc_weapon_bank_ammo.fill(0);
  ship.npc_weapon_bank_secondary.fill(0);
  ship.npc_weapon_bank_cooldown.fill(0.0F);
  ship.npc_weapon_bank_burst_counter.fill(0);
  const ShipClass *cls = ShipClassFor(state, ship);
  if (cls != nullptr) {
    for (const ShipDefaultWeaponBank &stock : cls->stock_weapons) {
      if (stock.weapon_id < 0x80 || stock.weapon_id >= 0x180) {
        continue;
      }
      const auto bank = static_cast<std::size_t>(stock.weapon_id - 0x80);
      ship.npc_weapon_bank_ammo[bank] = std::max<std::int16_t>(stock.count, 0);
      // -1 is the original unlimited-secondary sentinel.
      ship.npc_weapon_bank_secondary[bank] = stock.ammo_load;
    }
    // Weapon_InitShipWeaponBursts (0x00413810): a configured burst weapon
    // (burst_cycle AND burst_reset_cooldown) starts with a zeroed burst
    // counter and its cooldown preloaded to the reset cooldown.
    for (std::size_t bank = 0; bank < 0x100; ++bank) {
      const Weapon *w =
          state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
      if (w != nullptr && ship.npc_weapon_bank_ammo[bank] > 0 &&
          w->burst_cycle_ticks > 0 && w->burst_reset_cooldown > 0) {
        ship.npc_weapon_bank_burst_counter[bank] = 0;
        ship.npc_weapon_bank_cooldown[bank] =
            static_cast<float>(w->burst_reset_cooldown);
      }
    }
  }
  ship.npc_weapon_banks_ship_class = ship.ship_class_id;
}

// Ghidra 0x004138a0 Weapon_ClassifyShipWeaponAmmoReadiness.
int NovaWeapon_ClassifyAmmoReadiness(const GameState &state, const Ship &ship) {
  int armed = 0;
  int usable = 0;
  int depleted = 0;
  for (std::size_t bank = 0; bank < ship.npc_weapon_bank_ammo.size(); ++bank) {
    if (ship.npc_weapon_bank_ammo[bank] <= 0) {
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
      if (ship.npc_weapon_bank_secondary[bank] < 1) {
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

// Game-bearing in degrees (0 = up, clockwise) from (x1,y1) to (x2,y2). Local
// twin of ship_ai.cpp's BearingDeg (GameState angle convention), wrapped to
// [0, 360).
float BearingDeg(float x1, float y1, float x2, float y2) {
  const float rad = std::atan2(x2 - x1, -(y2 - y1));
  float deg = rad * (180.0F / 3.14159265358979323846F);
  deg = std::fmod(deg, 360.0F);
  if (deg < 0.0F) {
    deg += 360.0F;
  }
  return deg;
}

// The weapon loaded in a bank, or nullptr when the bank holds nothing valid.
const Weapon *WeaponAt(const GameState &state, std::int16_t bank) {
  // Banks are indexed by zero-based weapon id; the scenario Weapon() lookup
  // uses the 0x80.. residue. Bank slot b == weapon resource id -- but our
  // clean Weapon table is indexed by (weapon id - 0x80). Resolve via the
  // resource-id offset: bank slot `b` corresponds to weapon resource `b+0x80`.
  return state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
}

// ---- Guidance math (Math_* helpers cited from Shot_UpdateShotGuidance) ----

// Ghidra 0x0046b210 Math_ShortestAngleDeltaDeg: absolute shortest angular
// distance between two game-degree bearings, in [0,180]. The original loses
// the rotation sign; direction is recovered by comparing the raw wrapped
// delta against the 181-degree split (see NovaWeapon_TurnShotToward).
int ShortestAngleDeltaDeg(int from, int to) {
  int delta = std::abs(from - to);
  if ((from > 179) != (to > 179)) {
    delta = 360 - delta;
  }
  if (delta > 180) {
    delta = 360 - delta;
  }
  return delta;
}

// The original's ROUND-then-wrap heading quantization: round to nearest
// integer degree, then a single-step wrap into [0,360) (k_wrap_360_f32
// 0x005753b8). The per-frame turn is far smaller than 360 deg, so one
// subtraction matches the original's do-while.
int RoundHeadingDeg(float deg) {
  const int rounded = static_cast<int>(std::lround(deg));
  return (rounded % 360 + 360) % 360;
}

// Ghidra 0x0043b4a0 Math_AddPolarVelocity: ADD a polar vector onto an XY
// velocity pair using the game angle convention (sin for x, -cos for y).
void AddPolarVelocity(float bearing_deg,
                      float speed,
                      float &vel_x,
                      float &vel_y) {
  const float rad = bearing_deg * (3.14159265358979323846F / 180.0F);
  vel_x += std::sin(rad) * speed;
  vel_y -= std::cos(rad) * speed;
}

// Ghidra 0x0043b6a0 Math_ClampVelocityComponents: componentwise box clamp to
// +/- max_speed.
void ClampVelocityComponents(float &vel_x, float &vel_y, float max_speed) {
  vel_x = std::clamp(vel_x, -max_speed, max_speed);
  vel_y = std::clamp(vel_y, -max_speed, max_speed);
}

// Shared tail of every guidance state: wrap the heading into [0,360), then
// rebuild the shot velocity from scratch as polar(heading, projectile_speed)
// clamped componentwise to the projectile speed.
void WrapHeadingAndRebuildVelocity(const Weapon &weapon, ActiveShot &shot) {
  shot.heading_deg = std::fmod(shot.heading_deg, 360.0F);
  if (shot.heading_deg < 0.0F) {
    shot.heading_deg += 360.0F;
  }
  const float speed = weapon.projectile_speed / 100.0F;
  float vel_x = 0.0F;
  float vel_y = 0.0F;
  AddPolarVelocity(static_cast<float>(RoundHeadingDeg(shot.heading_deg)),
                   speed,
                   vel_x,
                   vel_y);
  ClampVelocityComponents(vel_x, vel_y, speed);
  shot.vel_x = vel_x;
  shot.vel_y = vel_y;
}

// One guidance turn step (shared by states 0/1/999): rotate the shot heading
// toward `bearing` by at most turn_rate degrees, choosing the shorter wrap
// direction (the original splits at 181 degrees).
void TurnShotToward(ActiveShot &shot,
                    int bearing,
                    float turn_rate,
                    float frame_scale) {
  const int heading = RoundHeadingDeg(shot.heading_deg);
  const int delta = ShortestAngleDeltaDeg(bearing, heading);
  if (static_cast<int>(std::lround(std::abs(turn_rate))) >= delta) {
    return; // within one tick's turn of the bearing; hold
  }
  int forward = bearing - heading;
  forward %= 360;
  if (forward < 0) {
    forward += 360;
  }
  if (forward < 181) {
    shot.heading_deg += turn_rate * frame_scale;
  } else {
    shot.heading_deg -= turn_rate * frame_scale;
  }
}

// NovaRandom_Range([0,n)) stand-in on the GameState LCG.
int RandomBelow(GameState &state, int n) {
  if (n <= 0) {
    return 0;
  }
  return std::uniform_int_distribution<int>{0, n - 1}(state.rng);
}

} // namespace

void NovaWeapon_ClearTransientCombatState(GameState &state) {
  // The original retires every ShotState during Stellar_TravelToSystem and
  // marks all live beam records inactive during the landing transition.
  // Ship slots (including the player's) get their jamming-score cache reseeded
  // to -1 at allocation; the port resets the persistent player ship here and
  // on outfit changes (OutfitMarkStatsDirty) instead.
  state.player.jamming_score.fill(-1);
  state.active_shots.clear();
  for (BeamHit &beam : state.beam_hit_queue) {
    beam = BeamHit{};
  }
  for (ImpactEffectInstance &effect : state.impact_effect_instances) {
    effect = ImpactEffectInstance{};
  }
  state.pending_fire_sounds.clear();
  state.pending_impact_sounds.clear();
}

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
    state.weapon_bank_ammo[bank * kBankStride] =
        static_cast<std::int16_t>(stock.count > 0 ? stock.count : 0);
    if (stock.ammo_load > 0) {
      state.weapon_bank_secondary[bank * kBankStride] =
          static_cast<std::int16_t>(stock.ammo_load);
    }
  }
}

// Ghidra 0x00463260 Weapon_RebuildWeaponBankPoolsFromOwnedOutfits.
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
    OutfitMarkStatsDirty(state);
  }
}

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
               : ship.npc_weapon_bank_ammo[static_cast<std::size_t>(bank)];
  };
  auto bank_secondary = [&](std::int16_t bank) -> std::int16_t {
    return is_player
               ? BankSecondary(state, bank)
               : ship.npc_weapon_bank_secondary[static_cast<std::size_t>(bank)];
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
      is_player ? BankAmmo(state, bank)
                : ship.npc_weapon_bank_ammo[static_cast<std::size_t>(bank)];
  if (mounted < 1) {
    return false;
  }
  const ShipClass *carried = state.scenario.Ship(def->ammo_type);
  return carried != nullptr &&
         (carried->capability_flags & kEscapeShipClassFlag) != 0;
}

} // namespace

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
        is_player ? BankAmmo(state, bank)
                  : ship.npc_weapon_bank_ammo[static_cast<std::size_t>(bank)];
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

// The FISTP(nearest-even) + signed/unsigned backoff idiom used by the range
// functions (disasm 0x0046cfe1..0x0046d01f): nets to floor() for value >= 0
// and to ceil() for value < 0. Range data is non-negative, so this is
// floor() in practice (same idiom as FloorAbsDelta at 0x00411600).
[[nodiscard]] int RoundRangeEnvelope(float value) {
  int rounded = static_cast<int>(std::nearbyint(value));
  const float remainder = value - static_cast<float>(rounded);
  if (value >= 0.0F) {
    if (remainder < 0.0F) { // FISTP rounded up
      --rounded;
    }
  } else if (remainder > 0.0F) { // FISTP rounded down
    ++rounded;
  }
  return rounded;
}

// DAT_00575850 (== DAT_005750b8, the AI call site's scale): mode-1 guided.
inline constexpr float kGuidedMaxRangeScale = 0.85F;
// DAT_005757c8: mode-6 rocket.
inline constexpr float kRocketMaxRangeScale = 0.5F;

} // namespace

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
               : ship.npc_weapon_bank_ammo[static_cast<std::size_t>(bank)];
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
    return is_player
               ? BankSecondary(state, slot)
               : ship.npc_weapon_bank_secondary[static_cast<std::size_t>(slot)];
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

// The displayed rotation frame of the ship's sprite. TODO(decomp): the
// original reads the sprite object's rotation counter (sprite+0x68) modulo
// FramesPer; the port derives the same displayed frame from the heading with
// the renderer's mapping (spaceflight_view FrameForHeading).
[[nodiscard]] int RotationFrameForShip(const Ship &ship,
                                       int frames_per_rotation) {
  if (frames_per_rotation < 1) {
    return 0;
  }
  const float kTwoPi = 6.28318530717958647692F;
  const float normalized = std::fmod(ship.heading + kTwoPi, kTwoPi);
  int frame =
      static_cast<int>(std::lround(normalized / kTwoPi *
                                   static_cast<float>(frames_per_rotation))) %
      frames_per_rotation;
  if (frame < 0) {
    frame += frames_per_rotation;
  }
  return frame;
}

// Ghidra 0x0046c5c0 Weapon_ApplyTurretSpreadVelocity: apply one turret
// group's quadrant-barrel muzzle displacement to a position. Accumulates
// polar(forward, ship_bearing) + polar(lateral, ship_bearing + 90 mod 360)
// (barrel i = group*4+quadrant; ShipClass.muzzle_forward/lateral), scales x
// by the NEAR pair (muzzle_scale_near_*) when the accumulated y < 0 and the
// FAR pair otherwise, then pos += (scaled_x, scaled_y - drop).
void ApplyTurretSpreadVelocity(const ShipClass &cls,
                               float &pos_x,
                               float &pos_y,
                               std::int16_t ship_bearing_deg,
                               int turret_group_id,
                               int quadrant_index) {
  if (turret_group_id < 0 || turret_group_id >= 4 || quadrant_index < 0 ||
      quadrant_index >= 4) {
    return;
  }
  float acc_x = 0.0F;
  float acc_y = 0.0F;
  AddPolarVelocity(
      static_cast<float>(ship_bearing_deg),
      static_cast<float>(cls.muzzle_forward[turret_group_id][quadrant_index]),
      acc_x,
      acc_y);
  // (bearing + 90) truncated modulo 360 (C signed-division semantics).
  int side_bearing = static_cast<int>(ship_bearing_deg) + 90;
  side_bearing -= 360 * (side_bearing / 360);
  AddPolarVelocity(
      static_cast<float>(side_bearing),
      static_cast<float>(cls.muzzle_lateral[turret_group_id][quadrant_index]),
      acc_x,
      acc_y);
  const float scale_x =
      acc_y < 0.0F ? cls.muzzle_scale_near_x : cls.muzzle_scale_far_x;
  const float scale_y =
      acc_y < 0.0F ? cls.muzzle_scale_near_y : cls.muzzle_scale_far_y;
  pos_x += acc_x * scale_x;
  pos_y += acc_y * scale_y -
           static_cast<float>(cls.muzzle_drop[turret_group_id][quadrant_index]);
}

// Ghidra 0x0046c4e0 Weapon_ChooseBestTurretQuadrantForTarget: apply each of
// the four quadrant barrel offsets to the muzzle position and return the
// quadrant minimizing the squared distance to the target point. Returns 0
// (not -1) for invalid arguments -- original quirk.
[[nodiscard]] int
ChooseBestTurretQuadrantForTarget(const ShipClass &cls,
                                  float muzzle_x,
                                  float muzzle_y,
                                  std::int16_t ship_bearing_deg,
                                  int turret_group_id,
                                  float target_x,
                                  float target_y) {
  if (turret_group_id < 0 || turret_group_id >= 4) {
    return 0;
  }
  int best = -1;
  float best_dist_sq = 0.0F;
  for (int quadrant = 0; quadrant < 4; ++quadrant) {
    float x = muzzle_x;
    float y = muzzle_y;
    ApplyTurretSpreadVelocity(
        cls, x, y, ship_bearing_deg, turret_group_id, quadrant);
    const float dx = x - target_x;
    const float dy = y - target_y;
    const float dist_sq = dx * dx + dy * dy;
    if (best == -1 || dist_sq < best_dist_sq) {
      best = quadrant;
      best_dist_sq = dist_sq;
    }
  }
  return best;
}

} // namespace

// Ghidra 0x0046c320 Weapon_SelectTurretQuadrant: pick and advance the firing
// barrel quadrant for the weapon's turret group, offsetting muzzle_pos to the
// barrel. The bearing is the DISPLAYED rotation frame scaled back to degrees
// (frame * 360 / frames_per_rotation, floor idiom). When the weapon has
// flags_tertiary 0x10 and a target position is supplied, the quadrant is
// replaced by the target-nearest barrel (0x0046c4e0); an out-of-range stored
// quadrant re-rolls randomly in [0,4). Returns the quadrant used, or -1 when
// the weapon has no valid turret group. The quadrant advances modulo 4 after
// every shot (per-ship state, Ship.muzzle_quadrant).
std::int16_t NovaWeapon_SelectTurretQuadrant(GameState &state,
                                             Ship &ship,
                                             std::int16_t weapon_id,
                                             float &muzzle_x,
                                             float &muzzle_y,
                                             const float *target_pos) {
  const Weapon *w = WeaponAt(state, weapon_id);
  const ShipClass *cls = ShipClassFor(state, ship);
  if (w == nullptr || cls == nullptr) {
    return -1;
  }
  const int group = static_cast<int>(w->turret_group_id);
  if (group < 0 || group >= 4) {
    return -1;
  }
  const int frames =
      cls->frames_per_rotation > 0 ? cls->frames_per_rotation : 36;
  const int frame = RotationFrameForShip(ship, frames);
  const std::int16_t ship_bearing_deg =
      static_cast<std::int16_t>(RoundRangeEnvelope(
          static_cast<float>(frame) * (360.0F / static_cast<float>(frames))));
  auto &quadrant_state = ship.muzzle_quadrant[static_cast<std::size_t>(group)];
  if ((w->flags_tertiary & 0x10) != 0 && target_pos != nullptr) {
    quadrant_state = static_cast<std::int8_t>(
        ChooseBestTurretQuadrantForTarget(*cls,
                                          muzzle_x,
                                          muzzle_y,
                                          ship_bearing_deg,
                                          group,
                                          *target_pos,
                                          *(target_pos + 1)));
  }
  if (quadrant_state < 0 || quadrant_state > 3) {
    quadrant_state = static_cast<std::int8_t>(RandomBelow(state, 4));
  }
  const int quadrant = quadrant_state;
  ApplyTurretSpreadVelocity(
      *cls, muzzle_x, muzzle_y, ship_bearing_deg, group, quadrant);
  quadrant_state = static_cast<std::int8_t>((quadrant + 1) & 3);
  // The original re-validates after the advance (unreachable with the mod-4
  // update) and re-rolls randomly when still invalid.
  if (quadrant_state < 0 || quadrant_state > 3) {
    quadrant_state = static_cast<std::int8_t>(RandomBelow(state, 4));
  }
  return static_cast<std::int16_t>(quadrant);
}

int NovaWeapon_SpawnProjectile(GameState &state,
                               std::int16_t owner_ship_slot,
                               std::int16_t target_ship_slot,
                               std::int16_t weapon_id,
                               bool spawn_without_owner,
                               bool apply_random_spread) {
  if (weapon_id < 0 || weapon_id >= 0x100) {
    return -1;
  }
  const Weapon *w = WeaponAt(state, weapon_id);
  if (w == nullptr) {
    return -1;
  }

  const bool owner_in_range =
      owner_ship_slot >= 0 &&
      owner_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips);
  if (!spawn_without_owner && !owner_in_range) {
    return -1;
  }

  ActiveShot shot;
  shot.weapon_id = weapon_id;
  // Shot_SpawnShotFromWeapon stores the owner slot even for ownerless
  // (submunition) spawns; spawn_without_owner only suppresses the
  // owner-relative position/velocity/heading/muzzle/turret setup below, which
  // Shot_SpawnLinkedShotsOnImpact overwrites with the impact context.
  shot.owner_ship_slot = owner_ship_slot;
  shot.target_ship_slot = target_ship_slot;
  shot.system_id = owner_in_range
                       ? state.ShipAt(static_cast<std::size_t>(owner_ship_slot))
                             .current_system_id
                       : state.player.current_system_id;
  // Fuse: a weapon without a fuse gets the -1.0 "no fuse" sentinel (the port
  // does not tick fuses yet; the field is carried for that slice).
  shot.fuse_elapsed = w->fuse_ticks < 1 ? -1.0F : 0.0F;
  shot.retarget_cooldown = 0;

  const int mode = w->weapon_mode_code;
  constexpr float kDegPerRad = 180.0F / 3.14159265358979323846F;
  // Shot heading in game degrees throughout; the muzzle geometry converts back
  // to radians locally (Ship.heading is stored in radians).
  float heading = 0.0F;
  // Shot_SpawnShotFromWeapon local_36: modes 4/7/8 fired with a target are
  // "aimed" (bearing to the target, then overwritten by the predictive lead).
  bool aim_led = false;
  // Shot_SpawnShotFromWeapon local_30: parallel-launch side for negative
  // spread weapons (0 = fire along the aim heading; +/-1 = hull heading
  // +/- |spread|, see the velocity block).
  int parallel_side = 0;
  // Weapon_SelectTurretQuadrant result (-1 when the weapon has no turret
  // group or no owner context).
  int turret_quadrant = -1;
  if (owner_in_range && !spawn_without_owner) {
    Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    shot.pos_x = owner.pos_x;
    shot.pos_y = owner.pos_y;
    shot.vel_x = owner.vel_x;
    shot.vel_y = owner.vel_y;
    heading = owner.heading * kDegPerRad;
    const bool target_in_range =
        target_ship_slot >= 0 &&
        target_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips);
    if ((mode == 4 || mode == 7 || mode == 8) && target_in_range) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(target_ship_slot));
      heading = BearingDeg(shot.pos_x, shot.pos_y, target.pos_x, target.pos_y);
      aim_led = true;
    }
    // Player-launched freefall bombs keep only 80% of the ship's velocity
    // (k_bomb_launch_vel_scale_f64 0x005752a0); NPC bays fire a full polar
    // vector instead (see the velocity rules below).
    if (owner_ship_slot == 0 && mode == 5) {
      shot.vel_x *= 0.8F;
      shot.vel_y *= 0.8F;
    }

    // Shot_SpawnShotFromWeapon: Weapon_SelectTurretQuadrant (0x0046c320)
    // picks/advances the barrel quadrant and offsets the spawn position to
    // the muzzle; with a target slot it also feeds the target position for
    // the flags_tertiary 0x10 target-nearest quadrant choice.
    const float *target_pos =
        target_ship_slot >= 0 && target_ship_slot < static_cast<std::int16_t>(
                                                        GameState::kMaxShips)
            ? &state.ShipAt(static_cast<std::size_t>(target_ship_slot)).pos_x
            : nullptr;
    turret_quadrant = NovaWeapon_SelectTurretQuadrant(
        state, owner, weapon_id, shot.pos_x, shot.pos_y, target_pos);
    // Negative shot_random_spread marks parallel multi-barrel launch; the
    // sign of the selected barrel's lateral offset picks the side of the
    // hull heading the volley leaves on (used by the velocity block below).
    if (w->inaccuracy < 0 && turret_quadrant >= 0) {
      const ShipClass *owner_cls = ShipClassFor(state, owner);
      const int group = static_cast<int>(w->turret_group_id);
      if (owner_cls != nullptr && owner_cls->muzzle_ready && group >= 0 &&
          group < 4) {
        const std::int16_t lateral =
            owner_cls->muzzle_lateral[static_cast<std::size_t>(
                group)][static_cast<std::size_t>(turret_quadrant)];
        if (lateral < 0) {
          parallel_side = -1;
        } else if (lateral > 0) {
          parallel_side = 1;
        }
      }
    }
  }

  shot.heading_deg = static_cast<float>(RoundHeadingDeg(heading));

  // Interference confusion (mode 1 + Seeker 0x0008): at launch the system's
  // Interference stat gates a Random(k_interference_scale / frame_scale) roll;
  // a hit latches the 999 weave state for the whole flight.
  if (mode == 1 && (w->flags_quaternary & 0x0008U) != 0U && owner_in_range) {
    const System *system =
        state.scenario.System(static_cast<std::int16_t>(shot.system_id + 0x80));
    if (system != nullptr && system->interference > 0) {
      const int roll_range =
          std::max(1,
                   static_cast<int>(std::lround(
                       100.0F / std::max(0.01F, state.last_frame_tick_scale))));
      if (RandomBelow(state, roll_range) + 1 <= system->interference) {
        shot.retarget_cooldown = 999;
      }
    }
  }

  if (aim_led && owner_in_range) {
    // Ship_AimWeaponPredictive overwrites the plain bearing with the intercept
    // lead for every owner (player turrets included). The original measures
    // from the muzzle position (fourth argument), already quadrant-offset.
    const Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    const Ship &target =
        state.ShipAt(static_cast<std::size_t>(target_ship_slot));
    heading = static_cast<float>(NovaAi_AimWeaponPredictiveFrom(
        state, owner, target, weapon_id, shot.pos_x, shot.pos_y));
    shot.heading_deg = static_cast<float>(RoundHeadingDeg(heading));
  }

  // Positive shot_random_spread jitters the firing bearing by
  // [-spread, spread) (NovaRandom_Range(spread*2) - spread), wrapped into
  // [0,360). Mode 5 jitters AFTER the velocity block instead (below).
  auto apply_spread = [&]() {
    if (w->inaccuracy > 0) {
      shot.heading_deg = static_cast<float>(
          static_cast<int>(shot.heading_deg) +
          RandomBelow(state, static_cast<int>(w->inaccuracy) * 2) -
          w->inaccuracy);
      shot.heading_deg =
          std::fmod(std::fmod(shot.heading_deg, 360.0F) + 360.0F, 360.0F);
    }
  };
  if (apply_random_spread && mode != 5) {
    apply_spread();
  }

  // Velocity rules: modes other than 5/6 (and mode 5/6 launched by an NPC bay,
  // owner slot > 0) rebuild the vector as owner velocity + polar(heading,
  // speed). Ownerless or player-launched mode 5/6 keep the inherited velocity
  // only: bombs fall, rockets accelerate onto their heading in the guidance
  // pass. Negative shot_random_spread (parallel_side != 0) fires along the
  // hull heading +/- |spread| from the selected muzzle side. Original quirk:
  // the hull heading is stored in RADIANS but |spread| is added to it and the
  // sum is consumed as DEGREES by Math_AddPolarVelocity -- reproduced as-is.
  const float speed = w->projectile_speed / 100.0F;
  if ((mode != 5 && mode != 6) || shot.owner_ship_slot > 0) {
    if (owner_ship_slot < 0 ||
        owner_ship_slot >= static_cast<std::int16_t>(GameState::kMaxShips) ||
        parallel_side == 0) {
      AddPolarVelocity(static_cast<float>(RoundHeadingDeg(shot.heading_deg)),
                       speed,
                       shot.vel_x,
                       shot.vel_y);
    } else {
      const Ship &owner =
          state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
      const float abs_spread = std::fabs(static_cast<float>(w->inaccuracy));
      float launch_deg = parallel_side > 0 ? abs_spread + owner.heading
                                           : owner.heading - abs_spread;
      launch_deg = std::fmod(std::fmod(launch_deg, 360.0F) + 360.0F, 360.0F);
      AddPolarVelocity(static_cast<float>(RoundRangeEnvelope(launch_deg)),
                       speed,
                       shot.vel_x,
                       shot.vel_y);
    }
  }
  if (apply_random_spread && mode == 5) {
    apply_spread();
  }

  shot.life_ticks_remaining =
      std::max(1.0F, static_cast<float>(w->lifetime_ticks));
  shot.life_frames = static_cast<int>(std::ceil(shot.life_ticks_remaining));
  shot.collision_radius_px = 2.0F;
  // Weapon_GetShotImpactVariant (0x0046c2f0): Flags2 bit 0x1000 makes a
  // weapon disable but not destroy. Shot_ResolveShipHitFromWeapon preserves
  // one armor point for that variant.
  shot.impact_variant =
      (w->flags_secondary & 0x1000U) != 0U ? static_cast<std::int8_t>(1) : 0;
  // Ghidra 0x004115a0 Ship_IsShipInAiState0x0D runs inline here.
  // Shot_SpawnShotFromWeapon (0x0041fd30): owners in disable mode (AI state
  // 0x0D) also mark their shots to leave the target at 1 armor. The original
  // additionally marks shots from owners locked on a disabled target
  // (Ship_IsShipLockedOnTarget); that check is deferred TODO(decomp).
  if (shot.impact_variant == 0 && owner_ship_slot > 0 &&
      owner_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips) &&
      state.ShipAt(static_cast<std::size_t>(owner_ship_slot)).ai_state_code ==
          0x0D) {
    shot.impact_variant = 1;
  }
  // Suicide weapons (ammo cost -999) consume the firing ship entirely.
  if (w->ammo_type == -999 && owner_in_range && !spawn_without_owner) {
    Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    owner.shield_points = 0.0F;
    owner.armor_points = 0.0F;
  }
  // Seek-channel seeding: each channel rolls a jam vulnerability in
  // [0, JamVuln] (0 when the channel is unused); Shot_UpdateShotGuidance
  // checks these rolls against the target's jamming score per frame.
  for (std::size_t channel = 0; channel < shot.lock_quality.size(); ++channel) {
    const std::int16_t vuln = w->jam_vuln[channel];
    shot.lock_quality[channel] =
        vuln < 1 ? 0 : static_cast<std::int16_t>(RandomBelow(state, vuln + 1));
  }
  state.active_shots.push_back(shot);
  return static_cast<int>(state.active_shots.size() - 1);
}

// Ghidra 0x0046BA30 Ship_FindNearestHittableWeaponTarget. Returns the slot of
// the nearest active ship in the player's system that this shot's weapon can
// hit (Weapon_CanWeaponHitTarget 0x00426ef0 ->
// NovaWeapon_CanProjectileHitShip), minimizing the integer-rounded squared
// distance from the shot position. -1 when no candidate qualifies. Used by
// Shot_SpawnLinkedShotsOnImpact for the Bible Flags2 0x0010 "submunitions fire
// toward nearest valid target" arm.
[[nodiscard]] std::int16_t
FindNearestHittableWeaponTarget(const GameState &state,
                                const ActiveShot &shot) {
  std::int16_t best_slot = -1;
  std::int16_t best_dist_sq = 0;
  for (std::int16_t slot = 0;
       slot < static_cast<std::int16_t>(GameState::kMaxShips);
       ++slot) {
    const Ship &candidate = state.ShipAt(static_cast<std::size_t>(slot));
    if (!candidate.is_active ||
        candidate.current_system_id != state.player.current_system_id) {
      continue;
    }
    if (!NovaWeapon_CanProjectileHitShip(state, shot, slot)) {
      continue;
    }
    // The original narrows each axis to a short before squaring, so the sum
    // wraps at 16 bits; preserve that squared-distance comparison exactly.
    const auto dx = static_cast<std::int16_t>(
        std::abs(static_cast<int>(std::lround(candidate.pos_x)) -
                 static_cast<int>(std::lround(shot.pos_x))));
    const auto dy = static_cast<std::int16_t>(
        std::abs(static_cast<int>(std::lround(candidate.pos_y)) -
                 static_cast<int>(std::lround(shot.pos_y))));
    const auto dist_sq = static_cast<std::int16_t>(dx * dx + dy * dy);
    if (best_slot == -1 || dist_sq < best_dist_sq) {
      best_slot = slot;
      best_dist_sq = dist_sq;
    }
  }
  return best_slot;
}

// Ghidra 0x00420D30 Shot_SpawnLinkedShotsOnImpact. Spawns the impacting
// weapon's submunitions (Bible SubCount/SubType/SubTheta/SubLimit) at the
// impact position, inheriting owner/target context and incrementing the
// recursion generation for the SubLimit guard. Called by
// Shot_ResolveShotCollisionHit (0x00437780) with allow_linked=1 on the
// blast-proximity path. `impacting_shot` is read only before the first child is
// appended, so a reference into GameState::active_shots is safe here.
void NovaWeapon_SpawnLinkedShotsOnImpact(
    GameState &state,
    const ActiveShot &impacting_shot,
    std::int16_t fallback_target_ship_slot) {
  const Weapon *impacting_weapon = WeaponAt(state, impacting_shot.weapon_id);
  if (impacting_weapon == nullptr) {
    return;
  }
  const std::int16_t linked_weapon_id = impacting_weapon->range_link_weapon_id;
  if (linked_weapon_id == -1 || impacting_weapon->range_link_gate < 1) {
    return;
  }
  if (impacting_weapon->range_link_extra_count > 0 &&
      impacting_weapon->range_link_extra_count <=
          impacting_shot.linked_shot_generation) {
    return;
  }

  // The original resolves the linked weapon's preloaded fire-sound handle and
  // plays it spatialized at the impact against the player as listener. The
  // clean-room queues it for the spaceflight loop, which owns SDL audio; its
  // consumer applies the flags_primary 0x10 "don't retrigger while active"
  // gate.
  const Weapon *linked_weapon = WeaponAt(state, linked_weapon_id);
  if (linked_weapon != nullptr && linked_weapon->fire_sound >= 0 &&
      linked_weapon->fire_sound < 0x100) {
    state.pending_fire_sounds.push_back(
        {linked_weapon->fire_sound,
         impacting_shot.pos_x,
         impacting_shot.pos_y,
         (linked_weapon->flags & 0x0010U) != 0U});
  }

  for (std::int16_t index = 0; index < impacting_weapon->range_link_gate;
       ++index) {
    const int spawned =
        NovaWeapon_SpawnProjectile(state,
                                   impacting_shot.owner_ship_slot,
                                   fallback_target_ship_slot,
                                   linked_weapon_id,
                                   /*spawn_without_owner=*/true,
                                   /*apply_random_spread=*/true);
    if (spawned < 0) {
      return;
    }
    ActiveShot &child = state.active_shots[static_cast<std::size_t>(spawned)];
    child.pos_x = impacting_shot.pos_x;
    child.pos_y = impacting_shot.pos_y;
    child.vel_x = 0.0F;
    child.vel_y = 0.0F;
    child.linked_shot_generation = impacting_shot.linked_shot_generation + 1;
    child.heading_deg = impacting_shot.heading_deg;
    if (impacting_weapon->weapon_mode_code == 9) {
      child.target_ship_slot = impacting_shot.target_ship_slot;
    } else if ((impacting_weapon->flags_secondary & 0x0010U) != 0U) {
      child.target_ship_slot = -1;
      child.target_ship_slot = FindNearestHittableWeaponTarget(state, child);
      if (child.target_ship_slot == -1) {
        child.target_ship_slot = fallback_target_ship_slot;
      }
      if (child.target_ship_slot == -1) {
        child.heading_deg = impacting_shot.heading_deg;
      } else {
        const Ship &target =
            state.ShipAt(static_cast<std::size_t>(child.target_ship_slot));
        child.heading_deg = BearingDeg(impacting_shot.pos_x,
                                       impacting_shot.pos_y,
                                       target.pos_x,
                                       target.pos_y);
      }
    }
    // Bible SubTheta: positive randomizes each heading within +/-spread;
    // negative fans the rounds deterministically across
    // abs(spread) * (SubCount-1) degrees centred on the parent heading.
    const std::int16_t spread = impacting_weapon->range_link_spread;
    if (spread < 0) {
      const int step = std::abs(static_cast<int>(spread));
      const int total = step * (impacting_weapon->range_link_gate - 1);
      child.heading_deg -= static_cast<float>(total / 2);
      child.heading_deg += static_cast<float>(step * index);
    } else if (spread > 0) {
      const int roll = RandomBelow(state, spread * 2 + 1);
      child.heading_deg += static_cast<float>(spread - roll);
    }
    if (child.heading_deg < 0.0F) {
      child.heading_deg += 360.0F;
    }
    if (child.heading_deg >= 360.0F) {
      child.heading_deg -= 360.0F;
    }
    // Mode 6 keeps the parent's velocity and accelerates under guidance; every
    // other mode rebuilds from the (spread) heading at the linked weapon speed.
    const Weapon *child_weapon = WeaponAt(state, child.weapon_id);
    if (child_weapon != nullptr && child_weapon->weapon_mode_code == 6) {
      child.vel_x = impacting_shot.vel_x;
      child.vel_y = impacting_shot.vel_y;
    } else if (child_weapon != nullptr) {
      AddPolarVelocity(static_cast<float>(RoundHeadingDeg(child.heading_deg)),
                       child_weapon->projectile_speed / 100.0F,
                       child.vel_x,
                       child.vel_y);
    }
  }
}

// Ghidra Shot_UpdateShotGuidance (0x00431530): per-frame projectile guidance,
// ported per the plate comment in the Ghidra DB (state machine on
// ShotState.retarget_cooldown: 0 homing, 1 asteroid target, 998 inert latch,
// 999 interference weave; mode-6 rocket acceleration blend and mode-5 bomb
// nose-follow post passes). `elapsed_ticks` stands in for the original's
// g_avg_frame_tick_scale.
void NovaWeapon_UpdateShotGuidance(GameState &state,
                                   ActiveShot &shot,
                                   float elapsed_ticks) {
  const Weapon *w = WeaponAt(state, shot.weapon_id);
  if (w == nullptr) {
    return;
  }
  const int mode = w->weapon_mode_code;
  if (mode == 9) {
    // Point-defense shots fly straight; the lead was applied at fire time.
    return;
  }
  const float frame_scale = std::max(0.0F, elapsed_ticks);
  const int cooldown = shot.retarget_cooldown;

  if (cooldown == 0) {
    if (mode != 1) {
      return; // only homing weapons guide in the normal state
    }
    // The target must be gone (-1) or active in the shot's system.
    const std::int16_t target_slot = shot.target_ship_slot;
    if (target_slot != -1) {
      if (!state.SlotInRange(static_cast<std::size_t>(target_slot))) {
        return;
      }
      const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
      if (!target.is_active || target.current_system_id != shot.system_id) {
        return;
      }
    }

    int bearing = RoundHeadingDeg(shot.heading_deg);
    if (target_slot != -1) {
      const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
      bearing = static_cast<int>(
          BearingDeg(shot.pos_x, shot.pos_y, target.pos_x, target.pos_y));
    }
    const float turn_rate = w->guided_turn_rate;
    const float remaining =
        static_cast<float>(w->lifetime_ticks) - shot.life_ticks_remaining;
    // Homing runs only while more than frame_scale * 30 ticks of life remain:
    // guided weapons fly dumb over roughly their final second.
    if (frame_scale * kGuidanceLifeGateF64 < remaining) {
      float effective_turn = turn_rate;
      // Jamming: the first seek channel whose vulnerability roll loses to the
      // target's jamming score dulls (or reverses) the turn and can flip the
      // missile onto its owner.
      if (target_slot != -1) {
        Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
        for (int channel = 0; channel < 4; ++channel) {
          const int lock = shot.lock_quality[channel];
          if (lock <= 0) {
            continue;
          }
          if (NovaAi_GetShipJammingScore(state, target, channel) > 100 - lock) {
            if (effective_turn > 0.0F) {
              effective_turn = ((w->flags_quaternary & 0x0010U) != 0U)
                                   ? effective_turn * kJamTurnSignF32
                                   : 0.0F;
            }
            if ((w->flags_quaternary & 0x8000U) != 0U &&
                RandomBelow(state, 500) == 0 && shot.owner_ship_slot >= 0 &&
                shot.owner_ship_slot <
                    static_cast<std::int16_t>(GameState::kMaxShips)) {
              shot.target_ship_slot = shot.owner_ship_slot;
              shot.owner_ship_slot = -1;
            }
            break;
          }
        }
      }
      // A target the owner cannot legitimately engage (cloak rules) makes the
      // missile go dumb; Seeker 0x8000 may retarget the owner instead.
      if (target_slot != -1 && shot.owner_ship_slot >= 0 &&
          shot.owner_ship_slot <
              static_cast<std::int16_t>(GameState::kMaxShips)) {
        const Ship &target =
            state.ShipAt(static_cast<std::size_t>(target_slot));
        const Ship &owner =
            state.ShipAt(static_cast<std::size_t>(shot.owner_ship_slot));
        if (!NovaAiShip_CanEngageTargetUnderCloakRules(state, target, owner)) {
          effective_turn = 0.0F;
          if ((w->flags_quaternary & 0x8000U) != 0U &&
              RandomBelow(state, 1000) == 0) {
            shot.target_ship_slot = shot.owner_ship_slot;
            shot.owner_ship_slot = -1;
          }
        }
      }
      // Seeker 0x4000 (loses lock if target not directly ahead): inside 250 px
      // on both axes with the target more than 45 deg off the nose, drop it.
      if (target_slot != -1 && (w->flags_quaternary & 0x4000U) != 0U) {
        const Ship &target =
            state.ShipAt(static_cast<std::size_t>(target_slot));
        if (std::abs(target.pos_x - shot.pos_x) < 250.0F &&
            std::abs(target.pos_y - shot.pos_y) < 250.0F) {
          const int to_target = static_cast<int>(
              BearingDeg(shot.pos_x, shot.pos_y, target.pos_x, target.pos_y));
          const int off_nose =
              std::abs(to_target - RoundHeadingDeg(shot.heading_deg)) % 360;
          if (off_nose > 45) {
            shot.target_ship_slot = -1;
          }
        }
      }
      TurnShotToward(shot, bearing, effective_turn, frame_scale);
      WrapHeadingAndRebuildVelocity(*w, shot);
    }
    // Seeker 0x0002 (decoyed by asteroids): 1-in-10 per frame, a nearby active
    // asteroid roughly ahead (200 px box, 16 deg cone) becomes the target and
    // latches state 1.
    if ((w->flags_quaternary & 0x0002U) != 0U && RandomBelow(state, 10) == 0) {
      for (std::size_t i = 0; i < state.asteroid_pool.size(); ++i) {
        const AsteroidState &asteroid = state.asteroid_pool[i];
        if (!asteroid.active) {
          continue;
        }
        if (std::abs(asteroid.target_pos_x - shot.pos_x) >= 200.0F ||
            std::abs(asteroid.target_pos_y - shot.pos_y) >= 200.0F) {
          continue;
        }
        const int to_asteroid =
            static_cast<int>(BearingDeg(shot.pos_x,
                                        shot.pos_y,
                                        asteroid.target_pos_x,
                                        asteroid.target_pos_y));
        if (ShortestAngleDeltaDeg(to_asteroid,
                                  RoundHeadingDeg(shot.heading_deg)) < 16) {
          shot.retarget_cooldown = 1;
          shot.target_ship_slot = static_cast<std::int16_t>(i);
          break;
        }
      }
    }
    return;
  }

  if (cooldown == 999) {
    // Confused by interference: weave left/right on a 300-frame phase while
    // more than 15 ticks of life remain, then rebuild the velocity.
    const float remaining =
        static_cast<float>(w->lifetime_ticks) - shot.life_ticks_remaining;
    if (kGuidanceLifeGateLooseF64 < remaining) {
      const int effective_turn =
          static_cast<int>(std::lround(w->guided_turn_rate));
      if (state.spaceflight_frame_counter % 300 < 150) {
        shot.heading_deg -= static_cast<float>(effective_turn);
      } else {
        shot.heading_deg += static_cast<float>(effective_turn);
      }
    }
    WrapHeadingAndRebuildVelocity(*w, shot);
    // Seeker 0x8000 may recover, retargeting onto the owner.
    if ((w->flags_quaternary & 0x8000U) != 0U &&
        RandomBelow(state, 1000) == 0 && shot.owner_ship_slot >= 0 &&
        shot.owner_ship_slot <
            static_cast<std::int16_t>(GameState::kMaxShips)) {
      shot.retarget_cooldown = 0;
      shot.target_ship_slot = shot.owner_ship_slot;
      shot.owner_ship_slot = -1;
    }
    return;
  }
  if (cooldown == 998) {
    return; // inert latch (target lost); no guidance, no velocity rebuild
  }

  // Any other state (set 1 = asteroid target): fly toward the recorded
  // asteroid slot (or straight ahead once it deactivates).
  int bearing = RoundHeadingDeg(shot.heading_deg);
  if (cooldown == 1) {
    const std::int16_t asteroid_slot = shot.target_ship_slot;
    if (asteroid_slot == -1) {
      bearing = RoundHeadingDeg(shot.heading_deg);
    } else if (asteroid_slot <
               static_cast<std::int16_t>(state.asteroid_pool.size())) {
      const AsteroidState &asteroid =
          state.asteroid_pool[static_cast<std::size_t>(asteroid_slot)];
      if (!asteroid.active) {
        shot.target_ship_slot = -1;
        bearing = RoundHeadingDeg(shot.heading_deg);
      } else {
        bearing = static_cast<int>(BearingDeg(shot.pos_x,
                                              shot.pos_y,
                                              asteroid.target_pos_x,
                                              asteroid.target_pos_y));
      }
    } else {
      shot.target_ship_slot = -1;
    }
  }
  const float remaining =
      static_cast<float>(w->lifetime_ticks) - shot.life_ticks_remaining;
  if (kGuidanceLifeGateLooseF64 < remaining) {
    TurnShotToward(shot, bearing, w->guided_turn_rate, frame_scale);
  }
  WrapHeadingAndRebuildVelocity(*w, shot);

  // Mode 6 (freeflight rocket): blend the inherited velocity toward the aim
  // heading -- (vel*94 + polar*5) * 0.01 per frame.
  if (mode == 6) {
    const float speed = w->projectile_speed / 100.0F;
    float polar_x = 0.0F;
    float polar_y = 0.0F;
    AddPolarVelocity(static_cast<float>(RoundHeadingDeg(shot.heading_deg)),
                     speed,
                     polar_x,
                     polar_y);
    shot.vel_x =
        (shot.vel_x * kRocketBlendOldF32 + polar_x * kRocketBlendNewF32) *
        kOnePercentF64;
    shot.vel_y =
        (shot.vel_y * kRocketBlendOldF32 + polar_y * kRocketBlendNewF32) *
        kOnePercentF64;
  }
  // Mode 5 (freefall bomb): the nose follows the velocity vector, turning
  // toward it by 1 deg/frame (g_cloak_fade_passive_decay 0x00575318).
  if (mode == 5) {
    // Scale the velocity before the bearing math as the original does
    // (k_bearing_precision_scale_f64 1000.0).
    const int vel_bearing = static_cast<int>(
        BearingDeg(0.0F, 0.0F, shot.vel_x * 1000.0F, shot.vel_y * 1000.0F));
    if (ShortestAngleDeltaDeg(vel_bearing, RoundHeadingDeg(shot.heading_deg)) >
        0) {
      int forward = vel_bearing - RoundHeadingDeg(shot.heading_deg);
      forward %= 360;
      if (forward < 0) {
        forward += 360;
      }
      if (forward < 181) {
        shot.heading_deg += kBombNoseTurnRate;
      } else {
        shot.heading_deg -= kBombNoseTurnRate;
      }
    }
  }
}

// Ghidra 0x00455150 Weapon_FirePlayerWeaponBank.
void NovaWeapon_FirePlayerWeaponBank(GameState &state,
                                     std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return;
  }
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (w == nullptr) {
    return;
  }
  Ship &player = state.player;

  // Cloak gate: while the player is cloaked past the visibility threshold only
  // weapons with flags_secondary 0x4000 may fire.
  if (NovaTargeting_ShipAtCloakVisibilityThreshold(player) &&
      (w->flags_secondary & 0x4000U) == 0U) {
    return;
  }
  // Waypoint-arrival marker arm (ship classes with sprite_behavior_flags
  // 2|0x80 and waypoint_arrival_marker_b > 0): fields not modelled in the
  // port. TODO(decomp(0x00455150)) skipped: waypoint markers.
  if (state.weapon_bank_cooldown[weapon_bank] > 0.0F) {
    return; // still cooling down
  }
  if (player.ai_station_hold_timer != 0.0F) {
    return; // the original requires an exactly-zero station-hold timer
  }

  const std::int16_t mode = w->weapon_mode_code;
  const std::int16_t target_slot = player.primary_target_ship_slot;
  const bool has_target =
      target_slot >= 0 &&
      target_slot < static_cast<std::int16_t>(GameState::kMaxShips) &&
      state.ShipAt(static_cast<std::size_t>(target_slot)).is_active;

  const int burst_attempts =
      NovaWeapon_GetWeaponBurstAttempts(state, player, weapon_bank);
  int volley_fired = 0;
  const float heading_deg = player.heading * (180.0F / 3.14159265358979323846F);

  for (int attempt = 0; attempt < burst_attempts; ++attempt) {
    if (!NovaWeapon_CanFireWeaponBank(state, player, weapon_bank)) {
      continue;
    }
    bool fired = false;
    if (mode == -1 || mode == 1 || mode == 5 || mode == 6) {
      // Straight projectile (-1/6), homing (1), freefall (5).
      fired = NovaWeapon_SpawnProjectile(state,
                                         0,
                                         target_slot,
                                         weapon_bank,
                                         /*spawn_without_owner=*/false,
                                         /*apply_random_spread=*/true) >= 0;
    } else if (mode == 0) {
      // Fixed beam along the current heading (the original queues the record
      // with the live target slot; the endpoint stays heading-driven).
      fired = NovaWeapon_QueueBeamHit(
          state,
          0,
          target_slot,
          weapon_bank,
          /*forced_targeting=*/-1,
          static_cast<std::int16_t>(std::lround(heading_deg)));
    } else if (mode == 3 || mode == 4) {
      // Turreted beam (3) / turreted projectile (4): fire only while the
      // target is NOT inside a turret blind-spot sector.
      if (has_target) {
        const Ship &target =
            state.ShipAt(static_cast<std::size_t>(target_slot));
        const std::int16_t bearing =
            static_cast<std::int16_t>(std::lround(BearingDeg(
                player.pos_x, player.pos_y, target.pos_x, target.pos_y)));
        if (!NovaAi_WeaponIsTargetBearingInTurretBlindSpot(
                *state.scenario.Ship(
                    static_cast<std::int16_t>(player.ship_class_id + 0x80)),
                *w,
                static_cast<std::int16_t>(std::lround(
                    player.heading * (180.0F / 3.14159265358979323846F))),
                bearing)) {
          if (mode == 3) {
            fired = NovaWeapon_QueueBeamHit(state,
                                            0,
                                            target_slot,
                                            weapon_bank,
                                            /*forced_targeting=*/-1,
                                            bearing);
          } else {
            fired = NovaWeapon_SpawnProjectile(
                        state, 0, target_slot, weapon_bank, false, true) >= 0;
          }
        }
      }
    } else if (mode == 7 || mode == 8) {
      // Guided launch gate: mode 7 requires the target within 46 deg of the
      // nose, mode 8 of the tail (heading + 180). Outside the gate, mode 7
      // dumb-fires without a target; mode 8 holds fire.
      if (has_target) {
        const Ship &target =
            state.ShipAt(static_cast<std::size_t>(target_slot));
        const float tb =
            BearingDeg(player.pos_x, player.pos_y, target.pos_x, target.pos_y);
        const float reference =
            (mode == 8) ? std::remainder(heading_deg + 180.0F, 360.0F)
                        : heading_deg;
        const float delta = std::abs(std::remainder(tb - reference, 360.0F));
        if (delta < 46.0F) {
          fired = NovaWeapon_SpawnProjectile(
                      state, 0, target_slot, weapon_bank, false, true) >= 0;
        } else if (mode == 7) {
          fired = NovaWeapon_SpawnProjectile(
                      state, 0, -1, weapon_bank, false, true) >= 0;
        }
      } else if (mode == 7) {
        fired = NovaWeapon_SpawnProjectile(
                    state, 0, -1, weapon_bank, false, true) >= 0;
      }
    } else if (mode == 99) {
      // Carrier-bay launch (Weapon_SpawnShipFromCarrierBayWeapon): the port
      // does not spawn bay ships from the fire path yet.
      // TODO(decomp(0x00455150)) skipped: launch-bay ship spawn.
    }

    if (!fired) {
      continue;
    }
    ++volley_fired;
    // flags_secondary 0x200: muzzle sprite flash to level 32. The port does
    // not model the player weapon-sprite flash overlay yet.
    // TODO(decomp(0x00455150)) skipped: weapon_sprite_flash_level.

    // Per-shot cost (skipped for burst-counted weapons, flags_tertiary 0x1;
    // they pay once per burst-cycle wrap below). Energy weapons (cost -1) are
    // free per shot; codes in [-999,-1] spend nothing; codes < -999 draw fuel
    // at (|cost| - 1000) * 0.1 units; [0,255] consumes one round from that
    // bank's secondary counter. Mode-99 bays spend from their own counter.
    if ((w->flags_tertiary & 0x0001U) == 0U) {
      const int cost = w->ammo_type;
      if (cost < -999) {
        player.fuel_points = std::max(
            0.0F, player.fuel_points - static_cast<float>(-cost - 1000) * 0.1F);
      } else {
        std::int16_t spend_slot = -1;
        if (cost >= 0 && cost <= 0xff) {
          spend_slot = static_cast<std::int16_t>(cost);
        } else if (mode == 99) {
          spend_slot = weapon_bank;
        }
        if (spend_slot != -1) {
          std::int16_t &counter = BankSecondary(state, spend_slot);
          counter =
              static_cast<std::int16_t>(std::max<std::int16_t>(0, counter - 1));
        }
      }
    }
  }

  if (volley_fired < 1) {
    return;
  }
  // A volley actually fired: queue the fire sound (the port plays it through
  // the spaceflight loop's sound queue; the original's beam modes 0/3 play
  // theirs through a separate immediate NovaAudio_PlaySpatialByDistance call,
  // approximated here by the same queued path).
  if (w->fire_sound >= 0) {
    state.pending_fire_sounds.push_back({w->fire_sound,
                                         player.pos_x,
                                         player.pos_y,
                                         (w->flags & 0x0010U) != 0U});
  }

  // Kickback (resource "recoil"): rearward polar impulse of kickback /
  // hull_mass, clamped per axis to the class base speed
  // (Math_AddPolarVelocityWithClamp 0x0043b4e0).
  if (w->kickback_impulse > 0) {
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(player.ship_class_id + 0x80));
    if (cls != nullptr && cls->mass_tons > 0) {
      const float rear_bearing = std::remainder(heading_deg + 180.0F, 360.0F);
      NovaPlayer_AddPolarVelocityClamped(
          rear_bearing * (3.14159265358979323846F / 180.0F),
          static_cast<float>(w->kickback_impulse) /
              static_cast<float>(cls->mass_tons),
          cls->speed,
          player.vel_x,
          player.vel_y);
    }
  }

  // Bank cooldown: flags_primary 0x40 weapons reload for the fixed interval;
  // the rest scale by the volley and the mount count (a second identical
  // weapon doubles the fire rate).
  if ((w->flags & 0x0040U) == 0U) {
    const int mount_count =
        std::max(1, static_cast<int>(BankAmmo(state, weapon_bank)));
    state.weapon_bank_cooldown[weapon_bank] =
        static_cast<float>(volley_fired) *
        static_cast<float>(std::max(1, static_cast<int>(w->reload_ticks))) /
        static_cast<float>(mount_count);
  } else {
    state.weapon_bank_cooldown[weapon_bank] =
        static_cast<float>(w->reload_ticks);
  }

  // flags_tertiary 0x20 (linked fire): every other bank's cooldown rises to
  // at least this bank's cooldown + 2.0 ticks (_DAT_00575698), so bank groups
  // fire in sequence instead of simultaneously.
  if ((w->flags_tertiary & 0x0020U) != 0U) {
    const float floor = state.weapon_bank_cooldown[weapon_bank] + 2.0F;
    for (std::int16_t b = 0; b < 0x100; ++b) {
      if (b != weapon_bank) {
        float &cd = state.weapon_bank_cooldown[b];
        if (cd < floor) {
          cd = floor;
        }
      }
    }
  }

  // Burst cycle: advance the per-bank counter; on the wrap edge of a
  // burst-counted weapon (flags_tertiary 0x1) pay one burst's worth of
  // ammo/fuel; when the counter reaches Weapon_GetWeaponFireIntervalTicks
  // (0x0046f270) it resets and the bank preloads burst_reset_cooldown.
  if (w->burst_cycle_ticks > 0) {
    std::int16_t &cycle = state.weapon_bank_burst_counter[weapon_bank];
    cycle = static_cast<std::int16_t>(cycle + 1);
    if ((w->flags_tertiary & 0x0001U) != 0U &&
        cycle % w->burst_cycle_ticks == 0) {
      const int pay = (w->flags & 0x0040U) != 0U ? volley_fired : 1;
      if (mode == 99) {
        std::int16_t &counter = BankSecondary(state, weapon_bank);
        counter = static_cast<std::int16_t>(std::max<std::int16_t>(
            0, counter - static_cast<std::int16_t>(pay)));
      } else {
        const int cost = w->ammo_type;
        if (cost < -999) {
          player.fuel_points =
              std::max(0.0F,
                       player.fuel_points -
                           static_cast<float>(pay * (-cost - 1000)) * 0.1F);
        } else if (cost >= 0 && cost <= 0xff) {
          std::int16_t &counter =
              BankSecondary(state, static_cast<std::int16_t>(cost));
          counter = static_cast<std::int16_t>(std::max<std::int16_t>(
              0, counter - static_cast<std::int16_t>(pay)));
        }
      }
    }
    const int interval =
        (w->flags & 0x0040U) != 0U
            ? w->burst_cycle_ticks
            : std::max(1, static_cast<int>(BankAmmo(state, weapon_bank))) *
                  w->burst_cycle_ticks;
    if (cycle >= interval) {
      cycle = 0;
      state.weapon_bank_cooldown[weapon_bank] =
          static_cast<float>(w->burst_reset_cooldown);
    }
  }
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
      // g_shipAvailabilityCachesDirty = 1: the port's availability caches are
      // deferred (TODO(decomp)).
    }
  }

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

void NovaWeapon_TickNpcWeaponBanks(Ship &ship, float elapsed_ticks) {
  const float ticks = std::max(0.0F, elapsed_ticks);
  for (float &cooldown : ship.npc_weapon_bank_cooldown) {
    cooldown = std::max(0.0F, cooldown - ticks);
  }
}

// Ghidra 0x00427a90 Shot_QueueBeamHit.
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
               : ship.npc_weapon_bank_ammo[static_cast<std::size_t>(slot)];
  };
  auto bank_secondary = [&](std::int16_t slot) -> std::int16_t {
    return is_player
               ? BankSecondary(state, slot)
               : ship.npc_weapon_bank_secondary[static_cast<std::size_t>(slot)];
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

bool NovaWeapon_QueueBeamHit(GameState &state,
                             std::int16_t owner_ship_slot,
                             std::int16_t target_ship_slot,
                             std::int16_t weapon_id,
                             std::int16_t forced_targeting,
                             std::int16_t firing_bearing_deg) {
  if (owner_ship_slot < 0 ||
      owner_ship_slot >= static_cast<std::int16_t>(GameState::kMaxShips) ||
      weapon_id < 0 || weapon_id >= 0x100) {
    return false;
  }
  const Weapon *weapon = WeaponAt(state, weapon_id);
  if (weapon == nullptr) {
    return false;
  }
  for (BeamHit &beam : state.beam_hit_queue) {
    if (beam.lifetime_ticks >= -1) {
      continue;
    }
    const Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    beam.source_x = owner.pos_x;
    beam.source_y = owner.pos_y;
    beam.target_x = owner.pos_x;
    beam.target_y = owner.pos_y;
    bool aimed_at_target = false;
    if (target_ship_slot >= 0 &&
        target_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips)) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(target_ship_slot));
      beam.target_x = target.pos_x;
      beam.target_y = target.pos_y;
      aimed_at_target = true;
    }
    if (!aimed_at_target) {
      // No target: lay the beam downrange along the firing bearing. The
      // original derives endpoints per frame in Shot_UpdateBeamHitQueue
      // (0x0042f270); BeamLength + 32 is its beam-reach constant.
      const float reach = static_cast<float>(weapon->beam_length_px) + 32.0F;
      const float rad = static_cast<float>(firing_bearing_deg) *
                        (3.14159265358979323846F / 180.0F);
      beam.target_x = beam.source_x + std::sin(rad) * reach;
      beam.target_y = beam.source_y - std::cos(rad) * reach;
    }
    beam.lifetime_ticks = std::max<std::int16_t>(1, weapon->lifetime_ticks);
    beam.animation_counter = 0;
    beam.weapon_id = weapon_id;
    beam.owner_ship_slot = owner_ship_slot;
    beam.target_ship_slot = target_ship_slot;
    beam.forced_targeting = forced_targeting;
    beam.turret_quadrant = -1;
    beam.turret_group_id = weapon->turret_group_id;
    beam.impact_variant = (weapon->flags_secondary & 0x1000U) != 0U ? 1 : 0;
    beam.impact_resolved = false;
    return true;
  }
  return false;
}

// Ghidra 0x0042f270 Shot_UpdateBeamHitQueue.
void NovaWeapon_TickBeamHitQueue(GameState &state, float elapsed_ticks) {
  const float ticks = std::max(0.0F, elapsed_ticks);
  for (BeamHit &beam : state.beam_hit_queue) {
    if (beam.lifetime_ticks < -1) {
      continue;
    }
    if (beam.target_ship_slot >= 0 &&
        beam.target_ship_slot <
            static_cast<std::int16_t>(GameState::kMaxShips)) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(beam.target_ship_slot));
      beam.target_x = target.pos_x;
      beam.target_y = target.pos_y;
      if (!beam.impact_resolved && target.is_active) {
        NovaWeapon_ResolveDirectWeaponHit(state,
                                          beam.owner_ship_slot,
                                          beam.target_ship_slot,
                                          beam.weapon_id,
                                          beam.impact_variant);
        beam.impact_resolved = true;
      }
    }
    beam.lifetime_remainder += ticks;
    const std::int16_t whole_ticks =
        static_cast<std::int16_t>(beam.lifetime_remainder);
    if (whole_ticks <= 0) {
      continue;
    }
    beam.lifetime_remainder -= static_cast<float>(whole_ticks);
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(beam.weapon_id + 0x80));
    for (std::int16_t tick = 0; tick < whole_ticks && beam.lifetime_ticks >= 0;
         ++tick) {
      // Decay phase (Bible "Decay", WeaponDef fuse_ticks): once the lifetime
      // reaches 0, a beam with a positive fuse holds on screen while
      // animation_counter + falloff < 0x10, counting animation_counter up;
      // the renderer shrinks the corona / fades the beam with it. The
      // original only ever increments animation_counter in this branch.
      if (beam.lifetime_ticks == 0 && weapon != nullptr &&
          weapon->fuse_ticks > 0) {
        beam.animation_counter =
            static_cast<std::int16_t>(beam.animation_counter + 1);
        if (beam.animation_counter + weapon->beam_falloff < 0x10) {
          continue;
        }
      }
      beam.lifetime_ticks = static_cast<std::int16_t>(beam.lifetime_ticks - 1);
    }
    if (beam.lifetime_ticks < 0) {
      beam = BeamHit{};
    }
  }
}

void NovaWeapon_FireNpcWeaponBank(GameState &state, Ship &ship) {
  const std::int16_t bank = ship.active_weapon_bank_slot;
  // Ship_HandleShip (0x00433050) keeps destroyed slots around long enough for
  // their death/debris handling, but Weapon_FireShipWeapons must not launch a
  // bank that was latched before the lethal hit.
  if (ship.ship_instance_id == 0 || bank < 0 || bank >= 0x100 ||
      ship.ai_fire_trigger_latch == 0 || ship.death_timer_active > 0.0F ||
      ship.armor_points <= 0.0F) {
    if (ship.death_timer_active > 0.0F || ship.armor_points <= 0.0F) {
      ship.active_weapon_bank_slot = -1;
      ship.ai_fire_trigger_latch = 0;
    }
    return;
  }
  const std::size_t index = static_cast<std::size_t>(bank);
  const Weapon *weapon =
      state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
  if (weapon == nullptr || ship.npc_weapon_bank_ammo[index] <= 0 ||
      ship.npc_weapon_bank_cooldown[index] > 0.0F) {
    return;
  }
  // Weapon_CanFireWeaponBank is the authoritative ammo/energy gate.  In
  // particular, energy weapons (ammo_type == -1) can have secondary == 0;
  // that counter is not an ammunition requirement for them.
  if (weapon->weapon_mode_code == 99 &&
      ship.npc_weapon_bank_secondary[index] < 1) {
    return;
  }
  const std::int16_t mode = weapon->weapon_mode_code;

  const std::int16_t target_slot = ship.primary_target_ship_slot;
  const bool has_target =
      target_slot >= 0 &&
      state.SlotInRange(static_cast<std::size_t>(target_slot)) &&
      state.ShipAt(static_cast<std::size_t>(target_slot)).is_active;
  const Ship *target =
      has_target ? &state.ShipAt(static_cast<std::size_t>(target_slot))
                 : nullptr;

  // Weapon_GetWeaponBurstAttempts (0x0046f2c0): how many shots this trigger
  // fires. Non-burst weapons (flags_primary 0x40 clear) get exactly one; a
  // burst bank starts from the mounted ammo count and is capped by the loaded
  // secondary ammo of its cost bank. Fuel-cost (ammo_type < -999) burst capping
  // is deferred (the port does not model fuel-on-weapons).
  int burst_attempts = 1;
  if ((weapon->flags & 0x0040U) != 0) {
    burst_attempts = ship.npc_weapon_bank_ammo[index];
    if ((weapon->flags_tertiary & 0x0001U) != 0) {
      const int cost = weapon->ammo_type;
      if (cost >= 0 && cost <= 0xff) {
        burst_attempts = std::min(
            burst_attempts,
            static_cast<int>(
                ship.npc_weapon_bank_secondary[static_cast<std::size_t>(
                    cost)]));
      }
    }
    burst_attempts = std::max(0, burst_attempts);
  }
  if (burst_attempts < 1) {
    return;
  }

  // Weapon_IsTargetBearingInTurretBlindSpot (0x0046b360, formerly the
  // misnamed Weapon_IsWeaponArcAllowed): fixed forward/side/rear sector test.
  // Front <46 deg, side <136 deg, else rear; each sector is BLIND when the
  // weapon's flags_primary bit 0x1000/0x2000/0x4000 (or the ship class
  // capability flags) is set. Turret weapons usually clear all three bits, so
  // blind_spot() is false and turreted modes fire through the reach branch.
  const ShipClass *ship_cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  auto blind_spot = [&](float target_bearing_deg) {
    const float delta = std::abs(std::remainder(
        target_bearing_deg - ship.heading * (180.0F / 3.14159265358979323846F),
        360.0F));
    if (delta < 46.0F) {
      return (weapon->flags & 0x1000U) != 0U ||
             (ship_cls != nullptr &&
              (ship_cls->capability_flags & 0x1000U) != 0U);
    }
    if (delta < 136.0F) {
      return (weapon->flags & 0x2000U) != 0U ||
             (ship_cls != nullptr &&
              (ship_cls->capability_flags & 0x2000U) != 0U);
    }
    return (weapon->flags & 0x4000U) != 0U ||
           (ship_cls != nullptr &&
            (ship_cls->capability_flags & 0x4000U) != 0U);
  };
  // Weapon_FireShipWeapons (0x00414550) uses BeamLength + 32 for mode 3 and
  // the post-load projectile range (+0x5c) + 32 for mode 4. Modes 7/8 use the
  // same projectile range envelope for their quadrant checks.
  const float projectile_turret_reach = weapon->range_scalar + 32.0F;
  const float beam_turret_reach =
      static_cast<float>(weapon->beam_length_px) + 32.0F;

  const std::size_t cost_index = [&]() -> std::size_t {
    const int cost = weapon->ammo_type;
    return (cost >= 0 && cost <= 0xff) ? static_cast<std::size_t>(cost) : index;
  }();

  // ---- burst loop (Weapon_FireShipWeapons 0x00414550) ----
  int shots_fired = 0;
  for (int attempt = 0; attempt < burst_attempts; ++attempt) {
    // Weapon_CanFireWeaponBank (0x00468990) per-burst gate on THIS SHIP's own
    // banks. Cooldown is already 0 at entry and stays 0 through the burst;
    // the firing bank must still carry loaded ammo. Cloak gating
    // (flags_secondary 0x4000) is deferred.
    if (!NovaWeapon_CanFireWeaponBank(state, ship, bank)) {
      continue;
    }
    bool fired = false;
    if (mode == 0) {
      // Beam: no arc gate (original's mode -1/0/6 block fires regardless).
      fired = NovaWeapon_QueueBeamHit(
          state,
          ship.ship_instance_id,
          ship.primary_target_ship_slot,
          bank,
          -1,
          static_cast<std::int16_t>(
              std::lround(ship.heading * (180.0F / 3.14159265358979323846F))));
    } else if (mode == 3 || mode == 4) {
      // Turreted beam (3) / turreted unguided (4): fire only when the target
      // is NOT in the fixed arc (the turret's relief role) but within reach.
      if (has_target && target != nullptr) {
        const float tb =
            BearingDeg(ship.pos_x, ship.pos_y, target->pos_x, target->pos_y);
        const float reach =
            (mode == 3) ? beam_turret_reach : projectile_turret_reach;
        if (!blind_spot(tb) && std::abs(ship.pos_x - target->pos_x) < reach &&
            std::abs(ship.pos_y - target->pos_y) < reach) {
          if (mode == 3) {
            fired = NovaWeapon_QueueBeamHit(
                state,
                ship.ship_instance_id,
                ship.primary_target_ship_slot,
                bank,
                -1,
                static_cast<std::int16_t>(std::lround(tb)));
          } else {
            fired = NovaWeapon_SpawnProjectile(state,
                                               ship.ship_instance_id,
                                               ship.primary_target_ship_slot,
                                               bank,
                                               false,
                                               true) >= 0;
          }
        }
      }
    } else if (mode == 7 || mode == 8) {
      // Front (7) / rear (8) quadrant turret: within 46 deg of the nose/tail
      // AND within reach (original's unaff_EBP < 0x2e gate).
      if (has_target && target != nullptr) {
        const float tb =
            BearingDeg(ship.pos_x, ship.pos_y, target->pos_x, target->pos_y);
        const float cur_deg = ship.heading * (180.0F / 3.14159265358979323846F);
        const float reference =
            (mode == 8) ? std::remainder(cur_deg + 180.0F, 360.0F) : cur_deg;
        const float delta = std::abs(std::remainder(tb - reference, 360.0F));
        if (delta < 46.0F &&
            std::abs(ship.pos_x - target->pos_x) < projectile_turret_reach &&
            std::abs(ship.pos_y - target->pos_y) < projectile_turret_reach) {
          fired = NovaWeapon_SpawnProjectile(state,
                                             ship.ship_instance_id,
                                             ship.primary_target_ship_slot,
                                             bank,
                                             false,
                                             true) >= 0;
        }
      }
    } else if (mode == -1 || mode == 1 || mode == 5 || mode == 6 || mode == 9) {
      // Straight projectile (-1/6), homing (1), freefall (5): fire toward the
      // primary target. Mode 1 requires a live target like the original. Mode
      // 9 (point defense) keeps firing at the primary target -- its dedicated
      // targeting (Weapon_SelectTurretTargetWithinArc 0x0043a310) is deferred.
      if (mode == 1 && !has_target) {
        continue;
      }
      fired = NovaWeapon_SpawnProjectile(state,
                                         ship.ship_instance_id,
                                         ship.primary_target_ship_slot,
                                         bank,
                                         false,
                                         true) >= 0;
    } else {
      return; // unsupported weapon mode in this bank
    }
    if (!fired) {
      continue;
    }
    ++shots_fired;
    // Per-burst ammo consumption (one per successful shot, mirroring the
    // original's loop).
    const std::int16_t secondary = ship.npc_weapon_bank_secondary[index];
    if (secondary > 0 && ship.pers_def_slot != 0x3ff &&
        (weapon->flags_tertiary & 0x0001U) == 0U) {
      ship.npc_weapon_bank_secondary[index] =
          static_cast<std::int16_t>(secondary - 1);
    }
  }

  if (shots_fired < 1) {
    return;
  }
  // A volley fired (sVar9 >= 1 in Weapon_FireShipWeapons): queue the fire
  // sound, sourced at this ship, for the spaceflight loop. The original plays
  // it via NovaAudio_PlaySpatialByDistance with the player ship as listener,
  // so NPC fire fades with distance; flags_primary bit 0x10 marks sounds that
  // must not stack (NovaAudio_CountActiveByHandle gate).
  if (weapon->fire_sound >= 0) {
    state.pending_fire_sounds.push_back({weapon->fire_sound,
                                         ship.pos_x,
                                         ship.pos_y,
                                         (weapon->flags & 0x0010U) != 0U});
  }
  const int mount_count =
      std::max(1, static_cast<int>(ship.npc_weapon_bank_ammo[index]));
  float fire_cooldown;
  if ((weapon->flags & 0x0040U) == 0) {
    // Original: local_1c = sVar9 * (speed_scalar / mount_count).
    fire_cooldown = static_cast<float>(
                        std::max(1, static_cast<int>(weapon->reload_ticks))) *
                    static_cast<float>(std::max(1, shots_fired)) /
                    static_cast<float>(mount_count);
  } else {
    fire_cooldown = static_cast<float>(weapon->reload_ticks);
  }
  // Ghidra Weapon_FireShipWeapons (0x00414550) also scales this cooldown
  // when firing at the player by a combat-rating ladder
  // (k_npc_fire_cooldown_scale_1p75/1p5/1p25/1p1 = 1.75/1.5/1.25/1.1 as the
  // rating climbs through ship-strength*100/400/800/1600; no scale at
  // >=1600). LARGER cooldown = SLOWER fire, so hostile NPCs are gentle on
  // weak players and full-rate on veterans. TODO(decomp(0x00414550)) skipped:
  // g_player_combat_rating_points is not tracked, so the baseline (full-rate)
  // value is emitted.

  // Burst cycle (Weapon_FireShipWeapons): count a cycle tick; on the wrap
  // edge (flags_tertiary & 1) consume one round from the secondary ammo bank;
  // when Weapon_GetWeaponFireIntervalTicks is reached, reset the counter and
  // preload the reset cooldown.
  if (weapon->burst_cycle_ticks > 0) {
    ship.npc_weapon_bank_burst_counter[index] = static_cast<std::int16_t>(
        ship.npc_weapon_bank_burst_counter[index] + 1);
    if ((weapon->flags_tertiary & 0x0001U) != 0 &&
        (ship.npc_weapon_bank_burst_counter[index] %
         weapon->burst_cycle_ticks) == 0) {
      auto &cost_secondary = ship.npc_weapon_bank_secondary[cost_index];
      cost_secondary = static_cast<std::int16_t>(
          std::max(0, static_cast<int>(cost_secondary) - 1));
    }
    const std::int16_t interval =
        (weapon->flags & 0x0040U) != 0
            ? weapon->burst_cycle_ticks
            : static_cast<std::int16_t>(mount_count *
                                        weapon->burst_cycle_ticks);
    if (interval > 0 && ship.npc_weapon_bank_burst_counter[index] >= interval) {
      ship.npc_weapon_bank_burst_counter[index] = 0;
      fire_cooldown = static_cast<float>(weapon->burst_reset_cooldown);
    }
  }
  ship.npc_weapon_bank_cooldown[index] = fire_cooldown;
  ship.ai_fire_trigger_latch = 0;
  ship.active_weapon_bank_slot = -1;
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

void NovaWeapon_TickShots(GameState &state,
                          float frame_time_ms,
                          float elapsed_ticks) {
  // Advance shots per Shot_HandleShot (0x00435830): sanitize ownership/target
  // latches, count lifetime down, run the guidance pass, then integrate the
  // position with the (possibly rebuilt) velocity. Shots that cross zero are
  // retired with the weapon's expiry impact; collision-resolved shots are
  // consumed earlier in the frame by the collision pass.
  auto &shots = state.active_shots;
  const float tick_scale = std::max(0.0F, elapsed_ticks);
  state.last_frame_tick_scale = tick_scale;
  ++state.spaceflight_frame_counter;
  for (auto &shot : shots) {
    if (shot.consumed) {
      continue;
    }
    const Weapon *weapon = WeaponAt(state, shot.weapon_id);
    if (weapon == nullptr) {
      shot.consumed = true;
      continue;
    }
    // Owner sanitation and the target latches. Point-defense shots are always
    // targetless (they were lead-aimed at fire time); a mode-1 shot whose
    // recorded target died latches 998 and flies inert for the rest of its
    // life.
    if (shot.owner_ship_slot < -1 ||
        shot.owner_ship_slot >=
            static_cast<std::int16_t>(GameState::kMaxShips)) {
      shot.owner_ship_slot = -1;
    }
    if (weapon->weapon_mode_code == 9) {
      shot.target_ship_slot = -1;
    } else {
      const std::int16_t target = shot.target_ship_slot;
      if (target < 0 ||
          target >= static_cast<std::int16_t>(GameState::kMaxShips)) {
        shot.target_ship_slot = -1;
      } else if (shot.retarget_cooldown == 0 &&
                 !state.ShipAt(static_cast<std::size_t>(target)).is_active) {
        shot.retarget_cooldown = 998;
        shot.target_ship_slot = -1;
      }
    }

    shot.life_ticks_remaining -= tick_scale;
    shot.life_frames =
        static_cast<int>(std::ceil(std::max(0.0F, shot.life_ticks_remaining)));
    if (shot.life_ticks_remaining <= 0.0F) {
      // Ghidra Shot_HandleShot (0x00435830) emits the weapon's expiry/fuse
      // impact for shots in the player's system (gated in the original by
      // life > k_shot_expiry_min_life -63000, which a -1 expiry satisfies).
      if (shot.system_id == state.player.current_system_id) {
        NovaEffects_SpawnAreaImpact(state,
                                    shot.pos_x,
                                    shot.pos_y,
                                    weapon->impact_effect_id,
                                    weapon->splash_radius,
                                    true);
      }
      shot.consumed = true;
      continue;
    }
    // Guidance runs before movement: a homing shot turns and rebuilds its
    // velocity, then the (possibly new) vector integrates this frame.
    NovaWeapon_UpdateShotGuidance(state, shot, tick_scale);
    shot.pos_x += shot.vel_x * tick_scale;
    shot.pos_y += shot.vel_y * tick_scale;
    NovaWeapon_StepShotAnimation(state, shot, frame_time_ms);
  }
  shots.erase(std::remove_if(shots.begin(),
                             shots.end(),
                             [](const ActiveShot &s) { return s.consumed; }),
              shots.end());
  // Cooldown decay moved to NovaWeapon_TickPlayerWeaponBankCooldowns (the
  // faithful PlayerTick_WeaponCommands tail: ammo>0 gate + ionization pin),
  // called from the spaceflight loop's player tick.
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
  // bank whose id equals its ammo_type.
  const std::int16_t source_bank =
      (w->ammo_type < 0 || w->ammo_type > 0xff || w->weapon_mode_code == 99)
          ? weapon_bank
          : w->ammo_type;
  return std::min<std::int16_t>(BankSecondary(state, source_bank), 9999);
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
    const auto resource_id = NovaWeapon_FireSoundResourceId(fire_sound_slot);
    if (resource_id >= GameState::kGameplaySoundFirstId &&
        static_cast<std::size_t>(resource_id) <
            static_cast<std::size_t>(GameState::kGameplaySoundFirstId) +
                GameState::kGameplaySoundCount) {
      state.gameplay_sounds[resource_id - GameState::kGameplaySoundFirstId] =
          *state.weapon_fire_sounds[fire_sound_slot];
    }
    NovaLog::Info("cached weapon fire sound slot {} (snd id {})",
                  fire_sound_slot,
                  NovaWeapon_FireSoundResourceId(fire_sound_slot));
  } else {
    NovaLog::Warn("weapon fire sound slot {} (snd id {}) failed to decode",
                  fire_sound_slot,
                  NovaWeapon_FireSoundResourceId(fire_sound_slot));
  }
}

// Ghidra 0x004b0740 NovaAudio_PreloadGameplayData (partial: this covers the
// weapon-fire slice of the original's startup snd preload; effect/cloak
// ranges remain TODO(decomp)).
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

void NovaWeapon_PreloadGameplaySounds(GameState &state) {
  std::size_t loaded = 0;
  for (std::size_t offset = 0; offset < GameState::kGameplaySoundCount;
       ++offset) {
    if (state.gameplay_sounds[offset].has_value()) {
      ++loaded;
      continue;
    }
    const auto resource_id =
        static_cast<std::uint16_t>(GameState::kGameplaySoundFirstId + offset);
    const auto resource = NovaResource_LoadSndData(resource_id);
    if (!resource) {
      continue;
    }
    if (auto decoded = NovaSound_Decode(*resource)) {
      state.gameplay_sounds[offset] = std::move(*decoded);
      ++loaded;
    } else {
      NovaLog::Warn("gameplay snd resource {} failed to decode", resource_id);
    }
  }
  NovaLog::Info("preloaded {} gameplay sound resources (ids {}..{})",
                loaded,
                GameState::kGameplaySoundFirstId,
                GameState::kGameplaySoundFirstId +
                    GameState::kGameplaySoundCount - 1);
}

// Ghidra NovaAudio_PlaySpatialByDistance (0x004692e0): the original computes
// left/right channel gains from the rounded source/listener displacement,
// clamps each channel to the [extent/8, extent] band, then averages the
// channels (NovaAudio_QueueCenteredSound -> Audio_AllocateVoiceSlot feeds
// a single mono gain to the mixer). The extent is the sound-volume scaled
// global (0..0x100, Frame_UpdateEffectIntensityGlobal), so the *distance*
// behavior factors out into a pure 0..1 attenuation; the caller's master
// volume carries the preference. Within 200 px the sound plays at full
// volume; beyond that the loud channel falls as 722500/d^2 (full at 850 px)
// and the quiet channel as 40000/d^2, each floored at 1/8 of the extent. The
// integer channel truncation, clamp, and (L+R+1)>>1 average are preserved so
// the gain matches the original at 1/256 resolution.
float NovaWeapon_ComputeSpatialFireGain(float listener_x,
                                        float listener_y,
                                        float src_x,
                                        float src_y) {
  const int dx = std::lround(src_x - listener_x);
  const int dy = std::lround(src_y - listener_y);
  const std::int64_t dist_sq =
      static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy;
  if (dist_sq <= 40000) {
    return 1.0F; // 200 px or closer (including src == listener): full volume
  }
  // Channel gain as the original computes it with the extent at full scale
  // (E = 0x100): (E * numerator) / dist_sq, truncated, clamped to [E/8, E].
  auto channel = [dist_sq](std::int64_t numerator) -> std::int64_t {
    const std::int64_t gain = (numerator * 256) / dist_sq;
    return std::clamp<std::int64_t>(gain, 32, 256);
  };
  const std::int64_t loud = channel(722500);
  const std::int64_t quiet = channel(40000);
  std::int64_t left = loud;
  std::int64_t right = loud;
  if (dx < 200) {
    if (dx < -200) {
      right = quiet; // source clearly to the left
    }
    // -200 <= dx < 200: horizontally centered, both channels loud
  } else {
    right = loud;
    left = quiet; // source clearly to the right
  }
  const std::int64_t average = (left + right + 1) >> 1;
  return static_cast<float>(average) / 256.0F;
}

} // namespace game
