#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/government.hpp"
#include "game/landed_store.hpp"
#include "game/landed_window.hpp"
#include "game/mission.hpp"
#include "game/outfit.hpp"
#include "game/ship_ai.hpp"
#include "game/ship_spawn.hpp"
#include "game/ship_visual.hpp"
#include "game/spaceflight.hpp"
#include "game/travel.hpp"
#include "game/weapon.hpp"

#include <array>
#include <string>
#include <string_view>

// The player's ship class supplies the baseline Contribute mask that almost
// every outfit's Require mask depends on (e.g. the starter Shuttle contributes
// Require high-word bit 0, matching the req_hi=1 on base weapons such as the
// Light Blaster). Without seeding the aggregate from the ship class a fresh
// player fails every Require gate and can buy nothing. Regression for the
// "can't buy outfits" bug.
TEST_CASE("Outfitter buying respects the ship class contribute baseline",
          "[landed_store][scenario]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // the starter Shuttle (id 0x80)
  state.player.credits = 10'000;

  // The starter Shuttle's Contribute mask comes from the shp payload: lo=0,
  // hi=1 (bit 0 of the high word), giving a fresh player the base that the
  // Light Blaster (req_hi=1) and friends require to be purchasable.
  const game::ShipClass *shuttle = state.scenario.Ship(0x80);
  REQUIRE(shuttle != nullptr);
  CHECK(shuttle->contribute_lo == 0);
  CHECK(shuttle->contribute_hi == 1);
  // The previously (mis)decoded require at shp +0x72a is the Shuttle's cost,
  // not a Require mask; the real Require (+0x380/+0x384) is empty.
  CHECK(shuttle->require_lo == 0);
  CHECK(shuttle->require_hi == 0);

  // With the baseline contribute mask the Light Blaster is purchasable and
  // buys cleanly.
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x80));
  game::LandedStoreSession session;
  CHECK(game::NovaLanded_BuyOutfit(state, 0x80, 0x80, 1) == 1);
  CHECK(state.inventory.outfit_owned_count[0] == 1);
  CHECK(state.player.credits == 5'000); // paid the 5000 credit list price
}

TEST_CASE("Ship replacement rechecks requirements and preserves mutation order",
          "[landed_store][shipyard]") {
  game::GameState state;
  state.scenario.ships.resize(2);
  state.scenario.stellars.resize(1);
  state.scenario.outfits.resize(1);
  state.scenario.weapons.resize(1);
  state.player.ship_class_id = 0;
  state.player.ship_name = "Wayfarer";
  state.player.credits = 1'000;
  state.scenario.ships[0].cost = 100;
  state.scenario.ships[0].cargo_holds = 10;
  state.scenario.ships[0].contribute_lo = 1;
  state.scenario.ships[1].cost = 500;
  state.scenario.ships[1].cargo_holds = 5;
  state.scenario.ships[1].require_lo = 2;
  state.scenario.ships[1].buy_random = 100;

  CHECK_FALSE(game::NovaLanded_CanBuyShip(state, 0x80, 0x81));
  state.scenario.ships[0].contribute_lo = 3;
  state.scenario.ships[1].availability_expr = "b42";
  CHECK_FALSE(game::NovaLanded_CanBuyShip(state, 0x80, 0x81));
  state.control.bits.set(42);
  CHECK(game::NovaLanded_CanBuyShip(state, 0x80, 0x81));

  state.inventory.cargo_bins[0] = 8;
  state.inventory.junk_counts[0] = 8;
  state.player.primary_target_ship_slot = 7;
  state.player.active_weapon_bank_slot = 3;
  REQUIRE(game::NovaLanded_BuyShip(state, 0x80, 0x81, "Peregrine"));
  CHECK(state.player.ship_class_id == 1);
  CHECK(state.player.ship_name == "Peregrine");
  CHECK(state.player.credits == 525); // 25-credit trade-in, then 500 list price
  CHECK(state.player.primary_target_ship_slot == -1);
  CHECK(state.player.active_weapon_bank_slot == -1);
  CHECK(state.inventory.cargo_bins[0] == 5);
  CHECK(state.inventory.junk_counts[0] == 0);
  CHECK(state.ship_class_limit_rolls[1] >= 1);
  CHECK(state.ship_class_limit_rolls[1] <= 100);

  state.player.ship_class_id = 0;
  state.player.ship_name = "Wayfarer";
  state.player.credits = 1'000;
  REQUIRE(game::NovaLanded_BuyShip(state, 0x80, 0x81, ""));
  CHECK(state.player.ship_name.empty());
}

TEST_CASE("Capture ship swap leaves the outgoing player hull operable",
          "[landed_store][capture]") {
  game::GameState state;
  state.scenario.ships.resize(2);
  state.player.is_active = true;
  state.player.ship_class_id = 0;
  state.player.current_system_id = 3;
  state.player.armor_points = 90.0F;
  state.scenario.ships[0].base_armor = 90;
  state.scenario.ships[1].base_armor = 120;

  game::Ship &captured = state.ShipAt(1);
  captured.is_active = true;
  captured.ship_class_id = 1;
  captured.ship_instance_id = 1;
  captured.current_system_id = 3;
  captured.armor_points = 10.0F;

  REQUIRE(game::Player_ReplaceShipWithCapturedHull(state, captured, false));
  CHECK(state.player.ship_class_id == 1);

  const game::Ship *outgoing = nullptr;
  for (std::size_t slot = 1; slot < game::GameState::kMaxShips; ++slot) {
    const game::Ship &candidate = state.ShipAt(slot);
    if (candidate.is_active && candidate.ship_class_id == 0) {
      outgoing = &candidate;
      break;
    }
  }
  REQUIRE(outgoing != nullptr);
  CHECK(outgoing->ai_behavior_code == 6);
  CHECK(outgoing->squad_leader_ship_slot == 0);
  CHECK_FALSE(game::NovaAiShip_IsDisabled(state, *outgoing));
}

