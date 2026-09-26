#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace game {

struct GameState;

// Transient scope mirroring Ghidra g_mission_interaction_window (0x00774AE4),
// set only where the original actually owns the window (Mission BBS, offer
// window). Mission_ActivateMissionAtSlot (0x0043f100) reads it. Save/restore
// guards re-entrant windows; a nested S starts no window and inherits the
// enclosing value.
class MissionInteractionWindowScope {
public:
  explicit MissionInteractionWindowScope(GameState &state) noexcept;
  ~MissionInteractionWindowScope();
  MissionInteractionWindowScope(const MissionInteractionWindowScope &) = delete;
  MissionInteractionWindowScope &
  operator=(const MissionInteractionWindowScope &) = delete;

private:
  GameState *state_;
  bool previous_;
};

struct MissionListEvaluation {
  std::vector<std::int16_t> page_zero;
  std::vector<std::int16_t> page_one;
  bool has_return_mission = false;
};

// Locator warm-up and target-cache population corresponding to
// Misn_ResolveMissionStellarLocators (0x0043c3e0) and
// Mission_ResolveMissionStellarTargets (0x0043d240).
void Mission_ResolveMissionStellarLocators(GameState &state);

// Ghidra 0x0043d240 Mission_ResolveMissionStellarTargets. Resolves one
// zero-based mission definition into its offer-time target cache. Personality
// LinkMission spawns call this directly so their linked offer has fresh
// TravelStel/ReturnStel, cargo, pay, and deadline values.
void Mission_ResolveMissionStellarTargets(GameState &state,
                                          std::int16_t mission_id);

// Ghidra 0x00468b50 Mission_IsStellarValidRandomDestination. Bible rule for a
// randomly selected mission destination: it must be far enough from the
// offering system and guaranteed to exist for the whole game despite system
// swapping. Rejects a candidate in the reference stellar's system or a
// directly adjacent system, then requires the candidate to be a nav default
// in every system of its visibility-parent (same-coordinate twin) chain.
// `candidate` and `reference` are 0-based stellar indices; a reference outside
// the loaded table selects the chain-only arm (the offering/locator callers
// pass -1).
[[nodiscard]] bool Mission_IsStellarValidRandomDestination(
    const GameState &state, std::int16_t candidate, std::int16_t reference);

// Ghidra 0x0044a4d0 Ship_ExpandStringPlaceholders. Expands the desc/misn text
// placeholder blocks the original runs inside Ui_LoadSelectionDialogResource
// (0x004c6d50) on every desc load:
//   {g"a" "b"} / {G"a" "b"}  gender-conditional text (a = male arm, b =
//                            female arm; the player's 'm' latch chooses)
//   {pN"a" "b"}              registration conditional (the licensed game takes
//                            the first arm; unregistered compares the shareware
//                            day counter against N, ncb Pxxx semantics)
//   {bN"a" "b"}              Nova control bit N (DAT_005914cc)
//   !                         leading negator inside a block
// with backslash escapes inside the quoted strings. Unmatched '{' content is
// swallowed (the original's state 1 has no terminator branch -- quirk kept),
// and the negator latch persists across blocks (also kept).
// The port models a registered game, so {pN} keys on control.registered and
// {bN} reads control.bits. The trailing <PSRK>/<SSRK> ship-class cache pass has
// no port consumer yet.
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

// UI sink for the post-activation acceptance dialogs that the original runs
// inline in Mission_ActivateMissionAtSlot (0x0043f100): the Brief desc
// (payload +0x34) with starmap access, then the LoadCarg desc (payload +0x38)
// when PickupMode 0 puts the cargo aboard at accept. The flight/docked layer
// wires it to NovaMission_RunAcceptanceDialogs; because the mission-script S
// opcode activates missions from inside an on-accept/on-abort payload, the
// same sink is threaded through Mission_ActivateAtSlot so script-started
// missions still show their readers. Unset sinks keep the state-only port.
using MissionAcceptanceSink = std::function<void(std::int16_t mission_def)>;

