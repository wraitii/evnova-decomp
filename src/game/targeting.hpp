#pragma once

// Clean-room targeting / selection / landing primitives.
//
// Two independent target channels exist in the original: the *travel/landing
// stellar* (ai_secondary_target_slot, auto-seeded to the nearest playable
// stellar each frame, cycled with Tab) and the *primary target ship* (a slot
// in g_ship_states, selected with the backquote cycle, the nearest-hostile/
// engaged commands, or a mouse click). Both are reconstructed here as pure
// predicates and geometry over the scenario tables + the player's GameState
// -- the self-contained subset that does not need the NPC ship-fleet
// container or the interaction dialogs. Each maps a named Ghidra function
// (address in the comment).
//
//   Stellar_IsStellarActive                      0x0046E3C0  activity gate
//   Stellar_StellarTargetsSpriteSetActive        0x0046E3F0  targeting gate
//   Stellar_IsStellarUsableForTravel             0x0046E440  travel/land gate
//   Stellar_ComputeTravelRangeSq                 0x00465610  proximity radius
//   Stellar_IsStellarAdjacentToCurrentSystem     0x0040CD80  adjacency test
//   System_FindSystemContainingStellar           0x0046E790  locate stellar
//   System_UpdateSystemAndStellarDisplayState    0x00432470  (scope 3 only:
//     the per-tick re-derivation of stellar system_id + is_available for the
//     player's current system; the sprite-set bookkeeping is left to the view)
//   Ship_IsShipCloakVisibilityThresholdActive    0x0046C7A0  cloak gate
//   Ship_IsShipEligibleForDistressCall           0x0040F6D0  distress gate
//   Ship_IsShipAcquirableAsTarget                0x0040FAA0  acquire gate
//   Ship_FindNextPlayerCycleTarget               0x00461BD0  ship cycle fwd
//   Ship_FindPreviousPlayerCycleTarget           0x00461F60  ship cycle back
//   Ship_SelectNearestEngagedTarget              0x00462850  nearest engaged
//   Ship_SelectNearestHostileCombatTarget        0x00462BD0  nearest hostile
//   Outfit_HasScannerTargetUntargetableCapability 0x0046C930 (scanner gate)
//   Outfit_HasCloakScannerTargetCloakedCapability 0x0046CA60 (scanner gate)
//
// The runtime stellar "activity" state (sprite population / sprite handle
// active / engagement access counter) is carried on Stellar so the predicates
// stay faithful without entangling the sim in the SDL sprite engine; the
// renderer (SpaceflightView) drives those fields as it spawns/advances a
// stellar's ambient sprite, exactly as the original's sprite pass does.

#include <cstdint>

#include "game_state.hpp"
#include "scenario_data.hpp"

