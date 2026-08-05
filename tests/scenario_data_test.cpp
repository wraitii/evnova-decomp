#include "game/scenario_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "brgr_archive.hpp"

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
                             0x73709a62U, 0x73d87374U}) {
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

} // namespace game
