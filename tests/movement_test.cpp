#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/outfit.hpp"
#include "game/ship_ai.hpp"
#include "game/spaceflight.hpp"
#include "game/spaceflight_internal.hpp"

#include <numbers>

namespace {

game::ShipClass TestShipClass() {
  game::ShipClass ship_class;
  ship_class.accel = 500.0F;
  ship_class.speed = 400.0F;
  ship_class.turn_rate = 40.0F;
  return ship_class;
}

} // namespace

TEST_CASE("flight integration uses elapsed original-cadence ticks") {
  game::PlayerShip ship;
  FlightInput input;
  input.thrust = true;

  const auto stats =
      game::NovaPlayer_IntegrateMovement(ship, input, TestShipClass(), 0.5F);

  CHECK(stats.max_speed_px_per_tick == Catch::Approx(4.0F));
  CHECK(stats.thrust_px_per_tick2 == Catch::Approx(0.1F));
  CHECK(ship.vel_y == Catch::Approx(-0.05F));
  CHECK(ship.pos_y == Catch::Approx(-0.025F));
}

TEST_CASE("reverse command turns the ship but preserves its velocity") {
  game::PlayerShip ship;
  ship.vel_y = -2.0F;
  FlightInput input;
  input.reverse = true;

  (void)game::NovaPlayer_IntegrateMovement(ship, input, TestShipClass(), 1.0F);

  CHECK(ship.heading ==
        Catch::Approx(4.0F * std::numbers::pi_v<float> / 180.0F));
  CHECK(ship.vel_x == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(-2.0F));
  CHECK(ship.pos_y == Catch::Approx(-2.0F));
  CHECK_FALSE(ship.engine_thrust);
}

TEST_CASE("reverse does not combine with manual turn input") {
  game::PlayerShip ship;
  ship.vel_y = -2.0F;
  FlightInput input;
  input.reverse = true;
  input.turn_right = true;

  (void)game::NovaPlayer_IntegrateMovement(ship, input, TestShipClass(), 1.0F);

  CHECK(ship.heading ==
        Catch::Approx(4.0F * std::numbers::pi_v<float> / 180.0F));
}

TEST_CASE(
    "reverse stops reporting a turn once aligned with the reverse heading") {
  // Regression: the reverse arm must leave turn_dir = 0 once the hull faces
  // the reverse of its velocity; otherwise TickPlayerTurnBankAnimation keeps
  // accumulating bank after the turnaround finishes (the original's shared
  // auto-turn continuation, Ghidra 0x00450086, leaves sVar8 = 0 there).
  game::PlayerShip ship;
  ship.vel_y = -2.0F;                       // moving up -> reverse heading 180
  ship.heading = std::numbers::pi_v<float>; // already facing the reverse
  FlightInput input;
  input.reverse = true;

  const auto stats =
      game::NovaPlayer_IntegrateMovement(ship, input, TestShipClass(), 1.0F);

  CHECK(stats.turn_dir == 0);
  CHECK(ship.heading == Catch::Approx(std::numbers::pi_v<float>));
  CHECK(ship.vel_y == Catch::Approx(-2.0F));
}

TEST_CASE("inertialess reverse decays scalar speed instead of turning") {
  // Ghidra 0x0044ffa8: for inertialess ships the reverse command retro-decays
  // the maintained scalar speed (+0x48) by the effective thrust step and
  // floors it at zero; it does not run the non-inertialess opposite-velocity
  // turnaround, so the heading and turn_dir are untouched.
  game::PlayerShip ship;
  ship.heading = 0.0F;
  ship.vel_x = 0.0F;
  ship.vel_y = -2.0F;
  ship.speed = 2.0F;
  FlightInput input;
  input.reverse = true;
  game::PlayerMovementOptions opts;
  opts.inertialess = true;

  const auto stats = game::NovaPlayer_IntegrateMovement(
      ship, input, TestShipClass(), 1.0F, opts);

  CHECK(ship.heading == Catch::Approx(0.0F));
  CHECK(stats.turn_dir == 0);
  // 500/10000*2 = 0.1 px/tick^2 thrust step for the test class.
  CHECK(ship.speed == Catch::Approx(1.9F));
  // The steering block follows heading * speed, so the velocity comes down
  // to the reduced scalar (0, -1.9) without a turn.
  CHECK(ship.vel_x == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(-1.9F));
}

TEST_CASE("inertialess reverse floors at zero and never goes negative") {
  game::PlayerShip ship;
  ship.vel_y = -0.05F;
  ship.speed = 0.05F;
  FlightInput input;
  input.reverse = true;
  game::PlayerMovementOptions opts;
  opts.inertialess = true;

  (void)game::NovaPlayer_IntegrateMovement(
      ship, input, TestShipClass(), 1.0F, opts);

  CHECK(ship.speed == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(0.0F));
}

TEST_CASE("flight turns at the original rounded effective turn rate") {
  game::PlayerShip ship;
  game::ShipClass ship_class = TestShipClass();
  // Resource maneuver 45 becomes 4.5 degrees/tick, which the original player
  // control path rounds before applying the frame-time multiplier.
  ship_class.turn_rate = 45.0F;
  FlightInput input;
  input.turn_right = true;

  (void)game::NovaPlayer_IntegrateMovement(ship, input, ship_class, 1.0F);

  CHECK(ship.heading ==
        Catch::Approx(5.0F * std::numbers::pi_v<float> / 180.0F));
}

