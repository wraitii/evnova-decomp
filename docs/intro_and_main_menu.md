# Startup Splash, Main Menu, and New-Game Intro Cinematic

Documentation of the EV Nova boot → splash → main menu → gameplay flow, plus the **separate**
new-game intro cinematic, and the frame renderer that draws the main menu / in-game HUD.
All addresses reference the Ghidra DB.

## ⚠️ Two very different things — do not conflate

This codebase has **two independent sequences** that are easy to mix up:

1. **Startup splash** (boot phase, *before* the main menu). Played by
   `NovaUi_PresentLoadingSplashFrame` / `NovaUi_PresentStartupSplashFrame` inside
   `NovaGameSession_Run`, before `NovaMainLoop_Run` ever runs. These are static PICT
   images (no animation/timing) and always play on every app launch.
2. **New-game intro cinematic** (after a pilot starts, *inside* flight entry). Played by
   `IntroCinematic_Run`, configured by `IntroCinematic_SetupFrames`. Plays only on a new
   pilot's first flight entry (gated on `g_intro_played`), never during app boot.

Naming convention: the boot imagery is called **splash**; the delayed scripted sequence is
called **intro cinematic** (`IntroCinematic_*`). Nothing named `IntroCinematic_*` runs at
startup, and the splash functions never run at new-game time.

## Flow map

```
WinMain (0x00871450)
  -> NovaProgramEntry (0x00503f30)
     -> NovaApp_Run (0x004d2a80)
        -> NovaGameSession_Run (0x00416100)        // heavy bootstrap
           -> NovaUi_PresentLoadingSplashFrame     // loading splash (PICT 0x1fa4)
           -> ... subsystem + data-table init ...
           -> NovaUi_PresentStartupSplashFrame     // startup splash (PICT 0x83)
           -> ResourceData_VerifyCatalogChecksum   // full-catalog checksum (skipped in port)
           -> NovaUi_RunProgressBarReveal          // startup progress bar
           -> NovaMainLoop_Run                     // persistent main loop (menu → game)
              -> NovaMainLoop_UpdateFrame
              -> NovaCommand_DispatchToMode        // poll command -> mode action
                 -> NovaGameMode_DispatchAction    // menu actions (new/open/quit/...)
                    - action 0  Menu_RunNewGameFlow           (new game)
                       -> Menu_RunPilotSelectionDialog
                       -> IntroCinematic_SetupFrames  // configure INTRO CINEMATIC data (not splash)
                    - action 1  Menu_OpenPilotFileDialog      (open pilot save)
                    - action 2  quit (DAT_00596d39 = 1)
                    - action 3  Ship_RunSpaceflightMode        (enter spaceflight)
                       -> IntroCinematic_Run        // play INTRO CINEMATIC (new pilot only)
                    - action 4  Menu_RunSettingsDialog        (preferences)
                    - action 5/6 Menu_RunAboutNovaDialog     (About Nova, d£sc 0x7fff)
        -> QuickTime_Terminate
```

## Splash frames (startup, pre-menu)

All splash/intro/panel compositing shares one offscreen surface:

- `DAT_00597950` — shared offscreen splash/panel DrawContext handle (the full-size gameplay
  offscreen surface allocated by `FUN_004ac950`; companion surface `DAT_00597944`).
- `DAT_00597954` — full-game-size rect bounding that offscreen surface.

`NovaUi_PresentLoadingSplashFrame` (0x004ab070)
- Loading splash shown during blocking startup transitions.
- Loads PICT resource `0x1fa4`, centers it into the shared offscreen rect `DAT_00597954`, blits
  to the shared surface `DAT_00597950` and to the render owner, then finishes the blocking
  transition frame.

`NovaUi_PresentStartupSplashFrame` (0x004aaf60)
- Startup splash shown near the end of game-session init (`NovaUi_ClearMainWindowAndHoldFrame`
  follows the data-table loads).
- Loads PICT resource `0x83`, centers/blits to the shared surface, then blits the surface to the
  render-owner rect and commits the frame.

### Startup progress bar

Immediately after the startup splash, `NovaGameSession_Run` runs the loading progress bar over
that same splash while `NovaData_LoadAllShipClassVisualAndLaunchData` preloads the per-class ship
visuals. (In the original the startup splash is first shown alone while `ResourceData_VerifyCatalogChecksum`
0x004cd7e0 walks every resource type; the port skips that check and reveals the bar at once.)

| addr | function | role |
|------|----------|------|
| `0x004ab1b0` | `NovaUi_RunProgressBarReveal` | Seeds total = `ResourceData_CountEntries(0x73689570)` ('ship'), resets value, then expands the bar outline in from a flat line (`MarkTickAndWait(2)` per step) and calls the redraw. |
| `0x004ab3a0` | `NovaUi_ProgressCallbackNoOp` | No-op progress sink. |
| `0x004ab3b0` | `NovaUi_AddProgressAndRedraw` | Adds to `g_loading_progress_value` (0x0085d140) and redraws. |
| `0x004ab3d0` | `NovaUi_RedrawProgressBar` | Draws the bar onto `DAT_00597950` and blits to the owner. |

