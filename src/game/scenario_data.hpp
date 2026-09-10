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
constexpr std::uint32_t kShipResourceType = 0x73689570;    // sh\x95p
constexpr std::uint32_t kOutfitResourceType = 0x6f9f7466;  // o\x9ftf
constexpr std::uint32_t kWeaponResourceType = 0x77916170;  // w\x91ap
constexpr std::uint32_t kStellarResourceType = 0x73709a62; // sp\x9ab
constexpr std::uint32_t kSystemResourceType = 0x73d87374;  // s\xd8st
// n\x91bu "nebula" resources (the star-map background regions; loader call
// FUN_004ce250(0x6e916275, id) in NovaData_LoadScenarioResourceTables
// 0x004bd3c0, ids 0x80..0x9f).
constexpr std::uint32_t kNebulaResourceType = 0x6e916275;     // n\x91bu
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
// p\x91rs (0x70917273) — AI "personality" (mission-ship) definitions. One
// record per named AI person; loaded by NovaData_LoadScenarioResourceTables
// (0x004bd3c0, personality pass around 0x004c4400) into g_pers_defs,
// 0x400 slots indexed by resource id minus 0x80. See PersDef.
constexpr std::uint32_t kPersResourceType = 0x70917273;
// Impact/explosion definition family read by
// NovaData_LoadScenarioResourceTables (0x004bd3c0). The binary FourCC is shown
// as 0x629a9a6d by Ghidra; each record is 0x18 bytes in the source resource and
// contributes the three runtime fields below.
constexpr std::uint32_t kImpactEffectResourceType = 0x629a9a6d;
// crön (0x63729a6e) — time-dependent event definitions, driven once per
// game-day by Mission_TickDailyCronEvents (0x00439500) after the calendar
// advance. The original loads 0x200 slots (resource id 0x80 + i) into the
// g_cron_event_states blocks (stride 0x350) in the cron pass of
// NovaData_LoadScenarioResourceTables (0x004bd3c0). See CronEventDef.
constexpr std::uint32_t kCronResourceType = 0x63729a6e;
// öops (0x9a6f7073) — planetary disaster / commodity price-shock definitions.
// The original loads up to 0x100 slots (resource id 0x80 + i, g_disaster_defs,
// stride 0x210) in the disaster pass of NovaData_LoadScenarioResourceTables
// (0x004bd3c0); 19 are present in the shipped data (ids 0x80..0x92, Nova Data
// 2). System_UpdateDisasterStates (0x00424f90) rolls them once per game-day
// and NovaUi_HandleTravelDestinationInteractionLoop (0x0048c730) applies the
// price delta to the commodity exchange. See DisasterDef.
constexpr std::uint32_t kDisasterResourceType = 0x9a6f7073;
// j\x9fnk (0x6a9f6e6b) — specialized trade commodities (Bible "j\xf6nk
// resource"). The original loads 0x80 slots (resource id 0x80 + i) into
// g_junk_defs (stride 0x526) in the junk pass of
// NovaData_LoadScenarioResourceTables (0x004bd3c0); the commodity exchange
// (NovaUi_RunTradeCenterWindow 0x0048c730) presents up to two of them as
// rows 6/7. See JunkDef.
constexpr std::uint32_t kJunkResourceType = 0x6a9f6e6b;
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
  std::int16_t link_system_filter = -1; // MisnDef +0x00
  // Payload +0x04: Bible AvailLoc — where the mission is offered. The loader
  // clamps negative values to 0 (mission computer); lane 0 = offered at the
  // mission computer, lane 1 = bar/other locations, 2 = offered from a ship
  // (interaction context only).
  std::int16_t avail_location = 0; // +0x04 (Ghidra: return_stellar_id)
  // Payload +0x06: Bible AvailRecord — legal-record gate (0 ignored, positive
  // = record must be >= value, negative = record must be <= value,
  // -32000/-32001 = stellar domination arms).
  std::int16_t avail_record = 0; // +0x06 (Ghidra: special_ship_goal)
  // Payload +0x08: Bible AvailRating — combat-rating gate (-1/0 ignored).
  std::int16_t avail_rating = -1; // +0x08 (Ghidra: special_ship_behavior)
  // Payload +0x0a: Bible AvailRandom — percent chance (>=100 always; <1 never).
  std::int16_t avail_random = 0; // +0x0a (Ghidra: special_ship_start)
  std::int16_t travel_stellar_locator = -1; // payload +0x0c (TravelStel)
  std::int16_t return_stellar_locator = -1; // payload +0x0e (ReturnStel)
  std::int16_t cargo_qty_tons = 0;          // resource +0x12 (Bible CargoQty)
  std::int16_t cargo_type_resource = -1;    // resource +0x10 (Bible CargoType:
                                            // -1 none, 0-255, 1000 random)
  // Payload +0x5a: Bible "Ship" restriction (0x80..0x37f player class must
  // match; 0x468..0x767 must not match; 0x850..0x94f inherent-govt match;
  // 0xc38..0xd37 inherent-govt mismatch; other values ignored).
  std::int16_t ship_restriction_filter =
      -1;                              // +0x5a (Ghidra: on_start_condition)
  std::int16_t aux_ship_dude = -1;     // resource +0x24
  std::int16_t special_ship_dude = -1; // resource +0x24; active +0x08
  std::int16_t aux_ship_system = -1;   // resource +0x22
  // Payload +0x14/+0x16/+0x18: Bible PickupMode (-1 ignored, 0 at mission
  // start, 1 at TravelStel, 2 when boarding), DropOffMode (0 at TravelStel,
  // 1 at ReturnStel) and ScanMask (govts whose scanners flag the cargo).
  std::int16_t pickup_mode = -1;            // +0x14 (active +0x16)
  std::int16_t drop_off_mode = -1;          // +0x16 (active +0x18)
  std::int16_t scan_mask = 0;               // +0x18 (active +0x1a)
  std::int16_t time_limit_days = 0;         // payload +0x40 (active +0x45)
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
  // Bible CanAbort (0 = player cannot abort, 1 = can); Ghidra MisnActive
  // +0x32 can_abort.
  bool can_abort = false;                // +0x42
  std::int16_t initial_briefing_id = -1; // +0x34
  // Desc-resource ids from payload +0x34..+0x3e (BriefText, QuickBrief,
  // LoadCargText, DumpCargoText, CompText, FailText), plus ShipDoneText
  // (+0x44) and the +0x58 id copied to active slot +0x41 (provisional).
  std::array<std::int16_t, 6> text_description_ids{}; // +0x34..+0x3e
  std::int16_t ship_done_text_id = -1;                // +0x44
  std::int16_t slot_aux_text_id = -1;                 // +0x58 (provisional)
  // Mission availability expression from the resource string block (+0x5c).
  // The original caches its result at MisnDef +0x16.
  std::string availability_expr;
  bool is_available_runtime = false;
  // Payload +0x656/+0x65a: 64-bit Require mask, checked against the player's
  // aggregated Contribute mask by Outfit_EvaluateRequireMask (0x0046cd80)
  // in the offering eligibility chain.
  std::uint32_t require_mask_lo = 0; // +0x656
  std::uint32_t require_mask_hi = 0; // +0x65a
  // MisnDef +0x128, loaded from mïsn payload +0x7a0. Lower values sort first.
  std::int16_t list_priority = 0;
  std::string display_name;
  std::array<std::byte, 0x7b2> raw_payload{};
};