// Ghidra Ship_HandlePlayerShipCore 0x0044aa70: keyboard turn, reverse and
// thrust are gated on !Ship_IsShipDisabled(player) (the parent's local_251
// latch). The face-target auto-turn continuation is not gated.
TEST_CASE("disabled player ignores keyboard turn, thrust and reverse") {
  game::PlayerMovementOptions opts;
  opts.fire_restricted = true;

  game::PlayerShip turning;
  FlightInput turn_input;
  turn_input.turn_right = true;
  (void)game::NovaPlayer_IntegrateMovement(
      turning, turn_input, TestShipClass(), 1.0F, opts);
  CHECK(turning.heading == Catch::Approx(0.0F));

  game::PlayerShip thrusting;
  FlightInput thrust_input;
  thrust_input.thrust = true;
  (void)game::NovaPlayer_IntegrateMovement(
      thrusting, thrust_input, TestShipClass(), 1.0F, opts);
  CHECK(thrusting.vel_y == Catch::Approx(0.0F));
  CHECK_FALSE(thrusting.engine_thrust);

  game::PlayerShip reversing;
  reversing.vel_y = -2.0F;
  FlightInput reverse_input;
  reverse_input.reverse = true;
  (void)game::NovaPlayer_IntegrateMovement(
      reversing, reverse_input, TestShipClass(), 1.0F, opts);
  CHECK(reversing.heading == Catch::Approx(0.0F));
  CHECK(reversing.pos_y == Catch::Approx(-2.0F));
}

TEST_CASE("face-target auto-turn still runs while the player is disabled") {
  game::PlayerMovementOptions opts;
  opts.fire_restricted = true;
  opts.face_target_armed = true;
  game::PlayerShip ship;
  ship.ai_desired_heading_deg = 90; // well beyond one 4-degree turn step

  (void)game::NovaPlayer_IntegrateMovement(
      ship, FlightInput{}, TestShipClass(), 1.0F, opts);

  CHECK(ship.heading > 0.0F);
}

// --- NPC ship movement (NovaShip_IntegrateNpcMovement; Ghidra Ship_HandleShip
// movement block) ---

TEST_CASE("npc turns toward its desired heading at the class turn rate") {
  game::GameState state;
  game::Ship ship;
  ship.ai_desired_heading_deg = 90; // heading 90 deg = right/down-clockwise
  // Class turn 40 raw -> 4.0 deg/tick continuous (not integer-rounded like the
  // player keyboard path).
  game::ShipClass cls = TestShipClass();
  cls.turn_rate = 40.0F;

  // Two ticks: 8 degrees toward the desired heading from 0.
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 2.0F);
  CHECK(ship.heading ==
        Catch::Approx(8.0F * std::numbers::pi_v<float> / 180.0F));
}

TEST_CASE("npc player-aggro accumulator decays in normalized simulation time") {
  game::GameState state;
  game::Ship ship;
  const game::ShipClass cls = TestShipClass();
  ship.player_aggro_accumulator = 2.0F;

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 0.5F);
  CHECK(ship.player_aggro_accumulator == Catch::Approx(1.75F));

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 4.0F);
  CHECK(ship.player_aggro_accumulator == Catch::Approx(0.0F));
}

TEST_CASE("npc player-aggro accumulator does not decay below zero") {
  game::GameState state;
  game::Ship ship;
  const game::ShipClass cls = TestShipClass();
  ship.player_aggro_accumulator = -1.0F;

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  CHECK(ship.player_aggro_accumulator == Catch::Approx(-1.0F));
}

TEST_CASE("npc snaps exactly onto the desired heading within one turn step") {
  game::GameState state;
  game::Ship ship;
  ship.ai_desired_heading_deg = -45; // 315
  game::ShipClass cls = TestShipClass();
  cls.turn_rate = 40.0F; // 4 deg/tick

  // Far enough ticks to overshoot the snap window, so it lands exactly.
  for (int i = 0; i < 30; ++i) {
    game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  }
  CHECK(ship.heading ==
        Catch::Approx(315.0F * std::numbers::pi_v<float> / 180.0F));
}

TEST_CASE("npc forward thrust accelerates along the heading") {
  game::GameState state;
  game::Ship ship;
  ship.ai_desired_heading_deg = 0; // heading 0 = up (-y)
  ship.ai_desired_speed = 100.0F;
  // ai_forward_thrust_cmd is the instantaneous per-frame thrust magnitude the
  // AI orders (not the class accel); the step is cmd * ticks.
  ship.ai_forward_thrust_cmd = 10.0F;
  game::ShipClass cls = TestShipClass();
  cls.accel = 500.0F; // -> thrust 0.1 px/tick^2 (cruise cap / speed projection)
  cls.speed = 400.0F; // -> max speed 4.0 px/tick

  // The inherited zero velocity is integrated first; current-frame thrust then
  // installs the velocity used by the following frame.
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  CHECK(ship.vel_y == Catch::Approx(-10.0F)); // polar(0)= up / -y for sin/cos
  CHECK(ship.pos_y == Catch::Approx(0.0F));
}

TEST_CASE("state 2 mode 4 applies the jump ramp directly to position") {
  game::GameState state;
  game::Ship ship;
  game::ShipClass cls = TestShipClass();
  ship.is_active = true;
  ship.ship_instance_id = 1;
  ship.ai_state_code = 2;
  ship.ai_control_mode = 4;
  ship.ai_station_hold_timer = 1.0F;
  ship.ai_desired_heading_deg = 0;
  ship.ai_mode_start_time_ms = 0;
  state.tick_60hz = 200;

  // The original mode-4 arm (Ship_HandleShip, 0x00433050) adds the ramp to
  // position; it does not accumulate the ramp into ordinary velocity. The
  // clock and the duration are both 1/60 s ticks (Stellar_GetJumpSequence
  // Duration60Hz = 364).
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  state.tick_60hz = 233;
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);

  const float first_ramp = 200.0F / 3.64F - 35.0F;
  const float second_ramp = 233.0F / 3.64F - 35.0F;
  CHECK(ship.pos_y == Catch::Approx(-(first_ramp + second_ramp)));
  CHECK(ship.vel_x == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(0.0F));
}

