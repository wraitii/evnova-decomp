#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/gameplay_interface.hpp"
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

TEST_CASE("sprite metadata decodes from its documented big-endian layout") {
  constexpr std::array<std::byte, 12> bytes{
      std::byte{0x1f},
      std::byte{0x74},
      std::byte{0x1f},
      std::byte{0x7e},
      std::byte{0x00},
      std::byte{0x64},
      std::byte{0x00},
      std::byte{0x3c},
      std::byte{0x00},
      std::byte{0x01},
      std::byte{0x00},
      std::byte{0x02},
  };

  const auto definition = NovaSpriteDefinition_Parse(bytes);

  REQUIRE(definition);
  CHECK(definition->sprites_resource_id == 0x1f74);
  CHECK(definition->mask_resource_id == 0x1f7e);
  CHECK(definition->tile_width == 100);
  CHECK(definition->tile_height == 60);
  CHECK(definition->tiles_x == 1);
  CHECK(definition->tiles_y == 2);
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

TEST_CASE("main-menu style decodes native colors and 1024x768 button origins") {
  std::array<std::byte, 0xf4> bytes{};
  bytes[0x4c] = std::byte{0x00};
  bytes[0x4d] = std::byte{0x09};
  bytes[0x4e] = std::byte{0x00};
  bytes[0x4f] = std::byte{0xff};
  bytes[0x50] = std::byte{0x00};
  bytes[0x52] = std::byte{0x00};
  bytes[0x53] = std::byte{0x80};
  bytes[0x54] = std::byte{0x00};
  bytes[0x72] = std::byte{0x01};
  bytes[0x73] = std::byte{0x5d};
  bytes[0x74] = std::byte{0x01};
  bytes[0x75] = std::byte{0x90};
  bytes[0xe4] = std::byte{0x01};
  bytes[0xe5] = std::byte{0xbc};
  bytes[0xe6] = std::byte{0x01};
  bytes[0xe7] = std::byte{0xd1};
  bytes[0xe8] = std::byte{0x01};
  bytes[0xe9] = std::byte{0x57};
  bytes[0xea] = std::byte{0x01};
  bytes[0xeb] = std::byte{0x8f};

  const auto style = NovaMainMenuStyle_Parse(bytes);

  REQUIRE(style);
  CHECK(style->menu_font_size == 9);
  CHECK(style->menu_bright.red == 0);
  CHECK(style->menu_bright.green == 255);
  CHECK(style->menu_bright.blue == 0);
  CHECK(style->menu_dim.green == 128);
  CHECK(style->button_origins[0].x == 349);
  CHECK(style->button_origins[0].y == 400);
  CHECK(style->center_preview_origin.x == 444);
  CHECK(style->center_preview_origin.y == 465);
  CHECK(style->row_reveal_origins[0].x == 343);
  CHECK(style->row_reveal_origins[0].y == 399);
}

TEST_CASE("resource resolver locates the real menu sprite definitions") {
  if (!MenuArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }

  // Button sprites 600-605 are 1x2-tile sheets; logo 606 is a 1x7 sheet with
  // no mask. These are the ids FUN_004ad960 loads into DAT_00596cb8.
  const auto definition_600 = NovaResource_LoadMainMenuSpriteDefinition(600);
  REQUIRE(definition_600);
  CHECK(definition_600->sprites_resource_id == 0x1f72);
  CHECK(definition_600->mask_resource_id == 0x1f7c);
  CHECK(definition_600->tile_width == 120);
  CHECK(definition_600->tile_height == 61);
  CHECK(definition_600->tiles_x == 1);
  CHECK(definition_600->tiles_y == 2);

  const auto definition_605 = NovaResource_LoadMainMenuSpriteDefinition(605);
  REQUIRE(definition_605);
  CHECK(definition_605->sprites_resource_id == 0x1f77);
  CHECK(definition_605->mask_resource_id == 0x1f81);

  const auto definition_606 = NovaResource_LoadMainMenuSpriteDefinition(606);
  REQUIRE(definition_606);
  CHECK(definition_606->sprites_resource_id == 0x1f4a);
  CHECK(definition_606->mask_resource_id == 0xffff);
  CHECK(definition_606->tile_width == 654);
  CHECK(definition_606->tile_height == 209);
  CHECK(definition_606->tiles_y == 7);

  const auto definition_607 = NovaResource_LoadMainMenuSpriteDefinition(607);
  REQUIRE(definition_607);
  CHECK(definition_607->sprites_resource_id == 0x1f54);
  CHECK(definition_607->tile_width == 136);
  CHECK(definition_607->tile_height == 98);
  CHECK(definition_607->tiles_y == 7);

  constexpr std::array<std::uint16_t, 3> reveal_frame_counts{11, 10, 11};
  for (std::size_t row = 0; row < reveal_frame_counts.size(); ++row) {
    const auto definition = NovaResource_LoadMainMenuSpriteDefinition(
        static_cast<std::uint16_t>(608 + row));
    REQUIRE(definition);
    CHECK(definition->mask_resource_id == 0xffff);
    CHECK(definition->tiles_x == 1);
    CHECK(definition->tiles_y == reveal_frame_counts[row]);
  }
}

TEST_CASE("real main-menu RLE sprite sheets decode two frames") {
  if (!MenuArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }

  for (std::uint16_t sprite_id = 600; sprite_id <= 605; ++sprite_id) {
    const auto definition =
        NovaResource_LoadMainMenuSpriteDefinition(sprite_id);
    REQUIRE(definition);
    const auto bytes = NovaResource_Load(kResourceTypeRleSheet16,
                                         definition->sprites_resource_id);
    REQUIRE(bytes);
    const auto sheet = RleSpriteSheet_Decode16(*bytes);
    REQUIRE(sheet);
    CHECK(sheet->width == definition->tile_width);
    CHECK(sheet->height == definition->tile_height);
    REQUIRE(sheet->frames.size() == 2);
    CHECK(std::ranges::any_of(
        sheet->frames[0].rgba_pixels,
        [](std::uint8_t component) { return component != 0; }));
    CHECK(sheet->frames[0].rgba_pixels != sheet->frames[1].rgba_pixels);
  }
}

TEST_CASE("resource resolver decodes the real colors main-menu style") {
  if (!MenuArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }

  const auto style = NovaResource_LoadMainMenuStyle();
  REQUIRE(style);
  // The actual menu colors are green (0,255,0) bright and (0,128,0) dim.
  CHECK(style->menu_font_size == 9);
  CHECK(style->menu_bright.red == 0);
  CHECK(style->menu_bright.green == 255);
  CHECK(style->menu_bright.blue == 0);
  CHECK(style->menu_dim.green == 128);
  // Two columns x three rows on the 1024x768 backdrop.
  CHECK(style->button_origins[0].x == 349);
  CHECK(style->button_origins[0].y == 400);
  CHECK(style->button_origins[2].x == 345);
  CHECK(style->button_origins[2].y == 528);
  CHECK(style->button_origins[3].x == 555);
  CHECK(style->button_origins[3].y == 401);
  CHECK(style->button_origins[5].x == 580);
  CHECK(style->button_origins[5].y == 528);
}

TEST_CASE("resource resolver locates the splash PICTs in Nova Titles 1") {
  if (!MenuArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }

  // PICT 0x1fa4 ("Atmos/ambrosia", 369x558) is the loading splash; PICT 0x83
  // (832x624) is the Ambrosia startup splash. Both are 16-bit PICTs.
  const auto loading = NovaResource_LoadPictData(0x1fa4);
  REQUIRE(loading);
  CHECK(loading->size() == 190550);
  const auto loading_image = Resource_LoadPictAsImage(*loading);
  REQUIRE(loading_image);
  CHECK(loading_image->width == 369);
  CHECK(loading_image->height == 558);
  CHECK(loading_image->rgba_pixels.size() == 369U * 558U * 4U);

  const auto startup = NovaResource_LoadPictData(0x83);
  REQUIRE(startup);
  CHECK(startup->size() == 384926);
  const auto startup_image = Resource_LoadPictAsImage(*startup);
  REQUIRE(startup_image);
  CHECK(startup_image->width == 832);
  CHECK(startup_image->height == 624);
  CHECK(startup_image->rgba_pixels.size() == 832U * 624U * 4U);
}

TEST_CASE("main-menu backdrop and logo PICTs decode to the 1024x768 space") {
  if (!MenuArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }

  // Backdrop PICT 0x1f40 is the full 1024x768 ship-interior scene; the logo
  // PICT 0x1f4a is a 7-frame vertical stack of 654x209 tiles (sp\x95n 606).
  const auto backdrop = NovaResource_LoadMainMenuBackdropData();
  REQUIRE(backdrop);
  const auto backdrop_image = Resource_LoadPictAsImage(*backdrop);
  REQUIRE(backdrop_image);
  CHECK(backdrop_image->width == 1024);
  CHECK(backdrop_image->height == 768);
  CHECK(backdrop_image->rgba_pixels.size() == 1024U * 768U * 4U);

  const auto logo = NovaResource_LoadMainMenuLogoData();
  REQUIRE(logo);
  const auto logo_image = Resource_LoadPictAsImage(*logo);
  REQUIRE(logo_image);
  CHECK(logo_image->width == 654);
  CHECK(logo_image->height == 209 * 7);
  CHECK(logo_image->rgba_pixels.size() == 654U * (209U * 7U) * 4U);
}

TEST_CASE("HUD cockpit PICTs decode to the 194x767 status-bar strip") {
  if (!MenuArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }

  // The gameplay HUD "status bar" cockpit art is a narrow, full-height strip:
  // PICT 0x2bc (Default), 0x2c0 (Auroran), 0x2c2 (Vell-os). Each interface
  // layout names its own cockpit PICT (payload +0xa4).
  for (const auto id : {0x2bc, 0x2c0, 0x2c2}) {
    const auto data = NovaResource_LoadPictData(static_cast<std::uint16_t>(id));
    REQUIRE(data);
    const auto img = Resource_LoadPictAsImage(*data);
    REQUIRE(img);
    CHECK(img->width == 194);
    CHECK(img->height == 767);
    CHECK(img->rgba_pixels.size() == 194U * 767U * 4U);
  }
}

TEST_CASE("HUD interface layout names a decodable gov cockpit PICT") {
  if (!MenuArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  // The Default interface (0x80) backs the starter ship's HUD; its cockpit
  // PICT must exist and decode so the HUD chrome is present, not just bars.
  const auto layout = game::NovaResource_LoadGameplayInterfaceLayout(0x80);
  REQUIRE(layout);
  REQUIRE(layout->interface_bg_pict_id >= 0x80);
  const auto data =
      NovaResource_LoadPictData(layout->interface_bg_pict_id);
  REQUIRE(data);
  const auto img = Resource_LoadPictAsImage(*data);
  REQUIRE(img);
  CHECK(img->width == 194);
  CHECK(img->height == 767);
}