namespace {

void SetupCaptureFixture(game::GameState &state) {
  state.scenario.ships.resize(2);
  state.player.is_active = true;
  state.player.ship_class_id = 0;
  state.player.current_system_id = 3;
  state.player.armor_points = 90.0F;
  state.scenario.ships[0].base_armor = 90;
  state.scenario.ships[1].base_armor = 120;
  game::Ship &captured = state.ShipAt(1);
  captured.is_active = true;
  captured.ship_class_id = 1;
  captured.ship_instance_id = 1;
  captured.current_system_id = 3;
  captured.armor_points = 10.0F;
}

const game::Ship *FindActiveHullByClass(const game::GameState &state,
                                        std::int16_t class_id) {
  for (std::size_t slot = 1; slot < game::GameState::kMaxShips; ++slot) {
    const game::Ship &ship = state.ShipAt(slot);
    if (ship.is_active && ship.ship_class_id == class_id) {
      return &ship;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("Capture preserves the player's credits, identity and mission fields",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.player.credits = 12'345;
  state.player.dude_class_id = 7;
  state.player.mission_owner_slot = 3;
  state.player.escort_command_code = 9;
  state.player.ai_state_code = 4;
  state.scenario.pers_defs.resize(2);
  state.player.pers_def_slot = 1;
  state.scenario.pers_defs[1].alive = true;
  state.ShipAt(1).pers_def_slot = 0;
  state.ShipAt(1).credits = 999;
  state.scenario.pers_defs[0].alive = true;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.player.credits == 12'345);
  CHECK(state.player.dude_class_id == 7);
  CHECK(state.player.mission_owner_slot == 3);
  CHECK(state.player.escort_command_code == 9);
  CHECK(state.player.ai_state_code == 4);
  CHECK(state.player.pers_def_slot == 1);
  CHECK_FALSE(state.scenario.pers_defs[0].alive);
}

TEST_CASE("Capture paint uses the captured personality colour",
          "[landed_store][capture][tint]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.ship_paint_rgb5 = {1, 2, 3};
  state.scenario.pers_defs.resize(1);
  state.ShipAt(1).pers_def_slot = 0;
  state.scenario.pers_defs[0].color_r5 = 5;
  state.scenario.pers_defs[0].color_g5 = 11;
  state.scenario.pers_defs[0].color_b5 = 21;
  // A personality color takes priority over the faction government color.
  state.scenario.governments.resize(1);
  state.scenario.governments[0].ship_red = 0x80;
  state.ShipAt(1).faction_or_government_id = 0;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.ship_paint_rgb5 == std::array<std::uint16_t, 3>{5, 11, 21});
  // The player's per-frame tint resolves from that written paint.
  const game::NovaShipTintColor tint =
      game::NovaShip_ResolveTintColor(state, state.player);
  CHECK(tint.red == 5);
  CHECK(tint.green == 11);
  CHECK(tint.blue == 21);
}

TEST_CASE("Capture paint stores the government 8-bit ShipColor << 8",
          "[landed_store][capture][tint]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.scenario.governments.resize(1);
  state.scenario.governments[0].ship_red = 0xff;
  state.scenario.governments[0].ship_green = 0x80;
  state.scenario.governments[0].ship_blue = 0x01;
  state.ShipAt(1).pers_def_slot = -1;
  state.ShipAt(1).faction_or_government_id = 0;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.ship_paint_rgb5 ==
        std::array<std::uint16_t, 3>{0xff00, 0x8000, 0x0100});
}

TEST_CASE("Ship tint falls back to neutral 0x20 when everything is zero",
          "[landed_store][capture][tint]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.ship_paint_rgb5 = {0, 0, 0};
  // The captured hull has no personality and no faction color, so the
  // override resolves all-zero and the resolver substitutes the 0x20 neutral.
  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.ship_paint_rgb5 ==
        std::array<std::uint16_t, 3>{0x20, 0x20, 0x20});
}

TEST_CASE("Player ship tint reads the global paint for slot 0",
          "[landed_store][capture][tint]") {
  game::GameState state;
  state.player.ship_instance_id = 0;
  state.ship_paint_rgb5 = {3, 17, 29};
  const game::NovaShipTintColor tint =
      game::NovaShip_ResolveTintColor(state, state.player);
  CHECK(tint.red == 3);
  CHECK(tint.green == 17);
  CHECK(tint.blue == 29);
}

// 0x00427770 decodes the FIRST ModType-43 slot's 15-bit ModVal into the 5-bit
// global paint, even when the outfit has later paint slots.
TEST_CASE("Paint outfit decodes only the first ModType-43 slot",
          "[landed_store][tint]") {
  game::GameState state;
  state.scenario.outfits.resize(1);
  state.inventory.outfit_owned_count.fill(0);
  game::Outfit &outfit = state.scenario.outfits[0];
  const auto kPaint = static_cast<std::int16_t>(game::OutfitEffect::kPaint);
  outfit.mod_type = kPaint;
  outfit.mod_val = static_cast<std::int16_t>((0x1f << 10) | (0 << 5) | 0);
  outfit.alt_mod_types = {kPaint, 0, 0};
  outfit.alt_mod_vals = {static_cast<std::int16_t>(0 | (0 << 5) | 0x1f), 0, 0};

  REQUIRE(game::NovaOutfit_GrantOutfitToPlayer(state, 0));
  CHECK(state.ship_paint_rgb5 == std::array<std::uint16_t, 3>{0x1f, 0, 0});
}

TEST_CASE("Capture releases only followers of the captured hull",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.scenario.ships[1].default_ai_behavior = 4;
  game::Ship &captured_follower = state.ShipAt(2);
  captured_follower.is_active = true;
  captured_follower.ship_class_id = 1;
  captured_follower.ship_instance_id = 2;
  captured_follower.current_system_id = 3;
  captured_follower.squad_leader_ship_slot = 1;
  captured_follower.armor_points = 10.0F;
  game::Ship &player_follower = state.ShipAt(3);
  player_follower.is_active = true;
  player_follower.ship_class_id = 1;
  player_follower.ship_instance_id = 3;
  player_follower.current_system_id = 3;
  player_follower.squad_leader_ship_slot = 0;
  player_follower.armor_points = 10.0F;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(captured_follower.squad_leader_ship_slot == -1);
  CHECK(captured_follower.ai_behavior_code == 4);
  CHECK(player_follower.squad_leader_ship_slot == 0);
}

TEST_CASE("Capture truncates both hull headings from radians to degrees",
          "[landed_store][capture]") {
  constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
  game::GameState state;
  SetupCaptureFixture(state);
  state.player.heading = 1.5F * kDegToRad;
  state.ShipAt(1).heading = 90.0F * kDegToRad;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.player.ai_desired_heading_deg == 90);
  const game::Ship *outgoing = FindActiveHullByClass(state, 0);
  REQUIRE(outgoing != nullptr);
  CHECK(outgoing->ai_desired_heading_deg == 1);
}

TEST_CASE("Undamaged capture swaps cargo between the two hulls",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.inventory.cargo_bins[0] = 25;
  state.ShipAt(1).cargo_bins[0] = 7;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.inventory.cargo_bins[0] == 7);
  const game::Ship *outgoing = FindActiveHullByClass(state, 0);
  REQUIRE(outgoing != nullptr);
  CHECK(outgoing->cargo_bins[0] == 25);
}

