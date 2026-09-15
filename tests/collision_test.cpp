#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/collision.hpp"
#include "game/game_state.hpp"
#include "game/ship_ai.hpp"
#include "game/ship_visual.hpp"
#include "game/spaceflight.hpp"
#include "game/weapon.hpp"

#include <algorithm>

namespace game {
namespace {

void SeedCollisionScenario(GameState &state) {
  // Collision *logic* tests stay on the explicit circle envelope so they are
  // independent of the shipped sprite masks; the pixel-mask path has its own
  // tests in sprite_mask_test.cpp.
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
  // Real ships carry a nonzero InherentAI behavior code; the hit path only
  // applies aggro/retarget updates when ai_behavior_code > 0 (0x004192d0).
  target.ai_behavior_code = 5;
  target.pos_x = 10.0F;
  target.pos_y = 0.0F;
  target.collision_radius_px = 10.0F;
}

int SpawnTestShot(GameState &state) {
  const int shot = NovaWeapon_SpawnProjectile(state, 0, -1, 0);
  REQUIRE(shot == 0);
  return shot;
}

void ActivateHostileShip(GameState &state,
                         std::int16_t slot,
                         float x,
                         float y) {
  Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.is_active = true;
  ship.ship_instance_id = slot;
  ship.ship_class_id = 0;
  ship.current_system_id = 0;
  ship.armor_points = 100.0F;
  ship.shield_points = 100.0F;
  ship.ai_behavior_code = 5;
  ship.pos_x = x;
  ship.pos_y = y;
  ship.collision_radius_px = 10.0F;
}

// weapon[0] is the blast weapon that reaches weapon[1] as its submunition
// (Bible SubCount/SubType).
void SeedLinkedShotScenario(GameState &state) {
  SeedCollisionScenario(state);
  state.scenario.weapons.resize(2);
  Weapon &parent = state.scenario.weapons[0];
  parent.blast_radius = 50;
  parent.projectile_speed = 100.0F; // 1 px/frame
  parent.range_link_gate = 2;
  parent.range_link_weapon_id = 1;
  parent.range_link_spread = 0;

  Weapon &child = state.scenario.weapons[1];
  child.weapon_mode_code = -1;
  child.projectile_speed = 200.0F; // 2 px/frame
  child.lifetime_ticks = 20;
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

TEST_CASE("combat rating credits only player-side kills",
          "[collision][rating]") {
  auto kill_target = [](GameState &state, std::int16_t attacker_slot) {
    Ship &target = state.ShipAt(1);
    ResolveShipHitFromWeapon(state,
                             /*target_slot=*/1,
                             target,
                             target.pos_x,
                             target.pos_y,
                             /*impact_impulse=*/0,
                             /*armor_damage=*/200,
                             /*shield_damage=*/0,
                             attacker_slot,
                             /*allow_aggro_updates=*/true,
                             /*suppress_retarget_logic=*/true,
                             /*force_armor_only=*/false,
                             /*bypass_shields=*/true,
                             /*player_aggro_delta=*/0);
  };

  SECTION("player kill is credited") {
    GameState state;
    SeedCollisionScenario(state);
    state.scenario.ships[0].strength = 50;
    kill_target(state, 0);
    CHECK(state.player_combat_rating_points == 10);
  }

  SECTION("direct player escort kill is credited") {
    GameState state;
    SeedCollisionScenario(state);
    state.scenario.ships[0].strength = 50;
    ActivateHostileShip(state, 2, 20.0F, 0.0F);
    state.ShipAt(2).squad_leader_ship_slot = 0;
    kill_target(state, 2);
    CHECK(state.player_combat_rating_points == 10);
  }

  SECTION("unrelated NPC kill is not credited") {
    GameState state;
    SeedCollisionScenario(state);
    state.scenario.ships[0].strength = 50;
    ActivateHostileShip(state, 2, 20.0F, 0.0F);
    kill_target(state, 2);
    CHECK(state.player_combat_rating_points == 0);
  }

  SECTION("defense-fleet victim is not credited") {
    GameState state;
    SeedCollisionScenario(state);
    state.scenario.ships[0].strength = 50;
    state.ShipAt(1).defense_fleet_home_stellar_id = 0x80;
    kill_target(state, 0);
    CHECK(state.player_combat_rating_points == 0);
  }
}

TEST_CASE("projectile impact consumes shields before armor", "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  SpawnTestShot(state);
  state.active_shots[0].target_ship_slot = 1;

  REQUIRE(NovaWeapon_CanProjectileHitShip(state, state.active_shots[0], 1));
  NovaWeapon_ResolveDirectShotCollisions(state);

  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(10.0F));
  CHECK(state.ShipAt(1).armor_points == Catch::Approx(100.0F));
  CHECK(state.ShipAt(1).primary_target_ship_slot == 0);
  // A player attack sets the hostile primary target, but does not overwrite
  // the separate escort/leader-chain link.
  CHECK(state.ShipAt(1).squad_leader_ship_slot == -1);
  CHECK(state.ShipAt(1).ai_hostility_accumulator == 35);
  CHECK(state.ShipAt(1).shield_bubble_flash_intensity == Catch::Approx(32.0F));
  CHECK(state.ShipAt(1).ai_state_code == 4);
}

TEST_CASE("incidental player hits accumulate before a later retarget",
          "[collision][aggro]") {
  GameState state;
  SeedCollisionScenario(state);
  Ship &target = state.ShipAt(1);
  target.player_aggro_accumulator = 45.0F;
  target.primary_target_ship_slot = -1;
  target.ai_state_code = 0;

  ResolveShipHitFromWeapon(state,
                           /*target_slot=*/1,
                           target,
                           target.pos_x,
                           target.pos_y,
                           /*impact_impulse=*/0,
                           /*armor_damage=*/1,
                           /*shield_damage=*/1,
                           /*attacker_ship_slot=*/0,
                           /*allow_aggro_updates=*/true,
                           /*suppress_retarget_logic=*/false,
                           /*force_armor_only=*/false,
                           /*bypass_shields=*/false,
                           /*player_aggro_delta=*/10);

  // The threshold test precedes this hit's +15 increment.
  CHECK(target.player_aggro_accumulator == Catch::Approx(62.5F));
  CHECK(target.primary_target_ship_slot == -1);
  CHECK(target.ai_state_code == 0);

  ResolveShipHitFromWeapon(state,
                           /*target_slot=*/1,
                           target,
                           target.pos_x,
                           target.pos_y,
                           /*impact_impulse=*/0,
                           /*armor_damage=*/1,
                           /*shield_damage=*/1,
                           /*attacker_ship_slot=*/0,
                           /*allow_aggro_updates=*/true,
                           /*suppress_retarget_logic=*/false,
                           /*force_armor_only=*/false,
                           /*bypass_shields=*/false,
                           /*player_aggro_delta=*/10);

  CHECK(target.player_aggro_accumulator == Catch::Approx(0.0F));
  CHECK(target.primary_target_ship_slot == 0);
  CHECK(target.ai_state_code == 4);
}

TEST_CASE(
    "targeted player hits use the ordinary response without aggro buildup",
    "[collision][aggro]") {
  GameState state;
  SeedCollisionScenario(state);
  Ship &target = state.ShipAt(1);

  ResolveShipHitFromWeapon(state,
                           /*target_slot=*/1,
                           target,
                           target.pos_x,
                           target.pos_y,
                           /*impact_impulse=*/0,
                           /*armor_damage=*/1,
                           /*shield_damage=*/1,
                           /*attacker_ship_slot=*/0,
                           /*allow_aggro_updates=*/true,
                           /*suppress_retarget_logic=*/true,
                           /*force_armor_only=*/false,
                           /*bypass_shields=*/false,
                           /*player_aggro_delta=*/10);

  CHECK(target.player_aggro_accumulator == Catch::Approx(0.0F));
  CHECK(target.primary_target_ship_slot == 0);
  CHECK(target.ai_state_code == 4);
}

TEST_CASE("player attack alerts same-government NPCs", "[collision][ai]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.governments.resize(1);
  state.ShipAt(1).faction_or_government_id = 0;
  state.ShipAt(1).ai_behavior_code = 3;
  state.player.primary_target_ship_slot = 1;

  Ship &wingmate = state.ShipAt(2);
  wingmate.is_active = true;
  wingmate.ship_instance_id = 2;
  wingmate.ship_class_id = 0;
  wingmate.current_system_id = 0;
  wingmate.faction_or_government_id = 0;
  wingmate.ai_behavior_code = 3;
  wingmate.ai_state_code = 0;
  wingmate.pers_def_slot = 0;
  wingmate.pos_x = 100.0F;
  wingmate.pos_y = 100.0F;

  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, 1, 0) == 0);
  NovaWeapon_ResolveDirectShotCollisions(state);

