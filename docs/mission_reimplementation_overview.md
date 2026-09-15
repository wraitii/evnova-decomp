# Mission reimplementation overview

Status update (after the 2025 mission-loop passes): the core loop is
implemented end to end for non-special-ship missions. `src/game/mission.cpp`,
`mission_script.cpp`, and the landing path in `spaceflight.cpp` now cover the
data model, BBS evaluation/activation, the timer tick, per-tick objective
evaluation, the landing gate, and success/failure resolution with the PayVal
credit/reputation opcode. The daily driver, `Mission_TickDailyWorldUpdate`
(0x00466CB0), advances the in-game calendar (GameDate, seeded from the local
clock with year+250 at new game, round-tripped in the .plt block1 +0x14/16/18),
counts down mission deadlines, and runs on hyperspace arrival (1/2/3 days by
hull mass), landing (1 day) and launch (15..44 docked days); DatePostInc
re-runs it per count. The driver now also ticks crön events, stellar tribute
income, the per-stellar garrison/schedule countdown, ship/outfit availability
rerolls, and the öops disaster states (System_UpdateDisasterStates 0x00424F90).
Still open in the driver: the rank (ränk) daily salary (the rank table is not
modelled). The per-system reinforcement cooldown (`SystemDef +0xC4`
`reinf_cooldown_days`, which the decompiler prints as `dude_prob +0x1c`) is
ticked as `state.reinforcement_retrigger_delay`. See the progress tracker for
per-function percentages.

## Disasters (öops)

`ScenarioData::disaster_defs` holds the 0x100 öops slots (id 0x80 + i,
stride 0x210) decoded by `DecodeDisaster`; the payload is five big-endian
words (target stellar, commodity, signed price delta, duration days, per-day
start chance) plus the ActivateOn C string, and the record name is the display
label. `System_UpdateDisasterStates` runs each game-day from the driver: a
defined idle slot rolls the chance, tests ActivateOn, then activates on its
bound stellar (or a random available, non-travel-flagged stellar for target
-1) for the duration; active slots count down. The Bar/travel-news report
now renders active disasters (`Bar_ComposeDisasterReport` in docked_dialog).
The remaining consumer is the commodity-exchange price delta
(NovaUi_HandleTravelDestinationInteractionLoop 0x0048c730, not ported); the
.plt block2 +0x3088/+0x3288 runtime persistence is also untracked.

## Verified m\xefsn payload map (offsets ground-truthed against the loader
0x0043BBB0, populate 0x0043F8C0, and the EVN Bible; the loader skips payload
+0x002, so the Bible's AvailLoc/Record/Rating/Random
sit at +0x004/+0x006/+0x008/+0x00a and TravelStel/ReturnStel at
+0x00c/+0x00e)

```text
+0x000 AvailStel      +0x004 AvailLoc       +0x006 AvailRecord
+0x008 AvailRating    +0x00a AvailRandom    +0x00c TravelStel
+0x00e ReturnStel     +0x010 CargoType      +0x012 CargoQty (tons)
+0x014 PickupMode     +0x016 DropOffMode    +0x018 ScanMask
+0x01c PayVal (4b)    +0x020 ShipCount      +0x022 ShipSyst (-6 = follow player)
+0x024 ShipDude       +0x026 ShipGoal       +0x028 ShipBehav
+0x02a ShipNameID     +0x02c ShipStart      +0x02e CompGovt
+0x030 CompReward     +0x032 ShipSubtitle   +0x034..+0x03e desc ids
                      (Brief, QuickBrief, LoadCarg, DumpCargo, Comp, Fail)
+0x040 TimeLimit      +0x042 CanAbort       +0x044 ShipDoneText   +0x048 AuxShipCount
+0x04a AuxShipDude    +0x04c AuxShipSyst    +0x050 Flags / +0x052 Flags2
+0x058 desc id (slot +0x41, provisional)    +0x05a Ship restriction
+0x05c Availability expr                    +0x656/+0x65a Require mask (8b)
+0x7a0 list priority
```