// Reconstructs ShipState +0x6C in its original units: a 0-based g_stellar_defs
// index, or a raw 0..15 adjacency slot in the mode-3 plotted-route context
// (0x004a8080); -1 when none. Read at the TravelStel latch site after the
// OnAccept payload, matching 0x0043f100 (an M/N payload can repoint the live
// field); only the landed arm is reachable because the latch also requires an
// actual docked visit.
[[nodiscard]] std::int16_t
Mission_OriginalAiSecondaryTargetSlot(const GameState &state);

// The TravelStel predicate fires only while MissionInteractionWindowScope is
// active AND the player is actually docked, and it reads the navigation target
// at latch time (after the OnAccept payload). That landed requirement is a
// port-only BUGFIX(original): 0x0043f100 also accepted an in-flight target or
// plotted-route adjacency slot via the raw field compare.
[[nodiscard]] bool
Mission_ActivateAtSlot(GameState &state,
                       std::int16_t mission_id,
                       const MissionAcceptanceSink &acceptance = {});

// Ghidra 0x0046b920 System_ResolveVisibleSystemForTravel. Follows a system's
// visibility remap chain (twin-system links written by the scenario loader)
// until a visible system is found; -1 for out-of-range ids or chains with no
// visible member.
[[nodiscard]] std::int16_t
Misn_ResolveVisibleSystemForTravel(const GameState &state,
                                   std::int16_t system_id);

// Ghidra 0x00447a30 Mission_DoesSystemMatchMissionLocator. Tests a system
// against an active mission's auxiliary-fleet system locator (AuxShipSyst,
// MisnActive +0x65): the -1/-6
// player-system sentinels, -2/-3 TravelStel/ReturnStel containment, plain
// system ids, the 5000-adjacency and 10000..31999 government codes. Feeds
// the mission-fleet respawn dispatch (System_TickNpcSpawnMaintenance
// 0x0041d6e0).
[[nodiscard]] bool Mission_DoesSystemMatchMissionLocator(
    const GameState &state, std::int16_t system_id, std::int16_t mission_slot);

// Ghidra 0x00448910 Mission_RefreshActiveMissionSpawnState. Event-triggered
// refresh over the 16 active missions: arms ShipStart-1 delayed arrivals,
// resets their live-count latch, and refreshes auxiliary-fleet budgets/clocks.
// Called on system/stellar entry and from two mission-script command branches;
// it is not an ordinary per-frame tick.
void Mission_RefreshActiveMissionSpawnState(GameState &state);

// Ghidra 0x00457580 Stellar_HandleStellarEntryAndExit, restricted-travel
// follow-player fleet rearm seeding (disassembly 0x004588c0-0x00458a15). Runs
// after Mission_RefreshActiveMissionSpawnState and before
// System_RebuildInitialNpcAndMissionPopulation on a hypergate/wormhole
// transfer. `hypergate_transfer` selects the original's hypergate arm
// (local_120 != -1) over the wormhole arm; see the .cpp for the exact
// flags_secondary/random rules.
void Mission_RearmFollowPlayerFleetsForRestrictedTravel(
    GameState &state, bool hypergate_transfer);

// Tail of 0x00448910 (the per-mission blocks from the flags-0x10 live-count
// latch onward), which Stellar_RunDockAndLaunchSequence inlines verbatim at
// 0x004560a0 in its launch tail. The launch deliberately omits the
// ShipStart-1 delayed-arrival head, so call this rather than the full
// refresh when reproducing the launch.
void Mission_RearmActiveMissionTimers(GameState &state);

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
// when the destination window owns the world
// (state.travel_destination_window_open), despawning
// it), optionally runs the slot's on-abort payload (Bible OnAbort, +0x5e8),
// then clears the slot's accepted/active latches. `acceptance` is forwarded to
// any mission the on-abort payload starts via the S opcode. The ambient-roll
// latch is not modelled yet (TODO(decomp)).
void Mission_ClearMisnSlotAssignments(
    GameState &state,
    std::int16_t mission_slot,
    bool emit_completion_payload,
    std::uint32_t now_ms,
    const MissionAcceptanceSink &acceptance = {});

