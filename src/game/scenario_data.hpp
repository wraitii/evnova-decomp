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
#include <string_view>
#include <vector>

namespace game {

// --------------------------------------------------------------------------
// Resource four-byte type codes (big-endian file values, matching the
// resource.map records and the constants in brgr_archive.cpp).
// --------------------------------------------------------------------------
namespace scenario {
constexpr std::uint32_t kShipResourceType = 0x73689570;       // sh\x95p
constexpr std::uint32_t kOutfitResourceType = 0x6f9f7466;     // o\x9ftf
constexpr std::uint32_t kWeaponResourceType = 0x77916170;     // w\x91ap
constexpr std::uint32_t kStellarResourceType = 0x73709a62;    // sp\x9ab
constexpr std::uint32_t kSystemResourceType = 0x73d87374;     // s\xd8st
constexpr std::uint32_t kGovernmentResourceType = 0x679a7674; // g\x9avt
constexpr std::uint32_t kFleetResourceType = 0x666c9174;      // fl\x91t
// d\x9fde (0x649f6465) — the "dude" (NPC pilot/drifter) template family. One
// record per dude def in Nova Data 1; see DudeDef. Loaded by the original
// NovaData_LoadScenarioResourceTables (0x004bd3c0) into the g_dude_defs table.
constexpr std::uint32_t kDudeResourceType = 0x649f6465; // d\x9fde
// "r\x9aid" (0x729a6964) — the ASTEROID/roid class family. Records are named
// "Metal Small".."Crystal Huge" (4 compositions x 4 sizes = 16 entries,
// resource ids 0x80..0x8f, the r\xf6id asteroid ids 128..143) and define the
// per-type wander/drift parameters fed to AsteroidState spawns
// (Asteroid_SpawnRecord 0x00421e60 / Asteroid_Spawn 0x00421830 wander reads).
// Decoded by the original loader (NovaData_LoadScenarioResourceTables
// 0x004bd3c0 at 0x004c6207) into the DAT_005912dc / DAT_005912f0 global pair,
// which share one 0x1c-byte-strided 16-row table. See AsteroidDef.
constexpr std::uint32_t kAsteroidResourceType = 0x729a6964; // r\x9aid
// m\x957n (0x6d95736e) — mission definitions and their embedded mission-ship
// records. The original loads up to 1000 entries into MisnDef (stride 0x12c)
// and later reads the same resource family by mission id when populating an
// accepted mission. See NovaResources_LoadMisnResourceDefs (0x0043bbb0).
constexpr std::uint32_t kMissionResourceType = 0x6d95736e;
// Impact/explosion definition family read by
// NovaData_LoadScenarioResourceTables (0x004bd3c0). The binary FourCC is shown
// as 0x629a9a6d by Ghidra; each record is 0x18 bytes in the source resource and
// contributes the three runtime fields below.
constexpr std::uint32_t kImpactEffectResourceType = 0x629a9a6d;
} // namespace scenario

// --------------------------------------------------------------------------
// Nova control bit (NCB) test-expression evaluator
// --------------------------------------------------------------------------
// Gates ship/outfit/mission availability and system visibility in the original
// (Ghidra NovaExpression_EvaluateToken 0x00448be0 /
// NovaExpression_EvaluateBoolean 0x00449020). Implements the Bible's
// test-expression grammar: Bxxx (control bit), Pxxx (registered-with-days), G
// (gender: male=1), Oxxx (owns outfit), Exxx (explored system), the `& | ! ( )`
// boolean operators, and counted sets
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

// Mutation callback used by the OnPurchase/OnSell/OnRetire NCB set strings.
// Set strings are a distinct language from availability tests: a bare Bn sets
// bit n, while !Bn or Bn=0 clears it. Unknown directives are deliberately
// ignored by this small executor until their opcode has been reconstructed.
struct ControlExpressionMutation {
  std::function<void(std::uint32_t bit, bool value)> set_control_bit;
};

// Evaluates a Nova control bit test expression against `state`. Returns true
// for an empty expression. Malformed/unknown tokens evaluate as false and are
// logged. Thread-safe (no hidden globals).
[[nodiscard]] bool
NovaControlExpression_Evaluate(std::string_view expression,
                               const ControlExpressionState &state);
void NovaControlExpression_ExecuteSet(
    std::string_view expression, const ControlExpressionMutation &mutation);

// Mission resource definition. This is deliberately a value model rather
// than a byte-for-byte packed struct: the original's 0x12c-byte MisnDef is a
// runtime projection of a larger m\x957n resource. Fields below are the
// offsets/uses established by the Ghidra loader; raw_payload retains the
// source bytes while the remaining fields are decoded.
struct MissionDef {
  bool present = false;
  std::int16_t link_system_filter = -1;     // MisnDef +0x00
  std::int16_t return_stellar_id = -1;      // resource +0x04
  std::int16_t special_ship_goal = 0;       // resource +0x04
  std::int16_t special_ship_behavior = 0;   // +0x06
  std::int16_t special_ship_start = 0;      // +0x08
  std::int16_t special_ship_count = 0;      // resource +0x12
  std::int16_t special_ship_system = -1;    // resource +0x10
  std::int16_t on_start_condition = -1;     // resource +0x5a
  std::int16_t on_fail_condition = -1;      // resource +0x0c
  std::int16_t on_success_condition = -1;   // resource +0x0e
  std::int16_t aux_ship_dude = -1;          // resource +0x24
  std::int16_t aux_ship_system = -1;        // resource +0x22
  std::int16_t special_ship_dude = -1;      // resource +0x24; active +0x08
  std::int16_t cargo_type = -1;             // +0x40
  std::int16_t cargo_quantity = 0;          // +0x42
  std::int16_t on_resolve_repeat_count = 0; // +0x48
  std::int32_t resource_delta_or_cost = 0;  // +0x4a
  std::int16_t aux_ships_left = 0;          // +0x50
  std::int16_t initial_ship_count = 0;      // +0x52
  std::uint16_t flags_primary = 0;          // resource +0x50; active +0x55
  std::uint16_t flags_secondary = 0;        // resource +0x52; active +0x57
  // Additional fields copied by Mission_PopulateMissionSlotFromDef
  // (0x0043f8c0). Names are intentionally descriptive but provisional where
  // the original MissionDef member remains unnamed.
  std::int16_t target_ship_count = 0;            // resource +0x20
  std::int16_t current_system_locator = -1;      // +0x22
  std::int16_t spawn_behavior = 0;               // +0x26
  std::int16_t fleet_spawn_goal = 0;             // +0x28
  std::int16_t special_ship_spawn_mode = 0;      // +0x2c
  std::int16_t competing_government_id = -1;     // +0x2e
  std::int16_t competing_reputation_delta = 0;   // +0x30
  std::int16_t special_ship_name_string_id = -1; // +0x2a
  std::int16_t random_text_string_id = -1;       // +0x32
  std::int16_t mission_ship_count_max = 0;       // +0x48
  std::int16_t auxiliary_ship_dude = -1;         // +0x4c
  std::int16_t mission_fleet_metric = 0;         // +0x4a
  std::int16_t start_system_locator = -1;        // +0x22, resolved at accept
  bool start_visited = false;                    // +0x42
  std::int16_t initial_briefing_id = -1;         // +0x34
  std::array<std::int16_t, 8> brief_description_ids{}; // +0x34..+0x42
  // Mission availability expression from the resource string block (+0x5c).
  // The original caches its result at MisnDef +0x16.
  std::string availability_expr;
  bool is_available_runtime = false;
  // MisnDef +0x128, loaded from mïsn payload +0x7a0. Lower values sort first.
  std::int16_t list_priority = 0;
  std::string display_name;
  std::array<std::byte, 0x7b2> raw_payload{};
};

// MissionShipDef fields currently identified by the type layout. The large
// weapon delta arrays are kept because they are semantically load-bearing for
// mission-ship spawning; unknown bytes remain available in raw_payload.
struct MissionShipDef {
  std::int16_t spawn_system_filter = -1;                    // +0x00
  std::int16_t government_id = -1;                          // +0x02
  std::int16_t ai_behavior_code = 0;                        // +0x04
  std::int16_t aggression_level = 0;                        // +0x06
  std::int16_t ship_class_id = -1;                          // +0x0a
  std::array<std::int16_t, 0x100> weapon_ammo_delta{};      // +0x18
  std::array<std::int16_t, 0x100> weapon_secondary_delta{}; // +0x418
  std::int32_t booty_base_credits = 0;                      // +0x618
  float shield_armor_scale = 1.0F;                          // +0x61c
  bool is_available_runtime = false;                        // +0x620
  std::uint8_t runtime_flag_a = 0;                          // +0x622
  std::uint8_t runtime_flag_b = 0;                          // +0x623
  std::uint8_t unique_spawn_tag = 0;                        // +0x624
  std::string availability_expression;      // +0x644, max 326 bytes
  std::int16_t display_name_string_id = -1; // +0x78a
  std::array<std::byte, 0x794> raw_payload{};
};

// A single stock weapon triple on a ship class: a weapon id plus how many to
// equip and the standard ammo load. The original arrays hold eight of these.
struct ShipDefaultWeaponBank {
  // Weapon resource id (128-383, stored zero-based after-128 in-game).
  std::int16_t weapon_id = -1;
  std::int16_t count = 0;
  std::int16_t ammo_load = 0;
};

// Ghidra ShipClassDef.tech_level sentinel (== (short)0xd8f1 == -9999) marking
// a class as nonexistent. Ship_HandleShip deactivates any ship flying such a
// class, and DudeDef decode nulls a ship type referencing one.
inline constexpr std::int16_t kShipClassNonexistentTechLevel =
    static_cast<std::int16_t>(0xd8f1);

// Ghidra ShipClassDef (g_ship_class_defs, 0x200 entries indexed by ship id
// minus 0x80). Only the fields the reimplementation needs so far are carried;
// the Bible documents the full set.
struct ShipClass {
  std::string display_name; // resource name / target display
  std::string short_name;   // shipyard menu label
  std::string long_name;    // purchase dialog / new-pilot text

