#include "game/game_state.hpp"
#include "game/impact_effects.hpp"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("impact packages dispatch their configured area effect", "[impact]") {
  GameState state;
  state.scenario.asteroid_defs[0].present = true;
  state.scenario.asteroid_defs[0].field_0x10 = 5;
  state.scenario.impact_effects[5].frame_rate_scale = 0.1F;

  NovaEffects_SpawnImpactEffectPackage(state, 40.0F, 50.0F, 0, false);

  REQUIRE(state.impact_effect_instances[0].anim_time == 0.0F);
  CHECK(state.impact_effect_instances[0].effect_id == 5);
  CHECK(state.pending_impact_sounds.empty());
}

} // namespace game