// p\x91rs personality definition (g_pers_defs slot, stride 0x794).
// Field offsets verified against the loader pass in
// NovaData_LoadScenarioResourceTables (0x004bd3c0); Bible names from the
// "The p\x91rs resource" section.
struct PersDef {
  // Presence/runtime gates. +0x620 and +0x623 are both set by the loader for
  // every present resource (+0x623 after the record is fully decoded); +0x622
  // is the cached ActiveOn evaluation, refreshed from availability_expression
  // by the mission-list evaluator (0x0044830c block of Mission_Evaluate-
  // MissionLists 0x0043cf00) each time it runs.
  bool present = false;              // +0x620
  bool loaded_latch = false;         // +0x623
  bool is_available_runtime = false; // +0x622

  std::int16_t spawn_system_filter = -1; // +0x00 LinkSyst (-1 any; +0x80 sys;
                                         // 10000/15000/20000/25000 govt codes)
  std::int16_t government_id = -1;       // +0x02 Govt (0x80.. rebased)
  std::int16_t ai_behavior_code = -1;    // +0x04 AI Type (raw; the spawner's
                                         // eligibility gate requires > 0)
  std::int16_t aggression_level = 0;     // +0x06 Aggress
  std::int16_t cowardice_pct = 0;        // +0x08 Coward (flee at shield %)
  std::int16_t ship_class_id = -1;       // +0x0a ShipType (rebased; out of
                                         // range -> 0, nonexistent -> -1)
  // +0x0c..+0x22: four weapon triples (Bible documents eight slots; the
  // binary reads and applies only four). The loader spreads them into
  // 0x100-entry tables indexed by weapon id minus 0x80: +0x18 WeapCount,
  // +0x418 AmmoLoad. Applied as deltas on top of the class default loadout.
  std::array<std::int16_t, 0x100> weapon_count_delta{};     // +0x18
  std::array<std::int16_t, 0x100> weapon_ammo_load_delta{}; // +0x418
  std::int32_t booty_base_credits =
      0;                             // +0x618 Credits (spawner applies +/-25%)
  float shield_armor_scale = 0.0F;   // +0x61c ShieldMod percent / 100.0
                                     // (<= 0 skips the shield rescale)
  std::int16_t hail_pict_id = -1;    // +0x12 HailPict (0xffff when < 0x80)
  std::int16_t comm_quote_id = 0;    // +0x0c CommQuote (STR# 7100 index)
  std::int16_t hail_quote_id = 0;    // +0x0e HailQuote (STR# 7101 index)
  std::int16_t link_mission_id = -1; // +0x10 LinkMission (rebased -0x80)
  std::int16_t flags_primary = 0;    // +0x14 Flags (grudge/escape pod/
                                     // quote gates/LinkMission handoff)
  std::int16_t flags_secondary = 0;  // +0x16 Flags2 (0x0001 starts no fuel)

