# Pilot save files (.plt): format and load/save flow

Reverse-engineering notes on how EV Nova persists a pilot's game state to
`<Nova Files>/<pilot name>.plt` and restores it. Derived from the Ghidra DB
(saver `0x004c7db0`/`0x004c7dd0`, loader `0x004cb260`).

## Two persistence tiers (don't confuse them)

1. **Pilot-save data block** — in-memory resource family `0x63688a72`
   ("pilot" registry), keyed by pilot name. Block size 0x16a (initialized by
   the per-id block initializer `FUN_004ce700`). Holds small per-pilot
   metadata used by the menu/new-game flow: start-type/ship-class designator
   (+4), intro cinematic PICT ids (+0x20) / durations (+0x28) / intro text dësc
   (+0x30, Bible chär `IntroTextID`), active flag (+0x132). Accessed via
   `ResourceData_AccessByKey(0x63688a72, name)`; created/updated by
   `PilotData_InitializePlayerState`. **Open question:** where the registry
   entries are (re)populated from at startup so the new-game dialog can list
   existing pilots — not yet found; may be a scan of `*.plt` in Nova Files.
   See `docs/char_resource_format.md` for the full `chär` template layout
   (start date, `DatePrefix`/`DateSuffix`, `IntroTextID`) and the gaps in this
   reimplementation.
2. **`.plt` file** — the actual disk persistence of the full game state.
   Written by the saver on new game / travel / periodic save; loaded by
   `PilotFile_LoadSave` (Open Pilot). This doc covers this tier.

## File layout (little-endian)

```
+0x00  u32  block1 size (0xe952 for a full save)
+0x04  block1 data (see below)
+..    u32  block2 size (0x66fe for a full save)
+..    block2 data (see below)
+..    ship-name C-string (NUL-terminated, no length prefix)
```

The u32 size headers are written with `FUN_004f22b0` (u32) and read back with
`FUN_004f2310` (4-byte read). The trailer (ship name, global `DAT_00599acc`)
is written with `FUN_004f22e0` (C-string + NUL) and read with
`FUN_004f2350(buf, 0x40, stream)`.

> Caveat: the "u32 size prefix" framing may be a community/port tweak. Verify
> against an OG save file before relying on it for interop. The block contents
> below are read at fixed offsets relative to each block's start, so both
> blocks must be present with their expected sizes for a valid save.
### Block 1 — "PilotState" (0xe952 = 59730 bytes)

