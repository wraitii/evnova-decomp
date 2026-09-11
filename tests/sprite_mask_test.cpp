#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/collision.hpp"
#include "game/game_state.hpp"
#include "game/sprite_mask.hpp"
#include "game/weapon.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace game {
namespace {

// Builds a mask from a small ASCII pattern ('.' = transparent, '#' = opaque).
SpriteMask MakeMask(const std::vector<const char *> &rows) {
  SpriteMask mask;
  if (rows.empty()) {
    return mask;
  }
  mask.height = static_cast<int>(rows.size());
  mask.width = static_cast<int>(std::char_traits<char>::length(rows.front()));
  mask.opaque.assign(static_cast<std::size_t>(mask.width) *
                         static_cast<std::size_t>(mask.height),
                     0);
  for (int y = 0; y < mask.height; ++y) {
    for (int x = 0; x < mask.width; ++x) {
      if (rows[static_cast<std::size_t>(y)][x] == '#') {
        mask.opaque[static_cast<std::size_t>(y) * mask.width +
                    static_cast<std::size_t>(x)] = 1;
      }
    }
  }
  return mask;
}

// A 1-pixel-wide vertical strip of `height` opaque pixels, used to probe the
// Sprite_GetShotHalfSpan boundary in the mask-vs-circle decision.
SpriteMask MakeStrip(int height) {
  SpriteMask mask;
  mask.width = 1;
  mask.height = height;
  mask.opaque.assign(static_cast<std::size_t>(height), 1);
  return mask;
}

// Two 3x3 masks with a single opaque centre pixel. Placed so the frame bounds
// overlap only in transparent corners, the pixel-mask test must miss even
// though the bounding circles would touch.
const SpriteMask kCentreDot = MakeMask({"...", ".#.", "..."});

void SeedCollisionScenario(GameState &state) {
  // The integration cases inject masks directly, so skip the live refresh (it
  // would overwrite the injected bindings with the shipped sprite geometry).
  state.collision_masks_enabled = false;
  state.scenario.weapons.resize(1);
  Weapon &weapon = state.scenario.weapons[0];
  weapon.weapon_mode_code = -1;
  weapon.projectile_speed = 0.0F;
  weapon.lifetime_ticks = 10;
  weapon.mass_damage = 25;
  weapon.energy_damage = 10;

  state.scenario.ships.resize(1);

  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.ship_class_id = 0;
  state.player.current_system_id = 0;
  state.player.armor_points = 100.0F;
  state.player.shield_points = 100.0F;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.player.collision_radius_px = 10.0F;

  Ship &target = state.ShipAt(1);
  target.is_active = true;
  target.ship_instance_id = 1;
  target.ship_class_id = 0;
  target.current_system_id = 0;
  target.armor_points = 100.0F;
  target.shield_points = 20.0F;
  target.ai_behavior_code = 5;
  target.pos_x = 10.0F;
  target.pos_y = 0.0F;
  target.collision_radius_px = 10.0F;
}

} // namespace

TEST_CASE("sprite mask thresholds rgba alpha", "[sprite_mask]") {
  const std::vector<std::uint8_t> pixels = {
      255,
      0,
      0,
      0, // fully transparent
      0,
      255,
      0,
      1, // alpha 1 = opaque at the default threshold
      0,
      0,
      255,
      7, // opaque
      9,
      9,
      9,
      200, // opaque
  };
  const SpriteMask mask = SpriteMask_FromRgba(pixels, 2, 2);
  REQUIRE(mask.width == 2);
  REQUIRE(mask.height == 2);
  CHECK(!mask.OpaqueAt(0, 0));
  CHECK(mask.OpaqueAt(1, 0));
  CHECK(mask.OpaqueAt(0, 1));
  CHECK(mask.OpaqueAt(1, 1));

  // A higher alpha threshold drops the alpha-1 pixel.
  const SpriteMask strict = SpriteMask_FromRgba(pixels, 2, 2, 2);
  CHECK(!strict.OpaqueAt(1, 0));
  CHECK(strict.OpaqueAt(1, 1));

  // A short buffer is rejected rather than read out of bounds.
  const SpriteMask truncated =
      SpriteMask_FromRgba(std::span(pixels).first(8), 2, 2);
  CHECK(truncated.Empty());
}

