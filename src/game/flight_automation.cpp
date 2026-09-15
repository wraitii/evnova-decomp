#include "flight_automation.hpp"

#include "ship_ai.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace game {
namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kDegToRad = kPi / 180.0F;
// Conservative fallback when the caller cannot resolve a target sprite: the
// arrival gate itself falls back to 75 (0x4b) but the un-prepared case can
// still land, so 100 keeps the stop point inside every ordinary envelope.
constexpr float kDefaultLandingEnvelope = 100.0F;
// The normal-arrival gate rejects per-axis velocity above 0.75; brake to a
// margin below it so the one-frame tap is not lost to a transient overshoot.
constexpr float kLandingSpeedGate = 0.55F;
constexpr float kSettledSpeed = 0.30F;
// Extra alignment slack (degrees) beyond one frame of turn before committing
// thrust; keeps a small misalignment from stalling the approach.
constexpr float kAlignSlackDeg = 5.0F;
// Turn-radius speed limit. A pure-seeking ship with turn rate omega orbiting a
// target at distance d is stable only while v <= omega * d; without this cap a
// slow-turning hull that misses the target keeps circling it forever. Half the
// theoretical limit keeps a comfortable margin so the nose can keep up.
constexpr float kTurnRadiusSpeedFraction = 0.5F;
// Velocity-error deadband (px/tick): below this the desired velocity is already
// matched, so the ship coasts instead of chattering thrust on and off.
constexpr float kArriveVelocityDeadband = 0.05F;
// kDestroy: approach while beyond this fraction of the best weapon reach, then
// hold station inside it and keep firing. Leaves a margin so a target drifting
// outward still stays in range rather than straddling the reach edge.
constexpr float kDestroyRangeFraction = 0.9F;
// kDestroy target-cycle budget per relevance half. The backquote cycle does not
// wrap; a full traversal plus the pass that clears the selection to "none" is
// at most kMaxShips+2 presses, so that bound cannot skip a matching ship.
constexpr int kCycleTapsPerPass = static_cast<int>(GameState::kMaxShips) + 2;

std::string Fold(std::string value);

// A destroy target is identified by its class display name / instance
// ship_name (case-insensitive) and/or an explicit ship identifier. The
// identifier matches either the live slot or the ship's instance id so callers
// can use whichever the probe/scenario exposes. `ship_id < 0` disables the id
// arm; an empty `folded` disables the name arm.
bool ShipMatchesDestroyTarget(const GameState &state,
                              const Ship &ship,
                              const std::string &folded,
                              std::int16_t ship_id) {
  if (ship_id >= 0) {
    const std::size_t id_slot = static_cast<std::size_t>(ship_id);
    if (ship.ship_instance_id == ship_id ||
        (state.SlotInRange(id_slot) && &ship == &state.ShipAt(id_slot))) {
      return true;
    }
  }
  if (folded.empty()) {
    return false;
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls != nullptr && Fold(cls->display_name) == folded) {
    return true;
  }
  return !ship.ship_name.empty() && Fold(ship.ship_name) == folded;
}

// First active, non-destroyed ship in the player's system matching the destroy
// target. Used both to fail a destroy request fast and to confirm a cycled
// selection landed on the wanted ship.
std::optional<std::int16_t> FindShipSlot(const GameState &state,
                                         const std::string &folded,
                                         std::int16_t ship_id) {
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || NovaAiShip_IsDestroyed(ship) ||
        ship.current_system_id != state.player.current_system_id) {
      continue;
    }
    if (ShipMatchesDestroyTarget(state, ship, folded, ship_id)) {
      return static_cast<std::int16_t>(slot);
    }
  }
  return std::nullopt;
}

