#include <catch2/catch_test_macros.hpp>

#include "game/presentation_scale.hpp"
#include "game/route_map.hpp"
#include "sdl_platform.hpp"

// Ghidra NovaUi_InitializeFlightViewSurfaces (0x004ab9d4) builds the route-map
// overlay rect as a top-left square of side round(view_width * 0.25) clamped to
// min 200 (DAT_00575a80 is the double 0.25 at 0x00575a80; its only reader is
// the `FMUL double ptr` at 0x004abc66). A windowless SdlPlatform reports the
// 1024x768 baseline playfield, so the side must be 256 -- the earlier 0.5 x
// fixed-640 stand-in produced 320 and shifted the chart centre.
TEST_CASE("route map overlay is a top-left square scaled from the view width",
          "[routemap]") {
  SdlPlatform platform;
  const SDL_FRect rect = game::RouteMap_OverlayRect(platform);
  CHECK(rect.x == 0.0F);
  CHECK(rect.y == 0.0F);
  CHECK(rect.w == 256.0F);
  CHECK(rect.h == 256.0F);
}

// The overlay placement is shared by the draw and the click hit-test:
// top-left anchored, authored square `(0,0,b,b)` mapped by
// `s_map = min(U, W/b, H/b)`. At U = 1 it must reproduce the old
// window-point rect exactly; a larger U enlarges it within the window fit.
TEST_CASE("route map overlay placement is top-left and honours the UI scale",
          "[routemap]") {
  SdlPlatform platform;
  const Placement neutral = game::RouteMap_OverlayPlacement(platform);
  CHECK(neutral.dst.x == 0.0F);
  CHECK(neutral.dst.y == 0.0F);
  CHECK(neutral.dst.w == 256.0F);
  CHECK(neutral.dst.h == 256.0F);
  CHECK(neutral.scale == 1.0F);
  CHECK(neutral.authored_size.x == 256.0F);
  CHECK(neutral.authored_size.y == 256.0F);

  platform.SetPresentationScale(game::PresentationScale{2.0F, 1.0F, 1.0F});
  const Placement scaled = game::RouteMap_OverlayPlacement(platform);
  // Window 1024x768: min(U=2, 1024/256=4, 768/256=3) = 2.
  CHECK(scaled.dst.w == 512.0F);
  CHECK(scaled.dst.h == 512.0F);
  CHECK(scaled.scale == 2.0F);
  // A raw window point at the chart centre maps to the authored centre.
  const SDL_FPoint centre = scaled.ToAuthored({256.0F, 256.0F});
  CHECK(centre.x == 128.0F);
  CHECK(centre.y == 128.0F);
}

// A window point outside the placed square must not hit-test inside it. This
// guards the shared draw/hit rect against an inverse-cancellation regression.
TEST_CASE("route map clicks map through the overlay placement", "[routemap]") {
  SdlPlatform platform;
  const Placement placement = game::RouteMap_OverlayPlacement(platform);
  const SDL_FPoint miss =
      placement.ToAuthored({placement.dst.w + 10.0F, placement.dst.h + 10.0F});
  CHECK(miss.x > placement.authored_size.x);
  CHECK(miss.y > placement.authored_size.y);
}