TEST_CASE("pixel mask overlap misses transparent corners inside bounds",
          "[sprite_mask]") {
  const SpriteMask &dot = kCentreDot;
  // Anchors at the frame centre (1.5, 1.5). World positions 2px apart: the 3x3
  // bounds overlap in corners but the single opaque pixels never coincide.
  CHECK_FALSE(SpriteMask_TestOverlap(
      dot, 0.0F, 0.0F, 1.5F, 1.5F, dot, 2.0F, 0.0F, 1.5F, 1.5F));
  // Same position: the centre pixels coincide.
  CHECK(SpriteMask_TestOverlap(
      dot, 0.0F, 0.0F, 1.5F, 1.5F, dot, 0.0F, 0.0F, 1.5F, 1.5F));
  // Disjoint bounds are an immediate miss.
  CHECK_FALSE(SpriteMask_TestOverlap(
      dot, 0.0F, 0.0F, 1.5F, 1.5F, dot, 100.0F, 0.0F, 1.5F, 1.5F));
}

TEST_CASE("pixel mask overlap respects local opaque runs", "[sprite_mask]") {
  const SpriteMask bar = MakeMask({"###", "...", "###"});
  // Anchor at the top-left so the placement math is exact (1.5 anchors round
  // asymmetrically around zero). Two bars 2px apart vertically: the bottom row
  // of A aligns with the top row of B, so there is a hit.
  CHECK(SpriteMask_TestOverlap(
      bar, 0.0F, 0.0F, 1.0F, 1.0F, bar, 0.0F, 2.0F, 1.0F, 1.0F));
  // 4px separation: no rows coincide.
  CHECK_FALSE(SpriteMask_TestOverlap(
      bar, 0.0F, 0.0F, 1.0F, 1.0F, bar, 0.0F, 4.0F, 1.0F, 1.0F));
}

TEST_CASE("bounding circle fallback mirrors half-height radius",
          "[sprite_mask]") {
  // 9x9 frames: radius 4 each, so centres 7px apart hit and 9px apart miss.
  CHECK(
      SpriteMask_TestBoundingCircleOverlap(9, 9, 0.0F, 0.0F, 9, 9, 7.0F, 0.0F));
  CHECK_FALSE(
      SpriteMask_TestBoundingCircleOverlap(9, 9, 0.0F, 0.0F, 9, 9, 9.0F, 0.0F));
  // Strict `<` at exactly the sum of radii (8) is a miss, matching 0x00475be0.
  CHECK_FALSE(
      SpriteMask_TestBoundingCircleOverlap(9, 9, 0.0F, 0.0F, 9, 9, 8.0F, 0.0F));
}

TEST_CASE("mask overlap honors the frame anchor offset", "[sprite_mask]") {
  const SpriteMask dot = kCentreDot;
  // With anchor (0,0) the two centre dots at 0 and 2px never coincide; shifting
  // B's anchor by 2px places its dot back on A's, so the overlap hits.
  CHECK_FALSE(SpriteMask_TestOverlap(
      dot, 0.0F, 0.0F, 0.0F, 0.0F, dot, 2.0F, 0.0F, 0.0F, 0.0F));
  CHECK(SpriteMask_TestOverlap(
      dot, 0.0F, 0.0F, 0.0F, 0.0F, dot, 2.0F, 0.0F, 2.0F, 0.0F));
}

