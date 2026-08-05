#pragma once

// Clean-room model of Nova's scenario data tables (systems, stellars, ship
// classes, outfits, weapons) as parsed from the shipped resource archives.
//
// The original loads these at startup in NovaData_LoadScenarioResourceTables
// (Ghidra 0x004bd3c0) by walking each resource family by id 0x80.. and parsing
// a fixed big-endian field layout (Macintosh resource heritage) into a set of
// globals (g_system_defs/SystemDef, g_stellar_defs/StellarDef,
// g_ship_class_defs/ShipClassDef, g_outfit_defs/OutfitDef,
// g_weapon_defs/WeaponDef). Field names here follow the EV Nova Bible; each
// struct names the Ghidra global/struct it reconstructs.
//
// The reimplementation keeps these as value tables owned by GameState rather
// than hidden globals (AGENTS.md). Decoders read the raw payload bytes big
// endian; sizes/permissions are validated defensively and out-of-range values
// are clamped the same way the loader clamps them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace game {

// --------------------------------------------------------------------------
// Resource four-byte type codes (big-endian file values, matching the
// resource.map records and the constants in brgr_archive.cpp).
// --------------------------------------------------------------------------
namespace scenario {
constexpr std::uint32_t kShipResourceType = 0x73689570;   // sh\x95p
constexpr std::uint32_t kOutfitResourceType = 0x6f9f7466; // o\x9ftf
constexpr std::uint32_t kWeaponResourceType = 0x77916170; // w\x91ap
constexpr std::uint32_t kStellarResourceType = 0x73709a62; // sp\x9ab
constexpr std::uint32_t kSystemResourceType = 0x73d87374; // s\xd8st
} // namespace scenario

// --------------------------------------------------------------------------
// Nova control bit (NCB) test-expression evaluator
// --------------------------------------------------------------------------
// Gates ship/outfit/mission availability and system visibility in the original
// (Ghidra NovaExpression_EvaluateToken 0x00448be0 / NovaExpression_EvaluateBoolean
// 0x00449020). Implements the Bible's test-expression grammar: Bxxx (control
// bit), Pxxx (registered-with-days), G (gender: male=1), Oxxx (owns outfit),
// Exxx (explored system), the `& | ! ( )` boolean operators, and counted sets
// `[ ... ]` compared with `= < >`. Blank expressions evaluate to true (the
// original's default). The evaluator is pure: game-state lookups are injected
// through the state callback so scenario parsing stays independent of the game.
struct ControlExpressionState {
  // Bxxx: value of Nova control bit (mission bit) `bit`.
  std::function<bool(std::uint32_t bit)> get_control_bit;
  // Pxxx: true if the game is registered (or unregistered fewer than `days`
  // days). Pass an unused `days` when registration state is unknown.
  std::function<bool(std::uint32_t days)> is_registered;
  // G: player gender lookup, true = male.
  std::function<bool()> is_male;
  // Oxxx: true if the player owns (has in cargo) at least one outfit id.
  std::function<bool(std::int16_t outfit_id)> owns_outfit;
  // Exxx: true if the player has explored system id.
  std::function<bool(std::int16_t system_id)> has_explored;
};

// Evaluates a Nova control bit test expression against `state`. Returns true
// for an empty expression. Malformed/unknown tokens evaluate as false and are
// logged. Thread-safe (no hidden globals).
[[nodiscard]] bool NovaControlExpression_Evaluate(
    std::string_view expression, const ControlExpressionState &state);

// A single stock weapon triple on a ship class: a weapon id plus how many to
// equip and the standard ammo load. The original arrays hold eight of these.
struct ShipDefaultWeaponBank {
  // Weapon resource id (128-383, stored zero-based after-128 in-game).
  std::int16_t weapon_id = -1;
  std::int16_t count = 0;
  std::int16_t ammo_load = 0;
};

