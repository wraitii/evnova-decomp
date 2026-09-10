#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "pict_image.hpp"
#include "rle_sprite_sheet.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

namespace {

// The original game data is kept local and unversioned; skip data-dependent
// tests when the archives are not present relative to the working directory.
[[nodiscard]] bool MenuArchivesAvailable() {
  return std::filesystem::exists("EV Nova/Nova Files/Nova Graphics 3.rez") ||
         std::filesystem::exists(
             "../../../EV Nova/Nova Files/Nova Graphics 3.rez");
}

} // namespace

// The three-state button strips (0x1d4c..0x1d54) are nine PICTs: for each of
// normal/pressed/grey a 13px left cap, a 2px-wide stretchable middle tile, and
// a 13px right cap (NovaUi_InitThreeStateButtonArt, 0x004a2f50). The middle
// tiles are only 2px wide so their PICT rows are rowBytes = 4, which the game's
// row decoder FUN_004fcc00 stores RAW (the `param_3 < 8` branch) rather than
// length-prefixed packbits. Regression: they must decode to 2x25, not fail.
TEST_CASE("button strip middle tiles decode via the raw-row path") {
  if (!MenuArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  // (state) -> PICT ids {left, middle, right}.
  const std::array<std::array<std::uint16_t, 3>, 3> strips{{
      {{0x1d4c, 0x1d4d, 0x1d4e}},
      {{0x1d4f, 0x1d50, 0x1d51}},
      {{0x1d52, 0x1d53, 0x1d54}},
  }};
  for (std::size_t state = 0; state < strips.size(); ++state) {
    for (std::size_t piece = 0; piece < 3; ++piece) {
      const std::uint16_t id = strips[state][piece];
      const auto data = NovaResource_LoadPictData(id);
      REQUIRE(data);
      const auto img = Resource_LoadPictAsImage(*data);
      REQUIRE(img);
      // Caps are 13px wide, middle tiles 2px; all are 25px tall.
      const std::size_t expect_w = (piece == 1) ? 2U : 13U;
      CHECK(img->width == static_cast<int>(expect_w));
      CHECK(img->height == 25);
      CHECK(img->rgba_pixels.size() == expect_w * 25U * 4U);
      // Middle tiles carry real non-flat button pixels (not a null fallback).
      if (piece == 1) {
        CHECK(std::ranges::any_of(img->rgba_pixels,
                                  [](std::uint8_t c) { return c != 0; }));
      }
    }
  }
}

TEST_CASE("16-bit RLE sprite commands decode literal RGB555 pixels") {
  constexpr std::array<std::byte, 32> bytes{
      std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x01},
      std::byte{0x00}, std::byte{0x10}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x08},
      std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04},
      std::byte{0x7c}, std::byte{0x00}, std::byte{0x03}, std::byte{0xe0},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  };

  const auto sheet = RleSpriteSheet_Decode16(bytes);

  REQUIRE(sheet);
  REQUIRE(sheet->frames.size() == 1);
  CHECK(sheet->width == 2);
  CHECK(sheet->height == 1);
  CHECK(sheet->frames[0].rgba_pixels ==
        std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255});
}

// c\x9alr stores each color as a big-endian 0x00RRGGBB long, so the channels
// are bytes +1/+2/+3 from the field offset. A byte-order slip (reading
// +0/+1/+2) turns the store grid's gray/red into olive/green and the main-menu
// palette into green, which is the bug this locks down.
TEST_CASE("color-resource colors are read as big-endian RGB") {
  std::vector<std::byte> data(0x9e, std::byte{0});
  data[0x4d] = std::byte{9}; // menu font size (big-endian at 0x4c)
  // menu_bright +0x4e = 00 aa bb cc -> red=aa green=bb blue=cc
  data[0x4f] = std::byte{0xaa};
  data[0x50] = std::byte{0xbb};
  data[0x51] = std::byte{0xcc};
  // grid_bright +0x56 = 00 11 22 33
  data[0x57] = std::byte{0x11};
  data[0x58] = std::byte{0x22};
  data[0x59] = std::byte{0x33};
  // grid_dim +0x5a = 00 44 55 66
  data[0x5b] = std::byte{0x44};
  data[0x5c] = std::byte{0x55};
  data[0x5d] = std::byte{0x66};
  // list palette +0x8a/+0x8e/+0x92/+0x96/+0x9a
  data[0x8b] = std::byte{0x01};
  data[0x8c] = std::byte{0x02};
  data[0x8d] = std::byte{0x03};
  data[0x8f] = std::byte{0x04};
  data[0x90] = std::byte{0x05};
  data[0x91] = std::byte{0x06};
  data[0x93] = std::byte{0x07};
  data[0x94] = std::byte{0x08};
  data[0x95] = std::byte{0x09};
  data[0x97] = std::byte{0x0a};
  data[0x98] = std::byte{0x0b};
  data[0x99] = std::byte{0x0c};
  data[0x9b] = std::byte{0x0d};
  data[0x9c] = std::byte{0x0e};
  data[0x9d] = std::byte{0x0f};

  const auto style = NovaMainMenuStyle_Parse(data);
  REQUIRE(style);
  CHECK(style->menu_bright.red == 0xaa);
  CHECK(style->menu_bright.green == 0xbb);
  CHECK(style->menu_bright.blue == 0xcc);
  CHECK(style->grid_bright.red == 0x11);
  CHECK(style->grid_bright.green == 0x22);
  CHECK(style->grid_bright.blue == 0x33);
  CHECK(style->grid_dim.red == 0x44);
  CHECK(style->grid_dim.green == 0x55);
  CHECK(style->grid_dim.blue == 0x66);
  CHECK(style->floating_map.red == 0x01);
  CHECK(style->floating_map.green == 0x02);
  CHECK(style->floating_map.blue == 0x03);
  CHECK(style->list_text.red == 0x04);
  CHECK(style->list_text.green == 0x05);
  CHECK(style->list_text.blue == 0x06);
  CHECK(style->list_background.red == 0x07);
  CHECK(style->list_background.green == 0x08);
  CHECK(style->list_background.blue == 0x09);
  CHECK(style->list_hilite.red == 0x0a);
  CHECK(style->list_hilite.green == 0x0b);
  CHECK(style->list_hilite.blue == 0x0c);
  CHECK(style->escort_hilite.red == 0x0d);
  CHECK(style->escort_hilite.green == 0x0e);
  CHECK(style->escort_hilite.blue == 0x0f);
}