TEST_CASE("disabled and destroyed NPCs do not regenerate") {
  game::GameState state;
  game::ShipClass cls = TestShipClass();
  cls.base_shield = 100;
  cls.base_armor = 100;
  cls.shield_recharge = 5.0F;
  cls.armor_recharge = 5.0F;
  state.scenario.ships.push_back(cls);

  game::Ship disabled;
  disabled.ship_class_id = 0;
  disabled.ship_instance_id = 1;
  disabled.shield_points = 10.0F;
  disabled.armor_points = 20.0F; // below the original 33% threshold
  disabled.vel_y = -10.0F;
  disabled.speed = 10.0F;
  game::NovaShip_IntegrateNpcMovement(state, disabled, cls, 1.0F);
  CHECK(game::NovaAiShip_IsDisabled(state, disabled));
  CHECK(disabled.shield_points == Catch::Approx(10.0F));
  CHECK(disabled.armor_points == Catch::Approx(20.0F));
  // Ship_HandleShip's g_fire_restricted_ship_velocity_damp (0x00575448) is
  // 0.995 per original raw call: a disabled ship drifts to a stop gradually,
  // with one normalized tick representing 1/0.63 raw calls.
  const float damped_speed = 10.0F * std::pow(0.995F, 1.0F / 0.63F);
  CHECK(disabled.vel_y == Catch::Approx(-damped_speed));
  CHECK(disabled.pos_y == Catch::Approx(-damped_speed));
  CHECK(disabled.speed == Catch::Approx(damped_speed));

  game::Ship destroyed = disabled;
  destroyed.shield_points = 10.0F;
  destroyed.armor_points = 0.0F;
  game::NovaShip_IntegrateNpcMovement(state, destroyed, cls, 1.0F);
  CHECK(game::NovaAiShip_IsDestroyed(destroyed));
  CHECK(destroyed.shield_points == Catch::Approx(10.0F));
  CHECK(destroyed.armor_points == Catch::Approx(0.0F));
}

TEST_CASE("disabled NPCs hold their heading instead of turning") {
  game::GameState state;
  game::ShipClass cls = TestShipClass();
  cls.base_armor = 100;
  cls.turn_rate = 40.0F; // 4 deg/tick if the turn arm ran
  state.scenario.ships.push_back(cls);

  game::Ship ship;
  ship.ship_class_id = 0;
  ship.ship_instance_id = 1;
  ship.armor_points = 20.0F; // below max/3 -> disabled
  ship.ai_desired_heading_deg = 90;

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);

  // Ghidra gates the turn/regen block on !Ship_IsShipDisabled; a disabled
  // hull keeps its current heading (and ai_turn_bias_dir stays 0).
  CHECK(game::NovaAiShip_IsDisabled(state, ship));
  CHECK(ship.heading == Catch::Approx(0.0F));
  CHECK(ship.ai_turn_bias_dir == 0);
}

TEST_CASE("npc negative speed uses the physics override") {
  game::GameState state;
  game::Ship ship;
  ship.ai_desired_heading_deg = 0;
  ship.ai_desired_speed = -2.0F;
  ship.ai_forward_thrust_cmd = 1.0F;
  ship.vel_x = 99.0F;
  ship.vel_y = -50.0F;
  game::ShipClass cls = TestShipClass();

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  // Physics override re-imposes abs(desired)=2 along heading 0 (up/-y); old
  // velocity is discarded.
  CHECK(ship.vel_x == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(-2.0F));
  // The reverse path arms a 30..59-tick timer, then this movement tick consumes
  // one normalized reference tick from it.
  CHECK(ship.ai_maneuver_timer_ms >= 29.0F);
  CHECK(ship.ai_maneuver_timer_ms <= 58.0F);
}

TEST_CASE("npc arrival slowdown scales its decay across a slow frame") {
  game::GameState state;
  game::Ship ship;
  ship.ai_desired_heading_deg = 0;
  ship.ai_desired_speed = -30.0F;
  ship.ai_forward_thrust_cmd = -1.165F;
  game::ShipClass cls = TestShipClass();

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 2.0F);

  CHECK(ship.ai_desired_speed == Catch::Approx(-26.301587F));
  CHECK(ship.ai_maneuver_timer_ms <= 0.0F);
}

TEST_CASE("npc arrival installs velocity after integrating the current frame") {
  game::GameState state;
  game::Ship ship;
  ship.heading = 0.0F;
  ship.ai_desired_heading_deg = 0;
  ship.ai_desired_speed = -30.0F;
  ship.ai_forward_thrust_cmd = -1.165F;
  game::ShipClass cls = TestShipClass();

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 0.5F);

  CHECK(ship.pos_x == Catch::Approx(0.0F));
  CHECK(ship.pos_y == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(-30.0F));
}

TEST_CASE("npc inertia-less ships are pinned (vel zeroed, no motion)") {
  game::GameState state;
  game::Ship ship;
  ship.vel_x = 3.0F;
  ship.vel_y = -4.0F;
  game::ShipClass cls = TestShipClass();
  cls.accel = 0.0F;
  cls.speed = 0.0F;

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 10.0F);
  CHECK(ship.vel_x == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(0.0F));
  CHECK(ship.pos_x == Catch::Approx(0.0F));
  CHECK(ship.pos_y == Catch::Approx(0.0F));
}

