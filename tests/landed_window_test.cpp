#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/game_state.hpp"
#include "game/landed_window.hpp"
#include "game/scenario_data.hpp"
#include "game/targeting.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace {

using game::GameState;
using game::LandedContext;
using game::LandedService;
using game::ShipClass;
using game::Stellar;
using game::System;

// Deterministic scenario with a player ship and one landable stellar, injected
// directly (no archive), so the landed-window housekeeping is unit-testable.
struct Fixture {
  GameState state;
  Stellar *landing = nullptr;

  Fixture() {
    state.player.current_system_id = 0;
    state.player.ship_class_id = 0;

    ShipClass sc;
    sc.base_shield = 30;
    sc.base_armor = 30;
    sc.base_fuel = 300;
    sc.cargo_holds = 10;
    state.scenario.ships.push_back(sc); // index 0 == resource 0x80

    System s;
    s.name = "Test System";
    s.links = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    s.nav_defs = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    s.is_visible = true;
    s.nav_defs[0] = 0x81;
    state.scenario.systems.push_back(s); // index 0 == resource 0x80

    // Scenario accessor maps resource id -> index (id - 0x80). The colony is
    // resource 0x81, i.e. stellars[1]; reserve index 0 with an inert filler so
    // the index/position stays aligned with that convention.
    Stellar filler;
    filler.name = "(unused orbital)";
    filler.system_id = 0;
    state.scenario.stellars.push_back(filler); // index 0 == resource 0x80

    Stellar l;
    l.name = "Orbital Colony";
    // Engaged landable target: control bit + engaged + landable (0x83).
    l.flags = 0x83;
    l.availability_flags = 0;
    l.pos_x = 200;
    l.pos_y = 0;
    l.sprite_population = 1;
    l.sprite_handle_active = true;
    l.system_id = 0;
    state.scenario.stellars.push_back(l); // index 1 == resource 0x81

    landing = &state.scenario.stellars[1];
  }
};

} // namespace

// The landed context starts at "launch" (top of the services list) with a
// resolved stellar id once entered, mirroring the landing transition wiring.

TEST_CASE("entering the dock files the stellar id and resets selection",
          "[landed_window]") {
  Fixture f;
  f.state.player.pos_x = 200.0F;
  f.state.player.pos_y = 0.0F;
  f.state.player.shield_points = 5.0F;
  f.state.player.armor_points = 3.0F;
  f.state.player.credits = 500;
  f.landing->service_cost = 150;
  game::NovaTargeting_UpdatePlayerTarget(f.state);

  LandedContext ctx;
  REQUIRE(game::NovaLanding_EnterDocked(f.state, ctx) == true);
  CHECK(ctx.landed);
  CHECK(ctx.stellar_id == 0x81);
  CHECK(ctx.selection == LandedService::kLaunch);
  // The landing transition refills shields/armor and bills the service cost.
  CHECK(f.state.player.shield_points == Catch::Approx(30.0F));
  CHECK(f.state.player.armor_points == Catch::Approx(30.0F));
  CHECK(f.state.player.credits == 500 - 150);
}

// Fuel service prices a top-up from the (outfit derived) effective capacity.

TEST_CASE("refuel buys fuel toward capacity and bills credits",
          "[landed_window]") {
  Fixture f;
  // Deplete to a third full.
  f.state.player.fuel_points = 100.0F;
  f.state.player.credits = 500;
  // 2 credits per fuel point -> 400 credits fills the missing 200.
  const std::int32_t spent = game::NovaLanded_Refuel(f.state, 2);
  CHECK(spent == 400);
  CHECK(f.state.player.credits == 500 - 400);
  CHECK(f.state.player.fuel_points == Catch::Approx(300.0F));
}

TEST_CASE("refuel clamps at an empty wallet", "[landed_window]") {
  Fixture f;
  f.state.player.fuel_points = 0.0F;
  f.state.player.credits = 3;
  const std::int32_t spent = game::NovaLanded_Refuel(f.state, 2);
  CHECK(f.state.player.credits == 0);
  CHECK(spent == 3);
  // 3 credits at 2/point -> 1.5 fuel units.
  CHECK(f.state.player.fuel_points == Catch::Approx(1.5F));
}

TEST_CASE("refuel does nothing when the tank is already full",
          "[landed_window]") {
  Fixture f;
  f.state.player.fuel_points = 300.0F;
  f.state.player.credits = 500;
  CHECK(game::NovaLanded_Refuel(f.state, 2) == 0);
  CHECK(f.state.player.credits == 500);
}