MisnActive highlights: +0x00 TravelStel / +0x04 ReturnStel (resolved),
+0x12/+0x14 resolved CargoType/CargoQty, +0x16/+0x18/+0x1a
PickupMode/DropOffMode/ScanMask, +0x32 can_abort (the mïsn CanAbort flag,
payload +0x42), +0x33
carrying latch, +0x35..+0x43 desc ids
(+0x41 from payload +0x58, provisional; +0x43 ShipDone), +0x45 TimeLimit
countdown (-32000 = none). The Ghidra struct comments carry the same map.

## 1. Mission data model and loading

Ghidra identifies these mission-related runtime types:

- `MisnDef`: 300 bytes, up to 1000 definitions
- `PersDef (Ghidra type still MissionShipDef)`: 1940 bytes
- `MisnActive`: 2278 bytes, 16 active slots
- `MisnRuntimeFlags`: 20 bytes per slot

Primary functions:

- `0x0043BBB0` `NovaResources_LoadMisnResourceDefs`
- `0x0043C3E0` mission stellar-locator resolution
- `0x0043D240` mission stellar-target resolution
- `0x0043F8C0` active-mission-slot population

DONE: the clean-room model lives in `src/game/scenario_data.hpp`
(`MissionDef`) and `src/game/game_state.hpp` (`ActiveMission`,
`MissionRuntimeFlags`); `ScenarioData` decodes the m\xefsn family and the
pilot file round-trips active missions.

## 2. Mission-board availability and activation

