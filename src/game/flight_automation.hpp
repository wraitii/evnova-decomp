#pragma once

#include "../sdl_platform.hpp"
#include "game_state.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace game {
enum class FlightAutomationGoal { kNone, kLandAt, kJumpTo };
enum class FlightAutomationPhase {
  kIdle,
  kSelect,
  kMove,
  kEngage,
  kWaiting,
  kComplete,
  kFailed,
  kCancelled
};

struct FlightAutomationStatus {
  FlightAutomationGoal goal = FlightAutomationGoal::kNone;
  FlightAutomationPhase phase = FlightAutomationPhase::kIdle;
  std::string target;
  std::string detail;
};

// Optional clean-room tutorial input producer. It observes GameState and only
// modifies FlightInput; it never writes gameplay state.
class FlightAutomationController {
public:
  // `landing_envelope_axis_range` is the target's per-axis arrival envelope
  // (Stellar_MaxLandingDistance); <= 0 selects the conservative fallback. The
  // caller resolves it from the target sprite so the approach stops inside the
  // real gate instead of a fixed guess.
  bool LandAt(const GameState &,
              std::string,
              std::uint64_t now_ms,
              std::uint64_t timeout_ms = 180000,
              float landing_envelope_axis_range = -1.0F);
  bool JumpTo(const GameState &,
              std::string,
              std::uint64_t now_ms,
              std::uint64_t timeout_ms = 180000);
  void Cancel();
  void ObservedDocked();
  // `elapsed_ticks` is the normalized 30 Hz tick scale the integrator consumes
  // this frame (accelerated probe mode can be > 1); the low-level maneuver uses
  // it to widen its one-frame alignment gate and keep the same physics-derived
  // stopping distance.
  void Tick(const GameState &,
            std::uint64_t now_ms,
            FlightInput &,
            float elapsed_ticks = 1.0F);

  [[nodiscard]] const FlightAutomationStatus &status() const { return status_; }

  [[nodiscard]] static std::optional<std::int16_t>
  ResolveStellar(const GameState &, const std::string &);
  [[nodiscard]] static std::optional<std::int16_t>
  ResolveLinkedSystem(const GameState &, const std::string &);
  // Brakes the ship's velocity *relative to* (reference_vel_x,
  // reference_vel_y). Zero reference velocity is a full stop; a non-zero
  // reference matches a moving target (boarding).
  static void Stop(const PlayerShip &,
                   float turn_rate_deg,
                   FlightInput &,
                   float elapsed_ticks = 1.0F,
                   float reference_vel_x = 0.0F,
                   float reference_vel_y = 0.0F);
  static void MoveAwayFrom(const PlayerShip &,
                           float x,
                           float y,
                           float turn_rate_deg,
                           FlightInput &,
                           float elapsed_ticks = 1.0F);
  // General "arrive and settle" maneuver for a target point that may itself be
  // moving: approaches (x, y), brakes on relative velocity, and settles within
  // `radius` at the target's velocity. Landing passes a stationary stellar;
  // boarding passes a ship's position and velocity. Only FlightInput is
  // written.
  static void MoveToAndStopAt(const PlayerShip &,
                              float x,
                              float y,
                              float radius,
                              float turn_rate_deg,
                              float thrust,
                              FlightInput &,
                              float elapsed_ticks = 1.0F,
                              float target_vel_x = 0.0F,
                              float target_vel_y = 0.0F);

private:
  void Fail(std::string);
  void Tap(bool FlightInput::*field, FlightInput &);
  FlightAutomationStatus status_;
  std::int16_t target_id_ = -1;
  std::uint64_t deadline_ms_ = 0;
  float landing_envelope_axis_range_ = -1.0F;
  bool tap_release_ = false;
};
} // namespace game
