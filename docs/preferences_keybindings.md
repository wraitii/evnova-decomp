# Preferences, Key Settings, and the Input Binding Model

Reverse-engineering notes for the EV Nova preferences dialog, the separate Key
Settings modal, and the command-keybinding model that feeds gameplay input.
All addresses are Ghidra DB addresses. Companion to `intro_and_main_menu.md`
(action code 4 = Preferences) and `dlog_ditl_dialog_format.md` (DLOG/DITL).

Updated during the preferences pass. Ground truth: decomp + DITL item xrefs +
the `Ship_HandlePlayerShipControl` decomp (0x0044e019 — very slow to decompile,
be patient; it can take tens of seconds / a retry).

## Two dialogs — do not conflate

1. **Preferences / Settings** = `Menu_RunSettingsDialog` (0x00488650), DLOG
   `0xfa3` (25 items). Option toggles + sound volume + brightness sliders + a
   Key Settings button. The DLOG background is created by the dialog surface;
   its title band is the custom `0x4884f0` user-item callback, and the slider
   arrows are native PICT resources `0x86`/`0x87` at their 11x9 DITL cells.
   The shared surface is white with a black frame; its controls use the
   grayscale bevel RGBColor triples at `0x0056f118`..`0x0056f12a`, not Nova's
   blue gameplay-panel palette.
2. **Key Settings** = `Menu_RunKeySettingsDialog` (0x0048b280, renamed from
   FUN_0048b280), DLOG `0xfa2` (37 items) over backdrop PICT `0x8b`. A dedicated
   modal for rebinding the 34 gameplay commands.
   - `Menu_RunKeySettingsDialog` is entered from the Preferences dialog
     (ordinal 0x10) or, in-game, from `Ship_HandlePlayerShip`? — actually via
     the `_DAT_00591518` special-interaction command, distinct from the menu.
   - Its own loop draws `Menu_KeySettingsDraw` (0x0048b860) and handles input
     via `Menu_KeySettingsHandleInput` (0x0048b6d0).
   - The PICT is the window backdrop; row highlighting is not a yellow overlay:
     normal cells are white/black, while the selected cell is black with a
     white frame and white key text.

## The command/binding model

Gameplay input is *not* read directly from WASD/arrow scan codes anywhere in
the player core. Every rebindable action is a **command id that is looked up
through an editable key table**, polled via
`NovaInput_IsCommandActiveWithGameplayGuards(command_id)`
(0x00469ca0), which:

- returns 0 when `DAT_00596d3c` (modal/gameplay-suspended) is set;
- returns 0 when `command_id` is in the panel-suppressed list
  (`g_panel_suppressed_commands`, 5 entries, count `DAT_007354b4`);
- otherwise delegates to `FUN_004f1900(command_id)` →
  `(&DAT_0086e098)[command_id] & 1`.

**Two-tier model.** The slot index into `g_player_key_bindings[]` is the
*command id*, and the stored `short` value is the *key code* bound to it. The
callers pass `g_player_key_bindings[slot]` (the key code) as the `command_id`
argument to the probe (visible throughout `Ship_HandlePlayerShipControl`, e.g.
`NovaInput_IsCommandActiveWithGameplayGuards(g_player_key_bindings[0x13])`).
`DAT_0086e098` is a 128-entry (0x80) **key-state snapshot** indexed by key code
whose bit0 is the per-frame "is this key down" flag; the probe just reads that
bit. So the pipeline is: WM_key msg → `g_player_key_bindings` (persistent cmd→key)
→ per-frame key-state poll of `DAT_0086e098` → `FUN_004f1900` bit probe. The
key-state poll writers that xref `DAT_0086e098` are the raw window-message
handler `FUN_004d7330` and `Platform_WindowMessageInit` (0x004d7a80).

`NovaInput_CommandHasKeyBinding(cmd)` (0x00469cf0) is the same probe minus the
panel-suppression guard (also gated on `DAT_00596d3c`).

`NovaInput_PeekActiveCommand()` (0x00466810) returns the first pressed command
in 0..0x7f that is NOT a modifier range ({3..5}? the gate skips `uVar-2<=3`
{0..3,4}, `uVar-0x3a<=5` {0x38..0x40 mods}, `uVar-0x70<=1` {0x70..0x72 mods})
— used by the key-settings dialog to capture an arbitrary key press.

### The key table

`g_player_key_bindings` (0x005914e6), now retyped `short[0x52]` (82 entries,
most 0xff/unbound). Rebindable gameplay commands occupy slots 0..0x2a with
several later hardware slots (0x30+). The usable rebind range covered by the
Key Settings dialog is the 34 rows of the shadow block `DAT_00734bc8..` in
`Shot_SnapshotControlInputState` order.