**Stellar id conventions (verified against 0x0043d240/0x0046efd0):** resolved
mission targets (MisnActive +0x00/+0x04 and the DAT_00776b04 target table) are
**0-based stellar indices** — the original indexes `g_stellar_defs[]` directly
with them. The port's travel/landed context (`GameState::travel.selected_
stellar_id`, `LandedContext.stellar_id`) keeps **0x80-based resource ids**;
mission APIs taking a landed stellar rebase at the boundary
(`Mission_TickReactionSlotsForTravelInteraction`, `Mission_ActivateAtSlot`).
Comparing the two spaces directly silently broke tutorial-001 completion (the
ReturnStel gate never matched).

### 2.1 Random mission destination validity (0x00468b50)

A randomly selected stellar destination must satisfy the EV Nova Bible rule
(m\xefsn section): *the destination must be far enough from the system where
the mission is offered and must be guaranteed to exist throughout the game,
regardless of system swapping*. The gate for this is Ghidra `0x00468b50`,
**renamed** from the misleading `Stellar_IsStellarReachableForTravel` to
`Mission_IsStellarValidRandomDestination` (it is not physical travel
reachability). It rejects a candidate in the reference stellar's system or a
directly adjacent system, then walks the candidate system's
visibility-parent (same-coordinate twin) chain and requires the candidate to
be a nav default (`SystemDef.nav_stellar_ids`) in **every** system of the
chain - so a stellar missing from any duplicate system is never chosen
(`StellarDef.is_defined` also gates out stellars no system hosts). The port
lives in `mission.cpp` (`Mission_IsStellarValidRandomDestination`) and is
called from the stellar-locator candidate scan; the twin chain uses the
already-ported `NovaSystem_ResolveDiscoverySlot` (`0x0046b9b0`). The
reference/current stellar (the `param_2` anchor from
`Mission_ResolveMissionStellarTargets` `0x0043d240`) is now plumbed from the
flight selection or the current system's first nav default in the docked
context. The remaining fidelity difference is selection cadence: the original
first proves that one of its fixed 0x800 stellar slots is eligible, then draws
uniform random slot numbers until one passes; the port builds the eligible
vector and draws once from it. Both are uniform over eligible candidates, but
consume the RNG stream differently.

Related rename: Ghidra `0x00447f00 System_GetSystemDefFlagByte` is exactly the
`SystemDef.is_visible` (+0x1eb) predicate and was renamed
`System_IsSystemVisible` (port `NovaSystem_IsSystemVisible`).

The mission-list pipeline is implemented in `src/game/mission.cpp`:

- `0x0043CF00` `Mission_EvaluateMissionLists` — DONE (85%): two offering
  lanes (0 = mission computer, 1 = bar/services) driven by the
  `0x00441B40` eligibility chain; stable **descending** `list_priority`
  order (the original's bucket pass emits the highest MisnDef +0x128
  priority first).TODO(decomp): per-eligible-def target resolution and the
  `g_return_mission_list` finalize arm.
- `0x00441B40` `Mission_CheckMissionShipInteractionEligibility` — offering
  slice DONE (60%): the ten-gate chain (AvailStel locator families incl.
  the 31000-lane −30000 binary quirk, availability expression,
  AvailRecord vs `g_system_reputation`, AvailRating, AvailRandom vs the
  per-definition warp roll (`g_mission_offering_rolls`, DAT_00734c20,
  redrawn 1..100 on arrival and zeroed on duplicate accept), Flags2-0x0001
  cargo space, the 64-bit Require mask (`mïsn +0x656/+0x65a` via
  `NovaOutfit_EvaluateRequireMask`), the Ship restriction (+0x5a), Flags
  0x2000/0x4000 class arms, the PayVal credits gate, locator-candidate
  sanity, and the same-system (visibility-root) denial). The on-landing
  offer pass consumes it via the lane-1 walk below; the reaction-condition
  cache refresh of the original's param_2 callers remains TODO(decomp).
- Calendar/deadline plumbing (2025 dates pass): `GameDate` in
  `game_state.hpp`; `Mission_AdvanceGameDate` (0x00466c40),
  `Mission_ComputeDateAfterSteps` (0x0043f080, stores the absolute deadline
  into the runtime flags +0x06/+0x08/+0x0a at acceptance),
  `NovaStellar_ComputeHyperspaceTravelDays` (0x00465550), and
  `NovaText_FormatDateString` (0x00468450/0x00468600, STR# 0x89). Wired into
  the BBS/mission-info/starmap date lines and the `<DL>` token (active-slot
  arm; deadline==today -> empty string).

- `0x0044A4D0` `Ship_ExpandStringPlaceholders` — DONE (80%) as
  `Mission_ExpandStringPlaceholders`: the `{g}/{G}/{pN}/{bN}/!` placeholder
  state machine with escapes (quirks kept), gender arm reading the 'm'
  latch; wired at the desc consumers like the original's load-time pass in
  `Ui_LoadSelectionDialogResource`. TODO(decomp): {p} shareware arm, {b}
  byte table, <PSRK>/<SSRK> cache.
- `0x004982A0` `Ui_RunTravelSelectionDialog` — ported (70%) as
  `NovaUi_RunTextReaderDialog` (`selection_text_dialog.cpp`) on the shared
  `NovaTextScrollView` (clean-room `NovaTextView` 0x004BCD90 family):
  DLOG 0xbbb geometry, 3-PICT backdrop (0x214d body / 0x214c top strip /
  0x214e bottom strip, 0x00499870 order), auto-shrink to the
  measured pure-text height (clamped to 0x30; entry 3 bottom rises, entries
  1/5/6 shift up, window + children drop round(shrink*0.3)), ±10px
  scrolling with drawn arrow states, starmap action. TODO(decomp): variant ≥ 0x80 DLOG 0xbbc arm, status-string
  display, starmap preselect, static-surface redraw variants.
- `0x0043F100` `Mission_ActivateMissionAtSlot` — the on-accept payload
  (mïsn +0x15b set-expression: chain bits, X system-reveal, S auto-start) runs
  at the end of `Mission_ActivateAtSlot` like the original. The acceptance UI
  chain (Brief dialog payload +0x34 with starmap access, LoadCarg dialog +0x38
  on PickupMode 0) runs after activation via `NovaMission_RunAcceptanceDialogs`
  from both the offer-window and Mission BBS accept paths; the decline arm
  of the offer window shows the payload +0x58 desc (if any) and executes the
  +0x25a reaction script via `Mission_ExecuteReactionScript`.
- `0x00448670` `Mission_TriggerReturnMissionInteractions` — DONE (85%) as
  `Mission_TriggerLandingInteractions` (mission.cpp): context latch
  (DAT_00774ae2) with the non-3 wholesale clear of the shown latches
  (DAT_00773eed), lane-1 walk for the first `AvailLoc == context`
  definition still passing eligibility, offer via a `run_offer` callback,
  −1-return latch, and the DAT_00776af4 recheck timer. The Spaceport
  wiring (0x00491F30's `g_misn_list_page_group = 3` + context-3 call on
  landing) runs in `NovaLanded_RunWindow` over a dock snapshot.
- `0x00442510` `NovaUi_RunMissionShipInteractionWindow` — partial (35%) as
  `NovaMission_RunOfferWindow` (docked_mission_dialog.cpp): the text-offer arm over
  DLOG 0x3f8/DITL 1016 with the STR# 0x96 button defaults, the +0x18-&4
  empty-text auto-accept arm, and accept via `Mission_ActivateAtSlot`.
  TODO(decomp): mission-ship/hail branches, the variant ≥ 0x80 DLOG 0x3fc
  art path, status-string panel, nested starmap/special/mission-computer
  actions, text-view scrolling, and the decline-arm reaction script.
- `0x0043F100` `Mission_ActivateMissionAtSlot` — DONE (BBS passes the landed
  stellar; script `S` opcode passes `ai_secondary_target_slot`)
- `0x00447F20` mission condition-expression evaluation — DONE
- `0x0043F080` deadline-date calculation — DONE
- `0x00447A30` system/locator matching — DONE (clean-room locator resolver)

This layer determines which missions appear, sorts them, checks availability expressions, resolves destinations, and accepts a mission into one of 16 active slots.

The Mission BBS UI is a functional port in `src/game/docked_mission_dialog.cpp`
(evaluates available missions, DLOG/DITL 0x3ee layout, selection, accept via
`Mission_ActivateAtSlot`), though desc/briefing dialogs are still logged
TODOs. Ghidra distinguishes this from the active-mission computer and from
the galaxy map:

- `0x0043C470` `NovaUi_RunMissionBbsWindow` is the available-mission
  BBS. It opens DLOG `0x3EE`, loads PICT `0x2139`, rebuilds the available mission
  rows, shows the selected mission description (`mission_id + 4000`), and calls
  `Mission_ActivateMissionAtSlot` when the player accepts a row.
- `0x004612C0` `NovaUi_DrawCargoMissionStatusPanel` is the in-flight summary
  of cargo and mission state, not the BBS.
- `0x00446150` `NovaUi_RunMissionComputerWindow` reviews already-accepted
  missions. It supports destination context, starmap access, and aborting
  eligible active missions.
- `0x0047C8E0` `NovaUi_RunTravelDestinationServicesWindow` is the Bar/services
  modal, handling news, gambling, and escort-related actions; it is not the
  Mission BBS.

In our clean-room terminology, “destination main window” means the landed
current-pilot/travel interaction context associated with the player’s current
profile and selected stellar. It is distinct from the galaxy map window and
should not be used as a name for the map itself. The BBS implementation target
is therefore:

```text
landed Mission BBS button
  -> available-mission list
  -> select mission and inspect description
  -> Mission_ActivateAtSlot
  -> return to the landed window
