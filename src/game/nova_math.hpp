#pragma once

// Game-convention angle helpers (bearing 0 = up / -y, increasing clockwise).
// Shared so the AI, weapons and spawn code agree on one winding convention.

#include <cmath>

namespace game {

// Folds a degree value into [0, 360).
[[nodiscard]] inline float WrapDeg(float d) {
  constexpr float kFullCircleDeg = 360.0F;
  d = std::fmod(d, kFullCircleDeg);
  return d < 0.0F ? d + kFullCircleDeg : d;
}

// @port 0x0046B210 100%
// Ghidra Math_ShortestAngleDeltaDeg (0x0046b210): absolute shortest angular
// distance between two game-degree bearings, in [0,180]. The original loses the
// rotation sign; direction is recovered by comparing the raw wrapped delta
// against the 181-degree split. Shared by the weapon/guidance and AI code that
// previously kept file-local copies.
[[nodiscard]] inline int ShortestAngleDeltaDeg(int from, int to) {
  int delta = std::abs(from - to);
  if ((from > 179) != (to > 179)) {
    delta = 360 - delta;
  }
  if (delta > 180) {
    delta = 360 - delta;
  }
  return delta;
}

// Ghidra Math_BearingFromPointToPoint (0x0043b670): heading in degrees from
// (x1,y1) to (x2,y2). The codebase's heading convention is atan2(dx, -dy).
[[nodiscard]] inline float BearingDeg(float x1, float y1, float x2, float y2) {
  constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
  return WrapDeg(std::atan2(x2 - x1, -(y2 - y1)) / kDegToRad);
}

// Frame index for a heading, shared by the renderer and the collision mask so
// the displayed sprite and its mask select the same frame. EV Nova ships point
// 'up' at frame 0 with heading increasing clockwise; frames progress one per
// sector of the rotation. Guards a non-positive frame count so a degenerate
// class falls back to frame 0 rather than dividing by zero.
// TODO(decomp): verify phase/clockwise orientation against a real rendered
// ship -- this is the conventional mapping and should be re-checked once the
// ship is on screen.
[[nodiscard]] inline int FrameForHeading(float heading_radians,
                                         int frames_per_rotation) {
  if (frames_per_rotation <= 0) {
    return 0;
  }
  constexpr float kTwoPi = 6.28318530717958647692F;
  const float normalized = std::fmod(heading_radians + kTwoPi, kTwoPi);
  const float sector =
      (normalized / kTwoPi) * static_cast<float>(frames_per_rotation);
  int frame = static_cast<int>(std::lround(sector)) % frames_per_rotation;
  if (frame < 0) {
    frame += frames_per_rotation;
  }
  return frame;
}

} // namespace game