  std::int16_t cargo_holds = 0;   // Holds
  std::int16_t base_shield = 0;   // Shield
  std::int16_t base_armor = 0;    // Armor
  std::int16_t base_fuel = 0;     // Fuel (100 = 1 jump)
  std::int16_t free_mass = 0;     // FreeMass
  std::int16_t mass_tons = 0;     // Mass
  std::int16_t length_meters = 0; // Length
  std::int16_t tech_level = 0;    // TechLevel
  std::int32_t cost = 0;          // Cost
  // Bible: DeathDelay, Explode1, and Explode2.
  std::int16_t death_delay_frames = 0;
  std::int16_t destruction_effect_while_breaking = -1;
  std::int16_t destruction_effect_final = -1;
  std::int16_t display_weight = 0; // DispWeight

  // Movement / combat stats (float magnitudes repacked from the short field).
  float accel = 0.0F;           // Accel
  float speed = 0.0F;           // Speed
  float turn_rate = 0.0F;       // Maneuver
  float shield_recharge = 0.0F; // ShieldRech
  float armor_recharge = 0.0F;  // ArmorRech

  std::int16_t default_ai_behavior = 0; // InherentAI
  std::int16_t class_category = 0;      // Strength? / category
  std::int16_t crew = 0;                // Crew
  std::int16_t strength = 0;            // Strength

  std::int16_t inherent_combat_govt = -1;     // InherentGovt (combat)
  std::int16_t inherent_attributes_govt = -1; // InherentGovt (attributes)
  std::uint16_t capability_flags = 0;         // Flags
  std::uint16_t flags_secondary = 0;          // Flags2
  std::uint16_t availability_flags = 0;       // Flags3

  std::int16_t max_gun = 0;    // MaxGun
  std::int16_t max_turret = 0; // MaxTur

  // Ghidra ShipClassDef +0xA20 (ship payload +0x4c). The starting
  // timed_action_counter for a newly allocated ship; Ship_AllocateShipSlot
  // (Ship_AllocateShipSlotInSystem 0x004254b0) seeds a fresh ship slot from it.
  std::int16_t timed_action_counter_init = 0;

  // Ghidra ShipClassDef +0xA00 (payload +0x60). Pilot skill variance percent
  // used to seed each NPC's skill_variance_scale; it also gates some AI
  // cadence decisions. Valid stock values are 1..50%.
  std::int16_t skill_variance_percent = 0;
  // Ghidra ShipClassDef +0x9FA (ionization capacity). NPC effective-stat
  // helpers divide ShipState.ionization_points by this value; zero means
  // no ionization bar. The scenario loader reads the packed resource field at
  // payload +0x36C.
  std::int16_t ionization_capacity = 0;
  // Ghidra ShipClassDef +0x48 / resource payload +0x36A. Base ionization
  // dissipation rate in charge points per millisecond after the loader's
  // 0.01 scale and minimum-one clamp.
  float ionization_decay_rate = 0.0F;
  // Ghidra ShipClassDef +0xA24. Sprite/behavior flags (bit 1 = carries
  // waypoint arrival markers, bit 2 = banking ships with sprite_behavior_flags
  // 0x80 timing, etc.). Consulted by the AI travel/arrive logic.
  std::uint16_t sprite_behavior_flags = 0;