  std::string availability_expression; // +0x644 ActiveOn (payload +0x34)
  std::int16_t grant_item_class = -1;  // +0x784 GrantClass (0xffff when < 1)
  std::int16_t grant_probability = 0;  // +0x786 (payload +0x138, clamped
                                       // 0..100 — probability-like)
  std::int16_t grant_count = 0;        // +0x788 (payload +0x136)
  // Boarded-grant/outfit name ids assigned by the loader post-pass: each def
  // starts with its own zero-based index; later defs with an identical
  // display name adopt the first def's id (same-name personalities share one
  // display-name row).
  std::int16_t display_name_string_id = -1; // +0x78a
  std::uint8_t color_r5 = 0;                // +0x78c (payload +0x17a >> 19)
  std::uint8_t color_g5 = 0;                // +0x78e (>> 5)
  std::uint8_t color_b5 = 0;                // +0x790 (>> 3)

  // +0x624: resource record name with any ';'-subtitle stripped (loader
  // copies it bounded to 0x1e bytes and uses it for the same-name dedup).
  std::string display_name;
  // +0x743: payload +0x13a free-text name (converted to a Pascal string by
  // the loader; carries the special ship's override name when present).
  std::string special_ship_name;
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
  // Ghidra ShipClassDef +0xac <- shp payload +0x6e6 C string (Bible
  // "Subtitle: The subtitle to show on the target display for this ship
  // type", e.g. "Class A"). Drawn under the target-panel name.
  std::string subtitle;

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
  std::int16_t crew = 0; // Crew (Ghidra ShipClassDef.capture_power +0x9f2:
                         // ships with 0 crew cannot be boarded nor capture;
                         // the AI boarding selector reads it as capture_power)
  std::int16_t strength = 0; // Strength

  // Bible InherentGovt, normalized by the loader (0x004c149b..) into two
  // zero-based g_government_defs indexes (-1 = none). Use GovernmentByIndex.
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
  // Ghidra ShipClassDef +0x34 (shïp payload +0x5e, written by loader
  // 0x004bd3c0). Bible FuelRegen: "frames per 1 unit of fuel generated",
  // consumed by Ship_ComputeShipFuelRechargeRate 0x00463b30; the player only
  // benefits when class capability flag 0x0008 is set.
  std::int16_t fuel_regen = 0;
  // Ghidra ShipClassDef +0x9FA (ionization capacity). NPC effective-stat
  // helpers divide ShipState.ionization_points by this value; zero means
  // no ionization bar. The scenario loader reads the packed resource field at
  // payload +0x36C.
  std::int16_t ionization_capacity = 0;
  // Ghidra ShipClassDef +0xA04 (shïp payload +0x36E, Bible KeyCarried): the
  // zero-based "key carried ship type" class id. The loader (0x004bd3c0,
  // disasm 0x004c1967..0x004c19be) rebases resource values >= 0x80 by -0x80
  // and stores -1 for anything below. Weapon_HasLoadedLaunchBayAmmo
  // (0x00464670) matches a loaded mode-99 bay weapon whose ammo_type - 0x80
  // equals this id; -1 disables the launch-bay fire gate.
  std::int16_t key_carried_ship_class = -1;
  // Ghidra ShipClassDef +0x9fe <- sh\x8an AnimDelay +0x30 (loader
  // ShipClass_LoadShipClassVisualAndLaunchData 0x004b4ee0). Combat/sprite
  // animation cadence seed drawn by the ship spawn paths
  // (Ship_AllocateShipSlotInSystem 0x004254b0,
  // Weapon_SpawnShipFromCarrierBayWeapon 0x0041e640).
  std::int16_t combat_state_init_range = 0;
  // Ghidra ShipClassDef +0xa08: zero-based id of the class whose base sprite
  // this class cloned (0x004b4ee0 clone arm: the first EARLIER class in load
  // order whose sh\x8an BaseImageID matches; -1 when the class builds its own
  // sprite). Read by Ship_LaunchCarriedShipFromBay 0x00415ea0's bay-weapon
  // fallback, mapping a fighter variant back to its carrier's bay weapon.
  std::int16_t escort_type = -1;
  // Ghidra ShipClassDef +0xa06 (sh\x8an +0x34 FramesPer, 36 when 0): the
  // rotation-grid frame count. The turret muzzle bearing is the displayed
  // rotation frame scaled back to degrees (Weapon_SelectTurretQuadrant
  // 0x0046c320 reads it as (sprite_frame %% frames) * 360 / frames).
  std::int16_t frames_per_rotation = 36;
  // Weapon-exit (muzzle) geometry, decoded from the sh\x8an by the scenario
  // loader (mirrors the ShipClass_LoadShipClassVisualAndLaunchData 0x004b4ee0
  // copy into ShipClassDef +0xa42..+0xab0). EVN ships have up to four turret
  // groups of four quadrant barrels; Weapon_ApplyTurretSpreadVelocity
  // (0x0046c5c0) offsets a projectile to the barrel indexed
  // [turret_group][quadrant] and Weapon_SelectTurretQuadrant (0x0046c320)
  // cycles the quadrant per ship.
  bool muzzle_ready = false;
  std::array<std::array<std::int16_t, 4>, 4> muzzle_lateral{};
  std::array<std::array<std::int16_t, 4>, 4> muzzle_forward{};
  std::array<std::array<std::int16_t, 4>, 4> muzzle_drop{};
  // Weapon-exit compress scales: near pair applied when the barrel
  // displacement lands above the hull centre (y < 0), far pair otherwise
  // (Weapon_ApplyTurretSpreadVelocity 0x0046c5c0).
  float muzzle_scale_near_x = 1.0F;
  float muzzle_scale_near_y = 1.0F;
  float muzzle_scale_far_x = 1.0F;
  float muzzle_scale_far_y = 1.0F;
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
  // Availability percent pair (payload +0x388/+0x38a, clamped 0..100 by the
  // loader). buy_random is the static "readily available" percent the
  // shipyard buy list requires the per-day limit roll (GameState.
  // ship_class_limit_rolls, rerolled by 0x00466cb0's tail) to meet;
  // hire_random is the threshold half, consumed by the hire lane
  // (TODO(decomp): lane not modelled).
  std::int16_t buy_random = 0;
  std::int16_t hire_random = 0;
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

