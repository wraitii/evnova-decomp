# The `chär` resource (character template / pilot block)

Resource type `0x63688a72` (`ch` + Mac-Roman `ä` `0x8a` + `r`). The same FourCC is
used for the in-memory pilot-save registry, so `ResourceData_AccessByKey(0x63688a72,
name)` resolves either a scenario character template or a session/save pilot block.
The block is grown to `0x16a` bytes (`ResourceData_EnsureBlockSize`) by its readers.

Stock scenario: **Nova Data 1.rez, id `0x0080`, name `.Trader`** (the only shipped
`chär`, and hidden — its name starts with `.`, so it is the fallback template for the
0x c1e new-pilot dialog variant).

The authoritative field list is the EV Nova Bible ("The chär resource"); the binary
layout below was confirmed by disassembling `PilotData_InitializePlayerState`
(`0x004cd4b0`) and dumping the shipped `.Trader` payload.

## Layout

All multi-byte fields are big-endian in the resource payload. Values in parentheses
are the stock `.Trader` block.

| Offset | Size | Bible name | Notes |
|---|---|---|---|
| `+0x00` | int32 | `Cash` | Starting credits (25000). Clamped `>= 0`. |
| `+0x04` | short | `ShipType` | Starting ship class. `>= 0x80` → `- 0x80`; below that resolves to class 0 (0x80). |
| `+0x06` | short[4] | `System1-4` | Candidate starting systems (0x80/0x88/0xaa/0xb8). Values `< 0x80` are unused; all unused → current system 0. One is picked at random. |
| `+0x0e` | short[4] | `Govt1-4` | Governments for the starting legal record (all `-1`). |
| `+0x16` | short[4] | `Status1-4` | Legal status applied to the `Govt1-4` entries and negated for their enemies (all `-1`). |
| `+0x1e` | short | `Kills` | Starting combat rating (0). |
| `+0x20` | short[4] | `IntroPict1-4` | Intro frame PICT ids (0x2008/0x2009/0x200a/`-1`). |
| `+0x28` | short[4] | `PictDelay1-4` | Per-frame hold in 1/60 s ticks (45/45/45/`-1`); clamped `[0, 300]`. |
| `+0x30` | short | `IntroTextID` | **Desc resource id** shown after the PICT frames (`-1`). See below. |
| `+0x32` | pstring | `OnStart` | Control-bit set string run once when the pilot starts (empty). Read by `Mission_ExecuteReactionScript`; up to the `Flags` field. |
| `+0x132` | short | `Flags` | `0x0001` = default template (set). |
| `+0x134` | short | *(StartDay)* | Starting calendar day (23). Undocumented in the Bible; the binary reads it. |
| `+0x136` | short | *(StartMonth)* | Starting calendar month (6). Undocumented in the Bible. |
| `+0x138` | short | `StartYear` | Starting calendar year (1177). |
| `+0x13a` | C string | `DatePrefix` | Prepended to every displayed date (empty). |
| `+0x14a` | C string | `DateSuffix` | Appended to every displayed date (`" NC"`). |

### Start date (`+0x134/+0x136/+0x138`)

`PilotData_InitializePlayerState` (`0x004cd4b0`, `param_2 != 0`) reads the three
shorts into `DAT_00735478` / `DAT_00735476` / `DAT_00735474`. `Menu_RunNewGameFlow`
(`0x00489d70`) then copies them, at `0x0048a535`, into `g_current_game_year` /
`g_current_game_month` / `g_current_game_day_of_month` (`0x00735458`). This copy
**overwrites** the `Game_ResetNewGameState` (`0x004b4690`) seed that
`DrawContext_ReadRenderParams` (`0x004bbb10`) set from the real local clock with
`year += 0xfa` (250); that clock seed is only the pre-override fallback.

### DatePrefix / DateSuffix (`+0x13a` / `+0x14a`)

Copied by `PilotData_InitializePlayerState` into `g_date_prefix` / `g_date_suffix`.
Both date formatters use them:

- `NovaText_FormatDateString` (`0x00468450`) starts with `DatePrefix` and ends with
  `DateSuffix` (abbreviated month names, STR# 0x89 entries 13-24).
- `Stellar_FormatElapsedTravelTime` (`0x00468600`) shares the body with full month
  names (STR# 0x89 entries 1-12).

Stock output therefore reads e.g. `June 23rd, 1177 NC`.

### IntroTextID (`+0x30`)

`IntroCinematic_SetupFrames` (`0x004cd3b0`) puts the field into
`g_intro_cinematic.intro_text_desc_id` (Ghidra struct `IntroCinematicData`,
`0x007d1f42`). After the frames, `IntroCinematic_Run` (`0x0048adc0`) loads it with
`Ui_LoadSelectionDialogResource` (`0x004c6d50`, resource type `0x64917363` = `desc`)
and shows it in `Ui_RunTravelSelectionDialog` (`0x004982a0`). That function is the
game's **generic scrolling desc-text reader** — the "travel" in its name is a
misnomer (the same reader backs mission briefings/debriefs, landing text, About Nova,
etc.), so `+0x30` is the Bible `IntroTextID`, not a stellar travel destination.

When no pilot block is found, `IntroCinematic_SetupFrames` defaults the field to
`0x7ffd` (not a valid desc, but `!= -1`, so the reader still opens with empty text).

## Port status

`CharacterTemplate_Read` (`src/game/pilot_file.cpp`) and
`ApplyCharacterTemplate` (`src/game/new_pilot_flow.cpp`) apply the start date
(`Game_ResetNewGameState`'s clock+250 seed runs first as fallback), DatePrefix/
Suffix, cash/kills/legal record and the `OnStart` control-bit string. The random
`System1-4` pick is deliberately skipped (fixed start system). Remaining gap:
`IntroTextID` is stored but the epilogue is a stub (`RunPostIntroTextStub`), so
the desc is never shown (stock `.Trader` is `-1`).
