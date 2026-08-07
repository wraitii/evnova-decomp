#include "game/scenario_data.hpp"
#include "game/ship_visual.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <vector>

#include "brgr_archive.hpp"
#include "rle_sprite_sheet.hpp"

namespace game {

// These tests run against the shipped Nova Data archives; they assert values
// that were verified directly from the raw payload bytes, so they pin both the
// BRGR map resolution and the clean-room decoders to the exact game data.

TEST_CASE("scenario tables load ships, outfits and weapons",
          "[scenario][data]") {
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());

  // Verifiable against the resources: the default starter ship (id 0x80) has
  // the stats observed in its payload.
  const ShipClass *ship = data.Ship(0x80);
  REQUIRE(ship != nullptr);
  CHECK(ship->cargo_holds == 10);
  CHECK(ship->base_shield == 30);
  CHECK(ship->accel == 500.0F);
  CHECK(ship->speed == 400.0F);
  CHECK(ship->turn_rate == 40.0F);
  CHECK(ship->base_fuel == 300);
  CHECK(ship->free_mass == 8);
  CHECK(ship->base_armor == 30);
  CHECK(ship->mass_tons == 15);
  // Cost is repacked as a 4-byte big-endian field at 0x30 (0x00002710 = 10000).
  CHECK(ship->cost == 10000);

  // The starter ship's stock weapon banks: bank 0 has a single stock weapon of
  // weapon id 0x80 (the weapon defined at resource id 128).
  CHECK(ship->stock_weapons[0].weapon_id == 0x80);
  CHECK(ship->stock_weapons[0].count == 1);
  CHECK(ship->stock_weapons[0].ammo_load == -1); // no ammo (unlimited)

  // Weapon id 0x80 (a projectile): values verified from its payload.
  const Weapon *w = data.Weapon(0x80);
  REQUIRE(w != nullptr);
  CHECK(w->reload_ticks == 10);
  CHECK(w->lifetime_ticks == 13);
  CHECK(w->mass_damage == 1);
  CHECK(w->energy_damage == 4);
  CHECK(w->guidance_mode == -1); // unguided projectile
  CHECK(std::fabs(w->projectile_speed - 1500.0F) < 0.001F);
  CHECK(w->ammo_type == -1); // unlimited ammo
  CHECK(w->sprite_id == 0);
  // Weapon-decode audit: pin the re-named/offset fields to the Light Blaster's
  // actual payload (verified byte-for-byte). The loader maps the first packet
  // in Bible order and the tail per its WeaponDef mapping.
  CHECK(w->inaccuracy == 9); // shot_random_spread
  CHECK(w->fire_sound == 8);
  CHECK(w->impact_sound_slot == 10); // was mislabeled 'impact' but same offset
  CHECK(w->impact_effect_id == -1);  // was 'explosion'
  CHECK(w->blast_radius == 5);       // was 'prox_radius'
  CHECK(w->splash_radius == 6);      // was 'blast_radius'
  CHECK(w->flags == 0x6100);
  CHECK(w->flags_quaternary == 0U); // was mislabeled 'seeker'
  CHECK(w->flags_secondary == 0U);  // (payload +0x48)
  CHECK(w->flags_tertiary == 2U);   // (payload +0x66)
  CHECK(w->turret_arc_degrees == 0);
  CHECK(w->shot_anim_frame_dwell == 0);
  CHECK(w->kickback_impulse == 0);
  CHECK(w->burst_cycle_ticks == 0); // (the old mislabeled 'max_ammo' at +0x5a)
  CHECK(w->burst_reset_cooldown == 0);
  CHECK(w->retarget_interval_ticks == 0);

  // Outfits load; the first outfit is a weapon-type (ModType 1 -> weapon).
  CHECK(data.Outfit(0x80) != nullptr);
  CHECK(data.Outfit(0x80)->mod_type == 1);
  CHECK(data.Outfit(0x80)->mod_val == 0x80); // references weapon id 128
}