// Ghidra ShipClassDef (g_ship_class_defs, 0x200 entries indexed by ship id
// minus 0x80). Only the fields the reimplementation needs so far are carried;
// the Bible documents the full set.
struct ShipClass {
  std::string display_name;       // resource name / target display
  std::string short_name;         // shipyard menu label
  std::string long_name;          // purchase dialog / new-pilot text

  std::int16_t cargo_holds = 0;   // Holds
  std::int16_t base_shield = 0;   // Shield
  std::int16_t base_armor = 0;    // Armor
  std::int16_t base_fuel = 0;     // Fuel (100 = 1 jump)
  std::int16_t free_mass = 0;     // FreeMass
  std::int16_t mass_tons = 0;     // Mass
  std::int16_t length_meters = 0; // Length
  std::int16_t tech_level = 0;    // TechLevel
  std::int32_t cost = 0;          // Cost
  std::int16_t display_weight = 0; // DispWeight

  // Movement / combat stats (float magnitudes repacked from the short field).
  float accel = 0.0F;             // Accel
  float speed = 0.0F;             // Speed
  float turn_rate = 0.0F;         // Maneuver
  float shield_recharge = 0.0F;   // ShieldRech
  float armor_recharge = 0.0F;    // ArmorRech

  std::int16_t default_ai_behavior = 0; // InherentAI
  std::int16_t class_category = 0;      // Strength? / category
  std::int16_t crew = 0;                // Crew
  std::int16_t strength = 0;            // Strength

  std::int16_t inherent_combat_govt = -1;     // InherentGovt (combat)
  std::int16_t inherent_attributes_govt = -1; // InherentGovt (attributes)
  std::uint16_t capability_flags = 0;         // Flags
  std::uint16_t flags_secondary = 0;          // Flags2
  std::uint16_t availability_flags = 0;       // Flags3

  std::int16_t max_gun = 0;        // MaxGun
  std::int16_t max_turret = 0;     // MaxTur

  // DefaultItems (outfit ids, zero-based after 0x80) + counts, up to 8. These
  // seed the player's starting inventory when bought/captured.
  std::array<std::int16_t, 8> default_outfit_ids{-1, -1, -1, -1, -1, -1, -1, -1};
  std::array<std::int16_t, 8> default_outfit_counts{};
  // The 8 stock weapon banks.
  std::array<ShipDefaultWeaponBank, 8> stock_weapons{};

  // Player-facing strings used by the new-pilot flow.
  std::string availability_expr; // Availability
  std::string on_purchase_expr;  // OnPurchase
};

// Ghidra OutfitDef (g_outfit_defs, 0x200 entries indexed by outfit id minus
// 0x80). One purchasable item / ship component.
//
// Payload layout is the o\x9ftf resource (Nova Bible): the numeric header is
// followed by the mod block, Max/Flags/Cost, the availability/on-purchase
// strings, a Contribute/Require 64-bit pair each, the ShortName/LCName/LCPlural
// strings, and a tail block holding DispWeight/Graphic/BuyRandom/ItemClass.
// The record *name* (BRGR resource.map) is surfaced as `name`, not a numeric
// header field.
struct Outfit {
  std::string name;          // resource record name (BRGR display name)
  std::string availability_expr; // Availability (control test expression)
  std::string on_purchase_expr;  // OnPurchase (control set expression)

  std::int16_t display_weight = 0; // DispWeight
  std::int16_t mass_tons = 0;      // Mass
  std::int16_t tech_level = 0;     // TechLevel
  std::int16_t mod_type = 0;       // ModType (primary)
  std::int16_t mod_val = 0;        // ModVal
  // Alternate mods 2-4 (ModType2-4 / ModVal2-4).
  std::array<std::int16_t, 3> alt_mod_types{};
  std::array<std::int16_t, 3> alt_mod_vals{};
  std::int16_t max_count = 0;      // Max
  std::uint16_t flags = 0;         // Flags
  std::int32_t cost = 0;           // Cost (4 bytes @ 0x0e of the payload)

