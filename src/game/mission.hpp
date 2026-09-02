#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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

// Ghidra 0x0044a4d0 Ship_ExpandStringPlaceholders. Expands the desc/misn text
// placeholder blocks the original runs inside Ui_LoadSelectionDialogResource
// (0x004c6d50) on every desc load:
//   {g"a" "b"} / {G"a" "b"}  gender-conditional text (a = male arm, b =
//                            female arm; the player's 'm' latch chooses)
//   {pN"a" "b"}              player-ship-count conditional (N)
//   {bN"a" "b"}              base-ship-count conditional (N)
//   !                         leading negator inside a block
// with backslash escapes inside the quoted strings. Unmatched '{' content is
// swallowed (the original's state 1 has no terminator branch -- quirk kept),
// and the negator latch persists across blocks (also kept).
// TODO(decomp): the {p}/{b} condition bodies (licensed/shareware day counter
// and the ship-count byte table at DAT_005914cc) are unresolved; the port
// currently evaluates both as true (the licensed-game {p} arm) and logs. The
// trailing <PSRK>/<SSRK> ship-class cache pass has no port consumer yet.
void Mission_ExpandStringPlaceholders(const GameState &state,
                                      std::string &text);

[[nodiscard]] MissionListEvaluation
Mission_EvaluateMissionLists(GameState &state);

// Ghidra 0x004444f0 Stellar_BuildTravelDestinationDescription (wildcard pass)
// + 0x00445d70 Mission_ReplaceSubstringInMissionText. Expands the Bible
// mission-text wildcards (<DST>, <DSY>, <RST>, <RSY>, <CT>, <CQ>, <SN>, <DL>,
// <PN>, <PNN>, <PSN>, <PST>, <OSN>, <PRK>, <SRK>, <RRK>, <PAY>, <REG>) in
// `text`. `offering_list` selects the offer-row arm (mission_id = definition
// index, targets from mission_target_resolutions); the active arm reads the
// accepted mission slot.
[[nodiscard]] std::string
Mission_ExpandMissionWildcards(const GameState &state,
                               std::string_view text,
                               bool offering_list,
                               std::int16_t mission_id);

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

// landed_stellar_id is the stellar the player is docked at when accepting
// (the BBS context) as a 0x80-based resource id; a mission whose resolved
// TravelStel matches it skips the initial destination briefing.
[[nodiscard]] bool Mission_ActivateAtSlot(GameState &state,
                                          std::int16_t mission_id,
                                          std::int16_t landed_stellar_id);

// Ghidra 0x0046b920 System_ResolveVisibleSystemForTravel. Follows a system's
// visibility remap chain (twin-system links written by the scenario loader)
// until a visible system is found; -1 for out-of-range ids or chains with no
// visible member.
[[nodiscard]] std::int16_t
Misn_ResolveVisibleSystemForTravel(const GameState &state,
                                   std::int16_t system_id);

// Ghidra 0x00447a30 Mission_DoesSystemMatchMissionLocator. Tests a system
// against an active mission's spawn locator (MisnActive +0x65): the -1/-6
// player-system sentinels, -2/-3 TravelStel/ReturnStel containment, plain
// system ids, the 5000-adjacency and 10000..31999 government codes. Feeds
// the mission-fleet respawn dispatch (System_TickNpcSpawnMaintenance
// 0x0041d6e0).
[[nodiscard]] bool Mission_DoesSystemMatchMissionLocator(
    const GameState &state, std::int16_t system_id, std::int16_t mission_slot);

// Ghidra 0x00448910 Misn_TickActiveMissionTimers. Per-tick maintenance over
// the 16 active-mission runtime slots: resolves each mission's destination
// system, seeds the spawn/rearm timers and encounter odds from RNG, and
// clears transient counters. The original also re-ticks after mission script
// execution and in the landing flow; those call sites are not wired yet
// (TODO(decomp)).
void Misn_TickActiveMissionTimers(GameState &state);

// Ghidra 0x00448090 NovaResources_EvaluateAvailability (system slice):
// re-evaluates every system's is_visible from its Visibility NCB, re-homes
// each nav stellar to the first visible claiming system, and relocates the
// player out of a current system whose visibility failed. The mïsn
// availability arm runs inline in Mission_EvaluateMissionLists.
void NovaResources_EvaluateAvailability(GameState &state);

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

// UI sink for the mission debrief text-reader dialogs (MisnActive +0x3d Comp
// on success, +0x3f Fail on failure). The landing gate invokes it with the
// composed dialog text; the flight-layer caller wires it to
// NovaUi_RunTextReaderDialog. Unset sinks keep the pre-port TODO logging.
using MissionDebriefSink = std::function<void(const std::string &text)>;

// Ghidra 0x00440410 Mission_ResolveMissionSuccess. Shows the success debrief
// dialog (+0x3d Comp dësc) through `debrief` when wired, runs the success
// payload, applies the competing-government reputation delta across systems
// (equal governments in full, allies/hostiles scaled by 0.5), and applies the
// PayVal credit/reputation opcode. The on-resolve availability reroll
// (ShipClass_RerollShipClassAvailabilityChances 0x00466cb0) is not
// reconstructed yet (TODO(decomp)); it is logged when it would fire.
void Mission_ResolveMissionSuccess(GameState &state,
                                   std::int16_t mission_slot,
                                   const MissionDebriefSink &debrief = {});

// Ghidra 0x00440930 Mission_ResolveMissionFailure. Runs the failure payload,
// subtracts half the reputation delta from the competing government's
// systems, clears the slot, shows the failure debrief dialog (+0x3f Fail
// dësc) through `debrief` when wired, and releases assigned ships.
void Mission_ResolveMissionFailure(GameState &state,
                                   std::int16_t mission_slot,
                                   std::uint32_t now_ms,
                                   const MissionDebriefSink &debrief = {});

