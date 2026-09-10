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
  kShieldRecharge = 5, // faster shield recharge (1000 = one more point/frame)
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
  kFuelScoop = 18,       // frames per 1 unit of fuel; negative = fuel suck (see
                         // Ship_ComputeShipFuelRechargeRate 0x00463b30)
  kAutoRefuel = 19,      // auto-refueller: tops fuel to capacity at 1
                         // credit/unit on landing at a landable stellar
                         // (Outfit_RefuelShipWithCredits 0x004250f0, run
                         // from Stellar_TravelToSystem 0x00455e57; the
                         // Bible's "ignored" note is wrong for the engine)
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
// Ghidra 0x004656a0 Outfit_ClampOutfitOwnedCountToCurrentLimits.
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

// Applies the on-acquire side effects of granting the player one outfit id
// (zero-based, matching Outfit_AddInstalledOutfit / Outfit_RemoveOutfit).
// Ghidra 0x00427770 Outfit_GrantOutfitToPlayer. Scans the outfit's four
// (ModType, ModVal) slots:
//  - ModType 16 (kMap): galaxy-map reveal (see below) -> returns consumed;
//  - ModType 43 (kPaint): ship tint -> consumed (rendering not modelled yet);
//  - ModType 21 (kCleanRecord): clears negative system reputation (-1 = all
//    visible systems, else the government id in ModVal) -> consumed;
//  - otherwise: adds one to the owned inventory (via
//    Outfit_AddInstalledOutfit) -> returns NOT consumed.
// A consumed outfit is a one-shot effect item: it does not stack in the
// inventory, matching the original (map/paint/record outfits never increment
// g_outfit_owned_count).
//
// The map reveal (ModType 16 ModVal):
//  - >= 1: flood-reveals every system within ModVal links of the player's
//    current system at discovery level 2;
//  - == -1: reveals every visible neutral (government -1) system that has a
//    usable travel destination;
//  - <= -1000: reveals every visible system whose government lists
//    -(ModVal + 1000) in its Class 1-4 fields.
[[nodiscard]] bool
NovaOutfit_GrantOutfitToPlayer(GameState &state,
                               std::int16_t outfit_resource_id);

// Cargo bookkeeping (mirrors Outfit_ComputePlayerCargoAndJunkTotal /
// ComputeFleetCargoCapacity / ComputeRemainingCargoSpace). Single-ship player
// capacity for now (no NPC fleet), so fleet capacity == the player's own
// outfit-derived cargo capacity.
[[nodiscard]] std::int16_t
Outfit_ComputePlayerCargoAndJunkTotal(const GameState &state);
[[nodiscard]] std::int16_t
Outfit_ComputePlayerFleetCargoCapacity(const GameState &state);
// Ghidra 0x0046a730 Ship_ComputeShipTotalMass. The player ship's total
// tonnage: ship-class base mass plus, for each owned outfit, owned_count *
// its mass contribution (mod type 2, hull-proportional via the load-time
// derived PurchaseMass).
[[nodiscard]] std::int32_t
Outfit_ComputePlayerTotalMass(const GameState &state);

// Remaining free cargo space, clamped >= 0. This build has no mission-fleet
// objects, so no mission cargo is committed here.
[[nodiscard]] std::int16_t
Outfit_ComputeRemainingCargoSpace(const GameState &state);

// Ghidra 0x0041f330 Outfit_RedistributeFleetCargoOverflow. The fleet cargo
// overflow / jettison pass. Called with `jettison_all` true by the Player Info
// Cargo page's Jettison action (0x00499c10) and with either value by the
// in-flight dump-cargo command (0x0044aa70). Clears the player's six cargo
// bins and every junk count; `jettison_all` additionally drains the cargo of
// abortable active missions and fails them (STR# 0x7d2 0x11c "Mission
// failed."). Plays the jettison cue and shows STR# 0x7d2 0x121/0x122.
// Spawns the visible jettisoned-cargo pods through the FreeflightObjectState
// pool (Ship_SpawnFreeflightObjectForShip 0x0041f800), ROUND(share/5) clamped
// [1,12] per eligible player/escort hull. `now_ms` is the sim clock used by
// the mission-failure teardown helpers.
void NovaOutfit_RedistributeFleetCargoOverflow(GameState &state,
                                               bool jettison_all,
                                               std::uint32_t now_ms);

