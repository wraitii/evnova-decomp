#include <catch2/catch_test_macros.hpp>

#include "game/presentation_scale.hpp"
#include "game/spaceflight_view.hpp"
#include "sdl_platform.hpp"

// The flight scene is a full-window placement at the flight-scene scale `F`;
// the authored gameplay viewport is the window minus
// the drawn cockpit-strip reserve, divided by `F`. The same helper feeds the
// world draw and the raw-window-point click mapping, so these checks pin the
// neutral-at-1 regression and the `F` behaviour together.
//
// A windowless SdlPlatform reports the 1024x768 baseline playfield and a
// hud_scale of 1; the stock strip is 194x767, so the height fit caps the strip
// scale at 768/767 and the reserve rounds to 194.

TEST_CASE("neutral flight scene geometry reproduces the 1:1 viewport",
          "[flightscene]") {
  SdlPlatform platform;
  const game::FlightSceneGeometry geometry =
      game::FlightSceneGeometryFor(platform);
  CHECK(geometry.viewport_w == 1024 - 194);
  CHECK(geometry.viewport_h == 768);
  CHECK(geometry.placement.scale == 1.0F);
  CHECK(geometry.placement.dst.x == 0.0F);
  CHECK(geometry.placement.dst.y == 0.0F);
  CHECK(geometry.placement.dst.w == 1024.0F);
  CHECK(geometry.placement.dst.h == 768.0F);
  CHECK(geometry.placement.authored_size.x == 1024.0F);
  CHECK(geometry.placement.authored_size.y == 768.0F);
}

TEST_CASE("flight scene scale shrinks the authored viewport but fills the "
          "window",
          "[flightscene]") {
  SdlPlatform platform;
  platform.SetPresentationScale(game::PresentationScale{1.0F, 2.0F, 1.0F});
  const game::FlightSceneGeometry geometry =
      game::FlightSceneGeometryFor(platform);
  // Integer truncation matches the old window-point viewport at F = 1.
  CHECK(geometry.viewport_w == (1024 - 194) / 2);
  CHECK(geometry.viewport_h == 768 / 2);
  CHECK(geometry.placement.scale == 2.0F);
  CHECK(geometry.placement.dst.w == 1024.0F);
  CHECK(geometry.placement.dst.h == 768.0F);
  // Authored extent is the window divided by F.
  CHECK(geometry.placement.authored_size.x == 512.0F);
  CHECK(geometry.placement.authored_size.y == 384.0F);

  platform.SetPresentationScale(game::PresentationScale{1.0F, 0.5F, 1.0F});
  const game::FlightSceneGeometry zoomed_out =
      game::FlightSceneGeometryFor(platform);
  CHECK(zoomed_out.viewport_w == (1024 - 194) * 2);
  CHECK(zoomed_out.viewport_h == 768 * 2);
  CHECK(zoomed_out.placement.scale == 0.5F);
}

// `F` scales only the world: the UI scale is unrelated and must not move the
// viewport.
TEST_CASE("UI scale does not change the authored flight viewport at F = 1",
          "[flightscene]") {
  SdlPlatform platform;
  platform.SetPresentationScale(game::PresentationScale{2.0F, 1.0F, 1.0F});
  const game::FlightSceneGeometry geometry =
      game::FlightSceneGeometryFor(platform);
  // At the 1024x768 baseline the strip fit is height-capped at 768/767, so the
  // reserve is unchanged and the authored viewport matches the neutral case.
  CHECK(geometry.viewport_w == 1024 - 194);
  CHECK(geometry.viewport_h == 768);
}
