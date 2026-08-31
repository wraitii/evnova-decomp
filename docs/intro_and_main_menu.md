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
   pilot's first flight entry (gated on `DAT_00596d35`), never during app boot.

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
                    - action 5/6 Menu_OpenGalaxyMapDialog     (About Nova; misnomer, d£sc 0x7fff)
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
| `a`   | 5/6    | About Nova (`Menu_OpenGalaxyMapDialog` 0x00486120 — misnomer; loads dësc 0x7fff "About text" into the selection dialog, DLOG `0xbbb`; the spœn 605 button is labelled ABOUT NOVA) |
| `x`   | —      | Immediate travel-selection dialog |

- `Menu_OpenPilotFileDialog` (0x004c9e90, formerly `FUN_004c9e90`) — GetOpenFileNameA pilot selector; on selection
  resets ship state and loads the save via `PilotFile_LoadSave` (0x004cb260).
- `Menu_RunNewGameFlow` (0x00489d70) — full new-game init: pilot selection dialog, player reset,
  scenario tables, starting destination, `IntroCinematic_SetupFrames`, sets `DAT_00596d28=1`,
  renders.
- `Menu_RunPilotSelectionDialog` (0x0048a7e0) — pilot choose/name dialog (variant 0xc1d/0xc1e).
- `Menu_RunSettingsDialog` (0x00488650) — preferences dialog (0xfa3).
- `Menu_OpenGalaxyMapDialog` (0x00486120) — travel-selection/galaxy dialog.

## Intro cinematic — NOT the splash, plays only on new-game flight entry

`IntroCinematic_Run` (0x0048adc0, formerly `Ship_RunStartupSequence`) plays the intro cinematic
when a pilot first enters spaceflight (gated on `DAT_00596d35`). This is distinct from the
boot-phase splash above; it is a timed scripted sequence tied to a starting a run.

- `DAT_00596d35` — intro-played latch, set to 1 in `Ship_RunSpaceflightMode` after the cinematic
  first runs; **persisted in the pilot save** (offset `0x3086`, restored by `PilotFile_LoadSave` and
  written back by the save writer `PilotFile_SaveGameCore`).
- For each of (up to 4) intro frames: loads/fills the frame PICT (id from
  g_intro_cinematic.source_pict_ids[i]), centers and blits it to the shared offscreen surface
  `DAT_00597950`, then waits the per-frame duration (g_intro_cinematic.duration_60h_ticks[i],
  in 1/60s ticks → ms = ticks*60). **Frame ids below 0x80 are rewritten to -1 with duration 0**
  and valid frames' durations clamp to `[0, 300]` (both in `IntroCinematic_SetupFrames`).
- Input split (verified against the key-state polling in `Input_PumpAndTestCommand` →
  `FUN_004f1900`, a *level* table, and the delay-free wait loop): Enter (0x1c) and Space (0x39)
  advance only the current frame; the **primary command** (`g_player_key_bindings[0x17]`, default
  0x01 = the mouse) latches `bVar9`, which skips **all** remaining frames and suppresses the
  post-intro dialog. A separate in-rect click latch (`local_19`) advances one frame, but is only
  reachable when a full press+release lands inside one poll interval, so in practice a click
  skips the whole intro.
- Each arted frame also plays **snd 0x7533** (loaded via `NovaSound_LoadDecodedById`
  0x004bc2a0, formerly misnamed `LoadStringResourceCopyById`; `NovaAudio_QueueCenteredSound` →
  `Audio_AllocateVoiceSlot` queues it centered). Stock Nova ships no snd 0x7533, so the intro
  is silent there. There is **no** on-screen hint text in the original (an earlier reading
  treated the sound id as a string resource — that was wrong).
- After the sequence, if not skipped and a post-intro travel destination is set
  (g_intro_cinematic.post_intro_dest_id != -1) it opens the intro travel-selection dialog
  (`Ui_LoadSelectionDialogResource` + `Stellar_BuildTravelDestinationDescription` +
  `Ui_RunTravelSelectionDialog`, gating `DAT_007d1fa6 = 1`). **Stock data never opens it**: the
  .Trader block carries post_intro_dest_id = -1; the 0x7ffd "open anyway" value only occurs in
  the no-save fallback.

Intro frame data lives in a single typed structure:

```c
/** base 0x007d1f42, size 18 */
struct IntroCinematicData {
    short post_intro_dest_id;      // +0x00 destination opened after the cinematic (-1/0x7ffd none)
    short source_pict_ids[4];      // +0x02 intro frame PICT ids (0xffff terminates)
    short duration_60h_ticks[4];   // +0x0A per-frame wait in 1/60s ticks (clamped [0,300])
};
extern IntroCinematicData g_intro_cinematic;   // resolves from 0x007d1f42
```

`IntroCinematic_SetupFrames` (0x004cd3b0, formerly `FUN_004cd3b0`)
- Resolves the pilot block via `ResourceData_AccessByKey(0x63688a72, key)` where key is the
  **selected character template's registered name** (`DAT_007d22b7`, filled from the pilot
  dialog; `Menu_RunNewGameFlow` falls back to family entry 1 when the 0xc1e variant leaves it
  empty) and populates `g_intro_cinematic`: source_pict_ids (block `+0x20`), duration_60h_ticks
  (block `+0x28`), and post_intro_dest_id (block `+0x30`).
- When no pilot block exists, defaults to a single intro frame PICT `0x2008` for 10 ticks with
  destination 0x7ffd.
- Companion accessors: `ResourceData_AccessByKey` (0x004ce300) and
  `PilotData_FindActivePilotName` (0x004cd290, first family entry with flags bit 0 set at
  block+0x132 — stock: .Trader — used to preselect the dialog's Character popup).
- Called **only** from `Menu_RunNewGameFlow` (new game), never during app boot, and before
  `IntroCinematic_Run` plays.

`View_ResetCameraAndHover` (0x00486790) clears g_intro_cinematic.post_intro_dest_id /
source_pict_ids to `-1` before the main loop, so the intro only plays for a freshly configured
game.

## Naming conventions

- **Splash** (boot-phase static PICT): `NovaUi_Present*SplashFrame`.
- **Intro cinematic** (new-game delayed sequence): `IntroCinematic_SetupFrames` /
  `IntroCinematic_Run`, struct `IntroCinematicData`, global `g_intro_cinematic`.
- Main-menu actions: `Menu_*`.
- Frame/overlay: `NovaRender_*` / `NovaHud_*`.
