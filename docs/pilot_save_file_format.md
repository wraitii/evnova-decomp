# Pilot save files (.plt): format and load/save flow

Reverse-engineering notes on how EV Nova persists a pilot's game state to
`<Nova Files>/<pilot name>.plt` and restores it. The SDL port writes the same
files under `SDL_GetPrefPath("Ambrosia Software", "EV Nova")`, alongside its
preferences, and autoresumes exclusively from that directory. Derived from the Ghidra DB
(saver `0x004c7db0`/`0x004c7dd0`, loader `0x004cb260`).

External references archived locally:

- `docs/reference/pilotformat.txt` — Andrew Madsen's Mac/Windows pilot layout
  and SimpleCrypt key, mirrored from
  https://andrews05.github.io/evstuff/guides/pilotformat.txt.
- `docs/assets/escape-velocity-nova-pilot-conversion/` — Halprin's reference
  Mac-to-Windows converter. Its per-mission padding removal is correct; its
  blanket two-byte endian swap is not used here (see below).
- `docs/assets/ResForge/` — the maintained ResForge resource-fork editor.

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
`FUN_004f2310` (4-byte read). The trailer (ship name, global `g_player_ship_name`)
is written with `FUN_004f22e0` (C-string + NUL) and read with
`FUN_004f2350(buf, 0x40, stream)`.

The size-prefix framing is present in the archived Windows pilot
`Archer (PC).plt` and in the converted Mac fixtures under `docs/assets/pilots`.
The block contents below are read at fixed offsets relative to each block's
start, so both blocks must be present with their expected sizes for a valid
save.

### Archived fixture notes

The companion download-page descriptions under `EVN/Cheats` identify four of
the converted Mac fixtures by their hulls: Alien (Shuttle), Hunter (Federation
Carrier), Pirate Hunter (Thunderforge), and Plank (an upgraded Valkyrie). The
Plank description says Ship Variants 1.0.5 is required, but that attribution is
not yet established: the Class IV hull is present in stock Nova data and is
normally unavailable for purchase. Treat the stated plugin dependency as
provisional rather than evidence that the saved class itself is plugin-only.

A Latin-text scan of the raw and decoded saves found mission-state strings
(for example `United Shipping`, `Vengeance` fragments, `Warrior's Pride`,
`Bounty Hunter`, and `Black Dragon`) and the expected pilot nicknames. These
are embedded in the two save blocks and are not player ship names. No credible
UTF-16LE/BE names were found. Of the fixture trailers, only
`Archer (PC).plt` is non-empty (`Serenity`). The classic-Mac format stores the
ship name as the name of resource 129 rather than in either resource payload.
The converted fixtures now preserve those names: Alien is `Vell-os Javelin`,
Hunter is `Fed Carrier `, Pirate Hunter is `Aurora Thunderforge ` (both include
their original trailing space), Plank is `Vengence Reaper` (original spelling),
and Rick Hunter is `Pirate Hunter II`.

The fixtures were reconverted from their original Mac resource forks after
the 0x60-byte mission-padding difference was identified. Their recovered
combat ratings are Alien 34915, Hunter 31979, Pirate Hunter 0, Plank 25251,
and Rick Hunter 0. The earlier prefix-truncated conversions read `-1` from a
misaligned escort field and lost the real Mac +0xe9ae rating entirely.

### Block 1 — "PilotState" (0xe952 = 59730 bytes)

The Mac resource-128 structure is 0xe9b2 bytes. Its 16 MissionData records
each contain six platform padding bytes (a short at Mac +0x20, a byte at
+0x35, and three trailing bytes). Windows omits them, so fields after the
mission array move earlier by 0x60 bytes: for example mission bits are Mac
+0xb81e / Windows +0xb7be and combat rating is Mac +0xe9ae / Windows
+0xe94e. Conversion must decrypt first, remove those pads per record, then
re-encrypt the shortened block while preserving its big-endian payload for the
loader's existing Mac-compatibility path. Truncating the Mac block loses the
combat rating and misaligns every post-mission field. The archived third-party
converter instead swaps every two-byte pair; do not copy that step blindly,
because it corrupts 32-bit fields and byte/Pascal strings.

