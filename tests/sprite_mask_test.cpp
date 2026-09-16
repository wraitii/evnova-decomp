#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/collision.hpp"
#include "game/game_state.hpp"
#include "game/mission.hpp"
#include "game/new_pilot_flow.hpp"
#include "game/ship_visual.hpp"
#include "game/sprite_mask.hpp"
#include "game/targeting.hpp"
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

// Stellar contact scenarios: one system with nav_defs[0] = stellar 0x80, one
// planet-type-capable weapon (0x80), and one ship class. Masks are injected, so
// the live refresh is disabled unless a test opts back in.
void SeedStellarScenario(GameState &state) {
  state.collision_masks_enabled = false;
  state.scenario.weapons.resize(1);
  Weapon &weapon = state.scenario.weapons[0];
  weapon.weapon_mode_code = -1;
  weapon.projectile_speed = 0.0F;
  weapon.lifetime_ticks = 10;
  weapon.mass_damage = 20;
  weapon.energy_damage = 30;
  weapon.flags_secondary = 0x0400; // planet-type weapon
  weapon.impact_effect_id = -1;
  weapon.splash_radius = 0;
  weapon.blast_radius = 0;

  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs.fill(-1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Test Stellar";
  stellar.pos_x = 0;
  stellar.pos_y = 0;
  stellar.strength_capacity = 100; // Strength capacity > 0
  stellar.strength = 100;
  stellar.availability_flags = 0x100; // fatal collision body
  stellar.explosion_type = -1;
  stellar.destroyed_days_remaining = 0;
  stellar.schedule_days = 0;

  state.scenario.ships.resize(1);

  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.ship_class_id = 0;
  state.player.current_system_id = 0;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.player.armor_points = 100.0F;
  state.player.primary_target_ship_slot = -1;
}

void SeedCrashNpc(GameState &state) {
  Ship &target = state.ShipAt(1);
  target.is_active = true;
  target.ship_instance_id = 1;
  target.ship_class_id = 0;
  target.current_system_id = 0;
  target.armor_points = 100.0F;
  target.death_timer_active = 0.0F;
  target.pos_x = 0.0F;
  target.pos_y = 0.0F;
}

// 3x3 single-opaque-centre mask; world positions 2px apart overlap only in
// transparent corners.
const SpriteMask kStellarDot = MakeMask({"...", ".#.", "..."});

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
  state.scenario.asteroid_defs[0].strength = 100;
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
  state.scenario.asteroid_defs[0].strength = 100;
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

TEST_CASE("planet-type weapon pixel-mask contact uses opaque overlap",
          "[collision][stellar]") {
  GameState state;
  SeedStellarScenario(state);
  Stellar &stellar = state.scenario.stellars[0];
  stellar.collision_mask.mask = &kStellarDot;
  stellar.collision_mask.anchor_x = 1.5F;
  stellar.collision_mask.anchor_y = 1.5F;

  // Transparent AABB overlap: 2px apart, centres never coincide -> miss.
  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  ActiveShot &shot = state.active_shots[0];
  shot.pos_x = 0.0F;
  shot.pos_y = 0.0F;
  shot.collision_mask = stellar.collision_mask;
  stellar.pos_x = 2;
  NovaWeapon_ResolveProjectileCollisions(state);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(stellar.strength == 100);

  // Opaque hit at the same position: 50 combined damage, shot consumed.
  stellar.pos_x = 0;
  state.active_shots[0].pos_x = 0.0F;
  state.active_shots[0].pos_y = 0.0F;
  NovaWeapon_ResolveProjectileCollisions(state);
  CHECK(state.active_shots.empty());
  CHECK(stellar.strength == 50);
}

TEST_CASE("non-planet-type weapons pass through stellars",
          "[collision][stellar]") {
  GameState state;
  SeedStellarScenario(state);
  state.scenario.weapons[0].flags_secondary = 0; // not 0x400
  Stellar &stellar = state.scenario.stellars[0];
  stellar.collision_mask.mask = &kStellarDot;
  stellar.collision_mask.anchor_x = 1.5F;
  stellar.collision_mask.anchor_y = 1.5F;

  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  state.active_shots[0].collision_mask = stellar.collision_mask;
  NovaWeapon_ResolveProjectileCollisions(state);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(stellar.strength == 100);
}

TEST_CASE("stellar destruction re-arms the regeneration countdown",
          "[collision][stellar]") {
  GameState state;
  SeedStellarScenario(state);
  Stellar &stellar = state.scenario.stellars[0];
  stellar.strength = 10; // below the 50 combined damage
  stellar.schedule_days = 7;
  stellar.collision_mask.mask = &kStellarDot;
  stellar.collision_mask.anchor_x = 1.5F;
  stellar.collision_mask.anchor_y = 1.5F;

  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  state.active_shots[0].collision_mask = stellar.collision_mask;
  NovaWeapon_ResolveProjectileCollisions(state);
  CHECK(state.active_shots.empty());
  // The destruction package clamps Strength to -1 and seeds
  // destroyed_days_remaining from the schedule day field (Ghidra +0x3c = -1,
  // +0x47c = +0x47a).
  CHECK(stellar.strength == -1);
  CHECK(stellar.destroyed_days_remaining == 7);

  // A destroyed (now "active") stellar no longer accepts planet-type fire.
  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  state.active_shots[0].collision_mask = stellar.collision_mask;
  NovaWeapon_ResolveProjectileCollisions(state);
  REQUIRE(state.active_shots.size() == 1);
}

TEST_CASE("fatal stellar crash instant-kills a non-immune ship",
          "[collision][stellar]") {
  GameState state;
  SeedStellarScenario(state);
  SeedCrashNpc(state);
  Stellar &stellar = state.scenario.stellars[0];
  stellar.collision_mask.mask = &kStellarDot;
  stellar.collision_mask.anchor_x = 1.5F;
  stellar.collision_mask.anchor_y = 1.5F;
  Ship &target = state.ShipAt(1);
  target.collision_mask.mask = &kStellarDot;
  target.collision_mask.anchor_x = 1.5F;
  target.collision_mask.anchor_y = 1.5F;
  state.player.primary_target_ship_slot = 1;

  NovaStellar_HandleShipStellarCrash(state);
  CHECK_FALSE(target.is_active);
  CHECK(target.armor_points == Catch::Approx(-1000.0F));
  CHECK(target.death_timer_active == Catch::Approx(0.0F));
  CHECK(state.player.primary_target_ship_slot == -1);
}

TEST_CASE("fatal stellar crash gates on flags, transparency, and immunity",
          "[collision][stellar]") {
  const auto run_crash =
      [](bool fatal, bool overlap, bool npc, std::uint16_t class_flags) {
        GameState state;
        SeedStellarScenario(state);
        SeedCrashNpc(state);
        Stellar &stellar = state.scenario.stellars[0];
        stellar.availability_flags = fatal ? 0x100 : 0x0000;
        stellar.collision_mask.mask = &kStellarDot;
        stellar.collision_mask.anchor_x = 1.5F;
        stellar.collision_mask.anchor_y = 1.5F;
        stellar.pos_x = overlap ? 0 : 100;
        state.scenario.ships[0].availability_flags = class_flags;
        Ship &target = state.ShipAt(1);
        target.ship_instance_id = npc ? 1 : 0;
        target.collision_mask.mask = &kStellarDot;
        target.collision_mask.anchor_x = 1.5F;
        target.collision_mask.anchor_y = 1.5F;
        NovaStellar_HandleShipStellarCrash(state);
        return target.is_active;
      };

  CHECK_FALSE(run_crash(/*fatal=*/true, /*overlap=*/true, /*npc=*/true, 0));
  CHECK(run_crash(/*fatal=*/false, /*overlap=*/true, /*npc=*/true, 0));
  CHECK(run_crash(/*fatal=*/true, /*overlap=*/false, /*npc=*/true, 0));
  // Ship-class availability_flags 0x20 grants crash immunity (0x0046e210).
  CHECK(run_crash(/*fatal=*/true, /*overlap=*/true, /*npc=*/true, 0x20));
}

TEST_CASE("player outfit ModType 0x2a grants crash immunity",
          "[collision][stellar]") {
  GameState state;
  SeedStellarScenario(state);
  Stellar &stellar = state.scenario.stellars[0];
  stellar.collision_mask.mask = &kStellarDot;
  stellar.collision_mask.anchor_x = 1.5F;
  stellar.collision_mask.anchor_y = 1.5F;
  // The player ship is the victim; without an outfit it dies.
  Ship &player = state.player;
  player.collision_mask.mask = &kStellarDot;
  player.collision_mask.anchor_x = 1.5F;
  player.collision_mask.anchor_y = 1.5F;

  NovaStellar_HandleShipStellarCrash(state);
  CHECK_FALSE(player.is_active);

  // Re-run with an owned ModType-0x2a outfit: the player survives.
  player.is_active = true;
  player.armor_points = 100.0F;
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].mod_type = 0x2a;
  state.inventory.outfit_owned_count[0] = 1;
  NovaStellar_HandleShipStellarCrash(state);
  CHECK(player.is_active);
}

