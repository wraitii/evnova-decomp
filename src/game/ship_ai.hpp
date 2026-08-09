#pragma once

// Clean-room reconstruction of the NPC ship AI decision layer, mirroring the
// Ghidra per-ship AI functions documented in the plates. On the movement side
// the AI writes the concrete fields already consumed by
// NovaShip_IntegrateNpcMovement (ai_desired_heading_deg / ai_desired_speed /
// ai_forward_thrust_cmd); the AI state machine (Ship_UpdateShipAiState
// 0x00405590) maintains Ship.ai_state_code, the behavior supervisors
// (0x00401000 and the 0x00402xxx/0x00403xxx set) pick primary targets and
// escalate between states, and Ship_ApplyShipAiControls (0x00408150) finally
// translates Ship.ai_control_mode into those concrete movement fields. Called
// once per active NPC ship per frame from the spaceflight loop's
// Stub_AiRoutines (Ghidra scope 6 part 2).
//
// Convention mirrors ship_spawn.cpp: each Ghidra function maps to one top-level
// helper with the address in a comment; NPC ships carry no outfit inventory, so
// the class base values stand in for the effective stats the original derives
// from outfits/status effects (TODO(decomp) markers below). Units are the
// simulation's reference cadence (30 Hz); rates passed in are already frame-
// scaled by the caller where the original does the same.

#include "game_state.hpp"
#include "scenario_data.hpp"

namespace game {

// Ghidra 0x004688e0 Ship_IsShipDestroyed. True when the death timer is active
// (death_timer_active > 0) or armor_points <= 0.
[[nodiscard]] bool NovaAiShip_IsDestroyed(const Ship &ship);

// Ghidra 0x00410670 Ship_EnterShipAiState0x02_ClearPrimaryTarget. Enters AI
// state 0x02 (idle-template / approach station-keeping), clears the current
// primary target, clamps the station-hold timer below zero, and records the
// current tick count in ai_mode_start_time_ms.
void NovaAi_EnterState2ClearPrimaryTarget(Ship &ship, std::uint32_t now_ms);

// Ghidra 0x00405590 Ship_UpdateShipAiState. The per-frame AI state machine.
// Given the ship's current ai_state_code it maintains that state and writes
// the companion Ship.ai_control_mode field (consumed by
// Ship_ApplyShipAiControls to produce concrete steering/thrust). States 0..0x16
// as enumerated in the plate comment in ship_ai.cpp. Most states are faithful;
// the HUD/mission flavor side-effects (extortion messages, carrier-bay launch,
// fuel-transfer chatter) are documented no-ops until those systems are
// reconstructed.
void NovaAi_UpdateShipState(GameState &state, Ship &ship, std::uint32_t now_ms);

// Ghidra 0x00408150 Ship_ApplyShipAiControls. Transforms the ship's
// ai_control_mode (written each frame by the state machine) into the concrete
// movement fields the integrator consumes: ai_desired_heading_deg,
// ai_desired_speed, ai_forward_thrust_cmd. This is the bridge that makes the
// AI state machine actually move ships. Also latches ai_fire_trigger_latch for
// the firing path (deferred to Phase 5). Provisional: many per-mode turn/thrust
// polynomials come from global constants that would need gameplay observation
// to pin exactly.
void NovaAi_ApplyControls(GameState &state, Ship &ship, float frame_time_ms);

// Ghidra 0x00401000 Ship_UpdateShipAI. The top-level per-ship AI entry: applies
// the global "heavy AI" cadence gating/skips, recomputes some stat caches,
// dispatches to the behavior supervisor selected by ship.ai_behavior_code, then
// runs the state machine and applies controls. `skip_heavy_ai` mirrors the
// original's parameter and forces the reduced path. Wire this once per active
// NPC ship per frame (it replaces Stub_AiRoutines scope-6 part 2).
void NovaAi_UpdateShipAI(GameState &state,
                         Ship &ship,
                         bool skip_heavy_ai,
                         std::uint32_t now_ms);

} // namespace game
