#include <catch2/catch_test_macros.hpp>

#include "game/services_buttons.hpp"

#include <array>
#include <memory>
#include <string_view>

// 0x004a3340's s=2 arrows, rasterized by 0x004b97e0: shorten the larger
// endpoint on each axis, then stamp a 2x2 pen at every diagonal point.
// Check the complete footprint, including empty pixels, at native and HiDPI
// scales. Thin SDL lines can leave gaps even when their bounds look right.
TEST_CASE("button arrows match the original solid centred raster") {
  constexpr std::array<std::string_view, 5> up_mask{
      "...###...", "..#####..", ".###.###.", "###...###", "##.....##"};
  for (const int size : {23, 25}) {
    for (const int scale : {1, 2}) {
      for (const bool up : {false, true}) {
        for (const Uint8 shade : {Uint8{255}, Uint8{0x26}}) {
          CAPTURE(size, scale, up, shade);
          const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>
              surface(SDL_CreateSurface(
                          40 * scale, 40 * scale, SDL_PIXELFORMAT_RGBA32),
                      SDL_DestroySurface);
          REQUIRE(surface);
          const std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>
              renderer(SDL_CreateSoftwareRenderer(surface.get()),
                       SDL_DestroyRenderer);
          REQUIRE(renderer);
          REQUIRE(SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255));
          REQUIRE(SDL_RenderClear(renderer.get()));
          REQUIRE(SDL_SetRenderScale(renderer.get(),
                                     static_cast<float>(scale),
                                     static_cast<float>(scale)));
          // A half-logical-pixel origin at 2x must translate the whole glyph,
          // not change its local shape or the width of its strokes.
          const float origin = scale == 2 ? 4.5F : 4.0F;
          game::DrawThreeStateButtonArrow(renderer.get(),
                                          {origin,
                                           origin,
                                           static_cast<float>(size),
                                           static_cast<float>(size)},
                                          up,
                                          {shade, shade, shade, 255});
          REQUIRE(SDL_FlushRenderer(renderer.get()));
          const int left =
              static_cast<int>(origin * scale) + (size / 2 - 4) * scale;
          const int top =
              static_cast<int>(origin * scale) + (size / 2 - 2) * scale;
          for (int y = 0; y < surface->h; ++y) {
            for (int x = 0; x < surface->w; ++x) {
              bool lit = false;
              if (x >= left && x < left + 9 * scale && y >= top &&
                  y < top + 5 * scale) {
                const int row = (y - top) / scale;
                lit = up_mask[up ? row : 4 - row][(x - left) / scale] == '#';
              }
              Uint8 r{}, g{}, b{}, a{};
              REQUIRE(
                  SDL_ReadSurfacePixel(surface.get(), x, y, &r, &g, &b, &a));
              const Uint8 expected = lit ? shade : 0;
              CAPTURE(x, y);
              REQUIRE(r == expected);
              REQUIRE(g == expected);
              REQUIRE(b == expected);
              REQUIRE(a == 255);
            }
          }
        }
      }
    }
  }
}