TEST_CASE("npc coasts without thrust (no forward command)") {
  game::GameState state;
  game::Ship ship;
  // ai_desired_speed == 0 and ai_forward_thrust_cmd == 0: the thrust block is
  // skipped entirely; velocity is preserved and position integrated.
  ship.vel_x = 1.0F;
  ship.vel_y = -2.0F;
  game::ShipClass cls = TestShipClass();

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 3.0F);
  CHECK(ship.vel_x == Catch::Approx(1.0F)); // momentum preserved
  CHECK(ship.vel_y == Catch::Approx(-2.0F));
  CHECK(ship.pos_x == Catch::Approx(3.0F)); // pos += vel * ticks
  CHECK(ship.pos_y == Catch::Approx(-6.0F));
}

TEST_CASE("steer velocity advances the prior velocity toward heading*speed") {
  game::Ship ship;
  ship.heading = 0.0F; // heading 0 = up (-y)
  ship.speed = 4.0F;   // inertialess scalar speed
  // Prior velocity points elsewhere (large positive x) so the steer advances
  // it toward the heading*speed command at a bounded per-axis rate.
  ship.vel_x = 100.0F;
  ship.vel_y = -100.0F;

  // eff_thrust 0.1 * 4.0 turn-scale = 0.4 step per tick.
  game::NovaShip_SteerVelocityTowardShipHeading(ship, 0.1F, 1.0F);
  // New heading*vel = (0,-4). The PRIOR velocity moves toward it by at most
  // 0.4 per axis (clamped, never crossing the command). Starting far away, the
  // result stays within one step of the prior velocity:
  CHECK(ship.vel_x == Catch::Approx(99.6F));  // 100 moved 0.4 toward 0
  CHECK(ship.vel_y == Catch::Approx(-99.6F)); // -100 moved 0.4 toward -4
}

TEST_CASE("inertialess npc keeps a scalar clamped speed and applies it") {
  game::GameState state;
  game::Ship ship;
  // flags_secondary 0x40 marks the class as an inertialess ship.
  game::ShipClass cls = TestShipClass();
  cls.flags_secondary = 0x40;
  ship.ai_desired_heading_deg = 0;
  ship.ai_desired_speed = 100.0F;
  ship.ai_forward_thrust_cmd = 2.0F;
  ship.speed = 1.0F;

  // The original position block consumes the inherited scalar speed first:
  // steering from rest toward heading*1 advances the prior (zero) velocity by
  // the 0.4 bounded step, leaving vel_y at -0.4, and that velocity is
  // integrated. Current-frame thrust then raises the scalar speed from 1 to 3
  // for the following frame.
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  CHECK(ship.speed == Catch::Approx(3.0F));
  CHECK(ship.vel_y == Catch::Approx(-0.4F)); // heading 0 = up, one steer step
  CHECK(ship.pos_y == Catch::Approx(-0.4F));
}

TEST_CASE("inertialess detect excludes ai_control_mode 0x0c") {
  game::Ship ship;
  game::ShipClass cls = TestShipClass();
  cls.flags_secondary = 0x40;
  CHECK(game::NovaShip_IsInertialess(ship, cls));
  ship.ai_control_mode = 0x0c;
  CHECK_FALSE(game::NovaShip_IsInertialess(ship, cls));
}

// --- NPC engine-glow level (Ghidra Ship_HandleShip field_0xc8d4) ---

TEST_CASE("npc full-burn glow ramps toward the 0x20 cap") {
  game::GameState state;
  game::Ship ship;
  // Full burn: thrust command at/above 2x effective thrust (2*0.1 = 0.2).
  ship.ai_forward_thrust_cmd = 0.5F;
  game::ShipClass cls = TestShipClass(); // accel 500 -> eff_thrust 0.1

  // ramps +1/frame from 0 toward 32.
  for (int i = 0; i < 40; ++i) {
    game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  }
  CHECK(ship.engine_glow_level == 0x20);
  // 0x20/24 = 1.333, clamped to the [0,1] alpha range.
  CHECK(ship.engine_glow_intensity == Catch::Approx(1.0F));
}

TEST_CASE("npc low-throttle glow settles at the 0x18 cruise level") {
  game::GameState state;
  game::Ship ship;
  ship.ai_forward_thrust_cmd = 0.05F; // < 2x eff_thrust -> low throttle
  game::ShipClass cls = TestShipClass();

  for (int i = 0; i < 40; ++i) {
    game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  }
  CHECK(ship.engine_glow_level == 0x18);
}

TEST_CASE("npc glow fades to zero when thrust stops") {
  game::GameState state;
  game::Ship ship;
  ship.ai_forward_thrust_cmd = 0.5F; // burn up first
  game::ShipClass cls = TestShipClass();
  for (int i = 0; i < 40; ++i) {
    game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  }
  CHECK(ship.engine_glow_level == 0x20);

  // Stop thrusting: glow decays one unit per 21 ms logical call toward 0.
  ship.ai_forward_thrust_cmd = 0.0F;
  for (int i = 0; i < 40; ++i) {
    game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  }
  CHECK(ship.engine_glow_level == 0);
  CHECK(ship.engine_glow_intensity == Catch::Approx(0.0F));
}

TEST_CASE("npc maneuver timer fades glow only once per logical call") {
  game::GameState state;
  game::Ship ship;
  game::ShipClass cls = TestShipClass();
  ship.engine_glow_level = 10;
  ship.ai_forward_thrust_cmd = 0.5F;
  ship.ai_maneuver_timer_ms = 10.0F;

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 0.63F);

  // Ghidra's glow block takes the single fade label when the maneuver timer
  // suppresses thrust. It does not apply a second timer-specific decrement.
  CHECK(ship.engine_glow_level == 9);
}