  std::int16_t item_class = 0; // ItemClass
  // Bible-style "in stock" percent (OutfitDef +0x1c from o\9ftf payload +0x3f0,
  // clamped 0..100 by the loader): the outfitter lists an unowned outfit only
  // while the per-day stock roll (GameState.outfit_stock_rolls) is <= this.
  std::int16_t stock_threshold = 0;
  std::int16_t sprite_id = 0; // Graphic (p\x9ari sprite id)
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
  // BeamLength + 32 as the reach envelope only for beam modes 0 and 3
  // (Weapon_IsShipWithinWeaponRangeOfTarget 0x00411600; mode 10 takes the
  // range_scalar path there, disasm-verified 2026).
  std::int16_t beam_length_px = 0; // resource +0x30 / Ghidra +0x70
  // Ghidra WeaponDef.homing_strength_or_turn_rate (+0x72, loaded from resource
  // +0x32): triple-purpose. For beam weapons (modes 0/3/10) it is the Bible
  // BeamWidth: the core radius in pixels (0 = no center beam, corona only, and
  // the SWBeams renderer forces it to 1 for lightning beams). For an
  // animation-frame weapon set it is the shot animation frame-dwell time in ms
  // (Shot_HandleShot accumulates it into ShotState.anim_elapsed and steps
  // frame_cycle_index each time the dwell is crossed); for guided weapons it is
  // the turn rate. The Light Blaster's payload keeps it at 0, which makes even
  // an animated frame-stepper advance every frame.
  std::int16_t shot_anim_frame_dwell = 0;

  // Beam render fields (SWBeams renderer, WeaponDef +0x74/+0x76/+0x78/
  // +0x84/+0x88; verified against the loader copy at 0x004bd3c0).
  // Bible Falloff (resource +0x34): corona falloff rate, 2..16 in practice.
  // The loader defaults it to 0x10 when a beam has no value and zeroes it for
  // lightning beams (which have no corona). Larger = corona falls off faster;
  // beam lifetime extends by 16 - falloff decay ticks when fuse_ticks > 0.
  std::int16_t beam_falloff = 0;
  // Bible LiDensity (resource +0x6e): 0 = normal straight beam; > 0 = lightning
  // beam with this many zig-zags per 100 px (loader clamps to >= 2).
  std::int16_t beam_lightning_density = 0;
  // Bible LiAmplitude (resource +0x70): lightning zig-zag amplitude in pixels
  // (loader clamps to >= 1 when lightning).
  std::int16_t beam_lightning_amplitude = 0;
  // Bible BeamColor (resource +0x36) / CoronaColor (+0x3a), packed 0x00RRGGBB.
  // The original converts these to RGB555/palette entries per surface depth for
  // its 15-bit beam blending; this port keeps them 24-bit (SDL blends in 8-bit
  // channels instead).
  std::uint32_t beam_core_color = 0;
  std::uint32_t beam_corona_color = 0;
  // Ghidra WeaponDef.guided_turn_rate (+0x58, float): guided missile turn rate
  // in game degrees/tick. Loaded from payload +0x6a (Bible "GuidedTurn") * 0.1
  // (k_guided_turn_scale_f64 @0x00575e58). Shot_UpdateShotGuidance integrates
  // the shot heading by this each frame; a defeated jamming lock zeroes it or
  // (Seeker 0x0010) negates it so the missile flies away.
  float guided_turn_rate = 0.0F;
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
  // Bible MaxAmmo (resource +0x6c -> WeaponDef +0x1e, loader 0x004bd3c0):
  // max ammo per weapon instance; 0/-1 defers to the outfit Max field. Read
  // by ShipClass_HasPlayerBayCapacityFor (0x004694a0) as a fighter bay's
  // per-mounted-unit capacity.
  std::int16_t max_ammo = -1;

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
  std::uint16_t availability_flags = 0; // availability_flags (+0x20)
  // Bible "Tribute": the stellar's daily payout when dominated (StellarDef
  // +0x46a). Payload +0x0a; -1/0 falls back to 1000 x TechLevel in the
  // loader. Collected once per game-day by the income pass (0x00423540) for
  // available stellars carrying the +0x46 marker.
  std::int16_t tribute = 0;
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
  // CustSndID (+0x1a) is an ambient sound for ordinary stellars. Only the
  // 0x1000 hypergate and 0x2000 wormhole lanes reinterpret it as an emergence
  // angle; ordinary stellars deliberately leave this empty.
  std::optional<std::int16_t> emergence_angle_deg;

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

