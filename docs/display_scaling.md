# Display and UI scaling (design note)

Status: **options note, not implemented.** The current build has no user-facing
scale setting. This note records the axes a future scale feature could take,
how each maps onto the port's existing presentation model, and the open
decisions. It is a design note, not reconstructed original behaviour.

For the original dialog geometry see `docs/dlog_ditl_dialog_format.md`; for the
port's modal compositing rules see its §7. The CE's experimental scaling (the
only reference implementation) is a binary patch; it is **not** original
Ambrosia code, and the port does not model it.

## 1. Current presentation model

The port keeps a **640x480 logical content canvas** for its static art screens
(the splash, main menu and intro - called "fixed screens" in `SdlPlatform`
because their backdrop is a fixed-size picture that gets upscaled). It lets the
window be larger. That 640x480 is a port layout choice, not a game-wide
floor: Nova's native art is authored for a **1024x768** canvas (main-menu
background, the 194x767 cockpit strip) and the original refuses to launch below
an **800x600** screen (`EVNova.ini` S38; the beta history mentions centering on
"screens 800x600 or smaller"). Free flight and several modals already draw in
window-point space and routinely exceed 640x480. `SdlPlatform`
(`src/sdl_platform.hpp` / `sdl_platform.cpp`) selects one of these policies per
frame:

| policy | used by | mapping |
|---|---|---|
| `kScaledPlayfield` | splash, main menu, intro | 640x480 canvas letterboxed uniformly up to the window (SDL logical presentation) |
| `kCenteredPlayfield` | docked/landed screens, starmap, some in-flight modals | 640x480 canvas at native window-point size, integer-centred, black bars |
| `kFullscreenPlayfield` | free flight (`spaceflight_view`) and several flight modals | 1 logical unit = 1 window point, world extends to the whole window; HUD chrome stays fixed size |
| `ApplyWindowPointDrawing` | `ui_dialog` modal runtime | 1 logical unit = 1 window point, no logical presentation; modals composite over the presented frame at 1:1 |

`WindowPixelDensity()` (`SDL_GetWindowPixelDensity`) multiplies the SDL render
scale in the fullscreen/centred/point modes, so on a HiDPI display one logical
unit is one window *point* (2+ backing pixels). `text_raster_scale()` uses the
same density (and the letterbox factor for `kScaled`) so text is rasterised at
the destination resolution and then drawn at its logical size.

**Reference fixed metrics the port currently hard-codes** (and which a GUI scale
would have to move):

| port constant | value | original/CE global |
|---|---|---|
| `kGameplayHudStripWidth` (`gameplay_interface.hpp`) | `0xC2` = 194 | CE `g_statusBarWidth` (`DAT_0088c020`) |
| grid cell width × height | 83 × 54 | CE `g_gridCellWidth` / `g_gridCellHeight` |
| grid thumb size | 32 | CE `g_gridThumbSize` |
| list row base height | 8 (row pitch 12 = 8×1.5) | CE `g_listItemBaseHeight` (`DAT_0088c01c`) |
| mission text size | `9.0F` | shared screen font DAT_00735686 |

The CE `ui_scale` scales all of those by a factor, then patches ~87 call sites
plus two asm hooks to reposition text and remap clicks. The port would express
the same intent with a scale factor on the layout metrics instead of call-site
hacks.

## 2. Why scale

The original targets a 1024x768 native canvas and requires an 800x600 screen
minimum; the port's static art screens use a 640x480 logical canvas. On a
1440p/4K display the port's `kFullscreen` gameplay maps one logical unit to one
window point, so a 32px ship sprite and the 194px HUD strip are physically small
and text is hard to read. Those art screens already upscale (`kScaled`); free
flight does not.

## 3. Axis A — full-game scale

Multiply the renderer scale for the extending world (i.e. `ApplyWindowPointDrawing`
uses `density * s`), so **world, HUD and all text grow together**. The window
shows proportionally less of the system (or the window must grow to compensate).

* Cheapest to implement: one multiplier in the presentation helpers; SDL then
  maps input coordinates consistently, so most hit-testing needs no change.
* Must be reflected in `logical_playfield_size()`, `playfield_window_rect()`,
  `text_raster_scale()`, the probe's published geometry, and any place that
  converts window points to logical units by hand.
* Applies naturally to all four policies; for `kScaled` it is an extra factor
  on top of the letterbox mapping.
