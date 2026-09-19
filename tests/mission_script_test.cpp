#include "game/game_state.hpp"
#include "game/mission_script.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace game;

TEST_CASE("mission script executor follows Bible control-bit set syntax") {
  GameState state;
  Mission_ExecuteScript(state, "b311 !b312 ^b311");

  CHECK(state.control.ControlBit(311) == false);
  CHECK(state.control.ControlBit(312) == false);
}

TEST_CASE("mission script executor skips unknown opcodes without aborting") {
  GameState state;
  Mission_ExecuteScript(state, "Z128 b5");

  CHECK(state.control.ControlBit(5));
}

TEST_CASE("mission script executor implements mission lifecycle operators") {
  GameState state;
  state.active_mission_runtime_flags[0].is_active = true;
  state.active_missions[0].mission_template_id = 2;

  Mission_ExecuteScript(state, "F130");
  CHECK(state.active_mission_runtime_flags[0].is_failed);

  Mission_ExecuteScript(state, "A130");
  CHECK_FALSE(state.active_mission_runtime_flags[0].is_active);
}

TEST_CASE("mission script executor mutates ranks, exploration, and stellars") {
  GameState state;
  state.scenario.stellars.resize(1);
  state.scenario.systems.resize(1);
  state.scenario.ranks.resize(0x80);
  for (std::size_t i = 0; i < state.scenario.ranks.size(); ++i) {
    state.scenario.ranks[i].id = static_cast<std::int16_t>(i);
  }

  Mission_ExecuteScript(state, "K131 X128 Y128 U128");

  CHECK(state.scenario.ranks[3].active);
  CHECK(state.recently_activated_rank_id == 3);
  CHECK(state.scenario.systems[0].discovery_state > 0);
  CHECK_FALSE(state.scenario.stellars[0].is_destroyed);

  Mission_ExecuteScript(state, "Y128");
  CHECK(state.scenario.stellars[0].is_destroyed);
}

TEST_CASE("mission script executor supports random two-branch choices") {
  GameState state;
  state.rng.seed(42);

  Mission_ExecuteScript(state, "R(b7 !b8)");

  CHECK(state.control.ControlBit(7) != state.control.ControlBit(8));
}

TEST_CASE("mission script movement uses the first nav stellar and N keeps "
          "position") {
  GameState state;
  state.scenario.systems.resize(2);
  state.scenario.stellars.resize(1);
  state.scenario.systems[1].pos_x = 100;
  state.scenario.systems[1].pos_y = 200;
  state.scenario.systems[1].nav_defs[0] = 0x80;
  state.scenario.stellars[0].pos_x = 12;
  state.scenario.stellars[0].pos_y = 34;

  Mission_ExecuteScript(state, "M129");
  CHECK(state.player.current_system_id == 1);
  CHECK(state.player.pos_x == 12.0F);
  CHECK(state.player.pos_y == 34.0F);

  // N never touches the position (Ghidra 0x00449bcc..0x00449bda).
  Mission_ExecuteScript(state, "N129");
  CHECK(state.player.pos_x == 12.0F);
  CHECK(state.player.pos_y == 34.0F);
}

TEST_CASE("docked M queues the first nav and docked N latches the skip") {
  GameState state;
  state.scenario.systems.resize(2);
  state.scenario.stellars.resize(1);
  state.scenario.systems[1].nav_defs[0] = 0x80;
  state.scenario.stellars[0].pos_x = 12;
  state.scenario.stellars[0].pos_y = 34;
  state.system_transition_active = true;
  state.player.pos_x = -5.0F;
  state.player.pos_y = -6.0F;

  // Docked M stashes the 0-based nav index for the launch tail instead of
  // moving the ship (Ghidra 0x004499e1..0x004499f5).
  Mission_ExecuteScript(state, "M129");
  CHECK(state.player.pos_x == -5.0F);
  CHECK(state.player.pos_y == -6.0F);
  CHECK(state.player.ai_secondary_target_slot == 0);

  // Docked N clears the target and latches the one-shot launch skip.
  Mission_ExecuteScript(state, "N129");
  CHECK(state.player.ai_secondary_target_slot == -1);
  CHECK(state.skip_player_reposition_once);
}

TEST_CASE("M into a nav-less system uses the in-system origin") {
  GameState state;
  state.scenario.systems.resize(2);
  state.player.pos_x = 5.0F;
  state.player.pos_y = 6.0F;

  // BUGFIX(original): with no nav stellar the executable leaves the ship put
  // (the in-system origin is (0,0), not System::pos_x).
  Mission_ExecuteScript(state, "M129");
  CHECK(state.player.current_system_id == 1);
  CHECK(state.player.pos_x == 0.0F);
  CHECK(state.player.pos_y == 0.0F);
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

  Mission_ExecuteScript(state, "C129");
  CHECK(state.player.ship_class_id == 1);
  CHECK(state.inventory.outfit_owned_count[0] == 2);

  Mission_ExecuteScript(state, "H129");
  CHECK(state.inventory.outfit_owned_count[0] == 1);
}

TEST_CASE(
    "mission script executor recovers from malformed random expressions") {
  GameState state;
  Mission_ExecuteScript(state, "R(b1) b6");

  CHECK(state.control.ControlBit(6));
}

TEST_CASE("mission script entrypoints retain their Ghidra call boundaries") {
  GameState state;

  Mission_ExecuteReactionScript(state, "b11");
  Mission_RunMisnScriptPayload(state, "b12", 0);
  Mission_ExecuteMisnScriptEngine(state, "b13");

  CHECK(state.control.ControlBit(11));
  CHECK(state.control.ControlBit(12));
  CHECK(state.control.ControlBit(13));
  // The original does not validate the payload slot (a short global); an
  // out-of-range value runs the script and only skips the Q text-tag context.
  Mission_RunMisnScriptPayload(state, "b14", 16);
  CHECK(state.control.ControlBit(14));
}

TEST_CASE("mission payload context expands Q message text tags") {
  GameState state;
  state.pilot.first_name = "Jane";
  // Q7022 loads a random "Prodigal Son Replies" entry ("To: Captain <PN>...").
  Mission_RunMisnScriptPayload(state, "Q7022", 0);
  CHECK(state.hud_overlay.active);
  CHECK(state.hud_overlay.message.find("<PN>") == std::string::npos);
  CHECK(state.hud_overlay.message.find("Jane") != std::string::npos);

  // Without a payload context (the original's 0xffff) the tag is left intact;
  // only slots 0..15 run the travel-destination expansion pass.
  GameState no_context;
  no_context.pilot.first_name = "Jane";
  Mission_RunMisnScriptPayload(no_context, "Q7022", -1);
  CHECK(no_context.hud_overlay.message.find("<PN>") != std::string::npos);
}