TEST_CASE("scenario resource families resolve through the BRGR adapter",
          "[scenario][brgr]") {
  // The five scenario families live across the Nova Data archives; the adapter
  // must find them all (regression: Nova Data 4's w\x91ap / o\x9ftf records
  // were previously missed by a too-loose resource.map scan).
  for (std::uint32_t type : {0x73689570U,
                             0x6f9f7466U,
                             0x77916170U,
                             0x73709a62U,
                             0x73d87374U,
                             0x679a7674U}) {
    const auto first = NovaResource_Load(type, 0x80);
    CHECK(first.has_value());
  }
}

TEST_CASE("record names are surfaced from the BRGR map for ships and outfits",
          "[scenario][brgr]") {
  // The scenario loader's display names come from the resource.map record name
  // (never a numeric header field); assert they reach the model tables.
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());
  CHECK(data.Ship(0x80) != nullptr);
  CHECK(data.Ship(0x80)->display_name == "Shuttle");
  CHECK(data.Outfit(0x80) != nullptr);
  CHECK(data.Outfit(0x80)->name == "Light Blaster");
  CHECK(data.System(0x80) != nullptr);
  CHECK(data.System(0x80)->name == "Kania");
  CHECK(data.Stellar(0x80) != nullptr);
  CHECK(data.Stellar(0x80)->name == "Earth");
  CHECK(data.Weapon(0x80) != nullptr);
  CHECK(data.Weapon(0x80)->name == "Light Blaster");
}

TEST_CASE("outfit tail fields decode at their real payload offsets",
          "[scenario][data]") {
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());
  const auto *o = data.Outfit(0x80); // Light Blaster
  REQUIRE(o != nullptr);
  // Verified from the raw payload: Mass=3, TechLevel=4, ModType=1 (weapon),
  // Cost is a 4-byte big-endian value at +0x0e (5000 for the Light Blaster).
  CHECK(o->mass_tons == 3);
  CHECK(o->tech_level == 4);
  CHECK(o->mod_type == 1);
  CHECK(o->mod_val == 0x80);
  CHECK(o->max_count == 8);
  CHECK(o->flags == 0x0001U); // fixed gun
  CHECK(o->cost == 5000);
  // Tail block: DispWeight, Graphic, BuyRandom, ItemClass from +0x3ec..+0x3f3.
  CHECK(o->display_weight == 0);
  CHECK(o->sprite_id == 0);
  CHECK(o->buy_random == 100);
  CHECK(o->item_class == 0x7f);
  // ShortName / LCName / LCPlural Pascal strings (here stored as C strings
  // trimmed of their length byte by the loader; we surface them as-is).
  CHECK(o->short_name == "Light Blaster");
  // Contribute/Require 64-bit pairs from +0x1e/+0x22 and +0x26/+0x2a.
  CHECK(o->contribute_lo == 0);
  CHECK(o->contribute_hi == 0);
  CHECK(o->require_lo == 0);
  CHECK(o->require_hi == 1);
}

TEST_CASE("outfit purchase mass/price derived computation",
          "[scenario][data]") {
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());
  const auto *o = data.Outfit(0x80);
  REQUIRE(o != nullptr);
  const std::int16_t hull = 15; // the Shuttle's Mass
  // Light Blaster: no mass/price proportional flag, so cost/mass pass through.
  CHECK(o->PurchasePrice(hull) == 5000);
  CHECK(o->PurchaseMass(hull) == 3);

  // A mass-proportional outfit (Flags bit 0x0400): scaled = Mass*hull/100, but
  // never below the base positive mass. We don't pin a specific item here (the
  // ratio varies by item); assert the positive base is preserved and scaling
  // applied when the flag is set.
  const auto *weapon_outfit = data.Outfit(0x81);
  REQUIRE(weapon_outfit != nullptr);
  if ((weapon_outfit->flags & 0x0400U) != 0) {
    CHECK(weapon_outfit->PurchaseMass(hull) >= 1);
  } else {
    CHECK(weapon_outfit->PurchaseMass(hull) == weapon_outfit->mass_tons);
  }
}