  // Ghidra ShipClassDef +0xA0A (pict_fallback_sprite_resource_id): the large
  // (200x200) portrait PICT drawn in the ship-comm dialog (DLOG 0x3ef item 10)
  // and the shipyard list. Ghidra NovaData_LoadAllShipClassVisualAndLaunchData
  // (0x004aeda0) stores `5000 + (zero-based class id)` when PICT(index+5000)
  // exists, otherwise `5000 + clone_source_ship_class` (the source's portrait)
  // -- the portrait lives in the 5000+ PICT range, distinct from the target-
  // panel 3000+ PICT set. 0 when neither resolution succeeded.
  std::uint16_t pict_fallback_sprite_resource_id = 0;

  // Ghidra ShipClassDef +0xA0C (clone_source_ship_class). The zero-based ship
  // class that owns the base sprites this class shares: the first class whose
  // sh\x8an BaseImageID matches this class's (derived by ShipClass_LoadShip-
  // ClassVisualAndLaunchData 0x004b4ee0's clone branch, stored at load time by
  // NovaData_LoadAllShipClassVisualAndLaunchData 0x004aeda0). Classes that
  // clone an earlier class reuse its target-info PICT (Bible: "give the first
  // of any series of identical-looking ship types a target pict ... and the
  // engine will use it for all higher-numbered ship types with the same base
  // sprites"); the portrait resource is 3000 + this id. -1 when not derived
  // (the decoder falls back to the class's own id).
  std::int16_t clone_source_ship_class = -1;

  // DefaultItems (outfit ids, zero-based after 0x80) + counts, up to 8. These
  // seed the player's starting inventory when bought/captured.
  std::array<std::int16_t, 8> default_outfit_ids{
      -1, -1, -1, -1, -1, -1, -1, -1};
  std::array<std::int16_t, 8> default_outfit_counts{};
  // The 8 stock weapon banks.
  std::array<ShipDefaultWeaponBank, 8> stock_weapons{};

  // Player-facing strings used by the new-pilot flow.
  std::string availability_expr; // Availability
  std::string on_purchase_expr;  // OnPurchase
  std::string on_retire_expr;    // OnRetire
  // Contribute / Require 64-bit pairs (payload +0x64/+0x68 and +0x380/+0x384).
  // Contribute is the ship's baseline for the player's aggregate Contribute
  // mask (Mission_AccumulatePlayerContributeMask reads ship
  // ShipClassDef.field_0xa30/0xa34, populated from shp +0x64/+0x68) and gates
  // Require checks on outfits/missions/ships. See landed_store ContributeMask.
  std::uint32_t contribute_lo = 0;
  std::uint32_t contribute_hi = 0;
  std::uint32_t require_lo = 0;
  std::uint32_t require_hi = 0;
  std::int16_t buy_random = 100;
  std::int16_t hire_random = 100;
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
  std::string name;              // resource record name (BRGR display name)
  std::string availability_expr; // Availability (control test expression)
  std::string on_purchase_expr;  // OnPurchase (control set expression)
  std::string on_sell_expr;      // OnSell (control set expression)

  std::int16_t display_weight = 0; // DispWeight
  std::int16_t mass_tons = 0;      // Mass
  std::int16_t tech_level = 0;     // TechLevel
  std::int16_t mod_type = 0;       // ModType (primary)
  std::int16_t mod_val = 0;        // ModVal (zero-based for weapon/ammo/bomb)
  // Alternate mods 2-4 (ModType2-4 / ModVal2-4).
  std::array<std::int16_t, 3> alt_mod_types{};
  std::array<std::int16_t, 3> alt_mod_vals{};
  // Decode stores weapon/ammo/bomb mod values zero-based (the loader subtracts
  // 0x80 for ModType 1/3/0x15 with ModVal > 0x7f; see DecodeOutfit), so a
  // weapon outfit's mod_val equals its zero-based weapon bank slot (resource id
  // minus 0x80) and weapon banking (NovaWeapon_*) indexes banks directly with
  // mod_val. Other mod types keep their raw payload value.
  std::int16_t max_count = 0; // Max
  std::uint16_t flags = 0;    // Flags
  std::int32_t cost = 0;      // Cost (4 bytes @ 0x0e of the payload)

  // Contribute / Require 64-bit pairs (contribute_lo/hi, require_lo/hi).
  // Contribute bits are ORed into the player's aggregate Contribute mask;
  // Require bits must each be covered by that mask for the item to sell.
  std::uint32_t contribute_lo = 0; // Contribute (low word)
  std::uint32_t contribute_hi = 0; // Contribute (high word)
  std::uint32_t require_lo = 0;    // Require (low word)
  std::uint32_t require_hi = 0;    // Require (high word)

  std::int16_t item_class = 0;   // ItemClass
  std::int16_t buy_random = 100; // BuyRandom (1-100; <1/ >100 mean 100)
  std::int16_t sprite_id = 0;    // Graphic (p\x9ari sprite id)
  // Runtime field_0x378 is initialized from Flags bit 0x0004 and is the
  // marker retained across player-ship replacement.
  bool persistent_on_ship_swap = false;

  std::string short_name; // ShortName (dialog menu label)
  std::string lc_name;    // LCName (lowercase singular)
  std::string lc_plural;  // LCPlural (lowercase plural)

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

  // Field offsets are verified against the raw w\x91ap payload bytes and the
  // original loader (0x004bd3c0) which copies each to a g_weapon_defs slot.
  // The Bible ordering and the loader's provisional WeaponDef names differ
  // slightly; the offset values below are the source of truth (validated
  // against the Light Blaster payload: reload 10, count 13, etc.) and the
  // Ghidra name is noted where it diverges.

  // Reload0 (Ghidra speed_scalar): the fire cadence, in reference frames, for
  // a non-burst weapon. The original Weapon_FirePlayerWeaponBank sets the
  // bank cooldown to this value (Weapon_GetWeaponBurstAttempts == 1); stored
  // as float speed_scalar in WeaponDef.
  std::int16_t reload_ticks = 30;
  std::int16_t lifetime_ticks = 30;  // Count2
  std::int16_t mass_damage = 0;      // MassDmg4 (WeaponDef field_0x2)
  std::int16_t energy_damage = 0;    // EnergyDmg6 (WeaponDef field_0x4)
  std::int16_t guidance_mode = 0;    // Guidance8
  std::int16_t weapon_mode_code = 0; // (runtime alias of Guidance)
  float projectile_speed = 0.0F;     // Speed_a, raw (pixels/frame * 100);
                                     // divide by 100 for px/frame.
  // WeaponDef +0x5c, built by the original loader after all weapons are read.
  // For projectile-like modes this is the normalized speed * lifetime, plus
  // any linked weapon contributions. Turret selection adds the original
  // 32-pixel reach allowance at the call site.
  float range_scalar = 0.0F;
  std::int16_t ammo_type = -1; // AmmoType_c (ammo_or_energy_cost_code)