  CHECK(wingmate.ai_state_code == 4);
  CHECK(wingmate.primary_target_ship_slot == 0);
}

TEST_CASE("player can continue firing after target becomes hostile",
          "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  SpawnTestShot(state);

  NovaWeapon_ResolveDirectShotCollisions(state);
  REQUIRE(state.active_shots.empty());

  SpawnTestShot(state);
  REQUIRE(NovaWeapon_CanProjectileHitShip(state, state.active_shots[0], 1));
  NovaWeapon_ResolveDirectShotCollisions(state);

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
  NovaWeapon_ResolveDirectShotCollisions(state);
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

  NovaWeapon_TickShots(state, 0.5F);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].pos_x == Catch::Approx(1.0F));
  CHECK(state.active_shots[0].life_ticks_remaining == Catch::Approx(1.0F));
  CHECK(state.active_shots[0].life_frames == 1);

  NovaWeapon_TickShots(state, 0.75F);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].pos_x == Catch::Approx(2.5F));
  CHECK(state.active_shots[0].life_ticks_remaining == Catch::Approx(0.25F));

  NovaWeapon_TickShots(state, 0.25F);
  CHECK(state.active_shots.empty());
}

TEST_CASE("animated projectile frame delay is measured in 30 Hz ticks",
          "[collision][weapon]") {
  GameState state;
  SeedCollisionScenario(state);
  Weapon &weapon = state.scenario.weapons[0];
  weapon.flags |= 0x0001U;
  weapon.beam_width_or_animation_frame_delay = 2;

  ActiveShot shot;
  shot.weapon_id = 0;
  shot.owner_ship_slot = 0;
  shot.system_id = 0;
  shot.life_frames = 10;
  shot.life_ticks_remaining = 10.0F;
  state.active_shots.push_back(shot);

  // Four 60 Hz updates contribute four half-ticks: two normalized ticks, or
  // 2/30 second, independent of the update rate.
  for (int update = 0; update < 3; ++update) {
    NovaWeapon_TickShots(state, 0.5F);
    REQUIRE(state.active_shots.size() == 1);
    CHECK(state.active_shots[0].frame_cycle_index == 0);
  }
  NovaWeapon_TickShots(state, 0.5F);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].frame_cycle_index == 1);
  CHECK(state.active_shots[0].anim_elapsed == Catch::Approx(0.0F));
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

  NovaWeapon_ResolveDirectShotCollisions(state);

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