TEST_CASE("nova control bit expression evaluator", "[scenario][control]") {
  using game::ControlExpressionState;
  using game::NovaControlExpression_Evaluate;
  ControlExpressionState state;
  state.get_control_bit = [](std::uint32_t bit) { return bit == 5; };
  state.is_male = [] { return true; };
  state.owns_outfit = [](std::int16_t id) { return id == 0x80; };
  state.is_registered = [](std::uint32_t) { return true; };
  state.has_explored = [](std::int16_t id) { return id == 0x81; };

  // Blank expression -> true (the original's default).
  CHECK(NovaControlExpression_Evaluate("", state));
  CHECK(NovaControlExpression_Evaluate("b5", state));
  CHECK_FALSE(NovaControlExpression_Evaluate("b6", state));
  CHECK(NovaControlExpression_Evaluate("!b6", state));
  CHECK(NovaControlExpression_Evaluate("g", state));
  CHECK(NovaControlExpression_Evaluate("b5 & g", state));
  CHECK(NovaControlExpression_Evaluate("o128", state)); // outfit id 0x80
  CHECK(NovaControlExpression_Evaluate("e129", state)); // system id 0x81
  CHECK_NOTHROW(NovaControlExpression_Evaluate("b5 | b6", state));
  CHECK(NovaControlExpression_Evaluate("b5 | b6", state));
  CHECK_FALSE(NovaControlExpression_Evaluate("b6 & b5", state));
  CHECK(NovaControlExpression_Evaluate("(b5 | b6) & g", state));
  CHECK(NovaControlExpression_Evaluate("b5 & (b6 | g)", state));
  // Counted set with comparison.
  ControlExpressionState s2;
  s2.get_control_bit = [](std::uint32_t bit) { return bit == 1 || bit == 2; };
  CHECK(NovaControlExpression_Evaluate("[b1 b2] = 2", s2));
  CHECK_FALSE(NovaControlExpression_Evaluate("[b1 b3] = 2", s2));
  CHECK(NovaControlExpression_Evaluate("[b1 b2] > 0", s2));
}

TEST_CASE("government table loads and decodes the Federation class",
          "[scenario][data]") {
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());
  // The federal government (id 0x80) is the canonical / starter faction; these
  // values were verified from the raw g\x9avt payload in Nova Data 1, pinning
  // both the map resolution and the DecodeGovernment offsets.
  const Government *f = data.Government(0x80);
  REQUIRE(f != nullptr);
  REQUIRE(f->present);
  CHECK(f->name == "Federation");
  CHECK(f->comm_name == "Federation");
  CHECK(f->medium_name == "Federation");
  // voice_type_code 1 (raw) -> mode -1.
  CHECK(f->voice_type_code == 1);
  CHECK(f->voice_type_mode == -1);
  CHECK(f->flags_primary == 0xe2b0U);
  CHECK(f->ai_skill_percent == -32768);
  // classes/ally/enemy lists from payload +0x18/+0x20/+0x28.
  CHECK(f->classes[0] == 1);
  CHECK(f->classes[1] == -1);
  CHECK(f->ally_classes == std::array<std::int16_t, 4>{0, 1, 12, 13});
  CHECK(f->enemy_classes == std::array<std::int16_t, 4>{2, 10, 16, 9});
  // Reputation penalties (payload +0x0a..+0x12).
  CHECK(f->disable_penalty == 1);
  CHECK(f->board_penalty == 1);
  CHECK(f->kill_penalty == 2);
  CHECK(f->shoot_penalty == 5);
  CHECK(f->max_odds == 5);
  // Skill fractions: pilot src 100 -> 1.0, combat src 200 -> 2.0.
  CHECK(f->pilot_skill_scale == 1.0F);
  CHECK(f->combat_rating_scale == 2.0F);
  // Inherent jamming: jam[0] payload +0x06 = 0; jam[1..3] from +0x5c = 7/5/0.
  CHECK(f->inherent_jam == std::array<std::int16_t, 4>{0, 7, 5, 0});
  // Theme color from packed RGB24 at payload +0xa4 = 0x002c2caf.
  CHECK(f->theme_red == 0x2c);
  CHECK(f->theme_green == 0x2c);
  CHECK(f->theme_blue == 0xaf);
  // Interface/news picture ids resolve into the 0x80.. range.
  CHECK(f->interface_id == 0x82);
  CHECK(f->news_pic_id == 9001);
}