  std::int16_t sprite_id = 0;   // Graphic_e (shot_sprite_set_id)
  std::int16_t inaccuracy = 0;  // Inaccuracy10 (shot_random_spread)
  std::int16_t fire_sound = -1; // Sound12 (fire_sound_slot)

  // Impact14: impulse applied to the struck ship, not an audio resource. The
  // Ghidra runtime field is still provisionally named `impact_sound_slot`.
  std::int16_t impact_impulse = 0;
  std::int16_t impact_effect_id = -1; // ExplodType16
  std::int16_t blast_radius = 0;      // ProxRadius18
  std::int16_t splash_radius = 0;     // BlastRadius1a

  // Fuse (resource +0x22; Ghidra WeaponDef.fuse_ticks). A positive value
  // advances the shot's fuse_elapsed timer in Shot_HandleShot.
  std::int16_t fuse_ticks = 0;
  // Resource +0x46, stored by Ghidra as WeaponDef.field_0x1c. The collision
  // callback and Shot_ResolveCollisions stop accepting contacts during this
  // final portion of a shot's lifetime. The exact data-editor label remains
  // provisional because the same field also controls late sprite-frame wrap.
  std::int16_t late_collision_window_ticks = 0;

  // Ionization (+0x4a) is accumulated on ShipState.ionization_points when
  // this weapon hits. IonizeColor (+0x72) is retained as packed RGB for the
  // later ship-tint/status renderer.
  std::int16_t ionization_points = 0;
  std::uint32_t ionization_color = 0;

  std::uint16_t flags = 0;            // Flags1c (flags_primary)
  std::uint16_t flags_quaternary = 0; // Seeker1e (flags_quaternary)
  std::uint16_t flags_secondary = 0;  // (resource +0x48, flags_secondary)
  std::uint16_t flags_tertiary = 0;   // (resource +0x66, flags_tertiary)

  // The resource field is BeamLength, not a turret angle. The original uses
  // BeamLength + 32 as the range envelope for beam modes 0/3/10.
  std::int16_t beam_length_px = 0; // resource +0x30 / Ghidra +0x70
  // Ghidra WeaponDef.homing_strength_or_turn_rate (+0x72, loaded from resource
  // +0x32): dual-purpose. For an animation-frame weapon set it is the shot
  // animation frame-dwell time in ms (Shot_HandleShot accumulates it into
  // ShotState.anim_elapsed and steps frame_cycle_index each time the dwell is
  // crossed); for guided weapons it is the turn rate. The Light Blaster's
  // payload keeps it at 0, which makes even an animated frame-stepper advance
  // every frame.
  std::int16_t shot_anim_frame_dwell = 0;
  std::int16_t kickback_impulse =
      0; // (resource +0x56, WeaponDef field_0x12;
         // recoil kickback, was mislabeled burst_count)
  std::int16_t turret_group_id = -1; // (resource +0x58, was mislabeled
                                     // burst_reload)
  // Burst-cycle fields drive a weapon that fires a burst then resets on a
  // cooldown (Weapon_GetWeaponFireIntervalTicks / Weapon_FirePlayerWeaponBank).
  std::int16_t burst_cycle_ticks = 0; // (resource +0x5a, was mislabeled
                                      // max_ammo)
  std::int16_t burst_reset_cooldown = 0;
  std::int16_t retarget_interval_ticks = 0;

  // Resource +0x3e/+0x40/+0x44 feed the original loader's post-pass range
  // calculation. A valid link adds that weapon's travel distance; the final
  // count is used only for the self-link special case.
  std::int16_t range_link_gate = 0;
  std::int16_t range_link_weapon_id = -1; // zero-based, -1 when absent
  std::int16_t range_link_extra_count = 0;

  std::array<std::int16_t, 4> jam_vuln{}; // JamVuln1-4 (resource +0x5e..)
};

// Ghidra StellarDef (g_stellar_defs, entries indexed by stellar id minus
// 0x80). One planet/station/object in a system.
struct Stellar {
  std::string name; // resource name

  std::int16_t pos_x = 0; // xPos
  std::int16_t pos_y = 0; // yPos
  // link_a_id (+0x04): the primary spin sprite-set id for this body's graphic
  // (the game loads spin resource id+1000 for its sprite; see Stellar_Update-
  // StellarSprites). Bounded 0..255 by the loader.
  std::int16_t link_a_id = 0;
  // link_b_id (+0x240): alternate spin sprite-set id used when the body is in
  // its non-active zone state. Bounded 0..255 (else -1) by the loader.
  std::int16_t link_b_id = -1;

  std::uint32_t flags = 0; // travel_flags (+0x06; land/dock/trade, economy...)
  std::uint16_t availability_flags = 0;       // availability_flags (+0x20)
  std::int32_t tribute = 0;                   // Tribute
  std::int16_t tech_level = 0;                // TechLevel
  std::array<std::int16_t, 8> special_tech{}; // SpecialTech1-8

  std::int16_t government_id = -1; // Govt (+0x14; <0x80 -> -1)
  std::int16_t min_status = 0;     // reputation_threshold (+0x16)
  // engage_highlight_frame (Ghidra StellarDef +0x26, payload +0x18): the
  // frame index at which an animated stellar (hypergate/wormhole, avail
  // 0x1000) shows its engaged/pulse highlight. Clamped to the middle frame by
  // the animator when unset/out of range.
  std::int16_t engage_highlight_frame = 0;

  std::int16_t cust_pict_id = -1; // CustPicID
  std::int16_t cust_snd_id = -1;  // CustSndID

  std::int16_t defense_dude_id = -1; // DefenseDude
  std::int16_t defense_count = 0;    // DefCount
  std::uint16_t flags2 = 0;          // Flags2

  // Animation timing for an animated stellar (Ghidra StellarDef +0x470/+0x472;
  // loaded from the sp\x6fb payload +0x22/+0x24). animation_dwell_time is the
  // Bible AnimDelay (frame dwell, 30ths of a second);
  // animation_frame_multiplier is the Bible Frame0Bias, a multiplier that holds
  // the first (frame 0) of the sequence longer (used when current_frame == 0).
  // Drives the frame stepping in Ghidra Stellar_UpdateStellarSprites
  // (0x0042cd10).
  std::int16_t animation_dwell_time = 0;       // AnimDelay
  std::int16_t animation_frame_multiplier = 0; // Frame0Bias
  std::array<std::int16_t, 8> hyperlinks{
      -1, -1, -1, -1, -1, -1, -1, -1}; // HyperLink1-8