`NovaPrefs_ResetKeyBindings` (0x004b4400, renamed) writes the PC-scan-code
defaults. Values are mostly Microsoft "virtual-key" / DirectInput scan codes,
but the four **flight** slots store ASCII lowercase letters instead (a porting
quirk): **turn-left = 0x13 idx stores 0x63 `c`, turn-right = 0x14 idx stores
0x64 `d`, forward = 0x15 idx stores 0x61 `a`, reverse = 0x16 idx stores 0x66
`f`** — see *movement keys* below; they run through the table too, and are
NOT the arrow/WASD keys.

### The shadow control block and save block

`Shot_SnapshotControlInputState` (0x00466850) copies the 34 live key cells into
a shadow region `DAT_00734bc8`; `Frame_RestoreControlInputState` (0x00466a30)
copies them back. The Key Settings dialog edits the **shadow** (so Cancel
discards), and commits via `Frame_RestoreControlInputState` + `NovaPrefs_SaveToDisk`.

`DAT_00734bc8` is the *source order* for:
- the 34 Key Settings rows (draw handler reads `(&DAT_00734bc8)[sVar5]`, row
  index sVar5, items 5..0x26 → ordinal-5);
- the .prf +0x08 control block (36 shorts) in `NovaPrefs_LoadOrInit` /
  `NovaPrefs_SaveToDisk`;
- duplicate detection (`ControlState_FindBindingConflict`, 0x00466c10).

The shadow is **not** a straight copy of a contiguous g_player_key_bindings
range — `Shot_SnapshotControlInputState` copies 34 *selected* slots into a
specific display order. Shadow offset → source command id (from the decomp):
`bc8→[0x15]`(fwd), `bca→[0x16]`(rev), `bcc→[0x14]`(turn R), `bce→[0x13]`(turn L),
`bd0→[0x18]`(AB), `bd2→[7]`, `bd4→[0xc]`(H), `bd6→[0xd]`, `bd8→[0xe]`(J),
`bda→[8]`, `bdc→[4]`, `bde→[5]`, `be0→[2]`(fire), `be2→[3]`, `be4→[0]`,
`be6→[1]`, `be8→[0xa]`, `bea→[0xb]`, `bec→[0x2a]`(E), `bee→[0x30]`,
`bf0→[0x31]`, `bf2→[0x32]`, `bf4→[0x33]`, `bf6→[0x17]`(mouse), `bf8→[6]`,
`bfa→[0x10]`(B), `bfc→[0xf]`(K), `bfe→[0x11]`(X), `c00→[0x12]`(zoom),
`c02→[0x29]`(U), `c04→[9]`(M), `c06→[0x19]`(P), `c08→[0x28]`(I), `c0a→[0x34]`(F12).
So the Key Settings dialog's first four rows are fwd/rev/(turn R)/(turn L);
row order follows this exact permutation, not command-id order. Note the
.persisted control block is actually **36** shorts (SaveToDisk writes 6 per
iteration, 6 iterations, while `sVar5<0x24`): the snapshot's 34 display rows
plus 2 more slots past `+0xc0a` that the dialog never shows (their meaning is
still untyped).

## Preferences dialog item map (DLOG 0xfa3, items 1-based)

Verified against the decomp + raw DITL (window 336x296 at (152,92) on the
640x480 playfield; `index` is the zero-based `NovaDialogItem::index`, and
`UiPanel_GetEntryInfo`/the interaction result use the 1-based ordinal =
index+1). "inverted" = the box is checked when the global is 0 (a Mac-era
0=on flag).

| ord | index | control | title / global |
|---|---|---|---|
| 1 | 0 | button | OK — `NovaPrefs_SaveToDisk` (+ close, sets `bVar1`) |
| 2 | 1 | checkbox | Share Processor Time — `g_pref_share_processor_time` (forced checked when `DAT_007355d0>=0x1000`) |
| 4 | 3 | static text | "Sound Volume:" label |
| 5 | 4 | static text | **sound volume value** — STR# 0x88 row `volume+1` |
| 6,7 | 5,6 | arrows | sound slider down/up — `g_pref_sound_volume` 0..8 |
| 8 | 7 | checkbox | Intro Music — `g_pref_intro_music` (off stops the music) |
| 9 | 8 | checkbox | QuickTime Movies — inverted |
| 10 | 9 | checkbox | Smoke Trails — inverted |
| 11 | 10 | checkbox | Run in a window — `DAT_00bec178` via `DDIsWindowed`, immediate toggle |
| 12 | 11 | checkbox | Ship Animations — `g_pref_ship_animations` |
| 13 | 12 | checkbox | Engine Glows — `g_pref_engine_glows` |
| 14 | 13 | checkbox | Running Lights — `g_pref_running_lights` |
| 15 | 14 | checkbox | Weapon Effects — `g_pref_weapon_effects` |
| 16 | 15 | button | **Key Settings** — `Menu_RunKeySettingsDialog` |
| 18 | 17 | checkbox | Parallax Starfield — `g_pref_parallax_starfield` |
| 20 | 19 | checkbox | Ambient Sounds — `g_pref_ambient_sounds` |
| 21 | 20 | checkbox | Hyperspace Effects — inverted (`g_hyperspace_effects`; CE also uses as input lock) |
| 22 | 21 | checkbox | Check For Updates — inverted |
| 23 | 22 | static text | "Brightness:" label |
| 24 | 23 | static text | **brightness value** — STR# 0x8b row `brightness+1` |
| 25,26 | 24,25 | arrows | brightness slider down/up — `g_pref_brightness` 0..6 |

