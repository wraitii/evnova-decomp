#pragma once

// Clean-room reconstruction of the player's outfit inventory and the
// outfit-driven effective-stat aggregation the original computes from it
// (Ghidra Outfit_* / Ship_ComputeShip* over g_outfit_owned_count,
// g_outfit_defs, g_ship_class_defs). This is the basis for the fuel/shield/
// armor regeneration and travel work that depends on outfit-derived maximums
// and recharge rates; see outfit.cpp for the per-function Ghidra addresses.
//
// Ownership model (Ghidra): the player owns a stackable count of each outfit
// id (g_outfit_owned_count, 0x200 entries indexed by outfit id - 0x80). An
// outfit may provide several independent effects plus up to four mods (a
// weapon and its ammo, etc. are separate outfit ids). Several shared helpers
// aggregate those mods into effective ship stats; the original caches the
// player's results and invalidates them when outfit ownership changes
// (the -1.0 sentinel globals _DAT_00735688/_DAT_00735690/...).
//
// The outfit mod type codes are the canonical EV Nova Bible "ModType" table
// (see the notes in outfit.cpp); each named constant below matches the Bible
// number, so opcodes that this build does not yet consume are still labelled
// for future reconstruction.

#include "game_state.hpp"
#include "scenario_data.hpp"

#include <cstdint>