  // service_cost (StellarDef +0x38, payload +0x234; ServiceCost): the landing/
  // Destination service cost. Its exact collection path remains unverified;
  // do not treat this as a direct target-action docking fee.
  std::int32_t service_cost = 0;
  // Gravity is a float in the original; stored as its encoded half/short here.
  std::int16_t gravity = 0;         // Gravity
  std::int16_t weapon_id = -1;      // Weapon
  std::int32_t strength = 0;        // Strength (negative/total = invincible)
  std::int16_t dead_type = 0;       // DeadType
  std::int16_t dead_time = 0;       // DeadTime
  std::int16_t explosion_type = -1; // ExplodType

  // ---- Runtime targeting / display state (decoded with, not from, the
  // payload). The original keeps these on StellarDef (+0x14/+0x44/+0x45/
  // +0x46/+0x3c/+0x40/+0x47c) and re-derives them per tick in
  // System_UpdateSystemAndStellarDisplayState (0x00432470) / the sprite pass;
  // they gate the targeting/selection predicates in targeting.cpp. ----
  // Which 0-based system this stellar belongs to (+0x14). Re-homed to the
  // player's current system when it is found in that system's nav list.
  std::int16_t system_id = -1;
  // is_available (+0x44): whether the stellar may currently be interacted
  // with (its system is visible and it is reachable). Set by the display-state
  // refresh.
  bool is_available = false;
  // hazard_marker (field_0x46): set when the stellar's system is visible and
  // its availability_flags carry the 0x20 hazard/derelict bit; colours the
  // stellar as a hazard on the radar/target display.
  bool hazard_marker = false;
  // sprite_population (+0x40): count of ambient sprites currently spawned for
  // this stellar. A population >0 combined with a live/engaged sprite handle is
  // one half of Stellar_IsStellarActive's gate.
  int sprite_population = 0;
  // sprite_handle_active (+0x3c): whether the stellar's ambient sprite is
  // presently loaded via a live handle (in the original a negative handle is
  // the active sentinel). Read along with the engagement access counter by
  // Stellar_IsStellarActive.
  bool sprite_handle_active = false;
  // engage_access (+0x47c): the stellar's engagement-access counter, bumped by
  // the travel/targeting interaction when a ship engages this stellar. >0 keeps
  // the stellar "active" even while its ambient sprite is unloaded.
  std::int16_t engage_access = 0;

  // ---- Hostile-ship deposit bookkeeping (Ghidra StellarDef +0x4e/+0x50 and
  // the derelict sentinel +0x47). The original keeps a pool of patrol/defence
  // ships staged at a stellar (max_ship_count = the mounted garrison size,
  // present_ship_count = how many are currently spawned). The destination-
  // interaction dialog's attack branch (NovaUi_RunTravelDestinationInter-
  // actionWindow 0x00480030) scans these to decide whether to spawn a fresh
  // hostile fleet for the stellar, and rescales present_ship_count after a
  // confrontation. field_0x47 is a single-byte derelict / abandoned sentinel
  // that suppresses the hostile re-spawn latching. TODO(decomp): the attack
  // branch that reads them is deferred (see negotiation_dialog.cpp); these
  // fields are modelled now so the data is present.
  int present_ship_count = 0; // StellarDef +0x50
  int max_ship_count = 0;     // StellarDef +0x4e (garrison size; >0x3e9/0x2711
                              //  rescale branches)
  std::uint8_t field_0x47 = 0;
  // Runtime destruction latch used by the Y/U mission-script operators.
  // The original stores this across several unnamed StellarDef fields; this
  // explicit projection keeps the gameplay state testable.
  bool is_destroyed = false;
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

  std::uint16_t flags_primary = 0; // GovtDef 0x20 (payload +0x02)
  // Known bits (from Government_AreGovtsAllied / _HostileOrXenophobic):
  //   0x0001 xenophobic (attacks on sight), 0x0800 derelict (no alliance
  //   checks).
  // Known bits (from NovaUi_PaintStarmapGovDisc 0x004aa070 / the political
  // overlay): 0x0002 = political-map small-disc tier (overlay disc radius
  // round(11/zoom)+9 cells, strength fade 0.4, instead of round(22/zoom)+12 /
  // 0.2); 0x0004 = excluded from the political map (no overlay disc drawn).
  std::uint16_t scan_mask_short =
      0; // GovtDef 0x22 (payload +0x04) [Provisional]
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
  float pilot_skill_scale = 1.0F;    // GovtDef 0x64 (payload +0x30)
  float combat_rating_scale = 0.01F; // GovtDef 0x60 (payload +0x16)

  std::uint32_t scan_mask_lo = 0; // GovtDef 0x68 (payload +0x54)
  std::uint32_t scan_mask_hi = 0; // GovtDef 0x6c (payload +0x58)

  // Theme colors tag a government's systems/ships on the HUD map. The loader
  // stores 16-bit fields at GovtDef 0x70/0x76 from a packed RGB24 payload
  // (+0xa4 theme, +0xa8 ship) and mirrors the 8-bit values at 0x7c/0x80; the
  // 8-bit forms are what gameplay consumes, so that is what we carry.
  std::uint8_t theme_red = 0, theme_green = 0, theme_blue = 0;
  std::uint8_t ship_red = 0, ship_green = 0, ship_blue = 0;

  // Two per-government boolean policy flags (GovtDef +0x84, provisional).
  // Government_GetGovernmentPolicyFlag (0x0046e860) reads policy_flags[index]
  // for index 0/1; flag 0 gates player target acquisition
  // (Ship_IsShipAcquirableAsTarget 0x0040faa0: a candidate with flag 0 set is
  // acquirable) and several aggro/relation decisions. The writer is not yet
  // identified (mission/faction hostility system), so the flag stays 0 in the
  // current build (TODO(decomp)).
  std::array<std::uint8_t, 2> policy_flags{}; // GovtDef +0x84/+0x85