// Parses a pure-decimal target string as a ship identifier, or -1 when it is
// not a number. This is the id fallback for callers that pass the id via the
// target string (the explicit `ship_id` argument takes precedence).
std::int16_t ParseShipId(const std::string &text) {
  if (text.empty() || text.size() > 5) {
    return -1;
  }
  std::int32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return -1;
    }
    value = value * 10 + (c - '0');
  }
  return value <= 0x7fff ? static_cast<std::int16_t>(value) : -1;
}

std::string Fold(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
  return value;
}

// Game convention: heading 0 points up, increasing clockwise.
float Bearing(float dx, float dy) { return std::atan2(dx, -dy); }

float AngleDelta(float from, float to) {
  return std::remainder(to - from, 2.0F * kPi);
}

// Turns toward `desired`, treating one frame of turn (`elapsed_ticks` worth) as
// the deadband so accelerated frames do not overshoot the heading.
void Face(float heading,
          float desired,
          float turn_rate_deg,
          float elapsed_ticks,
          FlightInput &input) {
  const float step_deg = turn_rate_deg * elapsed_ticks;
  const float tolerance = std::max(1.0F, step_deg) * kDegToRad;
  const float delta = AngleDelta(heading, desired);
  if (delta < -tolerance)
    input.turn_left = true;
  else if (delta > tolerance)
    input.turn_right = true;
}

// Pick the player's primary bank to lead with. The primary-fire loop fires
// every non-secondary bank, so with mixed weapons there is no single true
// intercept; prefer the first armed, ready, lead-capable projectile bank
// (mode -1/4/6..9, the Ship_AimWeaponPredictive gate) and otherwise fall back
// to the first armed ready primary bank, for which the aim helper returns the
// straight bearing a beam/guided weapon wants. -1 when no primary bank is
// ready. The 100-stride player bank layout matches GameState (see weapon.cpp
// BankAmmo) and this is automation-only selection, not gameplay state.
std::int16_t LeadWeaponBank(const GameState &state, const Ship &ship) {
  constexpr std::size_t kBankStride = 100;
  std::int16_t first_primary = -1;
  for (std::int16_t bank = 0; bank < 0x100; ++bank) {
    if (state.weapon_bank_ammo[static_cast<std::size_t>(bank) * kBankStride] <=
        0) {
      continue;
    }
    const Weapon *w =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (w == nullptr || (w->flags & 0x0002U) != 0U ||
        !NovaWeapon_CanFireWeaponBank(state, ship, bank)) {
      continue;
    }
    if (first_primary < 0) {
      first_primary = bank;
    }
    const std::int16_t mode = w->weapon_mode_code;
    if (mode == -1 || mode == 4 || (mode >= 6 && mode <= 9)) {
      return bank;
    }
  }
  return first_primary;
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
                                        std::uint64_t timeout_ms,
                                        float landing_envelope_axis_range) {
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
  landing_envelope_axis_range_ = landing_envelope_axis_range;
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
  landing_envelope_axis_range_ = -1.0F;
  tap_release_ = false;
  return true;
}

