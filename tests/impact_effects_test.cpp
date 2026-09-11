#include "game/game_state.hpp"
#include "game/impact_effects.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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

TEST_CASE("SWParticles advance at a fixed 60 Hz and expire",
          "[impact][particle]") {
  GameState state;
  SwParticle particle;
  particle.life_ticks = 3;
  particle.vel_x = 256;  // 1 px/update
  particle.vel_y = -256; // 1 px/update up
  state.sw_particles.push_back(particle);

  // A quarter of a 30 Hz tick is half a 60 Hz update: banked, no step.
  NovaEffects_TickSwParticles(state, 0.25F);
  REQUIRE(state.sw_particles.size() == 1);
  CHECK(state.sw_particles[0].pos_x == 0);

  // The second quarter completes the update.
  NovaEffects_TickSwParticles(state, 0.25F);
  REQUIRE(state.sw_particles.size() == 1);
  CHECK(state.sw_particles[0].life_ticks == 2);
  CHECK(state.sw_particles[0].pos_x == 256);
  CHECK(state.sw_particles[0].pos_y == -256);

  // One 30 Hz tick is two 60 Hz updates: life 2 -> 1 (moves), 1 -> 0 (freed).
  NovaEffects_TickSwParticles(state, 1.0F);
  CHECK(state.sw_particles.empty());
}

TEST_CASE("impact packages dispatch their configured area effect", "[impact]") {
  GameState state;
  // ScenarioData allocates the 0x80 asteroid rows during archive loading;
  // this unit test constructs the state without loading archives.
  state.scenario.asteroid_defs.resize(0x80);
  state.scenario.asteroid_defs[0].present = true;
  state.scenario.asteroid_defs[0].field_0x10 = 5;
  state.scenario.impact_effects[5].frame_rate_scale = 0.1F;

  NovaEffects_SpawnImpactEffectPackage(state, 40.0F, 50.0F, 0, false);

  REQUIRE(state.impact_effect_instances[0].anim_time == 0.0F);
  CHECK(state.impact_effect_instances[0].effect_id == 5);
  CHECK(state.pending_impact_sounds.empty());
}

} // namespace game
