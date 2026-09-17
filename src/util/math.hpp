#pragma once

// Small numeric helpers shared across translation units.

#include <cmath>
#include <cstdint>

namespace evnova::util {

// Ghidra 0x0043b4a0 Math_AddPolarVelocity: add a polar vector to an XY pair
// using the game's screen convention (bearing 0 = up, increasing clockwise:
// x += sin*speed, y -= cos*speed). The pair is either a velocity or a
// world-position offset depending on the call site.
inline void AddPolar(float angle_rad, float speed, float &x, float &y) {
  x += std::sin(angle_rad) * speed;
  y -= std::cos(angle_rad) * speed;
}

// Truncate toward zero, matching the original's x87 FIST + residual/sign
// correction (e.g. 0x00480030, 0x00482280), not round-half-up.
[[nodiscard]] inline std::int32_t TruncateToInt32(double v) {
  return static_cast<std::int32_t>(v);
}

} // namespace evnova::util
