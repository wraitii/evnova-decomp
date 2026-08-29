#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace game {

struct GameState;

struct MissionListEvaluation {
  std::vector<std::int16_t> page_zero;
  std::vector<std::int16_t> page_one;
  bool has_return_mission = false;
};

// Locator warm-up and target-cache population corresponding to
// Misn_ResolveMissionStellarLocators (0x0043c3e0) and
// Mission_ResolveMissionStellarTargets (0x0043d240).
void Mission_ResolveMissionStellarLocators(GameState &state);

[[nodiscard]] MissionListEvaluation
Mission_EvaluateMissionLists(GameState &state);

// Mission IDs in this API are zero-based definition indices, matching the
// original mission lists and MisnActive.mission_template_id. Scenario resource
// IDs are translated at the ScenarioData boundary.
//
// Clean-room counterparts of the accepted-mission portions of
// Mission_PopulateMissionSlotFromDef (0x0043f8c0) and
// Mission_ActivateMissionAtSlot (0x0043f100). They include slot population,
// acceptance resource gates, counters, and rearm initialization; reaction
// scripts and UI refreshes remain outside this state-only API.
[[nodiscard]] bool Mission_PopulateActiveSlot(GameState &state,
                                              std::int16_t mission_id,
                                              std::size_t active_slot);

[[nodiscard]] bool Mission_ActivateAtSlot(GameState &state,
                                          std::int16_t mission_id);

// Ghidra 0x0046b920 System_ResolveVisibleSystemForTravel. Follows a system's
// visibility remap chain (twin-system links written by the scenario loader)
// until a visible system is found; -1 for out-of-range ids or chains with no
// visible member.
[[nodiscard]] std::int16_t
Misn_ResolveVisibleSystemForTravel(const GameState &state,
                                   std::int16_t system_id);

// Ghidra 0x00448910 Misn_TickActiveMissionTimers. Per-tick maintenance over
// the 16 active-mission runtime slots: resolves each mission's destination
// system, seeds the spawn/rearm timers and encounter odds from RNG, and
// clears transient counters. The original also re-ticks after mission script
// execution and in the landing flow; those call sites are not wired yet
// (TODO(decomp)).
void Misn_TickActiveMissionTimers(GameState &state);

// Ghidra 0x00447f20 Mission_CheckReactionConditionSatisfied. Tests a
// reaction/availability condition string with the shared NCB expression
// evaluator: empty strings pass, strings without a known expression head fail
// closed, and adjacent open parens are normalized with a space before
// evaluation.
[[nodiscard]] bool
Mission_CheckReactionConditionSatisfied(const GameState &state,
                                        std::string_view condition);

// Ghidra 0x00440aa0 Mission_ClearMisnSlotAssignments. Releases every ship
// assigned to the mission-fleet slot (clearing its fleet/targeting state and,
// in the original, despawning it while the travel scene owns the world),
// optionally runs the slot's resolve-script payload, then clears the slot's
// accepted/active latches. The original's g_travel_scene_ctx despawn arm and
// the ambient-roll latch are not modelled yet (TODO(decomp)).
void Mission_ClearMisnSlotAssignments(GameState &state,
                                      std::int16_t mission_slot,
                                      bool emit_completion_payload,
                                      std::uint32_t now_ms);

// Ghidra 0x00440410 Mission_ResolveMissionSuccess. Runs the success payload,
// applies the competing-government reputation delta across systems (equal
// governments in full, allies/hostiles scaled by 0.5), and applies the
// PayVal credit/reputation opcode. The success debrief dialog
// (MisnActive +0x3d) and the on-resolve availability reroll
// (ShipClass_RerollShipClassAvailabilityChances 0x00466cb0) are not
// reconstructed yet (TODO(decomp)); both are logged when they would fire.
void Mission_ResolveMissionSuccess(GameState &state, std::int16_t mission_slot);

// Ghidra 0x00440930 Mission_ResolveMissionFailure. Runs the failure payload,
// subtracts half the reputation delta from the competing government's
// systems, clears the slot, and releases assigned ships. The failure debrief
// dialog (MisnActive +0x3f) is not reconstructed yet (TODO(decomp)); it is
// logged when it would fire.
void Mission_ResolveMissionFailure(GameState &state,
                                   std::int16_t mission_slot,
                                   std::uint32_t now_ms);

} // namespace game
