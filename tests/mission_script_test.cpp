#include "game/game_state.hpp"
#include "game/mission_script.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace game;

TEST_CASE("mission script executor follows Bible control-bit set syntax") {
  GameState state;
  const auto result = Mission_ExecuteScript(state, "b311 !b312 ^b311");

  REQUIRE(result.ok());
  CHECK(result.commands_executed == 3);
  CHECK(state.control.ControlBit(311) == false);
  CHECK(state.control.ControlBit(312) == false);
}

TEST_CASE("mission script executor reports unknown opcodes") {
  GameState state;
  const auto result = Mission_ExecuteScript(state, "Z128");

  REQUIRE_FALSE(result.ok());
  REQUIRE(result.diagnostics.size() == 1);
  CHECK(result.diagnostics.front().offset == 0);
}

TEST_CASE("mission script executor implements mission lifecycle operators") {
  GameState state;
  state.active_mission_runtime_flags[0].is_active = true;
  state.active_missions[0].mission_template_id = 2;

  auto result = Mission_ExecuteScript(state, "F130");
  REQUIRE(result.ok());
  CHECK(state.active_mission_runtime_flags[0].is_failed);

  result = Mission_ExecuteScript(state, "A130");
  REQUIRE(result.ok());
  CHECK_FALSE(state.active_mission_runtime_flags[0].is_active);
}

TEST_CASE("mission script executor mutates ranks, exploration, and stellars") {
  GameState state;
  state.scenario.stellars.resize(1);
  state.scenario.ranks.resize(0x80);
  for (std::size_t i = 0; i < state.scenario.ranks.size(); ++i) {
    state.scenario.ranks[i].id = static_cast<std::int16_t>(i);
  }

  const auto result = Mission_ExecuteScript(state, "K131 X128 Y128 U128");

  REQUIRE(result.ok());
  CHECK(result.commands_executed == 4);
  CHECK(state.scenario.ranks[3].active);
  CHECK(state.recently_activated_rank_id == 3);
  CHECK(state.control.explored_systems.test(0));
  CHECK_FALSE(state.scenario.stellars[0].is_destroyed);

  REQUIRE(Mission_ExecuteScript(state, "Y128").ok());
  CHECK(state.scenario.stellars[0].is_destroyed);
}

TEST_CASE("mission script executor supports random two-branch choices") {
  GameState state;
  state.rng.seed(42);

  const auto result = Mission_ExecuteScript(state, "R(b7 !b8)");

  REQUIRE(result.ok());
  CHECK(result.commands_executed == 1);
  CHECK(state.control.ControlBit(7) != state.control.ControlBit(8));
}

TEST_CASE("mission script movement uses system center and first stellar") {
  GameState state;
  state.scenario.systems.resize(2);
  state.scenario.stellars.resize(1);
  state.scenario.systems[1].pos_x = 100;
  state.scenario.systems[1].pos_y = 200;
  state.scenario.systems[1].nav_defs[0] = 0x80;
  state.scenario.stellars[0].pos_x = 12;
  state.scenario.stellars[0].pos_y = 34;

  REQUIRE(Mission_ExecuteScript(state, "M129").ok());
  CHECK(state.player.current_system_id == 1);
  CHECK(state.player.pos_x == 12.0F);
  CHECK(state.player.pos_y == 34.0F);

  REQUIRE(Mission_ExecuteScript(state, "N129").ok());
  CHECK(state.player.pos_x == 100.0F);
  CHECK(state.player.pos_y == 200.0F);
}

TEST_CASE(
    "mission script ship changes preserve and replace loadouts correctly") {
  GameState state;
  state.scenario.ships.resize(2);
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].max_count = 10;
  state.scenario.ships[1].default_outfit_ids[0] = 0;
  state.scenario.ships[1].default_outfit_counts[0] = 1;
  state.inventory.outfit_owned_count[0] = 2;

  REQUIRE(Mission_ExecuteScript(state, "C129").ok());
  CHECK(state.player.ship_class_id == 1);
  CHECK(state.inventory.outfit_owned_count[0] == 2);

  REQUIRE(Mission_ExecuteScript(state, "H129").ok());
  CHECK(state.inventory.outfit_owned_count[0] == 1);
}

TEST_CASE("mission script executor rejects malformed random expressions") {
  GameState state;
  const auto result = Mission_ExecuteScript(state, "R(b1)");

  REQUIRE_FALSE(result.ok());
  REQUIRE(result.diagnostics.size() == 1);
}

TEST_CASE("mission script entrypoints retain their Ghidra call boundaries") {
  GameState state;

  REQUIRE(Mission_ExecuteReactionScript(state, "b11").ok());
  REQUIRE(Mission_RunMisnScriptPayload(state, "b12", 0).ok());
  REQUIRE(Mission_ExecuteMisnScriptEngine(state, "b13").ok());

  CHECK(state.control.ControlBit(11));
  CHECK(state.control.ControlBit(12));
  CHECK(state.control.ControlBit(13));
  // The original does not validate the payload slot (a short global); an
  // out-of-range value runs the script and only skips the Q text-tag context.
  CHECK(Mission_RunMisnScriptPayload(state, "b14", 16).ok());
  CHECK(state.control.ControlBit(14));
}

TEST_CASE("mission payload context expands Q message text tags") {
  GameState state;
  state.pilot.first_name = "Jane";
  // Q7022 loads a random "Prodigal Son Replies" entry ("To: Captain <PN>...").
  const auto result = Mission_RunMisnScriptPayload(state, "Q7022", 0);
  REQUIRE(result.ok());
  CHECK(state.hud_overlay.active);
  CHECK(state.hud_overlay.message.find("<PN>") == std::string::npos);
  CHECK(state.hud_overlay.message.find("Jane") != std::string::npos);

  // Without a payload context (the original's 0xffff) the tag is left intact;
  // only slots 0..15 run the travel-destination expansion pass.
  GameState no_context;
  no_context.pilot.first_name = "Jane";
  REQUIRE(Mission_RunMisnScriptPayload(no_context, "Q7022", -1).ok());
  CHECK(no_context.hud_overlay.message.find("<PN>") != std::string::npos);
}
