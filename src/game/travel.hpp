#pragma once

// Clean-room cross-system travel (the plain hyperspace jump between adjacent
// systems). This does NOT cover hypergate/wormhole destination selection
// (which needs the interaction dialog) or the full in-flight hyperspace flight
// (Stellar_HandlePlayerHyperspaceSequence, 0x0044f3d0 -- 2670 lines entangled
// with audio, fleet warp-sync and missions). It reconstructs the faithful,
// self-contained primitives and string them into an explicit jump state
// machine that the spaceflight loop ticks once a frame:
//
//   Stellar_FindNearestAvailableTravelStellar  (0x00462db0)  nearest travel pt
//   Stellar_CanShipInitiateJumpSequence        (0x00415b80)  fuel/mission gate
//   the jump-complete block in Ship_HandlePlayerShipCore      fuel burn + the
//     current_system_id change, reposition, shield/armor refill, star re-spawn
//
// Mapping (confirmed from the decomp + the Bible): a System's adjacency block
// is 32 entries -- indices 0..15 are the destination systems (Con1-16, our
// System.links) and indices 16..31 are the travel stellars (NavDef1-16, our
// System.nav_defs). The jump TARGET is a System.links entry (a direct
// hyperlink); the NavDef list only names the departure-point stellar for some
// slots and is NOT paired 1:1 with every link in the scenario data (e.g. Kania
// links to Tichel at slot 3 with no travel stellar there, yet 'j' still jumps
// Kania->Tichel). So a plotted destination is resolved purely against links.
//
// The in-flight hyperspace flight is modelled as visible phases (see the
// NovaTravel_Tick notes below), mirroring the original's pre-fire block in
// Ship_HandlePlayerShip (0x0044b120, travel_transfer_mode == 3):
//   kSlowTurn -- while the ship still has velocity it turns around to face the
//     REVERSE of its velocity (flying out from the system, that points back
//     toward the jump vector) at a fast minimum turn rate (max(computed+1, 20)
//     deg/tick), brakes by _DAT_005755f0 (0.992)/frame, and once facing ramps
//     the engine glow by +3/frame to 24 (ShipState +0xc8d4). Ends when the
//     ship has come to a stop (|vel| < 2).
//   kHold -- the ship re-aims at the destination bearing at class turn rate
//     and holds briefly with the glow at max (the original's
//     ai_station_hold_timer ramp), then fires.
//   kFlying -- in-tunnel coast at max speed while the spaceflight view renders
//     the streaking star tunnel, then completion (fuel burn, system change,
//     arrival at max speed aimed at the new-system center).
// The original also stages jump audio (Stellar_TriggerHyperspaceAudioOnce,
// NovaAudio_PreStageJumpSoundBySeconds) and warp-syncs escort ships by jump
// depth (Stellar_ComputeShipJumpDepth); those remain documented divergences
// for a later pass.

#include "game_state.hpp"

