// Ghidra 0x004cd3b0 IntroCinematic_SetupFrames + 0x004cd290
// PilotData_FindActivePilotName against the stock scenario data: the keyed
// pilot block (0x63688a72 family) and its intro/flags fields are final, so
// pin them. Stock ch\x9ar 0x0080 ".Trader": intro PICTs 0x2008/0x2009/0x200a
// for 45 1/60s ticks each, post-intro destination -1 (no dialog), flags bit 0
// set (the active entry).

#include "brgr_archive.hpp"
#include "game/intro_cinematic.hpp"
#include "game/pilot_file.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_CASE("IntroCinematic_SetupFrames reads the keyed .Trader block") {
  if (!std::filesystem::exists("EV Nova/Nova.rez")) {
    SKIP("Nova .rez archives not present");
  }
  game::GameState state;
  game::NovaIntroCinematic_SetupFrames(state, ".Trader");

  const auto &cinematic = state.intro_cinematic;
  CHECK(cinematic.source_pict_ids[0] == 0x2008);
  CHECK(cinematic.source_pict_ids[1] == 0x2009);
  CHECK(cinematic.source_pict_ids[2] == 0x200a);
  CHECK(cinematic.source_pict_ids[3] == -1);
  CHECK(cinematic.duration_60h_ticks[0] == 45);
  CHECK(cinematic.duration_60h_ticks[1] == 45);
  CHECK(cinematic.duration_60h_ticks[2] == 45);
  CHECK(cinematic.duration_60h_ticks[3] == 0);
  // Stock block destination: -1 -> the post-intro travel dialog does NOT open.
  CHECK(cinematic.post_intro_dest_id == -1);
  CHECK(!cinematic.should_open_post_intro_dialog());
}

TEST_CASE("IntroCinematic_SetupFrames falls back without a pilot block") {
  game::GameState state;
  game::NovaIntroCinematic_SetupFrames(state, "no such pilot");
  const auto &cinematic = state.intro_cinematic;
  CHECK(cinematic.source_pict_ids[0] == 0x2008);
  CHECK(cinematic.source_pict_ids[1] == -1);
  CHECK(cinematic.duration_60h_ticks[0] == 10);
  CHECK(cinematic.post_intro_dest_id == 0x7ffd);
  CHECK(cinematic.should_open_post_intro_dialog());
}

TEST_CASE("PilotData_FindActivePilotName finds the flagged .Trader entry") {
  if (!std::filesystem::exists("EV Nova/Nova.rez")) {
    SKIP("Nova .rez archives not present");
  }
  CHECK(game::PilotData_FindActivePilotName() == ".Trader");
}
