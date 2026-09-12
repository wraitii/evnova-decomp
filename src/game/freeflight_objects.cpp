#include "freeflight_objects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

#include "../brgr_archive.hpp"
#include "game_state.hpp"
#include "ship_visual.hpp"

namespace game {
namespace {

constexpr float kPi = 3.14159265358979323846F;

// DAT_005753a0: the freeflight spin sets' frame count (spin 500 is a 36-tile
// sheet). The tick wraps the frame accumulator in [0, this).
constexpr float kFreeflightSpinFrames = 36.0F;

// The 500+index spin-sprite table assigned by Sprite_AssignSpriteSet; index 0
// is the stock cargo/junk pod set for the jettison variant.
constexpr std::int16_t kFreeflightSpinBase = 500;

// _DAT_00575280 = 0.5, used to offset the spawn from the source ship's frame
// span; k_interference_scale_f64 (0x00575288) = 100.0, scaling the second
// velocity scatter.
constexpr float kLaunchSpanShare = 0.5F;
constexpr float kInterferenceScale = 100.0F;

// _DAT_00575290 = 0.2 and _DAT_00575298 = 0.01, the at-position launch scale.
constexpr float kAtPositionScaleA = 0.2F;
constexpr float kAtPositionScaleB = 0.01F;

std::int16_t RandomRange(GameState &state, int bound) {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::int16_t>(
      std::uniform_int_distribution<int>{0, bound - 1}(state.rng));
}

// Math_AddPolarVelocity (0x0043b4a0): add a polar vector to an XY pair using
// the game's 0 = up, clockwise angle convention (x += sin*speed,
// y -= cos*speed). Here the pair is either a velocity or (for the jettison
// position offset) a position, matching the original's two call sites.
void AddPolar(float angle_rad, float speed, float &x, float &y) {
  x += std::sin(angle_rad) * speed;
  y -= std::cos(angle_rad) * speed;
}

// Sprite_GetFrameFullHeight (0x00462390) for a ship: its current frame's full
// width, defaulting to 0x20 when no descriptor is decoded. Mirrors
// TargetFrameSpan (boarding_plunder.cpp), reading the sh\x8an base size at the
// renderer's class-id convention.
float ShipFrameWidthSpan(const Ship &ship) {
  const auto class_id = static_cast<std::uint16_t>(ship.ship_class_id + 0x80);
  if (const auto resource =
          NovaResource_Load(kShipVisualResourceType, class_id)) {
    if (const auto visual = DecodeShipVisualDescriptor(*resource)) {
      if (visual->base_x_size > 0) {
        return static_cast<float>(visual->base_x_size);
      }
    }
  }
  return 32.0F;
}

// Shared tail of both spawn variants: pick a random spin animation rate.
std::int16_t RandomSpinRate(GameState &state) {
  const std::int16_t roll = RandomRange(state, 4);
  if (roll == 0) {
    return -1;
  }
  if (roll == 3) {
    return 1;
  }
  return 0;
}

} // namespace

// Ghidra 0x0041f800 Ship_SpawnFreeflightObjectForShip.
void NovaFreeflight_SpawnForShip(GameState &state, const Ship &ship) {
  for (FreeflightObjectState &object : state.freeflight_objects) {
    if (object.lifetime_ticks >= 0.0F) {
      continue;
    }
    object.pos_x = ship.pos_x;
    object.pos_y = ship.pos_y;
    object.vel_x = ship.vel_x;
    object.vel_y = ship.vel_y;
    object.lifetime_ticks = static_cast<float>(RandomRange(state, 0x5a) + 0xb4);
    object.system_id = ship.current_system_id;
    object.frame_counter = static_cast<float>(RandomRange(state, 0x24));
    object.persistent = false;
    object.extra = 0;
    object.sprite_set_index = 0;
    object.spin_rate = RandomSpinRate(state);

    // Launch scatter: offset the spawn behind the ship by half the frame
    // span, then add a backward velocity scatter (0.30..0.69 px/tick).
    const float back = ship.heading + kPi;
    const float span = ShipFrameWidthSpan(ship);
    AddPolar(
        back, std::trunc(span * kLaunchSpanShare), object.pos_x, object.pos_y);
    const float scatter =
        (static_cast<float>(RandomRange(state, 0x28)) + 0x1e) /
        kInterferenceScale;
    const float spread = (static_cast<float>(RandomRange(state, 0x1e)) -
                          static_cast<float>(RandomRange(state, 0xf))) *
                         (kPi / 180.0F);
    AddPolar(back + spread, scatter, object.vel_x, object.vel_y);
    return;
  }
}

// Ghidra 0x0041fb50 Ship_SpawnFreeflightObjectAtPosition.
void NovaFreeflight_SpawnAtPosition(GameState &state,
                                    float pos_x,
                                    float pos_y,
                                    std::int16_t extra,
                                    std::int16_t sprite_set_index) {
  for (FreeflightObjectState &object : state.freeflight_objects) {
    if (object.lifetime_ticks >= 0.0F) {
      continue;
    }
    object.pos_x = pos_x;
    object.pos_y = pos_y;
    object.vel_x = 0.0F;
    object.vel_y = 0.0F;
    object.lifetime_ticks = static_cast<float>(RandomRange(state, 200) + 300);
    object.system_id = state.player.current_system_id;
    object.frame_counter = static_cast<float>(RandomRange(state, 0x24));
    object.persistent = true;
    object.extra = extra;
    object.sprite_set_index = sprite_set_index;
    object.spin_rate = RandomSpinRate(state);

    const float speed = (static_cast<float>(RandomRange(state, 0x51)) + 0x3c) *
                        kAtPositionScaleA * kAtPositionScaleB;
    const float angle =
        static_cast<float>(RandomRange(state, 0x168)) * (kPi / 180.0F);
    AddPolar(angle, speed, object.vel_x, object.vel_y);
    return;
  }
}

// Ghidra 0x0042c1b0 Frame_UpdateFreeflightObjectSprites (simulation half).
// The original also cancels every object when the DAT_00596d2a "clear
// transient sprites" latch is set; the port has no counterpart (the effect
// pools behave the same), so objects are retired by lifetime/system instead.
void NovaFreeflight_Tick(GameState &state, float elapsed_ticks) {
  const float delta = std::max(0.0F, elapsed_ticks);
  for (FreeflightObjectState &object : state.freeflight_objects) {
    if (object.lifetime_ticks < 0.0F) {
      continue;
    }
    object.frame_counter += static_cast<float>(object.spin_rate) * delta;
    while (object.frame_counter >= kFreeflightSpinFrames) {
      object.frame_counter -= kFreeflightSpinFrames;
    }
    while (object.frame_counter < 0.0F) {
      object.frame_counter += kFreeflightSpinFrames;
    }
    object.lifetime_ticks -= delta;
    object.pos_x += object.vel_x * delta;
    object.pos_y += object.vel_y * delta;
  }
}

// The spin-sprite resource id for one freeflight object type. Kept here so
// the renderer and any future consumer agree on the 500+index table.
std::uint16_t NovaFreeflightSpriteSetId(std::int16_t sprite_set_index) {
  return static_cast<std::uint16_t>(kFreeflightSpinBase + sprite_set_index);
}

} // namespace game
