#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/docked_dialog.hpp"
#include "game/landed_window.hpp"
#include "pict_image.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
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

// The docked inner panel is filled with the landing stellar's description,
// loaded from the stellar's "desc" landing-description block (Ghidra
// Ui_LoadSelectionDialogResource, key 0x64917363) keyed by the raw stellar
// resource id. Regression: the leading C-string parses into the description
// text (and the 2-byte BE variant + trailing status follow the text, matching
// the decompiled layout).
TEST_CASE("landing description loader decodes the desc block",
          "[landed_window]") {
  if (!std::filesystem::exists("EV Nova/Nova.rez") &&
      !std::filesystem::exists("EV Nova/Nova Files/Nova.rez") &&
      !std::filesystem::exists("../../../EV Nova/Nova.rez") &&
      !std::filesystem::exists("../../../EV Nova/Nova Files/Nova.rez")) {
    SKIP("Nova.rez not present");
  }

  // Stellar 0x80 (Earth) and 0x9d (Viking, the starting Tichel landing
  // stellar) both carry real landing descriptions. Compute plain boolean
  // results first (the assertions just check those scalars); the leading text
  // must be non-empty and start with the stellar name.
  {
    const auto earth = NovaResource_LoadStellarDescription(0x80);
    const bool earth_ok =
        earth.has_value() && !earth->text.empty() &&
        earth->text.starts_with("Earth");
    CHECK(earth_ok);
  }
  {
    const auto viking = NovaResource_LoadStellarDescription(0x9d);
    const bool viking_ok =
        viking.has_value() && !viking->text.empty() &&
        viking->text.starts_with("Viking");
    CHECK(viking_ok);
  }

  // An id with no desc block (e.g. 0x7f, below the resource range) yields
  // nothing rather than a crash.
  const bool missing_ok = !NovaResource_LoadStellarDescription(0x7f).has_value();
  CHECK(missing_ok);
}

// The docked landing-description panel is word-wrapped. The wrap must NOT
// "cumulatively append": each new line must start fresh (with only its own
// words), never re-embed the already-drained earlier line. Regression for a
// bug where a wrapping line re-appended the drained text onto the next line,
// making every subsequent line repeat all prior words.
TEST_CASE("landing description word-wrap produces distinct lines",
          "[landed_window]") {
  using game::WrapDescriptionLines;
  auto width = [](std::string_view s) { return static_cast<int>(s.size()); };

  const std::string text =
      "The warriors on this station hold themselves ready to deal with any "
      "large-scale threat to the sovereignty of the Polaris.";
  const auto lines = WrapDescriptionLines(text, 40, width);
  REQUIRE(lines.size() >= 3);

  // Each wrapped line fits the row (except a single-word overflow, not the
  // case here) and no line is a substring of a later line.
  for (std::size_t i = 0; i < lines.size(); ++i) {
    CHECK(static_cast<int>(lines[i].size()) <= 40);
    for (std::size_t j = i + 1; j < lines.size(); ++j) {
      CHECK_FALSE(lines[j].starts_with(lines[i]));
      CHECK(lines[j].find(lines[i]) == std::string::npos);
    }
  }
  // Rejoining the wrapped lines with a single separating space must reproduce
  // the original text exactly (the wrap only ever drops the whitespace between
  // a line's last word and the next, never any word characters).
  {
    std::string rejoined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i != 0) {
        rejoined.push_back(' ');
      }
      rejoined += lines[i];
    }
    CHECK(rejoined == text);
  }

  // A single over-wide word is emitted on its own (unsplit) line.
  const auto long_word = WrapDescriptionLines("supercalifragilistic", 4, width);
  REQUIRE(long_word.size() == 1);
  CHECK(long_word[0] == "supercalifragilistic");
}

TEST_CASE("normal landing arrival charges once and restores the docked ship",
          "[landed_window]") {
  // SDL-free core of Stellar_TravelToSystem's normal-arrival bookkeeping.
  // The target is deliberately an inactive (not currently rendered) stellar:
  // StellarTargetsSpriteSetActive accepts the matching inactive state unless
  // the stellar's 0x80 engaged bit is set.
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Testport";
  stellar.pos_x = 123;
  stellar.pos_y = -456;
  stellar.flags = 0x1;
  stellar.is_available = true;
  stellar.system_id = 0;
  stellar.service_cost = 75;
  state.scenario.ships.resize(1);
  state.scenario.ships[0].base_shield = 300;
  state.scenario.ships[0].base_armor = 250;

  state.player.current_system_id = 0;
  state.player.ship_class_id = 0;
  state.player.credits = 100;
  state.player.pos_x = 3.0F;
  state.player.pos_y = -300.0F;
  state.player.vel_x = 2.0F;
  state.player.vel_y = -1.0F;
  state.player.speed = 4.0F;
  state.player.shield_points = 1.0F;
  state.player.armor_points = 2.0F;
  state.travel.selected_stellar_id = 0x80;

  game::LandedContext ctx;
  REQUIRE(game::NovaLanding_EnterDocked(state, ctx));
  CHECK(ctx.landed);
  CHECK(ctx.stellar_id == 0x80);
  CHECK(state.travel.landed_this_frame);
  CHECK(state.player.credits == 25);
  CHECK(state.player.pos_x == 123.0F);
  CHECK(state.player.pos_y == -456.0F);
  CHECK(state.player.vel_x == 0.0F);
  CHECK(state.player.vel_y == 0.0F);
  CHECK(state.player.speed == 0.0F);
  CHECK(state.player.shield_points == 300.0F);
  CHECK(state.player.armor_points == 250.0F);
}

// The sub-window dialogs render each docked service's frame PICT over the dock
// (NovaDocked_SubWindowFramePict). Every documented Nova Graphics 3 frame id
// must resolve to a decodable PICT so the dialog has real art rather than the
// placeholder panel.
TEST_CASE("docked sub-window frame PICTs decode from Nova Graphics",
          "[landed_window][docked_dialog]") {
  if (!std::filesystem::exists("EV Nova/Nova.rez") &&
      !std::filesystem::exists("EV Nova/Nova Files/Nova.rez") &&
      !std::filesystem::exists("../../../EV Nova/Nova.rez") &&
      !std::filesystem::exists("../../../EV Nova/Nova Files/Nova.rez")) {
    SKIP("Nova.rez not present");
  }
  const auto services = {game::LandedService::kShipyard,
                         game::LandedService::kOutfit,
                         game::LandedService::kBar,
                         game::LandedService::kMissionBoard,
                         game::LandedService::kStarmap,
                         game::LandedService::kBuySellCargo};
  for (const auto s : services) {
    const auto id = game::NovaDocked_SubWindowFramePict(s);
    REQUIRE(id != 0);
    const auto data = NovaResource_LoadPictData(id);
    REQUIRE(data);
    const auto img = Resource_LoadPictAsImage(*data);
    REQUIRE(img.has_value());
    REQUIRE(img->width > 0);
    REQUIRE(img->height > 0);
  }
}