  // service_cost (payload +0x234; StellarDef +0x38), a destination-service
  // value whose collection path is not yet reconstructed.
  std::int32_t service_cost = 0;
  // Payload +0x238 (StellarDef +0x46c): nonzero marks the stellar as a
  // "gravity shear" navigation hazard on the starmap's Navigation Hazards
  // line (NovaUi_RedrawStarmapWindow 0x004a62f0 tests this field). The
  // Bible-name for this payload word is unverified -- TODO(decomp).
  std::int16_t gravity_shear = 0;
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
  // fields are modelled so the data is present.
  int present_ship_count = 0; // StellarDef +0x50
  int max_ship_count = 0;     // StellarDef +0x4e (garrison size; >0x3e9/0x2711
                              //  rescale branches)
  // Daily-schedule state decoded from the sp\9bb tail (loader 0x004bd3c0).
  // schedule_days (+0x47a) is the seed from payload +0x242: while the stellar
  // is sprite-active and the seed is >= 0, the daily world update decrements
  // the countdown and, when it expires, runs the payload +0x345 set-string
  // through the reaction-script executor and latches the countdown off. A
  // negative seed pins the countdown at 1 (never fires). The original counts
  // down in StellarDef +0x47c, the same field the targeting code uses as the
  // engagement-access counter -- one shared runtime word; the port keeps both
  // views on it (engage_access is bumped by targeting, the daily tick drives
  // it as the countdown).
  std::int16_t schedule_days = 0; // payload +0x242 (StellarDef +0x47a)
  std::string schedule_script;    // payload +0x345 (StellarDef +0x365)
  // Day counter (StellarDef +0x2a): bumped once per game-day by the tribute
  // income pass (0x00423540) while the stellar pays out. No other consumer
  // decoded yet.
  std::int16_t held_days = 0;
  std::uint8_t field_0x47 = 0;
  // Runtime destruction latch used by the Y/U mission-script operators.
  // The original stores this across several unnamed StellarDef fields; this
  // explicit projection keeps the gameplay state testable.
  bool is_destroyed = false;
};

// Ghidra CronEventDef (the defined payload half of a g_cron_event_states
// block, 0x350 stride, 0x200 slots indexed by crön resource id minus 0x80).
// A crön defines an invisible time-dependent event: between FirstDay/Month/
// Year and LastDay/Month/Year it rolls trigger_odds once per game-day and,
// when the Require mask and EnableOn expression hold, becomes active for
// duration days (with pre/post holdoff delays), running its OnStart/OnEnd
// control-bit set strings via the reaction-script executor. While active (and
// past the pre-holdoff) its Contribute bits join the player's aggregate
// Contribute mask. Payload offsets from the loader's cron pass
// (NovaData_LoadScenarioResourceTables 0x004bd3c0, block size 0x336).
struct CronEventDef {
  bool present = false; // loader: resource exists (block +0x01)

  std::int16_t first_day = -1;   // payload +0x00
  std::int16_t first_month = -1; // payload +0x02
  std::int16_t first_year = -1;  // payload +0x04
  std::int16_t last_day = -1;    // payload +0x06
  std::int16_t last_month = -1;  // payload +0x08
  std::int16_t last_year = -1;   // payload +0x0a
  // Bible "Random": percent chance per eligible day. A block whose duration
  // is -1 (absent slot sentinel) is never ticked.
  std::int16_t trigger_odds = -1; // payload +0x0c
  std::int16_t duration = -1;     // payload +0x0e
  std::int16_t pre_holdoff = 0;   // payload +0x10
  std::int16_t post_holdoff = 0;  // payload +0x12
  // 0x0001 iterative OnStart, 0x0002 iterative OnEnd (loop while Require and
  // EnableOn hold, bail-out after 0x2711 iterations).
  std::uint16_t flags = 0; // payload +0x16 (block +0x3c)
  std::string enable_on;   // payload +0x18 (block +0x50)
  std::string on_start;    // payload +0x117 (block +0x14f)
  std::string on_end;      // payload +0x216 (block +0x24e)
  // Contribute joins the player's aggregate mask while active; Require gates
  // activation. (Loader: block +0x40/+0x44 <- payload +0x316/+0x31a,
  // block +0x48/+0x4c <- payload +0x31e/+0x322.)
  std::uint32_t contribute_lo = 0;
  std::uint32_t contribute_hi = 0;
  std::uint32_t require_lo = 0;
  std::uint32_t require_hi = 0;
  // News overrides while active: on stellars allied with news_govts[i] a
  // string is drawn from STR# govt_news_strs[i]. Loader: < 0x80 -> -1 for the
  // govts; < -1 -> -1 for the STR ids.
  std::array<std::int16_t, 4> news_govts{-1, -1, -1, -1};     // payload +0x326
  std::array<std::int16_t, 4> govt_news_strs{-1, -1, -1, -1}; // payload +0x32e
  // Independent news (Bible IndNewsStr): when no allied NewsGovt matches, a
  // string is drawn from STR# independent_news_str. The loader reads it at
  // payload +0x14 -- right after PostHoldoff, NOT after GovtNewsStr as the
  // Bible's field list implies -- and copies it to cron block +0x3a. <= 0 means
  // none.
  std::int16_t independent_news_str = -1; // payload +0x14 -> block +0x3a
};

