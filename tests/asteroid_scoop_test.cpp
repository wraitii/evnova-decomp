// Asteroid mining/scooping: the YieldQty/YieldType resource-box producer in
// Asteroid_SpawnDestructionPackage (0x00462550, ported as
// ResolveAsteroidDestructionPackage in collision.cpp) and the freeflight-object
// scoop arm of Ship_HandleSpritePairCollision (0x004374f0, ported as
// NovaWeapon_ResolveFreeflightScoop). See the EV Nova Bible r\xf6id section
// (YieldType 0..5 cargo / 1000..1127 j\xfcnk, YieldQty average +/- 50%) and the
// Ship Flags3 notes (0x0002 "scoops asteroid debris").

#include "game/collision.hpp"
#include "game/freeflight_objects.hpp"
#include "game/game_state.hpp"
#include "game/outfit.hpp"
#include "game/weapon.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace game {
namespace {

// Collision *logic* tests stay on the circle envelope so they do not depend on
// the shipped sprite archives. sprite_mask_test.cpp covers the generic mask
// machinery, but the freeflight scoop's pixel-mask overlap is not exercised by
// a dedicated sprite-resource test here.
void SeedPlayerForCollision(GameState &state) {
  state.scenario.ships.resize(1);
  state.scenario.ships[0].cargo_holds = 10;
  state.collision_masks_enabled = false;
  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.ship_class_id = 0;
  state.player.current_system_id = 0;
  state.player.armor_points = 100.0F;
  state.player.shield_points = 100.0F;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.player.collision_radius_px = 10.0F;
  state.player.faction_or_government_id = -1;
}

int ActiveFreeflightCount(const GameState &state) {
  int count = 0;
  for (const FreeflightObjectState &object : state.freeflight_objects) {
    if (object.lifetime_ticks >= 0.0F) {
      ++count;
    }
  }
  return count;
}

int ActiveChildAsteroidCount(const GameState &state) {
  int count = 0;
  for (std::size_t i = 1; i < state.asteroid_pool.size(); ++i) {
    if (state.asteroid_pool[i].active) {
      ++count;
    }
  }
  return count;
}

// One breakable asteroid type (integrity 5) with the requested yield/fragment
// row, plus a player-owned zero-speed weapon that removes exactly 10
// integrity. Returns the live asteroid record.
AsteroidState &SeedBreakableAsteroid(GameState &state,
                                     std::int16_t yield_qty,
                                     std::int16_t yield_type,
                                     std::array<std::int16_t, 3> fragments) {
  SeedPlayerForCollision(state);
  state.rng.seed(0x5eed);

  state.scenario.weapons.resize(1);
  Weapon &weapon = state.scenario.weapons[0];
  weapon.weapon_mode_code = -1;
  weapon.projectile_speed = 0.0F;
  weapon.lifetime_ticks = 10;
  weapon.mass_damage = 0;
  weapon.energy_damage = 10;

  state.scenario.asteroid_defs.resize(1);
  AsteroidDef &def = state.scenario.asteroid_defs[0];
  def.strength = 5;
  def.yield_qty = yield_qty;
  def.yield_type = yield_type;
  def.explode_type = -1; // no explosion effect (keeps RNG/effect pools clean)
  def.frag_type1 = fragments[0];
  def.frag_type2 = fragments[1];
  def.frag_count = fragments[2];

  AsteroidState &asteroid = state.asteroid_pool[0];
  asteroid.active = true;
  asteroid.wander_type = 0;
  asteroid.integrity = 5;
  asteroid.target_pos_x = 0.0F;
  asteroid.target_pos_y = 0.0F;
  return asteroid;
}

} // namespace

TEST_CASE("destroyed asteroid ejects YieldQty resource-boxes of YieldType",
          "[asteroid][scoop]") {
  GameState state;
  AsteroidState &asteroid = SeedBreakableAsteroid(state,
                                                  /*yield_qty=*/40,
                                                  /*yield_type=*/2,
                                                  /*fragments=*/{-1, -1, 0});

  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  NovaWeapon_ResolveDirectShotCollisions(state);

  REQUIRE_FALSE(asteroid.active);
  // (NovaRandom_Range(0x65) + 0x32) * 40 * 0.01 -> 20..60 boxes.
  const int boxes = ActiveFreeflightCount(state);
  CHECK(boxes >= 20);
  CHECK(boxes <= 60);
  CHECK(ActiveChildAsteroidCount(state) == 0);
  for (const FreeflightObjectState &object : state.freeflight_objects) {
    if (object.lifetime_ticks < 0.0F) {
      continue;
    }
    CHECK(object.persistent);
    CHECK(object.extra == 2);
    CHECK(object.system_id == 0);
    // wander_type 0 -> material slot 0 -> the 501 spin set.
    CHECK(object.sprite_set_index == 1);
  }
}

