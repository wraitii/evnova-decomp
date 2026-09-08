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

// Ghidra 0x00421500 Shot_SpawnImpactEffectSprite.
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

// Ghidra 0x00428090 Shot_SpawnShipDestructionDebrisPuff.
void NovaEffects_SpawnShipDestructionBurst(GameState &state,
                                           const Ship &ship,
                                           std::int16_t breaking_effect_id) {
  const float x = ship.pos_x;
  const float y = ship.pos_y;
  // The original's destruction path repeatedly calls the debris-puff helper
  // while the death presentation is active. The port has no separate
  // death-timer producer yet, so seed the equivalent short cadence here.
  // Keep the stock first flash in slot 0; Explode1 is the class-selected
  // breakup animation that follows it in the original sequence.
  NovaEffects_SpawnImpactEffect(state, x, y, 0, 0);
  if (breaking_effect_id >= 0 && breaking_effect_id != 0) {
    NovaEffects_SpawnImpactEffect(state, x, y, breaking_effect_id, 2);
  }
  for (int i = 1; i < 6; ++i) {
    const float extent = 7.0F + static_cast<float>(i * 3);
    const float dx = static_cast<float>(
                         RandomRange(state, 2 * static_cast<int>(extent) + 1)) -
                     extent;
    const float dy = static_cast<float>(
                         RandomRange(state, 2 * static_cast<int>(extent) + 1)) -
                     extent;
    NovaEffects_SpawnImpactEffect(state,
                                  x + dx,
                                  y + dy,
                                  static_cast<std::int16_t>(i % 2),
                                  static_cast<std::int16_t>(i * 5));
  }

  for (FadingEffectInstance &fragment : state.fading_effect_instances) {
    if (fragment.lifetime_ticks >= 0.0F) {
      continue;
    }
    fragment.pos_x = x;
    fragment.pos_y = y;
    fragment.vel_x = ship.vel_x;
    fragment.vel_y = ship.vel_y;
    fragment.lifetime_ticks = static_cast<float>(150 + RandomRange(state, 100));
    const float angle =
        static_cast<float>(RandomRange(state, 360)) * 0.01745329252F;
    const float speed = static_cast<float>(10 + RandomRange(state, 10));
    fragment.vel_x += std::sin(angle) * speed;
    fragment.vel_y += -std::cos(angle) * speed;
    fragment.heading_radians = angle;
    break;
  }

  const ImpactEffect *definition = state.scenario.ImpactEffectAt(0);
  if (definition != nullptr && definition->impact_sound_slot >= 0 &&
      definition->impact_sound_slot < 64) {
    state.pending_impact_sounds.push_back(
        {definition->impact_sound_slot, x, y});
  }
  state.pending_destruction_sounds.push_back({x, y});
}

void NovaEffects_SpawnShipDestructionFinale(GameState &state,
                                            const Ship &ship,
                                            std::int16_t final_effect_id) {
  if (final_effect_id < 0) {
    return;
  }
  const ShipClass *ship_class =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const std::int16_t mass = ship_class != nullptr ? ship_class->mass_tons : 0;
  NovaEffects_SpawnAreaImpact(state,
                              ship.pos_x,
                              ship.pos_y,
                              final_effect_id,
                              final_effect_id >= kLargeEffectBase ? mass : 0,
                              true);
}

// Ghidra 0x0043b170 Frame_UpdateFadingEffectSprites.
void NovaEffects_TickFadingEffects(GameState &state, float elapsed_ticks) {
  const float delta = std::max(0.0F, elapsed_ticks);
  for (FadingEffectInstance &fragment : state.fading_effect_instances) {
    if (fragment.lifetime_ticks < 0.0F) {
      continue;
    }
    fragment.pos_x += fragment.vel_x * delta;
    fragment.pos_y += fragment.vel_y * delta;
    fragment.lifetime_ticks -= delta;
    if (fragment.lifetime_ticks <= 0.0F) {
      fragment.lifetime_ticks = -1.0F;
    }
  }
}

// Ghidra 0x004211d0 Shot_SpawnAreaImpactEffects.
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

// Ghidra 0x00462550 Weapon_SpawnWeaponImpactEffectPackage.
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

// Ghidra 0x0042e160 Shot_UpdateImpactEffectSprites.
void NovaEffects_TickImpactEffects(GameState &state, float elapsed_ticks) {
  const float delta = std::max(0.0F, elapsed_ticks);
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