* This is the "everything is bigger" mode; it does **not** solve wanting a
  bigger HUD *and* a wider world at the same time.

## 4. Axis B — GUI scale (CE `ui_scale` equivalent)

Scale **only the HUD chrome and dialogs**, leaving the world at `kFullscreen`
1:1 (so the same amount of world fits, but the UI is legible). This is what the
CE's `ui_scale` does and is the closest thing to a reference.

Would require:

* scaling the metrics in §1 (status-bar strip, grid cells/thumbs, list pitch)
  and the HUD text offsets that are currently raw constants;
* scaling DLOG window size/centring and DITL item rects (`Dialog_CreateFromDlog`
  is `0x008730A1`; the port path is `ui_dialog.cpp` `CenterDialogInPlayfield`
  plus the item layout);
* a mouse remap from the scaled draw space back to layout space (CE
  `FUN_008734E5`), or drawing the scaled HUD/dialogs through a separate SDL
  logical-presentation sub-rect so SDL maps input for us;
* deciding whether dialogs follow the GUI scale or the full-game scale when
  both are on (precedence rule).

Blocking inconsistency to resolve first: the port's modals do not share one
presentation — some use `kCenteredPlayfield`, some `ApplyWindowPointDrawing`,
some `kFullscreenPlayfield` after re-rendering the world (see §1). A GUI scale
needs a single, documented coordinate rule for modal windows.

## 5. Axis C — mission-dialog scale

An independent multiplier for the **mission dialogs only**: the read-only
mission offer/briefing window (DLOG `0x3f8`, variant `0x3fc`) and the in-flight
mission info / active-missions window (DLOG `0x3f4`). It enlarges the whole
window — frame, DITL item rects, text and hit-tests — without moving the rest
of the GUI. The Mission BBS (DLOG `0x3ee`) and the ship/planet comms windows
(`0x3ef`/`0x3f1`) are explicitly **out of scope**: they are considered fine
as-is.

This is the least invasive axis because those windows already parse their DLOG
at runtime. `LayoutMissionInfo` and `NovaMission_RunOfferWindow`
(`docked_mission_dialog.cpp`) derive the frame from `right-left`/`bottom-top`
and each control rect from its item offsets, so a scale applied to
`win_w`/`win_h` and to each item's `left/top/right/bottom` propagates through
the stored `SDL_FRect`s and hit-tests follow for free. Remaining work:

* scale the text constants (`kMissionListFontSize` / `kMissionInfoFontSize` =
  `9.0F`) and the list row pitch (base height 8 × the 1.5 double = 12);
* keep the `WrapDescriptionLines` / `LineHeight` reflow and the scroll extent in
  scaled units;
* scale the offer window's scroll-arrow offsets (the text view's content
  translation) with the same factor.

These modals draw in window-point space (`SetFullscreenPlayfield` /
`ApplyWindowPointDrawing`), so no extra coordinate mapping is needed. Drawing
the window larger than the authored DLOG also means the backdrop art must
follow: `0x3f4` stretches one frame PICT into the frame rect (already an
`SDL_RenderTexture`), while the `0x3f8` offer window composites top/bottom art
strips and would need them anchored to the scaled frame (the CE's
`scaleAndShiftRect_bottom` does the same).

The CE's `ui_scale` bundles these windows into the global GUI scale; this axis
deliberately separates them so mission readability can be tuned on its own.

## 6. Interaction and settings

* A and B are independent multipliers; B is best expressed as a scale applied to
  GUI layout coordinates, A as a multiplier on the world render scale.
* C is independent of both (mission dialogs only).
* Where the setting lives is open. Original `.prf` must stay byte-compatible, so
  a port setting belongs in the planned sibling `EV Nova Extra Prefs` INI
  (see `docs/preferences_keybindings.md` Persistence), or the port can read
  `ddraw.ini [EV Nova] ui_scale` to match the CE exactly. Use the shared
  compatibility policy for anything non-default.

## 7. Open decisions

1. Default behaviour on large displays: show more world at 1:1 (current), or
   scale up (A) by default?
2. Do we want CE parity (`ui_scale` = B, from `ddraw.ini`) or a port-native
   setting with separate A/B/C factors?
3. Normalise modal presentation (§4) before attempting B, or special-case the
   modal set?
4. Keep C scoped to the mission offer/info dialogs, or generalise it to any
   DLOG-based text window once the mechanism exists?
5. How do probe screenshots/geometry and the `text_raster_scale` contract stay
   correct under each mode?