  // Contribute / Require 64-bit pairs (contribute_lo/hi, require_lo/hi).
  // Contribute bits are ORed into the player's aggregate Contribute mask;
  // Require bits must each be covered by that mask for the item to sell.
  std::uint32_t contribute_lo = 0; // Contribute (low word)
  std::uint32_t contribute_hi = 0; // Contribute (high word)
  std::uint32_t require_lo = 0;    // Require (low word)
  std::uint32_t require_hi = 0;    // Require (high word)

  std::int16_t item_class = 0;     // ItemClass
  std::int16_t buy_random = 100;   // BuyRandom (1-100; <1/ >100 mean 100)
  std::int16_t sprite_id = 0;      // Graphic (p\x9ari sprite id)

  std::string short_name;          // ShortName (dialog menu label)
  std::string lc_name;             // LCName (lowercase singular)
  std::string lc_plural;           // LCPlural (lowercase plural)

  // Purchase-time derived cost/mass. The original computes these at load from
  // Cost/Mass and the relevant Flags bit, scaling by ship-class hull mass for
  // mass-proportional items (Flags bit 0x0200 cost, 0x0400 mass); see
  // Outfit_ComputeOutfitPurchasePrice / Outfit_ComputeOutfitPurchaseMass
  // (Ghidra 0x0046e910 / 0x0046e950).
  [[nodiscard]] std::int32_t PurchasePrice(std::int16_t ship_hull_mass) const;
  [[nodiscard]] std::int32_t PurchaseMass(std::int16_t ship_hull_mass) const;
};

// Ghidra WeaponDef (g_weapon_defs, 0x100 entries indexed by weapon id minus
// 0x80). One projectile/beam/bay weapon.
struct Weapon {
  std::string name; // resource name / status-display name

  std::int16_t reload_ticks = 30;   // Reload
  std::int16_t lifetime_ticks = 30; // Count
  std::int16_t mass_damage = 0;     // MassDmg
  std::int16_t energy_damage = 0;   // EnergyDmg
  std::int16_t guidance_mode = 0;   // Guidance
  std::int16_t weapon_mode_code = 0; // (runtime alias of Guidance)
  float projectile_speed = 0.0F;    // Speed (pixels/frame * 100)
  std::int16_t ammo_type = -1;      // AmmoType

  std::int16_t sprite_id = 0;       // Graphic
  std::int16_t inaccuracy = 0;      // Inaccuracy
  std::int16_t fire_sound = -1;     // Sound

  std::int16_t impact = 0;          // Impact
  std::int16_t explosion = -1;      // ExplodType
  std::int16_t prox_radius = 0;     // ProxRadius
  std::int16_t blast_radius = 0;    // BlastRadius

  std::uint16_t flags = 0;          // Flags
  std::uint16_t seeker = 0;         // Seeker

  std::int16_t beam_length = 0;     // BeamLength
  std::int16_t beam_width = 0;      // BeamWidth
  std::int16_t burst_count = 0;     // BurstCount
  std::int16_t burst_reload = 0;    // BurstReload
  std::int16_t max_ammo = 0;        // MaxAmmo

  std::array<std::int16_t, 4> jam_vuln{}; // JamVuln1-4
  std::uint16_t flags2 = 0;         // Flags2
  std::int16_t guided_turn = 0;     // GuidedTurn
};

// Ghidra StellarDef (g_stellar_defs, entries indexed by stellar id minus
// 0x80). One planet/station/object in a system.
struct Stellar {
  std::string name;        // resource name

  std::int16_t pos_x = 0;  // xPos
  std::int16_t pos_y = 0;  // yPos
  std::int16_t graphic_type = 0; // Type

  std::uint32_t flags = 0; // Flags (land/dock/trade, economy, etc.)
  std::int32_t tribute = 0; // Tribute
  std::int16_t tech_level = 0; // TechLevel
  std::array<std::int16_t, 8> special_tech{}; // SpecialTech1-8

  std::int16_t government_id = -1; // Govt
  std::int16_t min_status = 0;    // MinStatus

  std::int16_t cust_pict_id = -1; // CustPicID
  std::int16_t cust_snd_id = -1;  // CustSndID

