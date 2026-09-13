// Cargo bookkeeping tests: the Player Info Jettison pass
// (Player_RedistributeFleetCargoOverflow 0x0041f330) and the AI boarding
// bin-to-bin plunder stage of Boarding_BoardShipAndTransferCargo (0x00412550).

#include "game/boarding_plunder.hpp"
#include "game/freeflight_objects.hpp"
#include "game/game_state.hpp"
#include "game/outfit.hpp"
#include "game/scenario_data.hpp"

#include "brgr_archive.hpp"
#include "rle_sprite_sheet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>

namespace game {

TEST_CASE("jettison clears the player cargo bins and junk counts",
          "[cargo][jettison]") {
  GameState state;
  state.inventory.cargo_bins = {3, 0, 5, 0, 0, 7};
  state.inventory.junk_counts.fill(0);
  state.inventory.junk_counts[2] = 7;
  state.inventory.junk_counts[9] = 4;

  // jettison_all = false (the in-flight non-mission dump / overflow path):
  // only the standard bins and junk are cleared.
  Player_RedistributeFleetCargoOverflow(state,
                                        /*jettison_all=*/false,
                                        /*now_ms=*/1000);

  for (const std::int16_t bin : state.inventory.cargo_bins) {
    CHECK(bin == 0);
  }
  for (const std::int16_t junk : state.inventory.junk_counts) {
    CHECK(junk == 0);
  }
  CHECK_FALSE(state.stat_cache_valid);

  // 26 tons of cargo+junk -> ROUND(26 / 5) = 5 jettisoned cargo pods.
  std::size_t pods = 0;
  for (const FreeflightObjectState &object : state.freeflight_objects) {
    if (object.lifetime_ticks >= 0.0F) {
      ++pods;
      CHECK(object.sprite_set_index == 0);
      CHECK(object.lifetime_ticks >= 180.0F);
      CHECK(object.lifetime_ticks <= 269.0F);
      CHECK(object.spin_rate >= -1);
      CHECK(object.spin_rate <= 1);
    }
  }
  CHECK(pods == 5);
}

TEST_CASE("cargo total counts carried mission cargo", "[cargo]") {
  GameState state;
  state.inventory.cargo_bins = {2, 0, 0, 0, 0, 3};
  state.inventory.junk_counts.fill(0);
  state.inventory.junk_counts[1] = 4;

  CHECK(Player_ComputeCargoAndJunkTotal(state) == 9);

  constexpr std::size_t kSlot = 2;
  state.active_mission_runtime_flags[kSlot].is_active = true;
  ActiveMission &mission = state.active_missions[kSlot];
  mission.carrying_resources = true;
  mission.cargo_qty_tons = 7;
  CHECK(Player_ComputeCargoAndJunkTotal(state) == 16);

  // Inactive missions and non-carried cargo do not count.
  state.active_mission_runtime_flags[kSlot].is_active = false;
  CHECK(Player_ComputeCargoAndJunkTotal(state) == 9);
  state.active_mission_runtime_flags[kSlot].is_active = true;
  mission.carrying_resources = false;
  CHECK(Player_ComputeCargoAndJunkTotal(state) == 9);
}

TEST_CASE("cargo content predicate includes valid active mission cargo",
          "[cargo]") {
  GameState state;
  CHECK_FALSE(Player_HasAnyCargoMissionOrJunk(state));

  state.inventory.cargo_bins[2] = 1;
  CHECK(Player_HasAnyCargoMissionOrJunk(state));
  state.inventory.cargo_bins[2] = 0;

  constexpr std::size_t kSlot = 4;
  state.active_mission_runtime_flags[kSlot].is_active = true;
  ActiveMission &mission = state.active_missions[kSlot];
  mission.carrying_resources = true;
  mission.cargo_type_id = 3;
  mission.cargo_qty_tons = 0;
  CHECK(Player_HasAnyCargoMissionOrJunk(state));

  mission.cargo_type_id = -1;
  CHECK_FALSE(Player_HasAnyCargoMissionOrJunk(state));
  mission.cargo_type_id = 3;
  mission.cargo_qty_tons = -1;
  CHECK_FALSE(Player_HasAnyCargoMissionOrJunk(state));
  state.active_mission_runtime_flags[kSlot].is_active = false;
  state.inventory.junk_counts[0x7f] = 1;
  CHECK(Player_HasAnyCargoMissionOrJunk(state));
}

TEST_CASE("carried-ship outfit count prefers deployed craft then bay ammo",
          "[outfit][carrier]") {
  GameState state;
  state.scenario.outfits.resize(1);
  state.scenario.weapons.resize(1);
  state.scenario.ships.resize(1);

  Outfit &fighter_outfit = state.scenario.outfits[0];
  fighter_outfit.mod_type = static_cast<std::int16_t>(OutfitEffect::kAmmo);
  fighter_outfit.mod_val = 0;
  Weapon &bay = state.scenario.weapons[0];
  bay.weapon_mode_code = 99;
  bay.ammo_type = 0x80;

  state.weapon_bank_ammo[0] = 1;
  state.weapon_bank_secondary[0] = 3;
  CHECK(Outfit_CountCarriedShipsForOutfit(state, 0x80) == 3);
  CHECK(Outfit_PlayerHasOutfitForControlExpression(state, 0x80));

  Ship &first = state.ShipAt(1);
  first.is_active = true;
  first.ship_instance_id = 1;
  first.squad_leader_ship_slot = 0;
  first.ai_behavior_code = 5;
  first.ship_class_id = 0;
  Ship &second = state.ShipAt(2);
  second = first;
  second.ship_instance_id = 2;
  CHECK(Outfit_CountCarriedShipsForOutfit(state, 0x80) == 2);

  first.ai_behavior_code = 6;
  second.is_active = false;
  CHECK(Outfit_CountCarriedShipsForOutfit(state, 0x80) == 3);

  state.weapon_bank_ammo[0] = 0;
  CHECK(Outfit_CountCarriedShipsForOutfit(state, 0x80) == 0);
  CHECK_FALSE(Outfit_PlayerHasOutfitForControlExpression(state, 0x80));
  state.inventory.outfit_owned_count[0] = 1;
  CHECK(Outfit_PlayerHasOutfitForControlExpression(state, 0x80));
}

TEST_CASE("jettison_all drains abortable mission cargo and fails the mission",
          "[cargo][jettison]") {
  GameState state;
  state.inventory.cargo_bins = {2, 0, 0, 0, 0, 0};
  state.inventory.junk_counts.fill(0);

  constexpr std::size_t kSlot = 3;
  state.active_mission_runtime_flags[kSlot].is_active = true;
  ActiveMission &mission = state.active_missions[kSlot];
  mission.carrying_resources = true;
  mission.cargo_type_id = 1;
  mission.cargo_qty_tons = 4;
  mission.can_abort = true;
  mission.flags_primary = 0;

  Player_RedistributeFleetCargoOverflow(state,
                                        /*jettison_all=*/true,
                                        /*now_ms=*/2000);

  CHECK_FALSE(mission.carrying_resources);
  CHECK(state.active_mission_runtime_flags[kSlot].is_failed);
  for (const std::int16_t bin : state.inventory.cargo_bins) {
    CHECK(bin == 0);
  }
}

namespace {
// A boardable boarder hull: active, nonzero instance id, armed with cargo
// holds. Returns its zero-based class index.
int FindBoarderClass(const ScenarioData &data) {
  for (std::size_t i = 0; i < data.ships.size(); ++i) {
    if (data.ships[i].cargo_holds > 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}
} // namespace

TEST_CASE("AI boarding plunders the player's cargo into the boarder holds",
          "[cargo][boarding]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int boarder_class = FindBoarderClass(state.scenario);
  REQUIRE(boarder_class >= 0);
  const int boarder_holds =
      state.scenario.ships[static_cast<std::size_t>(boarder_class)].cargo_holds;

  // A boarder hull (NPC) and the player victim. The player is
  // ship_instance_id 0, so Boarding_BoardShipAndTransferCargo reads the
  // PlayerInventory bins.
  Ship &boarder = state.ShipAt(1);
  boarder.is_active = true;
  boarder.ship_instance_id = 1;
  boarder.ship_class_id = static_cast<std::int16_t>(boarder_class);
  boarder.armor_points = 100.0F;

  Ship &player = state.player;
  player.is_active = true;
  player.ship_instance_id = 0;
  player.armor_points = 100.0F;
  // Pick any loaded class for the victim's total cargo capacity.
  REQUIRE_FALSE(state.scenario.ships.empty());
  player.ship_class_id = 0;
  const std::int32_t capacity = Ship_ComputeShipTotalCargoCapacity(state);

  state.inventory.cargo_bins = {0, 0, 0, 0, 0, 0};
  state.inventory.cargo_bins[0] = 100; // more than any stock freighter's holds
  state.inventory.junk_counts.fill(0);
  state.player.credits = 10000;
  state.stat_cache_valid = true;

  Boarding_BoardShipAndTransferCargo(state, boarder, player, /*now_ms=*/3000);

  std::int32_t remaining = 0;
  for (const std::int16_t bin : state.inventory.cargo_bins) {
    remaining += bin;
  }
  const std::int32_t expected_taken =
      std::min<std::int32_t>(std::min(100, boarder_holds), capacity);
  CHECK(remaining == 100 - expected_taken);
  CHECK_FALSE(state.stat_cache_valid);
}

// The jettison pods render through spin 500 (the 500+index table); pin that
// the shipped resource exists and decodes to the 36-frame tile grid the tick
// assumes (DAT_005753a0 = 36.0).
TEST_CASE("freeflight jettison sprite set 500 is a 36-frame spin",
          "[cargo][jettison][sprite]") {
  if (!std::filesystem::exists("EV Nova/Nova Files/Nova Graphics 1.rez") &&
      !std::filesystem::exists(
          "../../../EV Nova/Nova Files/Nova Graphics 1.rez")) {
    SKIP("Nova .rez archives not present");
  }
  const auto descriptor = NovaResource_Load(kResourceTypeSprites, 500);
  REQUIRE(descriptor);
  const auto def = NovaSpriteDefinition_Parse(*descriptor);
  REQUIRE(def);
  const auto sheet =
      NovaResource_Load(kResourceTypeRleSheet16, def->sprites_resource_id);
  REQUIRE(sheet);
  const auto decoded = RleSpriteSheet_Decode16(*sheet);
  REQUIRE(decoded);
  CHECK(decoded->width == def->tile_width);
  CHECK(decoded->height == def->tile_height);
  CHECK(decoded->frames.size() == 36);
}

TEST_CASE("freeflight objects integrate and expire", "[cargo][freeflight]") {
  GameState state;
  state.player.current_system_id = 3;

  Ship ship;
  ship.pos_x = 10.0F;
  ship.pos_y = 20.0F;
  ship.vel_x = 1.0F;
  ship.vel_y = -2.0F;
  ship.current_system_id = 3;
  ship.heading = 0.0F; // facing up; pods scatter backwards

  NovaFreeflight_SpawnForShip(state, ship);
  const FreeflightObjectState *live = nullptr;
  for (const FreeflightObjectState &object : state.freeflight_objects) {
    if (object.lifetime_ticks >= 0.0F) {
      live = &object;
    }
  }
  REQUIRE(live != nullptr);
  CHECK(live->system_id == 3);

  const float start_x = live->pos_x;
  const float start_y = live->pos_y;
  const float lifetime = live->lifetime_ticks;
  NovaFreeflight_Tick(state, 1.0F);
  // Position integrates the object's velocity each tick.
  CHECK(live->pos_x == start_x + live->vel_x);
  CHECK(live->pos_y == start_y + live->vel_y);
  CHECK(live->lifetime_ticks == lifetime - 1.0F);

  // Advance past the lifetime: the slot is retired.
  NovaFreeflight_Tick(state, lifetime + 1.0F);
  CHECK(live->lifetime_ticks < 0.0F);
}

// Ghidra 0x0046a730 Ship_ComputeShipTotalCargoCapacity and 0x00463470
// Ship_ComputeShipFreeMass: the two player-ship outfit aggregates the cargo
// and outfit flows depend on. Neither is a mass - the first is total cargo
// capacity (Holds + ModType-2 outfits), the second is the remaining FreeMass.
TEST_CASE("player total cargo capacity and free mass aggregates",
          "[cargo][outfit]") {
  GameState state;
  state.scenario.ships.assign(1, {});
  state.scenario.ships[0].cargo_holds = 10;
  state.scenario.ships[0].free_mass = 20;
  state.scenario.ships[0].mass_tons = 100; // hull Mass, scales flagged outfits
  state.player.ship_class_id = 0;

  state.scenario.outfits.assign(3, {});
  state.scenario.outfits[0].mod_type = 2; // kCargoSpace
  state.scenario.outfits[0].mod_val = 3;
  state.scenario.outfits[0].mass_tons = 4;
  state.scenario.outfits[1].alt_mod_types[1] = 2; // cargo space in an alt slot
  state.scenario.outfits[1].alt_mod_vals[1] = 2;
  state.scenario.outfits[1].mass_tons = 1;
  state.scenario.outfits[2].mod_type = 4; // shields: not cargo
  state.scenario.outfits[2].mass_tons = 7;

  state.inventory.outfit_owned_count.fill(0);
  state.inventory.outfit_owned_count[0] = 2; // 2 * 3 tons of cargo space
  state.inventory.outfit_owned_count[1] = 1; // 1 * 2 tons of cargo space

  // Holds 10 + 2*3 + 1*2 = 18; the unowned shield outfit contributes nothing.
  CHECK(Ship_ComputeShipTotalCargoCapacity(state) == 18);
  CHECK(Player_ComputeFleetCargoCapacity(state) == 18);

  // FreeMass 20 minus every owned unit's purchase mass (2*4 + 1*1) = 11.
  CHECK(Outfit_ComputePlayerFreeMass(state) == 11);

  // The original clamps the free-mass result at zero.
  state.scenario.ships[0].free_mass = 5;
  CHECK(Outfit_ComputePlayerFreeMass(state) == 0);
}

// Ghidra 0x0046a7c0 Player_ComputeRemainingCargoSpace: player ship free holds
// = capacity - carried, where carried = the cargo bins + every active
// mission's CargoQty (+0x14, when +0x33 carrying and >= 0) + positive junk.
// The single-ship branch is NOT clamped; callers clamp it themselves.
TEST_CASE("remaining cargo space subtracts bins, mission cargo and junk",
          "[cargo][outfit]") {
  GameState state;
  state.scenario.ships.assign(1, {});
  state.scenario.ships[0].cargo_holds = 10;
  state.player.ship_class_id = 0;
  state.scenario.outfits.clear();
  state.inventory.cargo_bins = {1, 2, 0, 0, 0, 0};
  state.inventory.junk_counts.fill(0);

  // 10 - (1 + 2) = 7.
  CHECK(Player_ComputeRemainingCargoSpace(state) == 7);

  // Mission cargo joins the carried total (MisnActive +0x33/+0x14).
  state.active_mission_runtime_flags[1].is_active = true;
  state.active_missions[1].carrying_resources = true;
  state.active_missions[1].cargo_qty_tons = 4;
  CHECK(Player_ComputeCargoAndJunkTotal(state) == 7);
  CHECK(Player_ComputeRemainingCargoSpace(state) == 3);

  // Junk counts as carried, and an overloaded ship reports a negative value
  // rather than being clamped at zero.
  state.inventory.junk_counts[2] = 5;
  CHECK(Player_ComputeRemainingCargoSpace(state) == -2);

  // A negative mission CargoQty is ignored by the +0x14 sign gate.
  state.active_missions[1].cargo_qty_tons = -3;
  CHECK(Player_ComputeCargoAndJunkTotal(state) == 8);
  CHECK(Player_ComputeRemainingCargoSpace(state) == 2);
}

} // namespace game
