#include "util/render_clip_scope.hpp"

#include <SDL3/SDL.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace {

struct SoftwareTarget {
  std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface{
      SDL_CreateSurface(40, 40, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface};
  std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer{
      surface != nullptr ? SDL_CreateSoftwareRenderer(surface.get()) : nullptr,
      SDL_DestroyRenderer};
};

[[nodiscard]] SDL_Color Pixel(SDL_Surface &surface, int x, int y) {
  SDL_Color color{};
  REQUIRE(SDL_ReadSurfacePixel(
      &surface, x, y, &color.r, &color.g, &color.b, &color.a));
  return color;
}

void Fill(SDL_Renderer *renderer, const SDL_FRect &rect, Uint8 shade) {
  REQUIRE(SDL_SetRenderDrawColor(renderer, shade, shade, shade, 255));
  REQUIRE(SDL_RenderFillRect(renderer, &rect));
  REQUIRE(SDL_FlushRenderer(renderer));
}

void ClearTo(SDL_Renderer *renderer, Uint8 shade) {
  REQUIRE(SDL_SetRenderDrawColor(renderer, shade, shade, shade, 255));
  REQUIRE(SDL_RenderClear(renderer));
}

} // namespace

TEST_CASE("RenderClipScope confines drawing and restores a disabled clip",
          "[render_clip_scope]") {
  SoftwareTarget target;
  REQUIRE(target.renderer);
  ClearTo(target.renderer.get(), 0);
  REQUIRE_FALSE(SDL_RenderClipEnabled(target.renderer.get()));

  {
    const evnova::util::RenderClipScope clip(target.renderer.get(),
                                             SDL_FRect{10, 10, 20, 20});
    REQUIRE(SDL_RenderClipEnabled(target.renderer.get()));
    SDL_Rect active{};
    REQUIRE(SDL_GetRenderClipRect(target.renderer.get(), &active));
    CHECK(active.x == 10);
    CHECK(active.y == 10);
    CHECK(active.w == 20);
    CHECK(active.h == 20);
    Fill(target.renderer.get(), SDL_FRect{0, 0, 40, 40}, 255);
  }

  REQUIRE_FALSE(SDL_RenderClipEnabled(target.renderer.get()));
  CHECK(Pixel(*target.surface, 5, 20).r == 0);
  CHECK(Pixel(*target.surface, 20, 5).r == 0);
  CHECK(Pixel(*target.surface, 15, 15).r == 255);
  CHECK(Pixel(*target.surface, 29, 29).r == 255);
  CHECK(Pixel(*target.surface, 30, 30).r == 0);
}

TEST_CASE("RenderClipScope intersects an active clip and restores it",
          "[render_clip_scope]") {
  SoftwareTarget target;
  REQUIRE(target.renderer);
  ClearTo(target.renderer.get(), 0);
  const SDL_Rect previous{4, 4, 12, 12};
  REQUIRE(SDL_SetRenderClipRect(target.renderer.get(), &previous));

  {
    const evnova::util::RenderClipScope clip(target.renderer.get(),
                                             SDL_FRect{8, 8, 20, 20});
    SDL_Rect active{};
    REQUIRE(SDL_GetRenderClipRect(target.renderer.get(), &active));
    CHECK(active.x == 8);
    CHECK(active.y == 8);
    CHECK(active.w == 8);
    CHECK(active.h == 8);
    Fill(target.renderer.get(), SDL_FRect{0, 0, 40, 40}, 255);
  }

  // The prior clip (not the raw box) must be back in force.
  REQUIRE(SDL_RenderClipEnabled(target.renderer.get()));
  SDL_Rect restored{};
  REQUIRE(SDL_GetRenderClipRect(target.renderer.get(), &restored));
  CHECK(restored.x == previous.x);
  CHECK(restored.y == previous.y);
  CHECK(restored.w == previous.w);
  CHECK(restored.h == previous.h);
  CHECK(Pixel(*target.surface, 5, 5).r == 0); // outside the intersection
  CHECK(Pixel(*target.surface, 8, 8).r == 255);
  CHECK(Pixel(*target.surface, 15, 15).r == 255);
  CHECK(Pixel(*target.surface, 16, 16).r == 0);
  CHECK(Pixel(*target.surface, 20, 20).r == 0);
}

TEST_CASE("RenderClipScope with a disjoint box yields a zero-sized clip",
          "[render_clip_scope]") {
  SoftwareTarget target;
  REQUIRE(target.renderer);
  ClearTo(target.renderer.get(), 0);
  const SDL_Rect previous{0, 0, 5, 5};
  REQUIRE(SDL_SetRenderClipRect(target.renderer.get(), &previous));

  {
    const evnova::util::RenderClipScope clip(target.renderer.get(),
                                             SDL_FRect{20, 20, 5, 5});
    SDL_Rect active{};
    REQUIRE(SDL_GetRenderClipRect(target.renderer.get(), &active));
    CHECK((active.w == 0 || active.h == 0));
    CHECK(SDL_RenderClipEnabled(target.renderer.get()));
    Fill(target.renderer.get(), SDL_FRect{0, 0, 40, 40}, 255);
  }

  CHECK(Pixel(*target.surface, 2, 2).r == 0);
  CHECK(Pixel(*target.surface, 22, 22).r == 0);
  SDL_Rect restored{};
  REQUIRE(SDL_GetRenderClipRect(target.renderer.get(), &restored));
  CHECK(restored.x == previous.x);
  CHECK(restored.y == previous.y);
  CHECK(restored.w == previous.w);
  CHECK(restored.h == previous.h);
}