TEST_CASE("npc in AI state 0x16 keeps the glow drive instead of fading") {
  game::GameState state;
  game::Ship ship;
  game::ShipClass cls = TestShipClass(); // eff_thrust 0.1
  ship.engine_glow_level = 10;
  ship.ai_state_code = 0x16; // yielding: timer active but the gate still opens
  ship.ai_forward_thrust_cmd = 0.5F;
  ship.ai_maneuver_timer_ms = 10.0F;

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 0.63F);

  // Ghidra 0x00433050 opens the movement/glow block for state 0x16, and the
  // inner fade test requires `timer > 0 && !state0x16`. Full burn adds 1.
  CHECK(ship.engine_glow_level == 11);
}

TEST_CASE("npc engine glow banks fractional time at the original cadence") {
  game::GameState state;
  game::Ship ship;
  game::ShipClass cls = TestShipClass();
  ship.ai_forward_thrust_cmd = 0.5F; // full burn

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 0.31F);
  CHECK(ship.engine_glow_level == 0);
  CHECK(ship.engine_glow_raw_tick_accumulator == Catch::Approx(0.31F));

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 0.32F);
  CHECK(ship.engine_glow_level == 1);
  CHECK(ship.engine_glow_raw_tick_accumulator ==
        Catch::Approx(0.0F).margin(1e-6F));

  // A slow rendered frame replays every whole original call, preserving the
  // integer transition rather than applying one display-rate mutation.
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.26F);
  CHECK(ship.engine_glow_level == 3);
}

TEST_CASE("npc bank glow boost adds two with no 0x18 clamp") {
  game::GameState state;
  game::Ship ship;
  game::ShipClass cls = TestShipClass();
  cls.sprite_behavior_flags = 0x3; // banking (bit 0) + engine-glow (bit 2)
  cls.turn_rate = 10.0F;           // 1 deg/tick, so the target is far away
  ship.engine_glow_level = 23;
  ship.ai_desired_heading_deg = 90;
  ship.ai_forward_thrust_cmd = 0.5F; // full burn (>= 2x eff_thrust 0.2)

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);

  // The turn sets ai_turn_bias_dir; the bank boost adds 2 (23 -> 25, past the
  // 0x18 cruise level) and the full-burn arm then adds 1 -> 26. The original
  // 0x004350ee has no upper clamp.
  CHECK(ship.ai_turn_bias_dir != 0);
  CHECK(ship.engine_glow_level == 26);
}

TEST_CASE("state 2 mode 4 ramps engine glow with its departure step") {
  game::GameState state;
  game::Ship ship;
  game::ShipClass cls = TestShipClass();
  ship.is_active = true;
  ship.ship_instance_id = 1;
  ship.ai_state_code = 2;
  ship.ai_control_mode = 4;
  ship.ai_station_hold_timer = 1.0F;
  ship.ai_desired_heading_deg = 0;
  ship.ai_mode_start_time_ms = 0;
  state.tick_60hz = 200;

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);

  // Mode 4 adds three, then the ordinary zero-thrust glow path fades once.
  CHECK(ship.engine_glow_level == 2);
  CHECK(ship.engine_glow_intensity == Catch::Approx(2.0F / 24.0F));
}

TEST_CASE("npc_out-of-range class ship is deactivated by the guard") {
  game::GameState state;
  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.current_system_id = 0;
  ship.ship_class_id = 0x300; // > 0x2ff -> Ship_HandleShip deactivates
  ship.pos_x = 12.0F;

  game::NovaShip_TickNpcShips(state, 1.0F);
  CHECK_FALSE(ship.is_active);
}

TEST_CASE(
    "npc guard resets drifted slot fields and keeps a valid ship active") {
  game::GameState state;
  state.scenario.ships.emplace_back(); // resource id 0x80, valid (tech 0)
  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.current_system_id = 0;
  ship.ship_class_id = 0;
  ship.armor_points = 10.0F; // alive: destroyed hulls now run the death
                             // presentation (Ship_UpdateVisualState 0x428340)
  // Drifted slot fields out of their legal ranges (Ghidra resets to -1).
  ship.faction_or_government_id = 0x100; // > 0xff
  ship.dude_class_id = 0x200;            // > 0x1ff
  ship.pers_def_slot = 0x400;            // > 0x3ff
  ship.squad_leader_ship_slot = 0x40;    // > 0x3f
  ship.defense_fleet_home_stellar_id =
      0x800;                      // > 0x7ff (resets squad_leader_ship_slot)
  ship.mission_fleet_slot = 0x10; // > 0xf
  ship.primary_target_ship_slot = 0x40; // > 0x3f

  game::NovaShip_TickNpcShips(state, 1.0F);
  CHECK(ship.is_active); // valid class: not deactivated
  CHECK(ship.faction_or_government_id == -1);
  CHECK(ship.dude_class_id == -1);
  CHECK(ship.pers_def_slot == -1);
  CHECK(ship.squad_leader_ship_slot ==
        -1); // reset by both direct check + quirk
  CHECK(ship.mission_fleet_slot == -1);
  CHECK(ship.primary_target_ship_slot == -1);
}

TEST_CASE("destroyed npc coasts with its inertia instead of stopping") {
  game::GameState state;
  state.scenario.ships.emplace_back(TestShipClass());
  state.scenario.ships[0].base_armor = 100;
  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.current_system_id = 0;
  ship.ship_class_id = 0;
  ship.armor_points = -1.0F; // destroyed
  ship.death_timer_active = 5.0F;
  ship.vel_x = 10.0F;
  ship.vel_y = 0.0F;
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;

  game::NovaShip_TickNpcShips(state, 1.0F);

  // Continuous movement consumes the full normalized tick, while the port's
  // clean-room destruction scheduler consumes one whole 21 ms logical call
  // and banks the remaining fraction.
  const float damped_velocity = 10.0F * std::pow(0.995F, 1.0F / 0.63F);
  CHECK(ship.vel_x == Catch::Approx(damped_velocity));
  CHECK(ship.pos_x == Catch::Approx(damped_velocity));
  CHECK(ship.death_timer_active == Catch::Approx(4.0F));
  CHECK(ship.destruction_raw_tick_accumulator ==
        Catch::Approx(1.0F / 0.63F - 1.0F));
}