TEST_CASE("ownerless shots hit only their recorded target slot",
          "[collision][stellar-defense]") {
  GameState state;
  SeedCollisionScenario(state);

  // A stellar defense battery shot: owner_ship_slot -1, weapon bank 0. The
  // original Weapon_CanWeaponHitTarget (0x00426ef0) routes ownerless shots
  // through the 0x00427435 branch, which accepts only the recorded target slot
  // (or a ship whose defense_fleet_home_stellar_id matches it) before joining
  // the common capability/aggro tail.
  ActiveShot shot;
  shot.weapon_id = 0;
  shot.owner_ship_slot = -1;
  shot.target_ship_slot = 1;
  shot.system_id = 0;
  shot.life_ticks_remaining = 10.0F;

  CHECK(NovaWeapon_CanProjectileHitShip(state, shot, 1));

  Ship &second = state.ShipAt(2);
  second.is_active = true;
  second.ship_instance_id = 2;
  second.ship_class_id = 0;
  second.current_system_id = 0;
  second.armor_points = 100.0F;
  second.shield_points = 20.0F;
  second.ai_behavior_code = 5;
  CHECK(!NovaWeapon_CanProjectileHitShip(state, shot, 2));

  // Ghidra compares the recorded slot against both the candidate's
  // ship_instance_id and its defense_fleet_home_stellar_id.
  second.defense_fleet_home_stellar_id = 1;
  CHECK(NovaWeapon_CanProjectileHitShip(state, shot, 2));

  // An invalid recorded slot leaves the ownerless shot free to hit any ship.
  shot.target_ship_slot = -1;
  CHECK(NovaWeapon_CanProjectileHitShip(state, shot, 2));

  // Drive the direct pass: the ownerless shot connects with the recorded
  // target and consumes itself.
  state.active_shots.push_back(shot);
  state.active_shots[0].target_ship_slot = 1;
  state.active_shots[0].pos_x = 0.0F;
  state.active_shots[0].pos_y = 0.0F;
  NovaWeapon_ResolveDirectShotCollisions(state);
  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(10.0F));
}

TEST_CASE("player fire preserves state-9 hit-reset ordering",
          "[collision][ai]") {
  GameState state;
  SeedCollisionScenario(state);
  SpawnTestShot(state);
  Ship &target = state.ShipAt(1);
  target.ai_state_code = 9;
  target.ai_control_mode = 0x0B;
  target.primary_target_ship_slot = 2;
  target.ai_secondary_target_slot = 3;
  target.ai_maneuver_timer_ms = 99.0F;
  target.defense_fleet_home_stellar_id = -1;
  target.player_aggro_accumulator = 50.0F;

  NovaWeapon_ResolveDirectShotCollisions(state);

  // Ghidra calls Ship_SetShipHostileToPlayer before 0x004115c0. The setter
  // turns state 9 into 4, so the helper's state-9/0xF predicate is false.
  CHECK(target.ai_state_code == 4);
  CHECK(target.ai_control_mode == 0x0B);
  CHECK(target.primary_target_ship_slot == 0);
  CHECK(target.ai_secondary_target_slot == -1);
  CHECK(target.ai_maneuver_timer_ms == Catch::Approx(20.0F));
}

TEST_CASE("scripted manoeuvre targets ignore queued beam impacts",
          "[collision][weapon]") {
  GameState state;
  SeedCollisionScenario(state);
  Ship &target = state.ShipAt(1);
  target.ai_state_code = 0x10;

  NovaWeapon_ResolveDirectWeaponHit(state, 0, 1, 0);

  CHECK(target.shield_points == Catch::Approx(20.0F));
  CHECK(target.armor_points == Catch::Approx(100.0F));
}