TEST_CASE("ship mask-vs-circle decision follows the original thresholds",
          "[collision]") {
  const SpriteMask shot_dot = MakeMask({"#"});
  // Ship 2px from the shot: the circle radii overlap, but the 1-px opaque
  // columns never coincide. Returns true when the circle path produced a hit
  // (shot consumed), false when the mask path was used (transparent miss).
  const auto circle_hit = [&](float frame_scale, int ship_height) {
    GameState state;
    SeedCollisionScenario(state);
    state.last_frame_tick_scale = frame_scale;
    const SpriteMask strip = MakeStrip(ship_height);
    Ship &target = state.ShipAt(1);
    target.collision_mask.mask = &strip;
    target.collision_mask.anchor_x = 0.0F;
    target.collision_mask.anchor_y = 0.0F;
    target.pos_x = 2.0F;
    target.pos_y = 0.0F;
    REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
    state.active_shots[0].collision_mask.mask = &shot_dot;
    state.active_shots[0].collision_mask.anchor_x = 0.0F;
    state.active_shots[0].collision_mask.anchor_y = 0.0F;
    NovaWeapon_ResolveDirectShotCollisions(state);
    return state.active_shots.empty();
  };

  // scale < 2.0 and height > 0x20 -> pixel mask (miss).
  CHECK_FALSE(circle_hit(1.99F, 0x21));
  // scale == 2.0 -> circle (hit), matching `2.0 <= scale`.
  CHECK(circle_hit(2.0F, 0x21));
  // height == 0x20 -> circle (hit), matching `half_span < 0x21`.
  CHECK(circle_hit(1.99F, 0x20));
}

TEST_CASE("collision mask binding prefers pixel mask over circle",
          "[sprite_mask]") {
  const SpriteMask dot = kCentreDot;
  const SpriteMask &ref = dot;
  CollisionMaskBinding a;
  a.mask = &ref;
  a.anchor_x = 1.5F;
  a.anchor_y = 1.5F;
  CollisionMaskBinding b = a;
  // Large circles would overlap, but the transparent corners mean the pixel
  // masks miss.
  CHECK_FALSE(CollisionMask_TestContact(a, 0.0F, 0.0F, 50, b, 2.0F, 0.0F, 50));
  // Same position: masks hit.
  CHECK(CollisionMask_TestContact(a, 0.0F, 0.0F, 1, b, 0.0F, 0.0F, 1));
  // Null mask falls back to the circle.
  CollisionMaskBinding none;
  CHECK(CollisionMask_TestContact(none, 0.0F, 0.0F, 5, none, 8.0F, 0.0F, 5));
  CHECK_FALSE(
      CollisionMask_TestContact(none, 0.0F, 0.0F, 3, none, 8.0F, 0.0F, 3));
}

TEST_CASE("injected mask transparent miss keeps a projectile alive",
          "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  // A 33-tall opaque strip on the ship satisfies
  // Ship_HandleSpritePairCollision's > 0x20 mask threshold, so the pixel path
  // is used; the ship sits 2px from the shot so the 1px columns never coincide.
  const SpriteMask strip = MakeStrip(0x21);
  const SpriteMask shot_dot = MakeMask({"#"});
  state.ShipAt(1).collision_mask.mask = &strip;
  state.ShipAt(1).collision_mask.anchor_x = 0.0F;
  state.ShipAt(1).collision_mask.anchor_y = 0.0F;
  state.ShipAt(1).pos_x = 2.0F;
  state.ShipAt(1).pos_y = 0.0F;

  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  state.active_shots[0].collision_mask.mask = &shot_dot;
  state.active_shots[0].collision_mask.anchor_x = 0.0F;
  state.active_shots[0].collision_mask.anchor_y = 0.0F;
  NovaWeapon_ResolveDirectShotCollisions(state);

  // 2px apart: frames overlap, but the opaque columns never do.
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(20.0F));
}

TEST_CASE("direct asteroid contact uses the injected pixel mask",
          "[collision][asteroid]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.asteroid_defs.resize(1);
  state.scenario.asteroid_defs[0].wander_table_value = 100;
  state.ShipAt(1).is_active = false;

  AsteroidState &asteroid = state.asteroid_pool[0];
  asteroid.active = true;
  asteroid.wander_type = 0;
  asteroid.integrity = 100;
  asteroid.target_pos_x = 0.0F;
  asteroid.target_pos_y = 0.0F;
  const SpriteMask dot = MakeMask({"...", ".#.", "..."});
  asteroid.collision_mask.mask = &dot;
  asteroid.collision_mask.anchor_x = 1.5F;
  asteroid.collision_mask.anchor_y = 1.5F;

  // Overlapping masks consume the shot. The asteroid circle (25px) and the
  // shot circle (2px) also overlap here, so this checks the mask path reaches
  // the same hit resolution rather than proving circle bypass.
  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  state.active_shots[0].collision_mask = asteroid.collision_mask;
  NovaWeapon_ResolveDirectShotCollisions(state);

  CHECK(state.active_shots.empty());
  CHECK(asteroid.integrity == 90);
}