bool FlightAutomationController::DestroyShip(const GameState &state,
                                             std::string name,
                                             std::uint64_t now_ms,
                                             std::uint64_t timeout_ms,
                                             std::int16_t ship_id) {
  status_ = {FlightAutomationGoal::kDestroy,
             FlightAutomationPhase::kSelect,
             std::move(name),
             {}};
  target_id_ = -1;
  target_ship_id_ = ship_id;
  if (target_ship_id_ < 0) {
    target_ship_id_ = ParseShipId(status_.target);
  }
  deadline_ms_ = now_ms + timeout_ms;
  landing_envelope_axis_range_ = -1.0F;
  tap_release_ = false;
  cycle_pass_ = 0;
  cycle_taps_left_ = kCycleTapsPerPass;
  // Fail fast when nothing in the system matches: cycling could only exhaust
  // both halves and time out.
  if (!FindShipSlot(state, Fold(status_.target), target_ship_id_)) {
    Fail("no ship matching target in system");
    return false;
  }
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
                                      FlightInput &input,
                                      float elapsed_ticks,
                                      float reference_vel_x,
                                      float reference_vel_y) {
  // Brake the velocity RELATIVE to the reference. For a stationary reference
  // this is a full stop; for a moving boarding target it matches its velocity.
  const float rel_vel_x = ship.vel_x - reference_vel_x;
  const float rel_vel_y = ship.vel_y - reference_vel_y;
  if (std::hypot(rel_vel_x, rel_vel_y) <= kSettledSpeed)
    return;
  // Point the nose opposite the relative velocity and burn: the ship coasts
  // through the turnaround, then the inverse thrust decays the relative
  // velocity. Its bearing is unchanged by the deceleration, so the target
  // stays valid for the whole maneuver.
  const float reverse = Bearing(rel_vel_x, rel_vel_y) + kPi;
  Face(ship.heading, reverse, turn_rate_deg, elapsed_ticks, input);
  const float delta = std::abs(AngleDelta(ship.heading, reverse));
  const float align_deg = turn_rate_deg * elapsed_ticks + 2.0F;
  if (delta <= align_deg * kDegToRad)
    input.thrust = true;
}

void FlightAutomationController::MoveAwayFrom(const PlayerShip &ship,
                                              float x,
                                              float y,
                                              float turn_rate_deg,
                                              FlightInput &input,
                                              float elapsed_ticks) {
  float dx = ship.pos_x - x;
  float dy = ship.pos_y - y;
  // A ship exactly at the reference point has no radial bearing. Preserve its
  // current heading so the maneuver still makes deterministic progress.
  const float desired =
      (dx == 0.0F && dy == 0.0F) ? ship.heading : Bearing(dx, dy);
  Face(ship.heading, desired, turn_rate_deg, elapsed_ticks, input);
  const float delta = std::abs(AngleDelta(ship.heading, desired));
  const float align_deg = turn_rate_deg * elapsed_ticks + kAlignSlackDeg;
  if (delta <= align_deg * kDegToRad)
    input.thrust = true;
}

void FlightAutomationController::MoveToAndStopAt(const PlayerShip &ship,
                                                 float x,
                                                 float y,
                                                 float radius,
                                                 float turn_rate_deg,
                                                 float thrust,
                                                 FlightInput &input,
                                                 float elapsed_ticks,
                                                 float target_vel_x,
                                                 float target_vel_y) {
  const float dx = x - ship.pos_x;
  const float dy = y - ship.pos_y;
  const float distance = std::hypot(dx, dy);
  // All control is in the target's frame so a moving target (boarding) settles
  // with matched velocity instead of a full stop.
  const float rel_vel_x = ship.vel_x - target_vel_x;
  const float rel_vel_y = ship.vel_y - target_vel_y;
  // `radius` is the arrival tolerance, not a standoff: the ship drives to the
  // point itself and this branch only decides when it is close enough to stop.
  if (distance <= radius) {
    Stop(ship, turn_rate_deg, input, elapsed_ticks, target_vel_x, target_vel_y);
    return;
  }
  const float omega = std::max(1.0F, turn_rate_deg) * kDegToRad;
  const float turn_time_ticks = kPi / omega;
  // Speed whose 180-degree turnaround drift plus reverse-thrust stop exactly
  // fits the distance to the point: speed * pi/omega + speed^2/(2*thrust).
  float speed_cap = 0.0F;
  if (thrust > 0.0F) {
    speed_cap = thrust * (-turn_time_ticks +
                          std::sqrt(turn_time_ticks * turn_time_ticks +
                                    2.0F * distance / thrust));
  } else {
    speed_cap = distance / turn_time_ticks;
  }
  // Follow a moving target by leading it by the estimated closing time.
  float aim_x = x;
  float aim_y = y;
  if (speed_cap > kArriveVelocityDeadband) {
    const float lead_ticks = std::min(distance / speed_cap, 120.0F);
    aim_x += target_vel_x * lead_ticks;
    aim_y += target_vel_y * lead_ticks;
  }
  const float adx = aim_x - ship.pos_x;
  const float ady = aim_y - ship.pos_y;
  const float aim_distance = std::hypot(adx, ady);
  // Do not outrun the turn radius: a slow-turning ship must slow down to curve
  // onto the target instead of orbiting it.
  speed_cap =
      std::min(speed_cap, kTurnRadiusSpeedFraction * omega * aim_distance);
  // Velocity-error control: steer the relative velocity toward the desired
  // velocity (toward the aim point at the safe speed) instead of phase-
  // switching between "face target" and "face reverse velocity". The error
  // automatically encodes acceleration, braking, and lateral correction, so a
  // slow hull tightens its turn as it slows.
  const float dir_x = aim_distance > 1e-4F ? adx / aim_distance : 0.0F;
  const float dir_y = aim_distance > 1e-4F ? ady / aim_distance : 0.0F;
  const float err_x = dir_x * speed_cap - rel_vel_x;
  const float err_y = dir_y * speed_cap - rel_vel_y;
  if (std::hypot(err_x, err_y) <= kArriveVelocityDeadband)
    return;
  const float desired = Bearing(err_x, err_y);
  Face(ship.heading, desired, turn_rate_deg, elapsed_ticks, input);
  const float delta = std::abs(AngleDelta(ship.heading, desired));
  const float align_deg = turn_rate_deg * elapsed_ticks + kAlignSlackDeg;
  if (delta <= align_deg * kDegToRad)
    input.thrust = true;
}