namespace game {

// Mirrors Stellar_IsStellarActive (0x0046E3C0): true when the StellarDef is
// "active" for rendering/targeting -- it has spawned at least one sprite (a
// non-zero sprite population) and the sprite is either presently loaded via a
// live handle or is engaged (its engagement access counter > 0). The original
// reads the sprite handle as a negative "reserved" sentinel; our Stellar
// records that state as `sprite_handle_active`. This drives sprite-set
// (link_a/link_b) selection and defence/target passes.
[[nodiscard]] bool NovaTargeting_IsStellarActive(const Stellar &st);

// Mirrors Stellar_StellarTargetsSpriteSetActive (0x0046E3F0): true when a
// StellarDef's sprite set matches its per-stellar targeting activity state.
// The control bit (travel_flags & 1) must be set and the stellar must be
// sprite-active exactly when its engaged flag (travel_flags & 0x80) is set
// (i.e. the two agree). Shared gate for travel/landing, radar colour and
// adjacent-stellar selection.
[[nodiscard]] bool
NovaTargeting_StellarTargetsSpriteSetActive(const Stellar &st);

// Mirrors Stellar_IsStellarUsableForTravel (0x0046E440): true when a StellarDef
// may be a travel/land destination -- it passes StellarTargetsSpriteSetActive
// and is not in the reserved travel-flag lane (availability_flags & 0x3000).
[[nodiscard]] bool NovaTargeting_IsStellarUsableForTravel(const Stellar &st);

// Mirrors Stellar_ComputeTravelRangeSq (0x00465610): the squared no-jump /
// travel-engagement radius in world px. Base 1000*1000, to which each owned
// outfit of ModType 23 (hyperspace distance modifier) adds
// `mod_val * owned_count`; the sum is clamped >= 0 then squared. Follows the
// original's gate on the player "primary" ship instance. Returns the squared
// threshold >= 0.
[[nodiscard]] float NovaTargeting_ComputeTravelRangeSq(const GameState &state);

// Mirrors Stellar_IsStellarAdjacentToCurrentSystem (0x0040CD80): returns true
// when the given stellar id appears in `sys`'s nav list. Preserves the
// original's quirk that a system with no nav entries at all reports "true"
// (degenerate adjacency). `stellar_id` is a resource stellar id (0x80..).
[[nodiscard]] bool
NovaTargeting_IsStellarAdjacentToSystem(const System &sys,
                                        std::int16_t stellar_id);

// Mirrors System_FindSystemContainingStellar (0x0046E790): returns the
// zero-based index of the first system whose nav list contains `stellar_id`,
// preferring `is_visible` systems before falling back to all systems, or -1
// when no system owns it.
[[nodiscard]] std::int16_t
NovaTargeting_FindSystemContainingStellar(const ScenarioData &scenario,
                                          std::int16_t stellar_id);

// Mirrors scope (3) of System_UpdateSystemAndStellarDisplayState (0x00432470):
// re-derives, for the player's current system, each stellar's owning
// `system_id` and `is_available` / `hazard_marker` flags. A stellar is called
// available when it belongs to (or is re-homed to) a visible system. This is
// the per-tick availability evaluation the spaceflight pre-loop currently skips
// (TODO in NovaFrame_SpaceflightLoop).
void NovaTargeting_UpdateStellarAvailability(GameState &state);

// ---------------------------------------------------------------------------
// Ship targeting (player primary target selection).
// ---------------------------------------------------------------------------
// The player's "primary target" is an NPC ship slot (or the player themselves
// at slot 0) selected by the ` cycle / Tab commands, the nearest-hostile/
// engaged commands or a mouse click. All selection paths share the same
// eligibility core: active, in the player's system, not destroyed, not in AI
// state 0x15, visible through the cloak gate (unless a cloak scanner or the
// combat-cycle modifier applies), and not flagged untargetable by the ship
// class (flags_secondary bit 2) unless the player owns the scanner-target-
// untargetable outfit. Ported from the Ghidra functions listed per helper.

// Mirrors Ship_IsShipCloakVisibilityThresholdActive (0x0046c7a0): true when
// the ship's cloak fade has crossed the targeting/visibility gate. Ghidra's
// strict comparisons use 24.0 while entering, 8.0 while clearing, and 16.0
// for the baseline branch; the signed transition latch selects the first two.
// Ship.cloak_fade_progress is advanced by the visual updater, not by weapon-hit
// damage.
[[nodiscard]] bool NovaTargeting_ShipAtCloakVisibilityThreshold(
    const Ship &ship);

// Mirrors Ship_IsShipEligibleForDistressCall (0x0040f6d0): true when the ship
// is an active, non-fire-restricted combatant that could call for help -- not
// coasting through a reversal (reverse_speed_bias <= 0), holding a primary
// target that is either the player or a ship targeting the player, and not in
// a retreat/disengage AI state (7,9,15,10,11,5,12,18). Used by the
// nearest-hostile scan and the player target-acquisition predicate.
[[nodiscard]] bool
NovaTargeting_IsShipEligibleForDistressCall(const GameState &state,
                                            const Ship &ship);

// Mirrors Ship_IsShipAcquirableAsTarget (0x0040faa0): pairwise predicate for
// whether `acquirer` should validly acquire `candidate` as a target. The
// player branch (acquirer.ship_instance_id == 0) returns true when the
// candidate's government policy flag 0 is set or the candidate is
// distress-eligible; the NPC branch keeps the candidate when it is the
// acquirer's primary target (or another active ship targets it while the
// acquirer tracks that ship) and the acquirer is not in a disengage/retreat
// AI state.
[[nodiscard]] bool NovaTargeting_IsShipAcquirableAsTarget(
    const GameState &state, const Ship &candidate, const Ship &acquirer);

// Ghidra Ship_ClearOtherShipsTargetingShip (0x00415dc0): retire target
// references as soon as an NPC reaches the destroyed state.
void NovaTargeting_ClearDestroyedShipReferences(GameState &state,
                                                std::int16_t destroyed_slot);

// Mirrors Ship_FindNextPlayerCycleTarget (0x00461bd0) / _Previous
// (0x00461f60): returns the next (or previous) eligible ship slot after
// `current_slot` within `system_id`, wrapping from slot 1 (0 is the player;
// -1 starts the search at the first/last slot). `include_combat` mirrors the
// original's held modifier (input commands 0x1d Left-Ctrl / 0x6b 'k'): when
// true only combat-relevant ships (targeting the player or a ship that
// targets the player, excluding mission-fleet escorts) are candidates;
// otherwise only non-relevant ships are. Returns `current_slot` unchanged
// when no candidate exists (the caller clears the target on a no-op).
[[nodiscard]] std::int16_t NovaTargeting_FindNextPlayerCycleTarget(
    const GameState &state,
    std::int16_t current_slot,
    std::int16_t system_id,
    bool include_combat);
[[nodiscard]] std::int16_t NovaTargeting_FindPreviousPlayerCycleTarget(
    const GameState &state,
    std::int16_t current_slot,
    std::int16_t system_id,
    bool include_combat);

// Mirrors Ship_SelectNearestEngagedTarget (0x00462850): nearest active,
// non-destroyed ship in the player's system that is visible through the cloak
// gate (or the player has a cloak scanner), not in AI state 0x15, class
// not untargetable (or scanner), and whose ai_target_ship_slot is NOT the
// player (ships already locked onto the player are excluded). Returns the
// slot or -1 when none.
[[nodiscard]] std::int16_t
NovaTargeting_SelectNearestEngagedTarget(const GameState &state);

// Mirrors Ship_SelectNearestHostileCombatTarget (0x00462bd0): stricter scan
// than SelectNearestEngagedTarget -- the candidate must additionally be
// eligible for a distress call, or locked on its primary target (which must
// be targeting the player) in AI state 0x04, and not fire-restricted.
// Returns the slot or -1 when none.
[[nodiscard]] std::int16_t
NovaTargeting_SelectNearestHostileCombatTarget(const GameState &state);

// Per-frame player travel targeting. Normal stellar selection requires only a
// current-system available stellar with travel_flags bit 1. 0x3000 special
// lanes additionally require NovaTargeting_ComputeTravelRangeSq proximity.
// A player-cycled target remains selected while valid.
void NovaTargeting_UpdatePlayerTarget(GameState &state);

// Select the next (or previous when `forward` is false) playable stellar in
// the current system. Returns false when there is no eligible stellar.
bool NovaTargeting_CyclePlayerStellarTarget(GameState &state, bool forward);

// Mirrors Ship_HandlePlayerTargetActionCommand's stellar branch: a target
// action can open the destination-interaction window only for an available,
// unrestricted, sprite-active target with travel_flags bit 0x20 clear. This
// deliberately does not imply docking or a physical collision.
[[nodiscard]] bool
NovaTargeting_CanOpenTravelDestinationInteraction(const GameState &state);

} // namespace game
