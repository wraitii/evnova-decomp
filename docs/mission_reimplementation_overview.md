# Mission reimplementation overview

Status update (after the 2025 mission-loop passes): the core loop is
implemented end to end for non-special-ship missions. `src/game/mission.cpp`,
`mission_script.cpp`, and the landing path in `spaceflight.cpp` now cover the
data model, BBS evaluation/activation, the timer tick, per-tick objective
evaluation, the landing gate, and success/failure resolution with the PayVal
credit/reputation opcode. Still open: mission-ship/fleet spawning (goal
counters never increment without it), the daily driver (0x00466CB0: date
advance, deadline countdown, availability rerolls), and the desc/briefing
dialogs (logged TODOs). See the progress tracker for per-function percentages.

## Verified m\xefsn payload map (offsets ground-truthed against populate
0x0043F8C0, the loader 0x0043BBB0, and the EVN Bible)

```text
+0x000 AvailStel      +0x002 AvailLoc       +0x004 AvailRecord
+0x006 AvailRating    +0x008 AvailRandom    +0x00c TravelStel
+0x00e ReturnStel     +0x010 CargoType      +0x012 CargoQty (tons)
+0x014 PickupMode     +0x016 DropOffMode    +0x018 ScanMask
+0x01c PayVal (4b)    +0x020 ShipCount      +0x022 ShipSyst (-6 = follow player)
+0x024 ShipDude       +0x026 ShipGoal       +0x028 ShipBehav
+0x02a ShipNameID     +0x02c ShipStart      +0x02e CompGovt
+0x030 CompReward     +0x032 ShipSubtitle   +0x034..+0x03e desc ids
                      (Brief, QuickBrief, LoadCarg, DumpCargo, Comp, Fail)
+0x040 TimeLimit      +0x044 ShipDoneText   +0x048 AuxShipCount
+0x04a AuxShipDude    +0x04c AuxShipSyst    +0x050 Flags / +0x052 Flags2
+0x058 desc id (slot +0x41, provisional)    +0x05a OnStart set-string?
+0x05c Availability expr                    +0x7a0 list priority
```

MisnActive highlights: +0x00 TravelStel / +0x04 ReturnStel (resolved),
+0x12/+0x14 resolved CargoType/CargoQty, +0x16/+0x18/+0x1a
PickupMode/DropOffMode/ScanMask, +0x33 carrying latch, +0x35..+0x43 desc ids
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

The mission-list pipeline is implemented in `src/game/mission.cpp`:

- `0x0043CF00` `Mission_EvaluateMissionLists` — DONE
- `0x0043F100` `Mission_ActivateMissionAtSlot` — DONE (BBS passes the landed
  stellar; script `S` opcode passes `ai_secondary_target_slot`)
- `0x00447F20` mission condition-expression evaluation — DONE
- `0x0043F080` reaction schedule calculation — open
- `0x00447A30` system/locator matching — DONE (clean-room locator resolver)

This layer determines which missions appear, sorts them, checks availability expressions, resolves destinations, and accepts a mission into one of 16 active slots.

The Mission BBS UI is a functional port in `src/game/docked_dialog.cpp`
(evaluates available missions, DLOG/DITL 0x3ee layout, selection, accept via
`Mission_ActivateAtSlot`), though desc/briefing dialogs are still logged
TODOs. Ghidra distinguishes this from the active-mission computer and from
the galaxy map:

- `0x0043C470` `NovaUi_RunTravelDestinationMainWindow` is the available-mission
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

The existing `0x2139` frame in `src/game/docked_dialog.cpp` is the correct BBS
artwork, but its content should become this available-mission list rather than
the generic placeholder.

Remaining dialog entrypoints:

- `0x00440C90` mission-computer polling — open
- `0x00446150` mission-computer window — open (review/abort accepted
  missions)
- `0x0043C470` available Mission BBS — ported in `docked_dialog.cpp`
  (`RunMissionBoardDialog`), desc rendering TODO

## 3. Mission completion and failure

Implemented in `src/game/mission.cpp` / `mission_script.cpp`:

- `0x00440410` success — DONE (debrief dialog TODO)
- `0x00440930` failure — DONE (debrief dialog TODO)
- `0x00447D90` active mission resolution — DONE
- `0x00440AA0` mission/fleet assignment cleanup — DONE
- `0x00440BF0` quick failure — DONE
- `0x00448670` return-mission interactions — open
- `0x00440750` `Government_ApplyReputationCreditDelta` (PayVal) — DONE, full
  encoded range map verified against disasm + Bible
- `0x00446CB0` daily driver (deadline countdown, availability reroll) — open

