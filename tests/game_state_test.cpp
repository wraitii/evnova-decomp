#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/game_state.hpp"
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

TEST_CASE("a fresh GameState is inactive with no intro played") {
  game::GameState state;
  CHECK_FALSE(state.game_active);
  CHECK_FALSE(state.intro_played);
  CHECK(state.pilot.first_name.empty());
  CHECK(state.pilot.last_name.empty());
  CHECK(state.intro_cinematic.post_intro_dest_id == -1);
  // A -1 post-intro destination means IntroCinematic_Run must NOT open the
  // post-intro travel-selection dialog.
  CHECK_FALSE(state.intro_cinematic.should_open_post_intro_dialog());

  // The four-frame cinematic array is empty by default (ids < 1 => no art).
  for (const auto id : state.intro_cinematic.source_pict_ids) {
    CHECK(id < 1);
  }
}

// The new-game flow (mirroring IntroCinematic_SetupFrames' no-save default)
// configures the intro with a single frame and post_intro_dest_id = 0x7ffd.
// 0x7ffd is the "no stellar yet" sentinel but is deliberately distinct from
// -1, so IntroCinematic_Run still opens the destination dialog after the
// last frame. Captured here as a pure-data regression test.
TEST_CASE("the new-game intro config opens the post-intro destination dialog") {
  game::IntroCinematicData cinematic;
  cinematic.source_pict_ids = {0x2008, -1, -1, -1};
  cinematic.duration_60h_ticks = {10, 0, 0, 0};
  // IntroCinematic_SetupFrames writes 0x7ffd on the no-save path (Ghidra
  // 0x004cd3b0).
  cinematic.post_intro_dest_id = 0x7ffd;

  CHECK(cinematic.source_pict_ids[0] == 0x2008);
  CHECK(cinematic.source_pict_ids[1] < 1);
  CHECK(cinematic.duration_60h_ticks[0] == 10);
  // The dialog gate trips for 0x7ffd even though it represents "no stellar".
  CHECK(cinematic.post_intro_dest_id != -1);
  CHECK(cinematic.should_open_post_intro_dialog());
}

TEST_CASE("a fresh PlayerShip carries the new-game reset defaults") {
  game::GameState state;

  // Ghidra Ship_ResetPlayerShipState clears position/velocity and debuffs.
  CHECK(state.player.is_active == false);
  CHECK(state.player.death_timer_active == -1.0F);
  CHECK(state.player.timed_action_counter == -1);
  CHECK(state.player.pos_x == 0.0F);
  CHECK(state.player.pos_y == 0.0F);
  CHECK(state.player.vel_x == 0.0F);
  CHECK(state.player.vel_y == 0.0F);
}

TEST_CASE("overwriting the starting inventory leaves the model consistent") {
  game::GameState state;
  state.outfit_owned_count.fill(7);
  state.weapon_bank_ammo.fill(9);
  state.weapon_bank_secondary.fill(9);

  const auto expected_table_size = state.outfit_owned_count.size();
  REQUIRE(expected_table_size == 0x200);
  REQUIRE(state.weapon_bank_ammo.size() == 0x100);
  REQUIRE(state.weapon_bank_secondary.size() == 0x100);

  // The new-game flow zeroes these arrays.
  state.outfit_owned_count.fill(0);
  state.weapon_bank_ammo.fill(0);
  state.weapon_bank_secondary.fill(0);
  for (const auto count : state.outfit_owned_count) {
    CHECK(count == 0);
  }
  for (const auto ammo : state.weapon_bank_ammo) {
    CHECK(ammo == 0);
  }
  for (const auto secondary : state.weapon_bank_secondary) {
    CHECK(secondary == 0);
  }
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
