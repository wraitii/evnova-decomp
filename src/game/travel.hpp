#pragma once

// Clean-room cross-system travel (the plain hyperspace jump between adjacent
// systems). This does NOT cover hypergate/wormhole destination selection
// (which needs the interaction dialog). It reconstructs the faithful,
// self-contained primitives and strings them into an explicit jump state
// machine that the spaceflight loop ticks once a frame:
//
//   the jump blocks in Ship_HandlePlayerShipCore (0x0044aa70): the engage
//     gates (0x0044c195 dispatch), the turnaround/hold physics, and the fire
//     + arrival block at PlayerTick_HyperspaceSequenceAnchor (0x0044f3d0)
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
// The visible phases are (see the NovaTravel_Tick notes below):
//   kBrake -- while |round(vel)| >= 2 on either axis the ship faces the
//     REVERSE of its velocity, retro-thrusts once inside the facing window
//     max(class turn+1, 20) deg (a facing window, not a turn speed), ramps
//     the engine glow by +3/tick to 24, and damps velocity by
//     g_jump_turnaround_velocity_damp (0x5755f0, 0.99204) per 30 Hz tick.
//   kHold -- velocity damps by g_hyperspace_slow_phase_velocity_damp
//     (0x5755f8, 0.98007), the hull turns onto the map bearing toward the
//     destination system (the per-frame re-aim at LAB_0044edfd), and the
//     'Warp up' cue plays. The tail of the hold is the TUNNEL: once the hull
//     faces the jump bearing within max(class turn, 30 deg) the position
//     advances along it by min(progress, 50) px/tick with progress =
//     elapsed_60hz*multiplier/(364*0.01) - 35/multiplier -- both the elapsed
//     clock and the 364 duration are 1/60 s ticks (duration = the cue's own
//     length, snd 128 frames*60/rate), so for a stock ship the ramp starts
//     ~2.1 s into the hold and hits the cap at ~5.2 s -- and the engine glow
//     overdrives +4/tick to 32 (past the normal 24). The fire lands once the
//     hold passes g_hyperspace_engage_hold_30hz (0x5755a8, 30 ticks) and the
//     cue has finished (~6.1 s) -- the boom lands as the cue resolves.
//   fire -- the boom/arrival: full-screen flash + 'Warp out' cue + the
//     position hurl 1350 px from the destination center along the map
//     bearing + 180, velocity reset to max speed along the current heading,
//     fuel burn + system change. Control returns to normal flight
//     immediately (no separate post-fire tunnel phase); the ship streaks
//     through the new system while coasting.
// Known remaining divergences: escort warp-sync and the multi-jump outfit
// (Stellar_ComputeShipJumpDepth 0x0046cdd0), ShipClassDef
// jump_duration_multiplier-driven audio staging
// (NovaAudio_PreStageJumpSoundBySeconds), the multi-hop planned-route jump
// continuation, and the disabled-in-jump 'hyperspace field collapsed' exit.

#include <cstdint>

#include <vector>

#include "game_state.hpp"