// Whether the player owns at least one outfit whose any mod type equals
// `effect` (a cheap whole-inventory probe). The heavier per-slot helpers use
// this as a fast gate.
[[nodiscard]] bool Outfit_HasOwnedEffect(const GameState &state,
                                         OutfitEffect effect);

// Ghidra 0x00464b50 Outfit_HasCloakingDevice. True when the player inventory
// or an NPC ship-class default loadout contains ModType 17 (cloaking device).
// NPCs also include the original control-mode-0xc escort-target exception
// involving an area-cloak device.
[[nodiscard]] bool NovaOutfit_HasCloakingDevice(const GameState &state,
                                                const Ship &ship);

// Ghidra 0x00464c80 Outfit_HasAreaCloakingDevice. Same loadout scan, but
// requires ModType 17 ModVal bit 0x1000 (area cloak).
[[nodiscard]] bool NovaOutfit_HasAreaCloakingDevice(const GameState &state,
                                                    const Ship &ship);

// Ghidra 0x00464db0 Outfit_GetCloakFuelDrainFlags and 0x00465090
// Outfit_GetCloakShieldDrainFlags. These return the ModType 17 fuel/shield
// drain flag nibbles consulted by Ship_UpdateShipCloakStateFromTraits.
[[nodiscard]] std::int16_t
NovaOutfit_GetCloakFuelDrainFlags(const GameState &state, const Ship &ship);
[[nodiscard]] std::int16_t
NovaOutfit_GetCloakShieldDrainFlags(const GameState &state, const Ship &ship);

// Ghidra 0x00464e30 Outfit_HasCloakShieldDropOnActivation. ModType 17
// ModVal bit 0x0004 forces shields to zero when cloaking activates.
[[nodiscard]] bool
NovaOutfit_HasCloakShieldDropOnActivation(const GameState &state,
                                          const Ship &ship);

// Ghidra 0x0046e060 Ship_GetShipFuelBurnRate, for the player: the last
// owned outfit encountered with opcode 15 supplies ModVal / 30 fuel per
// original simulation tick. The original caches this result until inventory
// changes; this inexpensive clean-room scan is used only while afterburning.
[[nodiscard]] float
Outfit_GetPlayerAfterburnerFuelBurnRate(const GameState &state);

// Ghidra Ship_ComputeIonizationDecayRate (0x0046c080). Returns the class base
// dissipation rate plus player-owned ModType 39 (ion dissipator) bonuses.
// Rates are charge points per millisecond, matching g_avg_frame_time_ms.
[[nodiscard]] float
NovaOutfit_ComputeIonizationDecayRate(const GameState &state, const Ship &ship);

// Ghidra 0x0046cb90 Outfit_HasMiningScoopOutfit. Whether the ship carries a
// mining-scoop outfit (ModType 0x1F in any of the four mod slots): the player
// branch scans owned outfits, the NPC branch scans the ship class's default
// outfit slots. Seeds the per-ship mining_scoop_active latch at spawn time.
[[nodiscard]] bool NovaOutfit_HasMiningScoopOutfit(const GameState &state,
                                                   const Ship &ship);

// Marks the effective-stats cache dirty. Called by the inventory mutation
// helpers; the spaceflight loop reads cached stats to avoid re-scanning the
// 0x200-entry outfit table every frame.
void OutfitMarkStatsDirty(GameState &state);

// Ghidra Mission_AccumulatePlayerContributeMask (0x0046cca0): aggregates the
// 64-bit Contribute mask from the player's ship class and owned outfits.
// TODO(decomp) skipped: the active-mission cue arms (g_system_cues 0x120-
// stride contribute pairs) and the cron-event contributes, which the port's
// provisional cue model does not carry yet.
void NovaOutfit_AccumulatePlayerContributeMask(const GameState &state,
                                               std::uint32_t &contribute_lo,
                                               std::uint32_t &contribute_hi);

// Ghidra Outfit_EvaluateRequireMask (0x0046cd80): a 64-bit Require mask is
// satisfied when every required bit is present in the player's aggregated
// Contribute mask. Used by outfit purchase gates and the mission offering
// eligibility chain (m\xefsn payload +0x656/+0x65a).
[[nodiscard]] bool NovaOutfit_EvaluateRequireMask(const GameState &state,
                                                  std::uint32_t require_lo,
                                                  std::uint32_t require_hi);

} // namespace game