TEST_CASE("destroyed npc keeps its velocity through the AI decision pass") {
  game::GameState state;
  state.scenario.ships.emplace_back(TestShipClass());
  state.scenario.ships[0].base_armor = 100;
  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.current_system_id = 0;
  ship.ship_class_id = 0;
  ship.ai_behavior_code = 3;
  ship.armor_points = -1.0F; // destroyed
  ship.death_timer_active = 5.0F;
  ship.vel_x = 10.0F;
  ship.pos_x = 0.0F;

  // Model the live frame order: the scope-6 AI pass runs before the scope-4/5
  // ship pass (NovaFrame_TickSystems). The AI pass used to zero vel_x/vel_y for
  // destroyed hulls, which pinned every wreck before the integrator ran.
  game::NovaShip_TickNpcAi(state, 1.0F);
  CHECK(ship.vel_x == Catch::Approx(10.0F)); // AI pass must not clear inertia
  game::NovaShip_TickNpcShips(state, 1.0F);
  const float damped_velocity = 10.0F * std::pow(0.995F, 1.0F / 0.63F);
  CHECK(ship.vel_x == Catch::Approx(damped_velocity));
  CHECK(ship.pos_x == Catch::Approx(damped_velocity));
}

// --- Turn-rate floor (Ship_ComputeShipMaxTurnRateDeg NPC branch) ---
TEST_CASE("npc turn-rate floor is a no-op for a clean ship (computed==base)") {
  game::GameState state;
  game::Ship ship;
  ship.ai_desired_heading_deg = 90;
  game::ShipClass cls = TestShipClass();
  // Base turn 8 raw -> 0.8 deg/tick. Ship_ComputeShipMaxTurnRateDeg floors the
  // returned rate up to 1.0 only when the base rate is already >= 1.0 AND the
  // computed (post-damping) rate fell below it; a clean NPC has no status/
  // disable damping so computed == base and the floor never engages here. It
  // only starts to matter once status/disable damping is reconstructed (see
  // the integrator comment).
  cls.turn_rate = 8.0F;

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  CHECK(ship.heading ==
        Catch::Approx(0.8F * std::numbers::pi_v<float> / 180.0F));

  // Same for a genuinely sluggish class whose base is below the floor guard: it
  // is not raised to 1.0 (only bases already >= 1.0 are floored).
  ship.heading = 0.0F;
  cls.turn_rate = 5.0F;
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  CHECK(ship.heading ==
        Catch::Approx(0.5F * std::numbers::pi_v<float> / 180.0F));
}

// Government SkillMult applies to NPC thrust and max speed (but NOT turn rate),
// mirroring the NPC branch of Ship_ComputeShipEffectiveThrust /
// Ship_ComputeShipEffectiveMaxSpeed (0x004640a0 / 0x004642e0). Ships with no
// faction keep the class base values.
TEST_CASE("npc effective stats apply the government skill multiplier") {
  game::GameState state;
  // Hand-built government at zero-based index 0 (faction ids are zero-based in
  // the clean-room; the ScenarioData accessor adds 0x80).
  state.scenario.governments.clear();
  game::Government g;
  g.skill_mult = 0.5F;
  state.scenario.governments.push_back(g);

  game::Ship ship; // faction_or_government_id defaults to -1
  game::ShipClass cls =
      TestShipClass(); // accel 500 -> 0.1 px/tick^2; speed 400 -> 4.0

  // No faction: class base values unchanged.
  const auto base = game::NovaShip_ComputeEffectiveStats(state, ship, cls);
  CHECK(base.thrust_px_per_tick2 == Catch::Approx(0.1F));
  CHECK(base.max_speed_px_per_tick == Catch::Approx(4.0F));
  CHECK(base.turn_rate_deg_per_tick == Catch::Approx(4.0F));

  // Government scale 0.5 halves thrust and max speed; turn rate is untouched.
  ship.faction_or_government_id = 0;
  const auto scaled = game::NovaShip_ComputeEffectiveStats(state, ship, cls);
  CHECK(scaled.thrust_px_per_tick2 == Catch::Approx(0.05F));
  CHECK(scaled.max_speed_px_per_tick == Catch::Approx(2.0F));
  CHECK(scaled.turn_rate_deg_per_tick == Catch::Approx(4.0F));

  // The integrator and the controls bridge consume the same effective stats
  // (NovaShip_IntegrateNpcMovement / NovaAi_ApplyControls share the helper).
  game::Ship moving;
  moving.faction_or_government_id = 0;
  moving.ai_desired_heading_deg = 0;    // heading 0 = up (-y)
  moving.ai_desired_speed = 100.0F;     // desired > 0: throttle toward the cap
  moving.ai_forward_thrust_cmd = 0.05F; // eff_thrust * 1 tick
  game::NovaShip_IntegrateNpcMovement(state, moving, cls, 1.0F);
  CHECK(moving.vel_y == Catch::Approx(-0.05F));
}

