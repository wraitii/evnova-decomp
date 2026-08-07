#include "game/gameplay_interface.hpp"
#include "game/scenario_data.hpp"

#include <catch2/catch_test_macros.hpp>

namespace game {

// The interface-layout records are the game's cached HUD panel geometry. These
// tests pin the parser against the shipped Federation payload (interface 0x82,
// the starter ship class inherits this government). The values were verified
// directly from the raw archived bytes (big-endian shorts at the documented
// offsets from Ui_InstallGameplayInterfaceLayout 0x004cda50).

TEST_CASE("Federation gameplay interface layout decodes its panel rects",
          "[interface][data]") {
  const auto layout = NovaResource_LoadGameplayInterfaceLayout(0x82);
  REQUIRE(layout.has_value());

  // The three life-support bars sit at the left of the HUD strip as thin
  // vertical bars, stacked shield -> armor -> fuel (each ~8px wide).
  // Left column: shield (200..207), armor (216..223), fuel (234..241),
  // all spanning y 35..184.
  CHECK(layout->shield_panel.left == 200);
  CHECK(layout->shield_panel.top == 35);
  CHECK(layout->shield_panel.right == 207);
  CHECK(layout->shield_panel.bottom == 184);
  CHECK(layout->shield_panel.width() == 7);
  CHECK(layout->shield_panel.height() == 149);
  CHECK(layout->shield_panel.valid());

  CHECK(layout->armor_panel.left == 216);
  CHECK(layout->armor_panel.top == 35);
  CHECK(layout->armor_panel.right == 223);
  CHECK(layout->armor_panel.bottom == 184);
  CHECK(layout->armor_panel.valid());

  CHECK(layout->fuel_panel.left == 234);
  CHECK(layout->fuel_panel.top == 35);
  CHECK(layout->fuel_panel.right == 241);
  CHECK(layout->fuel_panel.bottom == 184);
  CHECK(layout->fuel_panel.valid());

  // Readout panels across the console: travel status (254..286),
  // weapon ammo (300..315), target status (330..442), cargo (458..552),
  // all top = 8, bottom = 184.
  CHECK(layout->travel_status_panel.left == 254);
  CHECK(layout->travel_status_panel.top == 8);
  CHECK(layout->travel_status_panel.right == 286);
  CHECK(layout->travel_status_panel.bottom == 184);
  CHECK(layout->travel_status_panel.valid());

  CHECK(layout->weapon_ammo_panel.left == 300);
  CHECK(layout->weapon_ammo_panel.top == 8);
  CHECK(layout->weapon_ammo_panel.right == 315);
  CHECK(layout->weapon_ammo_panel.bottom == 184);

  CHECK(layout->target_status_panel.left == 330);
  CHECK(layout->target_status_panel.top == 8);
  CHECK(layout->target_status_panel.right == 442);
  CHECK(layout->target_status_panel.bottom == 184);

  CHECK(layout->cargo_status_panel.left == 458);
  CHECK(layout->cargo_status_panel.top == 8);
  CHECK(layout->cargo_status_panel.right == 552);
  CHECK(layout->cargo_status_panel.bottom == 184);
}

TEST_CASE("Federation interface layout colors, font and background PICT",
          "[interface][data]") {
  const auto layout = NovaResource_LoadGameplayInterfaceLayout(0x82);
  REQUIRE(layout.has_value());

  // The colour slots carry the raw little-endian words from the archive:
  // slot0 `00 e3 f1 ff` -> 0xfff1e300 (value bar), slot1 `00 78 80 87` ->
  // 0x87807800 (dim label), and the bright-white / dim-gray status slots at
  // the +0x10/+0x14 entries: `00 ff ff ff` / `00 80 80 80`.
  CHECK(layout->color_word[0] == 0xfff1e300U);
  CHECK(layout->color_word[1] == 0x87807800U);
  CHECK(layout->color_word[2] == 0xffffff00U);
  CHECK(layout->color_word[3] == 0x80808000U);

  // Font family at +0x60 (a C string) and the two font sizes at +0xa0/+0xa2.
  CHECK(layout->font_family_name == "Geneva");
  CHECK(layout->font_size == 12);
  CHECK(layout->font_size_2 == 10);

  // Background cockpit PICT (layout +0xa4): the Federation interface art, id
  // 0x2be (702), clamped to >= 0x80.
  CHECK(layout->interface_bg_pict_id == 0x2be);
}

TEST_CASE("every shipped interface layout parses to valid rects",
          "[interface][data]") {
  // The archive exposes layout records for interfaces 0x80..0x86; each must
  // decode to a sane, non-empty shield/armor/fuel bar frame.
  for (std::uint16_t id = 0x80; id <= 0x86; ++id) {
    const auto layout = NovaResource_LoadGameplayInterfaceLayout(id);
    REQUIRE(layout.has_value());
    CHECK(layout->shield_panel.valid());
    CHECK(layout->armor_panel.valid());
    CHECK(layout->fuel_panel.valid());
    CHECK(layout->interface_bg_pict_id >= 0x80);
  }
}

TEST_CASE("missing interface layout returns nullopt", "[interface][data]") {
  // A synthetically absent interface id (no record) must not throw or return a
  // half-parsed layout.
  CHECK_FALSE(NovaResource_LoadGameplayInterfaceLayout(0x07fff).has_value());
}

// End-to-end: the starter Shuttle's government resolves to the Federation and
// its interface engine loads that government's HUD layout.
TEST_CASE("starter ship government selects the Federation interface layout",
          "[interface][data]") {
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());
  const ShipClass *shuttle = data.Ship(0x80);
  REQUIRE(shuttle != nullptr);
  const Government *fed = data.Government(0x80);
  REQUIRE(fed != nullptr);
  REQUIRE(fed->present);
  CHECK(fed->interface_id == 0x82);

  const auto layout = NovaResource_LoadGameplayInterfaceLayout(
      static_cast<std::uint16_t>(fed->interface_id));
  REQUIRE(layout.has_value());
  CHECK(layout->interface_bg_pict_id == 0x2be);
}

} // namespace game