TEST_CASE("Damaged capture scales player cargo and junk into the outgoing hull",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.scenario.ships[0].cargo_holds = 40;
  state.scenario.ships[1].cargo_holds = 40;
  state.scenario.ships[1].default_ai_behavior = 1;
  state.inventory.cargo_bins[0] = 100;
  state.inventory.junk_counts[0] = 100;
  // Denominator 40 (player) + 40 (escort slot 2) + 40 (the freshly allocated
  // outgoing replacement itself, which is already active with leader 0 and
  // behavior 6 — the original's inline denominator counts it too); recipient
  // capacity 40, so ratio 1/3 and a third of each holding transfers.
  game::Ship &escort = state.ShipAt(2);
  escort.is_active = true;
  escort.ship_class_id = 1;
  escort.ship_instance_id = 2;
  escort.current_system_id = 3;
  escort.squad_leader_ship_slot = 0;
  escort.ai_behavior_code = 6;
  escort.mission_fleet_slot = -1;
  escort.armor_points = 10.0F;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), true));
  CHECK(state.inventory.cargo_bins[0] == 0);
  CHECK(state.inventory.junk_counts[0] == 67);
  const game::Ship *outgoing = FindActiveHullByClass(state, 0);
  REQUIRE(outgoing != nullptr);
  CHECK(outgoing->cargo_bins[0] == 33);
  CHECK(outgoing->squad_leader_ship_slot == -1);
  CHECK(outgoing->ai_behavior_code == -1);
  CHECK(state.player.squad_leader_ship_slot == -1);
  CHECK(state.player.ai_behavior_code == -1);
}

TEST_CASE("Capture merges the captured hull's carried weapons and ammo",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.scenario.weapons.resize(0x100);
  state.scenario.outfits.resize(8);
  state.scenario.outfits[5].mod_type = 1; // weapon bank 0
  state.scenario.outfits[5].mod_val = 0;
  state.scenario.outfits[6].mod_type = 3; // ammo bank 0x10
  state.scenario.outfits[6].mod_val = 0x10;
  state.ShipAt(1).npc_weapon_count_by_class[0] = 2;
  state.ShipAt(1).npc_weapon_secondary_count_by_class[0] = 5;
  state.ShipAt(1).npc_weapon_banks_ship_class = 1;
  state.scenario.weapons[0].ammo_type = 0x10;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.weapon_count_by_class[0] == 2);
  CHECK(state.weapon_secondary_count_by_class[0x10 * 100] == 5);
}

TEST_CASE("Damaged-capture secondary copy tests the source bank, not the ammo "
          "bank",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.scenario.weapons.resize(0x100);
  state.scenario.outfits.resize(8);
  // Persistent weapon outfit keeps player bank 0's secondary at 1, so the
  // captured secondary must NOT be copied to the remapped ammo bank.
  state.scenario.outfits[5].mod_type = 1;
  state.scenario.outfits[5].mod_val = 0;
  state.scenario.outfits[5].persistent_on_ship_swap = true;
  state.inventory.outfit_owned_count[5] = 1;
  state.weapon_count_by_class[0] = 1;
  state.weapon_secondary_count_by_class[0] = 1;
  state.ShipAt(1).npc_weapon_secondary_count_by_class[0] = 5;
  state.ShipAt(1).npc_weapon_banks_ship_class = 1;
  state.scenario.weapons[0].ammo_type = 0x10;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.weapon_secondary_count_by_class[0x10 * 100] == 0);
}

TEST_CASE("Capture keeps a mode-99 captured secondary in its own bank",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.scenario.weapons.resize(0x100);
  state.scenario.outfits.resize(8);
  // Mode-99 bay ammo is backed by an ammo outfit at the same bank, so the
  // merge's leftover materializes and the rebuild restores it.
  state.scenario.outfits[6].mod_type = 3;
  state.scenario.outfits[6].mod_val = 0;
  state.scenario.weapons[0].weapon_mode_code = 99;
  state.scenario.weapons[0].ammo_type = 0x10; // ignored for mode 99
  state.ShipAt(1).npc_weapon_secondary_count_by_class[0] = 5;
  state.ShipAt(1).npc_weapon_banks_ship_class = 1;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.weapon_secondary_count_by_class[0] == 5);
  CHECK(state.weapon_secondary_count_by_class[0x10 * 100] == 0);
}

TEST_CASE("Captured secondary remap overwrites a non-empty destination bank",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.scenario.weapons.resize(0x100);
  state.scenario.outfits.resize(8);
  // Persistent weapon bank 0 keeps its player secondary across the clear.
  state.scenario.outfits[5].mod_type = 1;
  state.scenario.outfits[5].mod_val = 0;
  state.scenario.outfits[5].persistent_on_ship_swap = true;
  state.inventory.outfit_owned_count[5] = 1;
  state.weapon_count_by_class[0] = 1;
  state.weapon_secondary_count_by_class[0] = 1;
  // An ammo outfit at destination bank 0 lets the merge's leftover survive.
  state.scenario.outfits[6].mod_type = 3;
  state.scenario.outfits[6].mod_val = 0;
  // Captured source bank 1 remaps onto destination ammo bank 0. The original
  // gates on the SOURCE bank being empty and then writes the destination
  // unconditionally, overwriting the persistent round (5, not 1+5=6).
  state.ShipAt(1).npc_weapon_secondary_count_by_class[1] = 5;
  state.ShipAt(1).npc_weapon_banks_ship_class = 1;
  state.scenario.weapons[1].ammo_type = 0;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.weapon_secondary_count_by_class[0] == 5);
}

TEST_CASE("Outgoing hull NPC banks seed from the old class stock weapons",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.scenario.ships[0].stock_weapons[0] =
      game::ShipDefaultWeaponBank{0x80, 3, 7};

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  const game::Ship *outgoing = FindActiveHullByClass(state, 0);
  REQUIRE(outgoing != nullptr);
  CHECK(outgoing->npc_weapon_count_by_class[0] == 3);
  CHECK(outgoing->npc_weapon_secondary_count_by_class[0] == 7);
  CHECK(outgoing->npc_weapon_banks_ship_class == 0);
}

TEST_CASE("Replacement hull derives afterburner, mining and voice fields",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  // Old class: always-afterburner flag, inherent-government voice override and
  // a built-in mining-scoop default outfit.
  state.scenario.ships[0].capability_flags = 0x40;
  state.scenario.ships[0].inherent_attributes_govt = 0;
  state.scenario.ships[0].default_outfit_ids[0] = 0x80;
  state.scenario.ships[0].default_outfit_counts[0] = 1;
  state.scenario.governments.resize(1);
  state.scenario.governments[0].voice_type_mode = 1;
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].mod_type = 0x1f;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  const game::Ship *outgoing = FindActiveHullByClass(state, 0);
  REQUIRE(outgoing != nullptr);
  CHECK(outgoing->afterburner_latch == 1);
  CHECK(outgoing->mining_scoop_active == 1);
  CHECK(outgoing->voice_type_mode == 1);
}

TEST_CASE("Outgoing hull armor repair uses the outgoing class's flags",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  // Old (outgoing) hull is a 0x10-flag hull: repair fraction 0.1. The captured
  // hull has no such flag, so using it would give 0.3333 instead.
  state.scenario.ships[0].capability_flags = 0x10;
  state.player.armor_points = 0.0F;

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  const game::Ship *outgoing = FindActiveHullByClass(state, 0);
  REQUIRE(outgoing != nullptr);
  CHECK(outgoing->armor_points == Catch::Approx(90.0F * 0.1F + 1.0F));
}

