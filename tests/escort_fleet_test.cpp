#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "game/landed_store.hpp"
#include "game/scenario_data.hpp"

namespace {

using game::GameState;
using game::Ship;

// Zero-based class id of the Shuttle (shïp resource 0x80): a stock
// escort-upgradable ship whose payload carries UpgradeTo 0xBC / EscUpgrdCost
// 5000 / EscSellValue 0 (so the loader defaults the sale value to 10% of the
// 10000-credit cost).
constexpr std::int16_t kShuttle = 0x80 - 0x80;
constexpr std::int16_t kShuttleUpgrade = 0xBC - 0x80;

// Finds a stellar whose travel_flags carry the shipyard bit (0x8).
[[nodiscard]] std::int16_t FindShipyardStellar(const game::ScenarioData &sc) {
  for (std::size_t i = 0; i < sc.stellars.size(); ++i) {
    if ((sc.stellars[i].flags & 0x8U) != 0U) {
      return static_cast<std::int16_t>(i + 0x80);
    }
  }
  return -1;
}

// Lays a healthy, player-attached behavior-6 escort on `slot`.
[[nodiscard]] Ship &
MakeEscort(GameState &state, std::size_t slot, std::int16_t ship_class_id) {
  Ship &ship = state.ShipAt(slot);
  ship = Ship{};
  ship.is_active = true;
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.ship_class_id = ship_class_id;
  ship.ai_behavior_code = 6;
  ship.squad_leader_ship_slot = 0;
  ship.mission_fleet_slot = -1;
  ship.faction_or_government_id = -1;
  ship.defense_fleet_home_stellar_id = -1;
  ship.armor_points = 30.0F;
  ship.shield_points = 30.0F;
  return ship;
}

} // namespace

TEST_CASE("escort fleet trade sells released and upgrades marked escorts",
          "[scenario][escort]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const std::int16_t shipyard = FindShipyardStellar(state.scenario);
  REQUIRE(shipyard != -1);

  const game::ShipClass *shuttle = state.scenario.Ship(0x80);
  const game::ShipClass *upgraded = state.scenario.Ship(0xBC);
  REQUIRE(shuttle != nullptr);
  REQUIRE(upgraded != nullptr);
  REQUIRE(shuttle->escort_sell_value == 1000); // trunc(0.10 * 10000)
  REQUIRE(shuttle->escort_upgrade_cost == 5000);
  REQUIRE(shuttle->upgrade_to_ship_class_id == kShuttleUpgrade);

  Ship &sold = MakeEscort(state, 1, kShuttle);
  sold.escort_released_mark = 1;
  Ship &upgrade = MakeEscort(state, 2, kShuttle);
  upgrade.escort_upgrade_mark = 1;

  state.player.credits = 100000;
  const std::int32_t credits_before = state.player.credits;

  std::vector<std::string> messages;
  game::Player_ProcessEscortFleetAtStellar(
      state, shipyard, [&](const std::string &text) {
        messages.push_back(text);
      });

  // Release sale: deactivated and credited the default sell value.
  CHECK_FALSE(sold.is_active);
  CHECK(sold.squad_leader_ship_slot == -1);
  // Upgrade: paid the EscUpgrdCost, class switched, mark cleared, banks zeroed.
  CHECK(upgrade.ship_class_id == kShuttleUpgrade);
  CHECK(upgrade.escort_upgrade_mark == 0);
  CHECK(upgrade.npc_weapon_bank_ammo[0] == 0);
  CHECK(upgrade.escort_origin_mark == 0); // payroll therefore skipped
  // Net credits: +1000 sale, -5000 upgrade.
  CHECK(state.player.credits == credits_before + 1000 - 5000);

  // Refill pass ran for every slot: the upgraded ship now sits at its new
  // class base shield/armor.
  CHECK(upgrade.shield_points == static_cast<float>(upgraded->base_shield));
  CHECK(upgrade.armor_points == static_cast<float>(upgraded->base_armor));

  // One summary message, containing the localized count word and phrases.
  REQUIRE(messages.size() == 1);
  CHECK(messages[0].find("sold for a profit of") != std::string::npos);
  CHECK(messages[0].find("upgraded at a cost of") != std::string::npos);
}

TEST_CASE("escort payroll deducts upkeep and defects unpaid escorts",
          "[scenario][escort]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Upkeep is trunc(base_cost * 0.01) = 100 for the 10000-credit Shuttle.
  Ship &paid = MakeEscort(state, 1, kShuttle);
  paid.escort_origin_mark = 1;
  paid.credits = 0;
  Ship &unpaid = MakeEscort(state, 2, kShuttle);
  unpaid.escort_origin_mark = 1;

  state.player.credits = 150;

  std::vector<std::string> messages;
  game::Player_ProcessEscortPayroll(
      state, 1, [&](const std::string &text) { messages.push_back(text); });

  // Slot 1 was paid (150 -> 50); slot 2 could not be paid and defected.
  CHECK(state.player.credits == 50);
  CHECK(paid.is_active);
  CHECK_FALSE(unpaid.is_active);
  CHECK(unpaid.squad_leader_ship_slot == -1);
  CHECK(unpaid.ai_behavior_code == 1);
  REQUIRE(messages.size() == 1);
  CHECK_FALSE(messages[0].empty());
}

TEST_CASE("mission-fleet escorts are exempt from payroll",
          "[scenario][escort]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  Ship &escort = MakeEscort(state, 1, kShuttle);
  escort.escort_origin_mark = 1;
  escort.mission_fleet_slot = 0;
  state.active_mission_runtime_flags[0].is_active = true;
  state.active_missions[0].fleet_spawn_goal = 1;

  state.player.credits = 0;
  bool messaged = false;
  game::Player_ProcessEscortPayroll(
      state, 1, [&](const std::string &) { messaged = true; });

  CHECK(escort.is_active);
  CHECK(state.player.credits == 0);
  CHECK_FALSE(messaged);
}
