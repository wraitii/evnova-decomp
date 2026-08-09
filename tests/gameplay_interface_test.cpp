#include "game/gameplay_interface.hpp"

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

  // The resource uses QuickDraw order (top,left,bottom,right). The bars are
  // horizontal slots in the 194px-wide top-right strip.
  CHECK(layout->shield_panel.left == 35);
  CHECK(layout->shield_panel.top == 200);
  CHECK(layout->shield_panel.right == 184);
  CHECK(layout->shield_panel.bottom == 207);
  CHECK(layout->shield_panel.width() == 149);
  CHECK(layout->shield_panel.height() == 7);
  CHECK(layout->shield_panel.valid());

  CHECK(layout->armor_panel.left == 35);
  CHECK(layout->armor_panel.top == 216);
  CHECK(layout->armor_panel.right == 184);
  CHECK(layout->armor_panel.bottom == 223);
  CHECK(layout->armor_panel.valid());

  CHECK(layout->fuel_panel.left == 35);
  CHECK(layout->fuel_panel.top == 234);
  CHECK(layout->fuel_panel.right == 184);
  CHECK(layout->fuel_panel.bottom == 241);
  CHECK(layout->fuel_panel.valid());

  // Readouts stack vertically in the same strip and span x=8..184.
  CHECK(layout->travel_status_panel.left == 8);
  CHECK(layout->travel_status_panel.top == 254);
  CHECK(layout->travel_status_panel.right == 184);
  CHECK(layout->travel_status_panel.bottom == 286);
  CHECK(layout->travel_status_panel.valid());

  CHECK(layout->weapon_ammo_panel.left == 8);
  CHECK(layout->weapon_ammo_panel.top == 300);
  CHECK(layout->weapon_ammo_panel.right == 184);
  CHECK(layout->weapon_ammo_panel.bottom == 315);

  CHECK(layout->target_status_panel.left == 8);
  CHECK(layout->target_status_panel.top == 330);
  CHECK(layout->target_status_panel.right == 184);
  CHECK(layout->target_status_panel.bottom == 442);

  CHECK(layout->cargo_status_panel.left == 8);
  CHECK(layout->cargo_status_panel.top == 458);
  CHECK(layout->cargo_status_panel.right == 184);
  CHECK(layout->cargo_status_panel.bottom == 552);
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
  // The original's post-round adjustment produces floor(149 * 0.5) = 74.
  CHECK(half.height == 74);

  // Depleted -> clamp to zero (empty fill, nothing drawn).
  const HudBarFill empty = HudBar_FillRect(panel, 0.0F);
  CHECK(empty.empty());
}

} // namespace game
