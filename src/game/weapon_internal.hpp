#pragma once

#include "game_state.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace game::weapon_detail {

// The weapon loaded in a bank, or nullptr when the bank holds nothing valid.
inline const Weapon *WeaponAt(const GameState &state, std::int16_t bank) {
  // Banks are indexed by zero-based weapon id; the scenario Weapon() lookup
  // uses the 0x80.. residue. Bank slot b == weapon resource id -- but our
  // clean Weapon table is indexed by (weapon id - 0x80). Resolve via the
  // resource-id offset: bank slot `b` corresponds to weapon resource `b+0x80`.
  return state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
}

// The original's truncate-then-wrap heading quantization: truncate toward
// zero to an integer degree (x87 FIST + residual/sign correction), then a
// single-step wrap into [0,360) (k_wrap_360_f32 0x005753b8). The per-frame
// turn is far smaller than 360 deg, so one subtraction matches the original's
// do-while.
inline int RoundHeadingDeg(float deg) {
  const int rounded = static_cast<int>(deg);
  return (rounded % 360 + 360) % 360;
}

// Ghidra 0x0043b4a0 Math_AddPolarVelocity: ADD a polar vector onto an XY
// velocity pair using the game angle convention (sin for x, -cos for y).
inline void
AddPolarVelocity(float bearing_deg, float speed, float &vel_x, float &vel_y) {
  const float rad = bearing_deg * (3.14159265358979323846F / 180.0F);
  vel_x += std::sin(rad) * speed;
  vel_y -= std::cos(rad) * speed;
}

// The displayed rotation frame of the ship's sprite. TODO(decomp): the
// original reads the sprite object's rotation counter (sprite+0x68) modulo
// FramesPer; the port derives the same displayed frame from the heading with
// the renderer's mapping (spaceflight_view FrameForHeading).
[[nodiscard]] inline int RotationFrameForShip(const Ship &ship,
                                              int frames_per_rotation) {
  if (frames_per_rotation < 1) {
    return 0;
  }
  constexpr float kTwoPi = 6.28318530717958647692F;
  const float normalized = std::fmod(ship.heading + kTwoPi, kTwoPi);
  int frame =
      static_cast<int>(std::lround(normalized / kTwoPi *
                                   static_cast<float>(frames_per_rotation))) %
      frames_per_rotation;
  if (frame < 0) {
    frame += frames_per_rotation;
  }
  return frame;
}

// @port 0x0046C5C0 100%
// Ghidra 0x0046c5c0 Weapon_ApplyTurretSpreadVelocity: apply one turret
// group's quadrant-barrel muzzle displacement to a position. Accumulates
// polar(forward, ship_bearing) + polar(lateral, ship_bearing + 90 mod 360)
// (barrel i = group*4+quadrant; ShipClass.muzzle_forward/lateral), scales x
// by the NEAR pair (muzzle_scale_near_*) when the accumulated y < 0 and the
// FAR pair otherwise, then pos += (scaled_x, scaled_y - drop).
inline void ApplyTurretSpreadVelocity(const ShipClass &cls,
                                      float &pos_x,
                                      float &pos_y,
                                      std::int16_t ship_bearing_deg,
                                      int turret_group_id,
                                      int quadrant_index) {
  if (turret_group_id < 0 || turret_group_id >= 4 || quadrant_index < 0 ||
      quadrant_index >= 4) {
    return;
  }
  float acc_x = 0.0F;
  float acc_y = 0.0F;
  AddPolarVelocity(
      static_cast<float>(ship_bearing_deg),
      static_cast<float>(cls.muzzle_forward[turret_group_id][quadrant_index]),
      acc_x,
      acc_y);
  // (bearing + 90) truncated modulo 360 (C signed-division semantics).
  int side_bearing = static_cast<int>(ship_bearing_deg) + 90;
  side_bearing -= 360 * (side_bearing / 360);
  AddPolarVelocity(
      static_cast<float>(side_bearing),
      static_cast<float>(cls.muzzle_lateral[turret_group_id][quadrant_index]),
      acc_x,
      acc_y);
  const float scale_x =
      acc_y < 0.0F ? cls.muzzle_scale_near_x : cls.muzzle_scale_far_x;
  const float scale_y =
      acc_y < 0.0F ? cls.muzzle_scale_near_y : cls.muzzle_scale_far_y;
  pos_x += acc_x * scale_x;
  pos_y += acc_y * scale_y -
           static_cast<float>(cls.muzzle_drop[turret_group_id][quadrant_index]);
}

// @port 0x0046C4E0 100%
// Ghidra 0x0046c4e0 Weapon_ChooseBestTurretQuadrantForTarget: apply each of
// the four quadrant barrel offsets to the muzzle position and return the
// quadrant minimizing the squared distance to the target point. Returns 0
// (not -1) for invalid arguments -- original quirk.
[[nodiscard]] inline int
ChooseBestTurretQuadrantForTarget(const ShipClass &cls,
                                  float muzzle_x,
                                  float muzzle_y,
                                  std::int16_t ship_bearing_deg,
                                  int turret_group_id,
                                  float target_x,
                                  float target_y) {
  if (turret_group_id < 0 || turret_group_id >= 4) {
    return 0;
  }
  int best = -1;
  float best_dist_sq = 0.0F;
  for (int quadrant = 0; quadrant < 4; ++quadrant) {
    float x = muzzle_x;
    float y = muzzle_y;
    ApplyTurretSpreadVelocity(
        cls, x, y, ship_bearing_deg, turret_group_id, quadrant);
    const float dx = x - target_x;
    const float dy = y - target_y;
    const float dist_sq = dx * dx + dy * dy;
    if (best == -1 || dist_sq < best_dist_sq) {
      best = quadrant;
      best_dist_sq = dist_sq;
    }
  }
  return best;
}

// The FISTP(nearest-even) + signed/unsigned backoff idiom used by the range
// functions (disasm 0x0046cfe1..0x0046d01f): nets to floor() for value >= 0
// and to ceil() for value < 0. Range data is non-negative, so this is
// floor() in practice (same idiom as FloorAbsDelta at 0x00411600).
inline int RoundRangeEnvelope(float value) {
  int rounded = static_cast<int>(std::nearbyint(value));
  const float remainder = value - static_cast<float>(rounded);
  if (value >= 0.0F) {
    if (remainder < 0.0F) { // FISTP rounded up
      --rounded;
    }
  } else if (remainder > 0.0F) { // FISTP rounded down
    ++rounded;
  }
  return rounded;
}

// Bearing in degrees for a ship's displayed rotation frame:
// trunc(frame * 360 / frames_per_rotation). Shot_UpdateBeamHitQueue
// (0x0042f270) derives the turret-exit rotation this way before calling
// Weapon_ApplyTurretSpreadVelocity; Weapon_SelectTurretQuadrant (0x0046c320)
// uses the same quantization.
[[nodiscard]] inline std::int16_t
TurretBearingDegForShip(const Ship &ship, int frames_per_rotation) {
  const int frames = frames_per_rotation > 0 ? frames_per_rotation : 36;
  const int frame = RotationFrameForShip(ship, frames);
  return static_cast<std::int16_t>(RoundRangeEnvelope(
      static_cast<float>(frame) * (360.0F / static_cast<float>(frames))));
}

} // namespace game::weapon_detail
