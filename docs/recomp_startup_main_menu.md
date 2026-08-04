# Startup-to-main-menu reconstruction

This first recompilation slice intentionally follows the original high-level control flow:

```text
NovaProgramEntry
  -> NovaApp_Run
    -> NovaGameSession_Run
      -> NovaMainLoop_Run
        -> NovaMainLoop_UpdateFrame
          -> NovaGameMode_DispatchAction
        -> NovaRender_RedrawAndPresentFrame
```

`SdlPlatform` replaces the original Win32 window, event queue, rendering surface, and timing boundaries. QuickTime, archive/resource loading, installer/registration work, and all licence/tamper checks are out of scope for this slice.

The session now reproduces the original ordering of `NovaUi_PresentLoadingSplashFrame` (0x004ab070), `NovaUi_PresentStartupSplashFrame` (0x004aaf60), then the persistent main-menu loop. `Resource_LoadPictAsImage` now decodes the observed PICT v2 `DirectBitsRect` format (PackBits-compressed 16-bit RGB555) in C++ and displays the exact loading-splash resource, PICT `0x1fa4`, from `Nova Titles 1.rez` during the loading-splash phase.

`NovaUi_PresentStartupSplashFrame` deliberately uses a clean SDL Ambrosia screen instead of its original `PICT 0x83`. That exact resource is absent from the supplied Windows archive set; the comparable `PICT 900` Ambrosia asset is an indexed/QuickTime-flavoured PICT not yet supported by the narrow decoder. The temporary SDL replacement preserves the original routine's clear-then-image presentation and intentionally has no fictional progress indicator.

`NovaRender_RedrawAndPresentFrame` uses a fixed 640×480 letterboxed SDL composition as an interim equivalent of the original shared offscreen surface. It has the title/menu layout, star/planet backdrop, HUD frame, pulsing prompt, and hover focus regions. The BRGR adapter maps PICT `0x1fa4` to its actual title-archive region. The next fidelity step is mapping PICT `0x83` and decoding the six RLE menu focus sprites (resource ids 600–605). It exposes the original menu action mappings: New Game, Open Pilot, Quit, Preferences, and Star Map. The non-Quit action flows deliberately report that they remain unreconstructed.

## Main-menu asset findings

The shipped Nova Bible provides the resource-level contract for the menu:

- `sp.n` 600–605 are the six main-menu button sprites; `sp.n` 606 is the main-screen logo; 607–610 provide rollover and sliding effects.
- An `sp.n` describes a sprite image resource, a mask resource, per-tile size, and grid dimensions. It may point either to paired PICTs or to `rl.D`/`rl.8` data.
- In the supplied `Nova Graphics 3.rez`, the main-screen `sp.n` table is present and the associated `rl.D` entries resolve to PICT v2 payloads. This is substantially simpler than implementing the in-flight SpriteWorld renderer.
- The menu's native backdrop is 1024×768. Its `c...lr` resource supplies the six button positions relative to that backdrop, plus menu font, size, and bright/dim colors.

The reimplementation now has typed `sp.n` and `c...lr` readers for the supplied `Nova Graphics 3.rez`: it uses the documented sprite tile sizes, the native button origins, and the bright/dim menu palette for SDL hit regions and text. The six-button resource table is not fully mapped yet (`sp.n` 605 remains deliberately logged as unresolved), and paired PICT image/mask tiles still need decoding into SDL textures before the original focus artwork replaces the temporary text/rectangle presentation.
