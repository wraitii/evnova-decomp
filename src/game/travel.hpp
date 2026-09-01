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
//   kBrake -- while the ship still has velocity it turns around (at the class
//     turn rate) to face the REVERSE of its velocity (flying out from the
//     system, that points back toward the jump vector), brakes by
//     _DAT_005755f0 (0.992)/frame, and once within max(class turn+1, 20) deg of
//     that bearing (a facing window, not a turn speed) thrusts back along it,
//     ramping the engine glow by +3/frame to 24. Ends when the ship has come
//     to a stop (/vel/ < 0.5).
//   kHold -- finishes turning the hull onto the destination-system bearing.
//   kWarmup -- starts the rising 'Warp up' cue and holds for two seconds.
//   kZoom -- the engine glow ramps and the
//     ship thrusts to max speed along the jump heading so the ORIGIN system
//     parallaxes away (spaceflight view renders the streaking star tunnel via
//     the world movement delta). Duration approximates the original's
//     Stellar_GetJumpSequenceDurationMs / ShipClassDef.jump_duration_multiplier
//     (TODO(decomp)).
//   fire -- at the end of the zoom the boom/arrival lands: full-screen flash +
//     'Warp out' cue + system change, arriving in the NEW system at max speed;
//     control returns to normal flight (no separate post-fire tunnel phase).
// The original also stages jump audio (Stellar_TriggerHyperspaceAudioOnce,
// NovaAudio_PreStageJumpSoundBySeconds) and warp-syncs escort ships by jump
// depth (Stellar_ComputeShipJumpDepth); those remain documented divergences
// for a later pass.

#include <cstdint>

#include <vector>

#include "game_state.hpp"

namespace game {

// Number of fuel points a single jump burns. From the Bible "Fuel (100 = 1
// jump)" and the diagnostic jump gate Stellar_CanShipInitiateJumpSequence
// (class fuel_capacity >= 100) / the completion decrement `fuel_points -=
// FLOAT_kJumpFuelCost` (named 0x005755a4 in Ghidra).
inline constexpr float kJumpFuelCost = 100.0F;

// Ghidra 0x00462db0 Stellar_FindNearestAvailableTravelStellar: returns the
// travel-slot index (0..15) of the nearest available travel stellar in the
// player's current system, or -1 when no travel point qualifies. A travel
// stellar qualifies when it is present in the system's nav-defs, available,
// and has travel_flags bit 1. Restriction flags
// (availability_flags & 0x3000) require the ship to be within the jump range.
[[nodiscard]] int NovaTravel_FindNearestTravelPoint(const GameState &state);

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
// Handles (a) engaging a jump when the travel key is pressed near an available
// travel point, (b) running the engaged visible phases (brake onto the reverse
// of the jump vector, a short alignment hold, then the zoom thrust at max
// speed while the origin starfield streams away), and (c) completing the jump
// at the end of the zoom (fuel burn + system change + arrival at max speed +
// refill).
// `travel_input` is the edge-triggered travel key state; `frame_time_ms` scales
// the phases. Reads/writes state.travel; sets just_completed the frame the
// jump lands. Requires a valid scenario. While engaging, the caller must skip
// player movement integration (the jump owns the ship) and the spaceflight
// view branches on state.travel.engaging to render the star tunnel. The
// completed jump does NOT re-spawn the starfield itself; the spaceflight loop
// observes just_completed and calls SpaceflightView::SpawnAmbientStars.
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

// Marks one system visited at `level` (>=1): bumps discovery_state to `level`,
// and keeps the clean-room per-system fog bits + explored bitset in sync (the
// clean-room keeps is_visible/has_explored_flag as the targeting-fog record,
// while in the original both are load-time "syst exists" flags and targeting
// is not discovery-gated). Ghidra: the discovery_state writes of
// System_FloodDiscoverAdjacentSystems 0x00467ab0 plus the arrival pre-latches
// (0x0044aa70 PlayerTick_SystemTransitionAndArrival / 0x00455e10
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
// `visited` is the per-flood re-entry mask (the original's DAT_007cc590),
// sized 0x800 by the caller. Divergence: the original recurses only through
// systems that resolve visible (always true there -- is_visible is a load-time
// flag); the clean-room's is_visible is per-visit fog, so the flood recurses
// through any in-range link target or the reveal would die at the fog line.
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

// System-entry discovery: books the entered system (and its discovery slot)
// as visited at `level`, then rebuilds the map reveal from it. `level` is 1
// for an in-flight hyperspace arrival (0x0044aa70) and 2 for the landed
// stellar-travel arrival (0x00455e10 Stellar_TravelToSystem) and for
// map-outfit reveals (Outfit_GrantOutfitToPlayer 0x00427770, which floods
// deeper -- see NovaOutfit_ApplyMapReveal).
void NovaSystem_OnSystemEntered(GameState &state,
                                std::int16_t zero_based_system_id,
                                std::int16_t level);

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
