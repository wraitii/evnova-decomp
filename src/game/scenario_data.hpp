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
constexpr std::uint32_t kShipResourceType = 0x73689570;       // sh\x95p
constexpr std::uint32_t kOutfitResourceType = 0x6f9f7466;     // o\x9ftf
constexpr std::uint32_t kWeaponResourceType = 0x77916170;     // w\x91ap
constexpr std::uint32_t kStellarResourceType = 0x73709a62;    // sp\x9ab
constexpr std::uint32_t kSystemResourceType = 0x73d87374;     // s\xd8st
constexpr std::uint32_t kGovernmentResourceType = 0x679a7674; // g\x9avt
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
  std::string display_name; // resource name / target display
  std::string short_name;   // shipyard menu label
  std::string long_name;    // purchase dialog / new-pilot text

  std::int16_t cargo_holds = 0;    // Holds
  std::int16_t base_shield = 0;    // Shield
  std::int16_t base_armor = 0;     // Armor
  std::int16_t base_fuel = 0;      // Fuel (100 = 1 jump)
  std::int16_t free_mass = 0;      // FreeMass
  std::int16_t mass_tons = 0;      // Mass
  std::int16_t length_meters = 0;  // Length
  std::int16_t tech_level = 0;     // TechLevel
  std::int32_t cost = 0;           // Cost
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
  std::int16_t ammo_type = -1;       // AmmoType_c (ammo_or_energy_cost_code)

  std::int16_t sprite_id = 0;   // Graphic_e (shot_sprite_set_id)
  std::int16_t inaccuracy = 0;  // Inaccuracy10 (shot_random_spread)
  std::int16_t fire_sound = -1; // Sound12 (fire_sound_slot)

  std::int16_t impact_sound_slot = -1; // Impact14
  std::int16_t impact_effect_id = -1;  // ExplodType16
  std::int16_t blast_radius = 0;       // ProxRadius18
  std::int16_t splash_radius = 0;      // BlastRadius1a

  std::uint16_t flags = 0;            // Flags1c (flags_primary)
  std::uint16_t flags_quaternary = 0; // Seeker1e (flags_quaternary)
  std::uint16_t flags_secondary = 0;  // (resource +0x48, flags_secondary)
  std::uint16_t flags_tertiary = 0;   // (resource +0x66, flags_tertiary)

  std::int16_t turret_arc_degrees = 0; // (resource +0x30, was mislabeled
                                       // beam_length)
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
  std::uint16_t scan_mask_short =
      0; // GovtDef 0x22 (payload +0x04) [Provisional])
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
      links{}; // Con1-16 (system ids, stored -1/zero-based)
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
  std::array<std::int16_t, 8> dude_types{}; // DudeTypes (128-639, or neg fleet)
  std::array<std::int16_t, 8> dude_prob{};  // % Prob
  std::int16_t avg_ships = 0;               // AvgShips
  std::int16_t government_id = -1;          // Govt
  std::int16_t message_id = -1;             // Message
  std::int16_t asteroid_count = 0;          // Asteroids
  std::int16_t interference = 0;            // Interference
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
  std::uint16_t ast_types = 0;     // AstTypes
  std::int16_t reinf_fleet = -1;   // ReinfFleet
  std::int16_t reinf_time = 0;     // ReinfTime
  std::int16_t reinf_interval = 0; // ReinfIntrval
  std::string visibility_expr;     // Visibility

  // ---- Runtime discovery/visibility state (decoded with, not from, the
  // payload). Mirrors SystemDef is_visible / has_explored_flag / discovery
  // state, maintained by System_UpdateSystemAndStellarDisplayState and the
  // discovery flood (System_FloodDiscoverAdjacentSystems). These gate which
  // systems (and hence their stellars) the player may target. ----
  bool is_visible = false;
  bool has_explored_flag = false;
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