namespace game {

// The EV Nova outfit ModType (effect opcode) table. Values follow the Bible
// "If ModType is: Then it's:" table; only the subset this build aggregates is
// used today, but the full 1..50 set is named for clarity and later work.
enum class OutfitEffect : std::int16_t {
  kWeapon = 1,     // the id of the associated weapon resource (bay/launcher)
  kCargoSpace = 2, // tons of cargo space to add (installed permanently)
  kAmmo = 3,       // id of the associated weapon/ammo resource
  kShield = 4,     // shield points to add to max capacity
  kShieldRecharge = 5, // faster shield recharge (1000 = +1 pt/frame)
  kArmor = 6,          // armor points to add to max capacity
  kAccelerator = 7,    // accel to add (see sh\x8an)
  kSpeed = 8,          // speed to add (see sh\x8an)
  kTurn = 9,           // turn change (100 = 30 deg/sec)
  kUnused10 = 10,
  kEscapePod = 11,       // ignored
  kFuelCapacity = 12,    // extra fuel (100 = 1 jump)
  kDensityScanner = 13,  // ignored (gameplay)
  kIff = 14,             // colorized radar; ignored
  kAfterburner = 15,     // fuel use (units/sec)
  kMap = 16,             // starmap exploration range
  kCloaking = 17,        // cloaking device behavior flags
  kFuelScoop = 18,       // frames per fuel unit generated
  kAutoRefuel = 19,      // ignored
  kAutoEject = 20,       // ignored (needs escape pod)
  kCleanRecord = 21,     // govt id to clear legal record with
  kHyperspaceSpeed = 22, // +/- days hyperspace travel time
  kHyperspaceDist = 23,  // +/- no-jump zone radius
  kInterferenceMod = 24, // subtracts from system Interference for radar fuzz
  kMarines = 25,         // effective crew for capture odds
  kUnused26 = 26,
  kIncreaseMax = 27,   // multiplies the Max of another outfit by owned count
  kMurkMod = 28,       // +/- current system murkiness
  kArmorRecharge = 29, // faster armor recharge (1000 = +1 pt/frame)
  kCloakScanner = 30,  // reveal/target cloaked & untargetable ships
  kMiningScoop = 31,   // mining scoop
  kMultiJump = 32,     // extra jumps performed on a hyperspace jump
  kJam1 = 33,
  kJam2 = 34,
  kJam3 = 35,
  kJam4 = 36,
  kFastJump = 37, // enter hyperspace without slowing down
  kInertialDampener = 38,
  kIonDissipator = 39,
  kIonAbsorber = 40,
  kGravityResist = 41,
  kResistDeadlyStellars = 42,
  kPaint = 43, // 15-bit RGB ship paint color
  kReinforceInhibitor = 44,
  kModifyMaxGuns = 45,    // +/- max gun count
  kModifyMaxTurrets = 46, // +/- max turret count
  kBomb = 47,             // kills the player in flight
  kIffScrambler = 48,
  kRepairSystem = 49,  // occasionally repairs while disabled
  kNonlethalBomb = 50, // self-destructs and damages nonfatally
};

// ---------------------------------------------------------------------------
// Inventory mutation
// ---------------------------------------------------------------------------
// Effective-stats aggregation
// ---------------------------------------------------------------------------
// Computes a player ship's effective stats from the ship class and outfit
// inventory (mirrors Ship_ComputeShipMaxShieldPoints / MaxArmor /
// FuelCapacity / EffectiveThrust / speed / turn / recharge; see outfit.cpp for
// addresses). `ship_class_id` names the player's current ship class (in the
// zero-based after-128 space the scenario table expects via Ship()). The
// result does NOT read the player's current shield/armor/fuel, only the
// *capacity* / recharge-rate values.
[[nodiscard]] PlayerEffectiveStats
Outfit_ComputePlayerEffectiveStats(const GameState &state);

// Single-object ownership/limit resolution: the effective owned count and the
// maximum the player may hold for one outfit, honoring ammo-backed weapon
// limits, ModType-27 maximum multipliers and the gun/turret slot caps.
// Mirrors Outfit_ClampOutfitOwnedCountToCurrentLimits (0x004656a0).
struct OutfitOwnership {
  std::int16_t effective_owned = 0; // clamped owned count
  std::int16_t max_allowed = 0;     // what the player may hold
};

[[nodiscard]] OutfitOwnership
Outfit_ClampOwnedCountToLimits(const GameState &state,
                               std::int16_t outfit_resource_id);

// Adds/removes `count` of an outfit to the player's inventory, clamped to the
// ownership maximum (Outfit_ClampOwnedCountToLimits). Returns the number that
// was actually applied (the change may be less than requested). Marks the
// effective-stats cache dirty when anything changed. Credit is NOT handled.
[[nodiscard]] std::int16_t Outfit_AddInstalledOutfit(
    GameState &state, std::int16_t outfit_resource_id, std::int16_t count);
[[nodiscard]] std::int16_t Outfit_RemoveOutfit(GameState &state,
                                               std::int16_t outfit_resource_id,
                                               std::int16_t count);

// Cargo bookkeeping (mirrors Outfit_ComputePlayerCargoAndJunkTotal /
// ComputeFleetCargoCapacity / ComputeRemainingCargoSpace). Single-ship player
// capacity for now (no NPC fleet), so fleet capacity == the player's own
// outfit-derived cargo capacity.
[[nodiscard]] std::int16_t
Outfit_ComputePlayerCargoAndJunkTotal(const GameState &state);
[[nodiscard]] std::int16_t
Outfit_ComputePlayerFleetCargoCapacity(const GameState &state);
// Remaining free cargo space, clamped >= 0. This build has no mission-fleet
// objects, so no mission cargo is committed here.
[[nodiscard]] std::int16_t
Outfit_ComputeRemainingCargoSpace(const GameState &state);

// Whether the player owns at least one outfit whose any mod type equals
// `effect` (a cheap whole-inventory probe). The heavier per-slot helpers use
// this as a fast gate.
[[nodiscard]] bool Outfit_HasOwnedEffect(const GameState &state,
                                         OutfitEffect effect);

// Marks the effective-stats cache dirty. Called by the inventory mutation
// helpers; the spaceflight loop reads cached stats to avoid re-scanning the
// 0x200-entry outfit table every frame.
void OutfitMarkStatsDirty(GameState &state);

} // namespace game
