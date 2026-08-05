#pragma once

// Clean-room reconstruction of the in-game spaceflight mode, mirroring Ghidra
// 0x00489210 Ship_RunSpaceflightMode and its contained 0x00417600
// Frame_SpaceflightLoop / 0x004186b0 Frame_TickSystems. The full flight/AI/
// combat simulation is not reconstructed; this module provides the faithful
// *skeleton*: it plays the new-game intro cinematic on first entry, then runs
// the in-system loop with the original's phase/scope ordering
// (Frame_SpaceflightLoop pre-draw/sim/draw/post-draw, Frame_TickSystems's
// run_full_tick-gated scopes), each real scope yielded to a loud stub that
// logs what it stands in for (mirroring the Stub_* isolation in
// new_pilot_flow.cpp). As each gameplay subsystem is reconstructed, its stub
// grows a real implementation without changing the loop skeleton. Entering
// and leaving mirrors the original mode transition between the UI shell (main
// menu) and active flight.
//
// The reimplementation's escape back to the menu diverges from the original,
// which latches DAT_00596d38 on the primary mouse command through the pause
// menu; see the loop body in spaceflight.cpp.

#include "../sdl_platform.hpp"
#include "game_state.hpp"

class SdlPlatform;

namespace game {

// Enters in-system spaceflight mode. On the pilot's first entry (state
// intro_played == false) it first plays the intro cinematic, then sets
// intro_played = true immediately after it returns -- mirroring
// Ship_RunSpaceflightMode setting DAT_00596d35 = 0x01 right after
// IntroCinematic_Run(). Then it runs the in-game main loop until the player
// returns to the menu, at which point it returns. Mirrors
// Ship_RunSpaceflightMode's gating (DAT_00596d35) and clean return to the menu
// shell.
void NovaSpaceflight_Run(SdlPlatform &platform, GameState &state);

// Ship-class movement stats derived from the raw resource as the original
// loader (NovaData_LoadScenarioResourceTables 0x004bd3c0) derives
// ShipClassDef.base_turn_rate_deg / base_speed / accel(0x3c):
//   accel    = raw_accel    / 10000.0   (px/frame^2)
//   speed    = raw_speed    / 640.0     (px/frame)
//   turn     = raw_maneuver * 0.1       (deg/frame)
// See spaceflight.cpp for the source-constant references.
struct PlayerMovementStats {
  float turn_rate_deg_per_frame = 0.0F;
  float max_speed_px_per_frame = 0.0F;
  float thrust_px_per_frame2 = 0.0F;
};

// Pure free-flight physics integrator (unit-testable; no SDL). Derives stats
// from a ShipClass and advances the player ship for one frame according to the
// Input key latches. Faithful to the original movement model ("Player ship
// movement (free flight)" comment in spaceflight.cpp): bank continuously at
// the class turn rate while a turn key is held, thrust along heading as a
// per-AXIS-polar-clamped step (Math_AddPolarVelocityWithClamp 0x0043b4e0),
// inertia-preserving coast when thrust released, and reverse-thrust braking
// toward rest. Returns the same PlayerMovementStats it integrated with so the
// caller knows what was applied.
[[nodiscard]] PlayerMovementStats NovaPlayer_IntegrateMovement(
    PlayerShip &ship, const FlightInput &input, const ShipClass &ship_class);

// Reads the live flight controls (via SdlPlatform::PollFlightInput) and
// integrates the player ship's heading/throttle into GameState.player so the
// ship flies during flight.
extern void NovaPlayer_UpdateFromInput(SdlPlatform &platform, GameState &state);

} // namespace game
