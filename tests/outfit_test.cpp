#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/outfit.hpp"
#include "game/spaceflight.hpp"

namespace {

using game::GameState;
using game::Outfit;
using game::OutfitEffect;
using game::PlayerEffectiveStats;
using game::ScenarioData;
using game::ShipClass;

// A deterministic scenario table with a single ship class and a small set of
// outfits injected directly (no archive dependency), so the outfit aggregation
// is unit-testable in isolation.
struct Fixture {
  GameState state;
  ShipClass *cls = nullptr;

  Fixture() {
    // Player flies the default ship class.
    state.player.ship_class_id = 0;
    ShipClass sc;
    sc.base_shield = 30;
    sc.base_armor = 30;
    sc.base_fuel = 300;
    sc.cargo_holds = 10;
    sc.accel = 500.0F;
    sc.speed = 400.0F;
    sc.turn_rate = 40.0F;
    sc.max_gun = 2;
    sc.max_turret = 1;
    state.scenario.ships.push_back(sc); // index 0 == ship_id 0x80
    cls = &state.scenario.ships[0];
  }

  // Appends a new outfit at a given index (zero-based after-0x80) and returns
  // its mutable reference, growing the table if needed.
  Outfit &AddOutfit(std::size_t index) {
    while (state.scenario.outfits.size() <= index) {
      state.scenario.outfits.emplace_back();
    }
    return state.scenario.outfits[index];
  }

  // Sets an outfit's primary ModType1/Val1 effect.
  static void SetEffect(Outfit &o, OutfitEffect type, std::int16_t val) {
    o.mod_type = static_cast<std::int16_t>(type);
    o.mod_val = val;
  }
};

} // namespace

using game::OutfitOwnership;

TEST_CASE("effective shield/armor/fuel/cargo combine class base + opcodes",
          "[outfit]") {
  Fixture f;
  // A shield booster (opcode 4, +10 shield each) and an armor (opcode 6).
  Outfit &shield = f.AddOutfit(0);
  shield.mod_type = static_cast<std::int16_t>(OutfitEffect::kShield);
  shield.mod_val = 10;
  Outfit &armor = f.AddOutfit(1);
  armor.mod_type = static_cast<std::int16_t>(OutfitEffect::kArmor);
  armor.mod_val = 20;

  f.state.inventory.outfit_owned_count[0] = 2; // two shield boosters
  f.state.inventory.outfit_owned_count[1] = 1; // one armor plate

  const PlayerEffectiveStats s =
      game::Outfit_ComputePlayerEffectiveStats(f.state);
  CHECK(s.max_shield_points == Catch::Approx(30 + 2 * 10));
  CHECK(s.max_armor_points == Catch::Approx(30 + 1 * 20));
  CHECK(s.fuel_capacity == Catch::Approx(300.0F));
  CHECK(s.cargo_capacity == Catch::Approx(10.0F));
}

TEST_CASE("opcode 2 cargo and opcode 12 fuel expand the relevant values",
          "[outfit]") {
  Fixture f;
  Outfit &cargo = f.AddOutfit(0);
  cargo.mod_type = static_cast<std::int16_t>(OutfitEffect::kCargoSpace);
  cargo.mod_val = 5;
  Outfit &fuel = f.AddOutfit(1);
  fuel.mod_type = static_cast<std::int16_t>(OutfitEffect::kFuelCapacity);
  fuel.mod_val = 100; // +1 jump worth

  f.state.inventory.outfit_owned_count[0] = 3;
  f.state.inventory.outfit_owned_count[1] = 1;

  const PlayerEffectiveStats s =
      game::Outfit_ComputePlayerEffectiveStats(f.state);
  CHECK(s.cargo_capacity == Catch::Approx(10 + 3 * 5));
  CHECK(s.fuel_capacity == Catch::Approx(300 + 100));
}

TEST_CASE("movement opcodes 7/8/9 add to accel/speed/turn using loader scale",
          "[outfit]") {
  Fixture f;
  // opcode 7 (accel) modval 1000 -> the raw-accel increment 1000/10000 = 0.1,
  // added to the class's raw accel (500). The spaceflight loop then converts
  // the raw effective accel to px/frame^2 by /10000 as it does for the class.
  Outfit &acc = f.AddOutfit(0);
  acc.mod_type = static_cast<std::int16_t>(OutfitEffect::kAccelerator);
  acc.mod_val = 1000;
  f.state.inventory.outfit_owned_count[0] = 1;

  const PlayerEffectiveStats s =
      game::Outfit_ComputePlayerEffectiveStats(f.state);
  CHECK(s.thrust_raw == Catch::Approx(500.0F + 0.1F));
  CHECK(s.speed_raw == Catch::Approx(400.0F)); // unchanged
  CHECK(s.turn_raw == Catch::Approx(40.0F));   // unchanged
}

