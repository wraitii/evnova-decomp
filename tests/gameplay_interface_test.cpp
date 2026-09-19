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
// is chosen by the panel rect's aspect (height < width -> wide, else tall),
// and the fill anchor is the left edge (wide) or the bottom edge (tall). The
// original stores `trunc(anchor -/+ extent)` into the moved edge, and the x87
// FIST + residual/sign correction truncates toward zero (see
// docs/x87_precision.md), so the wide extent is floor(width*fraction)
// and the tall extent is ceil(height*fraction).

TEST_CASE("tall life-bar slot fills from the bottom up", "[interface][hud]") {
  // The Federation shield panel (200,35)-(207,184) is 7x149 (tall).
  const HudPanelRect panel{200, 35, 207, 184};

  // Full -> the whole slot is filled.
  const HudBarFill full = HudBar_FillRect(panel, 1.0F);
  CHECK(full.left == 200);
  CHECK(full.top == 35);
  CHECK(full.width == 7);
  CHECK(full.height == 149);
  CHECK_FALSE(full.empty());

  // Half -> anchored at the bottom: top = 184 - ceil(149*0.5) = 109, height 75.
  const HudBarFill half = HudBar_FillRect(panel, 0.5F);
  CHECK(half.left == 200);
  CHECK(half.top == 109);
  CHECK(half.width == 7);
  CHECK(half.height == 75);

  // Depleted -> empty (nothing drawn).
  CHECK(HudBar_FillRect(panel, 0.0F).empty());
}

// NovaUi_DrawPlayerFuelLevelBar (0x0045f086): sVar7 = trunc(fuel/100) (the
// residual/sign correction at 0x0045f2a6 makes the FIST truncate), and the
// reserve segment runs from floor(sVar7*100/capacity) to the usable edge; the
// wide slot moves its LEFT edge, so the segment is the fuel above the last full
// hundred. Capacity 200 keeps the hundred mark exact.
TEST_CASE("fuel reserve segment uses the truncated hundred mark",
          "[interface][hud]") {
  const HudPanelRect panel{0, 10, 200, 20}; // 200x10 wide
  constexpr float kCapacity = 200.0F;

  const auto reserve = [&](float fuel) {
    return HudBar_FuelReserveFill(panel, fuel, kCapacity);
  };

  // Below one hundred: sVar7 = 0, so the reserve is the whole usable fill.
  CHECK(reserve(49.0F).left == 0.0F);
  CHECK(reserve(49.0F).width == 49.0F);
  CHECK(reserve(50.0F).left == 0.0F);
  CHECK(reserve(50.0F).width == 50.0F);
  CHECK(reserve(51.0F).left == 0.0F);
  CHECK(reserve(51.0F).width == 51.0F);
  CHECK(reserve(99.0F).left == 0.0F);
  CHECK(reserve(99.0F).width == 99.0F);

  // Exactly one hundred: sVar7 = 1 and the segment is empty (the usable fill
  // alone is drawn).
  CHECK(reserve(100.0F).empty());

  // Above one hundred: the left edge jumps to the 100-ton mark (100px) and the
  // segment covers the remainder.
  CHECK(reserve(149.0F).left == 100.0F);
  CHECK(reserve(149.0F).width == 49.0F);
  CHECK(reserve(150.0F).left == 100.0F);
  CHECK(reserve(150.0F).width == 50.0F);
  CHECK(reserve(151.0F).left == 100.0F);
  CHECK(reserve(151.0F).width == 51.0F);

  // Orientation: a tall panel moves the BOTTOM edge and leaves the top at the
  // usable edge. For fuel 150/200 the usable top is 200-ceil(150)=50 and the
  // reserve bottom is 200-ceil(100)=100.
  const HudPanelRect tall{0, 0, 10, 200}; // 10x200 tall
  const HudBarFill tall_reserve =
      HudBar_FuelReserveFill(tall, 150.0F, kCapacity);
  CHECK(tall_reserve.left == 0.0F);
  CHECK(tall_reserve.width == 10.0F);
  CHECK(tall_reserve.top == 50.0F);
  CHECK(tall_reserve.height == 50.0F);
}

} // namespace game