  bool present =
      false; // GovtDef 0x86 is_present (slots zero-filled when absent)
};

// Ghidra SystemDef (g_system_defs, entries indexed by system id minus 0x80).
// One star system; links to 16 others and holds stellar nav defaults.
struct System {
  std::string name;       // resource name / map label
  std::int16_t pos_x = 0; // xPos
  std::int16_t pos_y = 0; // yPos
  std::array<std::int16_t, 16>
      links{}; // Con1-16 (system resource ids, 0x80-based; -1/less marks an
               //   empty slot). A jump target is a links entry, converted to a
               //   scenario index as (value - 0x80); the travel slot lookup
               //   (FindLinkedTravelSlot) compares against this 0x80-based id.
  std::array<std::int16_t, 16> nav_defs{-1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1,
                                        -1}; // NavDef1-16 (stellar ids)
  // Dude1-8 / DudeProb1-8 (payload +0x44/+0x54). Ordinary 0x80..0x27f
  // entries are rebased into dude-class indexes. Negative -0x80..-0x17f
  // entries are moved into encounter_fleet_* below instead.
  std::array<std::int16_t, 8> dude_class_ids{-1, -1, -1, -1, -1, -1, -1, -1};
  std::array<std::uint16_t, 8> dude_class_weights{};
  std::array<std::int16_t, 8> dude_types{}; // DudeTypes (+0x6e, rebased -0x80;
                                            //  <0x80 / >0x47e -> -1)
  std::array<std::int16_t, 8> dude_prob{};  // % Prob (+0x7e, clamped 0..100)
  // AvgShips (payload +0x64): the per-system NPC ship population cap that the
  // spawn maintainers replenish the system's active ship count toward.
  std::int16_t avg_ships = 0;
  // Govt (payload +0x66; <0x80 or >0x17f -> -1, else rebased -0x80). The
  // owning faction; gates alliance/hostility and the system's HUD/map color.
  std::int16_t government_id = -1;
  std::int16_t message_id = -1; // Message (+0x68)
  // Asteroids count (payload +0x6a): how many asteroid/drift-debris records to
  // spawn for the system (EV Nova Bible: 0 = none, 1-16 = that many "asteroid"
  // drift-char records). The loader stores this payload word into
  // SystemDef.asteroid_count (+0x94; verified in the 0x004bd3c0 system section:
  // MOV WORD [g_system_defs+ebp+0x94], payload+0x6a). Asteroid_InitSystem
  // (0x004216B0) spawns this many Asteroid_Spawn records and pre-warms the
  // 16-slot pool; Asteroid_Spawn bails out when it is < 1. Clarified from a
  // previous misname `roaming_ship_count`: these are ASTEROID drift records,
  // not NPC ships.
  std::int16_t asteroid_count = 0;
  std::int16_t interference = 0; // Interference (+0x6c)
  // BkgndColor (s\xd8st +0x8e): per-system space background tint stored as
  // 24-bit 0xRRGGBB. Decoded to reproduce the original's runtime mapping (it
  // reads the resource bytes as a little-endian 32-bit and takes R=byte+0x90,
  // G=byte+0x8f, B=byte+0x8e), so `(c>>16),(c>>8),c` are exactly the colours
  // NovaRender_SetSystemSpaceBackgroundColor paints. Pure black (0) when unset.
  std::uint32_t bkgnd_color = 0;
  // Murk (s\xd8st +0x92): murkiness 0-100; a negative value hides the starfield
  // (SystemDef.murk at +0xbc < 0; Ghidra previously mislabeled this field
  // "alert_level"). Feeds the ambient-star size scale as well.
  std::int16_t murk = 0;
  // AstTypes (payload +0x94): flag bits determining which asteroid types appear
  // in the system (EV Nova Bible: bit0 = Small metal r\xf6id 128, bit1 = Medium
  // metal, etc.). Copied verbatim into SystemDef.ast_types at +0x1f4;
  // Asteroid_Spawn (0x00421830) tests it via
  // (1 << (wander_type & 0x1f)) & this mask to reject a random asteroid type
  // the system does not host, and gives up entirely when it is 0. These are
  // the r\xf6id (asteroid) types, not ship roles; renamed from a misname
  // `roaming_direction_bitmap`.
  std::uint16_t ast_types = 0;
  std::int16_t reinf_fleet = -1;   // ReinfFleet
  std::int16_t reinf_time = 0;     // ReinfTime
  std::int16_t reinf_interval = 0; // ReinfIntrval
  std::string visibility_expr;     // Visibility

  // ---- Derived random-encounter binding. The loader separates negative
  // fleet references from the payload's Dude1-8 table and stores their ids and
  // weights here. Their weight sum is the per-system encounter chance;
  // EncounterFleet_SelectRandomEncounterFleetDefWeighted (0x0046b6d0) chooses
  // among the eligible entries when that encounter gate fires.
  std::array<std::int16_t, 8> encounter_fleet_ids{
      -1, -1, -1, -1, -1, -1, -1, -1};
  std::array<std::uint16_t, 8> encounter_fleet_weights{};
  std::int16_t encounter_fleet_count = 0; // SystemDef +0x8a (bound def count)
  std::int16_t encounter_chance_percent = 0; // SystemDef +0x8c (spawn odds)

  // ---- Runtime discovery/visibility state (decoded with, not from, the
  // payload). Mirrors SystemDef is_visible / has_explored_flag / discovery
  // state, maintained by System_UpdateSystemAndStellarDisplayState and the
  // discovery flood (System_FloodDiscoverAdjacentSystems). These gate which
  // systems (and hence their stellars) the player may target. ----
  bool is_visible = false;
  bool has_explored_flag = false;
};

// Ghidra RandomEncounterFleetDef (g_random_encounter_fleet_defs, up to 0x100
// entries indexed by fleet id minus 0x80). One random-encounter "fleet"
// template: a lead ship plus up to four escort classes with per-escort
// population ranges, a system/government filter that gates where the fleet may
// spawn and an availability NCB expression, plus an arrival message id and a
// flags word. The original loads these from the fl\x91t resource family in
// Nova Data 1 inside NovaData_LoadScenarioResourceTables (Ghidra 0x004bd3c0) at
// a 0x124-byte stride, < 0x80 lead/government/escort ids being sentinel-invalid
// (-1) after the loader's rebasing.
//
// Payload layout (big-endian fl\x91t record, >= 0x132 bytes): lead_ship_class
// +0x00, escort_ship_class_ids[4] +0x02, escort_min_count[4] +0x0a,
// escort_max_count[4] +0x12 (this is why government_id is loaded from +0x1a),
// government_id +0x1a, spawn_system_filter +0x1c, availability_expr +0x1e,
// arrival_message_id +0x11e, carry_cargo_flags +0x120. The in-memory
// RandomEncounterFleetDef mirrors the same field ordering with the
// availability block at +0x20.
struct FleetDef {
  // Lead ship class id (0.. space, rebased from payload by -0x80); -1 means
  // "no fleet" (absent slot or a <0x80 lead) and suppresses spawning.
  std::int16_t lead_ship_class_id = -1;
  // Government id (0.. space, < 0x80 -> -1). Applied to every ship spawned
  // from this def.
  std::int16_t government_id = -1;
  // Escort ship class ids (0.. space, < 0x80 -> -1 = "no escort slot").
  std::array<std::int16_t, 4> escort_ship_class_ids{-1, -1, -1, -1};
  // Minimum / maximum number of each escort class to spawn.
  std::array<std::int16_t, 4> escort_min_count{};
  std::array<std::int16_t, 4> escort_max_count{};
  // Filter that restricts which systems the fleet may spawn in
  // (EncounterFleet_TrySpawnRandomEncounterFleet reads the payload +0x1c
  // value verbatim):
  //   -1 -> anywhere; 0x80..9999 -> a specific system id;
  //   10000..14999 -> a specific government id; 15000..19999 -> allied govt;
  //   20000..24999 -> a different govt; 25000..29999 -> hostile govt;
  //   values in 1..0x7f / 0x20..0x7f are not matched.
  std::int16_t spawn_system_filter = 0;
  // Arrival overlay-message resource id (payload +0x11e). Used only by the
  // ignore-ship-availability spawn flavor (Ghidra EncounterFleet_SpawnRandom-
  // EncounterFleet 0x004259b0) to flash an "arrival" banner.
  std::int16_t arrival_message_id = 0;
  // Availability NCB test expression (loads from payload +0x1e, the same
  // offset as the government id's gap; a < 0x80 lead keeps this empty).
  std::string availability_expr;
  // Flags word (payload +0x120). Bit 0x0001: spawned ships may carry a
  // randomly-seeded cargo bin (gate checks default_ai_behavior < 3).
  std::uint16_t flags = 0;