TEST_CASE("base movement stats survive without outfits", "[outfit]") {
  Fixture f;
  const PlayerEffectiveStats s =
      game::Outfit_ComputePlayerEffectiveStats(f.state);
  CHECK(s.max_shield_points == Catch::Approx(30.0F));
  CHECK(s.max_armor_points == Catch::Approx(30.0F));
  CHECK(s.fuel_capacity == Catch::Approx(300.0F));
  CHECK(s.thrust_raw == Catch::Approx(500.0F));
  CHECK(s.speed_raw == Catch::Approx(400.0F));
  CHECK(s.turn_raw == Catch::Approx(40.0F));
}

TEST_CASE("owning a simple outfit is limited by its Max field", "[outfit]") {
  Fixture f;
  Outfit &o = f.AddOutfit(0);
  o.mod_type = static_cast<std::int16_t>(OutfitEffect::kShield);
  o.max_count = 4; // can hold at most 4

  f.state.inventory.outfit_owned_count[0] = 6;
  const OutfitOwnership lim = game::Outfit_ClampOwnedCountToLimits(f.state, 0);
  CHECK(lim.max_allowed == 4);
  CHECK(lim.effective_owned == 4); // clamped from 6
}

TEST_CASE("out-of-range outfit id resolves to zero ownership", "[outfit]") {
  Fixture f;
  const OutfitOwnership lim =
      game::Outfit_ClampOwnedCountToLimits(f.state, 0x201);
  CHECK(lim.max_allowed == 0);
  CHECK(lim.effective_owned == 0);
}

TEST_CASE("gun slot cap limits how many fixed guns the player can mount",
          "[outfit]") {
  Fixture f;
  f.cls->max_gun = 2; // ship can mount 2 guns
  // A fixed gun outfit (Flags 0x0001).
  Outfit &gun = f.AddOutfit(0);
  gun.flags = 0x0001;
  gun.max_count = 8;
  // Two boosters that lift the gun cap (opcode 45) by 1 each.
  Outfit &lift = f.AddOutfit(1);
  lift.mod_type = static_cast<std::int16_t>(OutfitEffect::kModifyMaxGuns);
  lift.mod_val = 1;
  f.state.inventory.outfit_owned_count[1] = 1;

  // With no guns owned yet, the player may take up to the (max_gun + lifts).
  const OutfitOwnership before =
      game::Outfit_ClampOwnedCountToLimits(f.state, 0);
  CHECK(before.max_allowed == 3); // 2 class + 1 lift

  // Owning 3 guns fills the cap: a 4th is clamped to 0 further / effective 3.
  f.state.inventory.outfit_owned_count[0] = 3;
  const OutfitOwnership after =
      game::Outfit_ClampOwnedCountToLimits(f.state, 0);
  CHECK(after.effective_owned == 3);
}

TEST_CASE("turrets clamp against the ship's max turret count", "[outfit]") {
  Fixture f;
  f.cls->max_turret = 1;
  // A turret outfit (Flags 0x0002).
  Outfit &tur = f.AddOutfit(0);
  tur.flags = 0x0002;
  tur.max_count = 5;
  f.state.inventory.outfit_owned_count[0] = 4;

  const OutfitOwnership lim = game::Outfit_ClampOwnedCountToLimits(f.state, 0);
  CHECK(lim.max_allowed == 1); // class cap
  CHECK(lim.effective_owned == 1);
}

TEST_CASE("AddInstalledOutfit respects the owned maximum", "[outfit]") {
  Fixture f;
  Outfit &o = f.AddOutfit(0);
  Fixture::SetEffect(o, OutfitEffect::kShield, 10);
  o.max_count = 2;

  CHECK(game::Outfit_AddInstalledOutfit(f.state, 0, 1) == 1);
  CHECK(f.state.inventory.outfit_owned_count[0] == 1);
  CHECK(game::Outfit_AddInstalledOutfit(f.state, 0, 1) == 1);
  CHECK(f.state.inventory.outfit_owned_count[0] == 2);
  // Third add is refused (at the Max).
  CHECK(game::Outfit_AddInstalledOutfit(f.state, 0, 1) == 0);
  CHECK(f.state.inventory.outfit_owned_count[0] == 2);
  // Mutating the inventory dirties the stat cache.
  CHECK(f.state.stat_cache_valid == false);
}

TEST_CASE("delta-armor under removal never drops below zero", "[outfit]") {
  Fixture f;
  Outfit &o = f.AddOutfit(0);
  Fixture::SetEffect(o, OutfitEffect::kArmor, 20);
  f.state.inventory.outfit_owned_count[0] = 3;

  CHECK(game::Outfit_RemoveOutfit(f.state, 0, 2) == 2);
  CHECK(f.state.inventory.outfit_owned_count[0] == 1);
  CHECK(game::Outfit_RemoveOutfit(f.state, 0, 10) == 1); // clamped by owned
  CHECK(f.state.inventory.outfit_owned_count[0] == 0);
}

TEST_CASE("cargo total is the sum of the six bins plus junk", "[outfit]") {
  Fixture f;
  f.state.inventory.cargo_bins = {2, 3, 0, 1, 0, 0}; // 6 tons
  f.state.inventory.junk_counts[0] = 4;
  f.state.inventory.junk_counts[3] = 1;
  CHECK(game::Outfit_ComputePlayerCargoAndJunkTotal(f.state) == 6 + 4 + 1);
}