| Offset | Size | Field |
|---|---|---|
| 0x0000 | u16 | jump destination stellar, stored as a **0-based `g_stellar_defs` index** (resource id - 0x80), not a 0x80-based resource id. 0xffff = none. Loader's fallback logic keys off this. |
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
| 0xb7be | 0x2710 (10000) | Nova Control Bits `b0..b9999` (`g_nova_control_bits`); one persistent byte per story/availability flag, tested as boolean but copied verbatim |
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
| 0x0002 | u16 | Strict Play latch (`g_strict_play`); loader stores `(value == 1)` |
| 0x0004 | u16 | gender latch (`g_player_is_male`, the global read by mission `{g}` expansions); loader stores `(value == 1)` |
| 0x0006 | u16[0x800] | per-stellar present ship counts |
| 0x1006 | u16[0x400] | përs personality alive flags (`g_pers_defs +0x620`) |
| 0x1806 | u16[0x400] | përs personality grudge flags (`g_pers_defs +0x621`) |
| 0x2006 | u16[0x40] | reserved (zeroed) |
| 0x2086 | u16[0x800] | per-stellar domination-day counter (`StellarDef +0x2A`, reference `stelAnnoyance`) |
| 0x3086 | u8 | seen-intro-screen latch (`g_intro_played`) — also noted in `intro_and_main_menu.md` |
| 0x3088 | u16[0x100] | disaster def ids |
| 0x3288 | u16[0x100] | disaster values (system ids, 0xffff default) |
| 0x3488 | u16[0x80] | junk item counts (`g_junk_defs+0x22`) |
| 0x3590 | u16[0x200] | cron event ids |
| 0x3990 | u16[0x200] | cron event values |
| 0x3d90 | u16[0x800] | per-system mutable reinforcement cooldown (`SystemDef +0xC4` `reinf_cooldown_days`; the decompiler also renders it as `dude_prob + 0x1c`, which is the same offset) |
| 0x4d90 | u16[0x800] | per-stellar regeneration countdown (`StellarDef +0x47C` `destroyed_days_remaining`, reference `stelDestroyed`). Restore: `<1` → `destroyed_days_remaining = -1` and live strength reset to capacity; `>=1` → `destroyed_days_remaining = value` and live strength `-1` |
| 0x5d90 | u16[4] | escort group-order command codes by class category (`g_target_category_command`); copied onto each active non-player squad-leading ship at load |
| 0x5d98 | char[0x40] | pilot nickname C-string (`g_player_nickname`) |
| 0x5dd8 | 3 × u16 | ship-paint 5-bit RGB color channels (`DAT_00733b4a/4c/4e`) |
| 0x5dde | u16[0x80] | rank active flags (`g_rank_defs` +0x00 per slot) |
| 0x5ede | char[15] | persistent string A (`g_date_prefix`) |
| 0x5eed | u8 | NUL |
| 0x5eee | char[15] | persistent string B (`g_date_suffix`) |
| 0x5efd | u8 | NUL |
| 0x5efe | u16[0x400] | reserved (zeroed) |
| 0x66fe | — | end |

## Functions

| Addr | Name | Role |
|---|---|---|
| 0x004c7db0 | `PilotFile_SaveGame` (was `Stellar_SetTravelDestination`) | Trampoline: guards on `DAT_00863f09`, then calls the saver with the current jump/travel destination as a 0-based `g_stellar_defs` index. This IS the pilot save entry point. Callers: `Menu_RunNewGameFlow` (initial save, index from `Stellar_FindNearestAvailableTravelStellar`), `Stellar_RunDockAndLaunchSequence` (`ship->ai_secondary_target_slot`), `Ship_HandlePlayerShipCore`. |
| 0x004c7dd0 | `PilotFile_SaveGameCore` | Saver core: builds `<nova_files><pilot name>.plt`, allocates block1 (0xe952) + block2 (0x66fe), fills all fields, writes `[u32 sz][data]` twice + ship-name trailer, closes. Guards on `DAT_00863f0a`. |
| 0x004cb260 | `PilotFile_LoadSave` | Loader (Open Pilot + startup auto-resume): reads `[u32 sz1][data1]` → restores PilotState; `[u32 sz2][data2]` → restores FleetState/world; then ship-name trailer. Derives the pilot name from the file path (after last ':', before '.'). Returns 0 ok; -0x2b missing/empty; -0x2a/-0x2d invalid block2; -0x2e repairs applied. |
| 0x008725b0 | `PilotSave_DecodeBlock` | Leaves plaintext blocks whose first u16 is below 0x800 alone; otherwise tail-calls the symmetric XOR transform at 0x0046f960 with `(data, size, 0xb36a210f)`. Both save blocks in the archived retail pilots use this encoding. |
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

