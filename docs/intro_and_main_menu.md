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
                    - action 5/6 Menu_OpenGalaxyMapDialog     (galaxy/starmap)
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
- Menu branch: when `DAT_00596d28 == 0` and the blink/pulse timer `DAT_007d251c` has reached
  its threshold `DAT_007d254e`, it draws the pulsing centered prompt (string key `0x7d2/0x114`).
- Active-game branch: draws the status panel (ship name, load, system, combat rank, date,
  ship image) and the top-of-screen info rows.
- In both branches it blits the 6 focus sprites (`DAT_00596cb8`) and draws the HUD focus overlay.

Blink/pulse timers (shared word array at `0x007d2518`, 3 elements = `g_timer_decay_counters`,
`DAT_007d251a`, `DAT_007d251c`), reset at main-loop entry and clamped to 32000 when input is
ready; compared against thresholds `DAT_007d254a/4c/4e`. `DAT_007d251c`/`DAT_007d254e` drive the
main-menu prompt pulse.

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
| `a`   | 5/6    | Galaxy/Starmap (`Menu_OpenGalaxyMapDialog`) |
| `x`   | —      | Immediate travel-selection dialog |

- `Menu_OpenPilotFileDialog` (0x004c9e90, formerly `FUN_004c9e90`) — GetOpenFileNameA pilot selector; on selection
  resets ship state and loads the save via `FUN_004cb260`.
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
  first runs; **persisted in the pilot save** (offset `0x3086`, restored by `FUN_004cb260` and
  written back by the save writer `FUN_004c7dd0`).
- For each of (up to 4) intro frames: loads/fills the frame PICT (id from
  g_intro_cinematic.source_pict_ids[i]), centers and blits it to the shared offscreen surface
  `DAT_00597950`, then waits the per-frame duration (g_intro_cinematic.duration_60h_ticks[i],
  in 1/60s ticks → ms = ticks*60) with input-to-skip (enter/space/primary). `0xffff` on a
  frame id terminates the frame list.
- After the sequence, if a post-intro travel destination is set
  (g_intro_cinematic.post_intro_dest_id != -1) it opens the intro travel-selection dialog.

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
- Reads the pilot-save data block (`FUN_004ce300`, key `0x63688a72`) and populates
  `g_intro_cinematic`: source_pict_ids (block `+0x20`), duration_60h_ticks (block `+0x28`), and
  post_intro_dest_id (block `+0x30`).
- When no pilot save exists, defaults to a single intro frame PICT `0x2008` for 10 ticks.
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
