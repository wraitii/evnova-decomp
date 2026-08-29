#include <catch2/catch_test_macros.hpp>

#include "game/hud_overlay.hpp"

// Pins the STR# 0x7d2 pool entries the boarding/plunder window (DLOG 0x3f3)
// is built from. The game's STR# helpers take 1-BASED indices
// (Resource_LoadStringEntry 0x004b8ca0 walks index-1 length-prefixed strings),
// so decompiled call sites show entry = pool index + 1; these are the
// 0-based pool entries the port's NovaHud_LoadStringEntry reads.
// See docs/boarding_plunder_capture.md and docs/reference/boarding.jpg.
TEST_CASE("STR# 0x7d2 boarding strings (0-based pool indices)") {
  using game::NovaHud_LoadStringEntry;
  auto entry = [](std::uint16_t index) {
    return NovaHud_LoadStringEntry(0x7d2, index);
  };

  // Window panel rows.
  REQUIRE(entry(0x00) == "ton");
  REQUIRE(entry(0x01) == "tons");
  REQUIRE(entry(0x06) == "Energy:");
  REQUIRE(entry(0x20) == "credits");
  REQUIRE(entry(0x6b) == "from this ship.");
  REQUIRE(entry(0x6c) == "Select what to plunder from this ship:");
  REQUIRE(entry(0x6d) == "Cargo:");
  REQUIRE(entry(0x6e) == "Ammo:");
  REQUIRE(entry(0x6f) == "Capture Odds:");
  REQUIRE(entry(0x14f) == "none");

  // Action overlays.
  REQUIRE(entry(0x70) ==
          "Oops! You tripped this ship's security self-destruct mechanism.");
  REQUIRE(entry(0x71) ==
          "You couldn't store any of the cargo you plundered from this ship.");
  REQUIRE(entry(0x72) == "You salvaged");
  REQUIRE(entry(0x73) == "You stole all the");
  REQUIRE(entry(0x74) == "You couldn't store any of the ammo you plundered "
                         "from this ship.");
  REQUIRE(entry(0x7a) == "You assigned this ship to your fleet of escorts.");
  REQUIRE(entry(0x7b) ==
          "You already have the maximum possible number of escorts.");
  REQUIRE(entry(0x7c) == "Your attempt to capture this ship was unsuccessful.");

  // Capture-decision dialog (DLOG 0x3fa / RunCaptureDecisionDialog): the
  // offer question drawn into the DITL item-2 text panel.
  REQUIRE(entry(0x75) ==
          "Do you want to use this ship as an escort, or would you rather "
          "trade places with its captain and use it as your own ship?");
  auto button = [](std::uint16_t index) {
    return NovaHud_LoadStringEntry(0x96, index);
  };
  REQUIRE(button(0x2d).value_or("") == "Use As Escort");
  REQUIRE(button(0x2e).value_or("") == "Use As My Ship");

  // Board-command denial overlays (NovaBoarding_HandleBoardTargetCommand;
  // the original passes 0x82/0x83/0x84 through the 1-based helper).
  REQUIRE(entry(0x81) == "You can't board this ship.");
  REQUIRE(entry(0x82) == "You're not close enough to board this ship.");
  REQUIRE(entry(0x83) == "You're moving too fast to board this ship.");

  // Energy-transfer overlays (DAT_0072d6cc/d7cc/d8cc).
  REQUIRE(entry(0x03) ==
          "You filled your reactors and batteries with energy from this ship.");
  REQUIRE(entry(0x04) == "You transferred all of this ship's energy to your "
                         "reactors and batteries.");
  REQUIRE(entry(0x05) == "You couldn't store any of the energy you "
                         "transferred from this ship.");
}

// The six standard boarding commodities (STR# 0xfa1, 0-based entry =
// cargo type). The original loader reads these via the 1-based helper at
// entry type+1 (FUN_004c7040 -> DAT_0069d2cc).
TEST_CASE("STR# 0xfa1 boarding commodity names") {
  const char *kExpected[6] = {"food",
                              "industrial goods",
                              "medical supplies",
                              "luxury goods",
                              "metal",
                              "equipment"};
  for (std::uint16_t type = 0; type < 6; ++type) {
    auto name = game::NovaHud_LoadStringEntry(0xfa1, type);
    REQUIRE(name.has_value());
    CHECK(*name == kExpected[type]);
  }
}