TEST_CASE("stellar mask refresh binds the shipped ambient frame",
          "[collision][stellar][data]") {
  const auto keys = NovaResource_AllKeys();
  SpriteMaskStore store;
  std::uint16_t spin_id = 0;
  int frame_count = 0;
  for (const auto &key : keys) {
    if (key.first != kResourceTypeSprites || key.second < 1000 ||
        key.second > 1255) {
      continue;
    }
    const int count = store.SpinFrameCount(key.second);
    if (count > 1) {
      spin_id = key.second;
      frame_count = count;
      break;
    }
  }
  if (spin_id == 0) {
    SKIP("Nova stellar spin archives unavailable");
  }

  REQUIRE(frame_count > 1);
  REQUIRE(store.Spin(spin_id, 0) != nullptr);

  GameState state;
  SeedStellarScenario(state);
  state.collision_masks_enabled = true;
  Stellar &stellar = state.scenario.stellars[0];
  stellar.link_a_id = static_cast<std::int16_t>(spin_id - 1000);
  stellar.link_b_id = -1;
  stellar.sprite_current_frame = 1;

  NovaCollision_RefreshCollisionMasks(state);
  REQUIRE(stellar.collision_mask.HasMask());
  CHECK(stellar.collision_mask.frame == 1);
  CHECK(stellar.collision_mask.mask->width > 0);
}

