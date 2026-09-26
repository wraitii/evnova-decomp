# Mission system reference

Source organization: `mission.cpp` owns offers, outcomes, and mission-ship
interactions; `mission_text.cpp` owns placeholder, wildcard, and date formatting;
`mission_world.cpp` owns calendar advancement and daily world updates;
`mission_script.cpp` owns the control-expression implementation.

Coverage lives in `decomp-progress.tsv`; this note records the original's data
model and behavior.

## Verified m\xefsn payload map

Offsets ground-truthed against the loader `0x0043BBB0`, populate `0x0043F8C0`,
and the EVN Bible. The loader skips payload `+0x002`, so the Bible's
AvailLoc/Record/Rating/Random sit at `+0x004/+0x006/+0x008/+0x00a` and
TravelStel/ReturnStel at `+0x00c/+0x00e`.

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
PickupMode/DropOffMode/ScanMask, +0x32 can_abort (payload +0x42), +0x33
carrying latch, +0x35..+0x43 desc ids (+0x41 from payload +0x58, provisional;
+0x43 ShipDone), +0x45 TimeLimit countdown (-32000 = none). The Ghidra struct
comments carry the same map.

## Data model

- `MisnDef`: 300 bytes, up to 1000 definitions.
- `PersDef`: 1940 bytes (`g_pers_defs`); confirmed runtime fields named, gaps
  undefined.
- `MisnActive`: 2278 bytes, 16 active slots.
- `MisnRuntimeFlags`: 20 bytes per slot.

Loader/locator functions: `0x0043BBB0` `NovaResources_LoadMisnResourceDefs`,
`0x0043C3E0` stellar-locator resolution, `0x0043D240` mission stellar-target
resolution, `0x0043F8C0` active-mission-slot population.

Clean-room model: `src/game/scenario_data.hpp` (`MissionDef`) and
`src/game/game_state.hpp` (`ActiveMission`, `MissionRuntimeFlags`).

## Stellar id conventions (0x0043d240 / 0x0046efd0)

Resolved mission targets (MisnActive +0x00/+0x04 and the DAT_00776b04 target
table) are **0-based stellar indices** — the original indexes `g_stellar_defs[]`
directly. The travel/landed context (`GameState::travel.selected_stellar_id`,
`LandedContext.stellar_id`) keeps **0x80-based resource ids**; mission APIs
taking a landed stellar rebase at the boundary. Comparing the two spaces
directly breaks ReturnStel gates (it silently broke tutorial-001 completion).

## Random mission destination validity (0x00468b50)

A randomly selected destination must satisfy the Bible rule: far enough from the
offering system and guaranteed to exist throughout the game regardless of system
swapping. Renamed from the misleading
`Stellar_IsStellarReachableForTravel` to
`Mission_IsStellarValidRandomDestination`. It rejects a candidate in the
reference stellar's system or a directly adjacent system, then walks the
candidate system's visibility-parent (same-coordinate twin) chain and requires
the candidate to be a nav default (`SystemDef.nav_stellar_ids`) in **every**
system of the chain, so a stellar missing from any duplicate system is never
chosen (`StellarDef.is_defined` also gates out stellars no system hosts). The
twin chain uses `NovaSystem_ResolveDiscoverySlot` (0x0046b9b0). The selection
cadence now matches the original: each random family proves one of its fixed
0x800 stellar slots is eligible (RNG-free), then rejection-samples
`NovaRandom_Range(0x800)` until one passes. The stellar selector's
15000..19999 allied family carries a binary quirk verified at
0x0043da4c/0x0043db4d/0x0043db53: it indexes `g_system_defs` with the
**stellar** slot (not the candidate's own system) and excludes the exact
stellar government before `Government_AreGovtsAllied`; the 30000/31000
class families also exclude the exact government. The port reproduces both.
The port's deliberate divergences are the no-candidate fallback (-1 /
TravelStel instead of the original anchor); the -2 arm's validity check,
which the original evaluates against the pre-scan hit (always true); and the
shared pre-scan/sampling predicate, which applies the sampling loop's
visibility test to the pre-scan so a pre-scan-only candidate cannot spin the
loop.

Related: Ghidra `0x00447f00 System_GetSystemDefFlagByte` is exactly the
`SystemDef.is_visible` (+0x1eb) predicate, renamed `System_IsSystemVisible`.

## Mission list pipeline

- `0x0043CF00` `Mission_EvaluateMissionLists`: two offering lanes (0 = mission
  computer, 1 = bar/services) driven by the `0x00441B40` eligibility chain;
  stable **descending** `list_priority` order (the original's bucket pass emits
  the highest MisnDef +0x128 priority first).
