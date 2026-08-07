#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "sdl_platform.hpp"

#include "game/game_state.hpp"
#include "game/scenario_data.hpp"
#include "game/sprite_world.hpp"
#include "game/weapon.hpp"

namespace game {

// Clean-room sprite anchors, lifecycle/refcounting and the time-animated shot
// frame cadence. The anchor/lifecycle tests are pure logic (no SDL renderer);
// the shot-animation test runs against the shipped Nova weapon/archive data.

namespace {
bool ArchivesAvailable() {
  return std::filesystem::exists("EV Nova/Nova.rez") ||
         std::filesystem::exists("EV Nova/Nova Files/Nova Data 1.rez") ||
         std::filesystem::exists("../../../EV Nova/Nova.rez");
}

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

TEST_CASE("Sprite_AnchorToScreen honours a non-central anchor",
          "[sprite][anchor]") {
  // A shot whose gun-fire point is 4px right of the frame's top-left: the draw
  // places that point on the world position, not the frame's centre.
  const auto p =
      Sprite_AnchorToScreen(320.0F, 200.0F, 0.0F, 0.0F, 640, 400, 4.0F, 4.0F);
  CHECK(p.screen_x == Catch::Approx(640.0F / 2.0F + 320.0F - 4.0F));
  CHECK(p.screen_y == Catch::Approx(400.0F / 2.0F + 200.0F - 4.0F));
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

// Sprite_AssignAsset binds a loaded asset's frames onto a live sprite, keeping
// the (shared) asset alive via refcount so the frame pixels outlive the local
// asset view. Returns the frame count bound and exposes each frame's anchor.
TEST_CASE("Sprite_AssignAsset binds an asset onto a live sprite",
          "[sprite][lifecycle]") {
  // Build a fake 2-frame asset by hand (no SDL/archives needed): a 35x35 tile
  // set with a centre anchor.
  auto asset = std::make_shared<SpriteAsset>();
  asset->tile_width = 35;
  asset->tile_height = 35;
  asset->frame_count = 2;
  asset->frames.resize(2);
  for (auto &f : asset->frames) {
    f.anchor_x = 17.5F;
    f.anchor_y = 17.5F;
  }

  Sprite sprite;
  REQUIRE(Sprite_AssignAsset(sprite, asset) == 2);
  CHECK(sprite.FrameCount() == 2);
  const SpriteFrameImage *img = sprite.TheFrame(1);
  REQUIRE(img != nullptr);
  CHECK(img->anchor_x == Catch::Approx(17.5F));
  CHECK(img->source_set.use_count() >= 1); // the sprite retains the shared set
}

// Time-animated shot-frame stepping (Shot_HandleShot animated branch): a weapon
// with flags_primary bit 0 set advances its shot's frame_cycle_index at the
// shot_anim_frame_dwell cadence as NovaWeapon_TickShots runs; a static/heading
// weapon (Light Blaster, bit clear) does not advance. Uses the shipped weapon
// data to drive the branch selection.
TEST_CASE("animated shot frame steps at the shot_anim_frame_dwell cadence",
          "[weapon][sprite][data]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Find any time-animated weapon (flags_primary bit 0 set). The starter
  // Light Blaster is static; other shipped weapons provide the animated branch.
  // `bank` is the shot's weapon_id that resolves back to the weapon (bank slot
  // b == weapon resource id b+0x80 in WeaponAt).
  const Weapon *animated = nullptr;
  int bank = 0;
  for (int i = 0; i < 256; ++i) {
    const Weapon *w =
        state.scenario.Weapon(static_cast<std::int16_t>(i + 0x80));
    if (w && (w->flags & 0x0001U) != 0) {
      animated = w;
      bank = i;
      break;
    }
  }
  if (!animated) {
    SKIP("no time-animated weapon in the shipped data");
  }

  state.player.ship_class_id = 0;
  state.active_shots.clear();
  ActiveShot shot;
  shot.weapon_id = static_cast<std::int16_t>(bank);
  shot.pos_x = 0.0F;
  shot.pos_y = 0.0F;
  shot.life_frames = 100;
  state.active_shots.push_back(shot);

  const std::int16_t dwell = animated->shot_anim_frame_dwell;
  // A single TickShots with a frame time >= dwell advances the cycle exactly
  // once and resets the dwell accumulator (dwell < 1 advances every frame).
  const int before = state.active_shots[0].frame_cycle_index;
  NovaWeapon_TickShots(state, static_cast<float>(std::max(1, dwell + 1)));
  const int after = state.active_shots[0].frame_cycle_index;
  CHECK(after == before + 1);
  CHECK(state.active_shots[0].anim_elapsed == Catch::Approx(0.0F));
}
} // namespace game