TEST_CASE("Capture runs OnRetire then OnCapture reaction scripts",
          "[landed_store][capture]") {
  game::GameState state;
  SetupCaptureFixture(state);
  state.scenario.ships[0].on_retire_expr = "B100";
  state.scenario.ships[1].on_capture_expr = "B101";

  REQUIRE(
      game::Player_ReplaceShipWithCapturedHull(state, state.ShipAt(1), false));
  CHECK(state.control.ControlBit(100));
  CHECK(state.control.ControlBit(101));
}

TEST_CASE("Cargo transfer uses the unclamped same-system fleet capacity",
          "[landed_store][shipyard]") {
  game::GameState state;
  state.scenario.ships.resize(2);
  state.player.ship_class_id = 0;
  state.player.current_system_id = 3;
  state.scenario.ships[0].cargo_holds = 20'000;
  state.scenario.ships[1].cargo_holds = 400;
  state.scenario.ships[1].default_ai_behavior = 0;

  for (std::size_t slot = 1; slot <= 50; ++slot) {
    game::Ship &escort = state.ShipAt(slot);
    escort.is_active = true;
    escort.armor_points = 1.0F;
    escort.ship_class_id = 1;
    escort.ship_instance_id = static_cast<std::int16_t>(slot);
    escort.current_system_id = 3;
    escort.squad_leader_ship_slot = 0;
    escort.ai_behavior_code = 6;
    escort.mission_fleet_slot = -1;
  }
  // This otherwise-eligible escort is outside the player's system and is not
  // part of 0x00469810's denominator.
  state.ShipAt(50).current_system_id = 4;
  state.inventory.cargo_bins[0] = 100;
  state.inventory.junk_counts[0] = 100;

  game::Player_TransferCargoAndJunkToEscortByRatio(state, 0);

  // 20,000 / (20,000 + 49 * 400) = 0.50505..., truncated per item.
  CHECK(state.inventory.cargo_bins[0] == 50);
  CHECK(state.inventory.junk_counts[0] == 50);
}

// The docked landing-description panel is word-wrapped. The wrap must NOT
// "cumulatively append": each new line must start fresh (with only its own
// words), never re-embed the already-drained earlier line. Regression for a
// bug where a wrapping line re-appended the drained text onto the next line,
// making every subsequent line repeat all prior words.
TEST_CASE("landing description word-wrap produces distinct lines",
          "[landed_window]") {
  using game::WrapDescriptionLines;
  auto width = [](std::string_view s) { return static_cast<int>(s.size()); };

  const std::string text =
      "The warriors on this station hold themselves ready to deal with any "
      "large-scale threat to the sovereignty of the Polaris.";
  const auto lines = WrapDescriptionLines(text, 40, width);
  REQUIRE(lines.size() >= 3);

  // Each wrapped line fits the row (except a single-word overflow, not the
  // case here) and no line is a substring of a later line.
  for (std::size_t i = 0; i < lines.size(); ++i) {
    CHECK(static_cast<int>(lines[i].size()) <= 40);
    for (std::size_t j = i + 1; j < lines.size(); ++j) {
      CHECK_FALSE(lines[j].starts_with(lines[i]));
      CHECK(lines[j].find(lines[i]) == std::string::npos);
    }
  }
  // Rejoining the wrapped lines with a single separating space must reproduce
  // the original text exactly (the wrap only ever drops the whitespace between
  // a line's last word and the next, never any word characters).
  {
    std::string rejoined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i != 0) {
        rejoined.push_back(' ');
      }
      rejoined += lines[i];
    }
    CHECK(rejoined == text);
  }

  // A single over-wide word is emitted on its own (unsplit) line.
  const auto long_word = WrapDescriptionLines("supercalifragilistic", 4, width);
  REQUIRE(long_word.size() == 1);
  CHECK(long_word[0] == "supercalifragilistic");
}

// Desc resources separate the "Requires: ..." paragraph with a double Mac
// return; the original DrawTextW(DT_WORDBREAK) renders that as a blank line and
// the port must not collapse it. A single return still just ends the line.
TEST_CASE("description wrap preserves explicit blank lines",
          "[landed_window]") {
  using game::WrapDescriptionLines;
  auto width = [](std::string_view s) { return static_cast<int>(s.size()); };

  const auto lines = WrapDescriptionLines(
      "The drive hums.\r\rRequires: Heavy Weapons License", 200, width);
  REQUIRE(lines.size() == 3);
  CHECK(lines[0] == "The drive hums.");
  CHECK(lines[1].empty());
  CHECK(lines[2] == "Requires: Heavy Weapons License");

  const auto single = WrapDescriptionLines("one\rtwo", 200, width);
  REQUIRE(single.size() == 2);
  CHECK(single[0] == "one");
  CHECK(single[1] == "two");
}

// Stellar_HandleStellarEntryAndExit's normal-arrival envelope is not the
// starmap travel-arm 250 (0x00459369); it is the target's spin-sprite span
// scaled by 1.75 (0x004587a3), with a 0x4b fallback when no sprite is prepared
// (0x00457f89).
TEST_CASE("landing arrival envelope follows the sprite span",
          "[landed_window]") {
  // No prepared sprite -> the original 0x4b fallback.
  CHECK(game::Stellar_MaxLandingDistance(0) == 75.0F);
  // round(height * 1.75), FIST/round-half-to-even semantics.
  CHECK(game::Stellar_MaxLandingDistance(32) == 56.0F);
  CHECK(game::Stellar_MaxLandingDistance(96) == 168.0F);
  CHECK(game::Stellar_MaxLandingDistance(150) == 262.0F);
}

// Regression: the gate must reject a target in the gap between the original
// 0x4b fallback and the old hardcoded 250 envelope when no sprite is prepared.
TEST_CASE("landing rejects a stellar outside the no-sprite envelope",
          "[landed_window]") {
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Testport";
  stellar.pos_x = 0;
  stellar.pos_y = 0;
  stellar.flags = 0x1;
  stellar.is_available = true;
  stellar.system_id = 0;
  state.player.current_system_id = 0;
  state.player.pos_x = 120.0F; // > 75 (no-sprite), < 250 (old port limit)
  state.player.pos_y = 0.0F;
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = 0x2ee; // approach armed

  game::LandedContext ctx;
  CHECK_FALSE(game::Stellar_Dock(state, ctx, 0));
  CHECK(ctx.denial == game::LandedDenial::kTooFar);
  // A prepared sprite whose depth spans the ship keeps the arrival in range.
  CHECK(game::Stellar_Dock(state, ctx, 96));
}

