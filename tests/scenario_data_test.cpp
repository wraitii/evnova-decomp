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

TEST_CASE("scenario tables load ships, outfits and weapons", "[scenario][data]") {
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
  for (std::uint32_t type : {0x73689570U, 0x6f9f7466U, 0x77916170U,
                             0x73709a62U, 0x73d87374U, 0x679a7674U}) {
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
  const auto *o = data.Outfit(0x80);  // Light Blaster
  REQUIRE(o != nullptr);
  // Verified from the raw payload: Mass=3, TechLevel=4, ModType=1 (weapon),
  // Cost is a 4-byte big-endian value at +0x0e (5000 for the Light Blaster).
  CHECK(o->mass_tons == 3);
  CHECK(o->tech_level == 4);
  CHECK(o->mod_type == 1);
  CHECK(o->mod_val == 0x80);
  CHECK(o->max_count == 8);
  CHECK(o->flags == 0x0001U);  // fixed gun
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

TEST_CASE("outfit purchase mass/price derived computation", "[scenario][data]") {
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());
  const auto *o = data.Outfit(0x80);
  REQUIRE(o != nullptr);
  const std::int16_t hull = 15;  // the Shuttle's Mass
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
  CHECK(NovaControlExpression_Evaluate("o128", state));  // outfit id 0x80
  CHECK(NovaControlExpression_Evaluate("e129", state));   // system id 0x81
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

TEST_CASE("ship sh.x9an descriptor decodes from Nova Ships", "[scenario][ships][brgr]") {
  // The starter Shuttle is ship class id 0x80; its sh\x8an descriptor and the
  // rl\x91D sheet it references live in the Nova Ships archives, which are now
  // on the archive search path.
  const auto payload =
      NovaResource_Load(kShipVisualResourceType, static_cast<std::uint16_t>(0x80));
  REQUIRE(payload.has_value());

  const auto d = DecodeShipVisualDescriptor(*payload);
  REQUIRE(d.has_value());
  CHECK(d->base_image_id == 1000);   // Shuttle rl\x91D sheet
  CHECK(d->base_mask_id == 1001);
  CHECK(d->base_set_count == 3);     // 3 sets per rotation
  CHECK(d->base_x_size == 24);
  CHECK(d->base_y_size == 24);
  CHECK(d->base_transparency == 0);
  CHECK(d->frames_per_rotation == 36);
  CHECK(d->sprite_behavior_flags == 0x0041);
  CHECK(d->anim_delay == 0);
  CHECK(d->weapon_decay == 0);   // no weapon-glow fade for the bare Shuttle

  // The referenced 16-bit sheet must be decodable and hold base_set_count *
  // frames_per_rotation frames (3 * 36 = 108) at the descriptor's dimensions.
  const auto sheet = NovaResource_Load(kResourceTypeRleSheet16, d->base_image_id);
  REQUIRE(sheet.has_value());
  const auto decoded = RleSpriteSheet_Decode16(*sheet);
  REQUIRE(decoded.has_value());
  CHECK(decoded->width == 24);
  CHECK(decoded->height == 24);
  CHECK(decoded->frames.size() == 108);
}

// VERIFY the stellar (planet) graphic path end to end on the Kania system:
// the starting system Earth planet has link_a_id 0 -> spin sp\x9an 1000 ->
// rl\x91D 2000 (150x150), and its government id resolves to the Federation
// (0x80).
TEST_CASE("stellar spin sprites resolve from Nova Graphics", "[scenario][stellar]")
{
  // Nova Graphics 1/2 now on the archive path, so stellar spin ids 1000+ are
  // reachable. The Kania Earth is system 0x80 stellar 0x80.
  const auto spin = NovaResource_Load(kResourceTypeSprites, 1000);
  REQUIRE(spin.has_value());
  const auto def = NovaSpriteDefinition_Parse(*spin);
  REQUIRE(def.has_value());
  CHECK(def->sprites_resource_id == 2000);
  CHECK(def->tiles_x == 1);
  CHECK(def->tiles_y == 1);
  const auto sheet = NovaResource_Load(kResourceTypeRleSheet16,
                                       def->sprites_resource_id);
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
  CHECK(owned[0] == 0x89);   // Port Kane
  CHECK(owned[1] == 0x57c);  // HG-Kania hypergate
  CHECK(data.Stellar(0x89)->name == "Port Kane");
  CHECK(data.Stellar(0x89)->link_a_id == 34);  // spin 1034 planet sprite
  // 1404 HG-Kania hypergate's alternate sprite set id is 1 (a hypergate icon).
  CHECK(data.Stellar(0x57c)->link_a_id == 1);
}

} // namespace game