TEST_CASE("npc effective stats port the high-confidence modifier branches") {
  game::GameState state;
  game::ShipClass cls = TestShipClass(); // accel 500, speed 400, turn 40
  cls.capability_flags = 0x0400;
  game::Ship ship;
  CHECK(game::NovaShip_ComputeEffectiveStats(state, ship, cls)
            .thrust_px_per_tick2 == Catch::Approx(0.0F));
  CHECK(game::NovaShip_ComputeEffectiveStats(state, ship, cls)
            .max_speed_px_per_tick == Catch::Approx(0.0F));
  // The 0x400 planet-type zeroing lives only in Ship_ComputeShipEffectiveThrust
  // (0x004640a0) and Ship_ComputeShipEffectiveMaxSpeed (0x004642e0). The
  // turn-rate helper (0x00463e70) has no capability check, so the rate stays at
  // base*0.1 = 4.0 deg/tick.
  CHECK(game::NovaShip_ComputeEffectiveStats(state, ship, cls)
            .turn_rate_deg_per_tick == Catch::Approx(4.0F));

  cls.capability_flags = 0;
  ship.ship_instance_id = 3;
  ship.velocity_match_target_ship_slot = 7;
  const auto matched = game::NovaShip_ComputeEffectiveStats(state, ship, cls);
  CHECK(matched.thrust_px_per_tick2 == Catch::Approx(0.1F / 3.0F));
  CHECK(matched.max_speed_px_per_tick == Catch::Approx(4.0F / 3.0F));
  CHECK(matched.turn_rate_deg_per_tick == Catch::Approx(4.0F / 3.0F));

  ship.velocity_match_target_ship_slot = -1;
  ship.pers_def_slot = 0x03ff;
  const auto mission = game::NovaShip_ComputeEffectiveStats(state, ship, cls);
  CHECK(mission.thrust_px_per_tick2 == Catch::Approx(0.2F));
  CHECK(mission.max_speed_px_per_tick == Catch::Approx(8.0F));
  CHECK(mission.turn_rate_deg_per_tick == Catch::Approx(5.0F));

  ship.pers_def_slot = -1;
  ship.ionization_points = 50.0F;
  cls.ionization_capacity = 100;
  const auto status = game::NovaShip_ComputeEffectiveStats(state, ship, cls);
  CHECK(status.thrust_px_per_tick2 == Catch::Approx(0.05F));
  CHECK(status.max_speed_px_per_tick == Catch::Approx(4.0F));
  CHECK(status.turn_rate_deg_per_tick == Catch::Approx(2.0F));
}

TEST_CASE("ionization decay uses class rate and player dissipator outfits") {
  game::GameState state;
  state.scenario.ships.resize(1);
  state.scenario.ships[0].ionization_decay_rate = 0.01F;
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].mod_type =
      static_cast<std::int16_t>(game::OutfitEffect::kIonDissipator);
  state.scenario.outfits[0].mod_val = 100;
  state.inventory.outfit_owned_count[0] = 2;

  game::Ship player;
  player.ship_instance_id = 0;
  player.ship_class_id = 0;
  CHECK(game::NovaOutfit_ComputeIonizationDecayRate(state, player) ==
        Catch::Approx(2.01F));

  game::Ship npc = player;
  npc.ship_instance_id = 1;
  CHECK(game::NovaOutfit_ComputeIonizationDecayRate(state, npc) ==
        Catch::Approx(0.01F));
}

namespace {

// One player ship class with the given ionization fields, plus a fresh derived
// cache. Used by the ionization update-loop tests below.
void SetPlayerIonization(game::GameState &state,
                         float decay_rate,
                         std::int16_t capacity) {
  state.scenario.ships.resize(1);
  state.scenario.ships[0].ionization_decay_rate = decay_rate;
  state.scenario.ships[0].ionization_capacity = capacity;
  state.player.ship_class_id = 0;
  state.player.ship_instance_id = 0;
  state.InvalidateDerivedStatCaches();
}

} // namespace

TEST_CASE(
    "ionization update stores unclamped negative charge, reset next frame") {
  game::GameState state;
  SetPlayerIonization(state, 2.0F, 100);
  state.player.ionization_points = 1.0F;

  game::spaceflight_detail::NovaShip_UpdateIonizationCharge(
      state, state.player, 4.0F, 1.0F);
  // 1 - 2*1 = -1 is stored raw; the original only re-normalizes it on the
  // next frame's pre-decay gate (0x0045073f / 0x0043373f).
  CHECK(state.player.ionization_points == -1.0F);

  game::spaceflight_detail::NovaShip_UpdateIonizationCharge(
      state, state.player, 4.0F, 1.0F);
  CHECK(state.player.ionization_points == 0.0F);
}

TEST_CASE("ionization update: negative post-decay charge widens the ramp cap") {
  game::GameState state;
  SetPlayerIonization(state, 2.0F, 100);
  state.player.ionization_points = 1.0F;
  state.player.vel_x = 4.02F;

  game::spaceflight_detail::NovaShip_UpdateIonizationCharge(
      state, state.player, 4.0F, 1.0F);
  // intensity = -1/100 = -0.01; cap = 1.01 * 4 = 4.04, so 4.02 is inside the
  // cap and must be left untouched. Clamping the intensity to zero would give
  // cap = 4.0 and step the velocity down to 3.995.
  CHECK(state.player.ionization_points == -1.0F);
  CHECK(state.player.vel_x == Catch::Approx(4.02F));
  CHECK(state.player.vel_y == 0.0F);
}

TEST_CASE("ionization update: ramp steps past the cap without clamping to it") {
  game::GameState state;
  SetPlayerIonization(state, 0.0F, 200);
  state.player.ionization_points = 100.0F; // intensity 0.5
  state.player.vel_x = 2.01F;

  game::spaceflight_detail::NovaShip_UpdateIonizationCharge(
      state, state.player, 4.0F, 1.0F);
  // cap = 0.5 * 4 = 2.0; the original stores 2.01 - 0.025 = 1.985, below the
  // cap (no std::max clamp).
  CHECK(state.player.vel_x == Catch::Approx(1.985F));
}