// Composed selection-dialog text plus the dësc trailing variant field, which
// the text reader consumes as custom art (PICT id >= 0x80 selects the
// DLOG 0xbbc arm). Callers that only have a bare string leave the variant 0.
struct MissionDialogText {
  std::string text;
  std::int16_t dialog_variant = 0;
};

// UI sink for the mission debrief text-reader dialogs (MisnActive +0x3d Comp
// on success, +0x3f Fail on failure). The landing gate invokes it with the
// composed dialog text and its variant; the flight-layer caller wires it to
// NovaUi_RunTextReaderDialog. Unset sinks log a TODO instead.
using MissionDebriefSink = std::function<void(const MissionDialogText &text)>;

// Ghidra Ui_LoadSelectionDialogResource (0x004c6d50) + Stellar_BuildTravel-
// DestinationDescription (0x004444f0): loads a dësc id, runs the ^-placeholder
// pass at load time, then the mission wildcard pass. `offering_list` selects
// the offer-row arm (mission_id = definition index); the active arm reads the
// accepted mission slot. Returns empty text when the resource is absent.
[[nodiscard]] MissionDialogText
Mission_LoadSelectionDialogText(const GameState &state,
                                std::uint16_t desc_id,
                                bool offering_list,
                                std::int16_t mission_id);

// Ghidra 0x00440410 Mission_ResolveMissionSuccess. Shows the success debrief
// dialog (+0x3d Comp dësc) through `debrief` when wired, runs the success
// payload, applies the competing-government reputation delta across systems
// (equal governments in full, allies/hostiles scaled by 0.5), and applies the
// PayVal credit/reputation opcode. The on-resolve repeat count (Bible
// DatePostInc) re-runs the daily world update once per count.
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
// resolve payload, on-resolve repeat count (DatePostInc daily world update),
// the Flags 0x0008 100-unit fuel penalty, Flags2 0x0002 pay application, the
// opt-in CompGovt/CompReward reputation walk for missions flagged Flags2
// 0x0002 or Flags 0x0040 (see docs/known_original_bugs.md and
// kApplyOriginalBugFixes), and slot teardown.
void Mission_ResolveMisnSlot(GameState &state,
                             std::int16_t mission_slot,
                             std::uint32_t now_ms);

// Ghidra 0x00443c60 Mission_HandleMissionOrSurrenderShipReaction. Per-tick
// objective evaluation for one active mission slot: drives the
// objective-complete/failed latches from the goal counters (ShipGoal 0-6),
// quick-fails overdue missions, and runs the completion payload / auto-abort
// resolution on the first objective-complete transition. `debrief` receives
// the ShipDone (+0x43) reader dialog on the first completion; unset sinks log
// a TODO instead.
void Mission_HandleMissionOrSurrenderShipReaction(
    GameState &state,
    std::int16_t mission_slot,
    std::uint32_t now_ms,
    const MissionDebriefSink &debrief = {});

// Ghidra 0x00443760 Mission_TickShipInteractionReactions. Per-tick driver
// over the 16 active-mission slots (TickSystems scope 0xb). `debrief` is
// forwarded to each slot's objective evaluation (ShipDone reader dialog).
void Mission_TickShipInteractionReactions(
    GameState &state,
    std::uint32_t now_ms,
    const MissionDebriefSink &debrief = {});

// Ghidra 0x00458802 slice of Stellar_HandleStellarEntryAndExit (and the game
// -start init at Ship_InitGameplayDataTables): redraws the per-definition
// offering roll (1..100) consumed by the AvailRandom gate of the mission
// offering eligibility chain. Call on every system arrival and when a new
// pilot starts.
void Mission_RerollOfferingRolls(GameState &state);