| Offset | Size | Field |
|---|---|---|
| 0x0000 | u16 | jump destination stellar id (0xffff = none). Loader's fallback logic keys off this. |
| 0x0002 | u16 | ship class id. Saver zeroes it if the license seed is not one of 4 magic values (`License_ComputeSeed` anti-piracy check — see quirks). |
| 0x0004 | u16[6] | cargo (commodity) counts `cargo_bin[0..5]` |
| 0x0010 | u16 | shield points (rounded). **Not read back by the loader** — it recomputes max shield from class+outfits. |
| 0x0012 | u16 | fuel points (rounded). Loader restores fuel from here. |
| 0x0014 | u16 | month (in-game calendar; ported as `GameState::date`) |
| 0x0016 | u16 | day |
| 0x0018 | u16 | year |
| 0x001a | u16[0x800] | per-system discovery/scan state (0x1000 bytes) |
| 0x101a | u16[0x200] | per-outfit owned counts (0x400 bytes). Saver writes via 8 interleaved-looking stores; `DAT_005993b6..c2` are just `g_outfit_owned_count[1..7]` — flat array. |
| 0x141a | u16[0x800] | per-system reputation (0x1000 bytes); `DAT_00733bca..d6` = `g_system_reputation[1..7]` slices |
| 0x241a | u16[0x100] | weapon bank ammo (only [0] of each 100-short bank slot is persisted) |
| 0x261a | u16[0x100] | weapon bank secondary counters (same stride) |
| 0x281a | u32 | credits |
| 0x281e | 16 × 0x14 | active-mission runtime flags (`g_active_misn_runtime_flags`) |
| 0x295e | 16 × 0x8e6 | active missions (`_g_active_misn` structs, byte copy) |
| 0xb7be | 0x2710 (10000) | persistent story/event data (`_DAT_005914cc` blob; contents unidentified) |
| 0xdece | u8[0x800] | per-stellar saved byte (gated on `special_tech+0x2f`) |
| 0xe6ce | u16[0x40] | fleet/escort ship class ids — behavior-6 (mission) escorts; +1000 marks `field_0xbb` |
| 0xe74e | u16[0x40] | carried-fighter class ids — behavior-5 ships (deployed carrier fighters: Weapon_SpawnShipFromCarrierBayWeapon 0x0041e640 seeds ai_behavior_code 5 + squad_leader_ship_slot = owner). At a hyperspace jump the arrival walk 0x0044f8d6 deactivates the ones that cannot jump and the overlay tallies them as "fighter(s) abandoned" (STR# 0x7d2 0xa4/0xa5) |
| 0xe7ce | u16[0x40] | escort upgrade flags |
| 0xe84e | u16[0x40] | escort released flags |
| 0xe8ce | u16[0x40] | voice type modes for active player-affiliated ships (behavior > 4, no mission) |
| 0xe94e | u32 | player combat rating points |
| 0xe952 | — | end |

### Block 2 — "FleetState"/world state (0x66fe = 26366 bytes)

| Offset | Size | Field |
|---|---|---|
| 0x0000 | u16 | **300** (version/magic). Loader rejects: `0x6b` → error -0x2d; `< 300` → error -0x2a. |
| 0x0002 | u16 | ongoing/new-pilot latch (`DAT_00596d2f`) |
| 0x0004 | u16 | strict-play code latch (`DAT_00734c1c`, set from the new-game code field == 'm') |
| 0x0006 | u16[0x800] | per-stellar present ship counts |
| 0x1006 | u16[0x400] | mission-ship-def active flags |
| 0x1806 | u16[0x400] | mission-ship-def visible flags |
| 0x2006 | u16[0x40] | reserved (zeroed) |
| 0x2086 | u16[0x800] | per-stellar `special_tech+0x14` (availability roll state) |
| 0x3086 | u8 | seen-intro-screen latch (`DAT_00596d35`) — also noted in `intro_and_main_menu.md` |
| 0x3088 | u16[0x100] | disaster def ids |
| 0x3288 | u16[0x100] | disaster values (system ids, 0xffff default) |
| 0x3488 | u16[0x80] | junk item counts (`g_junk_defs+0x22`) |
| 0x3590 | u16[0x200] | cron event ids |
| 0x3990 | u16[0x200] | cron event values |
| 0x3d90 | u16[0x800] | per-system encounter probability state (`dude_prob` field) |
| 0x4d90 | u16[0x800] | per-stellar availability state (`field_0x47c`) |
| 0x5d90 | u16[4] | escort command codes by class category (`DAT_007354c4..ca`) |
| 0x5d98 | char[0x40] | pilot nickname C-string (`DAT_005999cc`) |
| 0x5dd8 | 3 × u16 | `DAT_00733b4a/4c/4e` |
| 0x5dde | u16[0x80] | rank active flags (`g_rank_defs` +0x00 per slot) |
| 0x5ede | char[15] | persistent string A (`DAT_00733b0c`) |
| 0x5eed | u8 | NUL |
| 0x5eee | char[15] | persistent string B (`DAT_00733b1c`) |
| 0x5efd | u8 | NUL |
| 0x5efe | u16[0x400] | reserved (zeroed) |
| 0x66fe | — | end |

## Functions

| Addr | Name | Role |
|---|---|---|
| 0x004c7db0 | `PilotFile_SaveGame` (was `Stellar_SetTravelDestination`) | Trampoline: guards on `DAT_00863f09`, then calls the saver with the current jump/travel destination stellar id. This IS the pilot save entry point. Callers: `Menu_RunNewGameFlow` (initial save), `Stellar_RunDockAndLaunchSequence`, `Ship_HandlePlayerShipCore`. |
| 0x004c7dd0 | `PilotFile_SaveGameCore` | Saver core: builds `<nova_files><pilot name>.plt`, allocates block1 (0xe952) + block2 (0x66fe), fills all fields, writes `[u32 sz][data]` twice + ship-name trailer, closes. Guards on `DAT_00863f0a`. |
| 0x004cb260 | `PilotFile_LoadSave` | Loader (Open Pilot + startup auto-resume): reads `[u32 sz1][data1]` → restores PilotState; `[u32 sz2][data2]` → restores FleetState/world; then ship-name trailer. Derives the pilot name from the file path (after last ':', before '.'). Returns 0 ok; -0x2b missing/empty; -0x2a/-0x2d invalid block2; -0x2e repairs applied. |
| 0x008725b0 | `PilotSave_ValidateBlock` | Block validator: if first u16 < 0x800 → 0 (valid, no checksum); else runs a 32-bit checksum helper (LAB_0046f960, partially inlined). Nonzero → caller skips restoring that block. Suspected community-fix. |
| 0x004c7d40 | `PilotFile_RecordLastPilotPath` | Writes `<nova_files><string-table 0x82/4>` with the pilot file path (the *last-pilot marker file*, consumed by `PilotData_AutoresumeLastPilot` at startup; string id unresolved). Called at end of both save and load. |
| 0x004ca120 | `PilotData_AutoresumeLastPilot` (was `GameScenario_LoadStoryData`) | Startup auto-resume: reads the last-pilot marker file (same string 0x82/4), probes the named .plt, and if present resets player state and calls `PilotFile_LoadSave`. Called from `NovaGameSession_Run` immediately before `NovaMainLoop_Run`. |
| 0x004ca2c0 | `PilotDebug_WritePilotLog` | Debug-only: dumps `pilotlog.txt` ("EV Nova pilot data dump") when licensed runtime. Called at end of load. |
| 0x004cd030 | `PilotFile_ProbeExists` | Existence probe of a resolved .plt path (used for overwrite prompt / delete guard). |
| 0x004cd040 | `PilotFile_Delete` | Deletes `<name>.plt` (permadeath path from `Ship_RunSpaceflightMode`). |
| 0x004cd290 | `PilotData_FindActivePilotName` | Scans the 0x63688a72 registry for the active pilot (main-menu resume). |
| 0x004cd350 | `PilotData_ResolveStartType` | Reads start-type designator from registry block +4. |
| 0x004cd4b0 | `PilotData_InitializePlayerState` | Reads/seeds the 0x63688a72 registry block for a pilot name; runs scripts/crew when creating. |
| 0x004cd3b0 | `IntroCinematic_SetupFrames` | Reads intro-cinematic config from the registry block. |

## Loader behavior details

- **Position/heading are NOT saved.** The loader places the player at the
  saved jump-destination stellar's map coords with a random heading, then sets
  `current_system_id` from that stellar's system (falling back to
  `System_FindSystemContainingStellar`, then any explored system, then
  system 0). Starmap pan origin follows the resolved system.