TEST_CASE("targeted player beam alerts same-government warships",
          "[collision][beam][ai]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.governments.resize(1);
  state.ShipAt(1).faction_or_government_id = 0;

  Ship &responder = state.ShipAt(2);
  responder.is_active = true;
  responder.ship_instance_id = 2;
  responder.ship_class_id = 0;
  responder.current_system_id = 0;
  responder.faction_or_government_id = 0;
  responder.ai_behavior_code = 3;
  responder.ai_state_code = 0;
  responder.pers_def_slot = 0;

  NovaWeapon_ResolveDirectWeaponHit(state, 0, 1, 0);

  CHECK(responder.ai_state_code == 4);
  CHECK(responder.primary_target_ship_slot == 0);
}

TEST_CASE("lethal projectile leaves destruction to armor state and is consumed",
          "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  state.ShipAt(1).shield_points = 0.0F;
  state.ShipAt(1).armor_points = 10.0F;
  SpawnTestShot(state);

  NovaWeapon_ResolveDirectShotCollisions(state);

  CHECK(state.active_shots.empty());
  // Shot_ResolveShipHitFromWeapon does not arm the death timer itself; the
  // original ship handler consumes armor <= 0 on its later pass.
  CHECK(state.ShipAt(1).armor_points == Catch::Approx(-15.0F));
  CHECK(state.ShipAt(1).death_timer_active == Catch::Approx(-1.0F));
  CHECK(state.ShipAt(1).destruction_visual_triggered);
  // The original hit path only records the armor transition: the debris puff
  // belongs to Ship_HandleShip and the Explode1/Explode2 impacts belong to
  // Ship_UpdateVisualState, so no effect is queued at the hit site.
  CHECK(state.impact_effect_instances[0].effect_id == -1);
  CHECK(!NovaWeapon_CanProjectileHitShip(state, ActiveShot{}, 1));
}

TEST_CASE("proximity safety suppresses ship contacts just after launch",
          "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.weapons[0].proximity_safety_ticks = 3;
  SpawnTestShot(state);

  // At launch, remaining life is above Count - ProxSafety, so direct ship
  // contact is rejected. Equality at the three-tick boundary is accepted.
  NovaWeapon_ResolveDirectShotCollisions(state);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(20.0F));

  state.active_shots[0].life_ticks_remaining = 7.0F;
  NovaWeapon_ResolveDirectShotCollisions(state);
  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(10.0F));
}

TEST_CASE("projectile damage decay uses normalized 30 Hz ticks",
          "[collision][weapon]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.weapons[0].damage_decay_interval_ticks = 2;
  SpawnTestShot(state);

  // The original comparison is strict: exactly two accumulated ticks do not
  // decay yet. A fifth 60 Hz update crosses the interval and removes one point
  // from both direct-hit damage values.
  for (int update = 0; update < 4; ++update) {
    NovaWeapon_TickShots(state, 0.5F);
  }
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].damage_decay_points == 0);
  CHECK(state.active_shots[0].damage_decay_elapsed_ticks ==
        Catch::Approx(2.0F));

  NovaWeapon_TickShots(state, 0.5F);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].damage_decay_points == 1);
  CHECK(state.active_shots[0].damage_decay_elapsed_ticks ==
        Catch::Approx(0.0F));

  NovaWeapon_ResolveDirectShotCollisions(state);
  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(11.0F));
}

TEST_CASE("blast weapon strips asteroid integrity, splashes owner, and breaks",
          "[collision][asteroid]") {
  GameState state;
  SeedCollisionScenario(state);
  Weapon &weapon = state.scenario.weapons[0];
  weapon.blast_radius = 50;
  weapon.splash_radius = 30;
  weapon.impact_impulse = 200;

  // One asteroid type: integrity 5, splits into type-0 children (count base
  // 2 -> 1..2 children per break).
  state.scenario.asteroid_defs.resize(1);
  state.scenario.asteroid_defs[0].strength = 5;
  state.scenario.asteroid_defs[0].frag_type1 = 0;
  state.scenario.asteroid_defs[0].frag_type2 = -1;
  state.scenario.asteroid_defs[0].frag_count = 2;

  // Remove the test target ship so the proximity ship pass cannot claim the
  // blast before the asteroid scan runs.
  state.ShipAt(1).is_active = false;

  AsteroidState &asteroid = state.asteroid_pool[0];
  asteroid.active = true;
  asteroid.wander_type = 0;
  asteroid.integrity = 5; // normally seeded by Asteroid_SpawnRecord
  asteroid.target_pos_x = 0.0F;
  asteroid.target_pos_y = 0.0F;

  // The player's own blast splashes the player (flags_primary 0x100 clear):
  // shields 100 -> 90. The proximity ship pass cannot hit the owner, so the
  // shot survives for the asteroid scan.
  SpawnTestShot(state);
  NovaWeapon_ResolveProjectileCollisions(state);
  REQUIRE(state.active_shots.empty());
  CHECK(state.player.shield_points == Catch::Approx(90.0F));
  CHECK(asteroid.integrity == -5); // 5 - energy_damage(10)
  CHECK(!asteroid.active);
  // The break spawns 1..2 type-0 children in other pool slots.
  int children = 0;
  for (std::size_t i = 1; i < state.asteroid_pool.size(); ++i) {
    if (state.asteroid_pool[i].active) {
      CHECK(state.asteroid_pool[i].wander_type == 0);
      ++children;
    }
  }
  CHECK(children >= 1);
}

