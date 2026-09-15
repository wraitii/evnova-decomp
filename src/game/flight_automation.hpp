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
  bool LandAt(const GameState &,
              std::string,
              std::uint64_t now_ms,
              std::uint64_t timeout_ms = 180000);
  bool JumpTo(const GameState &,
              std::string,
              std::uint64_t now_ms,
              std::uint64_t timeout_ms = 180000);
  void Cancel();
  void ObservedDocked();
  void Tick(const GameState &, std::uint64_t now_ms, FlightInput &);

  [[nodiscard]] const FlightAutomationStatus &status() const { return status_; }

  [[nodiscard]] static std::optional<std::int16_t>
  ResolveStellar(const GameState &, const std::string &);
  [[nodiscard]] static std::optional<std::int16_t>
  ResolveLinkedSystem(const GameState &, const std::string &);
  static void Stop(const PlayerShip &, float turn_rate_deg, FlightInput &);
  static void MoveAwayFrom(
      const PlayerShip &, float x, float y, float turn_rate_deg, FlightInput &);
  static void MoveToAndStopAt(const PlayerShip &,
                              float x,
                              float y,
                              float radius,
                              float turn_rate_deg,
                              float thrust,
                              FlightInput &);

private:
  void Fail(std::string);
  void Tap(bool FlightInput::*field, FlightInput &);
  FlightAutomationStatus status_;
  std::int16_t target_id_ = -1;
  std::uint64_t deadline_ms_ = 0;
  bool tap_release_ = false;
};
} // namespace game
