#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/collision.hpp"
#include "game/game_state.hpp"
#include "game/ship_ai.hpp"
#include "game/ship_visual.hpp"
#include "game/spaceflight.hpp"
#include "game/weapon.hpp"

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
  NovaWeapon_ResolveDirectShotCollisions(state);

  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(10.0F));
  CHECK(state.ShipAt(1).armor_points == Catch::Approx(100.0F));
  CHECK(state.ShipAt(1).primary_target_ship_slot == 0);
  // A player attack sets the hostile primary target, but does not overwrite
  // the separate escort/leader-chain link.
  CHECK(state.ShipAt(1).squad_leader_ship_slot == -1);
  CHECK(state.ShipAt(1).ai_hostility_accumulator == 35);
  CHECK(state.ShipAt(1).hit_reaction_timer == Catch::Approx(32.0F));
  CHECK(state.ShipAt(1).ai_state_code == 4);
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
  REQUIRE(state.impact_effect_instances[0].effect_id == 0);
  CHECK(state.impact_effect_instances[0].anim_time == Catch::Approx(0.0F));
  CHECK(!NovaWeapon_CanProjectileHitShip(state, ActiveShot{}, 1));
}

TEST_CASE("late collision window stops contacts near expiry", "[collision]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.weapons[0].late_collision_window_ticks = 3;
  SpawnTestShot(state);

  state.active_shots[0].life_ticks_remaining = 2.0F;
  NovaWeapon_ResolveDirectShotCollisions(state);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(20.0F));

  state.active_shots[0].life_ticks_remaining = 4.0F;
  NovaWeapon_ResolveDirectShotCollisions(state);
  CHECK(state.active_shots.empty());
  CHECK(state.ShipAt(1).shield_points == Catch::Approx(10.0F));
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
  state.scenario.asteroid_defs[0].wander_table_value = 5;
  state.scenario.asteroid_defs[0].directions = {0, -1, 2};

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
  state.scenario.asteroid_defs[0].wander_table_value = 100;
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

TEST_CASE("a fusing direct shot still strikes an asteroid",
          "[collision][asteroid]") {
  GameState state;
  SeedCollisionScenario(state);
  state.scenario.asteroid_defs.resize(1);
  state.scenario.weapons[0].late_collision_window_ticks = 3;
  state.ShipAt(1).is_active = false;

  AsteroidState &asteroid = state.asteroid_pool[0];
  asteroid.active = true;
  asteroid.wander_type = 0;
  asteroid.integrity = 100;
  asteroid.target_pos_x = 0.0F;
  asteroid.target_pos_y = 0.0F;

  SpawnTestShot(state);
  // Inside the late window: the ship callback would reject this contact, but
  // the asteroid callback (0x00436f70) has no late-window gate.
  state.active_shots[0].life_ticks_remaining = 2.0F;
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

  // Shot_ResolveShipHitFromWeapon seeds NPC timers (the x1 case); the player's
  // timer is seeded by Ship_UpdateVisualState instead.
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
      state, state.player, /*elapsed_ticks=*/1.0F);
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
  // It does apply the fire-restricted 0.995 per-frame velocity damp.
  CHECK(NovaPlayer_TickStatusAndOutfitEvents(state,
                                             /*elapsed_ticks=*/1.0F,
                                             /*eject_command=*/false));
  CHECK(state.player.vel_x == Catch::Approx(9.95F));
  CHECK(state.player.death_timer_active <= 0.0F);
  // Ship_UpdateVisualState (scope 10, right after the core) seeds it.
  NovaShip_TickDestroyedShipVisualState(
      state, state.player, /*elapsed_ticks=*/1.0F);
  CHECK(state.player.death_timer_active == Catch::Approx(24.0F));
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
  NovaShip_TickDestroyedShipVisualState(state, state.player, 1.0F);
  REQUIRE(state.player.death_timer_active == Catch::Approx(90.0F));

  // Below 20 the Explode1 roll is 1-in-1, so one puff must spawn.
  state.player.death_timer_active = 19.0F;
  const std::size_t puffs_before = active_effects(state);
  NovaShip_TickDestroyedShipVisualState(state, state.player, 1.0F);
  CHECK(active_effects(state) == puffs_before + 1);
  CHECK(state.player.is_active);

  // Crossing the finale threshold spawns the Explode2 boom and deactivates.
  state.player.death_timer_active = 1.0F;
  const std::size_t finale_before = active_effects(state);
  NovaShip_TickDestroyedShipVisualState(state, state.player, 1.0F);
  CHECK(active_effects(state) == finale_before + 1);
  CHECK(!state.player.is_active);
}

TEST_CASE("inactive wreck holds the death screen until the -240 timer floor",
          "[collision][spaceflight]") {
  GameState state;
  SeedCollisionScenario(state);
  // State after the finale deactivated the hull with the presentation timer
  // still just above zero.
  state.player.is_active = false;
  state.player.death_timer_active = 2.0F;

  // The inactive prologue drains one addend per frame (0x0044af14) and keeps
  // the frame consumed without latching game-over.
  CHECK(NovaPlayer_TickStatusAndOutfitEvents(state, 1.0F, false));
  CHECK(state.player.death_timer_active == Catch::Approx(1.0F));
  CHECK_FALSE(state.game_over_pending);

  state.player.death_timer_active = -239.0F;
  CHECK(NovaPlayer_TickStatusAndOutfitEvents(state, 1.0F, false));
  CHECK(state.player.death_timer_active == Catch::Approx(-240.0F));
  CHECK_FALSE(state.game_over_pending);

  // At the floor the branch falls through to the game-over latch.
  CHECK(NovaPlayer_TickStatusAndOutfitEvents(state, 1.0F, false));
  CHECK(state.game_over_pending);
  CHECK(state.player.death_timer_active == Catch::Approx(-240.0F));
}

} // namespace game