TEST_CASE("ionization update: a zero cap steps and re-steps the same axis") {
  game::GameState state;
  SetPlayerIonization(state, 0.0F, 200);
  state.player.ionization_points = 100.0F; // intensity 0.5
  state.player.vel_x = 0.5F;

  // effective max speed 0 -> cap 0, step = 0.025 * 40 = 1.0. The original
  // re-tests the updated component, so it subtracts then adds back exactly.
  game::spaceflight_detail::NovaShip_UpdateIonizationCharge(
      state, state.player, 0.0F, 40.0F);
  CHECK(state.player.vel_x == 0.5F);
}

TEST_CASE("player ionization tick: dissipator decay and ModType-40 absorber "
          "capacity drive the ramp together") {
  game::GameState state;
  SetPlayerIonization(state, 0.0F, 100);
  state.scenario.ships[0].speed = 400.0F;
  state.scenario.outfits.resize(2);
  state.scenario.outfits[0].mod_type =
      static_cast<std::int16_t>(game::OutfitEffect::kIonAbsorber);
  state.scenario.outfits[0].mod_val = 100;
  state.scenario.outfits[1].mod_type =
      static_cast<std::int16_t>(game::OutfitEffect::kIonDissipator);
  state.scenario.outfits[1].mod_val = 100;
  state.inventory.outfit_owned_count[0] = 1; // absorber
  state.inventory.outfit_owned_count[1] = 1; // dissipator
  state.InvalidateDerivedStatCaches();
  state.player.ionization_points = 100.0F;
  state.player.vel_x = 2.5F;
  state.player.vel_y = -3.01F;

  game::PlayerTick_IonizationAndFuelRegeneration(state, 1000.0F / 30.0F);
  // decay = 0.01*100 = 1.0 -> points 99. Effective max = 4.0 raw x1.5
  // non-strict = 6.0; post-decay capacity = 100 + 100 = 200 -> intensity
  // 0.495, cap = 0.505*6.0 = 3.03.
  // `vel_x` 2.5 stays inside, which pins the absorber (without it capacity 100
  // -> intensity 0.99 -> capped 0.7 -> cap 1.8) *and* the x1.5 factor (with it
  // but no x1.5 the cap would be 2.02); both would step to 2.475.
  // `vel_y` -3.01 pins the post-decay ordering and negative axis: the pre-decay
  // charge gives cap 0.5*6.0 = 3.0 and would step it to -2.985.
  CHECK(state.player.ionization_points == 99.0F);
  CHECK(state.player.vel_x == Catch::Approx(2.5F));
  CHECK(state.player.vel_y == Catch::Approx(-3.01F));
}

TEST_CASE("effective max speed applies mission x2 and non-strict 1.5x") {
  game::GameState state;
  game::ShipClass cls = TestShipClass(); // speed 400 -> 4.0 px/tick
  state.scenario.ships.push_back(cls);
  state.player.ship_class_id = 0;

  game::Ship player;
  player.ship_instance_id = 0;
  player.ship_class_id = 0;
  player.squad_leader_ship_slot = 5; // not 0: no squad-leader arm

  // base 4.0, Strict Play off -> x1.5.
  CHECK(game::NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, player, cls) ==
        Catch::Approx(6.0F));

  state.pilot.strict_play = true;
  CHECK(game::NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, player, cls) ==
        Catch::Approx(4.0F));

  // Mission ship (pers_def_slot 0x3ff) x2, then non-strict x1.5.
  state.pilot.strict_play = false;
  player.pers_def_slot = 0x03ff;
  CHECK(game::NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, player, cls) ==
        Catch::Approx(12.0F));

  // A negative opcode-8 aggregate clamps to zero.
  state.scenario.ships[0].speed = -400.0F;
  state.player.pers_def_slot = -1;
  CHECK(game::NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, player, cls) ==
        0.0F);
}

TEST_CASE("effective max speed applies the squad-leader 1.5x to NPCs") {
  game::GameState state;
  game::ShipClass cls = TestShipClass(); // speed 400 -> 4.0 px/tick
  state.scenario.ships.push_back(cls);

  game::Ship npc;
  npc.ship_instance_id = 1;
  npc.ship_class_id = 0;
  npc.squad_leader_ship_slot = 0; // squad leader

  CHECK(game::NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, npc, cls) ==
        Catch::Approx(6.0F));

  npc.squad_leader_ship_slot = 3; // follower: no x1.5
  state.pilot.strict_play = false;
  CHECK(game::NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, npc, cls) ==
        Catch::Approx(4.0F));

  npc.squad_leader_ship_slot = 0;
  state.pilot.strict_play = true;
  CHECK(game::NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, npc, cls) ==
        Catch::Approx(4.0F));
}

TEST_CASE("npc ionization ramp runs in the ship pass after decay") {
  game::GameState state;
  game::ShipClass cls = TestShipClass();
  cls.ionization_capacity = 100;
  cls.ionization_decay_rate = 2.0F;
  state.scenario.ships.push_back(cls);

  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.current_system_id = 0;
  ship.ship_class_id = 0;
  ship.ship_instance_id = 1; // NPC
  ship.armor_points = 10.0F; // alive
  ship.ionization_points = 100.0F;
  ship.vel_x = 10.0F;
  ship.ai_maneuver_timer_ms = 10.0F; // coast: movement holds velocity

  game::NovaShip_TickNpcShips(state, 1.0F);

  // decay 2*1 -> 98; intensity 0.98 -> capped 0.7 -> cap 0.3*4 = 1.2; one
  // 0.025 step. Before the NPC ramp was ported this stayed at 10.0.
  CHECK(ship.ionization_points == 98.0F);
  CHECK(ship.vel_x == Catch::Approx(9.975F));
}
