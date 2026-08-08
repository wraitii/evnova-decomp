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

  // The starter shuttle carries no inherited government (both inherited-gov
  // fields are -1 in the shipped data), so Ui_InstallGameplayInterfaceLayout's
  // ship-class lookup falls back to the Default interface (0x80). This pins
  // that fallback so the HUD still resolves for the out-of-the-box ship.
  CHECK(shuttle->inherent_attributes_govt == -1);
  CHECK(shuttle->inherent_combat_govt == -1);

  const auto layout = NovaResource_LoadGameplayInterfaceLayout(
      static_cast<std::uint16_t>(fed->interface_id));
  REQUIRE(layout.has_value());
  CHECK(layout->interface_bg_pict_id == 0x2be);
}

// ---- Gameplay viewport / HUD-anchor geometry ----------------------------
// Mirrors NovaView_UpdateGameplayViewport (0x00488380).

TEST_CASE("full 1024x768 canvas yields origin (512,384) and anchor (0,0)",
          "[interface][geometry]") {
  const HudPanelRect surface{0, 0, 1024, 768};
  const auto g = GameplayGeometry_FromSurface(surface);
  // The 1024x768 frame fills the surface exactly; no nudges trigger.
  CHECK(g.surface_rect.left == 0);
  CHECK(g.surface_rect.top == 0);
  CHECK(g.surface_rect.right == 1024);
  CHECK(g.surface_rect.bottom == 768);
  CHECK(g.frame_rect.left == 0);
  CHECK(g.frame_rect.top == 0);
  CHECK(g.frame_rect.right == 1024);
  CHECK(g.frame_rect.bottom == 768);
  CHECK(g.hud_panel_origin_x == 512);
  CHECK(g.hud_panel_origin_y == 384);
  CHECK(g.hud_panel_anchor_x == 0);
  CHECK(g.hud_panel_anchor_y == 0);
}

TEST_CASE("a classic 640x480 surface centres the frame off-canvas and nudges "
          "up/left",
          "[interface][geometry]") {
  // A 640x480 window: the 1024x768 frame centres with its top-left at
  // (-192, -144), then the narrow (640 < 768) and short (480 < 641) viewport
  // nudges shift it up by 0x3c and left by 0x3c.
  const HudPanelRect surface{0, 0, 640, 480};
  const auto g = GameplayGeometry_FromSurface(surface);
  CHECK(g.frame_rect.left == -192 - 0x3c);  // -252
  CHECK(g.frame_rect.top == -144 - 0x3c);   // -204
  CHECK(g.frame_rect.right == -252 + 1024); // 772
  CHECK(g.frame_rect.bottom == -204 + 768); // 564
  // Origin = centre of the nudged frame rect.
  CHECK(g.hud_panel_origin_x == (-252 + 772 + 1) / 2);
  CHECK(g.hud_panel_origin_y == (-204 + 564 + 1) / 2);
  // Anchor = origin - (512, 384).
  CHECK(g.hud_panel_anchor_x == g.hud_panel_origin_x - 512);
  CHECK(g.hud_panel_anchor_y == g.hud_panel_origin_y - 384);
}

TEST_CASE("a tall/narrow surface nudges left when short (height < 0x281)",
          "[interface][geometry]") {
  // Width 900 >= 768 so the y nudge (width<768) does NOT apply. Height 500 <
  // 641 so the x nudge (height<641) shifts the frame left by 0x3c, which moves
  // the derived HUD origin accordingly.
  const HudPanelRect surface{10, 20, 910, 520};
  const auto g = GameplayGeometry_FromSurface(surface);
  CHECK(g.frame_rect.left == 10 + (900 - 1024) / 2 - 0x3c); // -112
  CHECK(g.frame_rect.top == 20 + (500 - 768) / 2);          // -114 (no y nudge)
  // Origin = centre of the nudged frame.
  const int cx = (g.frame_rect.right + g.frame_rect.left + 1) / 2;
  const int cy = (g.frame_rect.bottom + g.frame_rect.top + 1) / 2;
  CHECK(g.hud_panel_origin_x == 400);
  CHECK(g.hud_panel_origin_y == 270);
  CHECK(g.hud_panel_origin_x == cx);
  CHECK(g.hud_panel_origin_y == cy);
}

// ---- Life-bar fill geometry ----------------------------------------------
// Mirrors NovaUi_DrawPlayerShieldBar / _ArmorBar / _FuelLevelBar: the fill axis
// is chosen by the panel rect's aspect (tall slot fills from the top down;
// wide slot fills from the right, depleting left).

TEST_CASE("tall life-bar slot fills from the top down", "[interface][hud]") {
  // The Federation shield panel (200,35)-(207,184) is 7x149 (tall).
  const HudPanelRect panel{200, 35, 207, 184};

  // Full -> the whole slot is filled.
  const HudBarFill full = HudBar_FillRect(panel, 1.0F);
  CHECK(full.left == 200);
  CHECK(full.top == 35);
  CHECK(full.width == 7);
  CHECK(full.height == 149);
  CHECK_FALSE(full.empty());

  // Half -> anchored to the top, grows down to the midpoint.
  const HudBarFill half = HudBar_FillRect(panel, 0.5F);
  CHECK(half.left == 200);
  CHECK(half.top == 35);
  CHECK(half.width == 7);
  // floor(149 * 0.5) = 74.
  CHECK(half.height == 74);

  // Depleted -> clamp to zero (empty fill, nothing drawn).
  const HudBarFill empty = HudBar_FillRect(panel, 0.0F);
  CHECK(empty.empty());
}

TEST_CASE("wide bar slot fills from the right, depleting left",
          "[interface][hud]") {
  // A hypothetical horizontal gauge (width > height), e.g. 200x40.
  const HudPanelRect panel{50, 100, 200, 140};
  CHECK(panel.width() > panel.height());

  const HudBarFill full = HudBar_FillRect(panel, 1.0F);
  CHECK(full.left == 50);
  CHECK(full.width == 150);
  CHECK(full.top == 100);
  CHECK(full.height == 40);

  const HudBarFill half = HudBar_FillRect(panel, 0.5F);
  // floor(150 * 0.5) = 75; right-anchored -> left = 200 - 75 = 125.
  CHECK(half.width == 75);
  CHECK(half.left == 125);
  CHECK(half.top == 100);
  CHECK(half.height == 40);

  const HudBarFill quarter = HudBar_FillRect(panel, 0.25F);
  // floor(150 * 0.25) = 37; left = 200 - 37 = 163.
  CHECK(quarter.width == 37);
  CHECK(quarter.left == 163);
}

TEST_CASE("bar fill clamps fraction and rejects invalid panels",
          "[interface][hud]") {
  const HudPanelRect panel{200, 35, 207, 184};
  // fraction > 1 is clamped to the full slot.
  const HudBarFill over = HudBar_FillRect(panel, 2.0F);
  CHECK(over.left == 200);
  CHECK(over.height == 149);
  // Negative fraction clamps to empty (nothing drawn).
  CHECK(HudBar_FillRect(panel, -1.0F).empty());
  // A degenerate/degenerate panel yields an empty fill.
  CHECK(HudBar_FillRect({0, 0, 0, 0}, 0.5F).empty());
  CHECK(HudBar_FillRect({10, 10, 5, 5}, 0.5F).empty());
}

} // namespace game