- `0x00441B40` `Mission_CheckMissionShipInteractionEligibility`: the ten-gate
  chain — AvailStel locator families including the 31000-lane −30000 binary
  quirk, availability expression, AvailRecord vs `g_system_reputation`,
  AvailRating, AvailRandom vs the per-definition warp roll
  (`g_mission_offering_rolls`, DAT_00734c20, redrawn 1..100 on arrival and
  zeroed on duplicate accept), Flags2-0x0001 cargo space, the 64-bit Require
  mask (`mïsn +0x656/+0x65a` via `NovaOutfit_EvaluateRequireMask`), the Ship
  restriction (+0x5a), Flags 0x2000/0x4000 class arms, the PayVal credits gate,
  locator-candidate sanity, and the same-system (visibility-root) denial.
  AvailRecord -32000 (dominated selected stellar) and -32001 (any dominated
  stellar) are evaluated only while `g_in_flight == 0`; with the board
  offer context raised (0x0045b071) the value falls through to the ordinary
  `g_system_reputation` compare.
- `0x0043F100` `Mission_ActivateMissionAtSlot`: the on-accept payload
  (mïsn +0x15b set-expression: chain bits, X system-reveal, S auto-start) runs
  at the end. Acceptance UI (Brief dialog payload +0x34 with starmap access,
  LoadCarg dialog +0x38 on PickupMode 0) runs after activation. The decline arm
  shows the payload +0x58 desc (if any) and executes the +0x25a reaction script.
- `0x00448670` `Mission_RunAvailLocOffers`: context latch
  (DAT_00774ae2) with the non-3 wholesale clear of the shown latches
  (DAT_00773eed), a lane-1 walk for the first `AvailLoc == context` definition
  still passing eligibility, the −1-return latch, and the DAT_00776af4 recheck
  timer.
- `0x00442510` `NovaUi_RunMissionOfferWindow`: DLOG 0x3f8/DITL 1016
  with STR# 0x96 button defaults; the Flags-0x0004 empty-text auto-accept arm
  (the runtime field is Flags, not ScanMask); the variant ≥ 0x80 DLOG 0x3fc
  art path; the Flags 0x0004 ("can't refuse") arm, where both button actions
  activate and `0x004a1820` paints a single entry-6 accept button with the
  decline slot suppressed; accept via `Mission_ActivateAtSlot`.
- `0x0044A4D0` `Ship_ExpandStringPlaceholders`: the
  `{g}/{G}/{pN}/{bN}/!` placeholder state machine with escapes; the gender arm
  reads the 'm' latch. Wired at desc consumers via the load-time pass in
  `Ui_LoadSelectionDialogResource`.
- `0x004982A0` `Ui_RunTravelSelectionDialog`: the generic scrolling
  desc-text reader (the "travel" name is a misnomer), DLOG 0xbbb, 3-PICT
  backdrop (0x214d body / 0x214c top / 0x214e bottom, 0x00499870 order),
  auto-shrink to measured pure-text height (clamped to 0x30), ±10px scrolling.
- `0x00447F20` mission condition-expression evaluation;
  `0x00447A30` system/locator matching.

