#include <catch2/catch_test_macros.hpp>

#include "sdl_platform.hpp"
#include "util/placement.hpp"

#include <memory>

TEST_CASE("placement transforms are inverse and cap at native size") {
  const Placement placement =
      PlaceContained(SDL_FPoint{1024.0F, 768.0F}, SDL_FPoint{1600.0F, 1000.0F});
  REQUIRE(placement.scale == 1.0F);
  REQUIRE(placement.dst.x == 288.0F);
  REQUIRE(placement.dst.y == 116.0F);
  const SDL_FPoint authored{73.25F, 411.5F};
  const SDL_FPoint window = placement.ToWindow(authored);
  REQUIRE(placement.ToAuthored(window).x == authored.x);
  REQUIRE(placement.ToAuthored(window).y == authored.y);
}

TEST_CASE("placement canonicalization matches the SDL viewport") {
  Placement placement{{25.25F, 13.25F, 200.5F, 150.5F}, 0.5F};
  const Placement canonical = placement.Canonicalized();
  const SDL_Rect viewport = canonical.ToRenderViewport();
  REQUIRE(canonical.dst.x == viewport.x * canonical.scale);
  REQUIRE(canonical.dst.y == viewport.y * canonical.scale);
  REQUIRE(canonical.dst.w == viewport.w * canonical.scale);
  REQUIRE(canonical.dst.h == viewport.h * canonical.scale);
}

TEST_CASE("placement viewport clips and maps a half scale at density two") {
  const auto surface =
      std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>{
          SDL_CreateSurface(400, 300, SDL_PIXELFORMAT_RGBA32),
          SDL_DestroySurface};
  REQUIRE(surface != nullptr);
  const auto renderer =
      std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>{
          SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer};
  REQUIRE(renderer != nullptr);
  REQUIRE(SDL_FillSurfaceRect(
      surface.get(), nullptr, SDL_MapSurfaceRGBA(surface.get(), 0, 0, 0, 255)));

  // A 0.5 authored-to-window placement on a density-2 target has a net SDL
  // render scale of 1. A window-point origin of (25,13) must therefore land
  // at backing-pixel (50,26), regardless of the authored drawing scale.
  const Placement placement =
      PlaceContained({400.0F, 300.0F}, {800.0F, 600.0F});
  // Use a half-scale placement explicitly to exercise the conversion.
  Placement half = PlaceCenteredIn(SDL_FRect{25.0F, 13.0F, 400.0F, 300.0F},
                                   SDL_FPoint{400.0F, 300.0F});
  half.dst = {25.0F, 13.0F, 200.0F, 150.0F};
  half.scale = 0.5F;
  REQUIRE(placement.scale == 1.0F);
  const SDL_Rect viewport = half.ToRenderViewport();
  REQUIRE(viewport.x == 50);
  REQUIRE(viewport.y == 26);
  ApplyPlacementToRenderer(renderer.get(), half, 2.0F);
  REQUIRE(SDL_SetRenderDrawColor(renderer.get(), 255, 0, 0, 255));
  REQUIRE(SDL_RenderFillRect(renderer.get(), nullptr));
  REQUIRE(SDL_FlushRenderer(renderer.get()));

  Uint8 r{}, g{}, b{}, a{};
  REQUIRE(SDL_ReadSurfacePixel(surface.get(), 50, 26, &r, &g, &b, &a));
  REQUIRE(r == 255);
  REQUIRE(g == 0);
  REQUIRE(SDL_ReadSurfacePixel(surface.get(), 49, 26, &r, &g, &b, &a));
  REQUIRE(r == 0);
}

TEST_CASE("nested centered placements reflow with their containing screen") {
  const Placement screen =
      PlaceContained(SDL_FPoint{1024.0F, 768.0F}, SDL_FPoint{1600.0F, 1000.0F});
  const Placement dialog = PlaceCenteredIn(screen, SDL_FPoint{640.0F, 480.0F});
  const Placement child = PlaceCenteredIn(dialog, SDL_FPoint{320.0F, 240.0F});
  const Placement resized = child.Reflow(SDL_FPoint{1200.0F, 900.0F});
  const Placement resized_screen = screen.Reflow(SDL_FPoint{1200.0F, 900.0F});
  const Placement resized_dialog =
      PlaceCenteredIn(resized_screen, SDL_FPoint{640.0F, 480.0F});
  const Placement expected =
      PlaceCenteredIn(resized_dialog, SDL_FPoint{320.0F, 240.0F});
  REQUIRE(resized.dst.x == expected.dst.x);
  REQUIRE(resized.dst.y == expected.dst.y);
  REQUIRE(resized.dst.w == expected.dst.w);
  REQUIRE(resized.dst.h == expected.dst.h);
  const Placement resized_again = resized.Reflow({800.0F, 600.0F});
  REQUIRE(resized_again.dst.x == 240.0F);
  REQUIRE(resized_again.dst.y == 180.0F);
}

