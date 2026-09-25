#pragma once

// Shared conversion between the port's normalized tick delta and the original
// spaceflight loop's raw per-call cadence. Frame_MeasureFrameTiming
// (0x00432ea0) floors ordinary spaceflight calls at 21 ms and publishes
// elapsed_ms * 0.03 as the normalized 30 Hz tick scale, so one raw call is
// worth 21 * 0.03 = 0.63 normalized ticks. Consumers that model a discrete
// per-call mutation advance by elapsed_ticks / 0.63. See
// docs/frame_timing_and_cadence.md.

namespace game {

inline constexpr float kOriginalMaxRateFrameTicks = 21.0F * 0.03F;

// Normalized-tick delta -> whole raw spaceflight calls at the original
// maximum rate.
// @port 0x00432ea0 40% cadence,divergence
[[nodiscard]] constexpr float RawSpaceflightCallTicks(float elapsed_ticks) {
  return elapsed_ticks / kOriginalMaxRateFrameTicks;
}

} // namespace game
