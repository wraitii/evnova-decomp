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
// System.nav_defs). Picking travel-stellar slot i jumps the ship to
// System.links[i]. The travel slot (ai_secondary_target_slot) selects that
// pairing.
//
// The in-flight hyperspace flight itself is a simplified countdown stand-in
// (TODO(decomp)): the ship coasts and holds for jump_countdown_ticks, then the
// jump completes with the original's state changes. Visuals/audio and escort
// warp-sync are documented diviergences for a later pass.

#include "game_state.hpp"

namespace game {

// Number of fuel points a single jump burns. From the Bible "Fuel (100 = 1
// jump)" and the diagnostic jump gate Stellar_CanShipInitiateJumpSequence
// (class fuel_capacity >= 100) / the completion decrement `fuel_points -=
// _DAT_005755a4`.
inline constexpr float kJumpFuelCost = 100.0F;

// Mirrors Stellar_FindNearestAvailableTravelStellar (0x00462db0): returns the
// travel-slot index (0..15) of the nearest available travel stellar in the
// player's current system, or -1 when no travel point qualifies. A travel
// stellar qualifies when it is present in the system's nav-defs and is
// "available" (the original checks per-stellar is_available + travel_flags&1;
// we treat a present, resolved nav-stellar as available). Restriction flags
// (availability_flags & 0x3000) require the ship to be within the jump range.
[[nodiscard]] int NovaTravel_FindNearestTravelPoint(const GameState &state);

// Mirrors Stellar_CanShipInitiateJumpSequence (0x00415b80): returns true when
// the player ship may initiate a hyperspace jump. Gates on the ship class
// fuel capacity being at least one jump (kJumpFuelCost) and the ship not
// being locked onto another ship's velocity match (no NPC fleet, so that check
// is degenerate), plus having enough fuel on hand.
[[nodiscard]] bool NovaTravel_CanStartJump(const GameState &state);

// Ticks the cross-system travel state machine once per spaceflight frame.
// Handles (a) engaging a jump when the travel key is pressed near an available
// travel point, (b) running the engaged countdown, and (c) completing the jump
// (fuel burn + system change + reposition + refill) once the countdown elapses.
// `travel_input` is the edge-triggered travel key state; `frame_time_ms` scales
// the countdown. Reads/writes state.travel; sets just_completed the frame the
// jump lands. Requires a valid scenario. The completed jump does NOT re-spawn
// the starfield itself; the spaceflight loop observes just_completed and calls
// SpaceflightView::SpawnAmbientStars for the new system.
void NovaTravel_Tick(GameState &state, bool travel_input, float frame_time_ms);

} // namespace game