// Ghidra DisasterDef (g_disaster_defs, 0x100 slots indexed by resource id
// minus 0x80, stride 0x210). An öops record describes a temporary commodity
// price shock at a single stellar (the Bible's "disaster"): while the roll is
// active, the commodity exchange adds price_delta to that commodity's price.
// The static fields below are the resource payload (the Bible's Stellar /
// Commodity / PriceDelta / Duration / Freq / ActivateOn fields), the runtime
// trio is state the original keeps in the same table and persists in the .plt
// block2 (+0x3088/+0x3288).
struct DisasterDef {
  bool present = false; // loader: resource exists (g_disaster_defs +0x20e)

  // Bible "Stellar": 0x80-based stellar resource id this disaster is bound
  // to, or -1 for "any" (a random available, non-travel-flagged stellar is
  // chosen at activation). Loader stores payload word[0] verbatim at +0x00
  // (absent slots hold the 0x8001 sentinel).
  std::int16_t target_stellar = -1;
  // Bible "Commodity": 0 = food, 1 = industrial, ... Loader stores payload
  // word[1] at +0x04, clamping > 5 to 5 and < 0 to -1 (no commodity).
  std::int16_t commodity = -1;
  // Bible "PriceDelta": signed amount added to the commodity price while
  // active (negative lowers it). Loader payload word[2] -> +0x06.
  std::int16_t price_delta = 0;
  // Bible "Duration": days the disaster lasts. Loader payload word[3] ->
  // +0x08.
  std::int16_t duration_days = -1;
  // Bible "Freq": percent chance per eligible day to start. Loader payload
  // word[4] -> +0x0a.
  std::int16_t start_chance_percent = -1;
  // Bible "ActivateOn": control-bit test expression; blank means always
  // eligible. Loader C string at payload +0x0a -> +0x0e.
  std::string activation_expression;
  // Record name with the ';'-subtitle stripped (ResourceData_ReadEntryMetadata
  // + NameString_StripSubtitleSuffix), used by the travel-news report.
  std::string display_name;

  // ---- Runtime state (persisted in the pilot file, not in the resource) ----
  // Active stellar (0-based g_stellar_defs index) or -1 when idle
  // (+0x02 active_system_id_runtime).
  std::int16_t active_stellar = -1;
  // Remaining active days; -1 when idle (+0x0c days_remaining_runtime).
  std::int16_t days_remaining = -1;
  // +0x20d started_once_runtime. Only ever cleared by the daily update in the
  // shipped code, so this stays false and the negative-duration repeat arm is
  // dead; kept for parity and pilot-load.
  bool started_once = false;
};

// Ghidra g_junk_defs (0x005914bc, 0x80 slots indexed by resource id minus
// 0x80, stride 0x526). A j\x9fnk record is a specialized trade commodity (the
// Bible "j\xf6nk resource") that only appears at an explicit list of stellars.
// The commodity exchange (NovaUi_RunTradeCenterWindow 0x0048c730) shows up to
// two junk rows: the first record whose BoughtAt list contains the landed
// stellar with BuyOn passing (row 6), and the first whose SoldAt list contains
// it with SellOn passing (row 7). The runtime count is persisted at +0x22.
struct JunkDef {
  // Loader: resource exists (g_junk_defs +0x20 base_price stays valid).
  bool present = false;

  // Bible "SoldAt1-8": stellar resource ids where the commodity is sold,
  // rebased to 0-based indices (payload +0x00..+0x0e; <0x80 -> -1).
  std::array<std::int16_t, 8> sold_at{-1, -1, -1, -1, -1, -1, -1, -1};
  // Bible "BoughtAt1-8": stellar resource ids where it is purchased (payload
  // +0x10..+0x1e; <0x80 -> -1).
  std::array<std::int16_t, 8> bought_at{-1, -1, -1, -1, -1, -1, -1, -1};
  // Bible "BasePrice" (payload +0x20, g_junk_defs +0x20). Negative clamped to
  // 0 by the loader.
  std::int16_t base_price = 0;
  // Bible "Flags" (payload +0x22, g_junk_defs +0x24): 0x0001 tribbles,
  // 0x0002 perishable.
  std::uint16_t flags = 0;
  // Bible "ScanMask" (payload +0x24, g_junk_defs +0x26): a government whose
  // govt ScanMask ANDs this nonzero is hostile to the cargo.
  std::uint16_t scan_mask = 0;
  // Record name with the ';'-subtitle stripped (g_junk_defs +0x28, 0x3f
  // bytes); the trade-center row label / player-info group name.
  std::string display_name;
  // Bible "LCName" (payload +0x26, g_junk_defs +0x228): the lower-case name
  // shown in the player-info dialog.
  std::string lc_name;
  // Bible "Abbrev" (payload +0x66, g_junk_defs +0x128): the short status-bar
  // label.
  std::string abbrev;
  // Bible "BuyOn" (payload +0xa6, g_junk_defs +0x328): control-bit test
  // expression; blank = always eligible.
  std::string buy_on;
  // Bible "SellOn" (payload +0x1a5, g_junk_defs +0x427): control-bit test
  // expression; blank = always eligible.
  std::string sell_on;

