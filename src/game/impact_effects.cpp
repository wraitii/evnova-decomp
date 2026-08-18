#include "impact_effects.hpp"

#include "scenario_data.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace game {
namespace {

constexpr std::int16_t kFirstImpactEffect = 0;
constexpr std::int16_t kLastImpactEffect = 63;
constexpr std::int16_t kLargeEffectBase = 1000;

std::int16_t RandomRange(GameState &state, int bound) {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::int16_t>(
      std::uniform_int_distribution<int>{0, bound - 1}(state.rng));
}

bool ValidEffectId(std::int16_t effect_id) {
  return effect_id >= kFirstImpactEffect && effect_id <= kLastImpactEffect;
}

} // namespace

void NovaEffects_SpawnImpactEffect(GameState &state,
                                   float x,
                                   float y,
                                   std::int16_t effect_id,
                                   std::int16_t variant) {
  if (!ValidEffectId(effect_id)) {
    return;
  }
  for (ImpactEffectInstance &instance : state.impact_effect_instances) {
    if (instance.anim_time >= 0.0F) {
      continue;
    }
    instance.pos_x = x;
    instance.pos_y = y;
    instance.effect_id = effect_id;
    instance.anim_time = 0.0F;
    instance.delay_timer =
        static_cast<float>(std::max<std::int16_t>(0, variant));

    return;
  }
}

void NovaEffects_SpawnAreaImpact(GameState &state,
                                 float x,
                                 float y,
                                 std::int16_t effect_id,
                                 std::int16_t radius,
                                 bool play_sound) {
  if (effect_id < 0 ||
      (effect_id > kLastImpactEffect && effect_id < kLargeEffectBase) ||
      effect_id > 1063) {
    return;
  }
  const bool large = effect_id >= kLargeEffectBase;
  const std::int16_t base_effect =
      large ? static_cast<std::int16_t>(effect_id - kLargeEffectBase)
            : effect_id;
  if (!ValidEffectId(base_effect)) {
    return;
  }

  const std::int16_t safe_radius = std::max<std::int16_t>(0, radius);
  if (large && safe_radius > 0) {
    // These constants are the original DAT_005752b8/.80/.c8/.c0 values:
    // 0.04, 0.50, 0.25, and 0.16 respectively.
    const int inner_count = static_cast<int>(std::lround(safe_radius * 0.04F));
    const int inner_extent =
        std::max(1, static_cast<int>(std::lround(safe_radius * 0.50F)));
    const float inner_bias = safe_radius * 0.25F;
    for (int i = 0; i < inner_count; ++i) {
      NovaEffects_SpawnImpactEffect(
          state,
          x + static_cast<float>(RandomRange(state, inner_extent)) - inner_bias,
          y + static_cast<float>(RandomRange(state, inner_extent)) - inner_bias,
          1,
          static_cast<std::int16_t>(4 + RandomRange(state, 8)));
    }

    const int outer_count = static_cast<int>(std::lround(safe_radius * 0.16F));
    const int outer_extent = std::max(1, static_cast<int>(safe_radius));
    const float outer_bias = safe_radius * 0.50F;
    for (int i = 0; i < outer_count; ++i) {
      NovaEffects_SpawnImpactEffect(
          state,
          x + static_cast<float>(RandomRange(state, outer_extent)) - outer_bias,
          y + static_cast<float>(RandomRange(state, outer_extent)) - outer_bias,
          0,
          static_cast<std::int16_t>(8 + RandomRange(state, 16)));
    }
  }

  // The original's area helper plays the sound for the main effect only. The
  // child sprites are visual variants and must not each retrigger audio.
  NovaEffects_SpawnImpactEffect(state, x, y, base_effect, 0);
  const ImpactEffect *definition = state.scenario.ImpactEffectAt(base_effect);
  if (play_sound && definition != nullptr &&
      definition->impact_sound_slot >= 0 &&
      definition->impact_sound_slot < 64) {
    state.pending_impact_sounds.push_back(
        {definition->impact_sound_slot, x, y});
  }
}

void NovaEffects_SpawnImpactEffectPackage(GameState &state,
                                          float x,
                                          float y,
                                          std::int16_t package_id,
                                          bool play_sound) {
  const AsteroidDef *package = state.scenario.ImpactPackageAt(package_id);
  if (package == nullptr || !package->present) {
    return;
  }
  const std::int16_t area_effect_id = package->ImpactAreaEffectId();
  if (area_effect_id < 0) {
    return;
  }
  // The original package passes a zero radius: the package selects the
  // effect, while weapon splash radius belongs to the preceding hit path.
  NovaEffects_SpawnAreaImpact(state, x, y, area_effect_id, 0, play_sound);
}

void NovaEffects_TickImpactEffects(GameState &state, float elapsed_ms) {
  const float delta = std::max(0.0F, elapsed_ms);
  for (ImpactEffectInstance &instance : state.impact_effect_instances) {
    if (instance.anim_time < 0.0F) {
      continue;
    }
    const ImpactEffect *definition =
        state.scenario.ImpactEffectAt(instance.effect_id);
    if (definition == nullptr || definition->frame_rate_scale <= 0.0F) {
      instance.anim_time = -1.0F;
      continue;
    }
    if (instance.delay_timer > 0.0F) {
      instance.delay_timer = std::max(
          0.0F, instance.delay_timer - definition->frame_rate_scale * delta);
      continue;
    }
    instance.anim_time += definition->frame_rate_scale * delta;
  }
}

} // namespace game
