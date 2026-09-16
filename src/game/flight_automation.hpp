#pragma once

#include "../sdl_platform.hpp"
#include "game_state.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace game {
enum class FlightAutomationGoal { kNone, kLandAt, kJumpTo, kDestroy };
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
  // High-level "destroy a ship" routine. `target` matches an active ship in
  // the player's system by class display name (case-insensitive), falling back
  // to its instance ship_name; a pure-decimal target is also read as a ship
  // identifier when no name matches. `ship_id` (when >= 0) is an explicit
  // identifier that matches either a live ship slot or its instance id; it
  // takes precedence over the numeric-string fallback but not over a name.
  // Acquisition cycles the player's primary ship target with the backquote
  // hotkey (input.cycle_ship_target_next), first through the combat-relevant
  // half and then the non-relevant half, until it lands on a matching ship.
  // While locked it aims at the predicted intercept of the ready primary bank
  // every frame (leading a crossing target) and fires eagerly whenever the
  // target is within the best weapon reach, so a moving target cannot outrun
  // the projectile. Completes when the locked target is destroyed or gone;
  // fails when no matching ship exists or it leaves the system. `allow_missing`
  // relaxes the no-match failure: when the target is already gone (for example
  // a mission ship another AI destroyed first) the goal completes instead.
  // Probe scenarios set it for targets that other ships can destroy.
  bool DestroyShip(const GameState &,
                   std::string target,
                   std::uint64_t now_ms,
                   std::uint64_t timeout_ms = 180000,
                   std::int16_t ship_id = -1,
                   bool allow_missing = false);
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
  // kDestroy body: acquires via the target-cycle hotkey, then leads/fires.
  void TickDestroy(const GameState &, FlightInput &, float elapsed_ticks);
  FlightAutomationStatus status_;
  std::int16_t target_id_ = -1;
  // Explicit/numeric ship identifier for kDestroy; -1 when the name is the
  // only identity.
  std::int16_t target_ship_id_ = -1;
  // kDestroy: treat a vanished target as success rather than failure. Set by
  // DestroyShip for probe scenarios whose target other ships may destroy first.
  bool destroy_allow_missing_ = false;
  std::uint64_t deadline_ms_ = 0;
  float landing_envelope_axis_range_ = -1.0F;
  bool tap_release_ = false;
  // kDestroy acquisition cursor. `cycle_pass_` 0 selects the combat-relevant
  // cycle half (include-combat modifier held), 1 the non-relevant half; 2 means
  // both halves were exhausted without a match. `cycle_taps_left_` bounds each
  // half so a no-wrap cycle cannot spin forever.
  int cycle_pass_ = 0;
  int cycle_taps_left_ = 0;
};
} // namespace game
