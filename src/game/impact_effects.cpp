#include "impact_effects.hpp"

#include "frame_timing.hpp"
#include "nova_random.hpp"
#include "scenario_data.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace game {
namespace {

constexpr std::int16_t kFirstImpactEffect = 0;
constexpr std::int16_t kLastImpactEffect = 63;
constexpr std::int16_t kLargeEffectBase = 1000;
// The original converts a pixel/frame quantity to the SWParticle pool's
// 8.8 fixed-point representation by multiplying by 256.0f (DAT_00575270).
constexpr float kParticleFixedScale = 256.0F;
constexpr float kGameDegreesToRadians = 0.017453292519943295F;

[[nodiscard]] std::int32_t ParticleFixed(float pixels) {
  return static_cast<std::int32_t>(std::lround(pixels * kParticleFixedScale));
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
void NovaEffects_SpawnShipDestructionDebrisPuff(GameState &state,
                                                const Ship &ship) {
  // DAT_00575208 = 0.1: the random scatter speed is (10 + rand(10)) * 0.1.
  constexpr float kDebrisScatterSpeedScale = 0.1F;
  for (FadingEffectInstance &fragment : state.fading_effect_instances) {
    if (fragment.lifetime_ticks >= 0.0F) {
      continue;
    }
    // Lifetime fills 150..249 (rand(100) + 0x96).
    fragment.lifetime_ticks = static_cast<float>(150 + RandomBelow(state, 100));
    fragment.pos_x = ship.pos_x;
    fragment.pos_y = ship.pos_y;
    fragment.vel_x = ship.vel_x;
    fragment.vel_y = ship.vel_y;
    const float scatter_angle =
        static_cast<float>(RandomBelow(state, 360)) * kGameDegreesToRadians;
    const float scatter_speed =
        static_cast<float>(10 + RandomBelow(state, 10)) *
        kDebrisScatterSpeedScale;
    // Math_AddPolarVelocity: bearing 0 = up, increasing clockwise.
    fragment.vel_x += std::sin(scatter_angle) * scatter_speed;
    fragment.vel_y += -std::cos(scatter_angle) * scatter_speed;
    // The original orients the debris sprite by the resulting velocity's
    // bearing (Math_BearingFromPointToPoint on the post-scatter velocity).
    fragment.heading_radians = std::atan2(fragment.vel_x, -fragment.vel_y);
    return;
  }
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
    // 0.04, 0.50, 0.25, and 0.16 respectively. 0x004211d0 truncates each
    // count toward zero (x87 FIST + residual/sign), not round-to-nearest.
    const int inner_count = static_cast<int>(safe_radius * 0.04F);
    const int inner_extent = std::max(1, static_cast<int>(safe_radius * 0.50F));
    const float inner_bias = safe_radius * 0.25F;
    for (int i = 0; i < inner_count; ++i) {
      NovaEffects_SpawnImpactEffect(
          state,
          x + static_cast<float>(RandomBelow(state, inner_extent)) - inner_bias,
          y + static_cast<float>(RandomBelow(state, inner_extent)) - inner_bias,
          1,
          static_cast<std::int16_t>(4 + RandomBelow(state, 8)));
    }

    const int outer_count = static_cast<int>(safe_radius * 0.16F);
    const int outer_extent = std::max(1, static_cast<int>(safe_radius));
    const float outer_bias = safe_radius * 0.50F;
    for (int i = 0; i < outer_count; ++i) {
      NovaEffects_SpawnImpactEffect(
          state,
          x + static_cast<float>(RandomBelow(state, outer_extent)) - outer_bias,
          y + static_cast<float>(RandomBelow(state, outer_extent)) - outer_bias,
          0,
          static_cast<std::int16_t>(8 + RandomBelow(state, 16)));
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

// Ghidra Weapon_SpawnWeaponImpactParticleBurst (0x004274d0).
void NovaEffects_SpawnWeaponImpactParticleBurst(GameState &state,
                                                float x,
                                                float y,
                                                float speed,
                                                std::int16_t scatter,
                                                std::int16_t life_base,
                                                std::int16_t life_max,
                                                std::uint32_t color,
                                                std::int16_t blend_mode,
                                                std::int16_t count,
                                                std::int16_t position_scatter) {
  if (count <= 0) {
    return;
  }
  const int scatter_span = static_cast<int>(scatter) * 2 + 1;
  const int scatter_floor = 100 - static_cast<int>(scatter);
  const bool scale_speed = scatter > 0 && scatter < 100;
  const int life_span =
      static_cast<int>(life_max) - static_cast<int>(life_base) + 1;
  const int position_span = static_cast<int>(position_scatter) * 100;

  for (int i = 0; i < count; ++i) {
    // Original order: scale RNG, heading RNG, then (for a scattered position)
    // the offset-magnitude and offset-heading RNGs. Preserving the order keeps
    // the shared game RNG stream aligned with the disassembly.
    float particle_speed = speed;
    if (scale_speed) {
      particle_speed =
          static_cast<float>(scatter_floor + RandomBelow(state, scatter_span)) *
          0.01F * particle_speed;
    }
    // Math_AddPolarVelocity: bearing 0 = up, increasing clockwise.
    const float angle =
        static_cast<float>(RandomBelow(state, 0x168)) * kGameDegreesToRadians;
    const float vel_x = std::sin(angle) * particle_speed;
    const float vel_y = -std::cos(angle) * particle_speed;

    std::int16_t life = life_base;
    if (life_base < life_max) {
      life =
          static_cast<std::int16_t>(life_base + RandomBelow(state, life_span));
    }

    float pos_x = x;
    float pos_y = y;
    if (position_scatter > 0) {
      const float magnitude =
          static_cast<float>(RandomBelow(state, position_span)) * 0.01F;
      const float offset_angle =
          static_cast<float>(RandomBelow(state, 0x168)) * kGameDegreesToRadians;
      pos_x += std::sin(offset_angle) * magnitude;
      pos_y += -std::cos(offset_angle) * magnitude;
    }

    // Ghidra 0x0047b960 SWParticles_SpawnParticle: the original free-head
    // allocator writes the same 8.8 fixed state; the clean-room appends to a
    // growable vector because the shipped 100000-slot pool is never full.
    SwParticle particle;
    particle.life_ticks = life;
    particle.pos_x = ParticleFixed(pos_x);
    particle.pos_y = ParticleFixed(pos_y);
    particle.vel_x = ParticleFixed(vel_x);
    particle.vel_y = ParticleFixed(vel_y);
    particle.blend_mode = blend_mode;
    particle.color = color;
    state.sw_particles.push_back(particle);
  }
}

// Convenience wrapper for the four weapon-impact callsites.
void NovaEffects_SpawnWeaponImpactBurstForWeapon(GameState &state,
                                                 float x,
                                                 float y,
                                                 const Weapon &weapon,
                                                 std::int16_t scatter) {
  if (weapon.impact_particle_count <= 0) {
    return;
  }
  // Shot_ResolveShotCollisionHit computes trunc(frame_base * 1.25) as the
  // lifetime upper bound (double DAT_005753e0 = 1.25; x87 FIST +
  // residual/sign correction, not round-to-nearest).
  const auto life_max = static_cast<std::int16_t>(
      static_cast<float>(weapon.impact_particle_frame_base) * 1.25F);
  NovaEffects_SpawnWeaponImpactParticleBurst(state,
                                             x,
                                             y,
                                             weapon.impact_particle_speed,
                                             scatter,
                                             weapon.impact_particle_frame_base,
                                             life_max,
                                             weapon.impact_particle_color,
                                             /*blend_mode=*/0x20,
                                             weapon.impact_particle_count,
                                             /*position_scatter=*/0);
}

// Ghidra Shot_HandleShot (0x00435830), continuous trail arm
// 0x00436077..0x00436262. The original selects color before speed, then calls
// Weapon_SpawnWeaponImpactParticleBurst with scatter and position scatter both
// zero. The loader's eight variants are runtime WeaponDef bands, not the
// separate animated sprite smoke-puff pool.
void NovaEffects_SpawnWeaponTrailParticles(GameState &state,
                                           float x,
                                           float y,
                                           const Weapon &weapon,
                                           float heading_deg,
                                           float anchor_offset_px,
                                           std::int16_t blend_mode) {
  if (weapon.trail_particle_count <= 0) {
    return;
  }
  const auto color_index = static_cast<std::size_t>(RandomBelow(
      state, static_cast<int>(weapon.trail_particle_color_variants.size())));
  const auto speed_index = static_cast<std::size_t>(RandomBelow(
      state, static_cast<int>(weapon.trail_particle_speed_variants.size())));
  const float speed = weapon.trail_particle_speed_variants[speed_index];
  const std::uint32_t color = weapon.trail_particle_color_variants[color_index];

  // DAT_00575450 is the original 180-degree offset: trail particles start at
  // the rear edge of the shot sprite. ActiveShot does not own the SDL sprite
  // handle, so the caller supplies the best available half-height estimate.
  const float behind_angle = (heading_deg + 180.0F) * kGameDegreesToRadians;
  x += std::sin(behind_angle) * anchor_offset_px;
  y -= std::cos(behind_angle) * anchor_offset_px;
  NovaEffects_SpawnWeaponImpactParticleBurst(state,
                                             x,
                                             y,
                                             speed,
                                             /*scatter=*/0,
                                             weapon.trail_particle_life_min,
                                             weapon.trail_particle_life_max,
                                             color,
                                             blend_mode,
                                             weapon.trail_particle_count,
                                             /*position_scatter=*/0);
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

// The original's SWParticles_Update (0x0047c800) runs once per rendered frame
// through SWParticles_UpdateDirtyPixels in the present hook
// (Frame_PresentViewportAndParticles 0x00439d40). Although the sprite world
// itself has no target-FPS cap, the enclosing flight loop admits at most one
// iteration per 21 ms (Frame_MeasureFrameTiming 0x00432ea0). Bank whole
// logical calls so the discrete lifetime/integration order remains faithful
// while staying independent of the port's display refresh rate.

// Ghidra SWParticles_Update (0x0047c800).
void NovaEffects_TickSwParticles(GameState &state, float elapsed_ticks) {
  state.sw_particle_tick_accumulator +=
      std::max(0.0F, elapsed_ticks) / kOriginalMaxRateFrameTicks;
  while (state.sw_particle_tick_accumulator >= 1.0F) {
    state.sw_particle_tick_accumulator -= 1.0F;
    for (SwParticle &particle : state.sw_particles) {
      if (particle.life_ticks <= 0) {
        continue;
      }
      particle.life_ticks = static_cast<std::int16_t>(particle.life_ticks - 1);
      if (particle.life_ticks > 0) {
        particle.pos_x += particle.vel_x;
        particle.pos_y += particle.vel_y;
        // The shipped pool is allocated with gravity 0
        // (SWParticles_AllocatePool(100000, 0)), so vel_y is not advanced.
      }
    }
  }
  std::erase_if(state.sw_particles, [](const SwParticle &particle) {
    return particle.life_ticks <= 0;
  });
}

} // namespace game