// Ghidra 0x0043bbb0 NovaResources_LoadMisnResourceDefs, runtime half. The
// m\xefsn definition decode itself runs inline in
// ScenarioData::LoadFromArchives (which owns the 1000-entry table); this resets
// the cross-cutting GameState the original clears around that decode: the
// in-flight/speaker/script-context latches, the option-gated active-slot and
// control-bit clear, and the interaction shown/context latches. Call alongside
// LoadFromArchives at session bootstrap (NovaGameSession_Run 0x00416100) and
// fresh-world reloads, so a second new game cannot inherit the previous
// pilot's mission interaction latches. The original's debug-option guards
// (FUN_004cd0b0) are unmodelled and documented as skipped at the definition.
void Mission_ResetRuntimeStateOnMissionDefsLoad(GameState &state);

// Outcome of one mission-offer interaction window (NovaUi_RunMissionOfferWindow
// 0x00442510 return values).
enum class MissionOfferResult {
  kAccepted,         // Mission_ActivateMissionAtSlot succeeded (return 1)
  kDeclined,         // player took the decline arm (return 0)
  kActivationFailed, // auto-accept/window accept failed (return -1)
};

// Ghidra 0x00448670 Mission_RunAvailLocOffers. Runs one
// mission-offer interaction for `context` (the original's first parameter,
// matched against MisnDef AvailLoc; the Spaceport loop calls it with 3 right
// after landing, the services windows with their own lanes). Clears the
// per-definition shown latch whenever the context changes away from 3, walks
// the lane-1 offering list for the first definition whose AvailLoc == context
// that still passes the eligibility chain and has not been shown, shows it via
// `run_offer`, and re-arms the recheck timer (DAT_00776af4 = now + rand(0..29)
// + 30, in 1/60 s ticks). An accepted offer removes the definition from the
// offering list -- the port re-evaluates lists on demand, and an activated
// mission fails the duplicate-active check on the next evaluation, so the
// removal is implicit. Returns true when an offer interaction ran.
[[nodiscard]] bool Mission_RunAvailLocOffers(
    GameState &state,
    std::int16_t context,
    std::uint32_t now_ms,
    const std::function<MissionOfferResult(std::int16_t mission_def)>
        &run_offer);

// Ghidra 0x004438d0 Mission_ProcessInteractionReactionSlotResources. Landing
// interaction pass for one slot: mission-cargo pickup/drop-off at the
// TravelStel and final delivery at the ReturnStel (Bible PickupMode /
// DropOffMode). `debrief` receives the LoadCargo (+0x39) / DumpCargo (+0x3b)
// reader dialogs the original runs inline; unset sinks log a TODO instead.
// `landed_stellar_id` is a 0-based stellar index (the driver rebases
// the 0x80-based context id).
void Mission_ProcessInteractionReactionSlotResources(
    GameState &state,
    std::int16_t mission_slot,
    std::int16_t landed_stellar_id,
    const MissionDebriefSink &debrief = {});

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
// showing the original's STR# 0x7d2 0x165/0x166 denial dialog through
// `debrief`) when total mass or free cargo space is short, otherwise dirties
// the inventory/loadout latch and succeeds.
[[nodiscard]] bool Mission_TryConsumeMissionInteractionResources(
    GameState &state,
    std::int16_t count,
    const MissionDebriefSink &debrief = {});

struct GameDate;
struct Ship;

// Ghidra 0x00466c40 Mission_AdvanceGameDate. Advances a game
// date one day: month lengths 31 / 30 / Feb 28 (29 on the original's leap
// quirk (year + (year < 0 ? 3 : 0)) & 3 == 0), rolling month -> year; a
// month past 12 rolls at the next month end (the clock-seeded start date can
// hold months > 12).
void Mission_AdvanceGameDate(GameDate &date);

// Ghidra 0x0043f080 Mission_ComputeDateAfterSteps. Returns a
// clone of the current in-game date advanced by `steps` days; the deadline
// date the mission-acceptance path stores into the slot runtime flags.
[[nodiscard]] GameDate Mission_ComputeDateAfterSteps(const GameState &state,
                                                     std::int16_t steps);

