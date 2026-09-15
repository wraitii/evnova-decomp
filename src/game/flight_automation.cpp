#include "flight_automation.hpp"

#include "travel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace game {
namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kLandingRadius = 220.0F;
constexpr float kSettledSpeed = 0.30F;

std::string Fold(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
  return value;
}

float Bearing(float dx, float dy) { return std::atan2(dx, -dy); }

void Face(float heading,
          float desired,
          float turn_rate_deg,
          FlightInput &input) {
  const float tolerance = std::max(1.0F, turn_rate_deg) * kPi / 180.0F;
  const float delta = std::remainder(desired - heading, 2.0F * kPi);
  if (delta < -tolerance)
    input.turn_left = true;
  else if (delta > tolerance)
    input.turn_right = true;
}
} // namespace

std::optional<std::int16_t>
FlightAutomationController::ResolveStellar(const GameState &state,
                                           const std::string &name) {
  const System *system = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (!system)
    return std::nullopt;
  const std::string folded = Fold(name);
  std::optional<std::int16_t> found;
  for (const std::int16_t id : system->nav_defs) {
    const Stellar *stellar = state.scenario.Stellar(id);
    if (!stellar || Fold(stellar->name) != folded)
      continue;
    if (found && *found != id)
      return std::nullopt;
    found = id;
  }
  return found;
}

std::optional<std::int16_t>
FlightAutomationController::ResolveLinkedSystem(const GameState &state,
                                                const std::string &name) {
  const System *system = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (!system)
    return std::nullopt;
  const std::string folded = Fold(name);
  std::optional<std::int16_t> found;
  for (const std::int16_t resource_id : system->links) {
    const System *candidate = state.scenario.System(resource_id);
    if (!candidate || Fold(candidate->name) != folded)
      continue;
    const auto index = static_cast<std::int16_t>(resource_id - 0x80);
    if (index == state.player.current_system_id || (found && *found != index))
      return std::nullopt;
    found = index;
  }
  return found;
}

bool FlightAutomationController::LandAt(const GameState &state,
                                        std::string name,
                                        std::uint64_t now_ms,
                                        std::uint64_t timeout_ms) {
  const auto id = ResolveStellar(state, name);
  if (!id) {
    Fail("missing or ambiguous current-system stellar");
    return false;
  }
  status_ = {FlightAutomationGoal::kLandAt,
             FlightAutomationPhase::kSelect,
             std::move(name),
             {}};
  target_id_ = *id;
  deadline_ms_ = now_ms + timeout_ms;
  tap_release_ = false;
  return true;
}

bool FlightAutomationController::JumpTo(const GameState &state,
                                        std::string name,
                                        std::uint64_t now_ms,
                                        std::uint64_t timeout_ms) {
  const auto id = ResolveLinkedSystem(state, name);
  if (!id) {
    Fail("missing, ambiguous, current, or non-adjacent system");
    return false;
  }
  status_ = {FlightAutomationGoal::kJumpTo,
             FlightAutomationPhase::kSelect,
             std::move(name),
             {}};
  target_id_ = *id;
  deadline_ms_ = now_ms + timeout_ms;
  tap_release_ = false;
  return true;
}

void FlightAutomationController::Cancel() {
  status_.phase = FlightAutomationPhase::kCancelled;
  status_.detail = "cancelled";
  target_id_ = -1;
  tap_release_ = false;
}

void FlightAutomationController::ObservedDocked() {
  if (status_.goal == FlightAutomationGoal::kLandAt &&
      status_.phase == FlightAutomationPhase::kEngage) {
    status_.phase = FlightAutomationPhase::kComplete;
    status_.detail = "Spaceport observed";
  }
}

void FlightAutomationController::Fail(std::string detail) {
  status_.phase = FlightAutomationPhase::kFailed;
  status_.detail = std::move(detail);
  target_id_ = -1;
}

void FlightAutomationController::Tap(bool FlightInput::*field,
                                     FlightInput &input) {
  if (tap_release_) {
    tap_release_ = false;
    return;
  }
  input.*field = true;
  tap_release_ = true;
}

void FlightAutomationController::Stop(const PlayerShip &ship,
                                      float turn_rate_deg,
                                      FlightInput &input) {
  if (std::hypot(ship.vel_x, ship.vel_y) <= kSettledSpeed)
    return;
  const float reverse = Bearing(ship.vel_x, ship.vel_y) + kPi;
  Face(ship.heading, reverse, turn_rate_deg, input);
  const float delta =
      std::abs(std::remainder(reverse - ship.heading, 2.0F * kPi));
  if (delta <= (turn_rate_deg + 2.0F) * kPi / 180.0F)
    input.thrust = true;
}

