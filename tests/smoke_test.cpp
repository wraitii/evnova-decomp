#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "nova_app.hpp"

#include <array>
#include <filesystem>

namespace {

// The original game data is kept local and unversioned; skip data-dependent
// tests when the archives are not present relative to the working directory.
[[nodiscard]] bool MenuArchivesAvailable() {
  return std::filesystem::exists("EV Nova/Nova Files/Nova Graphics 3.rez") ||
         std::filesystem::exists(
             "../../../EV Nova/Nova Files/Nova Graphics 3.rez");
}

} // namespace

TEST_CASE("test infrastructure is available") { CHECK(true); }

TEST_CASE("main-menu shortcuts retain the original action mapping") {
  CHECK(NovaCommand_TranslateByInputMap('n') == GameModeAction::new_game);
  CHECK(NovaCommand_TranslateByInputMap('o') == GameModeAction::open_pilot);
  CHECK(NovaCommand_TranslateByInputMap('p') == GameModeAction::preferences);
  CHECK(NovaCommand_TranslateByInputMap('a') == GameModeAction::starmap);
  CHECK(NovaCommand_TranslateByInputMap('q') == GameModeAction::quit);
  CHECK_FALSE(NovaCommand_TranslateByInputMap('z').has_value());
}

TEST_CASE("sprite metadata decodes from its documented big-endian layout") {
  constexpr std::array<std::byte, 12> bytes{
      std::byte{0x1f}, std::byte{0x74}, std::byte{0x1f}, std::byte{0x7e},
      std::byte{0x00}, std::byte{0x64}, std::byte{0x00}, std::byte{0x3c},
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x02},
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

TEST_CASE("main-menu style decodes native colors and 1024x768 button origins") {
  std::array<std::byte, 0x8a> bytes{};
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

  const auto style = NovaMainMenuStyle_Parse(bytes);

  REQUIRE(style);
  CHECK(style->menu_font_size == 9);
  CHECK(style->menu_bright.red == 0);
  CHECK(style->menu_bright.green == 255);
  CHECK(style->menu_bright.blue == 0);
  CHECK(style->menu_dim.green == 128);
  CHECK(style->button_origins[0].x == 349);
  CHECK(style->button_origins[0].y == 400);
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

  const auto startup = NovaResource_LoadPictData(0x83);
  REQUIRE(startup);
  CHECK(startup->size() == 384926);
}