TEST_CASE("landing rejects a moving ship within range", "[landed_window]") {
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Testport";
  stellar.pos_x = 0;
  stellar.pos_y = 0;
  stellar.flags = 0x1;
  stellar.is_available = true;
  stellar.system_id = 0;
  state.player.current_system_id = 0;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.player.vel_x = 2.0F; // > 0.75, outside the docking envelope
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = 0x2ee;

  game::LandedContext ctx;
  CHECK_FALSE(game::Stellar_Dock(state, ctx, 96));
  CHECK(ctx.denial == game::LandedDenial::kTooFast);

  // Stopping the ship clears the last gate.
  state.player.vel_x = 0.0F;
  CHECK(game::Stellar_Dock(state, ctx, 96));
}

TEST_CASE("landing reports authorization denial before distance",
          "[landed_window]") {
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Restricted station";
  stellar.pos_x = 0;
  stellar.pos_y = 0;
  stellar.flags = 0x11; // targetable + station
  stellar.is_available = true;
  stellar.system_id = 0;
  stellar.government_id = 0;
  stellar.min_status = 100;
  state.scenario.governments.resize(1);
  state.player.current_system_id = 0;
  state.player.pos_x = 1'000.0F; // distance must not mask the permission result
  state.system_reputation.resize(1);
  state.system_reputation[0] = 0;
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = 0;

  game::LandedContext ctx;
  CHECK_FALSE(game::Stellar_Dock(state, ctx, 96));
  CHECK(ctx.denial == game::LandedDenial::kUnauthorized);

  state.system_reputation[0] = 100;
  CHECK_FALSE(game::Stellar_Dock(state, ctx, 96));
  CHECK(ctx.denial == game::LandedDenial::kTooFar);
}

TEST_CASE("landing approach timer arms within 250 and expires", "[travel]") {
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Testport";
  stellar.pos_x = 0;
  stellar.pos_y = 0;
  stellar.flags = 0x1;
  stellar.is_available = true;
  stellar.system_id = 0;
  state.player.current_system_id = 0;
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = -1;

  // Far away: initialises the timer but does not arm it (the original's
  // request radius is 0xfa on both axes).
  state.player.pos_x = 400.0F;
  game::NovaTravel_UpdateEngagementProgress(state);
  CHECK(state.travel.engage_timer == 0);

  // Within 250 on both axes: arms the request.
  state.player.pos_x = 100.0F;
  game::NovaTravel_UpdateEngagementProgress(state);
  CHECK(state.travel.engage_timer == 0x2ee);
  REQUIRE(state.pending_ui_sounds.size() == 1);
  CHECK(state.pending_ui_sounds.front().transition_index == 1);
  CHECK(state.pending_ui_sounds.front().priority_width == 1);

  // The cue is edge-triggered; the armed timer advances on the next tick.
  game::NovaTravel_UpdateEngagementProgress(state);
  CHECK(state.pending_ui_sounds.size() == 1);

  // Past 0x7ff the request expires and the selection clears.
  state.travel.engage_timer = 0x800;
  game::NovaTravel_UpdateEngagementProgress(state);
  CHECK(state.travel.engage_timer == -1);
  CHECK(state.travel.selected_stellar_id == -1);
}

TEST_CASE("normal landing arrival charges once; launch restores the ship",
          "[landed_window]") {
  // SDL-free core of Stellar_RunDockAndLaunchSequence's normal-arrival
  // bookkeeping. The target is deliberately an inactive (not currently
  // rendered) stellar: StellarTargetsSpriteSetActive accepts the matching
  // inactive state unless the stellar's 0x80 engaged bit is set.
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Testport";
  stellar.pos_x = 123;
  stellar.pos_y = -456;
  stellar.flags = 0x1;
  stellar.is_available = true;
  stellar.system_id = 0;
  stellar.service_cost = 75;
  state.scenario.ships.resize(1);
  state.scenario.ships[0].base_shield = 300;
  state.scenario.ships[0].base_armor = 250;

  state.player.current_system_id = 0;
  state.player.ship_class_id = 0;
  state.player.credits = 100;
  state.player.pos_x = 3.0F;
  state.player.pos_y = -300.0F;
  state.player.vel_x = 0.5F;
  state.player.vel_y = -0.5F;
  state.player.speed = 4.0F;
  state.player.shield_points = 1.0F;
  state.player.armor_points = 2.0F;
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = 0x2ee; // approach armed
  // A ship target held across the landing must be dropped by the launch
  // cleanup before the vacant-ship sweep recycles NPC slots
  // (Stellar_HandleStellarEntryAndExit 0x00458304).
  state.player.primary_target_ship_slot = 1;
  state.ship_reticle_pulse = 128.0F;

  // A healthy attached escort must survive the launch rebuild and be adopted
  // into the destination system before mission/ambient population is added.
  game::Ship &escort = state.ShipAt(1);
  escort.is_active = true;
  escort.ship_instance_id = 1;
  escort.ship_class_id = 0;
  escort.ai_behavior_code = 6;
  escort.squad_leader_ship_slot = 0;
  escort.mission_fleet_slot = -1;
  escort.defense_fleet_home_stellar_id = -1;
  escort.armor_points = 200.0F;
  escort.shield_points = 2.0F;
  game::Ship &disabled = state.ShipAt(2);
  disabled = escort;
  disabled.ship_instance_id = 2;
  disabled.armor_points = 1.0F;

  // A live resource-box (mined asteroid yield) and a destruction fragment
  // must be wiped by the landing entry: 0x00458186 raises the transition-frame
  // clear latches alongside g_no_asteroids_latch, which the port performs
  // synchronously in Stellar_Dock.
  state.freeflight_objects[0].lifetime_ticks = 400.0F;
  state.freeflight_objects[0].system_id = 0;
  state.freeflight_objects[0].extra = 2;
  state.freeflight_objects[0].persistent = true;
  state.fading_effect_instances[0].lifetime_ticks = 200.0F;

  game::LandedContext ctx;
  REQUIRE(game::Stellar_Dock(state, ctx, 96));
  CHECK(ctx.landed);
  CHECK(ctx.stellar_id == 0x80);
  CHECK(state.travel.landed_this_frame);
  CHECK(state.player.credits == 25);
  CHECK(state.freeflight_objects[0].lifetime_ticks < 0.0F);
  CHECK(state.fading_effect_instances[0].lifetime_ticks < 0.0F);
  // Arrival does NOT touch the ship's meters or kinematics: the original
  // leaves them to the launch tail, after the interaction loop returns
  // (Stellar_RunDockAndLaunchSequence 0x00455f99..0x0045602f).
  CHECK(state.player.pos_x == 3.0F);
  CHECK(state.player.pos_y == -300.0F);
  CHECK(state.player.shield_points == 1.0F);
  CHECK(state.player.armor_points == 2.0F);

  // Launch tail (0x00455f99..0x00456268): reposition + velocity kill,
  // shield/armor refill to the effective maxima, daily world tick.
  game::Stellar_Launch(state);
  CHECK(state.player.pos_x == 123.0F);
  CHECK(state.player.pos_y == -456.0F);
  CHECK(state.player.vel_x == 0.0F);
  CHECK(state.player.vel_y == 0.0F);
  CHECK(state.player.speed == 0.0F);
  CHECK(state.player.shield_points == 300.0F);
  CHECK(state.player.armor_points == 250.0F);
  CHECK(escort.is_active);
  CHECK(escort.current_system_id == state.player.current_system_id);
  CHECK(escort.heading == state.player.heading);
  CHECK(escort.shield_points == 300.0F);
  CHECK(escort.armor_points == 250.0F);
  CHECK_FALSE(disabled.is_active);
  // 0x00456109: the launch heading is a fresh rand(0x168) roll.
  CHECK(state.player.heading >= 0.0F);
  CHECK(state.player.heading < 6.2831855F);
  // 0x00456158: the travel selection resets on launch.
  CHECK(state.travel.selected_stellar_id == -1);
  // 0x00458304: the post-launch cleanup also clears the primary ship target
  // before the NPC rebuild can reuse the slot.
  CHECK(state.player.primary_target_ship_slot == -1);
  CHECK(state.ship_reticle_pulse == 0.0F);
  // 0x00456128: the launch tail swallows the held launch/cancel command edge
  // latches so the key that left the dock must be released before it re-fires.
  CHECK(state.command_latches.return_to_menu_was_held);
  CHECK(state.command_latches.land_was_held);
}