Data source (Bible `cölr`, loaded at `0x004c69ab`):

- Bar outline `+0x5e..+0x64`, native QuickDraw order (top,left,bottom,right), relative to the
  window center. Shipped Colors record: top `280`, left `-100`, bottom `290`, right `100`
  (a 200x10 reference bar near the bottom of a 1024x768 window).
- `ProgBright` `+0x66` = bright fill, `ProgDim` `+0x6a` = fill outline, `ProgOutline` `+0x6e` =
  bar outline (shipped: red 255/128 and gray 64).
- Fill span `DAT_00575a58 = 198.0` reference px.

The only progress producer is `NovaData_LoadAllShipClassVisualAndLaunchData` (0x004aeda0), which
calls `NovaUi_AddProgressAndRedraw(1.0)` once per successfully loaded ship class.

## Main menu renderer

`NovaRender_RedrawAndPresentFrame` (0x004873b0) is the central main-frame redraw + present
helper. It draws **either** the in-game HUD status panel **or** the main-menu title prompt,
depending on the game-active flag:

- `DAT_00596d28` — game-session active flag.
  - `0`  → no game loaded → **main menu / title screen**.
  - `!=0` → pilot game running (set by Menu_RunNewGameFlow and pilot-file open).
- Menu branch: when `DAT_00596d28 == 0` and the third slide-reveal counter has
  reached its threshold (`g_hud_overlay_decay_counters[2] >=
  g_hud_overlay_decay_thresholds[2]`), it draws the steady centered prompt
  string (STR# 0x7d2 entry 0x114, 1-based → "No Pilot File Loaded"). There is
  no blink timer: the gate is the reveal completion, and the string stays up.
  The older "pulsing prompt / blink timer DAT_007d251c" reading was a
  misinterpretation — DAT_007d251c/007d254e ARE that counter/threshold pair.
- Active-game branch: draws the bottom pilot status panel (Pilot Name / Ship
  Name / Ship Class left column; "Legal status in current system" (ladder STR#
  0x86, 0x00468d90) / Combat Rating (0x00469030, STR# 0x8a) / Current Date
  right column; "<name> has been killed" when destroyed, with a "Oh my God!
  They killed Kenny!" easter egg for a pilot named Kenny, DAT_0056ce68) and
  the clone-source ship portrait (DAT_00596d44) centered between the columns.
  Text is Geneva size 9 (g_main_menu_font_id 3, 0x004b32aa), labels dark red
  (DAT_00735652 = RGB555 0x84d0,0,0) and values bright red (DAT_0073564c =
  0xffff,0,0).

### Main-menu command/action mapping

`NovaCommand_DispatchToMode` (0x004872a0) maps polled command tokens into
`NovaGameMode_DispatchAction` (0x00486ed0) action codes:

| token | action | meaning |
|-------|--------|---------|
| `n`   | 0      | New Game (`Menu_RunNewGameFlow`) |
| `o`   | 1      | Open Pilot (`Menu_OpenPilotFileDialog`) |
| `q`   | 2      | Quit (sets `DAT_00596d39`) |
| `e`   | 3      | Enter Spaceflight (`Ship_RunSpaceflightMode`) |
| `p`   | 4      | Preferences (`Menu_RunSettingsDialog`, dialog `0xfa3`) |
| `a`   | 5/6    | About Nova (`Menu_RunAboutNovaDialog` 0x00486120; loads dësc 0x7fff "About text" into the selection dialog, DLOG `0xbbb`; the spœn 605 button is labelled ABOUT NOVA) |
| `x`   | —      | Immediate travel-selection dialog |

- `Menu_OpenPilotFileDialog` (0x004c9e90) — GetOpenFileNameA pilot selector; on selection
  resets ship state and loads the save via `PilotFile_LoadSave` (0x004cb260).
- `Menu_RunNewGameFlow` (0x00489d70) — full new-game init: pilot selection dialog, player reset,
  scenario tables, starting destination, `IntroCinematic_SetupFrames`, sets `DAT_00596d28=1`,
  renders.
- `Menu_RunPilotSelectionDialog` (0x0048a7e0) — pilot choose/name dialog (variant 0xc1d/0xc1e).
- `Menu_RunSettingsDialog` (0x00488650) — preferences dialog (0xfa3).
- `Menu_RunAboutNovaDialog` (0x00486120) — About Nova dialog.

## Intro cinematic — NOT the splash, plays only on new-game flight entry

`IntroCinematic_Run` (0x0048adc0) plays the intro cinematic
when a pilot first enters spaceflight (gated on `g_intro_played`). This is distinct from the
boot-phase splash above; it is a timed scripted sequence tied to a starting a run.

- `g_intro_played` — intro-played latch, set to 1 in `Ship_RunSpaceflightMode` after the cinematic
  first runs; **persisted in the pilot save** (offset `0x3086`, restored by `PilotFile_LoadSave` and
  written back by the save writer `PilotFile_SaveGameCore`).
- For each of (up to 4) intro frames: loads/fills the frame PICT (id from
  g_intro_cinematic.source_pict_ids[i]), centers and blits it to the shared offscreen surface
  `DAT_00597950`, then waits the per-frame duration (g_intro_cinematic.duration_60h_ticks[i],
  in 1/60s ticks → ms = ticks*60). **Frame ids below 0x80 are rewritten to -1 with duration 0**
  and valid frames' durations clamp to `[0, 300]` (both in `IntroCinematic_SetupFrames`).
- Input split (verified against the key-state polling in `Input_PumpAndTestCommand` →
  `FUN_004f1900`, a *level* table, and the delay-free wait loop): Enter (0x1c), Space (0x39)
  and an in-rect left click advance only the current frame. The in-rect path is the original's
  `local_19` latch, driven by the platform mouse-ready flag (`DAT_008701a0`, set on
  button-down and held until button-up in `FUN_004d7330`) while the cursor sits inside the
  render-owner rect — so a single click advances exactly one frame. The separate **skip
  command** (`g_player_key_bindings[0x17]`, default `0x01` = PC scancode 1 = **Escape**, not the
  mouse: `g_key_state_snapshot` is only fed by keyboard scancodes at the WM_KEYDOWN arm) latches
  `bVar9`, which skips **all** remaining frames and suppresses the intro-text dialog. The
  Ghidra `NovaPrefs_ResetKeyBindings` comment labels slot 0x17 "mouse-btn", but that is a
  mislabel — VK_LBUTTON is 1 yet the value is compared against the scancode-keyed
  `g_key_state_snapshot`, where 1 is Escape.
- Each arted frame also plays **snd 0x7533** (loaded via `NovaSound_LoadDecodedById`
  0x004bc2a0; `NovaAudio_QueueCenteredSound` →
  `Audio_AllocateVoiceSlot` queues it centered). Stock Nova ships no snd 0x7533, so the intro
  is silent there. There is **no** on-screen hint text in the original.
- After the sequence, if not skipped and an intro text dësc is set
  (`g_intro_cinematic.intro_text_desc_id != -1`, the Bible chär `IntroTextID` at block `+0x30`)
  it loads that dësc (`Ui_LoadSelectionDialogResource` 0x004c6d50) and shows it in the generic
  scrolling text-reader `Ui_RunTravelSelectionDialog` (0x004982a0, gating `DAT_007d1fa6 = 1`).
  The "travel" in that function name is a misnomer — it is the game's generic desc-text reader.
  **Stock data never opens it**: the .Trader block carries `intro_text_desc_id = -1`; the 0x7ffd
  "open with empty text" value only occurs in the no-save fallback.
  See `docs/char_resource_format.md` for the `chär` `+0x30` field and the other
  character-template fields.

Intro frame data lives in a single typed structure:

```c
/** base 0x007d1f42, size 18 */
struct IntroCinematicData {
    short intro_text_desc_id;      // +0x00 chär IntroTextID: dësc shown after the cinematic (-1/0x7ffd none)
    short source_pict_ids[4];      // +0x02 intro frame PICT ids (0xffff terminates)
    short duration_60h_ticks[4];   // +0x0A per-frame wait in 1/60s ticks (clamped [0,300])
};
extern IntroCinematicData g_intro_cinematic;   // resolves from 0x007d1f42
```

`IntroCinematic_SetupFrames` (0x004cd3b0)
- Resolves the pilot block via `ResourceData_AccessByKey(0x63688a72, key)` where key is the
  **selected character template's registered name** (`DAT_007d22b7`, filled from the pilot
  dialog; `Menu_RunNewGameFlow` falls back to family entry 1 when the 0xc1e variant leaves it
  empty) and populates `g_intro_cinematic`: source_pict_ids (block `+0x20`), duration_60h_ticks
  (block `+0x28`), and intro_text_desc_id (block `+0x30`, Bible chär `IntroTextID`).
- When no pilot block exists, defaults to a single intro frame PICT `0x2008` for 10 ticks with
  intro text desc 0x7ffd (not a valid desc: the reader opens with empty text).
- Companion accessors: `ResourceData_AccessByKey` (0x004ce300) and
  `PilotData_FindActivePilotName` (0x004cd290, first family entry with flags bit 0 set at
  block+0x132 — stock: .Trader — used to preselect the dialog's Character popup).
- Called **only** from `Menu_RunNewGameFlow` (new game), never during app boot, and before
  `IntroCinematic_Run` plays.

`View_ResetCameraAndHover` (0x00486790) clears g_intro_cinematic.intro_text_desc_id /
source_pict_ids to `-1` before the main loop, so the intro only plays for a freshly configured
game.

## Naming conventions

- **Splash** (boot-phase static PICT): `NovaUi_Present*SplashFrame`.
- **Intro cinematic** (new-game delayed sequence): `IntroCinematic_SetupFrames` /
  `IntroCinematic_Run`, struct `IntroCinematicData`, global `g_intro_cinematic`.
- Main-menu actions: `Menu_*`.
- Frame/overlay: `NovaRender_*` / `NovaHud_*`.