  [[nodiscard]] bool HasCargoFlag() const { return (flags & 0x0001U) != 0; }

  // Runtime availability (matches is_available_runtime at +0x122): derived by
  // evaluating availability_expr against the game state, not decoded from the
  // payload. Starts false so absent/untoggled defs never spawn.
  bool is_available_runtime = false;
};

// Ghidra DudeDef (g_dude_defs, up to 0x200 entries indexed by dude id minus
// 0x80). One "dude" template: an NPC drifter/pirate pilot category with an AI
// behavior, owning government, and up to 16 (ship-class, weight) pairs used to
// spawn a wandering ship (EncounterFleet_SpawnRandomSystemDudeShip / the
// roaming-dude spawners).
//
// The in-memory DudeDef is 74 bytes (
// ai_type +0x00, government_id +0x02, ship_types[16] +0x04, ship_probabilities
// [16] +0x24, booty_flags +0x44, hail_info_types +0x46) plus a present flag at
// +0x48. IMPORTANT: the d\x9fde payload layout does NOT mirror the struct.
// The original loader (NovaData_LoadScenarioResourceTables 0x004bd3c0, dude
// section at 0x004c2a2d..) re-arranges the payload's scattered fields into the
// struct, reads the 16-bit scalars big-endian, rebases government_id
// (-0x80 when in 0x80..0x180) and each ship type (-0x80 when in 0x80..0x380),
// and nulls out a ship type whose target ship-class def carries the
// "nonexistent" marker (ShipClassDef.tech_level == (short)0xd8f1 == -9999).
// The payload layout is:
//   +0x00 ai_type, +0x02 government_id, +0x04 booty_flags,
//   +0x06 hail_info_types, +0x08..+0x28 ship_types[16],
//   +0x28..+0x48 ship_probabilities[16]; bytes +0x48..+0x58 are padding
//   (the loader's EnsureBlockSize(payload, 0x58) requires an 88-byte record).
struct DudeDef {
  // AI behavior code (InherentAI) applied to ships spawned from this def, or
  // -1 to fall back to the ship class's default. Ghidra DudeDef +0x00.
  std::int16_t ai_type = -1;
  // Owning government (0.. space after the loader's -0x80 rebase). Applied to
  // every ship spawned from this def. Ghidra DudeDef +0x02.
  std::int16_t government_id = -1;
  // Candidate ship classes (0.. space, rebased -0x80 by the loader). A -1 slot
  // is an unused/removed entry; Dude_SelectShipTypeIndexFromDudeDef skips it.
  // Ghidra DudeDef +0x04.
  std::array<std::int16_t, 16> ship_types{
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  // Per-ship weight used by Dude_SelectShipTypeIndexFromDudeDef's weighted
  // pick. Ghidra DudeDef +0x24 (copied verbatim, no rebase).
  std::array<std::int16_t, 16> ship_probabilities{};
  // Booty / haul flags word. Ghidra DudeDef +0x44 (payload +0x04).
  std::uint16_t booty_flags = 0;
  // Hail/comm flags word. Ghidra DudeDef +0x46 (payload +0x06).
  std::uint16_t hail_info_types = 0;
  // set true by the loader for every present dude def (the struct's +0x48
  // flags word is set to 1; absent slots stay 0).
  bool present = false;
};

// One 0x1c-byte row of the shared asteroid-drift table the original exposes
// through two overlapping global labels: DAT_005912dc (a short[0xe] window)
// and DAT_005912f0 (a float[7] window), which are the same 16-row,
// 0x1c-byte-strided table. One row per resource id 0x80..0x8f (Metal/Ice /
// Dust/Crystal x Small/Medium/Big/Huge). Loaded by NovaData_LoadScenario-
// ResourceTables (0x004bd3c0 at 0x004c6207..) from the r\x9aid family into
// the g_asteroid_states type params.
//
// Only the two fields consumed by the spawn reads are confidently named:
//   +0x00 wander_table_value (read via DAT_005912dc[mode]), and
//   +0x14 wander_speed_multiplier (read via DAT_005912f0[mode]).
// The remaining fields are decoded with their loader-assigned offsets but
// semantically provisional (TODO(decomp): confirm against Asteroid_Spawn ring
// placement and the drift render). Payload layout (big-endian):
//   +0x00 value, +0x02 speed%, +0x04 field, +0x06 field(+0x02),
//   +0x08 field(+0x0c), +0x0a RGB bytes (565 -> 15-bit), +0x0e..+0x12 the
//   3-element direction sub-array, +0x14 field(+0x10), +0x16 lifetime.
struct AsteroidDef {
  // Lowest row field; stored into a spawned AsteroidState's
  // wander_table_value (+0x1c). Ghidra DAT_005912dc[mode] (+0x00).
  // Payload word[0x0]. Verbatim, no rebase.
  std::int16_t wander_table_value = 0;
  // Float wander-speed scale for this type; `(rand(0x29)+0x50) * this * 0.01`
  // yields a state's wander_speed (+0x18). Ghidra DAT_005912f0[mode]
  // (+0x14). Payload word[0x2] * 0.01 (0x64..0x12c => 0.5..3.0 shipped).
  float wander_speed_multiplier = 1.0F;
  // +0x02; loader validates payload word[0x6] >= 0. Provisional.
  std::int16_t field_0x02 = 0;
  // +0x04; loader accepts [-6,6] or [0x3e8,0x468). Provisional.
  std::int16_t field_0x04 = 0;
  // +0x0c; loader validates payload word[0x8] >= 0. Provisional.
  std::int16_t field_0x0c = 0;
  // +0x10; loader accepts [0,0x40) or [0x3e8,0x428). Provisional (the
  // ship/dude-id style window hints at a cross-reference).
  std::int16_t field_0x10 = 0;
  // +0x0e; payload word[0x16]. Scales with size tier in the shipped data
  // (Metal Small 150 / Medium 300 / Big 600 / Huge 1200), consistent with a
  // wander lifetime/count. PROVISIONAL: Asteroid_SpawnRecord reads the
  // per-type lifetime from the sprite descriptor (+0x54), not this field; the
  // drift layer (step 3/4) is expected to connect the two. Kept decoded so
  // the spawn path has a datapoint.
  std::int16_t lifetime = 0;
  // 3-element direction sub-array at +0x06/+0x08/+0x0a (payload word[0xe+i*2],
  // loader rebases 0x80..0x90 by -0x80, else requires <0x10). The third slot
  // (+0x0a) converges with the standalone +0x0a field.
  std::array<std::int16_t, 3> directions{0, 0, 0};
  // Packed 15-bit tint for the drift sprite, computed from the payload's
  // 3 RGB565 bytes via the loader's 565->555 downsample (red=byte[12]>>3,
  // green=byte[11]>>3, blue=byte[10]>>3). +0x18 (DAT_005912f4). Not yet
  // consumed (needs the drift-sprite render).
  std::uint32_t color = 0;
  // Set true for every present asteroid-type row (resource id 0x80..0x8f);
  // absent ids stay default.
  bool present = false;