TEST_CASE("a fragmenting asteroid still spawns its children and its yield",
          "[asteroid][scoop]") {
  GameState state;
  AsteroidState &asteroid = SeedBreakableAsteroid(state,
                                                  /*yield_qty=*/10,
                                                  /*yield_type=*/5,
                                                  /*fragments=*/{0, -1, 2});

  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  NovaWeapon_ResolveDirectShotCollisions(state);

  REQUIRE_FALSE(asteroid.active);
  // Child count = rand(2) + floor(2/2) = 1..2.
  const int children = ActiveChildAsteroidCount(state);
  CHECK(children >= 1);
  CHECK(children <= 2);
  // Yield = (rand(101) + 50) * 10 * 0.01 -> 5..15.
  const int boxes = ActiveFreeflightCount(state);
  CHECK(boxes >= 5);
  CHECK(boxes <= 15);
}

TEST_CASE("frag_count 1 spawns one child (BUGFIX(original))",
          "[asteroid][scoop]") {
  GameState state;
  AsteroidState &asteroid = SeedBreakableAsteroid(state,
                                                  /*yield_qty=*/0,
                                                  /*yield_type=*/0,
                                                  /*fragments=*/{0, -1, 1});

  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  NovaWeapon_ResolveDirectShotCollisions(state);

  REQUIRE_FALSE(asteroid.active);
  // Original: rand(1) + floor(1/2) == 0 children. BUGFIX(original): the Bible
  // documents the average +/-50%, whose lower bound is one.
  CHECK(ActiveChildAsteroidCount(state) == 1);
}

TEST_CASE("odd frag_count keeps the original floor baseline",
          "[asteroid][scoop]") {
  GameState state;
  AsteroidState &asteroid = SeedBreakableAsteroid(state,
                                                  /*yield_qty=*/0,
                                                  /*yield_type=*/0,
                                                  /*fragments=*/{0, -1, 3});

  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  NovaWeapon_ResolveDirectShotCollisions(state);

  REQUIRE_FALSE(asteroid.active);
  // floor(3/2)=1 baseline, roll 0..2 -> 1..3 (the +/-50% lower bound would be
  // 2; the original deliberately stays one low for odd counts).
  const int children = ActiveChildAsteroidCount(state);
  CHECK(children >= 1);
  CHECK(children <= 3);
}

TEST_CASE("scoop-equipped player collects cargo from a resource-box",
          "[scoop]") {
  GameState state;
  SeedPlayerForCollision(state);
  state.player.mining_scoop_active = true;

  FreeflightObjectState &box = state.freeflight_objects[0];
  box.lifetime_ticks = 100.0F;
  box.persistent = true;
  box.system_id = 0;
  box.extra = 2;
  box.pos_x = 0.0F;
  box.pos_y = 0.0F;

  NovaWeapon_ResolveFreeflightScoop(state);

  CHECK(box.lifetime_ticks < 0.0F);
  CHECK(state.inventory.cargo_bins[2] == 1);
}

TEST_CASE("a ship without a scoop ignores resource-boxes", "[scoop]") {
  GameState state;
  SeedPlayerForCollision(state);
  state.player.mining_scoop_active = false;

  FreeflightObjectState &box = state.freeflight_objects[0];
  box.lifetime_ticks = 100.0F;
  box.persistent = true;
  box.system_id = 0;
  box.extra = 2;
  box.pos_x = 0.0F;
  box.pos_y = 0.0F;

  NovaWeapon_ResolveFreeflightScoop(state);

  CHECK(box.lifetime_ticks >= 0.0F);
  CHECK(state.inventory.cargo_bins[2] == 0);
}

TEST_CASE("player scoop banks junk payloads into the junk counts", "[scoop]") {
  GameState state;
  SeedPlayerForCollision(state);
  state.player.mining_scoop_active = true;

  FreeflightObjectState &box = state.freeflight_objects[0];
  box.lifetime_ticks = 100.0F;
  box.persistent = true;
  box.system_id = 0;
  box.extra = 1003; // j\xfcnk index 3 (resource id 0x83)
  box.pos_x = 0.0F;
  box.pos_y = 0.0F;

  NovaWeapon_ResolveFreeflightScoop(state);

  CHECK(box.lifetime_ticks < 0.0F);
  CHECK(state.inventory.junk_counts[3] == 1);
  for (const std::int16_t bin : state.inventory.cargo_bins) {
    CHECK(bin == 0);
  }
}