TEST_CASE("direct projectile strips asteroid integrity without a blast",
          "[collision][asteroid]") {
  GameState state;
  SeedCollisionScenario(state);

  // One asteroid type with integrity 100 so a 10-point hit does not break it.
  state.scenario.asteroid_defs.resize(1);
  state.scenario.asteroid_defs[0].strength = 100;
  state.ShipAt(1).is_active = false; // keep the ship pass out of the way

  AsteroidState &asteroid = state.asteroid_pool[0];
  asteroid.active = true;
  asteroid.wander_type = 0;
  asteroid.integrity = 100;
  asteroid.target_pos_x = 0.0F;
  asteroid.target_pos_y = 0.0F;

  SpawnTestShot(state);
  REQUIRE(state.scenario.weapons[0].blast_radius == 0);
  NovaWeapon_ResolveDirectShotCollisions(state);

  CHECK(state.active_shots.empty());
  CHECK(asteroid.integrity == 90); // 100 - energy_damage(10)
  CHECK(asteroid.active);
  // No splash_radius on the weapon, so the player is untouched.
  CHECK(state.player.shield_points == Catch::Approx(100.0F));
}

TEST_CASE("a proximity-safe direct shot still strikes an asteroid",
          "[collision][asteroid]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.asteroid_defs.resize(1);
  state.scenario.weapons[0].proximity_safety_ticks = 3;
  state.ShipAt(1).is_active = false;

  AsteroidState &asteroid = state.asteroid_pool[0];
  asteroid.active = true;
  asteroid.wander_type = 0;
  asteroid.integrity = 100;
  asteroid.target_pos_x = 0.0F;
  asteroid.target_pos_y = 0.0F;

  SpawnTestShot(state);
  // Inside the initial safety delay the ship callback rejects contact, but the
  // asteroid callback (0x00436f70) has no ProxSafety gate.
  NovaWeapon_ResolveDirectShotCollisions(state);

  CHECK(state.active_shots.empty());
  CHECK(asteroid.integrity == 90);
}

TEST_CASE("passes-over-asteroids weapons skip direct asteroid contact",
          "[collision][asteroid]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.asteroid_defs.resize(1);
  state.scenario.weapons[0].flags_quaternary = 0x0001U; // Seeker "passes over"
  state.ShipAt(1).is_active = false;

  AsteroidState &asteroid = state.asteroid_pool[0];
  asteroid.active = true;
  asteroid.wander_type = 0;
  asteroid.integrity = 100;
  asteroid.target_pos_x = 0.0F;
  asteroid.target_pos_y = 0.0F;

  SpawnTestShot(state);
  NovaWeapon_ResolveDirectShotCollisions(state);

  REQUIRE(state.active_shots.size() == 1);
  CHECK(asteroid.integrity == 100);
}

TEST_CASE("NPC destruction seeds the class DeathDelay timer once",
          "[collision][ship_visual]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.ships[0].death_delay_frames = 10;
  state.ShipAt(1).armor_points = 1.0F;
  state.ShipAt(1).shield_points = 0.0F;

  // Shot_ResolveShipHitFromWeapon leaves destruction as an armor state; the
  // NPC timer (x1) is seeded by Ship_UpdateVisualState, not the hit site.
  ResolveShipHitFromWeapon(state,
                           /*target_slot=*/1,
                           state.ShipAt(1),
                           state.ShipAt(1).pos_x,
                           state.ShipAt(1).pos_y,
                           /*impact_impulse=*/0,
                           /*armor_damage=*/25,
                           /*shield_damage=*/0,
                           /*attacker_ship_slot=*/0,
                           /*allow_aggro_updates=*/true,
                           /*suppress_retarget_logic=*/false,
                           /*force_armor_only=*/false,
                           /*bypass_shields=*/false,
                           /*player_aggro_delta=*/0,
                           /*check_fire_restriction_transition=*/true);
  CHECK(NovaAiShip_IsDestroyed(state.ShipAt(1)));
  CHECK(state.ShipAt(1).death_timer_active <= 0.0F);
  NovaShip_TickDestroyedShipVisualState(state, state.ShipAt(1), 0.63F);
  CHECK(state.ShipAt(1).death_timer_active == Catch::Approx(10.0F));
}