  // The original reuses this 0x1c row as an impact-package definition in
  // Weapon_SpawnWeaponImpactEffectPackage (0x00462550). These aliases expose
  // that second interpretation without duplicating or reshaping the loaded
  // table. The package index is the row index 0..15, not a resource id.
  [[nodiscard]] std::int16_t ImpactFragmentCount() const { return field_0x02; }

  [[nodiscard]] std::int16_t ImpactFragmentType() const { return field_0x04; }

  [[nodiscard]] std::int16_t ImpactParticleCount() const { return field_0x0c; }

  [[nodiscard]] std::int16_t ImpactAreaEffectId() const { return field_0x10; }

  [[nodiscard]] std::int16_t ImpactSecondaryCount() const { return lifetime; }

  [[nodiscard]] std::int16_t ImpactSecondaryEffectId(std::size_t index) const {
    return index < directions.size() ? directions[index] : -1;
  }
};

// Ghidra ImpactEffectDef (g_impact_effect_defs, 0x005912c0), runtime stride
// 0x08. The source record stores an integer frame-rate scale, an impact sound
// slot, and the sprite-set index. Runtime animation advances by
// frame_rate_scale * elapsed_ms.
struct ImpactEffect {
  float frame_rate_scale = 1.0F;
  std::int16_t impact_sound_slot = -1;
  std::int16_t sprite_set_id = 0;
};

// Owns the parsed scenario tables indexed by (resource id - 0x80), mirroring
// the original global arrays (g_ship_class_defs etc.). Filled by
// LoadFromArchives(). Entries are created for every id in the family's valid
// range so callers can index directly; missing/empty resources decode to
// defaults (the original zero-fills those slots).
struct ScenarioData {
  std::vector<ShipClass> ships;        // indexed by ship_id - 0x80
  std::vector<Outfit> outfits;         // indexed by outfit_id - 0x80
  std::vector<Weapon> weapons;         // indexed by weapon_id - 0x80
  std::vector<Stellar> stellars;       // indexed by stellar_id - 0x80
  std::vector<System> systems;         // indexed by system_id - 0x80
  std::vector<Government> governments; // indexed by government_id - 0x80
  std::vector<FleetDef> fleets;        // indexed by fleet_id - 0x80
  std::vector<DudeDef> dudes;          // indexed by dude_id - 0x80
  std::vector<MissionDef> missions;    // indexed by mission id - 0x80
  // Asteroid/drift class table (r\x9aid family, one row per resource id
  // 0x80..0x8f). Ghidra g_asteroid_states's per-type params read via
  // the DAT_005912dc / DAT_005912f0 pair.
  std::vector<AsteroidDef> asteroid_defs; // indexed by type id - 0x80
  // Impact/explosion definitions are indexed directly by effect id 0..63;
  // the source resources themselves use ids 0x80..0xbf.
  std::array<ImpactEffect, 64> impact_effects{};

  // gh.id 0x80.. lookup for government/faction data.
  [[nodiscard]] const Government *Government(std::int16_t resource_id) const;

  // gh.id 0x80.. convention: returns the entry for the given resource id, or
  // nullptr when it is outside the loaded range.
  [[nodiscard]] const ShipClass *Ship(std::int16_t resource_id) const;
  [[nodiscard]] const Outfit *Outfit(std::int16_t resource_id) const;
  [[nodiscard]] const Weapon *Weapon(std::int16_t resource_id) const;
  [[nodiscard]] const Stellar *Stellar(std::int16_t resource_id) const;
  [[nodiscard]] const System *System(std::int16_t resource_id) const;
  // gh.id 0x80.. lookup for a random-encounter fleet template, or nullptr when
  // outside the loaded range.
  [[nodiscard]] const FleetDef *Fleet(std::int16_t resource_id) const;
  // gh.id 0x80.. lookup for a dude template (g_dude_defs), or nullptr when
  // outside the loaded range.
  [[nodiscard]] const DudeDef *Dude(std::int16_t resource_id) const;
  // gh.id 0x80.. lookup for mission definitions, or nullptr when outside the
  // loaded 1000-entry mission table.
  [[nodiscard]] const MissionDef *Mission(std::int16_t resource_id) const;
  // gh.id 0x80.. lookup for an asteroid-type row, or nullptr when outside the
  // loaded range.
  [[nodiscard]] const AsteroidDef *AsteroidType(std::int16_t resource_id) const;
  // Impact-package rows reuse the decoded r.x9aid table. The original indexes
  // these rows directly from a ShotState package slot (0..15).
  [[nodiscard]] const AsteroidDef *
  ImpactPackageAt(std::int16_t package_id) const;
  [[nodiscard]] const ImpactEffect *
  ImpactEffectAt(std::int16_t effect_id) const;

  // Ghidra NovaData_LoadScenarioResourceTables (0x004bd3c0). Walks each
  // resource family by id 0x80.. max and decodes it into the matching table.
  // Returns true when at least the ship and weapon tables have data.
  [[nodiscard]] bool LoadFromArchives();
};
} // namespace game