TEST_CASE("jettisoned non-persistent pods are not scooped", "[scoop]") {
  GameState state;
  SeedPlayerForCollision(state);
  state.player.mining_scoop_active = true;

  // Ship_SpawnFreeflightObjectForShip (0x0041f800) leaves +0x24 clear; the
  // scoop arm requires it set, so jettisoned cargo pods are not recoverable.
  FreeflightObjectState &pod = state.freeflight_objects[0];
  pod.lifetime_ticks = 100.0F;
  pod.persistent = false;
  pod.system_id = 0;
  pod.extra = 2;
  pod.pos_x = 0.0F;
  pod.pos_y = 0.0F;

  NovaWeapon_ResolveFreeflightScoop(state);

  CHECK(pod.lifetime_ticks >= 0.0F);
  CHECK(state.inventory.cargo_bins[2] == 0);
}

TEST_CASE("cargo capacity clears and re-arms the mining-scoop latch",
          "[scoop][outfit]") {
  GameState state;
  state.scenario.ships.resize(1);
  state.scenario.ships[0].cargo_holds = 5;
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].mod_type = 0x1f; // kMiningScoop
  state.inventory.outfit_owned_count[0] = 1;
  state.inventory.cargo_bins.fill(0);
  state.inventory.junk_counts.fill(0);
  state.player.ship_instance_id = 0;
  state.player.ship_class_id = 0;

  state.inventory.cargo_bins[0] = 4;
  NovaOutfit_RefreshPlayerMiningScoopActive(state);
  CHECK(state.player.mining_scoop_active);

  state.inventory.cargo_bins[0] = 5; // at capacity
  NovaOutfit_RefreshPlayerMiningScoopActive(state);
  CHECK_FALSE(state.player.mining_scoop_active);

  state.inventory.cargo_bins[0] = 3; // space freed
  NovaOutfit_RefreshPlayerMiningScoopActive(state);
  CHECK(state.player.mining_scoop_active);
}

TEST_CASE(
    "an owned mining scoop fills to capacity then leaves later boxes live",
    "[scoop][outfit]") {
  GameState state;
  SeedPlayerForCollision(state);
  state.scenario.ships.resize(1);
  state.scenario.ships[0].cargo_holds = 2;
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].mod_type = 0x1f; // kMiningScoop
  state.inventory.outfit_owned_count[0] = 1;
  state.inventory.cargo_bins.fill(0);
  state.inventory.junk_counts.fill(0);
  state.player.ship_class_id = 0;

  // Derive the latch from the actually-owned scoop outfit (not a manual set).
  NovaOutfit_RefreshPlayerMiningScoopActive(state);
  REQUIRE(state.player.mining_scoop_active);

  // Three overlapping boxes, all worth one unit of cargo bin 2. A 2-ton hold
  // can take only two; the third must stay live because the pickup recompute
  // clears the scoop latch the moment cargo reaches capacity.
  constexpr std::size_t kBoxes = 3;
  for (std::size_t i = 0; i < kBoxes; ++i) {
    FreeflightObjectState &box = state.freeflight_objects[i];
    box.lifetime_ticks = 100.0F;
    box.persistent = true;
    box.system_id = 0;
    box.extra = 2;
    box.pos_x = 0.0F;
    box.pos_y = 0.0F;
  }

  NovaWeapon_ResolveFreeflightScoop(state);

  CHECK(state.inventory.cargo_bins[2] == 2);
  CHECK(Player_ComputeCargoAndJunkTotal(state) == 2);
  CHECK(state.freeflight_objects[0].lifetime_ticks < 0.0F);
  CHECK(state.freeflight_objects[1].lifetime_ticks < 0.0F);
  CHECK(state.freeflight_objects[2].lifetime_ticks >= 0.0F);
  CHECK_FALSE(state.player.mining_scoop_active);
}

TEST_CASE("a full freeflight pool drops yield without overwriting", "[scoop]") {
  GameState state;
  SeedPlayerForCollision(state);
  for (FreeflightObjectState &object : state.freeflight_objects) {
    object.lifetime_ticks = 0.0F; // active
    object.extra = 99;
  }

  NovaFreeflight_SpawnAtPosition(state,
                                 1.0F,
                                 2.0F,
                                 /*extra=*/2,
                                 /*sprite_set_index=*/1);

  for (const FreeflightObjectState &object : state.freeflight_objects) {
    CHECK(object.extra == 99);
  }
  CHECK(ActiveFreeflightCount(state) ==
        static_cast<int>(FreeflightObjectState::kPoolSize));
}

} // namespace game
