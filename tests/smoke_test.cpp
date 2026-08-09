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
        CHECK(std::ranges::any_of(img->rgba_pixels, [](std::uint8_t c) {
          return c != 0;
        }));
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