TEST_CASE("ship sh.x9an descriptor decodes from Nova Ships",
          "[scenario][ships][brgr]") {
  // The starter Shuttle is ship class id 0x80; its sh\x8an descriptor and the
  // rl\x91D sheet it references live in the Nova Ships archives, which are now
  // on the archive search path.
  const auto payload = NovaResource_Load(kShipVisualResourceType,
                                         static_cast<std::uint16_t>(0x80));
  REQUIRE(payload.has_value());

  const auto d = DecodeShipVisualDescriptor(*payload);
  REQUIRE(d.has_value());
  CHECK(d->base_image_id == 1000); // Shuttle rl\x91D sheet
  CHECK(d->base_mask_id == 1001);
  CHECK(d->base_set_count == 3); // 3 sets per rotation
  CHECK(d->base_x_size == 24);
  CHECK(d->base_y_size == 24);
  CHECK(d->base_transparency == 0);
  CHECK(d->frames_per_rotation == 36);
  CHECK(d->sprite_behavior_flags == 0x0041);
  CHECK(d->anim_delay == 0);
  CHECK(d->weapon_decay == 0); // no weapon-glow fade for the bare Shuttle

  // The engine-glow layer names the 'Shuttle Eng Glow' rl\x9144 sheet 0x0578
  // (108-frame rotation grid matching the base). Ghidra
  // ShipClass_LoadShipClassVisualAndLaunchData reads the glow image/mask/xy at
  // +0x16/+0x18/+0x1a/+0x1c and binds it to the per-class glow sprite set.
  // The descriptor's GlowX/YSize (48) matches the sheet canvas: a 48x48 frame
  // (half again larger than the 24x24 hull) so the exhaust jets extend past the
  // ship.
  CHECK(d->engine_glow_image_id == 0x0578);
  CHECK(d->engine_glow_mask_id == 0x0579);
  CHECK(d->engine_glow_x_size == 48);
  CHECK(d->engine_glow_y_size == 48);

  // The referenced 16-bit sheet must be decodable and hold base_set_count *
  // frames_per_rotation frames (3 * 36 = 108) at the descriptor's dimensions.
  const auto sheet =
      NovaResource_Load(kResourceTypeRleSheet16, d->base_image_id);
  REQUIRE(sheet.has_value());
  const auto decoded = RleSpriteSheet_Decode16(*sheet);
  REQUIRE(decoded.has_value());
  CHECK(decoded->width == 24);
  CHECK(decoded->height == 24);
  CHECK(decoded->frames.size() == 108);

  // The glow sheet shares the base's rotation grid: same frame count, larger
  // canvas (48x48 vs the 24x24 hull).
  const auto glow_sheet =
      NovaResource_Load(kResourceTypeRleSheet16, d->engine_glow_image_id);
  REQUIRE(glow_sheet.has_value());
  const auto glow_decoded = RleSpriteSheet_Decode16(*glow_sheet);
  REQUIRE(glow_decoded.has_value());
  CHECK(glow_decoded->width == 48);
  CHECK(glow_decoded->height == 48);
  CHECK(glow_decoded->frames.size() == 108);
}

// VERIFY the stellar (planet) graphic path end to end on the Kania system:
// the starting system Earth planet has link_a_id 0 -> spin sp\x9an 1000 ->
// rl\x91D 2000 (150x150), and its government id resolves to the Federation
// (0x80).
TEST_CASE("stellar spin sprites resolve from Nova Graphics",
          "[scenario][stellar]") {
  // Nova Graphics 1/2 now on the archive path, so stellar spin ids 1000+ are
  // reachable. The Kania Earth is system 0x80 stellar 0x80.
  const auto spin = NovaResource_Load(kResourceTypeSprites, 1000);
  REQUIRE(spin.has_value());
  const auto def = NovaSpriteDefinition_Parse(*spin);
  REQUIRE(def.has_value());
  CHECK(def->sprites_resource_id == 2000);
  CHECK(def->tiles_x == 1);
  CHECK(def->tiles_y == 1);
  const auto sheet =
      NovaResource_Load(kResourceTypeRleSheet16, def->sprites_resource_id);
  REQUIRE(sheet.has_value());
  const auto decoded = RleSpriteSheet_Decode16(*sheet);
  REQUIRE(decoded.has_value());
  CHECK(decoded->width == 150);
  CHECK(decoded->height == 150);

  // The stellar decode surfaces link_a_id and the government id.
  game::ScenarioData data;
  REQUIRE(data.LoadFromArchives());
  const game::Stellar *earth = data.Stellar(0x80);
  REQUIRE(earth != nullptr);
  CHECK(earth->link_a_id == 0);
  CHECK(earth->government_id == 0x80);
  CHECK(earth->pos_x == 0);
  CHECK(earth->pos_y == 0);
  // A governed stellar resolves to a real government whose theme colour tints
  // its HUD/planet presentation.
  const game::Government *gov = data.Government(earth->government_id);
  REQUIRE(gov != nullptr);
  CHECK(gov->present);
}