  // ---- Runtime quantity (g_junk_defs +0x22, persisted in the pilot save) ----
  std::int32_t count = 0;
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
  std::string name; // resource record name (display / HUD label)
  std::string
      target_code; // name table (payload +0x44; g_government_name_table):
                   // the short string the target-status panel shows
                   // (Bible "TargetCode", e.g. " Fed.")
  std::string
      comm_name; // name table (payload +0x34; g_government_comm_name_table;
                 // Bible "CommName" -- comm chatter / fame label)
  std::string
      medium_name; // medium name table (payload +0x64; Bible "MediumName")

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
// Ghidra g_system_region_trigger_defs (stride 0x208, up to 32 entries loaded
// from n\x91bu resources in NovaData_LoadScenarioResourceTables 0x004bd3c0).
// Each record is one nebula/region backdrop on the galaxy starmap: a world
// rectangle (normal-zoom coordinates) plus two NCB strings, and two runtime
// bytes -- the cached ActiveOn result (+0x8) and the explored latch (+0x9).
struct Nebula {
  std::int16_t x = 0; // XPos (payload +0x00)
  std::int16_t y = 0; // YPos (payload +0x02)
  std::int16_t width =
      0; // XSize (payload +0x04); 0 when the resource is absent
  std::int16_t height = 0;           // YSize (payload +0x06)
  std::string active_on_expression;  // payload +0x08 (CString copy at +0xa)
  std::string on_explore_expression; // payload +0x107
  // ---- Runtime state (decoded with, not from, the payload). active_on is
  // re-evaluated wherever the original refreshes availability (empty
  // expressions evaluate true); explored latches once any visited system's
  // position falls inside the rect inset by 8 (System region-event pass
  // 0x00467bd0), which then executes the OnExplore set expression once.
  bool active_on = false;
  bool explored = false;
};

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
  // Murk (s\xd8st +0x92): murkiness 0-100; a negative value hides the
  // starfield (SystemDef.murk at +0xbc < 0; the payload name "alert_level" is
  // a misnomer). Feeds the ambient-star size scale as well.
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

  // SystemDef +0xb8/+0xba. Visibility twin remap: the scenario loader's
  // post-load pass (NovaData_LoadScenarioResourceTables 0x004bd3c0 /
  // 0x004beb4f) initializes both to -1, then groups decoded systems sharing
  // the same map position: the lowest id becomes the group root, later twins
  // chain via visible_parent_system_id, and every twin's
  // visibility_root_system_id points at the root. Consumed by
  // System_ResolveVisibleSystemForTravel (0x0046b920) and
  // System_ResolveSystemDiscoverySlot (0x0046b9b0).
  std::int16_t visible_parent_system_id = -1;  // +0xb8
  std::int16_t visibility_root_system_id = -1; // +0xba

  // ---- Runtime discovery/visibility state (decoded with, not from, the
  // payload). Mirrors SystemDef is_visible / has_explored_flag / discovery
  // state. Original semantics (verified against the loader 0x004bd3c0 and
  // NovaResources_EvaluateAvailability 0x00448090): has_explored_flag
  // (+0x1ed) and is_visible (+0x1eb) are set to 1 by the scenario loader for
  // every decoded syst - they are LOADED/availability flags, not fog (the
  // loader is the runtime writer; Ship_InitGameplayDataTables 0x004b0c20 only
  // clears them during startup, before the loader runs).
  // EvaluateAvailability re-filters is_visible through the system's
  // Visibility NCB. The per-system FOG state is discovery_state below.
  // Fog consumers read discovery_state / discovered_this_rebuild /
  // control.explored_systems instead; treating these loader flags as fog
  // state makes far-system stellars unavailable after a map reveal.
  bool is_visible = false;
  bool has_explored_flag = false;

  // Ghidra SystemDef +0x90 (short): persistent fog-of-war state, saved per
  // system as u16[0x800] in the pilot save (PilotFile_SaveGameCore
  // 0x004c7dd0 / LoadSave 0x004cb260). 0 = unknown; >=1 = visited (in-flight
  // jump arrival writes 1, landed/stellar travel and map-outfit reveals write
  // 2; the debug galaxy reveal also writes 2 everywhere).
  std::int16_t discovery_state = 0;