// Ghidra 0x0045c7a0 NovaUi_MarkTravelAndStatusPanelsDirty: arms every player
// command edge latch so a key held across a modal/mode boundary must be
// released and re-pressed. Regression for the dock-launch bounce (holding Esc
// undocked, then the still-held key immediately re-fired the menu exit).
TEST_CASE("MarkTravelAndStatusPanelsDirty swallows held command latches",
          "[spaceflight][input]") {
  game::GameState state;
  state.command_latches.land_was_held = false;
  state.command_latches.return_to_menu_was_held = false;
  state.cloak_command_latch = 0;
  state.route_map.zoom_command_latch = false;

  game::NovaUi_MarkTravelAndStatusPanelsDirty(state);

  CHECK(state.command_latches.target_cycle_was_held);
  CHECK(state.command_latches.destination_cycle_was_held);
  CHECK(state.command_latches.clear_target_was_held);
  CHECK(state.command_latches.ship_cycle_was_held);
  CHECK(state.command_latches.nearest_target_was_held);
  CHECK(state.command_latches.secondary_cycle_was_held);
  CHECK(state.command_latches.clear_secondary_was_held);
  CHECK(state.command_latches.starmap_was_held);
  CHECK(state.command_latches.mission_info_was_held);
  CHECK(state.command_latches.land_was_held);
  CHECK(state.command_latches.dismiss_was_held);
  CHECK(state.command_latches.target_action_was_held);
  CHECK(state.command_latches.board_was_held);
  CHECK(state.command_latches.return_to_menu_was_held);
  CHECK(state.cloak_command_latch == 1);
  CHECK(state.route_map.zoom_command_latch);
  for (const std::uint8_t latch : state.escort.select_key_latch) {
    CHECK(latch == 1);
  }
  for (const std::uint8_t latch : state.escort.order_key_latch) {
    CHECK(latch == 1);
  }
  CHECK(state.escort.panel_toggle_latch == 1);
}

// NovaUi_RunOutfitterInteractionLoop (0x0048ea70) clears the active weapon
// bank when the last unit of a mounted weapon outfit is sold, so a stale bank
// index can't point at an outfit the player no longer owns.
TEST_CASE("Outfitter sale clears the active weapon bank on the last unit",
          "[landed_store][outfitter]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // the starter Shuttle
  state.player.credits = 100'000;

  const game::Outfit *blaster = state.scenario.Outfit(0x80);
  REQUIRE(blaster != nullptr);
  REQUIRE(blaster->mod_type == 1); // a weapon outfit

  game::LandedStoreSession session =
      game::NovaLanded_OpenOutfitterSession(state, 0x80);
  REQUIRE(game::NovaLanded_BuyOutfit(state, 0x80, 0x80, 1) == 1);
  CHECK(state.inventory.outfit_owned_count[0] == 1);

  state.player.active_weapon_bank_slot = blaster->mod_val;
  const game::OutfitSaleResult sale =
      game::NovaLanded_SellOutfit(state, session, 0x80, 0x80, 1);
  CHECK(sale.sold == 1);
  CHECK(sale.block == game::OutfitSaleBlock::kNone);
  CHECK(state.inventory.outfit_owned_count[0] == 0);
  CHECK(state.player.active_weapon_bank_slot == -1);
}

TEST_CASE("launch rebuild includes missions accepted while docked") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  constexpr std::int16_t kRautherionSystem = 166 - 0x80;
  constexpr std::int16_t kRautherStellar = 191;
  constexpr std::int16_t kTutorial006Index = 754 - 0x80;
  constexpr std::int16_t kTutorialDerelictPers = 642 - 0x80;
  constexpr std::int16_t kPirateViperShipClass = 166 - 0x80;
  constexpr std::int16_t kDerelictsGovernment = 160 - 0x80;
  state.player.current_system_id = kRautherionSystem;
  state.player.ship_class_id = 0;

  REQUIRE(
      game::Mission_ActivateAtSlot(state, kTutorial006Index, kRautherStellar));
  REQUIRE(state.active_mission_runtime_flags[0].is_active);
  REQUIRE(state.active_mission_runtime_flags[1].is_active);
  REQUIRE(state.control.bits.test(9208));
  for (std::size_t slot = 1; slot < game::GameState::kMaxShips; ++slot) {
    CHECK_FALSE(state.ShipAt(slot).is_active);
  }

  game::Stellar_Launch(state);

  bool found_derelict = false;
  for (std::size_t slot = 1; slot < game::GameState::kMaxShips; ++slot) {
    const auto &ship = state.ShipAt(slot);
    if (ship.is_active && ship.pers_def_slot == kTutorialDerelictPers) {
      found_derelict = true;
      CHECK(ship.current_system_id == kRautherionSystem);
      CHECK(ship.ship_class_id == kPirateViperShipClass);
      CHECK(ship.faction_or_government_id == kDerelictsGovernment);
      CHECK(ship.mission_fleet_slot == -1);
    }
  }
  CHECK(found_derelict);
}

