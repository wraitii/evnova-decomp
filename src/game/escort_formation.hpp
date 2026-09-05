// Escort/formation positioning and attached-ship system transfer.
//
// Reconstructs the original's wedge-formation system:
//   - Ship_UpdateEscortFormations 0x00413990 (per-frame leader pass)
//   - Ship_SetEscortLaunchOffsetVelocity 0x00413b60 (wedge slot offsets)
//   - Ship_MoveShipTowardFormationOffset 0x00414390 (offset pursuit/snap)
//   - Ship_ReacquireAiTargetLeader 0x004156a0 (leader death handoff)
//   - the Frame_TickSystems 0x004186b0 scope-6 leader-flag pass that gates the
//     per-frame formation update (ShipState +0xC0/+0xC1/+0xC2)
//   - Ship_ResetShipToDefaultCombatState 0x0041e240 and the escort-adoption
//     slice of System_RebuildInitialNpcAndMissionPopulation 0x0041af90, which
//     transfers ships attached to the player (ai_target_ship_slot == 0) into
//     the player's system at every system entry.
#pragma once

#include "game_state.hpp"

namespace game {

// Ship_UpdateEscortFormations (0x00413990). For every active ship whose
// resolved_ai_target_ship_slot equals leader.ship_instance_id, assigns the
// follower's wedge slot around the leader (writing the follower's formation
// offset fields) and, when snap is set, moves it directly onto that offset
// (skipping state-0x15 arrivals). snap=1 callers: system-entry rebuild +
// encounter-fleet spawning; snap=0 is the per-frame AI pass.
void NovaEscort_UpdateFormations(GameState &state, Ship &leader, bool snap);

// Convenience wrapper of NovaEscort_UpdateFormations for the player as the
// formation leader (System_RebuildInitialNpcAndMissionPopulation calls it with
// g_ship_states[0], snap=1).
void NovaEscort_UpdateFormationsForPlayer(GameState &state);

// Ship_MoveShipTowardFormationOffset (0x00414390). Moves the ship onto its
// stored formation offset (game_state Ship::formation_offset_x/y): snap=true
// teleports position onto the offset; snap=false creeps per axis at
// effective thrust * 10 px/tick inside an 8 px deadzone, suppressed while
// ai_station_hold_timer > 0. No-op when the resolved target slot is invalid.
void NovaEscort_MoveTowardFormationOffset(GameState &state,
                                          Ship &ship,
                                          bool snap_to_offset,
                                          float elapsed_ticks);

// Frame_TickSystems (0x004186b0) scope-6 leader-flag slice. Snapshots every
// ship's ai_target_ship_slot, then for each active behavior>4 ship in the
// player's system validates/reacquires its leader and refreshes the
// +0xC0/+0xC1/+0xC2 leader bytes. Runs once per full tick before the per-ship
// AI dispatch; Ship_UpdateShipAI gates its per-frame formation pass on the
// +0xC2 byte this pass produces.
void NovaEscort_TickLeaderFlags(GameState &state);

// Ship_ReacquireAiTargetLeader (0x004156a0). ai_target_ship_slot must point at
// a live, non-disabled ship; otherwise the heaviest hull in the current system
// whose snapshot target matches the stale leader replaces it. With no
// candidate the ship reverts to its class default AI (state 0x13); when the
// replacement is the ship itself it copies the old leader's combat state (or
// reverts) and detaches.
void NovaEscort_ReacquireAiTargetLeader(GameState &state, Ship &ship);

// Ghidra 0x0041e240 Ship_ResetShipToDefaultCombatState. Guards on
// ai_target_ship_slot == 0 (attached to the player); refill mirrors the
// original's flag != 0 arm (shields/armor + class-default weapon stock).
void NovaShip_ResetToDefaultCombatState(GameState &state,
                                        Ship &ship,
                                        bool refill);

// Escort-adoption slice of System_RebuildInitialNpcAndMissionPopulation
// (0x0041af90, disassembly 0x0041b050-0x0041b237). Every active ship attached
// to the player (ai_target_ship_slot == 0) is adopted into the player's
// current system: disabled ships are deactivated (behavior-6 cargo escorts
// hand their cargo back first -- TODO(decomp(0x00469810)) transfer unported),
// the rest run Ship_ResetShipToDefaultCombatState (0x0041e240; refill=true
// also restores shields/armor and weapon stock, the original's flag != 0).
// The wedge then snaps around the player, and when the player's
// ai_station_hold_timer != 0 (jump arrival windows it at -999, 0x0044fa83 /
// 0x0044faa2) each attached ship is pushed ~891.7 px behind its own heading
// and flung forward at 50 px/tick: escorts stream in behind the jumping
// player. With refill=true the formation snap runs a second time at the end,
// mirroring the original's flag != 0 tail call (after mission fleets spawn).
void NovaSystem_RestorePlayerEscorts(GameState &state,
                                     bool refill,
                                     std::uint32_t now_ms);

} // namespace game