TEST_CASE("player destruction seeds a tripled death presentation timer",
          "[collision][ship_visual]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.ships[0].death_delay_frames = 10;
  state.player.armor_points = 1.0F;
  state.player.shield_points = 0.0F;

  ResolveShipHitFromWeapon(state,
                           /*target_slot=*/0,
                           state.player,
                           state.player.pos_x,
                           state.player.pos_y,
                           /*impact_impulse=*/0,
                           /*armor_damage=*/25,
                           /*shield_damage=*/0,
                           /*attacker_ship_slot=*/1,
                           /*allow_aggro_updates=*/true,
                           /*suppress_retarget_logic=*/false,
                           /*force_armor_only=*/false,
                           /*bypass_shields=*/false,
                           /*player_aggro_delta=*/0,
                           /*check_fire_restriction_transition=*/true);
  REQUIRE(NovaAiShip_IsDestroyed(state.player));
  // The collision path leaves the player's presentation timer alone so
  // Ship_UpdateVisualState (called from Frame_TickSystems scope 10) owns the
  // 3x scale.
  CHECK(state.player.death_timer_active <= 0.0F);

  NovaShip_TickDestroyedShipVisualState(
      state, state.player, /*elapsed_ticks=*/0.63F);
  // g_player_death_timer_scale (3.0) * DeathDelay (10).
  CHECK(state.player.death_timer_active == Catch::Approx(30.0F));
}

TEST_CASE("armor-only destruction starts the player death sequence",
          "[collision][ship_visual]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.ships[0].death_delay_frames = 8;
  // Self-destruct (and script kills) drop armor without a weapon hit, so no
  // collision path seeds the presentation.
  state.player.armor_points = -1.0F;
  state.player.vel_x = 10.0F;
  REQUIRE(state.player.death_timer_active <= 0.0F);

  // The player core consumes the destroyed frame but does not seed the timer.
  // It applies the fire-restricted 0.995 damp on the raw-call cadence.
  CHECK(PlayerTick_StatusAndOutfitEvents(state,
                                         /*elapsed_ticks=*/0.63F,
                                         /*eject_command=*/false));
  CHECK(state.player.vel_x == Catch::Approx(10.0F * 0.995F));
  CHECK(state.player.death_timer_active <= 0.0F);
  // Ship_UpdateVisualState (scope 10, right after the core) seeds it.
  NovaShip_TickDestroyedShipVisualState(
      state, state.player, /*elapsed_ticks=*/0.63F);
  CHECK(state.player.death_timer_active == Catch::Approx(24.0F));
  CHECK(state.player.death_timer_seeded);
}

TEST_CASE("player wreck sheds Explode1 puffs then runs the Explode2 finale",
          "[collision][ship_visual]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.ships[0].death_delay_frames = 30;
  state.scenario.ships[0].destruction_effect_while_breaking = 1;
  state.scenario.ships[0].destruction_effect_final = 2;
  state.player.armor_points = -1.0F;

  const auto active_effects = [](const GameState &s) {
    std::size_t count = 0;
    for (const ImpactEffectInstance &effect : s.impact_effect_instances) {
      if (effect.anim_time >= 0.0F) {
        ++count;
      }
    }
    return count;
  };

  // Seed the 3x presentation (90 ticks).
  NovaShip_TickDestroyedShipVisualState(state, state.player, 0.63F);
  REQUIRE(state.player.death_timer_active == Catch::Approx(90.0F));

  // Below 20 the Explode1 roll is 1-in-1, so one puff must spawn.
  state.player.death_timer_active = 19.0F;
  const std::size_t puffs_before = active_effects(state);
  NovaShip_TickDestroyedShipVisualState(state, state.player, 0.63F);
  CHECK(active_effects(state) == puffs_before + 1);
  CHECK(state.player.is_active);

  // Crossing the finale threshold spawns the Explode2 boom and deactivates.
  state.player.death_timer_active = 1.0F;
  const std::size_t finale_before = active_effects(state);
  NovaShip_TickDestroyedShipVisualState(state, state.player, 0.63F);
  CHECK(active_effects(state) == finale_before + 1);
  CHECK(!state.player.is_active);
}

TEST_CASE("death seed latch still runs the finale after a zero overshoot",
          "[collision][ship_visual]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.ships[0].death_delay_frames = 30;
  state.scenario.ships[0].destruction_effect_final = 2;
  state.player.armor_points = -1.0F;

  // Seed the 3x presentation once; the latch now owns the presentation.
  NovaShip_TickDestroyedShipVisualState(state, state.player, 0.63F);
  REQUIRE(state.player.death_timer_active == Catch::Approx(90.0F));
  REQUIRE(state.player.is_active);
  REQUIRE(state.player.death_timer_seeded);

  // A long time-adjusted raw-call step can move the timer from above the
  // finale window to below zero. The original's constant 1.0 step never
  // crosses zero, so it always hits 0<timer<=2.0; the latch must preserve that
  // by running the finale instead of re-seeding a fresh 90-tick presentation.
  state.player.death_timer_active = -0.5F;
  NovaShip_TickDestroyedShipVisualState(state, state.player, 0.63F);
  CHECK(!state.player.is_active);
  CHECK(state.player.destruction_finale_triggered);
}

