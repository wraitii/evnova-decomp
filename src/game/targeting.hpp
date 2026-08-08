#pragma once

// Clean-room stellar targeting / selection / landing primitives.
//
// The original keeps these as small StellarDef/SystemDef helpers that gate who
// the player (and AI) may travel to or land on, plus a per-tick refresh that
// re-derives which stellars belong to and are playable in the current system.
// They are reconstructed here as pure predicates and geometry over the scenario
// tables + the player's GameState -- the self-contained subset that does not
// need the NPC ship-fleet container or the interaction dialogs. Each maps a
// named Ghidra function (address in the comment).
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
//   Ship_FindNearestEngagedTarget                0x00462850  (used to seed the
//     player's initial target on system entry)
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

// Landing selection (clean-room, built from the primitives above): returns the
// resource id of the nearest currently playable landing/travel stellar in the
// player's current system -- one the ship is within
// Stellar_ComputeTravelRangeSq of, is not a reserved travel lane, and passes
// StellarTargetsSpriteSetActive. Returns -1 when none qualifies. Used to seed
// the player's chosen landing target (a stand-in for
// Ship_FindNearestEngagedTarget 0x00462850's stellar leg until ship-AI
// targeting exists).
[[nodiscard]] std::int16_t
NovaTargeting_FindNearestLandableStellar(const GameState &state);

// True when `st` is a landing (non-travel) target the player can dock at.
// The original dispatches Stellars with availability_flags bit 0x1000 to
// Stellar_LandOnSpob; the remaining usable points proceed through travel or
// hypergate paths.
[[nodiscard]] bool NovaTargeting_IsLandableStellar(const Stellar &st);

// Per-frame player travel/land targeting. Seeds the nearest usable stellar in
// the current system, but preserves a player-cycled target while valid. Unlike
// the old shortcut, selection is not constrained to docking range: a target
// remains selected while the player flies toward it.
void NovaTargeting_UpdatePlayerTarget(GameState &state);

// Select the next (or previous when `forward` is false) playable stellar in
// the current system. Returns false when there is no eligible stellar.
bool NovaTargeting_CyclePlayerStellarTarget(GameState &state, bool forward);

// Performs a landing on the player's currently selected stellar (the landing-
// transition subset of Stellar_LandOnSpob / Stellar_ProcessTravelAndLanding's
// landing dispatch). Gated on a selected landable stellar that the ship is
// within travel range of. On success it repositions the ship to the stellar,
// refills shields/armor from the effective maximums, deducts the stellar's
// service cost (clamped >= 0 credits), and sets
// state.travel.landed_this_frame. The dock/world UI that follows a real
// landing is opened by NovaLanded_RunWindow in landed_window.cpp, not here
// (this function is the pure transition used by the spaceflight loop). Returns
// true when a landing was performed this call.
bool NovaLanding_TryLand(GameState &state);

// True when the player's currently selected stellar (state.travel.selected_
// stellar_id) is a landable target that has cleared the original's final
// docking envelope (within 250px in both axes and essentially at rest).
[[nodiscard]] bool NovaTargeting_IsLandingAvailable(const GameState &state);

} // namespace game