Calendar/deadline plumbing: `GameDate`, `Mission_AdvanceGameDate` (0x00466c40),
`Mission_ComputeDateAfterSteps` (0x0043f080, stores the absolute deadline into
runtime flags +0x06/+0x08/+0x0a at acceptance),
`NovaStellar_ComputeHyperspaceTravelDays` (0x00465550), and
`NovaText_FormatDateString` (0x00468450/0x00468600, STR# 0x89). The `<DL>` token
active-slot arm renders empty when deadline == today.

### Mission BBS and mission computer

- `0x0043C470` `NovaUi_RunMissionBbsWindow`: DLOG 0x3EE, PICT 0x2139, rebuilds
  the available rows, shows the selected mission description
  (`mission_id + 4000`), and accepts via `Mission_ActivateMissionAtSlot`. The
  draw uses the Geneva-9 screen font and fixed 12px list pitch; the selected
  title panel is white Times 18 with a black no-selection fill; the description
  panel is the fill+InvertRect black/white pattern of
  `NovaUi_DrawMissionBbsWindow` 0x00441620. Display names strip the
  ';'-subtitle per `NameString_StripSubtitleSuffix` 0x004cd230.
- `0x00440C90` `NovaUi_PollMissionBbsWindow` is the BBS polling loop.
- `0x00446150` `NovaUi_RunMissionComputerWindow`: the in-flight active-missions
  modal (gameplay command 0x28, default key I) over DLOG/DITL 0x3f4 + PICT
  0x2145. Its list rebuild (0x00445dc0) skips flags-0x400 invisible missions;
  failed rows keep the DAT_0056c328 0xa5 marker; the selected mission's
  quick-brief desc runs through the placeholder + wildcard passes; Abort/Done
  (STR# 0x96 0x23/0x5) with Abort enabled on MisnActive +0x32; the starmap
  action uses the flags-0x100 destination preselect; abort applies
  −5× CompReward over every system the CompGovt owns, then
  `Mission_ClearMisnSlotAssignments(slot, 1)`. List rows sit on the fixed 12px
  native pitch (DAT_0088c01c 8 × 1.5 at 0x005754f8) and the description panel is
  the fill+InvertRect black/white pattern of 0x00446e00.
- `0x004612C0` `NovaUi_DrawCargoMissionStatusPanel` is the in-flight cargo/
  mission status summary, not the BBS.

## Completion and failure

Functions: `0x00440410` success (the +0x3d Comp debrief text reader),
`0x00440930` failure (the +0x3f Fail dësc), `0x00447D90` active-mission
resolution, `0x00440AA0` mission/fleet assignment cleanup, `0x00440BF0` quick
failure, `0x00440750` `Government_ApplyReputationCreditDelta` (PayVal), and
`0x00466CB0` the daily driver.

## Reaction scripts and timers

- `0x00448020` reaction-condition checking (0x00447F20), `0x00448050` script
  payload execution (shared executor, entrypoint).
- `0x00449370` mission script engine: Bible opcodes
  A/F/S/G/D/M/N/C/E/H/K/L/P/Q/T/U/X/Y plus the `b`/`!`/`^` control-bit
  forms and `R(...)`. The parser is the original's byte-at-a-time state
  machine (documented at the top of `src/game/mission_script.cpp`): opcode
  letters arm the command and zero the accumulator, digits accumulate, any
  other byte is a delimiter that executes, and the NUL terminator flushes a
  trailing token. `R` draws a 0/1 branch and skips the other alternative (the
  original two-byte skip quirk at branch 0 is reproduced).
- `0x00448910` active-mission timer tick; `0x00466C40` calendar advancement.
- `0x00872040` `Debug_HandleCheatAndNcbCommands`: debug/cheat path, not gameplay.
- The interpreter can mutate ships, active missions, rank definitions
  (`g_rank_defs`), stellar state, outfits, weapon/ship availability, and mission
  lists.

## Mission ships and fleets

- `0x0041CF40` mission-fleet spawn from a dude def: forced-class special-ship-type
  lock (flags 0x0800 + single-ship fleets), weighted type selection with the
  ignore-availability fallback, identity (`mission_fleet_slot` +
  `dude_class_id`), government, AI behavior (dude ai_type or class default),
  class vitals/skill variance/waypoint marker, and the ShipBehav 0 hostile arm.
  Ammo tables load lazily (`NovaWeapon_EnsureNpcWeaponBanks`).
- `0x0041c9f0` dude-def spawn (`NovaDude_SpawnShipFromDudeDefInSystem`), used by
  the aux-fleet respawn arm.
- `0x0041af90` system-entry restore: locator/system match, escort pre-count for
  follow-player ShipBehav 1 fleets, placement arms (`ship_goal` 3 center scatter,
  5 derelict wreck with the 33%/10% armor cut and the +0xB9 deadline latch,
  negative ShipStart nav-point arrival, ShipStart 2 cloak), escort behavior link,
  flags 0x0001 auto-resolve.
- `0x0041d6e0` per-tick respawn (inside `NovaSystem_TickNpcSpawnMaintenance`):
  aux-fleet top-up (roll clock, locator match, budget, flags 0x0010 indefinite
  respawns) and main-fleet rearm (spawn/rearm timer countdown, special-ship
  alive-count top-up via goal_count_remaining, arrival bearing from the player's
  previous system, ShipState +0x94).
- `0x0047a30` `Mission_DoesSystemMatchMissionLocator`: full locator decode
  including the 10000..31999 government codes.
- `0x00457580` restricted-travel (hypergate/wormhole) system-transition slice,
  after the transfer: `Mission_RefreshActiveMissionSpawnState`, the follow-player
  fleet rearm seeding (`Mission_RearmFollowPlayerFleetsForRestrictedTravel`:
  ShipBehav 1 forced to `spawn_rearm_timer = -1`/`goal_count_remaining = 0`,
  ShipBehav 0 staged at `0x7fff` or `0` by the government
  flags_secondary/random gate-arrival arms, a `-1` aux locator disabling the aux
  top-up clock), `System_RebuildInitialNpcAndMissionPopulation(cur, 0)`,
  `System_TickNpcSpawnMaintenance`, then the two AI state 0x15 gate-emergence
  passes (`NovaTravel_EmergeFollowFleetsFromGate` before the ambush,
  `NovaTravel_EmergeAttachedShipsFromGate` after it). Ported in
  `mission.cpp`/`travel.cpp`/`spaceflight.cpp`.
- `Mission_FollowPlayerFleet` field naming: `MisnActive +0x65` is **AuxShipSyst**
  (`aux_ship_system_locator`, clean-room) and `+0x67` is the aux spawn counter
  (`aux_ships_spawned`); the old `mission_fleet_metric_b/c` names were misleading.
- `0x0041ad50` deactivation tally credits the aux respawn budget.
- `g_active_misn` is typed `MisnActive *`, `g_active_misn_runtime_flags`
  `MisnRuntimeFlags *`; ShipState +0x94 is `jump_destination_system_id` (system
  being jumped toward / last left; -1 none, -2 mission-spawn sentinel).
- Related spawners: government assistance/reinforcement
  (`0x00413610`/`0x0043a020`); `0x004235c0` personality spawn (the përs table
  drives ambient personalities; its LinkMission arm refreshes the linked
  definition's target block through `Mission_ResolveMissionStellarTargets`);
  license-enforcer spawner `0x0046ac50`
  (`Registration_SpawnLicenseEnforcer`). The 1-in-7 "mission ship" arm in the
  ambient dispatcher is the përs path, with no separate active-mission-fleet
  branch. Reinforcement fleets are distinct: combat arms start the system's
  `ReinfTime` countdown, whose expiry calls the ordinary `flet` spawner with
  `ReinfFleet` and AI behavior 4 (the same state-0x15 restricted-stellar /
  state-0x08 slowdown arrival choice as random encounter fleets).
- Announcements `0x00426d10` (`Mission_ShowMissionShipAnnouncement`), ambush
  spawning `0x00426dd0` (`Mission_TrySpawnMissionShipAmbush`), and the
  `Ship_HandleShip` per-frame hail ladder (`Mission_TickShipHailLadder`: full
  pers Flags gate chain, distress throttle bypass, 1-in-0x8C roll + `+0xAC`
  re-hail window).

## In-flight interactions and goal counters

Runtime interaction chain: `0x00441B40` eligibility, `0x00442510` interaction
dialog, `0x00443760` per-tick interaction reactions (scope 0xb), `0x00443780`
reaction-slot travel interaction (landing gate), `0x004438D0` reaction resource
processing (cargo pickup/drop-off), `0x00443C60` mission/surrender/boarding
reaction handling for all ShipGoal arms, `0x00445D70` mission-text placeholder
replacement. This covers destroy, disable, board, escort, rescue, observe,
chase-off, and related objectives.

Counter writers are event-driven, keyed on `ShipState.mission_fleet_slot`:

- **Destroy — `Ship_UpdateVisualState` 0x00428340** (destruction arm): the hull
  blast (radius `mass*0.075+50`, damage `mass*0.0375+25`, capability-flags 0x400
  and pers-0x3ff hulls exempt, force-armor-only hits with the transition check
  armed), the quick-fail gate (mission active, not failed, `goal_counter_a == 0`,
  `ship_goal` 1/3, or 2/5 with the +0xB9 boarded latch clear, runtime flags 0x0400
  clear) → snd + STR# 0x7d2:0x11c + `Mission_FailMissionSlotQuick`, then
  `goal_counter_a++`, `target_ship_count--` (stops the system-entry restore
  respawning a wiped fleet), and the pers present-flag deactivation. The player
  death path calls the finale directly (mission arm is player-exempt).
- **Disable — `Ship_ApplyDamageToShip` 0x004192d0** (fire-restriction arm):
  transition-gated by the caller's `check_fire_restriction_transition`:
  `goal_counter_c++`; `ship_goal` 3 (escort) quick-fails unless flags 0x0400
  (goal 1 destroys-fail handled in 0x00443c60 evaluation); the armor pin and the
  player disable/destruction arms run with the per-frame
  `g_player_disable_message_shown` latch (reset 0x00417669).
- **Board/rescue — `Player_HandleBoardTargetCommand` 0x0045a3d0**:
  `goal_counter_b++` on the target's fleet for both the board-cargo arm
  (pickup_mode 2, after `Mission_TryConsumeMissionInteractionResources`,
  carrying_resources latch, STR# 0x7d2 0x6a + optional 0x6b + commodity name
  (STR# 0xfa1 / res 0x238c) + 0x6c overlay)
  and the rescue special-ship arm (`ship_goal` 2/5 + flags 0x0001 + single-ship
  fleet; overlay 0x7e, ai_maneuver_timer 100); both set the target's +0xB9
  boarded latch.

Other decoded facts from the 0x004192d0 read:

- **Disable-transition armor pin**: on the fire-restricted *transition*
  (pre-damage `Ship_IsShipFireRestricted` false, post-damage true, caller's
  `check_fire_restriction_transition` set), armor resets to 33% of max (10% with
  capability flags 0x10; +0x575238/+0x575208 doubles, +0.0 offset). This keeps
  disabled ships fire-restricted — `Ship_HandleShip` suppresses shield/armor
  regeneration while restricted.
- **Player-destruction mission arm**: when the player transitions to
  fire-restricted or is destroyed (both transition-gated), every active mission
  with flags 0x0004 quick-fails (snd + STR# 0x7d2:0x11c overlay). The destruction
  arm also shows STR# 0x7d2:0x120, or the pers-0x3ff (Shareware Enforcer) taunt
  from STR# 30000 8+rand(6) when the killer was an Enforcer.
- **Hit-path context**: pre-damage state capture (`local_12a` = fire-restricted,
  `local_11d` = destroyed) drives all transition gates; attacker-pers-0x3ff
  damage scaling (×2/×3/×5 by `_DAT_0059799e` tiers 0x28/0x3d/0x5b); station-hold
  state 0x0D forces armor-only hits; docked ships take no impulse
  (`ai_station_hold_timer <= 0`); shields clamp to −10% of max (`_DAT_00575208`)
  rather than zeroing.
- Completion semantics in 0x00443c60 compare counters against
  `mission_target_count` (the untouched total): destroy `total <= a`, disable
  `total <= c` (any destroy fails), board/rescue `total <= b`, escort = fleet
  survivors with a/c == 0.
- **Restricted-travel mission-fleet slice** of
  `Stellar_HandleStellarEntryAndExit` 0x00457580 (hypergate/wormhole only; a
  normal landing goes straight from `System_RebuildInitialNpcAndMissionPopulation
  (cur, 1)` to the shared display/ambush tail): seeds follow-player fleet rearm
  state (`spawn_rearm_timer` 0x7fff/−1, `goal_count_remaining` 0 or
  `target_ship_count`, government flags_secondary/random immediate re-arm), runs
  the per-tick maintenance, jumps out ShipBehav 0 follow fleets and the
  player's attached ships via AI state 0x15, and calls
  `Mission_TrySpawnMissionShipAmbush` in between the two emergence passes.

Ghidra field comments: MisnActive
`target_ship_count`/`goal_counter_a/b/c`/`aux_ships_spawned`/
`mission_ship_count_active`/`spawn_rearm_timer`, and ShipState +0xB9
`boarded_target_latch`. Renamed/retyped constants:
`g_destroyed_finale_threshold_f32` (0x0057531c, 2.0), puff-roll thresholds 20/40/60
(0x00575320/24/28), the 0.25 puff offset scale (0x00575330), the player ×3
death-timer scale (0x00575378), the hull blast radius/damage scales + addends
(0x00575380..0x00575398; `FMUL/FADD double ptr`, so `_f64` = 0.075/50.0 and
0.0375/25.0), the DeathDelay-half fraction (0x005753f8, 0.5), and the armor-pin
fraction/addend (0x00575238/0x00575230, 1/3 and 1.0).

## Disasters (öops)

`ScenarioData::disaster_defs` holds the 0x100 öops slots (id 0x80 + i, stride
0x210) decoded by `DecodeDisaster`; the payload is five big-endian words (target
stellar, commodity, signed price delta, duration days, per-day start chance) plus
the ActivateOn C string, and the record name is the display label.
`System_UpdateDisasterStates` runs each game-day from the daily driver: a defined
idle slot rolls the chance, tests ActivateOn, then activates on its bound stellar
(or a random available, non-travel-flagged stellar for target -1) for the
duration; active slots count down. The Bar/travel-news report renders active
disasters (`Bar_ComposeDisasterReport`); the commodity-exchange price delta and
the `.plt` block2 +0x3088/+0x3288 runtime persistence consume the same state.