// Armor repair service: hidden fleet charges a per-point price for the gap.

TEST_CASE("repair tops up depleted armor and bills credits",
          "[landed_window]") {
  Fixture f;
  f.state.player.armor_points = 10.0F;
  f.state.player.credits = 500;
  // 5 credits per armor point -> 100 credits to close the 20-point gap.
  const std::int32_t spent = game::NovaLanded_Repair(f.state, 5);
  CHECK(spent == 100);
  CHECK(f.state.player.credits == 500 - 100);
  CHECK(f.state.player.armor_points == Catch::Approx(30.0F));
}

TEST_CASE("repair clamps at an empty wallet", "[landed_window]") {
  Fixture f;
  f.state.player.armor_points = 10.0F;
  f.state.player.credits = 7;
  const std::int32_t spent = game::NovaLanded_Repair(f.state, 5);
  CHECK(spent == 7);
  CHECK(f.state.player.credits == 0);
  CHECK(f.state.player.armor_points == Catch::Approx(10.0F + 7.0F / 5.0F));
}

TEST_CASE("repair does nothing when armor is intact", "[landed_window]") {
  Fixture f;
  f.state.player.armor_points = 30.0F;
  f.state.player.credits = 500;
  CHECK(game::NovaLanded_Repair(f.state, 5) == 0);
  CHECK(f.state.player.credits == 500);
}

// The docked service buttons come from the real Spaceport DITL 0x3e8. The
// two-column arrangement (services 0-3 on the left column, 4-7 on the right,
// each column top-to-bottom) is what BuildServiceButtons produces, so this
// verifies the split/sort that lands Launch..BuySell left and Outfit..Starmap
// right stays faithful to the LandedService enum.
TEST_CASE("docked DITL buttons arrange the two service columns",
          "[landed_window]") {
  if (!std::filesystem::exists("EV Nova/Nova.rez") &&
      !std::filesystem::exists("EV Nova/Nova Files/Nova.rez") &&
      !std::filesystem::exists("../../../EV Nova/Nova.rez") &&
      !std::filesystem::exists("../../../EV Nova/Nova Files/Nova.rez")) {
    SKIP("Nova.rez not present");
  }
  const auto items = NovaResource_LoadDialogItems(0x3e8);
  REQUIRE(items);

  // Collect the eight 145x25 buttons into left/right columns, sorted by top
  // within each column (mirroring BuildServiceButtons / navigation col,row).
  std::vector<std::array<std::int16_t, 4>> left;
  std::vector<std::array<std::int16_t, 4>> right;
  for (const auto &item : *items) {
    if (item.right - item.left != 145 || item.bottom - item.top != 25) {
      continue;
    }
    (item.left < 400 ? left : right)
        .push_back({item.top, item.left, item.bottom, item.right});
  }
  REQUIRE(left.size() == 4);
  REQUIRE(right.size() == 4);
  const auto sort_rows = [](std::vector<std::array<std::int16_t, 4>> &col) {
    std::sort(col.begin(), col.end(),
              [](const auto &a, const auto &b) { return a[0] < b[0]; });
  };
  sort_rows(left);
  sort_rows(right);

  // Left column rows 0..3 sit at x=3 with tops 333, 374, 414, 456; right
  // column rows 0..3 at x=471 with tops 333, 375, 416, 456 (rows 1-2 are 1px
  // lower than the left column).
  const std::array<std::int16_t, 4> left_tops{333, 374, 414, 456};
  const std::array<std::int16_t, 4> right_tops{333, 375, 416, 456};
  for (std::size_t i = 0; i < 4; ++i) {
    constexpr std::int16_t kLeftX = 3;
    constexpr std::int16_t kRightX = 471;
    CHECK(left[i][1] == kLeftX);
    CHECK(left[i][0] == left_tops[i]);
    CHECK(right[i][1] == kRightX);
    CHECK(right[i][0] == right_tops[i]);
  }
}

