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
constexpr std::uint32_t kGovernmentResourceType = 0x679a7674; // g\x9avt
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
  // link_a_id (+0x04): the primary spin sprite-set id for this body's graphic
  // (the game loads spin resource id+1000 for its sprite; see Stellar_Update-
  // StellarSprites). Bounded 0..255 by the loader.
  std::int16_t link_a_id = 0;
  // link_b_id (+0x240): alternate spin sprite-set id used when the body is in
  // its non-active zone state. Bounded 0..255 (else -1) by the loader.
  std::int16_t link_b_id = -1;

  std::uint32_t flags = 0; // travel_flags (+0x06; land/dock/trade, economy...)
  std::uint16_t availability_flags = 0; // availability_flags (+0x20)
  std::int32_t tribute = 0; // Tribute
  std::int16_t tech_level = 0; // TechLevel
  std::array<std::int16_t, 8> special_tech{}; // SpecialTech1-8

  std::int16_t government_id = -1; // Govt (+0x14; <0x80 -> -1)
  std::int16_t min_status = 0;    // reputation_threshold (+0x16)

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

// Ghidra GovtDef (g_government_defs, up to 0x100 entries indexed by government
// id minus 0x80). A government defines a faction: its class/alliance/enemy
// relations, reputation penalties, AI/pilot skill, intel scan mask, theme
// colors for the HUD map, and the voice/interface/name tables. The original
// loads these from the g\x9avt resource family in Nova Data 1 and derives some
// runtime fields (voice_type_mode from the voice code range, the 8-bit theme
// colors from the 16-bit fields, fly-scaled pilot/combat skill).
//
// Field names mirror the EV Nova Bible `govmnt`/`government` layout; each
// notes the Ghidra GovtDef member (and the source payload offset) it decodes.
struct Government {
  std::string name;        // resource record name (display / HUD label)
  std::string comm_name;   // name table (comm chatter / fame label)
  std::string medium_name; // medium name table (mission/map label)

  // GovtDef +0x3e/+0x40: voice_type_code and its decoded companion mode. The
  // loader recodes raw 0..7 voices as mode -1, voices offset by 1000 as mode 1
  // and those offset by 2000 as mode 0 (subtracting the offset from the code);
  // out-of-range codes set both to -1.
  std::int16_t voice_type_code = -1;
  std::int16_t voice_type_mode = -1;

  std::uint16_t flags_primary = 0;   // GovtDef 0x20 (payload +0x02)
  // Known bits (from Government_AreGovtsAllied / _HostileOrXenophobic):
  //   0x0001 xenophobic (attacks on sight), 0x0800 derelict (no alliance checks).
  std::uint16_t scan_mask_short = 0; // GovtDef 0x22 (payload +0x04) [Provisional])
  std::int16_t ai_skill_percent = 0; // GovtDef 0x24 (payload +0x32)

  // Class / alliance / enemy id lists (payload +0x18/+0x20/+0x28). Govts share
  // a class id to be treated alike; the ally/enemy lists drive
  // Government_AreGovtsAllied / _HostileOrXenophobic (0x0046bc90 / 0x0046bdf0).
  std::array<std::int16_t, 4> classes{-1, -1, -1, -1};
  std::array<std::int16_t, 4> ally_classes{-1, -1, -1, -1};
  std::array<std::int16_t, 4> enemy_classes{-1, -1, -1, -1};

  std::int16_t interface_id = -1; // GovtDef 0x42 (payload +0xac; <0x80 -> -1)
  std::int16_t news_pic_id = -1;  // GovtDef 0x44 (payload +0xae; <0x80 -> -1)

  std::int16_t flee_shield_threshold = 0; // GovtDef 0x46 (payload +0x08)
  std::int16_t disable_penalty = 0;       // GovtDef 0x48 (payload +0x0a)
  std::int16_t board_penalty = 0;         // GovtDef 0x4a (payload +0x0c)
  std::int16_t kill_penalty = 0;          // GovtDef 0x4c (payload +0x0e)
  std::int16_t shoot_penalty = 0;         // GovtDef 0x4e (payload +0x10)
  std::int16_t max_odds = 0;              // GovtDef 0x50 (payload +0x12)
  std::int16_t bribe_cost_percent = 0;    // GovtDef 0x52 (payload +0x14)

  // Inherent electronic-warfare jamming values (GovtDef 0x54..0x5a). jam[0]
  // comes from payload +0x06; jam[1..3] from payload +0x5c..0x62, each clamped
  // to [0,100] by the loader.
  std::array<std::int16_t, 4> inherent_jam{};

  // GovtDef 0x60/0x64: pilot/combat skill as fractions. The loader scales the
  // payload int16 values by 0.01 (DAT_00575e60) and applies the defaults 1.0
  // (pilot) / 0.01 (combat) to degenerate inputs: pilot source < 1 -> 1.0,
  // combat result < 0.01 -> 0.01.
  float pilot_skill_scale = 1.0F;   // GovtDef 0x64 (payload +0x30)
  float combat_rating_scale = 0.01F; // GovtDef 0x60 (payload +0x16)

  std::uint32_t scan_mask_lo = 0; // GovtDef 0x68 (payload +0x54)
  std::uint32_t scan_mask_hi = 0; // GovtDef 0x6c (payload +0x58)

  // Theme colors tag a government's systems/ships on the HUD map. The loader
  // stores 16-bit fields at GovtDef 0x70/0x76 from a packed RGB24 payload
  // (+0xa4 theme, +0xa8 ship) and mirrors the 8-bit values at 0x7c/0x80; the
  // 8-bit forms are what gameplay consumes, so that is what we carry.
  std::uint8_t theme_red = 0, theme_green = 0, theme_blue = 0;
  std::uint8_t ship_red = 0, ship_green = 0, ship_blue = 0;

  bool present = false; // GovtDef 0x86 is_present (slots zero-filled when absent)
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
  // BkgndColor (s\xd8st +0x8e): per-system space background tint stored as 24-bit
  // 0xRRGGBB. Decoded to reproduce the original's runtime mapping (it reads the
  // resource bytes as a little-endian 32-bit and takes R=byte+0x90, G=byte+0x8f,
  // B=byte+0x8e), so `(c>>16),(c>>8),c` are exactly the colours
  // NovaRender_SetSystemSpaceBackgroundColor paints. Pure black (0) when unset.
  std::uint32_t bkgnd_color = 0;
  // Murk (s\xd8st +0x92): murkiness 0-100; a negative value hides the starfield
  // (SystemDef.alert_level < 0). Feeds the ambient-star size scale as well.
  std::int16_t murk = 0;
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
  std::vector<Government> governments; // indexed by government_id - 0x80

  // gh.id 0x80.. lookup for government/faction data.
  [[nodiscard]] const Government *Government(std::int16_t resource_id) const;

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