TEST_CASE("destroyed hull blast scales radius and damage by class mass",
          "[collision][ship_visual]") {
  GameState state;
  SeedCollisionScenario(state);
  // The four hull-blast constants at 0x00575380..0x0057539C are doubles:
  // radius round(Mass*0.075 + 50) and damage round(Mass*0.0375 + 25). At 150
  // tons that is a per-axis radius of 61 and 31 blast damage. A misaligned
  // float read of the same bytes yields 1.4/3.125 and 1.275/2.875, which
  // ballooned the radius to 213 and damaged ships the original would spare.
  state.scenario.ships[0].mass_tons = 150;
  state.scenario.ships[0].destruction_effect_final = 2;
  state.player.is_active = false;

  Ship &wreck = state.ShipAt(1);
  wreck.pos_x = 0.0F;
  wreck.pos_y = 0.0F;
  wreck.armor_points = 0.0F;
  wreck.shield_points = 0.0F;

  Ship &near = state.ShipAt(2);
  near.is_active = true;
  near.ship_instance_id = 2;
  near.ship_class_id = 0;
  near.armor_points = 1000.0F;
  near.shield_points = 0.0F;
  near.pos_x = 60.0F;
  near.pos_y = 0.0F;

  Ship &far = state.ShipAt(3);
  far.is_active = true;
  far.ship_instance_id = 3;
  far.ship_class_id = 0;
  far.armor_points = 1000.0F;
  far.shield_points = 0.0F;
  far.pos_x = 100.0F;
  far.pos_y = 0.0F;

  NovaShip_RunShipDestructionFinale(state, wreck);

  CHECK(near.armor_points == Catch::Approx(970.0F));
  CHECK(far.armor_points == Catch::Approx(1000.0F));
}

TEST_CASE("inactive wreck holds the death screen until the -240 timer floor",
          "[collision][spaceflight]") {
  GameState state;
  SeedCollisionScenario(state);
  // State after the finale deactivated the hull with the presentation timer
  // still just above zero.
  state.player.is_active = false;
  state.player.death_timer_active = 2.0F;

  // The inactive prologue drains one addend per original 21 ms spaceflight
  // call (0x0044afb0) and keeps the frame consumed without latching game-over.
  CHECK(PlayerTick_StatusAndOutfitEvents(state, 0.63F, false));
  CHECK(state.player.death_timer_active == Catch::Approx(1.0F));
  CHECK_FALSE(state.game_over_pending);

  state.player.death_timer_active = -239.0F;
  CHECK(PlayerTick_StatusAndOutfitEvents(state, 0.63F, false));
  CHECK(state.player.death_timer_active == Catch::Approx(-240.0F));
  CHECK_FALSE(state.game_over_pending);

  // At the floor the branch falls through to the game-over latch.
  CHECK(PlayerTick_StatusAndOutfitEvents(state, 0.63F, false));
  CHECK(state.game_over_pending);
  CHECK(state.player.death_timer_active == Catch::Approx(-240.0F));
}

TEST_CASE("active player death timer follows the original raw-call cadence",
          "[collision][spaceflight]") {
  GameState state;
  SeedCollisionScenario(state);
  state.player.is_active = true;
  state.player.armor_points = -1.0F;
  state.player.death_timer_active = 5.0F;

  CHECK(PlayerTick_StatusAndOutfitEvents(state, 0.63F, false));
  NovaShip_TickDestroyedShipVisualState(state, state.player, 0.63F);
  CHECK(state.player.death_timer_active == Catch::Approx(4.0F));
}

TEST_CASE("destroyed visual RNG banks partial raw calls",
          "[collision][ship_visual][timing]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.ships[0].destruction_effect_while_breaking = 1;
  state.player.armor_points = -1.0F;
  state.player.death_timer_seeded = true;
  state.player.death_timer_active = 19.0F; // Explode1 is guaranteed below 20

  const auto active_effects = [](const GameState &s) {
    return std::count_if(s.impact_effect_instances.begin(),
                         s.impact_effect_instances.end(),
                         [](const ImpactEffectInstance &effect) {
                           return effect.anim_time >= 0.0F;
                         });
  };

  NovaShip_TickDestroyedShipVisualState(state, state.player, 0.315F);
  CHECK(state.player.death_timer_active == Catch::Approx(19.0F));
  CHECK(active_effects(state) == 0);

  NovaShip_TickDestroyedShipVisualState(state, state.player, 0.315F);
  CHECK(state.player.death_timer_active == Catch::Approx(18.0F));
  CHECK(active_effects(state) == 1);
}