(Item index 18 is the 313x23 title band; index 2 is a wide bottom band;
index 16 lies outside the window bounds and is not interactive. These are the
`type==0x00` plain items the dialog never acts on.)

The DITL parser (`NovaResource_LoadDialogItems`, brgr_archive.cpp) decodes
these ordinals exactly (see recipe in `dlog_ditl_dialog_format.md`); the
checkbox/static labels come verbatim from each item's Pascal-string title.

## Key Settings dialog (DLOG 0xfa2)

- items 0/1/2 = **OK / Cancel / Set Default** buttons; item 3 = backdrop frame
  (582x307); items 4..37 = the 34 row cells (col x=102..183 rows 19..305,
  middle x=301..382, right x=500..581) — three columns of command rows.
- The DLOG bounds are 594x353; item 4 (zero-based item 3) is the native 582x307
  frame at (6,5). Its backdrop **PICT 0x8b** is a compact **0x99 1-bit
  DirectBitsRect** with an inline two-entry color table and a short preamble
  before the PackBits rows. The reimplementation decodes and blits it at native
  size, then paints each row's bound key name
  (`String_ExpandControlCode`, name table `PTR_s_Escape_005776dc`) into its
  cell; unbound rows draw STR# 0x7d2/0x119 ("none"); the selected row
  (`DAT_007d1f3c`) is highlighted.
- Input: click a row cell → selected row; press a key → `NovaInput_PeekActiveCommand`
  code stored into `DAT_00734bc8[row]`, selection advances to the next row
  (wraps over the 0x22 rows).
- OK: `ControlState_FindBindingConflict` scan; on duplicate → jump to that row;
  else commit + save. Cancel: discard. Set Default: `NovaPrefs_ResetKeyBindings`
  into the shadow.

## Movement / flight keys — confirmed via the table

`Ship_HandlePlayerShipControl` (0x0044e019) proves the **big WASD/arrow keys are
not hard-coded**: the forward/back/turn commands are read through
`g_player_key_bindings[...]` exactly like every other command. The flight slots
are all in the ASCII-lowercase row of the table (a porting quirk — unlike the
DIK scan codes used elsewhere). Definitively, from the player-control decomp:
- **turn left**  = `g_player_key_bindings[0x13]` (idx 19) = 0x63 = ASCII `c`
  (heading decreases; `unaff_ESI->heading -= turnrate*dt`)
- **turn right** = `g_player_key_bindings[0x14]` (idx 20) = 0x64 = ASCII `d`
  (heading increases)
- **forward thrust** = `g_player_key_bindings[0x15]` (idx 21) = 0x61 = ASCII `a`
  (the normal-acceleration branch: `Ship_ComputeShipEffectiveThrust` +
  `Math_AddPolarVelocityWithClamp` along current heading)
- **reverse orientation** = `g_player_key_bindings[0x16]` (idx 22) = 0x66 =
  ASCII `f` (turn-into-velocity / 180° turnaround mode: aligns the ship to
  its velocity direction, decelerating)
- **afterburner** = `g_player_key_bindings[0x18]` (idx 24) = 0x2c = Z (gates
  `g_player_afterburner_active`)

So the default clean-room flight keys are **A = forward, C = turn left,
D = turn right, F = reverse** — an ASCII-letter row embedded in an otherwise
DIK-scan-code table. Indices `[6]`/`[7]` are *not* flight/turn keys: from the
reset table those are `0x1c`=DIK Return/land and `0x1e`=DIK A, and in the
player control `[6]` (0x0044e019 line ~2045) toggles `DAT_007cab35` (a HUD
message/dismiss command), `[7]` (line ~575) triggers target selection.

Index mapping recap (table base 0x5914e6, `short[0x52]`, index == command id):
`[0x13]`=0x63 turn-left, `[0x14]`=0x64 turn-right, `[0x15]`=0x61 forward,
`[0x16]`=0x66 reverse, `[0x18]`=0x2c afterburner. Everything in the player
core reads through `g_player_key_bindings[...]`, never a hard-coded scancode.

