# Mission reimplementation overview

Mission reimplementation is essentially unstarted. The progress tracker contains 53 mission-labelled functions, but only the HUD cargo/mission panel has nonzero progress (45%). The existing C++ mainly establishes mission touchpoints and records deferred behavior.

## 1. Mission data model and loading

Ghidra identifies these mission-related runtime types:

- `MisnDef`: 300 bytes, up to 1000 definitions
- `MissionShipDef`: 1940 bytes
- `MisnActive`: 2278 bytes, 16 active slots
- `MisnRuntimeFlags`: 20 bytes per slot

Primary functions:

- `0x0043BBB0` `NovaResources_LoadMisnResourceDefs`
- `0x0043C3E0` mission stellar-locator resolution
- `0x0043D240` mission stellar-target resolution
- `0x0043F8C0` active-mission-slot population

This is the foundational gap. `ScenarioData` currently loads ships, outfits, stellars, governments, fleets, dudes, and asteroid types, but not mission resources. The first implementation pass should define clean-room mission definition, active-mission, mission-ship-definition, runtime-flag, and reaction structures, then decode the `mïsn` and related resources.

## 2. Mission-board availability and activation

The mission-list pipeline is understood in Ghidra but has no C++ equivalent:

- `0x0043CF00` `Mission_EvaluateMissionLists`
- `0x0043F100` `Mission_ActivateMissionAtSlot`
- `0x00447F20` mission condition-expression evaluation
- `0x0043F080` reaction schedule calculation
- `0x00447A30` system/locator matching

This layer determines which missions appear, sorts them, checks availability expressions, resolves destinations, and accepts a mission into one of 16 active slots.

The Mission BBS UI is currently a mocked service screen in
`src/game/landed_window.cpp`. Ghidra distinguishes this from the active-mission
computer and from the galaxy map:

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

The actual dialog entrypoints remain unimplemented:

- `0x0043C470` available Mission BBS / landed current-pilot window
- `0x00440C90` mission-computer polling
- `0x00446150` mission-computer window

## 3. Mission completion and failure

Core resolution functions are deferred:

- `0x00440410` success
- `0x00440930` failure
- `0x00447D90` active mission resolution
- `0x00440AA0` mission/fleet assignment cleanup
- `0x00440BF0` quick failure
- `0x00448670` return-mission interactions

These apply credits, reputation, follow-up missions, text, ship cleanup, and script payloads.

## 4. Reaction scripts and timers

The scripting subsystem includes:

- `0x00448020` reaction-condition checking
- `0x00448050` script payload execution
- `0x00449370` mission script engine
- `0x00448910` active-mission timer tick
- `0x00466C40` reaction schedule advancement
- `0x00872040` reaction input trigger

The interpreter can mutate ships, active missions, system cues, stellar state, outfits, weapon/ship availability, and mission lists. Its command grammar is only partially decoded, so it should follow the data-model and activation work.

## 5. Mission ships and fleets

Mission ships connect to the existing partial spawn and AI systems:

- `0x0041AF90` system mission-ship spawning
- `0x0041CF40` spawn from a dude definition
- `0x004235C0` spawn from a mission-ship definition
- `0x00426DD0` mission ambush spawning
- `0x0046AC50` ambient mission-ship spawning
- `0x00426D10` mission-ship announcements
- `0x004053C0` mission stellar-attack directive

Current spawn code skips the mission branch. Mission-fleet ownership, escort counts, and announcements are also absent.

## 6. In-flight interactions

The runtime interaction chain is identified but unimplemented:

- `0x00441B40` mission-ship interaction eligibility
- `0x00442510` mission-ship interaction dialog
- `0x00443760` per-tick interaction reactions
- `0x00443780` reaction-slot travel interaction
- `0x004438D0` reaction resource processing
- `0x00443C60` mission/surrender/boarding reaction handling
- `0x00445D70` mission-text placeholder replacement

This covers destroy, disable, board, escort, rescue, observe, chase-off, and related objective families.

## Recommended order

1. Decode mission resources and formalize mission-related types.
2. Add mission globals/state for active missions, runtime flags, mission-ship definitions, system cues, and timers.
3. Implement locator resolution and availability/list evaluation.
4. Implement Mission BBS display and accept/decline flow in the landed
   current-pilot/travel context, keeping it separate from the galaxy map and
   the active-mission computer.
5. Implement active-mission population, timers, and save/load.
6. Implement success/failure and reaction scripts.
7. Implement mission ship/fleet spawning.
8. Connect in-flight objectives, boarding, disable, escort, and interaction reactions.
9. Add mission markers/highlights and replace mocked Mission BBS/starmap behavior.

The immediate analysis target is `0x0043BBB0` together with the `MisnDef` and `MisnActive` layouts. Representing these in clean-room state avoids spreading opaque byte-offset manipulation through every higher-level mission function.

The clean-room mission APIs now follow the original runtime convention: mission
list entries and `MisnActive.mission_template_id` are zero-based definition
indices. The `0x80` resource-id offset is applied only when looking up an mïsn
resource in `ScenarioData`.