- **The saved destination is a 0-based `g_stellar_defs` index, not a resource
  id.** `0x004cb260` reads block1+0x00 straight into
  `g_ship_states->jump_destination_stellar_id` and indexes `g_stellar_defs`
  with it (`MOVSX ...; IMUL ...,0x498`), and the launch autosave passes
  `ship->ai_secondary_target_slot`, which is also an index. Storing a
  0x80-based resource id here lands the restored player on the wrong stellar,
  or at (0,0) when that index holds no defined stellar.
- **Position/heading are NOT saved.** The loader places the player at the
  saved jump-destination stellar's map coords with a random heading, then sets
  `current_system_id` from that stellar's system (falling back to
  `System_FindSystemContainingStellar`, then any explored system, then
  system 0). Starmap pan origin follows the resolved system.
- Shield/armor are recomputed from class+outfits; only fuel is taken from the
  file (block1 +0x12).
- Per-outfit / per-weapon entries whose def no longer exists are zeroed and
  latch `local_11` (repairs → final return -0x2e).
- After restore: name globals (`g_player_ship_name` ship name, `g_player_name` pilot
  name from path), per-ship/outfit availability rolls (`avail_roll_threshold/
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

### Repair status differs between startup and Open Pilot

`PilotFile_LoadSave` can finish restoring a usable pilot while returning
`-0x2e` (`kRepairsApplied`). One cause is a saved ship class whose current
definition has `TechLevel == -9999`, as happens when the pilot depends on a
plugin that is no longer loaded. The original marks the load repaired, replaces
the missing player ship with the first defined ship class (starting its scan at
class 0), and continues restoring the file. Missing outfits, weapon entries,
junk, ranks, escorts, and fighters have similar lossy repair paths.

The two callers intentionally handle `-0x2e` differently:

- **Startup autoresume** activates the pilot only when the loader returns
  exactly `0`. A repaired pilot has already been read and repaired in memory,
  but it is not marked active; the player must choose it explicitly through
  Open Pilot.
- **Open Pilot** accepts both `0` and `-0x2e` as successful loads. For
  `-0x2e`, it sets a caller-visible repair flag; `NovaGameMode_DispatchAction`
  then displays the warning from STR# `0x8c`, entry `0x34`, before activating
  the repaired pilot.

This prevents a plugin-dependent or otherwise damaged pilot from being
silently autoresumed with substituted data, while still allowing the player to
load it deliberately after a warning. The SDL port currently preserves the
activation distinction and logs the repair details; reproducing the warning
dialog remains a `TODO(decomp)`.

## Save trigger points

- New game (`Menu_RunNewGameFlow`) — writes the initial save with the starting
  travel destination.
- Travel to a system (`Stellar_RunDockAndLaunchSequence`).
- From the per-frame player-ship update (`Ship_HandlePlayerShipCore`) — exact
  cadence/condition not yet isolated (decompile of that routine is too large;
  check the jump/hyperspace anchors).

## Serializer and entry points

`src/game/pilot_file.{hpp,cpp}` carries:

- `PilotFileSerialize` / `PilotFileDeserialize` — pure byte transforms with
  the exact block offsets/framing above. Scalar/table fields are explicit;
  complete active-mission payloads are retained opaquely and overlaid with the
  modeled counters and six script buffers. Only block2's +0x2006 and +0x5efe
  reserved ranges are zero-filled, matching the original saver. Verified in
  `tests/pilot_file_test.cpp`.
- `PilotFileSaveGame` (0x004c7db0/0x004c7dd0), `PilotFileLoadSave`
  (0x004cb260, incl. name-from-path and jump-dest system resolution),
  `PilotFileProbeExists` (0x004cd030), `PilotFileDelete` (0x004cd040),
  `PilotSave_DecodeBlock` transform (0x008725b0).
- The last-pilot marker (0x004c7d40 / 0x004ca120) is reconstructed as the
  NUL-terminated pilot path in `<Nova Files>/Last Pilot` (STR# 0x82 entry 4),
  written after save/load and consumed once after staged startup loading.
  `PilotDebug_WritePilotLog` (0x004ca2c0) remains unported. The block2 përs
  alive/grudge flags at +0x1006/+0x1806 are decoded, applied with the
  original definition/AI gates, and saved. The block1 escort/fleet tables at
  +0xe6ce..+0xe8ce are now
  decoded, encoded, collected from live player-affiliated ships, and restored
  through `ShipClass_SpawnEscortShipFromClass`. Per-stellar saved
  domination bytes, garrison counts and domination-day counters, disaster/crön runtime
  counters, and rank active flags are preserved and applied with the original
  definition/state gates. Dates, all 0x800 discovery/reputation slots, the 16
  mission runtime-flag records, and the 16 active-mission records are now
  preserved; mission records retain their opaque script/text payload bytes.
  Also round-tripped now: the player combat rating (block1+0xe94e), the
  Strict Play and gender latches (block2+0x02/+0x04), the per-system
  reinforcement cooldown (block2+0x3d90), per-stellar regeneration countdown with
  its live-strength fallback (block2+0x4d90), and the escort group-order
  codes (block2+0x5d90).

The ship-paint RGB5 channels and date prefix/suffix are also restored and
saved. Date rendering consumes the affixes at the same boundary as the
original formatter. Active missions rebuild their two cached STR# display
names, deadline, count latch, randomized rearm clock, and cleared transient
metric after the raw records are loaded.

The scenario-aware load pass also mirrors the original recovery work: it
clears positive outfit, weapon-bank, and junk quantities whose definitions no
longer exist; repairs a missing ship class; recovers a `-1` jump destination;
resolves the current system through the stellar membership map and then the
first explored system; reconstructs saved escorts/fighters; and recomputes
active mission deadline dates from their saved remaining-day counters. Any
definition loss reports `kRepairsApplied` while retaining the usable pilot.

## Quirks / open questions

- **Block obfuscation** — `PilotSave_DecodeBlock` uses a symmetric XOR stream
  seeded with `0xb36a210f`. A plaintext block whose first u16 is naturally at
  least `0x800` would be mistaken for encoded data; shipped saves avoid that
  ambiguity by encoding both blocks.
- Converted classic-Mac pilots retain big-endian scalars and Pascal strings
  inside the size-prefixed blocks. Windows pilots use little-endian scalars and
  C strings. FleetState version 300 distinguishes the payload layouts. The
  loader forces the first-block transform after detecting a Mac FleetState:
  testing Mac ciphertext's first word as little-endian can otherwise resemble
  an unencoded jump id (as it does in `Alien.plt`).
  Both byte-order detection and forced block-1 decoding are clean-room
  compatibility divergences: each original platform read its own native save
  format and did not auto-detect the other platform. Pascal nickname detection
  is likewise independent of scalar byte order because `Archer (PC).plt`
  contains little-endian fields but retains a Pascal nickname.
- **License seed check** in the saver (block1 +2 zeroing for 4 magic seed
  values) — anti-piracy; likely differs from OG depending on build.
- **Registry population at startup** (where the 0x63688a72 pilot entries come
  from so the new-game dialog lists pilots) — not located yet.
- The two 15-byte strings (block2 `+0x5ede`/`+0x5eee`) are the character
  template's `DatePrefix`/`DateSuffix`. The `DAT_00733b4a/4c/4e` u16s at
  `+0x5dd8` are the ship-paint 5-bit RGB color channels (paint rendering is
  not modelled).
  Block1 `+0xb7be` is confirmed as the 10,000 Nova Control Bit bytes and is
  preserved exactly, including noncanonical nonzero values found in converted
  Mac pilots.
- `FUN_004cd7e0` sums all resource-family block sizes and compares against a
  stored total (entry `0x63739f6d` [0]) — a data-integrity check, not a save.