namespace game {

// Number of fuel points a single jump burns. From the Bible "Fuel (100 = 1
// jump)" and the diagnostic jump gate Stellar_CanShipInitiateJumpSequence
// (class fuel_capacity >= 100) / the completion decrement `fuel_points -=
// FLOAT_kJumpFuelCost` (named 0x005755a4 in Ghidra).
inline constexpr float kJumpFuelCost = 100.0F;

// Mirrors Stellar_FindNearestAvailableTravelStellar (0x00462db0): returns the
// travel-slot index (0..15) of the nearest available travel stellar in the
// player's current system, or -1 when no travel point qualifies. A travel
// stellar qualifies when it is present in the system's nav-defs, available,
// and has travel_flags bit 1. Restriction flags
// (availability_flags & 0x3000) require the ship to be within the jump range.
[[nodiscard]] int NovaTravel_FindNearestTravelPoint(const GameState &state);

// Mirrors Stellar_CanShipInitiateJumpSequence (0x00415b80): returns true when
// the player ship may initiate a hyperspace jump. Gates on the ship class
// fuel capacity being at least one jump (kJumpFuelCost) and the ship not
// being locked onto another ship's velocity match (no NPC fleet, so that check
// is degenerate). Fuel is consumed by the jump-completion path; this helper's
// original implementation does not inspect the current fuel amount.
[[nodiscard]] bool NovaTravel_CanStartJump(const GameState &state);

// Mirrors Stellar_CanShipInitiateJumpSequence (0x00415b80) for an arbitrary
// (NPC) ship: returns true when `ship` may initiate a hyperspace jump. Gates on
// the ship's own class fuel capacity (kJumpFuelCost), NOT the player's, and on
// the ship's CURRENT fuel being at least one jump (Stellar_HandlePlayerShipCore
// refuses to (re)enter hyperspace while fuel_points < kJumpFuelCost). The
// original also blocks while velocity-matched to another ship and under
// certain mission-ship flags; those need the velocity-match / mission systems
// and are deferred (see the .cpp).
[[nodiscard]] bool
NovaTravel_CanShipInitiateJumpSequence(const GameState &state,
                                       const Ship &ship);

// Ticks the cross-system travel state machine once per spaceflight frame.
// Handles (a) engaging a jump when the travel key is pressed near an available
// travel point, (b) running the engaged visible phases (slow-turn/brake onto
// the jump heading, then the in-tunnel coast at max speed while the starfield
// streams), and (c) completing the jump (fuel burn + system change + arrival at
// max speed aimed at the new system center + refill) once the tunnel elapses.
// `travel_input` is the edge-triggered travel key state; `frame_time_ms` scales
// the slow-turn. Reads/writes state.travel; sets just_completed the frame the
// jump lands. Requires a valid scenario. While engaging, the caller must skip
// player movement integration (the jump owns the ship) and the spaceflight
// view branches on state.travel.engaging to render the star tunnel. The
// completed jump does NOT re-spawn the starfield itself; the spaceflight loop
// observes just_completed and calls SpaceflightView::SpawnAmbientStars.
// Completion-scope discovery helper: marks `zero_based_system_id` explored
// (and each of its linked neighbours visible/explored) so the galaxy starmap
// reveals the neighbourhood when the player enters a system. Mirrors the
// discovery flood that runs on system entry in the original
// (System_FloodDiscoverAdjacentSystems / System_RebuildSystemVisibilityMap).
// Idempotent; safe on empty/out-of-range systems.
void NovaTravel_MarkSystemDiscovered(GameState &state,
                                     std::int16_t zero_based_system_id);

// Plots `destination_zero_based` (a system selected in the galaxy starmap) as
// the player's next-jump destination. Finds the current system's travel slot
// whose linked destination matches and stores the resolved slot + stellar on
// the travel state, so the HUD shows the plotted jump and 'j' engages it. When
// the destination is not directly linked from the current system (no single
// jump reaches it), the plot is recorded but no travel slot is armed; 'j'
// then falls back to the nearest travel point. Returns true when a direct
// travel slot was found and armed.
bool NovaTravel_PlotStarmapDestination(GameState &state,
                                       std::int16_t destination_zero_based);

// Cycles the player's next-jump destination SYSTEM through the systems
// directly linked to the current system, in slot order. Mirrors the original's
// command-0x60 block in Ship_HandlePlayerShip (g_playerCycleTravelTarget-
// CommandLatch / travel_transfer_mode == 3 / ai_secondary_target_slot++): a
// destination is only offered when its link slot resolves to a travelable
// system. Each press advances (forward=true) or retreats (forward=false) one
// slot (wrapping); sets the travel slot + destination on state.travel so 'j'
// jumps there and the HUD shows the name. Returns the newly selected
// destination zero-based system id, or -1 when the current system has no
// travelable links.
[[nodiscard]] std::int16_t NovaTravel_CycleDestinationSystem(GameState &state,
                                                             bool forward);

void NovaTravel_Tick(GameState &state, bool travel_input, float frame_time_ms);

} // namespace game