TEST_CASE("remaining cargo space is capacity minus carried, clamped >= 0",
          "[outfit]") {
  Fixture f;
  Outfit &cargo = f.AddOutfit(0);
  cargo.mod_type = static_cast<std::int16_t>(OutfitEffect::kCargoSpace);
  cargo.mod_val = 6; // +6 tons -> capacity 10+6 = 16
  f.state.inventory.outfit_owned_count[0] = 1;

  f.state.inventory.cargo_bins[0] = 5;
  f.state.inventory.cargo_bins[1] = 3; // 8 used
  CHECK(game::Outfit_ComputeRemainingCargoSpace(f.state) == 16 - 8);

  // Over-capacity clamps to zero.
  f.state.inventory.cargo_bins[0] = 30;
  CHECK(game::Outfit_ComputeRemainingCargoSpace(f.state) == 0);
}

TEST_CASE("HasOwnedEffect senses any owned outfit carrying the opcode",
          "[outfit]") {
  Fixture f;
  Outfit &c = f.AddOutfit(0);
  c.mod_type = static_cast<std::int16_t>(OutfitEffect::kCloaking);
  f.state.inventory.outfit_owned_count[0] = 1;
  CHECK(game::Outfit_HasOwnedEffect(f.state, OutfitEffect::kCloaking));

  // Not owned => false.
  f.state.inventory.outfit_owned_count[0] = 0;
  CHECK(!game::Outfit_HasOwnedEffect(f.state, OutfitEffect::kCloaking));
}

TEST_CASE("recharge rates combine the class base with opcode 5/29 bonuses",
          "[outfit]") {
  Fixture f;
  f.cls->shield_recharge = 2.0F; // a class that recharges 2 shield/frame
  f.cls->armor_recharge = 1.0F;
  Outfit &sh = f.AddOutfit(0);
  Fixture::SetEffect(sh, OutfitEffect::kShieldRecharge, 500); // +base/500
  f.state.inventory.outfit_owned_count[0] = 1;

  const PlayerEffectiveStats s =
      game::Outfit_ComputePlayerEffectiveStats(f.state);
  CHECK(s.shield_recharge > 2.0F); // base plus a bonus
  CHECK(s.armor_recharge == 1.0F); // unchanged
}

TEST_CASE("alternate mods on one outfit contribute their opcodes", "[outfit]") {
  Fixture f;
  // One outfit with two effects: +10 shield (primary) and +1 gun cap (alt 1).
  Outfit &o = f.AddOutfit(0);
  Fixture::SetEffect(o, OutfitEffect::kShield, 10);
  o.alt_mod_types[0] = static_cast<std::int16_t>(OutfitEffect::kModifyMaxGuns);
  o.alt_mod_vals[0] = 1;
  f.state.inventory.outfit_owned_count[0] = 2;

  const PlayerEffectiveStats s =
      game::Outfit_ComputePlayerEffectiveStats(f.state);
  CHECK(s.max_shield_points == Catch::Approx(30 + 2 * 10));
  CHECK(s.max_guns == 2 + 0 + 1); // class 2 + no prior lifts + 1 alt
}

TEST_CASE("TickShieldRecharge grows shields toward the effective max",
          "[outfit]") {
  Fixture f;
  // A shield-regenerating outfit adds a few shield points per reference frame.
  Outfit &sh = f.AddOutfit(0);
  Fixture::SetEffect(sh, OutfitEffect::kShield, 10); // +10 max
  f.state.inventory.outfit_owned_count[0] = 1;

  // Effective max shield = 30 (class) + 10 = 40.
  const PlayerEffectiveStats eff =
      game::Outfit_ComputePlayerEffectiveStats(f.state);
  REQUIRE(eff.max_shield_points == Catch::Approx(40.0F));

  // Start below max; a tick with the class's (fallback 0) recharge grows it a
  // little. With no recharge outfit the rate is 0, so nothing moves.
  f.state.player.shield_points = 10.0F;
  game::NovaPlayer_TickShieldRecharge(f.state, 16.0F);
  CHECK(f.state.player.shield_points == Catch::Approx(10.0F));

  // Give the class a recharge rate and verify shields climb and cap at max.
  f.cls->shield_recharge = 100.0F;  // 100/frame at reference cadence
  f.state.stat_cache_valid = false; // class changed -> recompute the cache
  f.state.player.shield_points = 10.0F;
  game::NovaPlayer_TickShieldRecharge(f.state, 1000.0F); // 1 reference frame
  CHECK(f.state.player.shield_points > 10.0F);
  CHECK(f.state.player.shield_points <= eff.max_shield_points + 1e-3F);

  // A very large tick overshoots but clamps exactly to the max.
  f.state.player.shield_points = 10.0F;
  game::NovaPlayer_TickShieldRecharge(f.state, 1e6F);
  CHECK(f.state.player.shield_points == Catch::Approx(eff.max_shield_points));
}
