#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/outfit.hpp"
#include "game/ship_ai.hpp"
#include "game/spaceflight.hpp"

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

  // One tick of forward thrust: step = cmd * ticks = 10.
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  CHECK(ship.vel_y == Catch::Approx(-10.0F)); // polar(0)= up / -y for sin/cos
  CHECK(ship.pos_y == Catch::Approx(-10.0F));
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

  // The original mode-4 arm (Ship_HandleShip, 0x00433050) adds the ramp to
  // position; it does not accumulate the ramp into ordinary velocity.
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F, 200);
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F, 233);

  const float first_ramp = 200.0F / 3.5F - 35.0F;
  const float second_ramp = 233.0F / 3.5F - 35.0F;
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
  // 0.995: a disabled ship drifts to a stop gradually, not immediately.
  CHECK(disabled.vel_y == Catch::Approx(-9.95F));
  CHECK(disabled.pos_y == Catch::Approx(-9.95F));
  CHECK(disabled.speed == Catch::Approx(9.95F));

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

  CHECK(ship.ai_desired_speed == Catch::Approx(-27.67F));
  CHECK(ship.ai_maneuver_timer_ms <= 0.0F);
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

TEST_CASE("steer velocity rotates heading*speed toward the prior velocity") {
  game::Ship ship;
  ship.heading = 0.0F; // heading 0 = up (-y)
  ship.speed = 4.0F;   // gravity-shield scalar speed
  // Prior velocity points elsewhere (large positive x) so the steer converges
  // from it toward the heading*speed vector at a bounded per-axis rate.
  ship.vel_x = 100.0F;
  ship.vel_y = -100.0F;

  // eff_thrust 0.1 * 4.0 turn-scale = 0.4 step per tick.
  game::NovaShip_SteerVelocityTowardShipHeading(ship, 0.1F, 1.0F);
  // new heading*vel = (0,-4). It moves from the prior velocity toward that by
  // at most 0.4 per axis (clamped, never crossing the new value). Starting far
  // away, the not-yet-clamped result stays within one step of the prior side
  // of the target:
  CHECK(ship.vel_x == Catch::Approx(0.4F));  // approached 0 from above
  CHECK(ship.vel_y == Catch::Approx(-4.4F)); // approached -4 from below
}

TEST_CASE("gravity-shield npc keeps a scalar clamped speed and applies it") {
  game::GameState state;
  game::Ship ship;
  // flags_secondary 0x40 marks the class as a gravity-shield ship.
  game::ShipClass cls = TestShipClass();
  cls.flags_secondary = 0x40;
  ship.ai_desired_heading_deg = 0;
  ship.ai_desired_speed = 100.0F;
  ship.ai_forward_thrust_cmd = 2.0F;
  ship.speed = 1.0F;

  // Forward thrust accumulates the scalar speed: 1 + cmd*ticks = 3, then the
  // position block steers the velocity toward heading*3 at 0.4/tick from rest:
  // vel_y approaches -3 by one step, so after one tick it sits at -2.6.
  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  CHECK(ship.speed == Catch::Approx(3.0F));
  CHECK(ship.vel_y == Catch::Approx(-2.6F)); // heading 0 = up, one steer step
  CHECK(ship.pos_y == Catch::Approx(-2.6F));
}

TEST_CASE("gravity-shield detect excludes ai_control_mode 0x0c") {
  game::Ship ship;
  game::ShipClass cls = TestShipClass();
  cls.flags_secondary = 0x40;
  CHECK(game::NovaShip_HasGravityShield(ship, cls));
  ship.ai_control_mode = 0x0c;
  CHECK_FALSE(game::NovaShip_HasGravityShield(ship, cls));
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

  // Stop thrusting: glow decays one unit/frame toward 0.
  ship.ai_forward_thrust_cmd = 0.0F;
  for (int i = 0; i < 40; ++i) {
    game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F);
  }
  CHECK(ship.engine_glow_level == 0);
  CHECK(ship.engine_glow_intensity == Catch::Approx(0.0F));
}

TEST_CASE("npc maneuver timer fades glow only once per frame") {
  game::GameState state;
  game::Ship ship;
  game::ShipClass cls = TestShipClass();
  ship.engine_glow_level = 10;
  ship.ai_forward_thrust_cmd = 0.5F;
  ship.ai_maneuver_timer_ms = 10.0F;

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 0.0F);

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

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 0.0F);

  // Ghidra 0x00433050 opens the movement/glow block for state 0x16, and the
  // inner fade test requires `timer > 0 && !state0x16`. Full burn adds 1.
  CHECK(ship.engine_glow_level == 11);
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

  game::NovaShip_IntegrateNpcMovement(state, ship, cls, 1.0F, 200);

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

  // Ship_HandleShip applies the disabled 0.995 damp then integrates one tick;
  // the wreck must not be pinned at its destruction point.
  CHECK(ship.vel_x == Catch::Approx(9.95F));
  CHECK(ship.pos_x == Catch::Approx(9.95F));
  CHECK(ship.death_timer_active == Catch::Approx(4.0F));
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
  CHECK(ship.vel_x == Catch::Approx(9.95F));
  CHECK(ship.pos_x == Catch::Approx(9.95F));
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

// Government combat_rating_scale applies to NPC thrust and max speed (but NOT
// turn rate), mirroring the NPC branch of Ship_ComputeShipEffectiveThrust /
// Ship_ComputeShipEffectiveMaxSpeed (0x004640a0 / 0x004642e0). Ships with no
// faction keep the class base values.
TEST_CASE("npc effective stats apply the government combat rating scale") {
  game::GameState state;
  // Hand-built government at zero-based index 0 (faction ids are zero-based in
  // the clean-room; the ScenarioData accessor adds 0x80).
  state.scenario.governments.clear();
  game::Government g;
  g.combat_rating_scale = 0.5F;
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