TEST_CASE("a dialog fits the window independently of its smaller background") {
  const Placement background =
      PlaceContained({200.0F, 100.0F}, {1000.0F, 800.0F});
  const Placement dialog =
      PlaceCenteredIn(background, SDL_FPoint{600.0F, 400.0F});
  REQUIRE(dialog.scale == 1.0F);
  REQUIRE(dialog.dst.x == 200.0F);
  REQUIRE(dialog.dst.y == 200.0F);
  const Placement resized = dialog.Reflow({300.0F, 250.0F});
  REQUIRE(resized.scale == 0.5F);
  REQUIRE(resized.dst.x == 0.0F);
  REQUIRE(resized.dst.y == 25.0F);
}

TEST_CASE("placement scopes restore through background replacement and early "
          "return") {
  SdlPlatform platform;
  platform.SetPlacement(PlaceContained({1024.0F, 768.0F}, {1600.0F, 1000.0F}));
  const auto modal = [&] {
    const SdlPlatform::ScopedPlacement scope(platform,
                                             platform.current_placement());
    platform.SetPlacement(PlaceWindow({1600.0F, 1000.0F}));
    {
      const SdlPlatform::ScopedPlacement nested(
          platform, PlaceContained({400.0F, 300.0F}, {1600.0F, 1000.0F}));
      platform.SetPlacement(
          PlaceContained({200.0F, 100.0F}, {1600.0F, 1000.0F}));
    }
    REQUIRE(platform.current_placement().dst.w == 1600.0F);
    return;
  };
  modal();
  REQUIRE(platform.current_placement().dst.w == 1024.0F);
  REQUIRE(platform.current_placement().dst.x == 288.0F);
}

TEST_CASE("requested scale defaults to neutral one") {
  REQUIRE(
      PlaceContained({100.0F, 100.0F}, {1000.0F, 1000.0F}).requested_scale ==
      1.0F);
  REQUIRE(PlaceWindow({800.0F, 600.0F}).requested_scale == 1.0F);
  REQUIRE(
      PlaceCenteredIn(SDL_FRect{0.0F, 0.0F, 800.0F, 600.0F}, {200.0F, 200.0F})
          .requested_scale == 1.0F);
}

TEST_CASE("requested scale composes once with the window fit") {
  const Placement up =
      PlaceContained({100.0F, 100.0F}, {1000.0F, 1000.0F}, 2.0F);
  REQUIRE(up.scale == 2.0F);
  REQUIRE(up.dst.x == 400.0F);
  REQUIRE(up.dst.y == 400.0F);
  REQUIRE(up.dst.w == 200.0F);

  const Placement down =
      PlaceContained({100.0F, 100.0F}, {1000.0F, 1000.0F}, 0.5F);
  REQUIRE(down.scale == 0.5F);
  REQUIRE(down.dst.x == 475.0F);
  REQUIRE(down.dst.w == 50.0F);

  // The fit clamp still wins when the request exceeds the window.
  const Placement clamped =
      PlaceContained({100.0F, 100.0F}, {150.0F, 150.0F}, 4.0F);
  REQUIRE(clamped.scale == 1.5F);
}

TEST_CASE("window-rule requested scale sets the authored extent") {
  const Placement scene = PlaceWindow({800.0F, 600.0F}, 2.0F);
  REQUIRE(scene.scale == 2.0F);
  REQUIRE(scene.dst.w == 800.0F);
  REQUIRE(scene.dst.h == 600.0F);
  REQUIRE(scene.authored_size.x == 400.0F);
  REQUIRE(scene.authored_size.y == 300.0F);
  // The authored viewport maps back to the full window.
  const SDL_FPoint window = scene.ToWindow({400.0F, 300.0F});
  REQUIRE(window.x == 800.0F);
  REQUIRE(window.y == 600.0F);
  const Placement resized = scene.Reflow({1024.0F, 768.0F});
  REQUIRE(resized.scale == 2.0F);
  REQUIRE(resized.dst.w == 1024.0F);
  REQUIRE(resized.authored_size.x == 512.0F);
}

TEST_CASE("requested scale persists through a contained reflow") {
  const Placement screen =
      PlaceContained({400.0F, 300.0F}, {1000.0F, 800.0F}, 2.0F);
  const Placement resized = screen.Reflow({1200.0F, 900.0F});
  REQUIRE(resized.requested_scale == 2.0F);
  REQUIRE(resized.scale == 2.0F);
}

TEST_CASE("requested scales survive a nested modal reflow") {
  const Placement screen =
      PlaceContained({640.0F, 480.0F}, {1600.0F, 1000.0F}, 2.0F);
  REQUIRE(screen.scale == 2.0F);
  const Placement dialog = PlaceCenteredIn(screen, {320.0F, 240.0F}, 1.5F);
  REQUIRE(dialog.scale == 1.5F);
  REQUIRE(dialog.anchor_requested_scale == 2.0F);
  const Placement resized = dialog.Reflow({1600.0F, 1200.0F});
  REQUIRE(resized.requested_scale == 1.5F);
  REQUIRE(resized.scale == 1.5F);
  REQUIRE(resized.dst.x == 560.0F);
  REQUIRE(resized.dst.y == 420.0F);
  REQUIRE(resized.dst.w == 480.0F);
  REQUIRE(resized.dst.h == 360.0F);
}
