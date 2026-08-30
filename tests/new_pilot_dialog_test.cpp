#include "brgr_archive.hpp"

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