// The Light Blaster's projectile shot sprite set. Each weapon's shot sprite
// set id (Ghidra WeaponDef.shot_sprite_set_id, our Weapon.sprite_id from the
// "Graphic" payload field) selects a ``g_weapon_sprite_set_table`` entry that
// FUN_004ad960 builds from the sp\x9an spin descriptor at id + 3000 (the
// weapon spin-object range). The Light Blaster's sprite_id is 0, so its bolt
// spinner is spin resource 3000. Pin the decode so the flight shot renderer
// mounts the real bolt rather than the bright-dot fallback.
TEST_CASE("light blaster shot sprite (spin resource 3000) decodes",
          "[scenario][weaponshot]") {
  game::ScenarioData data;
  REQUIRE(data.LoadFromArchives());
  const game::Weapon *w = data.Weapon(0x80);
  REQUIRE(w != nullptr);
  CHECK(w->sprite_id == 0);

  // spin descriptor id = shot_sprite_set_id + 3000.
  const auto spin_id = static_cast<std::uint16_t>(w->sprite_id + 3000);
  const auto spin = NovaResource_Load(kResourceTypeSprites, spin_id);
  REQUIRE(spin.has_value());
  const auto def = NovaSpriteDefinition_Parse(*spin);
  REQUIRE(def.has_value());

  const auto sheet_data =
      NovaResource_Load(kResourceTypeRleSheet16, def->sprites_resource_id);
  REQUIRE(sheet_data.has_value());
  const auto sheet = RleSpriteSheet_Decode16(*sheet_data);
  REQUIRE(sheet.has_value());
  // Each bolt frame is a single tile (one frame per direction/pulse, laid out
  // as tiles_x * tiles_y distinct frames in the sheet).
  CHECK(sheet->width == def->tile_width);
  CHECK(sheet->height == def->tile_height);
}

// Kania (system 0x80) owns exactly Port Kane + the HG-Kania hypergate as its
// space objects (System.nav_defs from payload +0x24). The flight renderer draws
// only the current system's nav_defs stellars, so this pins that membership.
TEST_CASE("system nav_defs identify the owned space stellars",
          "[scenario][system]") {
  game::ScenarioData data;
  REQUIRE(data.LoadFromArchives());
  const game::System *kania = data.System(0x80);
  REQUIRE(kania != nullptr);
  // Each nav_def >= 0x80 is an owned stellar resource id (other slots hold the
  // -1 sentinel).
  std::vector<std::int16_t> owned;
  for (const auto nav : kania->nav_defs) {
    if (nav >= 0x80) {
      owned.push_back(nav);
      REQUIRE(data.Stellar(nav) != nullptr);
    }
  }
  REQUIRE(owned.size() == 2);
  CHECK(owned[0] == 0x89);  // Port Kane
  CHECK(owned[1] == 0x57c); // HG-Kania hypergate
  CHECK(data.Stellar(0x89)->name == "Port Kane");
  CHECK(data.Stellar(0x89)->link_a_id == 34); // spin 1034 planet sprite
  // 1404 HG-Kania hypergate's alternate sprite set id is 1 (a hypergate icon).
  CHECK(data.Stellar(0x57c)->link_a_id == 1);
}