  // Ghidra SystemDef +0x1ec: transient "drawn on the current map view" latch,
  // recomputed by System_RebuildSystemVisibilityMap (0x00467970) and the per-
  // tick System_UpdateSystemAndStellarDisplayState (0x00432470): true for
  // visited systems and every travel-resolvable link neighbour of one. This
  // is what lets the starmap show one jump ahead without marking the
  // neighbour visited.
  bool discovered_this_rebuild = false;
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
  // Lowest row field; seeds a spawned AsteroidState's integrity (+0x1c),
  // which doubles as the asteroid's INTEGRITY
  // COUNTER: NovaUi_ResolveWeaponSplashImpact (0x00436ff0) decrements it by
  // the hitting weapon's shield damage (x10 for flags_secondary 0x8000) and
  // runs the destruction package below zero. Ghidra DAT_005912dc[mode]
  // (+0x00). Payload word[0x0]. Verbatim, no rebase.
  std::int16_t wander_table_value = 0;
  // Float wander-speed scale for this type; `(rand(0x29)+0x50) * this * 0.01`
  // yields a state's wander_speed (+0x18). Ghidra DAT_005912f0[mode]
  // (+0x14). Payload word[0x2] * 0.01 (0x64..0x12c => 0.5..3.0 shipped).
  float wander_speed_multiplier = 1.0F;
  // +0x02; DECODED (Weapon_SpawnWeaponImpactEffectPackage 0x00462550): the
  // number of junk freeflight objects spawned when the asteroid breaks
  // (object type from +0x04). Loader validates payload word[0x6] >= 0.
  std::int16_t field_0x02 = 0;
  // +0x04; DECODED: the freeflight-junk type id spawned on destruction
  // (freeflight objects are TODO(decomp) in the clean-room). Loader accepts
  // [-6,6] or [0x3e8,0x468).
  std::int16_t field_0x04 = 0;
  // +0x0c; DECODED: the debris SWParticle burst count emitted by the
  // destruction package. Loader validates payload word[0x8] >= 0.
  std::int16_t field_0x0c = 0;
  // +0x10; DECODED: the destruction area-effect id (Bible ExplodType-style
  // impact effect, -1 none) fired by Weapon_SpawnWeaponImpactEffectPackage.
  std::int16_t field_0x10 = 0;
  // +0x0e; DECODED: the asteroid's mass used as the divisor when a surviving
  // asteroid is nudged by weapon impact impulse (NovaUi_ResolveWeaponSplash-
  // Impact). Scales with size tier in the shipped data (Metal Small 150 /
  // Medium 300 / Big 600 / Huge 1200).
  std::int16_t lifetime = 0;
  // DECODED (Weapon_SpawnWeaponImpactEffectPackage 0x00462550): on
  // destruction a big asteroid splits into child asteroids of these two
  // types (+0x06/+0x08, 0-based r\xf6id indices, -1 = unset); the third slot
  // (+0x0a) is the split-count base: each break spawns
  // NovaRandom_Range(base) + ceil(base/2) children.
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
  std::vector<ShipClass> ships;  // indexed by ship_id - 0x80
  std::vector<Outfit> outfits;   // indexed by outfit_id - 0x80
  std::vector<Weapon> weapons;   // indexed by weapon_id - 0x80
  std::vector<Stellar> stellars; // indexed by stellar_id - 0x80
  std::vector<System> systems;   // indexed by system_id - 0x80
  // n\x91bu nebula/region table (g_system_region_trigger_defs): up to 32
  // entries, index = resource id - 0x80; absent ids keep width/height 0
  // (the original zero-fills the trigger rects before probing each id).
  std::vector<Nebula> nebulae;         // indexed by nebula_id - 0x80
  std::vector<Government> governments; // indexed by government_id - 0x80
  std::vector<FleetDef> fleets;        // indexed by fleet_id - 0x80
  std::vector<DudeDef> dudes;          // indexed by dude_id - 0x80
  std::vector<MissionDef> missions;    // indexed by mission id - 0x80
  // p\x91rs personality table (g_pers_defs): the original keeps 0x400
  // slots, slot i = resource id 0x80 + i (absent resources leave an inactive
  // row). Slot 0x3ff is reserved by the loader for the Shareware Enforcer
  // sentinel; the enforcer pass is TODO(decomp) — see LoadFromArchives.
  std::vector<PersDef> pers_defs; // indexed by pers id - 0x80
  // crön time-dependent events (g_cron_event_states defined half): 0x200
  // slots, slot i = resource id 0x80 + i; absent resources stay !present.
  // See CronEventDef and Mission_TickDailyCronEvents (0x00439500).
  std::vector<CronEventDef> cron_events; // indexed by crön id - 0x80
  // öops disaster / commodity-shock table (g_disaster_defs): 0x100 slots,
  // slot i = resource id 0x80 + i; absent resources stay !present. See
  // DisasterDef and System_UpdateDisasterStates (0x00424f90).
  std::vector<DisasterDef> disaster_defs; // indexed by disaster id - 0x80
  // j\x9fnk specialized-commodity table (g_junk_defs): 0x80 slots, slot i =
  // resource id 0x80 + i; absent resources stay !present. See JunkDef and
  // NovaUi_RunTradeCenterWindow (0x0048c730).
  std::vector<JunkDef> junk_defs; // indexed by junk id - 0x80
  // Asteroid/drift class table (r\x9aid family, one row per resource id
  // 0x80..0x8f). Ghidra g_asteroid_states's per-type params read via
  // the DAT_005912dc / DAT_005912f0 pair.
  std::vector<AsteroidDef> asteroid_defs; // indexed by type id - 0x80
  // Impact/explosion definitions are indexed directly by effect id 0..63;
  // the source resources themselves use ids 0x80..0xbf.
  std::array<ImpactEffect, 64> impact_effects{};

  // g_government_defs[faction] style lookup: zero-based index, unlike the
  // 0x80-based resource-id Government() below. Ship.faction_or_government_id
  // and the ship-class inherent-government fields live in this space (the
  // original indexes the def array directly with them). Declared before the
  // Government() member because that member's name shadows the struct type.
  [[nodiscard]] const struct Government *
  GovernmentByIndex(std::int16_t index) const;

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
  // gh.id 0x80.. lookup for a p\x91rs personality (g_pers_defs), or
  // nullptr when outside the loaded 0x400-entry table.
  [[nodiscard]] const PersDef *Pers(std::int16_t resource_id) const;
  // gh.id 0x80.. lookup for a j\x9fnk commodity (g_junk_defs), or nullptr when
  // outside the loaded 0x80-entry table.
  [[nodiscard]] const JunkDef *Junk(std::int16_t resource_id) const;
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