These apply credits, reputation, follow-up missions, text, ship cleanup, and script payloads.

## 4. Reaction scripts and timers

The scripting subsystem includes:

- `0x00448020` reaction-condition checking — DONE (0x00447F20 port)
- `0x00448050` script payload execution — DONE (entrypoint, shared executor)
- `0x00449370` mission script engine — ~75% (Bible opcodes A/F/S/G/D/M/N/C/
  E/H/K/L/P/Q/T/U/X/Y; grammar still partially decoded)
- `0x00448910` active-mission timer tick — DONE (player-tick call site)
- `0x00466C40` reaction schedule advancement — open
- `0x00872040` reaction input trigger — open

The interpreter can mutate ships, active missions, system cues, stellar state, outfits, weapon/ship availability, and mission lists. Its command grammar is only partially decoded, so it should follow the data-model and activation work.

## 5. Mission ships and fleets

Mission ships connect to the existing partial spawn and AI systems:

- `0x0041AF90` system mission-ship spawning (random personality arm DONE; special-system forced arm open)
- `0x0041CF40` spawn from a dude definition (mission fleet — open)
- `0x004235C0` personality spawn — DONE (renamed from
  "spawn from a mission-ship definition": the përs table drives ambient
  personalities as well as missions). See `NovaPers_SpawnShipFromPersDef` in
  `ship_spawn.cpp`. The LinkMission target-block arm is a logged skip until
  `Mission_ResolveMissionStellarTargets` (0x0043d240) lands, and the forced
  call sites (ambush 0x426dd0, player-core 0x452d5c) are not wired yet.
- `0x00426DD0` mission ambush spawning
- `0x0046AC50` ambient mission-ship spawning (Shareware Enforcer path)
- `0x00426D10` mission-ship announcements
- `0x004053C0` mission stellar-attack directive
- `0x00413610` government assistance/reinforcement trigger
- `0x0043A020` reinforcement countdown and fleet arrival

Current spawn code skips the mission branch, so the goal counters fed by
0x00443C60 never increment yet — this is the main gap between the implemented
mission loop and destroy/escort/observe gameplay. Mission-fleet ownership,
escort counts, and announcements are also absent. Reinforcement fleets are a related
but distinct path: combat arms `Government_TryTriggerGovtAssistanceEncounter`,
which starts the system's `ReinfTime` countdown; when it expires,
`System_UpdateRandomEncounterCountdown` calls the ordinary `flet` spawner with
the system's `ReinfFleet` and AI behavior `4`. That common spawner selects the
same arrival presentation as random encounter fleets: state `0x15` at an
adjacent restricted stellar, otherwise state `0x08` slowdown. The Bible's
mission `ShipStart = 1` is a separate mission-special-ship arrival mode.

## 6. In-flight interactions

The runtime interaction chain is now largely implemented:

- `0x00441B40` mission-ship interaction eligibility — open
- `0x00442510` mission-ship interaction dialog — open
- `0x00443760` per-tick interaction reactions — DONE (driver wired in
  TickSystems scope 0xb)
- `0x00443780` reaction-slot travel interaction — DONE (landing gate, runs
  once per docking; the original runs it per destination-window action)
- `0x004438D0` reaction resource processing — DONE (cargo pickup/drop-off)
- `0x00443C60` mission/surrender/boarding reaction handling — DONE for all
  ShipGoal arms; skipped: cloaked-target sprite-rect visibility for observe
  missions, g_travel_scene_ctx gate
- `0x00445D70` mission-text placeholder replacement — open

This covers destroy, disable, board, escort, rescue, observe, chase-off, and related objective families. The goal counters only move once mission-ship spawning lands.

## Recommended order

1. ~~Decode mission resources and formalize mission-related types.~~ DONE.
2. ~~Add mission globals/state for active missions, runtime flags, mission-ship definitions, system cues, and timers.~~ DONE (mission-ship definitions themselves still open).
3. ~~Implement locator resolution and availability/list evaluation.~~ DONE.
4. ~~Implement Mission BBS display and accept/decline flow in the landed
   current-pilot/travel context, keeping it separate from the galaxy map and
   the active-mission computer.~~ DONE (desc/briefing dialogs are logged
   TODOs).
5. Active-mission timers DONE; daily driver (0x00466CB0) open; save/load DONE
   for the mission blocks.
6. ~~Implement success/failure and reaction scripts.~~ DONE (debrief dialogs
   pending).
7. Implement mission ship/fleet spawning — **next major target**; feeds the
   goal counters.
8. Connect in-flight objectives, boarding, disable, escort, and interaction reactions — mostly DONE via 0x00443C60; boarding-pickup (PickupMode 2) pending.
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
