#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/collision.hpp"
#include "game/game_state.hpp"
#include "game/weapon.hpp"

namespace game {
namespace {

void SeedCollisionScenario(GameState &state) {
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
  target.pos_x = 10.0F;
  target.pos_y = 0.0F;
  target.collision_radius_px = 10.0F;
}

int SpawnTestShot(GameState &state) {
  const int shot = NovaWeapon_SpawnProjectile(state, 0, -1, 0);
  REQUIRE(shot == 0);
  return shot;
}

} // namespace

TEST_CASE("basic projectile carries owner and lifetime state", "[collision]") {
  GameState state;
  SeedCollisionScenario(state);

  const int shot_slot = SpawnTestShot(state);
  const ActiveShot &shot =
      state.active_shots[static_cast<std::size_t>(shot_slot)];
  CHECK(shot.owner_ship_slot == 0);
  CHECK(shot.target_ship_slot == -1);
  CHECK(shot.system_id == 0);
  CHECK(shot.life_frames == 10);
  CHECK(shot.life_ticks_remaining == Catch::Approx(10.0F));
}

TEST_CASE("projectile impact consumes shields before armor", "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  SpawnTestShot(state);

  REQUIRE(NovaWeapon_CanProjectileHitShip(state, state.active_shots[0], 1));
  NovaWeapon_ResolveProjectileCollisions(state);

  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(10.0F));
  CHECK(state.ShipAt(1).armor_points == Catch::Approx(100.0F));
  CHECK(state.ShipAt(1).primary_target_ship_slot == 0);
  // A player attack sets the hostile primary target, but does not overwrite
  // the separate escort/leader-chain link.
  CHECK(state.ShipAt(1).ai_target_ship_slot == -1);
  CHECK(state.ShipAt(1).ai_hostility_accumulator == 35);
  CHECK(state.ShipAt(1).hit_reaction_timer == Catch::Approx(32.0F));
  CHECK(state.ShipAt(1).ai_state_code == 4);
}

TEST_CASE("player can continue firing after target becomes hostile",
          "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  SpawnTestShot(state);

  NovaWeapon_ResolveProjectileCollisions(state);
  REQUIRE(state.active_shots.empty());

  SpawnTestShot(state);
  REQUIRE(NovaWeapon_CanProjectileHitShip(state, state.active_shots[0], 1));
  NovaWeapon_ResolveProjectileCollisions(state);

  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(0.0F));
  CHECK(state.ShipAt(1).armor_points == Catch::Approx(75.0F));
}

TEST_CASE("collision is resolved before projectile movement", "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  SpawnTestShot(state);
  state.active_shots[0].vel_x = 1000.0F;

  // The target overlaps the shot's current position. Moving first would send
  // this deliberately fast projectile past it; the frame loop's scope 9
  // collision pass must consume it before scope 7 Shot_HandleShot movement.
  NovaWeapon_ResolveProjectileCollisions(state);
  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(10.0F));
}

TEST_CASE("fractional projectile lifetime advances without rounding",
          "[collision]") {
  GameState state;
  SeedCollisionScenario(state);

  ActiveShot shot;
  shot.weapon_id = 0;
  shot.owner_ship_slot = 0;
  shot.system_id = 0;
  shot.life_frames = 2;
  shot.life_ticks_remaining = 1.5F;
  shot.vel_x = 2.0F;
  state.active_shots.push_back(shot);

  NovaWeapon_TickShots(state, 16.0F, 0.5F);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].pos_x == Catch::Approx(1.0F));
  CHECK(state.active_shots[0].life_ticks_remaining == Catch::Approx(1.0F));
  CHECK(state.active_shots[0].life_frames == 1);

  NovaWeapon_TickShots(state, 16.0F, 0.75F);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].pos_x == Catch::Approx(2.5F));
  CHECK(state.active_shots[0].life_ticks_remaining == Catch::Approx(0.25F));

  NovaWeapon_TickShots(state, 16.0F, 0.25F);
  CHECK(state.active_shots.empty());
}

TEST_CASE("mode one projectile requires its recorded target", "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.weapons[0].weapon_mode_code = 1;
  SpawnTestShot(state);

  CHECK(!NovaWeapon_CanProjectileHitShip(state, state.active_shots[0], 1));
  state.active_shots[0].target_ship_slot = 1;
  CHECK(NovaWeapon_CanProjectileHitShip(state, state.active_shots[0], 1));
}

TEST_CASE("shield-passing projectile applies direct armor damage",
          "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.weapons[0].flags = 0x0020U;
  SpawnTestShot(state);

  NovaWeapon_ResolveProjectileCollisions(state);

  CHECK(state.ShipAt(1).shield_points == Catch::Approx(20.0F));
  CHECK(state.ShipAt(1).armor_points == Catch::Approx(75.0F));
}

TEST_CASE("collision eligibility rejects friendly and scripted targets",
          "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  SpawnTestShot(state);

  state.player.faction_or_government_id = 4;
  state.ShipAt(1).faction_or_government_id = 4;
  CHECK(!NovaWeapon_CanProjectileHitShip(state, state.active_shots[0], 1));

  state.ShipAt(1).faction_or_government_id = -1;
  Ship &npc_owner = state.ShipAt(2);
  npc_owner = state.ShipAt(1);
  npc_owner.ship_instance_id = 2;
  npc_owner.ai_state_code = 0x10;
  ActiveShot npc_shot = state.active_shots[0];
  npc_shot.owner_ship_slot = 2;
  CHECK(!NovaWeapon_CanProjectileHitShip(state, npc_shot, 1));
}

TEST_CASE("lethal projectile leaves destruction to armor state and is consumed",
          "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  state.ShipAt(1).shield_points = 0.0F;
  state.ShipAt(1).armor_points = 10.0F;
  SpawnTestShot(state);

  NovaWeapon_ResolveProjectileCollisions(state);

  CHECK(state.active_shots.empty());
  // Shot_ResolveShipHitFromWeapon does not arm the death timer itself; the
  // original ship handler consumes armor <= 0 on its later pass.
  CHECK(state.ShipAt(1).armor_points == Catch::Approx(-15.0F));
  CHECK(state.ShipAt(1).death_timer_active == Catch::Approx(-1.0F));
  CHECK(!NovaWeapon_CanProjectileHitShip(state, ActiveShot{}, 1));
}

TEST_CASE("late collision window stops contacts near expiry", "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.weapons[0].late_collision_window_ticks = 3;
  SpawnTestShot(state);

  state.active_shots[0].life_ticks_remaining = 2.0F;
  NovaWeapon_ResolveProjectileCollisions(state);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(20.0F));

  state.active_shots[0].life_ticks_remaining = 4.0F;
  NovaWeapon_ResolveProjectileCollisions(state);
  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(10.0F));
}

} // namespace game