TEST_CASE("stellar active state and sprite link follow strength",
          "[collision][stellar]") {
  Stellar st;
  st.link_a_id = 3;
  st.link_b_id = 7;
  st.strength_capacity = 100;
  st.strength = 100;
  CHECK_FALSE(NovaTargeting_IsStellarActive(st));
  CHECK(NovaTargeting_StellarSpriteLinkId(st) == 3);

  st.strength = -1; // destroyed -> active, alternate zone
  CHECK(NovaTargeting_IsStellarActive(st));
  CHECK(NovaTargeting_StellarSpriteLinkId(st) == 7);

  st.link_b_id = -1; // no alternate zone -> fall back to link_a
  CHECK(NovaTargeting_StellarSpriteLinkId(st) == 3);

  st.strength = 100;
  st.destroyed_days_remaining = 5; // engaged -> active
  CHECK(NovaTargeting_IsStellarActive(st));
  CHECK(NovaTargeting_StellarSpriteLinkId(st) == 3);

  // Invincible capacity (Bible Strength 0/-1) is never active.
  st.strength_capacity = 0;
  CHECK_FALSE(NovaTargeting_IsStellarActive(st));
}

TEST_CASE("daily stellar regeneration restores live strength",
          "[collision][stellar]") {
  GameState state;
  SeedStellarScenario(state);
  Stellar &stellar = state.scenario.stellars[0];
  stellar.is_available = true;
  stellar.strength = -1;
  stellar.destroyed_days_remaining = 1;
  stellar.schedule_days = 3;
  REQUIRE(NovaTargeting_IsStellarActive(stellar));

  Mission_TickDailyWorldUpdate(state);
  CHECK(stellar.strength == stellar.strength_capacity);
  CHECK_FALSE(NovaTargeting_IsStellarActive(stellar));
}