  std::int16_t defense_dude_id = -1; // DefenseDude
  std::int16_t defense_count = 0;    // DefCount
  std::uint16_t flags2 = 0;         // Flags2

  std::int16_t anim_delay = 0;    // AnimDelay
  std::int16_t frame0_bias = 0;   // Frame0Bias
  std::array<std::int16_t, 8> hyperlinks{-1, -1, -1, -1, -1, -1, -1, -1}; // HyperLink1-8

  std::int16_t fee = 0;           // Fee
  // Gravity is a float in the original; stored as its encoded half/short here.
  std::int16_t gravity = 0;       // Gravity
  std::int16_t weapon_id = -1;    // Weapon
  std::int32_t strength = 0;      // Strength (negative/total = invincible)
  std::int16_t dead_type = 0;     // DeadType
  std::int16_t dead_time = 0;     // DeadTime
  std::int16_t explosion_type = -1; // ExplodType
};

// Ghidra SystemDef (g_system_defs, entries indexed by system id minus 0x80).
// One star system; links to 16 others and holds stellar nav defaults.
struct System {
  std::string name;            // resource name / map label
  std::int16_t pos_x = 0;      // xPos
  std::int16_t pos_y = 0;      // yPos
  std::array<std::int16_t, 16> links{};   // Con1-16 (system ids, stored -1/zero-based)
  std::array<std::int16_t, 16> nav_defs{-1, -1, -1, -1, -1, -1, -1, -1,
                                        -1, -1, -1, -1, -1, -1, -1, -1}; // NavDef1-16 (stellar ids)
  std::array<std::int16_t, 8> dude_types{}; // DudeTypes (128-639, or neg fleet)
  std::array<std::int16_t, 8> dude_prob{};  // % Prob
  std::int16_t avg_ships = 0;  // AvgShips
  std::int16_t government_id = -1; // Govt
  std::int16_t message_id = -1; // Message
  std::int16_t asteroid_count = 0; // Asteroids
  std::int16_t interference = 0;  // Interference
  std::uint32_t bkgnd_color = 0;  // BkgndColor
  std::int16_t murk = 0;          // Murk
  std::uint16_t ast_types = 0;    // AstTypes
  std::int16_t reinf_fleet = -1;  // ReinfFleet
  std::int16_t reinf_time = 0;    // ReinfTime
  std::int16_t reinf_interval = 0; // ReinfIntrval
  std::string visibility_expr;    // Visibility
};

// Owns the parsed scenario tables indexed by (resource id - 0x80), mirroring
// the original global arrays (g_ship_class_defs etc.). Filled by
// LoadFromArchives(). Entries are created for every id in the family's valid
// range so callers can index directly; missing/empty resources decode to
// defaults (the original zero-fills those slots).
struct ScenarioData {
  std::vector<ShipClass> ships;      // indexed by ship_id - 0x80
  std::vector<Outfit> outfits;       // indexed by outfit_id - 0x80
  std::vector<Weapon> weapons;       // indexed by weapon_id - 0x80
  std::vector<Stellar> stellars;     // indexed by stellar_id - 0x80
  std::vector<System> systems;       // indexed by system_id - 0x80

  // gh.id 0x80.. convention: returns the entry for the given resource id, or
  // nullptr when it is outside the loaded range.
  [[nodiscard]] const ShipClass *Ship(std::int16_t resource_id) const;
  [[nodiscard]] const Outfit *Outfit(std::int16_t resource_id) const;
  [[nodiscard]] const Weapon *Weapon(std::int16_t resource_id) const;
  [[nodiscard]] const Stellar *Stellar(std::int16_t resource_id) const;
  [[nodiscard]] const System *System(std::int16_t resource_id) const;

  // Ghidra NovaData_LoadScenarioResourceTables (0x004bd3c0). Walks each
  // resource family by id 0x80.. max and decodes it into the matching table.
  // Returns true when at least the ship and weapon tables have data.
  [[nodiscard]] bool LoadFromArchives();
};
} // namespace game
