#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

#include "game/escort_commands.hpp"
#include "game/pilot_file.hpp"
#include "game/scenario_data.hpp"
#include "game/ship_ai.hpp"

namespace {

using game::GameState;
using game::Ship;

void ClearShips(GameState &state) {
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    state.ShipAt(slot) = Ship{};
  }
}

[[nodiscard]] std::array<std::int16_t, 4>
FindCategoryClasses(const game::ScenarioData &scenario) {
  std::array<std::int16_t, 4> result{-1, -1, -1, -1};
  for (std::size_t index = 0; index < scenario.ships.size(); ++index) {
    const auto &ship_class = scenario.ships[index];
    if (ship_class.class_category >= 0 && ship_class.class_category < 4 &&
        result[static_cast<std::size_t>(ship_class.class_category)] == -1) {
      result[static_cast<std::size_t>(ship_class.class_category)] =
          static_cast<std::int16_t>(index);
    }
  }
  return result;
}

Ship &
MakeAttachedShip(GameState &state, std::size_t slot, std::int16_t class_id) {
  Ship &ship = state.ShipAt(slot);
  ship.is_active = true;
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.ship_class_id = class_id;
  ship.ai_behavior_code = 6;
  ship.squad_leader_ship_slot = 0;
  ship.mission_fleet_slot = -1;
  ship.faction_or_government_id = -1;
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = -1;
  ship.armor_points = 100.0F;
  ship.shield_points = 100.0F;
  return ship;
}

} // namespace

TEST_CASE("escort group selector uses the fixed number row key codes",
          "[escort][commands][input]") {
  CHECK(game::kEscortGroupSelectionKeyCodes ==
        std::array<std::uint16_t, 5>{0x02, 0x03, 0x04, 0x05, 0x06});
}

TEST_CASE("escort command panel fades after its inactivity timeout",
          "[escort][commands][input]") {
  GameState state;
  ClearShips(state);
  Ship &escort = state.ShipAt(1);
  escort.is_active = true;
  escort.squad_leader_ship_slot = 0;
  escort.armor_points = 100.0F;

  game::EscortCommandInput input;
  input.panel_toggle_held = true;
  game::PlayerTick_EscortCommands(state, input, 100);
  REQUIRE(state.escort.panel_timer == 32);

  input.panel_toggle_held = false;
  game::PlayerTick_EscortCommands(state, input, 580);
  CHECK(state.escort.panel_timer == 32);
  game::PlayerTick_EscortCommands(state, input, 581);
  CHECK(state.escort.panel_timer == 31);
  game::PlayerTick_EscortCommands(state, input, 582);
  CHECK(state.escort.panel_timer == 30);
}

TEST_CASE("escort order input updates the shared saved category orders",
          "[escort][commands]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  ClearShips(state);
  const auto classes = FindCategoryClasses(state.scenario);
  for (std::size_t category = 0; category < classes.size(); ++category) {
    REQUIRE(classes[category] >= 0);
    MakeAttachedShip(state, category + 1, classes[category]);
  }

  game::EscortCommandInput input;
  input.order_attack_held = true;
  std::int16_t selected = -1;
  SECTION("open panel all categories") { state.escort.panel_timer = 32; }
  SECTION("open panel selected category") {
    state.escort.panel_timer = 32;
    state.escort.selected_category = selected = 2;
  }
  SECTION("closed panel ignores previous category selection") {
    state.escort.selected_category = 2;
  }
  game::PlayerTick_EscortCommands(state, input, 100);

  for (std::size_t category = 0; category < classes.size(); ++category) {
    const auto expected =
        selected == -1 || selected == static_cast<std::int16_t>(category) ? 2
                                                                          : -1;
    CHECK(state.target_category_command[category] == expected);
    CHECK(state.ShipAt(category + 1).escort_command_code ==
          (expected == -1 ? 0 : expected));
  }
  const auto saved = game::PilotFileCollectFromState(state);
  CHECK(saved.target_category_command == state.target_category_command);
  state.target_category_command.fill(4);
  game::PilotFileApply(saved, state);
  CHECK(state.target_category_command == saved.target_category_command);
}

TEST_CASE("escort supervisor enters attack for an existing valid target",
          "[escort][commands][ai]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  ClearShips(state);
  const auto classes = FindCategoryClasses(state.scenario);
  REQUIRE(classes[0] >= 0);

  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.ai_state_code = 0;
  state.player.ai_control_mode = 0;
  state.player.armor_points = 100.0F;
  state.player.shield_points = 100.0F;
  Ship &escort = MakeAttachedShip(state, 1, classes[0]);
  Ship &target = state.ShipAt(2);
  target.is_active = true;
  target.ship_instance_id = 2;
  target.ship_class_id = classes[0];
  target.squad_leader_ship_slot = -1;
  target.armor_points = 100.0F;
  target.shield_points = 100.0F;

  state.player.primary_target_ship_slot = 2;
  state.scenario.ships[static_cast<std::size_t>(classes[0])].flags_secondary &=
      static_cast<std::uint16_t>(~0x80U);
  escort.ai_state_code = 10;
  game::EscortCommandInput input;
  input.order_attack_held = true;
  game::PlayerTick_EscortCommands(state, input, 100);
  REQUIRE(escort.primary_target_ship_slot == 2);

  game::NovaAi_UpdateEscortAI(state, escort, 0);

  CHECK(escort.escort_command_code ==
        static_cast<std::int16_t>(game::EscortOrder::kAttack));
  CHECK(escort.primary_target_ship_slot == 2);
  CHECK(escort.ai_state_code == 4);
}

TEST_CASE("replacement escort order cancels a returning fighter's targets",
          "[escort][commands]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  ClearShips(state);
  const auto classes = FindCategoryClasses(state.scenario);
  REQUIRE(classes[0] >= 0);

  Ship &fighter = MakeAttachedShip(state, 1, classes[0]);
  fighter.ai_behavior_code = 5;
  fighter.ai_state_code = 5;
  fighter.escort_command_code = 3;
  fighter.primary_target_ship_slot = 2;
  fighter.ai_secondary_target_slot = 0;
  SECTION("changed order") {}
  SECTION("unchanged order still cancels recovery targets") {
    fighter.escort_command_code = 0;
  }

  CHECK(game::Ship_CommandPlayerEscortGroup(
      state,
      -1,
      static_cast<std::int16_t>(game::EscortOrder::kFormation),
      true));
  CHECK(fighter.escort_command_code ==
        static_cast<std::int16_t>(game::EscortOrder::kFormation));
  CHECK(fighter.primary_target_ship_slot == -1);
  CHECK(fighter.ai_secondary_target_slot == -1);
}