namespace game {

// Number of fuel points a single jump burns. From the Bible "Fuel (100 = 1
// jump)" and the diagnostic jump gate Stellar_CanShipInitiateJumpSequence
// (class fuel_capacity >= 100) / the completion decrement `fuel_points -=
// FLOAT_kJumpFuelCost` (named 0x005755a4 in Ghidra).
inline constexpr float kJumpFuelCost = 100.0F;

// Voice tag for the 'Warp up' cue (snd 128) played at the start of the
// stationary hold. The fire gate counts active instances of this handle
// (NovaAudio_CountActiveByHandle on g_hyperspace_sound_handle_warp_up) and
// only fires the jump once the cue has finished; the spaceflight loop passes
// the live count into NovaTravel_Tick.
inline constexpr int kHyperspaceWarpUpSoundKey = 128;

// Ghidra 0x00462db0 Stellar_FindNearestAvailableTravelStellar: returns the
// travel-slot index (0..15) of the nearest available travel stellar in the
// player's current system, or -1 when no travel point qualifies. A travel
// stellar qualifies when it is present in the system's nav-defs, available,
// and has travel_flags bit 1. Restriction flags
// (availability_flags & 0x3000) require the ship to be within the jump range.
[[nodiscard]] int NovaTravel_FindNearestTravelPoint(const GameState &state);

// Ghidra Ship_HandlePlayerShipCore range probe (the flight-tail jump-range cue
// ~0x00450a2c and the NovaUi_DrawTravelStatusPanel 0x0045e400 destination
// colour test run the identical loop): true when the ship is far enough from
// the system center to jump -- no NON-restricted nav stellar of the current
// system sits within Stellar_ComputeTravelRangeSq of it. The distance is
// always measured from the system center (0,0); the nav loop only asks
// whether any non-restricted nav exists, so a system with none is always
// "in range". Restricted travel stellars (availability_flags 0x3000) do not
// gate the probe.
[[nodiscard]] bool NovaTravel_PlayerInJumpRange(const GameState &state);

// Ghidra 0x00415b80 Stellar_CanShipInitiateJumpSequence: returns true when
// the player ship may initiate a hyperspace jump. Gates on the ship class
// fuel capacity being at least one jump (kJumpFuelCost) and the ship not
// being locked onto another ship's velocity match (no NPC fleet, so that check
// is degenerate). Fuel is consumed by the jump-completion path; this helper's
// original implementation does not inspect the current fuel amount.
[[nodiscard]] bool NovaTravel_CanStartJump(const GameState &state);

// Ghidra 0x00415b80 Stellar_CanShipInitiateJumpSequence, for an arbitrary
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
// Mirrors the jump blocks of Ship_HandlePlayerShipCore (0x0044aa70):
//  (a) Engage (travel key, original binding 14): requires a plotted
//      destination (travel_transfer_mode == 3; a bare 'j' with nothing plotted
//      only shows the "select a destination" reminder, 0x0044c628), fuel >=
//      100, and the ship to be outside the no-jump radius around the SYSTEM
//      CENTER (Stellar_ComputeTravelRangeSq 0x00465610; denial STR# 0x7d2
//      0x2a). Engaging clears the stellar selection (the reticle hides, the
//      nav panel switches to Hyperspace) and starts the brake.
//  (b) kBrake: while |round(vel)| >= 2 on either axis the ship faces the
//      REVERSE of its velocity and retro-thrusts once inside the facing window
//      max(class turn + 1, 20 deg), damping velocity by
//      g_jump_turnaround_velocity_damp (0x5755f0, 0.9920) per 30 Hz tick.
//  (c) kHold: velocity damps by g_hyperspace_slow_phase_velocity_damp
//      (0x5755f8, 0.9801), the hull turns onto the map bearing toward the
//      destination system, and the 'Warp up' cue plays. The fire lands when
//      the hold passes 30 ticks (30 Hz) and the cue has finished
//      (g_hyperspace_engage_hold_30hz 0x5755a8); the tunnel ramp schedule is
//      cue-relative (see (b)/(c) notes in the NovaTravel_Tick docs below).
//  (d) Fire (0x0044f3d0 area): position hurls 1350 px
//      (g_hyperspace_engage_velocity_hurl 0x57600) from the destination
//      center along the map bearing + 180 (the near side), velocity resets to
//      max speed along the current heading, fuel burns, the system changes
//      and control returns to normal flight immediately -- the ship streaks
//      through the new system while coasting.
// `travel_input` is the edge-triggered travel key state; `frame_time_ms`
// scales the phases; `warp_up_sound_active` reports whether the 'Warp up' cue
// (kHyperspaceWarpUpSoundKey) is still playing, gating the fire exactly like
// the original's audio latch. Reads/writes state.travel; sets just_completed
// the frame the jump lands. While engaging, the caller must skip player
// movement integration (the jump owns the ship). The completed jump does NOT
// re-spawn the starfield itself; the spaceflight loop observes just_completed
// and calls SpaceflightView::SpawnAmbientStars.
// ---- Galaxy discovery (fog of war) ---------------------------------------
// The original's per-system fog state is SystemDef.discovery_state (+0x90):
// 0 = unknown, >=1 = visited (in-flight jump arrival writes 1, landed/stellar
// travel and map-outfit reveals write 2), persisted per system as u16[0x800]
// in the pilot save (PilotFile_SaveGameCore 0x004c7dd0 / LoadSave 0x004cb260).
// The starmap draws a system when discovery_state > 0 or the transient
// discovered_this_rebuild latch is set (visited systems plus their one-hop
// link neighbours, recomputed by the rebuild pass).

// Ghidra 0x0046b9b0 System_ResolveSystemDiscoverySlot. Discovery state is
// booked on a system's visibility root when the twin grouping has remapped it;
// the clean-room keeps the root ids at -1 (grouping pass not reconstructed),
// so this degrades to the system id itself.
[[nodiscard]] std::int16_t
NovaSystem_ResolveDiscoverySlot(const GameState &state, std::int16_t system_id);

// Ghidra 0x0046b920 System_ResolveVisibleSystemForTravel. Follows the
// visibility root/parent chain to a visible system; degrades to the plain
// is_visible test while the twin grouping is not reconstructed.
[[nodiscard]] std::int16_t
NovaSystem_ResolveVisibleForTravel(const GameState &state,
                                   std::int16_t system_id);

// Ghidra 0x00468af0 System_HasUsableTravelDestination. True when the system
// lists at least one stellar that is a normal, reachable destination
// (travel_flags bit 0x20 clear and availability_flags & 0x3000 clear).
[[nodiscard]] bool
NovaSystem_HasUsableTravelDestination(const GameState &state,
                                      std::int16_t system_id);

// Marks one system visited at `level` (>=1): bumps discovery_state to `level`
// and mirrors into the persistent explored bitset. Ghidra: the discovery_state
// writes of System_FloodDiscoverAdjacentSystems 0x00467ab0 plus the arrival
// pre-latches (0x0044aa70 PlayerTick_SystemTransitionAndArrival / 0x00455e10
// Stellar_TravelToSystem).
void NovaSystem_MarkSystemVisited(GameState &state,
                                  std::int16_t zero_based_system_id,
                                  std::int16_t level);

// Ghidra 0x00467ab0 System_FloodDiscoverAdjacentSystems. Recursive flood over
// the system link graph up to `max_depth` links out, bumping each reached
// system's discovery_state to at least `threshold` and booking the state on
// the reached system's discovery slot. The original triggers system-region
// events per newly reached system (Frame_TriggerSystemEvents 0x00467bd0);
// TODO(decomp(0x00467bd0)) skipped: region trigger defs are not modelled.
// `visited` is the per-flood re-entry mask (the original's
// g_stellar_flood_visit_mask, DAT_007cc590), sized 0x800 by the caller. The
// original recurses only through systems that resolve visible via
// System_ResolveVisibleSystemForTravel; there that is availability visibility
// (is_visible is a loader-set load-time flag), so every loaded system's links
// flood - the clean-room recurses through any in-range link target, which
// matches that behaviour (the visibility-root remap is TODO(decomp)).
void NovaSystem_FloodDiscoverAdjacentSystems(
    GameState &state,
    std::int16_t zero_based_system_id,
    std::int16_t depth,
    std::int16_t max_depth,
    std::int16_t threshold,
    std::vector<std::uint8_t> &visited);

// Ghidra 0x00467970 System_RebuildSystemVisibilityMap. Clears the flood mask,
// floods from `origin_zero_based` up to `max_depth` links at discovery
// `threshold`, then rebuilds the transient discovered_this_rebuild latch:
// every visited system gets it, and so does every travel-resolvable link
// neighbour of one (that latch is what lets the starmap show one jump ahead
// without marking the neighbour visited). Also recomputed per tick by the
// original's System_UpdateSystemAndStellarDisplayState 0x00432470.
void NovaSystem_RebuildDiscoveryState(GameState &state,
                                      std::int16_t origin_zero_based,
                                      std::int16_t max_depth,
                                      std::int16_t threshold);

// Ghidra System_UpdateSystemAndStellarDisplayState (0x00432470) scope B, plus
// the latch tail of System_RebuildSystemVisibilityMap (0x00467970): propagates
// discovery_state across discovery-slot twins, clears the transient
// discovered_this_rebuild latch, then latches every visible visited system
// and every travel-resolvable link neighbour (the starmap's one-jump-ahead
// window). The original re-runs this every flight tick, so a script reveal
// (mïsn X) grows its grey neighbour ring immediately.
void NovaSystem_RebuildDiscoveredLatch(GameState &state);

// System-entry discovery: books the entered system (and its discovery slot)
// as visited at `level`, then rebuilds the map reveal from it. `level` is 1
// for an in-flight hyperspace arrival (0x0044aa70) and 2 for the landed
// stellar-travel arrival (0x00455e10 Stellar_TravelToSystem) and for
// map-outfit reveals (Outfit_GrantOutfitToPlayer 0x00427770, which floods
// deeper -- see NovaOutfit_ApplyMapReveal).
void NovaSystem_OnSystemEntered(GameState &state,
                                std::int16_t zero_based_system_id,
                                std::int16_t level);

// Ghidra 0x00467bd0 (the system-region event pass the flood invokes per newly
// reached system). For every n\x91bu nebula whose cached ActiveOn result is
// true and whose explored latch is still clear: when the system's position
// falls inside the nebula rect inset by 8, latch it explored and execute the
// nebula's OnExplore control-bit set expression once. (The original also
// prints a debug log line; we log at info instead.)
void NovaSystem_TriggerNebulaRegionEvents(GameState &state,
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
// key-binding-13 block in PlayerTick_TravelSelectionCommands (0x0044b8b9..
// 0x0044def6, g_playerCycleTravelTargetCommandLatch / travel_transfer_mode ==
// 3 / ai_secondary_target_slot++): a destination is only offered when its link
// slot resolves to a visible system through the twin chain. Each press
// advances (forward=true) or retreats (forward=false) one slot (wrapping); sets
// the travel slot + destination on state.travel so 'j' jumps there and the HUD
// shows the name. Returns the newly selected
// destination zero-based system id, or -1 when the current system has no
// travelable links.
[[nodiscard]] std::int16_t NovaTravel_CycleDestinationSystem(GameState &state,
                                                             bool forward);

// ---------------------------------------------------------------------------
// Plotted starmap route (Ghidra DAT_00735404 = state.travel.starmap_route).
//
// Shift-clicking systems in the galaxy map extends/truncates a multi-hop route
// anchored at the current system; the drawn chain is green. The route persists
// across map sessions and advances hop-by-hop as the player jumps.
// ---------------------------------------------------------------------------

// Ghidra 0x004a7e80 NovaUi_NormalizeStarmapRoutePlan. Drops a leading empty
// slot; clears the whole route when the first hop is missing.
void NovaStarmap_NormalizeRoutePlan(GameState &state);

// Ghidra 0x004a7fc0 System_NormalizePlannedRouteToCurrentSystem (called from
// the arrival tick). When the next plotted hop is the system just entered, the
// hop is consumed; when no hop is plotted the route resets to empty.
void NovaStarmap_NormalizeRouteToCurrentSystem(GameState &state);

// Ghidra 0x004a8080 NovaUi_SyncTravelSelectionFromStarmapRoute. Arms the
// player's travel slot from the first plotted hop when it is directly linked
// from the current system (the HUD then shows the plotted jump).
void NovaStarmap_SyncTravelSelectionFromRoute(GameState &state);

// True when the route has at least one plotted hop (Ghidra DAT_007dc744).
[[nodiscard]] bool NovaStarmap_RouteHasHops(const GameState &state);

// Edits the plotted route at the twin-resolved clicked system (the starmap's
// shift-click path, Ghidra 0x004a47cc): reset when the hit is on the current
// system's discovery slot, truncate when it slot-matches a plotted hop (that
// hop and everything after are cleared, then the hit is re-appended, so the
// route ends AT the clicked hop), pop the tail when it is a twin of it, and
// otherwise append when the tail is visited (or the hit latched) and the hit
// is a travel-resolvable adjacency of the tail. Returns true only when the
// click appended a hop (the original moves the map selection in that case
// alone).
bool NovaStarmap_EditRouteAtHop(GameState &state, std::int16_t hit);

// Resets the route to just the current system (the Clear Route button,
// 0x004a3aa0 action-8 branch) and disarms the plotted travel slot.
void NovaStarmap_ClearRoute(GameState &state);

// `warp_up_sound_active`: true while the 'Warp up' cue voice is still playing
// (SdlAudio::CountActiveByKey(kHyperspaceWarpUpSoundKey) > 0). The fire gate
// waits for it to finish once the hold passes 30 ticks, mirroring the
// original's NovaAudio_CountActiveByHandle latch.
void NovaTravel_Tick(GameState &state,
                     bool travel_input,
                     float frame_time_ms,
                     bool warp_up_sound_active = false);

// Ghidra 0x00459950 NovaUi_UpdateTravelEngagementProgress: once per player
// tick while a travel stellar is selected, advances the landing/docking
// approach. When the selected stellar is in the current system and the
// player's reputation/hazard/mission/government state admits it, the engage
// timer (state.travel.engage_timer) is incremented and armed to 0x2ee the
// first tick the ship is within 250 px on both axes, showing the
// "cleared to dock/land" overlay; it expires (> 0x7ff) back to -1, clearing
// the selection. Hypergate/wormhole (availability 0x1000/0x2000) and
// cannot-land (travel_flags 0x20) arms are deferred (the port's docking gate
// rejects those targets anyway).
void NovaTravel_UpdateEngagementProgress(GameState &state);

} // namespace game
