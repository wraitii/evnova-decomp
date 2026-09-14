#include "game/game_state.hpp"
#include "game/impact_effects.hpp"
#include "game/weapon.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

namespace game {

TEST_CASE("impact effects advance from the fixed pool", "[impact]") {
  GameState state;
  state.scenario.impact_effects[3].frame_rate_scale = 0.1F;
  state.scenario.impact_effects[3].impact_sound_slot = 7;

  NovaEffects_SpawnImpactEffect(state, 120.0F, 240.0F, 3);

  REQUIRE(state.impact_effect_instances[0].anim_time == 0.0F);
  CHECK(state.impact_effect_instances[0].pos_x == 120.0F);
  CHECK(state.impact_effect_instances[0].pos_y == 240.0F);
  CHECK(state.impact_effect_instances[0].effect_id == 3);
  CHECK(state.pending_impact_sounds.empty());

  NovaEffects_TickImpactEffects(state, 10.0F);
  CHECK(state.impact_effect_instances[0].anim_time == 1.0F);
}

TEST_CASE("large impact effects scatter children and queue one main sound",
          "[impact]") {
  GameState state;
  state.scenario.impact_effects[2].frame_rate_scale = 0.1F;
  state.scenario.impact_effects[2].impact_sound_slot = 11;

  // 1002 produces four inner children, sixteen outer children, and one main
  // effect with the constants recovered from Shot_SpawnAreaImpactEffects.
  NovaEffects_SpawnAreaImpact(state, 500.0F, 600.0F, 1002, 100);

  int active = 0;
  int main_effects = 0;
  for (const ImpactEffectInstance &instance : state.impact_effect_instances) {
    if (instance.anim_time < 0.0F) {
      continue;
    }
    ++active;
    if (instance.effect_id == 2) {
      ++main_effects;
    }
  }
  CHECK(active == 21);
  CHECK(main_effects == 1);
  REQUIRE(state.pending_impact_sounds.size() == 1);
  CHECK(state.pending_impact_sounds[0].slot == 11);
  CHECK(state.pending_impact_sounds[0].src_x == 500.0F);
  CHECK(state.pending_impact_sounds[0].src_y == 600.0F);
}

TEST_CASE("weapon impact bursts emit SWParticles from the weapon fields",
          "[impact][particle]") {
  GameState state;
  state.rng.seed(0x5eedU);
  Weapon weapon;
  weapon.impact_particle_count = 5;
  weapon.impact_particle_frame_base = 8;
  weapon.impact_particle_speed = 2.0F;
  weapon.impact_particle_color = 0x00ff8000U;

  NovaEffects_SpawnWeaponImpactBurstForWeapon(
      state, 100.0F, 50.0F, weapon, /*scatter=*/0x14);

  REQUIRE(state.sw_particles.size() == 5);
  bool any_moving = false;
  for (const SwParticle &particle : state.sw_particles) {
    CHECK(particle.color == 0x00ff8000U);
    CHECK(particle.blend_mode == 0x20);
    // Life is uniform in [frame_base, round(frame_base * 1.25)] = [8, 10].
    CHECK(particle.life_ticks >= 8);
    CHECK(particle.life_ticks <= 10);
    // With no position scatter the spawn point is exactly the impact point.
    CHECK(particle.pos_x == 100 * 256);
    CHECK(particle.pos_y == 50 * 256);
    any_moving = any_moving || particle.vel_x != 0 || particle.vel_y != 0;
  }
  CHECK(any_moving);
}

TEST_CASE("weapon impact bursts spread the spawn point when asked",
          "[impact][particle]") {
  GameState state;
  state.rng.seed(0x5eedU);
  NovaEffects_SpawnWeaponImpactParticleBurst(state,
                                             100.0F,
                                             50.0F,
                                             /*speed=*/0.0F,
                                             /*scatter=*/0,
                                             /*life_base=*/4,
                                             /*life_max=*/4,
                                             /*color=*/0x00ffffffU,
                                             /*blend_mode=*/0x20,
                                             /*count=*/8,
                                             /*position_scatter=*/10);
  REQUIRE(state.sw_particles.size() == 8);
  bool any_offset = false;
  for (const SwParticle &particle : state.sw_particles) {
    const int dx = std::abs(particle.pos_x - 100 * 256);
    const int dy = std::abs(particle.pos_y - 50 * 256);
    // The offset vector magnitude is at most 10 px.
    CHECK(dx <= 10 * 256);
    CHECK(dy <= 10 * 256);
    any_offset = any_offset || dx != 0 || dy != 0;
  }
  CHECK(any_offset);
}

TEST_CASE("weapon trail particles use distinct precomputed variants",
          "[impact][particle][weapon-trail]") {
  GameState state;
  state.rng.seed(0x5eedU);
  Weapon weapon;
  weapon.trail_particle_count = 3;
  weapon.trail_particle_life_min = 4;
  weapon.trail_particle_life_max = 4;
  weapon.trail_particle_speed_variants = {
      1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F};
  weapon.trail_particle_color_variants = {0x00100000U,
                                          0x00200000U,
                                          0x00300000U,
                                          0x00400000U,
                                          0x00500000U,
                                          0x00600000U,
                                          0x00700000U,
                                          0x00800000U};

  // Heading 0 is up; the 180-degree rear anchor therefore moves the spawn
  // point down by the supplied four-pixel half-height.
  NovaEffects_SpawnWeaponTrailParticles(
      state, 100.0F, 50.0F, weapon, 0.0F, 4.0F);

  REQUIRE(state.sw_particles.size() == 3);
  for (const SwParticle &particle : state.sw_particles) {
    CHECK(particle.pos_x == 100 * 256);
    CHECK(particle.pos_y == 54 * 256);
    CHECK(particle.life_ticks == 4);
    CHECK(particle.blend_mode == 0x20);
    CHECK((particle.color & 0x00ff0000U) >= 0x00100000U);
    CHECK((particle.color & 0x00ff0000U) <= 0x00800000U);
    const float speed =
        std::sqrt(static_cast<float>(particle.vel_x * particle.vel_x +
                                     particle.vel_y * particle.vel_y)) /
        256.0F;
    CHECK(speed >= 0.9F);
    CHECK(speed <= 8.1F);
  }
}

TEST_CASE("shot tick emits configured trails at original raw-call cadence",
          "[impact][particle][weapon-trail]") {
  GameState state;
  state.scenario.weapons.resize(1);
  Weapon &weapon = state.scenario.weapons[0];
  weapon.trail_particle_count = 1;
  weapon.trail_particle_life_min = 10;
  weapon.trail_particle_life_max = 10;
  weapon.trail_particle_speed_variants.fill(1.0F);
  weapon.trail_particle_color_variants.fill(0x00abcdefU);
  ActiveShot shot;
  shot.weapon_id = 0;
  shot.system_id = state.player.current_system_id;
  shot.life_ticks_remaining = 10.0F;
  shot.heading_deg = 0.0F;
  state.active_shots.push_back(shot);

  NovaWeapon_TickShots(state, 0.3F);
  CHECK(state.sw_particles.empty());
  NovaWeapon_TickShots(state, 0.33F);
  REQUIRE(state.sw_particles.size() == 1);
  CHECK(state.sw_particles.front().color == 0x00abcdefU);
  CHECK(state.sw_particles.front().pos_y == 16 * 256);
}

TEST_CASE("SWParticles advance at the original 21 ms flight cadence and expire",
          "[impact][particle]") {
  GameState state;
  SwParticle particle;
  particle.life_ticks = 3;
  particle.vel_x = 256;  // 1 px/update
  particle.vel_y = -256; // 1 px/update up
  state.sw_particles.push_back(particle);

  // Half of an original 0.63-normalized-tick flight call is banked.
  NovaEffects_TickSwParticles(state, 0.315F);
  REQUIRE(state.sw_particles.size() == 1);
  CHECK(state.sw_particles[0].pos_x == 0);

  // The second half completes one discrete update.
  NovaEffects_TickSwParticles(state, 0.315F);
  REQUIRE(state.sw_particles.size() == 1);
  CHECK(state.sw_particles[0].life_ticks == 2);
  CHECK(state.sw_particles[0].pos_x == 256);
  CHECK(state.sw_particles[0].pos_y == -256);

  // Two further 21 ms calls: life 2 -> 1 (moves), 1 -> 0 (freed).
  NovaEffects_TickSwParticles(state, 1.26F);
  CHECK(state.sw_particles.empty());
}

TEST_CASE("ship destruction debris puffs seed one directional fragment each",
          "[impact][ship_visual]") {
  GameState state;
  state.rng.seed(0x5eedU);
  Ship ship;
  ship.pos_x = 10.0F;
  ship.pos_y = 20.0F;
  ship.vel_x = 3.0F;
  ship.vel_y = -4.0F;

  NovaEffects_SpawnShipDestructionDebrisPuff(state, ship);

  REQUIRE(state.fading_effect_instances[0].lifetime_ticks >= 150.0F);
  CHECK(state.fading_effect_instances[0].lifetime_ticks <= 249.0F);
  CHECK(state.fading_effect_instances[0].pos_x == 10.0F);
  CHECK(state.fading_effect_instances[0].pos_y == 20.0F);
  // Velocity = ship velocity + a scatter of magnitude (10 + rand(10)) * 0.1
  // = 1.0..1.9 (DAT_00575208).
  const float scatter_x = state.fading_effect_instances[0].vel_x - ship.vel_x;
  const float scatter_y = state.fading_effect_instances[0].vel_y - ship.vel_y;
  const float scatter =
      std::sqrt(scatter_x * scatter_x + scatter_y * scatter_y);
  CHECK(scatter >= 1.0F);
  CHECK(scatter <= 1.9F);
  REQUIRE(state.pending_destruction_sounds.size() == 1);
  CHECK(state.pending_destruction_sounds[0].src_x == 10.0F);
  CHECK(state.pending_destruction_sounds[0].src_y == 20.0F);

  // A second puff claims the next free slot rather than overwriting the first.
  NovaEffects_SpawnShipDestructionDebrisPuff(state, ship);
  CHECK(state.fading_effect_instances[0].lifetime_ticks >= 150.0F);
  CHECK(state.fading_effect_instances[1].lifetime_ticks >= 150.0F);
  CHECK(state.pending_destruction_sounds.size() == 2);
}

} // namespace game