// The per-system space background tint (syst BkgndColor +0x8e, 24-bit RRGGBB)
// and murk (syst +0x92) feed the flight backdrop + amber starfield. Values are
// verified against the raw payload bytes; they shape the ground-truth rendering
// decided in the reimplementation (SystemDef.field_0x1ee / alert_level).
TEST_CASE("system background color and murk decode from the payload",
          "[scenario][system]") {
  game::ScenarioData data;
  REQUIRE(data.LoadFromArchives());

  // Kania: pure-black space backdrop (BkgndColor = 0), murk 0 (stars shown,
  // full 32px sprite scale).
  const game::System *kania = data.System(0x80);
  REQUIRE(kania != nullptr);
  CHECK(kania->bkgnd_color == 0x000000);
  CHECK(kania->murk == 0);

  // Alphara: backdrop bytes at +0x8e..+0x90 are {0x00, 0x19, 0x19}; the
  // original's byte-order quirk maps R=+0x90, G=+0x8f, B=+0x8e, so the rendered
  // 0xRRGGBB is 0x191900 (R=G=0x19, B=0). Murk 20.
  const game::System *alphara = data.System(0x83);
  REQUIRE(alphara != nullptr);
  CHECK(alphara->name == "Alphara");
  CHECK(alphara->bkgnd_color == 0x191900);
  CHECK(alphara->murk == 20);
}

// The ambient star-field artwork: sp\x9an spin descriptor resource 700 is a
// 4x4 grid of 5x5px star tiles (16 distinct star shapes). Ghidra builds it into
// DAT_00593efc via Spin_ReadDescriptor(700,..) and the star spawn picks a
// random frame in [0, frame_count) where frame_count = tiles_x * tiles_y (the
// sprite +0x54 field). Pin the decode so the flight-view star rendering uses
// the real sheet.
TEST_CASE("starfield sheet (spin resource 700) decodes to 16 5x5 frames",
          "[scenario][starfield]") {
  using namespace game;
  const auto spin = NovaResource_Load(kResourceTypeSprites, 700);
  REQUIRE(spin.has_value());
  const auto def = NovaSpriteDefinition_Parse(*spin);
  REQUIRE(def.has_value());
  CHECK(def->tile_width == 5);
  CHECK(def->tile_height == 5);
  CHECK(def->tiles_x == 4);
  CHECK(def->tiles_y == 4);

  const auto sheet_data =
      NovaResource_Load(kResourceTypeRleSheet16, def->sprites_resource_id);
  REQUIRE(sheet_data.has_value());
  const auto sheet = RleSpriteSheet_Decode16(*sheet_data);
  REQUIRE(sheet.has_value());
  CHECK(sheet->width == 5);
  CHECK(sheet->height == 5);
  CHECK(sheet->frames.size() == 16);
}

// The stellar animation timing fields (sp\x6fb AnimDelay/Frame0Bias, Ghidra
// StellarDef +0x470/+0x472 from payload +0x22/+0x24) and the hypergate
// engage_highlight_frame (Ghidra StellarDef +0x26, payload +0x18) decode from
// the payload and gate the Stellar_UpdateStellarSprites frame stepping.
TEST_CASE("stellar animation fields decode", "[scenario][stellar]") {
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());

  // Earth / Port Kane: ordinary 1-frame planet sprites; dwell/multiplier 0 (so
  // the ambient stepper sees a single-frame set and stays on frame 0).
  const Stellar *earth = data.Stellar(0x80);
  REQUIRE(earth != nullptr);
  CHECK(earth->animation_dwell_time == 0);
  CHECK(earth->animation_frame_multiplier == 0);
  CHECK((earth->availability_flags & 0x1000) == 0); // not a hypergate

  const Stellar *portkane = data.Stellar(0x89);
  REQUIRE(portkane != nullptr);
  CHECK(portkane->animation_dwell_time == 0);
  CHECK(portkane->animation_frame_multiplier == 0);
  CHECK((portkane->availability_flags & 0x1000) == 0); // not a hypergate

  // HG-Kania hypergate: 42-frame animated set (link_a 1 -> spin 1001), the
  // hypergate availability bit (0x1000) set, and an engaged highlight frame 37
  // (holds/clamps on the opening/working boundary).
  const Stellar *hg = data.Stellar(0x57c);
  REQUIRE(hg != nullptr);
  CHECK(hg->link_a_id == 1);
  CHECK((hg->availability_flags & 0x1000) != 0); // hypergate
  CHECK(hg->engage_highlight_frame == 37);
  CHECK(hg->animation_dwell_time == 0);
  CHECK(hg->animation_frame_multiplier == 0);
}

} // namespace game
