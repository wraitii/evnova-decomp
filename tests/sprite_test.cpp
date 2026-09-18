#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL.h>

#include <memory>
#include <vector>

#include "game/sprite_world.hpp"
#include "sdl_platform.hpp"

namespace game {

// Clean-room sprite anchors and lifecycle/refcounting. These are pure logic
// (no SDL renderer): the anchor placement is shared by every in-flight draw
// path and the refcount bookkeeping is the sprite world's ownership core.

namespace {
// A minimal frame image carrying only paint geometry (no SDL texture), enough
// to exercise the lifecycle/refcount bookkeeping without a renderer.
std::shared_ptr<SpriteFrameImage> MetaImage(int w, int h) {
  return Sprite_InitFrameImage(
      static_cast<float>(w) / 2.0F, static_cast<float>(h) / 2.0F, w, h);
}
} // namespace

// Sprite_SetPositionFromCurrentFrameAnchor placement: for a frame anchored at
// its centre and a camera centred on the world origin, the frame's top-left is
// worldToScreen(nx,ny) - half-size -- i.e. the frame is centred, matching the
// historical draw. A non-central anchor shifts the frame so the anchor point
// (e.g. a shot's gun-fire point) lands on the world position instead.
TEST_CASE("Sprite_AnchorToScreen centres a centred-anchor frame",
          "[sprite][anchor]") {
  const float ax = 35.0F / 2.0F; // tile centre of the 35px light-blaster tile
  const float ay = 35.0F / 2.0F;
  const auto p =
      Sprite_AnchorToScreen(0.0F, 0.0F, 0.0F, 0.0F, 640, 400, ax, ay);
  CHECK(p.screen_x == Catch::Approx(640.0F / 2.0F - ax));
  CHECK(p.screen_y == Catch::Approx(400.0F / 2.0F - ay));
}

// Sprite lifecycle/refcounting: frames are appended, the sprite's presented
// frame clamps into range, and Release drops the set so the (shared) frame
// images are freed. Mirrors Sprite_AddFrame / Sprite_SetCurrentFrame /
// Sprite_Release refcount behaviour.
TEST_CASE("sprite frame lifecycle appends, clamps and releases",
          "[sprite][lifecycle]") {
  Sprite sprite;
  // A shared frame image kept alive across the sprite and an outer handle, so
  // we can observe the refcount drop after Release.
  std::shared_ptr<SpriteFrameImage> shared = MetaImage(35, 35);
  const auto *raw = shared.get();
  REQUIRE(sprite.AddFrame(shared) == 0);
  REQUIRE(sprite.AddFrame(MetaImage(35, 35)) == 1);
  CHECK(sprite.FrameCount() == 2);
  CHECK(sprite.TheFrame(-5) == raw); // clamps up to frame 0
  CHECK(sprite.TheFrame(1) != raw);  // frame 1 is the distinct image
  CHECK(sprite.TheFrame(99) != raw); // clamps to last frame (1)

  sprite.SetCurrentFrame(42); // clamps to last frame
  CHECK(sprite.TheFrame(42) != raw);

  // SetPositionFromCurrentFrameAnchor stores the located position.
  sprite.SetPositionFromCurrentFrameAnchor(120, -40);
  CHECK(sprite.LocatedX() == Catch::Approx(120.0F));
  CHECK(sprite.LocatedY() == Catch::Approx(-40.0F));

  // Releasing the sprite drops its refs; the outer handle stays valid until it
  // goes out of scope, then the image is freed.
  sprite.Release();
  CHECK(sprite.FrameCount() == 0);
  CHECK(sprite.TheFrame(0) == nullptr);
  CHECK(shared != nullptr);
}

// Ghidra 0x00438db0 Frame_UpdateSpriteDistanceIntensity: murk-scaled distance
// fog. distSq uses the x87-truncated (toward zero) absolute axis deltas, the
// product is scaled by the 1.2e-05 constant and truncated again, then clamped
// to 0..0x1f. murk 0 is always clear.
TEST_CASE("distance brightness matches the murk fog formula",
          "[sprite][murk]") {
  CHECK(Sprite_DistanceBrightness(0, 0.0F, 0.0F, 1000.0F, 0.0F) == 0);
  // dx=100 -> 100 * 100^2 * 1.2e-5 = 12.0 -> 12.
  CHECK(Sprite_DistanceBrightness(100, 0.0F, 0.0F, 100.0F, 0.0F) == 12);
  // The 0x1f ceiling applies for far/high-murk sprites.
  CHECK(Sprite_DistanceBrightness(100, 0.0F, 0.0F, 1000.0F, 0.0F) == 0x1f);
  // Axis deltas truncate toward zero before squaring: 1.9 -> 1 -> ~0.
  CHECK(Sprite_DistanceBrightness(100, 0.0F, 0.0F, 1.9F, 0.0F) == 0);
  // Symmetric in the axis deltas (both axes contribute).
  CHECK(Sprite_DistanceBrightness(100, 0.0F, 0.0F, 0.0F, -100.0F) == 12);
}

// The optional SpriteDrawOptions.tint_rgb5 color mod (SDL approximation of the
// original RGB555 hull tint): a white frame multiplied by 0x10/0x20/0x00 per
// channel must read back as 127/255/0.
TEST_CASE("sprite tint color mod multiplies the frame channels",
          "[sprite][tint]") {
  std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface{
      SDL_CreateSurface(40, 40, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface};
  REQUIRE(surface);
  std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer{
      SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer};
  REQUIRE(renderer);

  const std::vector<std::uint8_t> white(8 * 8 * 4, 0xff);
  auto texture = SdlTexture::Create(renderer.get(), 8, 8, white);
  REQUIRE(texture);
  Sprite sprite;
  REQUIRE(sprite.AddFrame(Sprite_InitFrameImage(
              std::move(texture), 0.0F, 0.0F, 8, 8)) >= 0);

  REQUIRE(SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255));
  REQUIRE(SDL_RenderClear(renderer.get()));
  SpriteDrawOptions opts;
  opts.tint_rgb5 = std::array<std::int16_t, 3>{0x10, 0x20, 0x00};
  DrawSprite(renderer.get(), sprite, 0, 0.0F, 0.0F, 0.0F, 0.0F, 40, 40, opts);
  REQUIRE(SDL_FlushRenderer(renderer.get()));

  SDL_Color color{};
  REQUIRE(SDL_ReadSurfacePixel(
      surface.get(), 22, 22, &color.r, &color.g, &color.b, &color.a));
  CHECK(color.r == 127);
  CHECK(color.g == 255);
  CHECK(color.b == 0);
}

// High-bit government colors are `byte << 8` (e.g. 0x8000). The raw channel is
// unsigned; a signed interpretation clamped it to black. The port currently
// reproduces only the multiplicative part and clamps above-neutral channels, so
// this must read back as the untinted frame (not black).
TEST_CASE("sprite tint keeps high-bit channels out of the black clamp",
          "[sprite][tint]") {
  std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface{
      SDL_CreateSurface(40, 40, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface};
  REQUIRE(surface);
  std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer{
      SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer};
  REQUIRE(renderer);

  const std::vector<std::uint8_t> white(8 * 8 * 4, 0xff);
  auto texture = SdlTexture::Create(renderer.get(), 8, 8, white);
  REQUIRE(texture);
  Sprite sprite;
  REQUIRE(sprite.AddFrame(Sprite_InitFrameImage(
              std::move(texture), 0.0F, 0.0F, 8, 8)) >= 0);

  REQUIRE(SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255));
  REQUIRE(SDL_RenderClear(renderer.get()));
  SpriteDrawOptions opts;
  opts.tint_rgb5 = std::array<std::int16_t, 3>{
      static_cast<std::int16_t>(0x8000), 0x20, 0x20};
  DrawSprite(renderer.get(), sprite, 0, 0.0F, 0.0F, 0.0F, 0.0F, 40, 40, opts);
  REQUIRE(SDL_FlushRenderer(renderer.get()));

  SDL_Color color{};
  REQUIRE(SDL_ReadSurfacePixel(
      surface.get(), 22, 22, &color.r, &color.g, &color.b, &color.a));
  CHECK(color.r == 255);
  CHECK(color.g == 255);
  CHECK(color.b == 255);
}

// The tint is applied per draw and restored afterwards, so a later untinted
// draw of the same shared frame texture is not left modulated.
TEST_CASE("sprite tint restores the shared texture modulation",
          "[sprite][tint]") {
  std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface{
      SDL_CreateSurface(40, 40, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface};
  REQUIRE(surface);
  std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer{
      SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer};
  REQUIRE(renderer);

  const std::vector<std::uint8_t> white(8 * 8 * 4, 0xff);
  auto texture = SdlTexture::Create(renderer.get(), 8, 8, white);
  REQUIRE(texture);
  Sprite sprite;
  REQUIRE(sprite.AddFrame(Sprite_InitFrameImage(
              std::move(texture), 0.0F, 0.0F, 8, 8)) >= 0);

  REQUIRE(SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255));
  REQUIRE(SDL_RenderClear(renderer.get()));
  SpriteDrawOptions tinted;
  tinted.tint_rgb5 = std::array<std::int16_t, 3>{0x10, 0x10, 0x10};
  DrawSprite(renderer.get(), sprite, 0, 0.0F, 0.0F, 0.0F, 0.0F, 40, 40, tinted);

  REQUIRE(SDL_RenderClear(renderer.get()));
  DrawSprite(renderer.get(),
             sprite,
             0,
             0.0F,
             0.0F,
             0.0F,
             0.0F,
             40,
             40,
             SpriteDrawOptions{});
  REQUIRE(SDL_FlushRenderer(renderer.get()));

  SDL_Color color{};
  REQUIRE(SDL_ReadSurfacePixel(
      surface.get(), 22, 22, &color.r, &color.g, &color.b, &color.a));
  CHECK(color.r == 255);
  CHECK(color.g == 255);
  CHECK(color.b == 255);
}

} // namespace game