// Ghidra 0x00440bf0 Mission_FailMissionSlotQuick. Immediate failure path:
// failure payload, failed latch, and ship release when the mission has
// placed any.
void Mission_FailMissionSlotQuick(GameState &state,
                                  std::int16_t mission_slot,
                                  std::uint32_t now_ms);

// Ghidra 0x00447d90 Mission_ResolveMisnSlot. Auto-abort/goal completion:
// resolve payload, optional daily rerolls (TODO(decomp)), the Flags 0x0008
// 100-unit fuel penalty, Flags2 0x0002 pay application, and slot teardown.
void Mission_ResolveMisnSlot(GameState &state,
                             std::int16_t mission_slot,
                             std::uint32_t now_ms);

// Ghidra 0x00443c60 Mission_HandleMissionOrSurrenderShipReaction. Per-tick
// objective evaluation for one active mission slot: drives the
// objective-complete/failed latches from the goal counters (ShipGoal 0-6),
// quick-fails overdue missions, and runs the completion payload / auto-abort
// resolution on the first objective-complete transition.
void Mission_HandleMissionOrSurrenderShipReaction(GameState &state,
                                                  std::int16_t mission_slot,
                                                  std::uint32_t now_ms);

// Ghidra 0x00443760 Mission_TickShipInteractionReactions. Per-tick driver
// over the 16 active-mission slots (TickSystems scope 0xb).
void Mission_TickShipInteractionReactions(GameState &state,
                                          std::uint32_t now_ms);

// Ghidra 0x00458802 slice of Stellar_ProcessTravelAndLanding (and the game
// -start init at Ship_InitGameplayDataTables): redraws the per-definition
// offering roll (1..100) consumed by the AvailRandom gate of the mission
// offering eligibility chain. Call on every system arrival and when a new
// pilot starts.
void Mission_RerollOfferingRolls(GameState &state);

// Outcome of one mission-offer interaction window (NovaUi_RunMissionShip-
// InteractionWindow 0x00442510 return values).
enum class MissionOfferResult {
  kAccepted,         // Mission_ActivateMissionAtSlot succeeded (return 1)
  kDeclined,         // player took the decline arm (return 0)
  kActivationFailed, // auto-accept/window accept failed (return -1)
};

// Ghidra 0x00448670 Mission_TriggerReturnMissionInteractions. Runs one
// mission-offer interaction for `context` (the original's first parameter,
// matched against MisnDef AvailLoc; the Spaceport loop calls it with 3 right
// after landing, the services windows with their own lanes). Clears the
// per-definition shown latch whenever the context changes away from 3, walks
// the lane-1 offering list for the first definition whose AvailLoc == context
// that still passes the eligibility chain and has not been shown, shows it via
// `run_offer`, and re-arms the recheck timer (DAT_00776af4 = now + rand(0..29)
// + 30). An accepted offer removes the definition from the offering list --
// the port re-evaluates lists on demand, and an activated mission fails the
// duplicate-active check on the next evaluation, so the removal is implicit.
// Returns true when an offer interaction ran.
[[nodiscard]] bool Mission_TriggerLandingInteractions(
    GameState &state,
    std::int16_t context,
    std::uint32_t now_ms,
    const std::function<MissionOfferResult(std::int16_t mission_def)>
        &run_offer);

// Ghidra 0x004438d0 Mission_ProcessInteractionReactionSlotResources. Landing
// interaction pass for one slot: mission-cargo pickup/drop-off at the
// TravelStel and final delivery at the ReturnStel (Bible PickupMode /
// DropOffMode). `landed_stellar_id` is a 0-based stellar index (the driver
// rebases the 0x80-based context id).
void Mission_ProcessInteractionReactionSlotResources(
    GameState &state,
    std::int16_t mission_slot,
    std::int16_t landed_stellar_id);

// Ghidra 0x00443780 Mission_TickReactionSlotsForTravelInteraction. The
// landing gate: evaluates objectives, processes cargo interactions, and
// resolves success/failure when docked at a mission's ReturnStel. `debrief`
// receives the success/failure debrief dialog texts. `landed_stellar_id` is
// the docked stellar as a 0x80-based resource id (the port's travel-context
// convention); it is rebased to the targets' 0-based index space internally.
void Mission_TickReactionSlotsForTravelInteraction(
    GameState &state,
    std::int16_t landed_stellar_id,
    std::uint32_t now_ms,
    const MissionDebriefSink &debrief = {});

// Ghidra 0x0046efd0 Stellar_AreStellarsEquivalent. Two stellar ids match when
// equal, or when their bodies share the same map position and display name
// (duplicate-resource twins). Ids are 0-based stellar indices (the original
// indexes g_stellar_defs directly); anything outside [0, 0x800) never
// matches. Callers holding a 0x80-based resource id must rebase first.
[[nodiscard]] bool NovaStellar_AreStellarsEquivalent(const GameState &state,
                                                     std::int16_t stellar_a,
                                                     std::int16_t stellar_b);

// Ghidra 0x00440370 Mission_TryConsumeMissionInteractionResources. Gates a
// mission interaction on the player hauling `count` tons: rejects (after
// showing the original's STR# 0x7d2 0x165/0x166 denial dialog, not yet
// reconstructed) when total mass or free cargo space is short, otherwise
// dirties the inventory/loadout latch and succeeds.
[[nodiscard]] bool
Mission_TryConsumeMissionInteractionResources(GameState &state,
                                              std::int16_t count);

} // namespace game