void FlightAutomationController::Tick(const GameState &state,
                                      std::uint64_t now_ms,
                                      FlightInput &input,
                                      float elapsed_ticks) {
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
  if (status_.goal == FlightAutomationGoal::kDestroy) {
    TickDestroy(state, input, elapsed_ticks);
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
      MoveAwayFrom(state.player, 0.0F, 0.0F, turn, input, elapsed_ticks);
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
  const float envelope = landing_envelope_axis_range_ > 0.0F
                             ? landing_envelope_axis_range_
                             : kDefaultLandingEnvelope;
  const float dx = static_cast<float>(stellar->pos_x) - state.player.pos_x;
  const float dy = static_cast<float>(stellar->pos_y) - state.player.pos_y;
  // Match the original arrival gate exactly (PlayerTick_LandCommandDispatch):
  // an axis-aligned envelope and a per-axis velocity limit, not a circular
  // radius. Tapping with the old fixed 220 px radius missed small-sprite
  // stellars whose real envelope is as small as 75 px.
  const bool inside_envelope =
      std::abs(dx) < envelope && std::abs(dy) < envelope;
  const bool slow_enough = std::abs(state.player.vel_x) <= kLandingSpeedGate &&
                           std::abs(state.player.vel_y) <= kLandingSpeedGate;
  if (inside_envelope && slow_enough && state.travel.engage_timer >= 0x2ee) {
    status_.phase = FlightAutomationPhase::kEngage;
    Tap(&FlightInput::land, input);
    return;
  }
  status_.phase = FlightAutomationPhase::kMove;
  // Arrive within half the gate so the tap has margin against drift; the
  // maneuver drives to the stellar and stops once inside that tolerance.
  MoveToAndStopAt(state.player,
                  static_cast<float>(stellar->pos_x),
                  static_cast<float>(stellar->pos_y),
                  envelope * 0.5F,
                  turn,
                  thrust,
                  input,
                  elapsed_ticks);
}

void FlightAutomationController::TickDestroy(const GameState &state,
                                             FlightInput &input,
                                             float elapsed_ticks) {
  const std::string folded = Fold(status_.target);
  if (target_id_ < 0) {
    // Lock immediately when the player already holds a matching target; this
    // also catches the frame after a successful cycle tap.
    const std::int16_t current = state.player.primary_target_ship_slot;
    if (current >= 0 && state.SlotInRange(static_cast<std::size_t>(current))) {
      const Ship &held = state.ShipAt(static_cast<std::size_t>(current));
      if (ShipMatchesDestroyTarget(state, held, folded, target_ship_id_)) {
        // Fall through so the lock frame already steers and fires.
        target_id_ = current;
      }
    }
  }
  if (target_id_ < 0) {
    // Drive the backquote cycle one press at a time: first the combat-relevant
    // half (include-combat modifier set), then the other half.
    if (cycle_pass_ > 1) {
      Fail("cycled every eligible ship without finding the target");
      return;
    }
    if (cycle_taps_left_ <= 0) {
      ++cycle_pass_;
      cycle_taps_left_ = kCycleTapsPerPass;
      return;
    }
    input.cycle_ship_include_combat = (cycle_pass_ == 0);
    const bool will_press = !tap_release_;
    Tap(&FlightInput::cycle_ship_target_next, input);
    if (will_press) {
      --cycle_taps_left_;
    }
    status_.phase = FlightAutomationPhase::kSelect;
    return;
  }

  // Locked: validate every frame so a destroyed or departed target ends the
  // goal instead of leaving the controller firing at empty space.
  if (!state.SlotInRange(static_cast<std::size_t>(target_id_))) {
    status_.phase = FlightAutomationPhase::kComplete;
    status_.detail = "target gone";
    return;
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_id_));
  if (!target.is_active || NovaAiShip_IsDestroyed(target)) {
    status_.phase = FlightAutomationPhase::kComplete;
    status_.detail = "target destroyed";
    return;
  }
  if (target.current_system_id != state.player.current_system_id ||
      target.ai_state_code == 0x15) {
    // AI state 0x15 is the jump-out approach; the target is leaving.
    Fail("target left the system");
    return;
  }
  // Face and fire are re-derived from the live geometry each frame, so weapon
  // pushback simply moves the aim point and the hull keeps tracking.
  const ShipClass *cls = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  const float turn = cls ? std::round(cls->turn_rate * 0.1F) : 1.0F;
  const float dx = target.pos_x - state.player.pos_x;
  const float dy = target.pos_y - state.player.pos_y;
  // Lead a crossing target: fire commands shoot along the current heading, so
  // aim at the predicted intercept instead of the target's stale position.
  // NovaAi_AimWeaponPredictive (Ghidra 0x0043b740) handles the flight-time
  // math and falls back to the straight bearing for beams/guided weapons.
  float desired = Bearing(dx, dy);
  const std::int16_t lead_bank = LeadWeaponBank(state, state.player);
  if (lead_bank >= 0) {
    const std::int16_t lead_deg =
        NovaAi_AimWeaponPredictive(state, state.player, target, lead_bank);
    desired = static_cast<float>(lead_deg) * kDegToRad;
  }
  Face(state.player.heading, desired, turn, elapsed_ticks, input);
  const float distance = std::hypot(dx, dy);
  const int reach = NovaWeapon_GetShipMaxWeaponRange(state, state.player);
  if (reach > 0 && distance <= static_cast<float>(reach)) {
    // Eager fire: the primary banks fire along the current heading on the
    // loop's own weapon-command pass.
    input.fire = true;
  }
  // Close the distance until inside the firing envelope, but only while the
  // nose is already on the target; otherwise turning (not thrusting) is the
  // right correction and thrusting would drift the aim.
  const float align_deg = turn * elapsed_ticks + kAlignSlackDeg;
  const bool aligned = std::abs(AngleDelta(state.player.heading, desired)) <=
                       align_deg * kDegToRad;
  const bool need_close = reach <= 0 || distance > static_cast<float>(reach) *
                                                       kDestroyRangeFraction;
  if (need_close && aligned) {
    input.thrust = true;
  }
  status_.phase = FlightAutomationPhase::kEngage;
}
} // namespace game