// Same as the test above, but with the arrival population that the real
// journey runs before the player lands and accepts Tutorial 006. The bit is
// still clear on arrival, so the derelict must be suppressed then, and the
// launch rebuild must spawn it once the mission accept sets b9208.
TEST_CASE("derelict spawns at launch after a pre-accept arrival population") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  constexpr std::int16_t kRautherionSystem = 166 - 0x80;
  constexpr std::int16_t kRautherStellar = 191;
  constexpr std::int16_t kTutorial006Index = 754 - 0x80;
  constexpr std::int16_t kTutorialDerelictPers = 642 - 0x80;
  state.player.current_system_id = kRautherionSystem;
  state.player.ship_class_id = 0;

  // System arrival with b9208 clear: no derelict yet.
  REQUIRE_FALSE(state.control.bits.test(9208));
  game::NovaSystem_PopulateInitialNpcShips(state, kRautherionSystem);
  for (std::size_t slot = 1; slot < game::GameState::kMaxShips; ++slot) {
    CHECK(state.ShipAt(slot).pers_def_slot != kTutorialDerelictPers);
  }

  // Accept Tutorial 006 while docked; the payload sets b9208 and activates
  // the silent companion mission.
  REQUIRE(
      game::Mission_ActivateAtSlot(state, kTutorial006Index, kRautherStellar));
  REQUIRE(state.control.bits.test(9208));

  game::Stellar_Launch(state);

  bool found_derelict = false;
  for (std::size_t slot = 1; slot < game::GameState::kMaxShips; ++slot) {
    const auto &ship = state.ShipAt(slot);
    if (ship.is_active && ship.pers_def_slot == kTutorialDerelictPers) {
      found_derelict = true;
    }
  }
  CHECK(found_derelict);
}

// DAT_007d4c0d (0x0048ea70): a stellar with a zero TechLevel and no positive
// SpecialTech has nothing to sell, which disables the Buy button.
TEST_CASE("stellar outfit availability follows tech level",
          "[landed_store][outfitter]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  int checked = 0;
  for (std::int16_t id = 0x80; id < 0x100; ++id) {
    const game::Stellar *stellar = state.scenario.Stellar(id);
    if (stellar == nullptr)
      continue;
    ++checked;
    bool expected = stellar->tech_level != 0;
    for (const std::int16_t tech : stellar->special_tech) {
      if (tech > 0) {
        expected = true;
        break;
      }
    }
    CHECK(game::NovaLanded_StellarSellsOutfits(state, id) == expected);
  }
  CHECK(checked > 0);
}

// NovaLanded_CanBuyOutfit (0x00491950) mode-99 bay arm: buying a
// carried-ship outfit requires a mounted bay with room for one more craft.
// The Viper (0x9e) is bound to weapon bank 21; its bay weapon is "Viper Bay"
// (0x9d, MaxAmmo 4) and its Require needs the Fighter Bay License (0x103).
TEST_CASE("Outfitter gate enforces fighter-bay capacity",
          "[landed_store][outfitter]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // the starter Shuttle
  state.player.credits = 5'000'000;

  const game::Outfit *viper = state.scenario.Outfit(0x9e);
  REQUIRE(viper != nullptr);
  REQUIRE(viper->mod_type ==
          static_cast<std::int16_t>(game::OutfitEffect::kAmmo));
  REQUIRE(viper->mod_val == 21);
  // The stock Viper carries a system/planet Availability expression; clear it
  // to isolate the bay-capacity gate under test.
  auto *mutable_viper = const_cast<game::Outfit *>(viper);
  const std::string saved_availability = mutable_viper->availability_expr;
  mutable_viper->availability_expr.clear();

  // Without the bay weapon mounted there is no holding capacity.
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x9e));

  // Grant the Fighter Bay License (require_lo bit 2) and mount the Viper Bay
  // directly, bypassing the ownership clamps (the license is a fixed grant).
  state.inventory.outfit_owned_count[0x103 - 0x80] = 1;
  state.inventory.outfit_owned_count[0x9d - 0x80] = 1;
  game::NovaWeapon_RebuildBanksFromOwnedOutfits(state);
  state.inventory.outfit_owned_count[0x9e - 0x80] = 0;
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x9e));

  // With the bay full (one mount x MaxAmmo 4) the gate closes.
  state.inventory.outfit_owned_count[0x9e - 0x80] = 4;
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x9e));

  mutable_viper->availability_expr = saved_availability;
}

// NovaLanded_CanBuyOutfit (0x00491950) negative-cargo arm: an outfit that
// trades cargo space away (ModType 2 with ModVal < 0) is purchasable only when
// the player hull's own free cargo covers the reduction. Mass Expansion (0xbe,
// ModVal -15) and Mass Retool (0xc0, ModVal -12) are the stock cases; their
// purchase mass is negative, so the free-mass gate deliberately skips them.
TEST_CASE("Outfitter gate enforces free cargo for negative cargo mods",
          "[landed_store][outfitter][cargo]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // the starter Shuttle, Holds 10
  state.player.credits = 5'000'000;
  state.inventory.cargo_bins.fill(0);
  state.inventory.junk_counts.fill(0);

  const game::Outfit *mass_expansion = state.scenario.Outfit(0xbe);
  const game::Outfit *mass_retool = state.scenario.Outfit(0xc0);
  REQUIRE(mass_expansion != nullptr);
  REQUIRE(mass_retool != nullptr);
  REQUIRE(mass_expansion->mod_type ==
          static_cast<std::int16_t>(game::OutfitEffect::kCargoSpace));
  REQUIRE(mass_expansion->mod_val == -15);
  REQUIRE(mass_retool->mod_val == -12);

  // Shuttle Holds 10: the 15-ton Mass Expansion does not fit, and neither
  // does the cheaper 12-ton Mass Retool once cargo is aboard.
  CHECK(game::Ship_ComputeShipTotalCargoCapacity(state) == 10);
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xbe));
  state.inventory.cargo_bins[0] = 1; // free cargo 9
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xc0));
  state.inventory.cargo_bins[0] = 0;

  // A Cargo Expansion adds 10 holds (capacity 20); the Mass Expansion now fits
  // while the 12-ton Mass Retool still does, and a 15-ton load closes both.
  state.inventory.outfit_owned_count[0xbd - 0x80] = 1;
  CHECK(game::Ship_ComputeShipTotalCargoCapacity(state) == 20);
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xbe));
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xc0));
  state.inventory.cargo_bins[0] = 5; // free cargo 15, exactly the Mass Exp cost
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xbe));
  state.inventory.cargo_bins[0] = 6; // free cargo 14 < 15
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xbe));
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xc0)); // 14 >= 12
  state.inventory.cargo_bins[0] = 9;                       // free cargo 11 < 12
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xc0));
  state.inventory.cargo_bins[0] = 0;

  // The +0xa40 flag (ShipClass::allows_mass_expansions) closes the arm even
  // with ample free cargo: a negative raw Holds clears it in the loader.
  auto *shuttle = const_cast<game::ShipClass *>(state.scenario.Ship(0x80));
  REQUIRE(shuttle != nullptr);
  shuttle->allows_mass_expansions = false;
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xbe));
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xc0));
  shuttle->allows_mass_expansions = true;
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0xbe));
}