// Ghidra 0x00465550 Stellar_ComputeHyperspaceTravelDays. Jump duration in
// whole days: 1 day at <= 99 tons hull mass, 2 at 100-199, 3 above; the
// player additionally adds owned-count x ModVal for every owned outfit whose
// one of the four ModTypes is 0x16 (jump-time booster), clamped to >= 1.
// The outfit arm is player-only (ship_instance_id == 0). The arrival caller
// takes the max over the player and every attached ship (0x0044f8d6 walk,
// ported in FireJump); this helper evaluates one ship.
[[nodiscard]] int
NovaStellar_ComputeHyperspaceTravelDays(const GameState &state,
                                        const Ship &ship);

// Ghidra 0x00439500 Mission_TickDailyCronEvents. Once-per-game-day driver
// over the 0x200 crön event slots (see CronEventDef): date-window + odds
// + Require/EnableOn activation, duration/holdoff countdowns, and the
// OnStart/OnEnd set-strings through the reaction-script executor. Called by
// Mission_TickDailyWorldUpdate right after the calendar advance.
void Mission_TickDailyCronEvents(GameState &state);

// Ghidra 0x00423540 Player_CollectStellarTribute. Daily tribute pass over
// available stellars carrying the +0x46 marker. Called by
// Mission_TickDailyWorldUpdate.
void Player_CollectStellarTribute(GameState &state);

// Ghidra 0x00424f90 System_UpdateDisasterStates. Per-game-day sweep of the
// 0x100 öops slots (g_disaster_defs, DisasterDef stride 0x210). Undefined
// slots reset to the idle sentinels (-1 active/days, started_once 0); a
// defined idle slot rolls start_chance_percent and, when its ActivateOn
// expression passes, activates on the bound stellar (or a random available,
// non-travel-flagged one when target_stellar is -1) for duration_days; an
// active slot counts its remaining days down. Called from
// Mission_TickDailyWorldUpdate after the tribute pass.
void System_UpdateDisasterStates(GameState &state);

// Ghidra 0x00466cb0 ShipClass_RerollShipClassAvailabilityChances (the daily
// world-update driver; runs once per elapsed game-day). Advances the
// calendar, ticks the crön events, counts down every active mission's
// deadline, collects tribute income, updates the disaster states, runs the
// per-stellar garrison resupply + schedule countdown, and rerolls ship/outfit
// availability. The per-system reinforcement cooldown (SystemDef +0xC4
// reinf_cooldown_days; the decompiler also prints it as dude_prob +0x1c) is
// ticked via state.reinforcement_retrigger_delay. The active-rank daily
// salary (ränk Salary, runtime +0x0c) is paid here (0x00466d63).
void Mission_TickDailyWorldUpdate(GameState &state);

// Ghidra 0x00468450 NovaText_FormatDateString / 0x00468600
// Stellar_FormatElapsedTravelTime (shared body). Formats "MONTH DAYst, YEAR"
// from STR# 0x89 (st/nd/rd/th suffixes with the 11-13 -> th special case).
// The HUD/UI sites use the abbreviated month names (entries 13-24, the
// 0x00468450 shape); the <DL> mission token uses the full names (entries
// 1-12, the 0x00468600 shape).
[[nodiscard]] std::string
NovaText_FormatDateString(const GameDate &date,
                          bool abbreviated_month,
                          std::string_view prefix = {},
                          std::string_view suffix = {});

// Ghidra 0x00426d10 Mission_ShowMissionShipAnnouncement. Plays the mission-
// ship hail: transition-table cue 4, then the hail text -- `STR ` resource
// (hail_quote_id + 4999) when that resource exists, else entry
// hail_quote_id of STR# 7101 (the pers HailQuote pool) -- run through the
// desc placeholder expansion and the mission-wildcard pass (mission context
// cleared, so mission tokens expand to their [Error] sentinels and <OSN> to
// the speaking ship's personality name via state.mission_speaker_ship_slot).
// The caller latches mission_speaker_ship_slot around the call. Shows the
// result as a 420-tick (0x1a4) HUD overlay.
void Mission_ShowMissionShipAnnouncement(GameState &state,
                                         std::int16_t hail_quote_id);

