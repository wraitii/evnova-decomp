#include "brgr_archive.hpp"
#include "pict_image.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

// TEMPORARY new-pilot dialog resource checks (dropped once the port settles).

namespace {

bool ArchivesPresent() {
  return std::filesystem::exists("EV Nova/Nova.rez");
}

} // namespace

TEST_CASE("Type-7/0x40 DITL tails carry refcons (new-pilot dialog)",
          "[new_pilot_dialog]") {
  if (!ArchivesPresent()) {
    SKIP("Nova .rez archives not present");
  }
  const auto items = NovaResource_LoadDialogItems(0xc1d);
  REQUIRE(items.has_value());
  REQUIRE(items->size() == 14);

  // Row 11 (item 10) -> MENU 0x1f4 "Gender"; row 13 (item 12) -> MENU 0x1f5.
  CHECK((*items)[10].type == 7);
  CHECK((*items)[10].refcon == 0x1f4);
  CHECK((*items)[12].type == 7);
  CHECK((*items)[12].refcon == 0x1f5);
  CHECK((*items)[2].type == 0x40);
  CHECK((*items)[2].refcon == 0x81); // PICT 129: Strict Play note art
  CHECK((*items)[13].type == 0x40);
  CHECK((*items)[13].refcon == 0x82); // PICT 130: pilot icon
  CHECK((*items)[3].type == 5);
  CHECK((*items)[3].title == "Strict Play");
  CHECK((*items)[7].type == 0x10);
  CHECK((*items)[8].type == 0x10);
}

TEST_CASE("MENU 0x1f4 is the Gender popup; 0x1f5 ships empty",
          "[new_pilot_dialog]") {
  if (!ArchivesPresent()) {
    SKIP("Nova .rez archives not present");
  }
  const auto gender = NovaResource_LoadMenuDefinition(0x1f4);
  REQUIRE(gender.has_value());
  CHECK(gender->title == "Gender");
  REQUIRE(gender->entries.size() == 2);
  CHECK(gender->entries[0] == "Male");
  CHECK(gender->entries[1] == "Female");

  const auto character = NovaResource_LoadMenuDefinition(0x1f5);
  REQUIRE(character.has_value());
  CHECK(character->title == "Character");
  CHECK(character->entries.empty());
}

TEST_CASE("PICT 0x81/0x82/0x8b decode (PackBitsRect + PackBitsRgn)",
          "[new_pilot_dialog]") {
  if (!ArchivesPresent()) {
    SKIP("Nova .rez archives not present");
  }
  // PICT 129 "Strict Play" note art: 0x98 PackBitsRect, 8-bit indexed,
  // rowBytes 0x80b8, 256-entry ColorTable with sequential ctFlags 0x8000.
  const auto strict_play = Resource_LoadPictAsImage(*NovaResource_LoadPictData(0x81));
  REQUIRE(strict_play.has_value());
  CHECK(strict_play->width == 182);
  CHECK(strict_play->height == 22);
  CHECK(strict_play->rgba_pixels[0] == 255);       // palette[0] white
  CHECK(strict_play->rgba_pixels[1] == 255);
  CHECK(strict_play->rgba_pixels[2] == 255);
  CHECK(strict_play->rgba_pixels[3] == 255);

  // PICT 130 pilot icon: 0x98 PackBitsRect, 8-bit indexed, 1-byte row
  // prefixes (rowBytes 32), explicit sequential palette values.
  const auto icon = Resource_LoadPictAsImage(*NovaResource_LoadPictData(0x82));
  REQUIRE(icon.has_value());
  CHECK(icon->width == 32);
  CHECK(icon->height == 32);
  CHECK(icon->rgba_pixels[0] == 102); // palette[129]: border grey
  CHECK(icon->rgba_pixels[4] == 34);  // palette[253]: dark fill
  CHECK(icon->rgba_pixels[(16 * 32 + 16) * 4 + 0] == 68); // palette[252] centre

  // PICT 0x8b Key Settings backdrop: 0x99 PackBitsRgn, 1-bit, 2-entry color
  // table, 1-byte row prefixes (masked rowBytes 74), 10-byte region skip.
  const auto backdrop = Resource_LoadPictAsImage(*NovaResource_LoadPictData(0x8b));
  REQUIRE(backdrop.has_value());
  CHECK(backdrop->width == 582);
  CHECK(backdrop->height == 307);
}

TEST_CASE("ch r census: one hidden .Trader template (stock Nova)",
          "[new_pilot_dialog]") {
  if (!ArchivesPresent()) {
    SKIP("Nova .rez archives not present");
  }
  std::vector<std::pair<std::uint32_t, std::uint16_t>> chars;
  for (const auto &[type, id] : NovaResource_AllKeys()) {
    if (type == kResourceTypeCharacter) {
      chars.emplace_back(type, id);
    }
  }
  REQUIRE(chars.size() == 1);
  const auto named =
      NovaResource_LoadNamed(kResourceTypeCharacter, chars[0].second);
  REQUIRE(named.has_value());
  CHECK(named->name == ".Trader");

  const auto block = NovaResource_Load(kResourceTypeCharacter, 0x0080);
  REQUIRE(block.has_value());
  // ShipType at +4: 0x80 -> start class 0 (minus 0x80).
  const std::uint16_t ship_type = static_cast<std::uint16_t>(
      (std::to_integer<unsigned>((*block)[4]) << 8U) |
      std::to_integer<unsigned>((*block)[5]));
  CHECK(ship_type == 0x80);
}