// NovaLanded_CanBuyOutfit (0x00491950) step 5: RequireGovt scopes an
// outfit's Require bits to one of four government-keyed outfit-id bands. The
// helper is a direct port of the decompiled band chain.
TEST_CASE("RequireGovt scopes outfit requirements to government bands",
          "[landed_store][outfitter][government]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  using game::NovaLanded_RequireGovtAllows;

  // -1 and any out-of-band value apply in every shop.
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, -1, 0));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, -1, -1));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x7f, 0));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x1234, 7));

  // Locate a government allied with 0 (Federation) and one that is not, so the
  // ally branches are exercised against the shipped relations.
  std::int16_t ally = -1;
  std::int16_t foe = -1;
  for (std::int16_t g = 1;
       g < static_cast<std::int16_t>(state.scenario.governments.size());
       ++g) {
    if (game::NovaGovernment_AreGovtsAllied(state.scenario, 0, g)) {
      if (ally == -1)
        ally = g;
    } else if (foe == -1) {
      foe = g;
    }
  }
  REQUIRE(foe != -1);

  // 0x80..0x17f licenses: own govt or an ally only; independent (govt -1) is
  // denied. Target govt = 0 -> require_govt 0x80.
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x80, 0));
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0x80, -1));
  if (ally != -1)
    CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x80, ally));
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0x80, foe));

  // 0x468..0x567 conditional: own govt, independent, or an ally.
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x468, 0));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x468, -1));
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0x468, foe));

  // 0x850..0x94f contraband: anything except own govt or an ally.
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0x850, 0));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x850, -1));
  if (ally != -1)
    CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0x850, ally));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x850, foe));

  // 0xc38..0xd37 strict contraband: denied for own govt, independent, or ally.
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0xc38, 0));
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0xc38, -1));
  if (ally != -1)
    CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0xc38, ally));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0xc38, foe));
}

// Integration through the real purchase gate: Earth (0x80) belongs to
// government 0 (Federation), so the outfit field alone flips the decision.
TEST_CASE("Outfitter gate enforces RequireGovt at the landed stellar",
          "[landed_store][outfitter][government]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // the starter Shuttle
  state.player.credits = 10'000'000;

  auto *outfit =
      const_cast<game::Outfit *>(state.scenario.Outfit(0x80)); // Light Blaster
  REQUIRE(outfit != nullptr);

  outfit->require_govt = 0x80; // license band, target govt 0
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x80));

  outfit->require_govt = 0xc38; // strict contraband: denied at govt 0
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x80));

  outfit->require_govt = -1; // applies everywhere again
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x80));
}

// Rank price scale: DAT_007d4bbc / DAT_007d4bc0 seed at 1.0 and fold in
// `PriceMod * 0.01` for every active + defined rank allied to the landed
// stellar's government (NovaUi_RunTravelDestinationInteractionLoop 0x00491f9b).
TEST_CASE("rank price scale folds allied active rank modifiers",
          "[landed_store][rank][price]") {
  game::GameState state;
  state.scenario.stellars.resize(1);
  state.scenario.governments.assign(2, {});
  // Cross-ally governments 0 and 1 via a shared class id.
  state.scenario.governments[0].classes = {5, -1, -1, -1};
  state.scenario.governments[1].ally_classes = {5, -1, -1, -1};
  state.scenario.stellars[0].government_id = 0;
  state.scenario.ranks.assign(0x80, {});

  state.scenario.ranks[0].defined = true;
  state.scenario.ranks[0].active = true;
  state.scenario.ranks[0].government_id = 1;
  state.scenario.ranks[0].price_mod = 50; // 0.5x
  CHECK(game::NovaLanded_RankPriceScale(state, 0x80) == Catch::Approx(0.5F));

  state.scenario.ranks[1].defined = true;
  state.scenario.ranks[1].active = true;
  state.scenario.ranks[1].government_id = 1;
  state.scenario.ranks[1].price_mod = 80; // 0.4x combined
  CHECK(game::NovaLanded_RankPriceScale(state, 0x80) == Catch::Approx(0.4F));

  // An inactive rank is ignored.
  state.scenario.ranks[0].active = false;
  CHECK(game::NovaLanded_RankPriceScale(state, 0x80) == Catch::Approx(0.8F));

  // A government-less stellar never scales.
  state.scenario.ranks[0].active = true;
  state.scenario.stellars[0].government_id = -1;
  CHECK(game::NovaLanded_RankPriceScale(state, 0x80) == Catch::Approx(1.0F));

  // A non-allied rank is ignored.
  state.scenario.stellars[0].government_id = 0;
  state.scenario.governments[1].ally_classes = {-1, -1, -1, -1};
  CHECK(game::NovaLanded_RankPriceScale(state, 0x80) == Catch::Approx(1.0F));
}

// BUGFIX(original): the original outfit path calls
// Outfit_ComputeScaledPurchasePrice with DAT_007d4bbc and discards the result
// (0x0048ea70 buy/sell, 0x00490c70 display, 0x00491950 eligibility). With
// kApplyOriginalBugFixes on, the port applies the allied rank PriceMod to
// outfit prices instead of charging the unscaled base cost.
TEST_CASE("outfit prices apply the allied rank discount",
          "[landed_store][rank][price]") {
  game::GameState state;
  state.scenario.stellars.resize(1);
  state.scenario.governments.assign(2, {});
  state.scenario.governments[0].classes = {5, -1, -1, -1};
  state.scenario.governments[1].ally_classes = {5, -1, -1, -1};
  state.scenario.stellars[0].government_id = 0;
  state.scenario.stellars[0].tech_level = 5;
  state.scenario.ships.resize(1);
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].cost = 1'000;
  state.scenario.outfits[0].tech_level = 5; // equal tech => no tech markdown
  state.scenario.ranks.assign(0x80, {});
  state.scenario.ranks[0].defined = true;
  state.scenario.ranks[0].active = true;
  state.scenario.ranks[0].government_id = 1;
  state.scenario.ranks[0].price_mod = 50; // 0.5x
  state.player.ship_class_id = 0;         // ship resource id 0x80

  // Base cost 1000 -> rank scale 0.5 -> 500, high enough for the 10-credit
  // quantum to leave it unchanged.
  CHECK(game::NovaLanded_OutfitPrice(state, 0x80, 0x80) == 500);
}

// Regression: the OnPurchase/OnSell/OnRetire/OnCapture set strings go through
// the shared reaction-script engine (Ghidra 0x00448020 -> 0x00449370), not a
// control-bit-only mini-parser. Base-data ship-upgrade outfits encode
// `H<class>` and the Forged Exotic license encodes `S<mission> D...`.
TEST_CASE("landed control set strings run the full mission engine",
          "[landed_store]") {
  game::GameState state;
  state.scenario.ships.resize(0x40);

  game::NovaLanded_ExecuteControlSet(state, "b42 H165", "outfit OnPurchase");

  CHECK(state.control.ControlBit(42));
  CHECK(state.player.ship_class_id == 165 - 0x80);
}
