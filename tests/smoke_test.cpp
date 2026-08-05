#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "nova_app.hpp"
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

TEST_CASE("resource resolver locates the main-menu sound blips") {
  if (!MenuArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }

  // Hover blip = "snd " 600, select blip = "snd " 601. Both are the tiny
  // uncompressed 8-bit mono 'NONE' form (Ghidra DAT_007d24b8[0] == 600).
  const auto hover = NovaResource_LoadSndData(600);
  REQUIRE(hover);
  const auto hover_sound = NovaSound_Decode(*hover);
  REQUIRE(hover_sound);
  CHECK(hover_sound->sample_rate == 11127);
  const auto select = NovaResource_LoadSndData(601);
  REQUIRE(select);
  const auto select_sound = NovaSound_Decode(*select);
  REQUIRE(select_sound);
  CHECK(select_sound->sample_rate == 11127);

  // Row reveal start/finish = snd 602/603, both mono Apple IMA4.
  for (std::uint16_t sound_id = 602; sound_id <= 603; ++sound_id) {
    const auto bytes = NovaResource_LoadSndData(sound_id);
    REQUIRE(bytes);
    const auto decoded = NovaSound_Decode(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->sample_rate == 22050);
    CHECK(decoded->channel_count == 1);
    CHECK_FALSE(decoded->samples.empty());
    CHECK(decoded->samples.size() % 64 == 0);
  }
}

TEST_CASE("NovaSound_Decode expands 8-bit menu blips to 16-bit 'NONE' PCM") {
  // Minimal format-2 'NONE' payload: first short == 2, a one-entry rate table
  // at offset 6 returning sub-structure offset 14 (Ghidra FUN_004d6d30), the
  // 'NONE' byte at 14 + 0x14, then 8-bit samples from 14 + 0x16 = 36.
  std::vector<std::byte> bytes(36 + 2, std::byte{0x00});
  bytes[0] = std::byte{0x00};
  bytes[1] = std::byte{0x02}; // first short == 2 (8-bit form)
  bytes[4] = std::byte{0x00};
  bytes[5] = std::byte{0x01}; // one table entry
  bytes[6] = std::byte{0x80};
  bytes[7] = std::byte{0x51};  // rate marker 0x8051 at table_pos 6
  bytes[10] = std::byte{0x00}; // table value at table_pos+4 (bytes 10..13)
  bytes[13] = std::byte{0x0e}; // value == 14 -> data offset 14
  bytes[14 + 8] = std::byte{0x2b};
  bytes[14 + 9] = std::byte{0x77};
  bytes[14 + 10] = std::byte{0x45};
  bytes[14 + 11] = std::byte{0xd1};   // 11127.27 Hz, unsigned 16.16
  bytes[14 + 0x14] = std::byte{0x00}; // cVar1 == 0 -> 'NONE'
  bytes[36] = std::byte{0x80};        // first 8-bit sample (silence)
  bytes[37] = std::byte{0x00};

  const auto sound = NovaSound_Decode(bytes);
  REQUIRE(sound);
  CHECK(sound->channel_count == 1);
  CHECK(sound->sample_rate == 11127);
  REQUIRE(sound->samples.size() == 2);
  // 16-bit expansion mirrors FUN_004d6900: (v + 0x80) | (v + 0x80) << 8,
  // truncated to 16 bits. So 0x80 -> 0x100 -> 0x0100, and 0x00 -> 0x8080.
  CHECK(sound->samples[0] == static_cast<std::int16_t>(0x0100));
  CHECK(sound->samples[1] ==
        static_cast<std::int16_t>(static_cast<std::uint16_t>(0x8080)));
  CHECK_FALSE(NovaSound_Decode(std::vector<std::byte>(40, std::byte{0x00}))
                  .has_value());
}

TEST_CASE("NovaSound_Decode expands a mono Apple IMA4 packet") {
  // Format-1 extended sound header at +0x14, followed by one 34-byte Apple
  // IMA4 packet. A zero predictor/index and zero nibbles decode to 64 zeroes.
  std::vector<std::byte> bytes(0x54 + 34, std::byte{0x00});
  bytes[1] = std::byte{0x01};
  bytes[0x14 + 7] = std::byte{0x01}; // one channel (big-endian u32)
  bytes[0x14 + 8] = std::byte{0x56};
  bytes[0x14 + 9] = std::byte{0x22}; // 22050 in 16.16 fixed point
  bytes[0x14 + 0x14] = std::byte{0xfe};
  bytes[0x14 + 0x28] = std::byte{'i'};
  bytes[0x14 + 0x29] = std::byte{'m'};
  bytes[0x14 + 0x2a] = std::byte{'a'};
  bytes[0x14 + 0x2b] = std::byte{'4'};
  bytes[0x14 + 0x3f] = std::byte{0x10};

  const auto sound = NovaSound_Decode(bytes);

  REQUIRE(sound);
  CHECK(sound->sample_rate == 22050);
  CHECK(sound->channel_count == 1);
  CHECK(sound->samples == std::vector<std::int16_t>(64, 0));
}

TEST_CASE("NovaSound_Decode carries predictor low bits between IMA4 packets") {
  std::vector<std::byte> bytes(0x54 + 68, std::byte{0x00});
  bytes[1] = std::byte{0x01};
  bytes[0x14 + 7] = std::byte{0x01};
  bytes[0x14 + 8] = std::byte{0x56};
  bytes[0x14 + 9] = std::byte{0x22};
  bytes[0x14 + 0x14] = std::byte{0xfe};
  bytes[0x14 + 0x28] = std::byte{'i'};
  bytes[0x14 + 0x29] = std::byte{'m'};
  bytes[0x14 + 0x2a] = std::byte{'a'};
  bytes[0x14 + 0x2b] = std::byte{'4'};
  bytes[0x14 + 0x3f] = std::byte{0x10};
  // Packet 0 starts at predictor/index zero. Its final high nibble 7 advances
  // the predictor to 11, leaving low bits that QuickTime carries forward.
  bytes[0x54 + 33] = std::byte{0x70};
  // Packet 1 replaces the predictor high bits with 0x0100 and starts at step
  // index zero. Its first zero nibble has zero delta, so the first output is
  // 0x0100 | 11 = 267.
  bytes[0x54 + 34] = std::byte{0x01};
  bytes[0x54 + 35] = std::byte{0x00};

  const auto sound = NovaSound_Decode(bytes);

  REQUIRE(sound);
  REQUIRE(sound->samples.size() == 128);
  CHECK(sound->samples[64] == 267);
}

TEST_CASE("main-menu style exposes the logo anchor from c\\x9alr +0xe0") {
  constexpr std::array<std::byte, 0xe4> bytes{};
  const auto style = NovaMainMenuStyle_Parse(bytes);
  REQUIRE(style);
  // All-zero payload: origin defaults to (0,0) and stays readable.
  CHECK(style->logo_origin.x == 0);
  CHECK(style->logo_origin.y == 0);
}