// Ghidra 0x00426dd0 Mission_TrySpawnMissionShipAmbush. When the scenario
// defines the ambush personality (pers slot 0x3fe present + loaded), counts
// the current system's dominated/hazard stellars (is_available +
// dominated set, availability_flags 0x20 clear) and rolls 1-in-10 (one
// candidate) / 1-in-5 (several) per call. On a hit spawns the forced
// personality 0x3fe in the player's system, flips it hostile (whose pers
// Flags 0x10 arm may hail first), then latches the speaker and plays the
// personality's own hail announcement. Called from the system-arrival slice
// of Stellar_HandleStellarEntryAndExit (0x00457580).
void Mission_TrySpawnMissionShipAmbush(GameState &state);

// Ghidra 0x00448660 Mission_ClearActiveReactionMission. Clears the
// interaction-walk context latch (DAT_00774ae2 -> -1); the travel-services /
// outfit / shipyard windows call it on close.
void Mission_ClearActiveReactionMission(GameState &state);

// Ghidra 0x0046f140 Ship_HasAnyCargoLootOrActiveMission. True when the
// player carries any cargo, any junk, or has any active mission slot. Used
// by the player special-interaction window and its tab strip.
[[nodiscard]] bool Ship_HasAnyCargoLootOrActiveMission(const GameState &state);

// Ghidra 0x00441b40 Mission_CheckMissionShipInteractionEligibility public
// wrapper (the BBS list builder uses the internal offering slice with the
// interaction context clear and no reaction recompute). `interaction_context`
// mirrors the original's g_in_flight set around ship-offering calls:
// AvailLoc 2 defs are then the only eligible lane, the AvailStel locator gate
// relaxes to a pass, and the cargo gate switches to the "carrying >= 1 ton"
// arm. `recompute_reaction` is the original's second argument (param_2): when
// true the caller requested a re-evaluation of this definition's availability
// expression, cached at MisnDef +0x16 and read by eligibility gate [1]. The
// board command and the target-action hail pass true; the hail ladder passes
// false even though it also runs with the interaction context set.
[[nodiscard]] bool
Mission_CheckMissionShipInteractionEligibility(GameState &state,
                                               std::int16_t mission_id,
                                               bool interaction_context,
                                               bool recompute_reaction);

// Ghidra 0x00454910 Ship_HandlePlayerTargetActionCommand, post-accept arm
// (runs inline): after a mission offered by a personality ship is accepted,
// retires the personality when Flags 0x0100 requests it, sends the hailed ship
// into AI state 2 when Flags 0x0800 is set, and replaces a single-ship
// Flags 0x0040 fleet with a same-class ship while preserving its kinematics.
// Returns true only when the replacement was spawned and retargeted.
[[nodiscard]] bool Mission_HandleAcceptedShipInteraction(
    GameState &state, std::int16_t target_ship_slot, std::uint32_t now_ms);

// Ghidra 0x00433050 mission-hail ladder (runs inline in Ship_HandleShip,
// after the shield/armor recharge, before the velocity-match block).
// Per-frame eligibility ladder over the ship's personality Flags for the
// idle mission-ship hail: Flags 0x20 (disabled-hail pairing), 0x10 (distress
// hail with throttle bypass), 0x04 (loaded latch), 0x08 (government aid),
// 0x400 + LinkMission (offering eligibility), 0x800 (AI state 2),
// 0x1000/0x2000/0x4000 (player-class AI gates), 0x80 (no repeat). Fires on
// the distress override or a 1-in-0x8C roll gated on no overlay showing and
// the +0xAC re-hail window (+0xa8c ticks); on fire it latches the speaker,
// announces the pers hail, and sets the +0xBC hail latch and +0xAC tick.
void Mission_TickShipHailLadder(GameState &state,
                                Ship &ship,
                                std::int64_t now_60hz);

} // namespace game