void FlightAutomationController::MoveAwayFrom(const PlayerShip &ship,
                                              float x,
                                              float y,
                                              float turn_rate_deg,
                                              FlightInput &input) {
  float dx = ship.pos_x - x;
  float dy = ship.pos_y - y;
  // A ship exactly at the reference point has no radial bearing. Preserve its
  // current heading so the maneuver still makes deterministic progress.
  const float desired =
      (dx == 0.0F && dy == 0.0F) ? ship.heading : Bearing(dx, dy);
  Face(ship.heading, desired, turn_rate_deg, input);
  const float delta =
      std::abs(std::remainder(desired - ship.heading, 2.0F * kPi));
  if (delta <= (turn_rate_deg + 5.0F) * kPi / 180.0F)
    input.thrust = true;
}

void FlightAutomationController::MoveToAndStopAt(const PlayerShip &ship,
                                                 float x,
                                                 float y,
                                                 float radius,
                                                 float turn_rate_deg,
                                                 float thrust,
                                                 FlightInput &input) {
  const float dx = x - ship.pos_x;
  const float dy = y - ship.pos_y;
  const float distance = std::hypot(dx, dy);
  const float speed = std::hypot(ship.vel_x, ship.vel_y);
  const float stopping =
      thrust > 0.0F ? speed * speed / (2.0F * thrust) : speed * 30.0F;
  const float turn_distance = speed * 180.0F / std::max(1.0F, turn_rate_deg);
  if (distance <= radius || distance - radius <= stopping + turn_distance) {
    Stop(ship, turn_rate_deg, input);
    return;
  }
  const float desired = Bearing(dx, dy);
  Face(ship.heading, desired, turn_rate_deg, input);
  const float delta =
      std::abs(std::remainder(desired - ship.heading, 2.0F * kPi));
  if (delta <= (turn_rate_deg + 5.0F) * kPi / 180.0F)
    input.thrust = true;
}

void FlightAutomationController::Tick(const GameState &state,
                                      std::uint64_t now_ms,
                                      FlightInput &input) {
  if (status_.phase == FlightAutomationPhase::kIdle ||
      status_.phase == FlightAutomationPhase::kComplete ||
      status_.phase == FlightAutomationPhase::kFailed ||
      status_.phase == FlightAutomationPhase::kCancelled)
    return;
  if (state.game_over_pending || !state.player.is_active) {
    Fail("player died or left flight");
    return;
  }
  if (now_ms > deadline_ms_) {
    Fail("simulation-time deadline exceeded");
    return;
  }
  if (status_.goal == FlightAutomationGoal::kJumpTo) {
    if (state.player.current_system_id == target_id_ &&
        !state.travel.engaging) {
      status_.phase = FlightAutomationPhase::kComplete;
      return;
    }
    if (state.travel.engaging) {
      status_.phase = FlightAutomationPhase::kWaiting;
      return;
    }
    if (!state.travel.hyperspace_mode) {
      Tap(&FlightInput::hyperspace_mode, input);
      return;
    }
    if (state.travel.destination_system_id != target_id_) {
      Tap(&FlightInput::cycle_destination_next, input);
      return;
    }
    if (!NovaTravel_PlayerInJumpRange(state)) {
      status_.phase = FlightAutomationPhase::kMove;
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
      const float turn = cls ? std::round(cls->turn_rate * 0.1F) : 1.0F;
      MoveAwayFrom(state.player, 0.0F, 0.0F, turn, input);
      return;
    }
    status_.phase = FlightAutomationPhase::kEngage;
    Tap(&FlightInput::travel, input);
    return;
  }
  if (ResolveStellar(state, status_.target) != target_id_) {
    Fail("landing destination was invalidated");
    return;
  }
  if (state.travel.selected_stellar_id != target_id_) {
    status_.phase = FlightAutomationPhase::kSelect;
    Tap(&FlightInput::cycle_target_next, input);
    return;
  }
  const Stellar *stellar = state.scenario.Stellar(target_id_);
  if (!stellar) {
    Fail("landing destination disappeared");
    return;
  }
  const ShipClass *cls = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  const float turn = cls ? std::round(cls->turn_rate * 0.1F) : 1.0F;
  const float thrust = cls ? cls->accel / 10000.0F * 2.0F : 0.01F;
  const float distance =
      std::hypot(static_cast<float>(stellar->pos_x) - state.player.pos_x,
                 static_cast<float>(stellar->pos_y) - state.player.pos_y);
  if (distance <= kLandingRadius &&
      std::hypot(state.player.vel_x, state.player.vel_y) <= kSettledSpeed &&
      state.travel.engage_timer >= 0x2ee) {
    status_.phase = FlightAutomationPhase::kEngage;
    Tap(&FlightInput::land, input);
    return;
  }
  status_.phase = FlightAutomationPhase::kMove;
  MoveToAndStopAt(state.player,
                  static_cast<float>(stellar->pos_x),
                  static_cast<float>(stellar->pos_y),
                  kLandingRadius,
                  turn,
                  thrust,
                  input);
}
} // namespace game
