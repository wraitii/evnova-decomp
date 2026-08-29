# Mission reimplementation overview

Status update (after the 2025 mission-loop passes): the core loop is
implemented end to end for non-special-ship missions. `src/game/mission.cpp`,
`mission_script.cpp`, and the landing path in `spaceflight.cpp` now cover the
data model, BBS evaluation/activation, the timer tick, per-tick objective
evaluation, the landing gate, and success/failure resolution with the PayVal
credit/reputation opcode. Still open (after the mission-fleet spawn/dispatch
pass): the daily driver (0x00466CB0: date advance, deadline countdown,
availability rerolls) and the desc/briefing dialogs (logged TODOs); the
mission goal counters now receive spawned fleets but still need their
death/disable/board increment sites. See the progress tracker for
per-function percentages.

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
    wired at landing, new-game and jump-arrival call sites): locator/system
    match, escort pre-count for follow-player ShipBehav 1 fleets, placement
    arms (spawn_behavior 3 center scatter, 5 derelict wreck with the
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

Ghidra DB improvements from this pass: `g_active_misn` retyped as
`MisnActive *`, `g_active_misn_runtime_flags` as `MisnRuntimeFlags *`, and
ShipState +0x94 named `jump_destination_system_id` (system being jumped
toward / last left; -1 none, -2 mission-spawn sentinel).

Remaining gaps in this section: the goal-counter increment sites on ship
death/disable/board (0x00443c60 only evaluates the counters today), the
hailed-escort respawn call site `0x00454910` (comm-dialog wiring), mission-
ship announcements `0x00426d10`, ambush spawning `0x00426dd0`, the ambient
Shareware-Enforcer spawner `0x0046ac50`, the stellar-attack directive
`0x004053c0`, government assistance/reinforcement (`0x00413610`/`0x0043a020`),
and the special-system forced personality arm of `0x0041af90`. `0x004235c0`
personality spawn — DONE (renamed from "spawn from a mission-ship
definition": the përs table drives ambient personalities as well as
missions); its LinkMission target-block arm is a logged skip until
`Mission_ResolveMissionStellarTargets` (0x0043d240) lands. Reinforcement fleets are a related
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

### 6.1 Decoded goal-counter producers (Ghidra pass, port pending)

The counter writers are event-driven, keyed on `ShipState.mission_fleet_slot`, and live in three functions:

- **Destroy — `Ship_UpdateVisualState` 0x00428340** (destruction arm): when a
  destroyed ship has `mission_fleet_slot != -1` and is not the player:
  quick-fail gate (mission active, not failed, `goal_counter_a == 0`,
  spawn_behavior 1/3, or 2/5 with the +0xB9 boarded latch clear, runtime
  flags 0x0400 clear) → snd + STR# 0x7d2:0x11c + `Mission_FailMissionSlotQuick`;
  then `goal_counter_a++`; then `target_ship_count--` (remaining fleet ships —
  this is what stops the system-entry restore from respawning a wiped fleet).
- **Disable — `Shot_ResolveShipHitFromWeapon` 0x004192d0** (fire-restriction
  arm): `goal_counter_c++`; spawn_behavior 3 (escort) quick-fails unless
  flags 0x0400 (goal 1 destroys-fail handled in 0x00443c60 evaluation).
- **Board/rescue — `Ship_HandlePlayerBoardTargetCommand` 0x0045a3d0**:
  `goal_counter_b++` on the target's fleet for both the board-cargo arm
  (pickup_mode 2, after `Mission_TryConsumeMissionInteractionResources`) and
  the rescue special-ship arm (spawn_behavior 2/5 + flags 0x0001 + single-ship
  fleet); both set the target's +0xB9 boarded latch.

Completion semantics in 0x00443c60 compare the counters against
`mission_target_count` (the untouched total): destroy `total <= a`, disable
`total <= c` (any destroy fails), board/rescue `total <= b`, escort = fleet
survivors with a/c == 0.

Also decoded: the **arrival mission-fleet slice** of
`Stellar_ProcessTravelAndLanding` 0x00457580 — seeds follow-player fleet
rearm state (`spawn_rearm_timer` 0x7fff/−1, `goal_count_remaining` from the
alive aux count, scan-mask random immediate re-arm), jumps out ShipBehav 0
follow fleets via AI state 0x15, and calls `Mission_TrySpawnMissionShipAmbush`
on landing.

Ghidra DB annotations added: plate comments on the three writer sites and
0x00457580, field comments on MisnActive
`target_ship_count`/`goal_counter_a/b/c`/`mission_fleet_metric_c`/
`mission_ship_count_active`/`spawn_rearm_timer`, and ShipState +0xB9 named
`boarded_target_latch`.

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
7. ~~Implement mission ship/fleet spawning~~ DONE (0x0041CF40 + dispatch);
   remaining: goal-counter increment sites on ship death, the hailed-escort
   respawn (0x00454910) and announcements (0x00426d10).
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
