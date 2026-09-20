#include <catch2/catch_test_macros.hpp>

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