TEST_CASE("proximity blast spawns linked submunitions with inherited context",
          "[collision][linked]") {
  GameState state;
  SeedLinkedShotScenario(state);
  SpawnTestShot(state);
  const float impact_x = state.active_shots[0].pos_x;
  const float impact_y = state.active_shots[0].pos_y;

  NovaWeapon_ResolveProjectileCollisions(state);

  REQUIRE(state.active_shots.size() == 2);
  for (const ActiveShot &child : state.active_shots) {
    CHECK(child.weapon_id == 1);
    CHECK(child.owner_ship_slot == 0);        // inherited owner
    CHECK(child.target_ship_slot == 1);       // proximity fallback target
    CHECK(child.linked_shot_generation == 1); // parent generation + 1
    CHECK(child.pos_x == Catch::Approx(impact_x));
    CHECK(child.pos_y == Catch::Approx(impact_y));
    CHECK(child.vel_x == Catch::Approx(0.0F));
    CHECK(child.vel_y == Catch::Approx(-2.0F)); // heading 0, 2 px/frame
  }
}

TEST_CASE("negative SubTheta fans linked shots deterministically",
          "[collision][linked]") {
  GameState state;
  SeedLinkedShotScenario(state);
  state.scenario.weapons[0].range_link_gate = 3;
  state.scenario.weapons[0].range_link_spread = -10;
  SpawnTestShot(state);

  NovaWeapon_ResolveProjectileCollisions(state);

  REQUIRE(state.active_shots.size() == 3);
  CHECK(state.active_shots[0].heading_deg == Catch::Approx(350.0F));
  CHECK(state.active_shots[1].heading_deg == Catch::Approx(0.0F));
  CHECK(state.active_shots[2].heading_deg == Catch::Approx(10.0F));
}

TEST_CASE("direct contact never spawns linked submunitions",
          "[collision][linked]") {
  GameState state;
  SeedLinkedShotScenario(state);
  SpawnTestShot(state);

  // The direct sprite-contact pass passes allow_linked=0, so even a weapon
  // with a valid linkage spawns nothing here.
  NovaWeapon_ResolveDirectShotCollisions(state);
  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(10.0F));
}

TEST_CASE("linked spawn requires a valid gate and linked weapon",
          "[collision][linked]") {
  GameState state;
  SeedLinkedShotScenario(state);
  ActiveShot parent;
  parent.weapon_id = 0;
  parent.owner_ship_slot = 0;

  state.scenario.weapons[0].range_link_weapon_id = -1;
  NovaWeapon_SpawnLinkedShotsOnImpact(state, parent, -1);
  CHECK(state.active_shots.empty());

  state.scenario.weapons[0].range_link_weapon_id = 1;
  state.scenario.weapons[0].range_link_gate = 0;
  NovaWeapon_SpawnLinkedShotsOnImpact(state, parent, -1);
  CHECK(state.active_shots.empty());
}

TEST_CASE("SubLimit stops recursive linked submunitions",
          "[collision][linked]") {
  GameState state;
  SeedLinkedShotScenario(state);
  state.scenario.weapons[0].range_link_extra_count = 1;

  ActiveShot parent;
  parent.weapon_id = 0;
  parent.owner_ship_slot = 0;
  parent.linked_shot_generation = 1; // already at the SubLimit
  NovaWeapon_SpawnLinkedShotsOnImpact(state, parent, -1);
  CHECK(state.active_shots.empty());

  parent.linked_shot_generation = 0;
  NovaWeapon_SpawnLinkedShotsOnImpact(state, parent, -1);
  CHECK(state.active_shots.size() == 2);
}

TEST_CASE("expiry launches linked shots unless Flags2 0x20 suppresses it",
          "[collision][linked]") {
  GameState state;
  SeedLinkedShotScenario(state);
  SpawnTestShot(state);
  state.active_shots[0].target_ship_slot = 1;
  state.active_shots[0].life_ticks_remaining = 1.0F;

  NovaWeapon_TickShots(state, 1.0F);

  REQUIRE(state.active_shots.size() == 2);
  for (const ActiveShot &child : state.active_shots) {
    CHECK(child.weapon_id == 1);
    CHECK(child.owner_ship_slot == 0);
    CHECK(child.target_ship_slot == 1);
    CHECK(child.linked_shot_generation == 1);
  }

  GameState suppressed_state;
  SeedLinkedShotScenario(suppressed_state);
  suppressed_state.scenario.weapons[0].flags_secondary = 0x0020U;
  SpawnTestShot(suppressed_state);
  suppressed_state.active_shots[0].life_ticks_remaining = 1.0F;

  NovaWeapon_TickShots(suppressed_state, 1.0F);

  CHECK(suppressed_state.active_shots.empty());
}

TEST_CASE("Flags2 0x0010 linked shots acquire the nearest hittable target",
          "[collision][linked]") {
  GameState state;
  SeedLinkedShotScenario(state);
  state.scenario.weapons[0].flags_secondary = 0x0010;
  ActivateHostileShip(state, 2, 5.0F, 0.0F); // nearer than ship 1 at (10,0)
  SpawnTestShot(state);

  NovaWeapon_ResolveProjectileCollisions(state);

  // The blast scan claims ship 1 first, but each child retargets to ship 2.
  REQUIRE(state.active_shots.size() == 2);
  for (const ActiveShot &child : state.active_shots) {
    CHECK(child.target_ship_slot == 2);
  }
}

} // namespace game
