#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "game/sprite_world.hpp"

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

} // namespace game
