#pragma once

#include "game_state.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace game::weapon_detail {

// The clean-room weapon banks use the original 100-element stride.
inline constexpr std::size_t kBankStride = 100;

inline std::int16_t &BankAmmo(GameState &state, std::int16_t bank) {
  return state
      .weapon_count_by_class[static_cast<std::size_t>(bank) * kBankStride];
}

inline const std::int16_t &BankAmmo(const GameState &state, std::int16_t bank) {
  return state
      .weapon_count_by_class[static_cast<std::size_t>(bank) * kBankStride];
}

inline std::int16_t &BankSecondary(GameState &state, std::int16_t bank) {
  return state.weapon_secondary_count_by_class[static_cast<std::size_t>(bank) *
                                               kBankStride];
}

inline const std::int16_t &BankSecondary(const GameState &state,
                                         std::int16_t bank) {
  return state.weapon_secondary_count_by_class[static_cast<std::size_t>(bank) *
                                               kBankStride];
}

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

} // namespace game::weapon_detail