CRITICAL reimplementation note: in the original, **WASD/arrow keys are NOT
the flight keys**. The default table binds forward to ASCII `a` (A), turn
left to `c` (C), turn right to `d` (D), and reverse to `f` (F) — all steered
through the binding table, never hard-coded. The current clean-room
`SdlPlatform::PollFlightInput()` hard-codes WASD + arrows; re-routing it
through the binding table is the faithful path and also makes every key fully
configurable (matching the "make all keys configurable" decision).

The global "arm/repeat modifier" pair 0x38/0x6f (Left Shift / the '`'-adjacent
key) recurs everywhere; Left/Right Shift = 0x2a/0x36 are the modifier pair for
backwards cycling. `NovaInput_PeekActiveCommand` deliberately skips these
modifier code ranges so any bound key can be captured.

## Persistence

`.prf` file (per-resolution, name = `<render_width>EV Nova Prefs.prf`, from
STR# 0x82/1 in EVNova.ini; full field map documented on the `NovaPrefs_LoadOrInit`
plate comment). Version `0x69`. Reserved slots 0x7e..0x88 are zeroed on save
(CE keybinding-extension space). The CE `Settings_LoadIniAndPrefs` reads
`key_x2mode` from `[EV Nova]` in EVNova.ini and applies it via
`Settings_PollKeyX2Mode` (poll of `g_key_x2mode`, default 0x14 = Caps Lock).

## EVNova.ini settings outside the `.prf` preferences

The Windows profile settings are read from `[EV Nova]` and are separate from
the per-resolution `<render_width>EV Nova Prefs.prf` state:

| key | reader | default / effect |
|---|---|---|
| `game_width` | `FUN_008721fc` | `0` means use the primary system metric; otherwise selects the requested game/display width before `VideoMode_ApplySnapshot`. |
| `game_height` | `FUN_008721fc` | `0` means use the primary system metric; otherwise selects the requested game/display height. |
| `key_x2mode` | `Settings_LoadIniAndPrefs` (`0x00872310`) | default string `0x14` (parsed with base autodetection, i.e. key code 20 / Caps Lock); `Settings_PollKeyX2Mode` polls that key each frame. |
| `ui_scale` | `Settings_LoadUiScale` (`0x00872e7e`) | default `1.0`; zero is coerced to `1.0`, and non-default values rescale the base display dimensions and font setup. |

These four values do not appear in the `.prf` payload and are not controls in
the Preferences dialog. The checked-in `EV Nova/EVNova.ini` is the extracted
numeric-section resource/string file and does not contain a live `[EV Nova]`
profile section; the table above is the runtime reader contract recovered from
the executable.

## Open TODO / work list for the reimplementation

1. **Complete:** `Resource_LoadPictAsImage` now decodes the compact 0x99
   1-bit/color-table PICT 0x8b backdrop, including its row preamble.
2. **Complete:** `Menu_RunSettingsDialog` is a clean-room modal over DLOG
   0xfa3 with resource-derived geometry, the custom title-band treatment,
   native PICT slider arrows, the original white/black surface and grayscale
   bevels, toggles, sliders, OK/Cancel,
   and the Key Settings entry point.
3. **Complete:** `Menu_RunKeySettingsDialog` now renders PICT 0x8b and its 34
   row cells; click-to-select, physical-key capture, duplicate validation,
   Set Default, and shadow-copy Cancel/OK behavior are implemented.
4. **Represent preferences explicitly** (AGENTS.md: avoid hidden globals):
   a `NovaPreferences` value struct on `NovaRuntime`/`GameState`, defaulting via
   `NovaPrefs_ResetToDefaults`-equivalent, loaded/saved through a `.prf`
   read/write, migrated across the current SDL build.
5. **Wire flight input through the table**: rework `SdlPlatform::PollFlightInput`
   (or a game-side abstraction) to derive `FlightInput.*` from the binding table
   by command id, so WASD/arrows/afterburner/fire/travel/starmap/etc. are all
   rebindable. Defaults mirror the original scan-code table; the CE x2mode key
   (0x14) and hyperspace-effect gates are separate settings.
6. **`g_hyperspace_effects`** reuse: decide whether the clean-room keeps the CE
   raw-input-lock behavior or treats it purely as the effect toggle (prefer the
   latter, documented divergence).

## Files for the reimplementation

- `src/game/preferences.{hpp,cpp}` (new) — `NovaPreferences` struct, defaults,
  .prf load/save, and the two dialog modals.
- `src/game/key_bindings.{hpp,cpp}` (new) — the binding table, names,
  `NovaCommand_*` active-probe helpers, `NovaInput_PeekActiveCommand`.
- `src/nova_app.cpp` — swap the `GameModeAction::preferences` Todo branch for a
  call into the preferences modal.
- `src/pict_image.cpp` — PICT 0x8b decode support.
- `src/sdl_platform.{hpp,cpp}` — route `PollFlightInput` through the table.
