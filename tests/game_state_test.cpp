#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/game_state.hpp"
#include "game/pilot_file.hpp"
#include "pict_image.hpp"

#include <filesystem>

namespace {

// Data-dependent introspection of the new-game intro art (Ghidra default PICT
// 0x2008). Skipped when the archives are missing, like the menu-archive tests.
bool NovaArchivesAvailable() {
  return std::filesystem::exists("EV Nova/Nova Files/Nova Titles 1.rez") ||
         std::filesystem::exists(
             "../../../EV Nova/Nova Files/Nova Titles 1.rez");
}

} // namespace

// The game-state model is pure data (no SDL/renderer), so these tests run in
// every environment. They document the defaults the new-pilot flow relies on.

// The pilot file is an on-demand serialized record of the live state (the
// original only materializes it for load/save, see pilot_file.hpp). A fresh
// record seeds a brand-new pilot exactly like PilotData_InitializePlayerState
// (0x004cd4b0): 10000 credits, ship class 0, system 0, no intro configured.
TEST_CASE("a fresh PilotFile seed matches PilotData_InitializePlayerState") {
  const game::PilotFile fresh = game::PilotFile::Fresh();
  CHECK(fresh.pilot_name.empty());
  CHECK(fresh.credits == 10000);
  CHECK(fresh.ship_class_id == 0);
  CHECK(fresh.current_system_id == 0);
  CHECK(fresh.death_timer_active == -1.0F);
  CHECK(fresh.timed_action_counter == -1);
  // No intro configured yet.
  CHECK(fresh.post_intro_dest_id == -1);
  for (const auto id : fresh.intro_source_pict_ids) {
    CHECK(id < 1);
  }
}

// Applying a seeded pilot record mirrors the live state the new-game flow
// produces (PilotData_InitializePlayerState fresh-seed + IntroCinematic_
// SetupFrames), without touching serialization (no .plt writer).
TEST_CASE("PilotFileApply seeds the live state from a pilot record") {
  game::PilotFile record = game::PilotFile::Fresh();
  record.pilot_name = "Rowan";
  record.intro_source_pict_ids = {0x2008, 0x2009, 0x200a, -1};
  record.intro_duration_60h_ticks = {45, 45, 45, 0};
  record.post_intro_dest_id = 0x7ffd;
  record.outfit_owned_count[9] = 2;

  game::GameState state;
  state.pilot.first_name = "not-yet-seeded";
  game::PilotFileApply(record, state);

  CHECK(state.pilot.first_name == "Rowan");
  // The fresh seed carries the PilotData_InitializePlayerState defaults.
  CHECK(state.player.credits == 10000);
  CHECK(state.player.ship_class_id == 0);
  CHECK(state.player.current_system_id == 0);
  CHECK(state.player.timed_action_counter == -1);
}

// Ghidra-linked single-bit flags: a fresh state is out of game and hasn't
// played the intro (DAT_00596d28 / DAT_00596d35, the latter cleared on new
// pilot so the intro plays on first entry).
TEST_CASE("a fresh GameState is inactive and has not played the intro") {
  game::GameState state;
  CHECK_FALSE(state.game_active);
  CHECK_FALSE(state.intro_played);
}

// The post-intro dialog gate (Ghidra IntroCinematic_Run) must open the
// travel-selection dialog for the new-game sentinel 0x7ffd ("no stellar yet,"
// written by IntroCinematic_SetupFrames 0x004cd3b0 on the no-save path),
// which is deliberately distinct from -1. A fresh (default) state must not.
TEST_CASE("post-intro dialog gate trips for 0x7ffd but not -1") {
  game::IntroCinematicData fresh;
  CHECK(fresh.post_intro_dest_id == -1);
  CHECK_FALSE(fresh.should_open_post_intro_dialog());

  game::IntroCinematicData new_game;
  new_game.post_intro_dest_id = 0x7ffd;
  CHECK(new_game.should_open_post_intro_dialog());
}

TEST_CASE("the default character resource supplies the three-frame intro") {
  if (!NovaArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }

  const auto intro = NovaResource_LoadCharacterIntro();
  REQUIRE(intro);
  REQUIRE(intro->pict_ids.size() == 4);
  CHECK(intro->pict_ids[0] == 0x2008);
  CHECK(intro->pict_ids[1] == 0x2009);
  CHECK(intro->pict_ids[2] == 0x200a);
  CHECK(intro->pict_ids[3] < 1);
  CHECK(intro->delay_ticks[0] == 45);
  CHECK(intro->delay_ticks[1] == 45);
  CHECK(intro->delay_ticks[2] == 45);
  CHECK(intro->delay_ticks[3] == 0);
}

TEST_CASE("the default new-game intro frame PICT 0x2008 is locatable") {
  if (!NovaArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }

  // IntroCinematic_SetupFrames defaults to a single intro PICT 0x2008 shown
  // for 10 ticks when no pilot save block exists. If it cannot be located, the
  // intro module falls back to a blank frame and logs a TODO; this test keeps
  // the fallback visible rather than silently passing.
  const auto data = NovaResource_LoadPictData(0x2008);
  CHECK(data.has_value());
  if (data) {
    const auto image = Resource_LoadPictAsImage(*data);
    CHECK(image.has_value());
  }
}