```

The existing `0x2139` frame in `src/game/docked_mission_dialog.cpp` is the correct BBS
artwork, but its content should become this available-mission list rather than
the generic placeholder.

Remaining dialog entrypoints:

- `0x00440C90` `NovaUi_PollMissionBbsWindow` polling — ported inline (35%) in
  `RunMissionBbsWindow` (`docked_mission_dialog.cpp`); remaining action codes and the
  native scrollbar are TODO(decomp)
- `0x00446150` mission-computer window — DONE (75%) as
  `NovaMission_RunMissionInfoWindow` (`docked_mission_dialog.cpp`): the in-flight
  active-missions modal (gameplay command 0x28, default key I; the caller in
  `spaceflight.cpp` counts visible missions first and plays the denied cue +
  STR# 0x7d2 0x162 overlay when there are none). DLOG/DITL 0x3f4 + PICT
  0x2145 layout, the 0x00445dc0 list rebuild (skips flags 0x400 invisible
  missions; failed rows keep the DAT_0056c328 0xa5 marker), the selected
  mission's quick-brief desc through the placeholder + wildcard passes, the
  Abort/Done button pair (STR# 0x96 0x23/0x5; Abort enabled on MisnActive
  +0x32 `can_abort`), the starmap action with the flags 0x100 destination
  preselect, and the abort arm (flags 0x40 → −5× CompReward over every
  system the CompGovt owns, then `Mission_ClearMisnSlotAssignments(slot,
  1)`). Rendering: all window text uses the shared
  Geneva-9 screen font (DAT_00735684/86, set in 0x004b0c20); list rows sit
  on the fixed 12px native pitch (DAT_0088c01c 8 × 1.5 scale at
  0x005754f8) with matching click hit-testing (click past the last row
  deselects, per 0x004d1db0); the description panel is black with white
  wrapped text — the fill + InvertRect cancel of 0x00446e00, not a white
  panel. Skips: game-calendar date line, ai_secondary_target_slot restore
  around a nested map destination window, list scrollbar. It renders the
  live flight view beneath itself — like every modal in the port, per the
  layering divergence documented in docs/dlog_ditl_dialog_format.md §7.1
  (the docked-layer snapshot plumbing was removed in the same pass).
- `0x0043C470` available Mission BBS — ported in `docked_mission_dialog.cpp`
  (`RunMissionBbsWindow`); 2025 fidelity pass: Geneva-9 screen font and
  fixed 12px native list row pitch, selected-title panel in white Times 18
  with a black no-selection fill, description panel as the fill+InvertRect
  black/white pattern of the draw `NovaUi_DrawMissionBbsWindow` 0x00441620,
  heading/date greys
  (DAT_00733b50/5c), and mission display names strip the ';'-subtitle per
  the 0x0043bbb0 loader (NameString_StripSubtitleSuffix 0x004cd230).

## 3. Mission completion and failure

Implemented in `src/game/mission.cpp` / `mission_script.cpp`:

- `0x00440410` success — DONE. The +0x3d Comp dësc debrief text-reader dialog
  runs through the landing-gate debrief sink (`MissionDebriefSink` wired to
  `NovaUi_RunTextReaderDialog` in spaceflight.cpp; layered over the live
  flight view, whereas the original shows it over the destination-window
  context — logged divergence).
- `0x00440930` failure — DONE (same debrief-sink path for the +0x3f Fail
  dësc).
- `0x00447D90` active mission resolution — DONE
- `0x00440AA0` mission/fleet assignment cleanup — DONE
- `0x00440BF0` quick failure — DONE
- `0x00448670` return-mission interactions — DONE (remaining services-window
  timer consumers are tracked separately)
- `0x00440750` `Government_ApplyReputationCreditDelta` (PayVal) — DONE, full
  encoded range map verified against disasm + Bible
- `0x00466CB0` daily driver — DONE for modeled data; rank salary and the
  unmodeled per-system suppression table remain skipped

These apply credits, reputation, follow-up missions, text, ship cleanup, and script payloads.

## 4. Reaction scripts and timers

The scripting subsystem includes:

- `0x00448020` reaction-condition checking — DONE (0x00447F20 port)
- `0x00448050` script payload execution — DONE (entrypoint, shared executor)
- `0x00449370` mission script engine — ~75% (Bible opcodes A/F/S/G/D/M/N/C/
  E/H/K/L/P/Q/T/U/X/Y; grammar still partially decoded)
- `0x00448910` active-mission timer tick — DONE (player-tick call site)
- `0x00466C40` calendar advancement — DONE
- `0x00872040` `Debug_HandleCheatAndNcbCommands` — unported debug/cheat path;
  renamed in Ghidra from the misleading mission-trigger name

The interpreter can mutate ships, active missions, rank definitions
(g_rank_defs), stellar state, outfits, weapon/ship availability, and mission
lists. Its command grammar is only partially decoded, so it should follow the
data-model and activation work.

## 5. Mission ships and fleets

Mission ships and fleets are now implemented end to end for the spawn/dispatch
half (the `0x0041CF40` + dispatch pass):

- `0x0041CF40` mission-fleet spawn from a dude def — DONE
  (`NovaMission_SpawnMissionShipFromDudeDef` in `ship_spawn.cpp`): forced-class
  special-ship-type lock (flags 0x0800 + single-ship fleets), weighted type
  selection with the ignore-availability fallback, identity
  (`mission_fleet_slot` + `dude_class_id`), government, AI behavior (dude
  ai_type or class default), class vitals/skill variance/waypoint marker, and
  the ShipBehav 0 hostile arm. The eager per-weapon ammo-table copy is
  replaced by the lazy `NovaWeapon_EnsureNpcWeaponBanks` (same loadout).
  Missing: the sprite-animation-timer seed from
  `ShipClassDef.combat_state_init_range` (field not loaded yet).
- `0x0041c9f0` dude-def spawn — DONE (`NovaDude_SpawnShipFromDudeDefInSystem`),
  used by the aux-fleet respawn arm.
- Dispatch — DONE in both owners of the mission-fleet slices:
  - `0x0041af90` system-entry restore (`NovaSystem_RestoreMissionFleets`,
    wired after dock launch, at new-game and jump-arrival call sites): locator/system
    match, escort pre-count for follow-player ShipBehav 1 fleets, placement
    arms (`ship_goal` 3 center scatter, 5 derelict wreck with the
    33%/10% armor cut and the +0xB9 deadline latch, negative ShipStart
    nav-point arrival, ShipStart 2 cloak), escort behavior link, flags
    0x0001 auto-resolve.
  - `0x0041d6e0` per-tick respawn (mission stepper inside
    `NovaSystem_TickNpcSpawnMaintenance`): aux-fleet top-up arm (roll clock,
    locator match, budget, flags 0x0010 indefinite respawns) and main-fleet
    rearm arm (spawn/rearm timer countdown, special-ship alive-count top-up
    via goal_count_remaining, arrival bearing from the player's previous
    system, ShipState +0x94).
- `0x0047a30` `Mission_DoesSystemMatchMissionLocator` — DONE (`mission.cpp`):
  full locator decode incl. the 10000..31999 government codes.
- `0x0041ad50` deactivation tally arm — DONE (aux respawn budget credit).

Ghidra DB: `g_active_misn` retyped as
`MisnActive *`, `g_active_misn_runtime_flags` as `MisnRuntimeFlags *`, and
ShipState +0x94 named `jump_destination_system_id` (system being jumped
toward / last left; -1 none, -2 mission-spawn sentinel).

Remaining gaps include the license-enforcer spawner `0x0046ac50` (renamed
`Registration_SpawnLicenseEnforcer` in Ghidra), government assistance/
reinforcement (`0x00413610`/`0x0043a020`), and the special-system forced
personality arm of `0x0041af90`. The goal-counter writers, hailed single-ship
replacement arm at `0x00454910`, and stellar-attack directive `0x004053c0`
are DONE. Mission-ship
announcements `0x00426d10` are DONE (`Mission_ShowMissionShipAnnouncement`,
mission.cpp), ambush spawning `0x00426dd0` is DONE
(`Mission_TrySpawnMissionShipAmbush`, wired at the jump-arrival tail in
spaceflight.cpp), and the Ship_HandleShip per-frame hail ladder is DONE
(`Mission_TickShipHailLadder`: the full pers Flags gate chain, the distress
throttle bypass, and the 1-in-0x8C roll + `+0xAC` re-hail window). The
`0x452d5c` forced-pers-0x201 call site is a debug/cheat spawn key arm
(0x00452b71) and is deliberately skipped. `0x004235c0`
personality spawn (the përs table drives ambient personalities as well as
missions); its LinkMission arm now refreshes the linked definition's target
block through `Mission_ResolveMissionStellarTargets` (0x0043d240). The 1-in-7
"mission ship" arm in the ambient dispatcher is this same përs path; Ghidra
shows no separate active-mission-fleet branch there. Reinforcement fleets are a related
but distinct path: combat arms `Government_TryTriggerGovtAssistanceEncounter`,
which starts the system's `ReinfTime` countdown; when it expires,
`System_UpdateRandomEncounterCountdown` calls the ordinary `flet` spawner with
the system's `ReinfFleet` and AI behavior `4`. That common spawner selects the
same arrival presentation as random encounter fleets: state `0x15` at an
adjacent restricted stellar, otherwise state `0x08` slowdown. The Bible's
mission `ShipStart = 1` is a separate mission-special-ship arrival mode.

## 6. In-flight interactions

The runtime interaction chain:

- `0x00441B40` mission-ship interaction eligibility
- `0x00442510` mission-ship interaction dialog
- `0x00443760` per-tick interaction reactions (driver wired in TickSystems
  scope 0xb)
- `0x00443780` reaction-slot travel interaction (landing gate, runs once on
  Spaceport entry ahead of the AvailLoc-3 offer pass, debriefs layered over
  the dock — the original's single call position in 0x00491f30)
- `0x004438D0` reaction resource processing (cargo pickup/drop-off)
- `0x00443C60` mission/surrender/boarding reaction handling for all ShipGoal
  arms (TODO(decomp): cloaked-target sprite-rect visibility for observe
  missions and the g_travel_scene_ctx gate)
- `0x00445D70` mission-text placeholder replacement

This covers destroy, disable, board, escort, rescue, observe, chase-off, and
related objective families.

### 6.1 Goal-counter producers

The counter writers are event-driven, keyed on `ShipState.mission_fleet_slot`:

- **Destroy — `Ship_UpdateVisualState` 0x00428340** (destruction arm):
  PORTED as `NovaShip_TickDestroyedShipVisualState` /
  `NovaShip_RunShipDestructionFinale` (`src/game/ship_visual.cpp`); the NPC
  pass runs after `Ship_HandleShip` in `Stub_HandleShips` (death-timer
  decrement in the destroyed branch, finale when 0 < timer <= 2.0,
  DAT_0057531c). Ported: the hull blast (radius `mass*0.075+50`, damage
  `mass*0.0375+25`, capability-flags 0x400 and pers-0x3ff hulls exempt,
  force-armor-only hits with the transition check armed), the quick-fail gate
  (mission active, not failed, `goal_counter_a == 0`, `ship_goal` 1/3, or
  2/5 with the +0xB9 boarded latch clear, runtime flags 0x0400 clear) → snd +
  STR# 0x7d2:0x11c + `Mission_FailMissionSlotQuick`, then `goal_counter_a++`,
  `target_ship_count--` (stops the system-entry restore respawning a wiped
  fleet), and the pers present-flag deactivation. The player death path calls
  the finale directly (mission arm is player-exempt). Visual-only slices
  (debris puffs above 2.0, sprite layering, audio) stay in the SDL view,
  which already spawns destruction visuals at the hit transition — divergence
  logged in `ship_visual.cpp`.
- **Disable — `Shot_ResolveShipHitFromWeapon` 0x004192d0** (fire-restriction
  arm): PORTED in `collision.cpp` (transition-gated by the caller's
  `check_fire_restriction_transition`, set by the hull blast):
  `goal_counter_c++`; `ship_goal` 3 (escort) quick-fails unless flags
  0x0400 (goal 1 destroys-fail handled in 0x00443c60 evaluation); the armor
  pin and the player disable/destruction arms below are ported with the
  per-frame `g_player_disable_message_shown` latch (reset 0x00417669).
- **Board/rescue — `Player_HandleBoardTargetCommand` 0x0045a3d0**:
  PORTED in `boarding_plunder.cpp`: `goal_counter_b++` on the target's fleet
  for both the board-cargo arm (pickup_mode 2, after
  `Mission_TryConsumeMissionInteractionResources`, carrying_resources latch,
  STR# 0x7d2 0x6a overlay without the CREC cargo name — TODO) and the rescue
  special-ship arm (`ship_goal` 2/5 + flags 0x0001 + single-ship fleet;
  overlay 0x7e, ai_maneuver_timer 100); both set the target's +0xB9 boarded
  latch. The port's duplicate provisional +0xB9 field (`escort_rehired_mark`)
  was consolidated into `boarded_target_latch`.

Also decoded in the same 0x004192d0 read (port target: collision.cpp hit path):

- **Disable-transition armor pin**: on the fire-restricted *transition*
  (pre-damage `Ship_IsShipFireRestricted` false, post-damage true, and the
  caller's `check_fire_restriction_transition` set), armor is reset to 33% of
  max (10% with capability flags 0x10; +0x575238/+0x575208 doubles, +0.0
  offset). This is the lock that keeps disabled ships fire-restricted —
  Ship_HandleShip suppresses shield/armor regeneration while restricted.
- **Player-destruction mission arm**: when the *player* transitions to
  fire-restricted or is destroyed (both with pre-state transition gates),
  every active mission with flags 0x0004 quick-fails (snd + STR# 0x7d2:0x11c
  overlay). The player-destruction arm also shows the STR# 0x7d2:0x120
  destruction overlay, or the pers-0x3ff (Shareware Enforcer) taunt from
  STR# 30000 8+rand(6) when the killer was an Enforcer.
- Hit-path context worth porting alongside: pre-damage state capture
  (`local_12a` = fire-restricted before damage, `local_11d` = destroyed
  before damage) drives all transition gates; attacker-pers-0x3ff damage
  scaling (×2/×3/×5 by `_DAT_0059799e` tiers 0x28/0x3d/0x5b); station-hold
  state 0x0D forces armor-only hits; docked ships take no impulse
  (`ai_station_hold_timer <= 0`); shields clamp to −10% of max
  (`_DAT_00575208`) rather than zeroing.

Completion semantics in 0x00443c60 compare the counters against
`mission_target_count` (the untouched total): destroy `total <= a`, disable
`total <= c` (any destroy fails), board/rescue `total <= b`, escort = fleet
survivors with a/c == 0.

Also decoded: the **arrival mission-fleet slice** of
`Stellar_HandleStellarEntryAndExit` 0x00457580 — seeds follow-player fleet
rearm state (`spawn_rearm_timer` 0x7fff/−1, `goal_count_remaining` from the
alive aux count, scan-mask random immediate re-arm), jumps out ShipBehav 0
follow fleets via AI state 0x15, and calls `Mission_TrySpawnMissionShipAmbush`
on landing.

Ghidra DB annotations: plate comments on the three writer sites and
0x00457580, field comments on MisnActive
`target_ship_count`/`goal_counter_a/b/c`/`mission_fleet_metric_c`/
`mission_ship_count_active`/`spawn_rearm_timer`, ShipState +0xB9 named
`boarded_target_latch`, and the destruction/disable constants
renamed and retyped: `g_destroyed_finale_threshold_f32` (0x0057531c, 2.0),
the puff-roll thresholds 20/40/60 (0x00575320/24/28), the 0.25 puff offset
scale (0x00575330), the player ×3 death-timer scale (0x00575378), the hull
blast radius/damage scales + addends (0x00575380..0x00575398; retyped from
`float` to `double` and renamed `_f64` — the original uses `FMUL/FADD double
ptr`, so the earlier `_f32` float values 1.4/3.125 and 1.275/2.875 were
misaligned aliases of the true 0.075/50.0 and 0.0375/25.0), the
DeathDelay-half fraction (0x005753f8, 0.5), and the armor-pin fraction/addend
(0x00575238/0x00575230, 1/3 and 1.0).

## Recommended order

1. ~~Decode mission resources and formalize mission-related types.~~ DONE.
2. ~~Add mission globals/state for active missions, runtime flags, mission-ship definitions, ranks, and timers.~~ DONE for modeled mission/personality data; the full rank table remains open.
3. ~~Implement locator resolution and availability/list evaluation.~~ DONE.
4. ~~Implement Mission BBS display and accept/decline flow in the landed
   current-pilot/travel context, keeping it separate from the galaxy map and
   the active-mission computer.~~ DONE (desc/briefing dialogs are logged
   TODOs).
5. Active-mission timers, modeled daily driver, and mission-block save/load
   are DONE; rank salary and one unmodeled system suppression table remain.
6. ~~Implement success/failure and reaction scripts.~~ DONE (debrief dialogs
   wired to the landing-gate sink).
7. ~~Implement mission ship/fleet spawning~~ DONE (0x0041CF40 + dispatch),
   including the hailed single-ship replacement arm (0x00454910). Announcements
   (0x00426d10), the ambush spawner (0x00426dd0), and the HandleShip per-frame
   hail ladder are DONE. Goal-counter increment sites on ship death/disable/
   board are DONE (see 6.1).
8. Connect in-flight objectives, boarding, disable, escort, and interaction reactions — mostly DONE via 0x00443C60; boarding-pickup (PickupMode 2) DONE.
9. Add mission markers/highlights and replace mocked Mission BBS/starmap behavior.

The data-model/analysis phase this doc originally targeted (`0x0043BBB0` and
the `MisnDef`/`MisnActive` layouts) is complete — the verified offset maps
above and the Ghidra struct comments are the reference. The personality
spawner `0x004235C0` is DONE (ambient personalities now spawn); the remaining
mission-fleet spawner is `0x0041CF40` (dude-def mission fleet), which lights
up the mission goal counters together with mission-fleet dispatch.

The clean-room mission APIs now follow the original runtime convention: mission
list entries and `MisnActive.mission_template_id` are zero-based definition
indices. The `0x80` resource-id offset is applied only when looking up an mïsn
resource in `ScenarioData`.