TEST_CASE("starts-destroyed stellars initialize strength and countdown",
          "[collision][stellar]") {
  GameState state;
  SeedStellarScenario(state);
  state.scenario.stellars.resize(2);
  Stellar &destroyed = state.scenario.stellars[0];
  destroyed.availability_flags |= 0x40;
  destroyed.strength_capacity = 50;
  destroyed.strength = 50;
  destroyed.schedule_days = 4;
  Stellar &normal = state.scenario.stellars[1];
  normal.availability_flags = 0;
  normal.strength_capacity = 50;
  normal.strength = 7;

  NovaNewPilot_ResetStellarStrengthForNewGame(state);
  CHECK(destroyed.strength == -1);
  CHECK(destroyed.destroyed_days_remaining == 4);
  CHECK(normal.strength == 50);
  CHECK(normal.destroyed_days_remaining == 0);

  // A negative schedule seed pins the regeneration countdown at 1.
  destroyed.schedule_days = -3;
  NovaNewPilot_ResetStellarStrengthForNewGame(state);
  CHECK(destroyed.strength == -1);
  CHECK(destroyed.destroyed_days_remaining == 1);
}

TEST_CASE("crash deactivates immediately while weapon damage enters death",
          "[collision][stellar]") {
  // Standard lethal weapon damage leaves the hull active and arms the death
  // timer, entering the Explode1/Explode2 death presentation.
  {
    GameState state;
    SeedStellarScenario(state);
    state.scenario.ships[0].death_delay_frames = 10;
    Ship &target = state.ShipAt(1);
    target.is_active = true;
    target.ship_instance_id = 1;
    target.ship_class_id = 0;
    target.current_system_id = 0;
    target.armor_points = 5.0F;
    ResolveShipHitFromWeapon(state,
                             /*target_slot=*/1,
                             target,
                             target.pos_x,
                             target.pos_y,
                             /*impact_impulse=*/0,
                             /*armor_damage=*/100,
                             /*shield_damage=*/0,
                             /*attacker_ship_slot=*/0,
                             /*allow_aggro_updates=*/false,
                             /*suppress_retarget_logic=*/false,
                             /*force_armor_only=*/false,
                             /*bypass_shields=*/true,
                             /*player_aggro_delta=*/0);
    CHECK(target.is_active);
    // The hit site leaves destruction as an armor state; Ship_UpdateVisualState
    // seeds the class DeathDelay timer.
    CHECK(target.death_timer_active <= 0.0F);
    NovaShip_TickDestroyedShipVisualState(state, target, 1.0F);
    CHECK(target.death_timer_active == Catch::Approx(10.0F));
  }
  // Fatal-stellar contact is an instant kill: active cleared and the death
  // timer forced to 0, so no death presentation runs afterwards.
  {
    GameState state;
    SeedStellarScenario(state);
    SeedCrashNpc(state);
    state.scenario.ships[0].death_delay_frames = 10;
    Stellar &stellar = state.scenario.stellars[0];
    stellar.collision_mask.mask = &kStellarDot;
    stellar.collision_mask.anchor_x = 1.5F;
    stellar.collision_mask.anchor_y = 1.5F;
    Ship &target = state.ShipAt(1);
    target.collision_mask.mask = &kStellarDot;
    target.collision_mask.anchor_x = 1.5F;
    target.collision_mask.anchor_y = 1.5F;

    NovaStellar_HandleShipStellarCrash(state);
    CHECK_FALSE(target.is_active);
    CHECK(target.death_timer_active == Catch::Approx(0.0F));
    CHECK(target.armor_points == Catch::Approx(-1000.0F));
  }
}

} // namespace game