TEST_CASE("asteroid mask transparent miss inside circle leaves it intact",
          "[collision][asteroid]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.asteroid_defs.resize(1);
  state.scenario.asteroid_defs[0].wander_table_value = 100;
  state.ShipAt(1).is_active = false;

  AsteroidState &asteroid = state.asteroid_pool[0];
  asteroid.active = true;
  asteroid.wander_type = 0;
  asteroid.integrity = 100;
  asteroid.target_pos_x = 2.0F;
  asteroid.target_pos_y = 0.0F;
  const SpriteMask dot = MakeMask({"...", ".#.", "..."});
  asteroid.collision_mask.mask = &dot;
  asteroid.collision_mask.anchor_x = 1.5F;
  asteroid.collision_mask.anchor_y = 1.5F;

  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  state.active_shots[0].collision_mask = asteroid.collision_mask;
  NovaWeapon_ResolveDirectShotCollisions(state);

  // The 25px asteroid circle and 2px shot circle overlap at 2px separation,
  // but the single opaque pixels do not, so the mask miss preserves both.
  REQUIRE(state.active_shots.size() == 1);
  CHECK(asteroid.integrity == 100);
}

TEST_CASE("sprite mask store decodes shipped frames and refresh binds them",
          "[sprite_mask][data]") {
  const auto keys = NovaResource_AllKeys();
  const bool have_sheets =
      std::any_of(keys.begin(), keys.end(), [](const auto &key) {
        return key.first == kResourceTypeRleSheet16 && key.second == 1000;
      });
  if (!have_sheets) {
    SKIP("Nova Ships archives unavailable");
  }

  // Shuttle hull sheet 1000 is 24x24 with 108 rotation/bank frames.
  SpriteMaskStore store;
  const SpriteMask *hull = store.Sheet(1000, 0);
  REQUIRE(hull != nullptr);
  CHECK_FALSE(hull->Empty());
  CHECK(hull->width == 24);
  CHECK(hull->height == 24);
  CHECK(store.SheetFrameCount(1000) == 108);

  // A real asteroid spin set (800 + type) must also decode.
  const SpriteMask *asteroid_mask = store.Spin(800, 0);
  REQUIRE(asteroid_mask != nullptr);
  CHECK_FALSE(asteroid_mask->Empty());
  CHECK(store.SpinFrameCount(800) > 0);

  // The gameplay refresh binds the intended ship frame from the class's
  // sh\x8an base sheet and the asteroid's wander frame.
  GameState state;
  state.collision_masks_enabled = true;
  state.scenario.ships.resize(1);
  state.scenario.ships[0].base_image_id = 1000;
  state.scenario.ships[0].frames_per_rotation = 36;
  state.scenario.ships[0].sprite_behavior_flags = 0;
  state.player.is_active = true;
  state.player.ship_class_id = 0;
  state.player.current_system_id = 0;
  state.player.heading = 0.0F;
  AsteroidState &asteroid = state.asteroid_pool[0];
  asteroid.active = true;
  asteroid.wander_type = 0;
  asteroid.wander_frame_accumulator = 0.0F;

  NovaCollision_RefreshCollisionMasks(state);
  REQUIRE(state.player.collision_mask.HasMask());
  CHECK(state.player.collision_mask.mask->width == 24);
  CHECK(state.player.collision_mask.frame == 0); // heading 0 = frame 0
  REQUIRE(asteroid.collision_mask.HasMask());
  CHECK(asteroid.collision_mask.mask->width == asteroid_mask->width);
}

} // namespace game
