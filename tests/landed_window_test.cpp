#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/landed_window.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

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