// The dialog-window layout adapter (#3) centers the 618x517 Spaceport window
// on the 640x480 logical panel (mirroring Dialog_CreateFromDlog) and maps each
// DITL item to panel space. This verifies the centered origin and the item
// classification (buttons vs panels vs title band).
TEST_CASE("dialog layout centers the dock window and buckets the items",
          "[landed_window]") {
  if (!std::filesystem::exists("EV Nova/Nova.rez") &&
      !std::filesystem::exists("EV Nova/Nova Files/Nova.rez") &&
      !std::filesystem::exists("../../../EV Nova/Nova.rez") &&
      !std::filesystem::exists("../../../EV Nova/Nova Files/Nova.rez")) {
    SKIP("Nova.rez not present");
  }
  using game::DockedItemKind;
  using game::DockedLayout;
  const SDL_FRect panel{0.0F, 0.0F, 640.0F, 480.0F};
  DockedLayout layout;
  const bool from_ditl = game::NovaDialogWindow_Layout(panel, layout);
  REQUIRE(from_ditl);
  CHECK(layout.from_ditl);

  // Window 618x517 centered on the 640x480 panel -> x=(640-618)/2=11,
  // y=(480-517)/2=-18.
  CHECK(layout.window.w == Catch::Approx(618.0F));
  CHECK(layout.window.h == Catch::Approx(517.0F));
  CHECK(layout.window.x == Catch::Approx(11.0F));
  CHECK(layout.window.y == Catch::Approx(-18.0F));

  // The 14 DITL items are all laid out; classify and count them.
  std::size_t buttons = 0, outer = 0, inner = 0, bands = 0;
  for (const auto &item : layout.items) {
    switch (item.kind) {
    case DockedItemKind::kButton: ++buttons; break;
    case DockedItemKind::kOuterPanel: ++outer; break;
    case DockedItemKind::kInnerPanel: ++inner; break;
    case DockedItemKind::kTitleBand: ++bands; break;
    case DockedItemKind::kOrnament: break;
    }
  }
  CHECK(buttons == 8);
  CHECK(outer == 1);   // 612x285 outer panel
  CHECK(inner == 1);   // 301x185 inner panel
  CHECK(bands == 1);   // 303x18 title band

  // Bottom-most left-column button rect: DITL (top 456, left 3) mapped by the
  // centered window origin (y=456-18=438, x=3+11=14), 145x25.
  float min_y = 1.0e9F;
  for (const auto &item : layout.items) {
    if (item.kind == DockedItemKind::kButton && item.rect.x < 400.0F) {
      min_y = std::min(min_y, item.rect.y);
    }
  }
  CHECK(min_y == Catch::Approx(-18.0F + 333.0F));
}

// The docked button physical order is the real Spaceport screen: LEFT column
// (top-to-bottom) = Bar, Mission BBS, Trade center, Repair; RIGHT column =
// Shipyard, Outfitter, Refuel, Leave. This pins the service<->grid lookups
// shared by BuildServiceButtons and the keyboard navigation.
TEST_CASE("docked services map to the real physical button order",
          "[landed_window]") {
  using game::LandedService;
  constexpr std::size_t Left = 0, Right = 1;
  // side, row -> service
  CHECK(game::NovaDialog_DockedServiceAt(Left, 0) == LandedService::kBar);
  CHECK(game::NovaDialog_DockedServiceAt(Left, 1) ==
        LandedService::kMissionBoard);
  CHECK(game::NovaDialog_DockedServiceAt(Left, 2) ==
        LandedService::kBuySellCargo);
  CHECK(game::NovaDialog_DockedServiceAt(Left, 3) ==
        LandedService::kRepair);
  CHECK(game::NovaDialog_DockedServiceAt(Right, 0) ==
        LandedService::kShipyard);
  CHECK(game::NovaDialog_DockedServiceAt(Right, 1) ==
        LandedService::kOutfit);
  CHECK(game::NovaDialog_DockedServiceAt(Right, 2) ==
        LandedService::kRefuel);
  CHECK(game::NovaDialog_DockedServiceAt(Right, 3) ==
        LandedService::kLaunch);

  // Inverse grid lookup agrees for each on-screen service; the no-slot starmap
  // has no grid position.
  CHECK(game::NovaDialog_DockedGridOf(LandedService::kBar) ==
        std::optional(std::pair{Left, 0}));
  CHECK(game::NovaDialog_DockedGridOf(LandedService::kLaunch) ==
        std::optional(std::pair{Right, 3}));
  CHECK(game::NovaDialog_DockedGridOf(LandedService::kStarmap) ==
        std::nullopt);
  // Every on-screen service round-trips to (side, row) and back.
  for (std::size_t side = 0; side < 2; ++side) {
    for (std::size_t row = 0; row < 4; ++row) {
      const auto svc = game::NovaDialog_DockedServiceAt(side, row);
      const auto pos = game::NovaDialog_DockedGridOf(svc);
      REQUIRE(pos);
      CHECK(pos->first == side);
      CHECK(pos->second == row);
    }
  }
}