- Shield/armor are recomputed from class+outfits; only fuel is taken from the
  file (block1 +0x12).
- Per-outfit / per-weapon entries whose def no longer exists are zeroed and
  latch `local_11` (repairs → final return -0x2e).
- After restore: name globals (`DAT_00599acc` ship name, `DAT_005997cc` pilot
  name from path), per-stellar availability rolls (`avail_roll_threshold/
  limit_licensed`), `g_last_system_for_ambient_rolls = 0xffff`, then the
  pilotlog dump, and finally ship-state runtime fields reset (waypoint markers,
  shield-bubble flash intensity, aggro accumulator, etc.).

## Where the load path is used

`PilotFile_LoadSave` has exactly two callers, both "at the start":

1. **Startup auto-resume** — `NovaGameSession_Run` (boot) calls
   `PilotData_AutoresumeLastPilot` (0x004ca120) immediately before
   `NovaMainLoop_Run`. It reads the last-pilot marker file
   (`<nova_files><string 0x82/4>`, written by `PilotFile_RecordLastPilotPath`),
   and if that names an existing .plt, resets player state + reputation and
   calls `PilotFile_LoadSave`. So the game resumes the most recent pilot
   automatically at boot. **This also confirms the `0x82/4` string is the
   marker-file name shared by the record-writer and this reader.**
2. **Main-menu Open Pilot** — `NovaGameMode_DispatchAction` action 1
   (`Menu_OpenPilotFileDialog`), via a GetOpenFileNameA dialog.

There is no mid-game reload path.

## Save trigger points

- New game (`Menu_RunNewGameFlow`) — writes the initial save with the starting
  travel destination.
- Travel to a system (`Stellar_RunDockAndLaunchSequence`).
- From the per-frame player-ship update (`Ship_HandlePlayerShipCore`) — exact
  cadence/condition not yet isolated (decompile of that routine is too large;
  check the jump/hyperspace anchors).

## Serializer and entry points

`src/game/pilot_file.{hpp,cpp}` carries:

- `PilotFileSerialize` / `PilotFileDeserialize` — pure byte transforms of the
  tracked subset with the exact block offsets/framing above (untracked regions
  zero-filled; verified in `tests/pilot_file_test.cpp`).
- `PilotFileSaveGame` (0x004c7db0/0x004c7dd0), `PilotFileLoadSave`
  (0x004cb260, incl. name-from-path and jump-dest system resolution),
  `PilotFileProbeExists` (0x004cd030), `PilotFileDelete` (0x004cd040),
  `PilotSave_ValidateBlock` gate (0x008725b0).
- Not reconstructed: the last-pilot marker file (0x004c7d40 / 0x004ca120;
  marker name string 0x82/4 unresolved), `PilotDebug_WritePilotLog`
  (0x004ca2c0), and the remaining untracked .plt regions (discovery,
  reputations, dates, story blob, disaster/cron tables, and mission-fleet
  tables). The 16 mission runtime-flag records and 16 active-mission records
  are now preserved by the clean-room serializer, including their opaque
  script/text payload bytes.

## Quirks / open questions

- **u32 size-prefix framing** vs OG format — verify with a real OG `.plt`.
- **`PilotSave_ValidateBlock` validator** — the `u16[0] < 0x800` gate + checksum
  branch is unusual; a genuine save with `jump_destination == -1` (0xffff) takes the
  checksum branch. Whether nonzero there still allows a restore, and what the
  checksum covers, is unresolved. Suspected "community fix" divergence.
- **License seed check** in the saver (block1 +2 zeroing for 4 magic seed
  values) — anti-piracy; likely differs from OG depending on build.
- **Registry population at startup** (where the 0x63688a72 pilot entries come
  from so the new-game dialog lists pilots) — not located yet.
- `0xb7be` 10000-byte story blob, the two 15-byte strings, and the
  `DAT_00733b4a/4c/4e` u16s are unidentified.
- `FUN_004cd7e0` sums all resource-family block sizes and compares against a
  stored total (entry `0x63739f6d` [0]) — a data-integrity check, not a save.
