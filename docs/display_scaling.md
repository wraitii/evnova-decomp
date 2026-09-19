# Display and UI scaling (design note)

Status: **placement model implemented; user-facing scale settings remain design
options.** Section 1 describes the port. Later sections record future scaling
choices, not reconstructed original behaviour.

For the original dialog geometry see `docs/dlog_ditl_dialog_format.md`; for the
port's modal compositing rules see its §7. The CE's experimental scaling (the
only reference implementation) is a binary patch; it is **not** original
Ambrosia code, and the port does not model it.

## 1. Current presentation model

Each screen owns its authored dimensions and chooses an explicit `Placement`.
Its `dst` is a content box in window points; `scale` maps authored coordinates
into that box. `ToWindow` and `ToAuthored` are the corresponding forward and
inverse mappings. There is no global 640x480 canvas or presentation mode.

| content | authored size | default placement |
|---|---|---|
| main menu / intro composition | 1024x768 | contained, centred, capped at 1x |
| startup / loading picture | decoded picture size (stock 832x624 / 369x558) | contained, centred, capped at 1x |
| landed spaceport panel | 618x517 | contained, centred, capped at 1x |
| fixed dialogs | actual DLOG or composition dimensions | contained, centred, capped at 1x |
| free-flight world | current window size | identity in window points; larger windows show more world |

`PlaceContained` uses `min(1, window_width/authored_width,
window_height/authored_height)`. Fixed screens clear their margins to black.
The menu's button coordinates are native 1024x768 coordinates, as specified by
the Bible's `Button1x & y` fields; art no longer passes through a 640x480 fit.
Missing splash/menu PICTs leave black with an error log; missing menu button
sheets retain basic clickable button rectangles.

Native 1x means **one window point per authored unit**, not one backing pixel.
`SdlPlatform` applies `placement.scale * WindowPixelDensity()` to drawing and
text rasterisation. Raster pictures therefore still scale on HiDPI displays.
`playfield_window_rect()` reports the active placement's `dst`;
`logical_playfield_size()` continues to report window dimensions for free
flight. Mouse input is mapped through the active placement. Probe UI rectangles
are published in window points using the same forward mapping.

Placement scopes restore the previous transform. A modal captures its
background content box before selecting its own placement; background callbacks
draw without presenting. Resizing recomputes placement from its rule and authored
dimensions, rather than reusing a stale destination rectangle. Existing
640-space compositions can retain their local coordinates without making 640
a platform-wide rule.

Dialog containment uses the available window, even when its background panel
is smaller than the dialog. SDL's viewport is integral and affected by render
scale, so the platform canonicalises destination bounds to that viewport and
uses the same bounds for drawing, input, and probes. The text reader retains
its original 0.3-times-shrink vertical offset inside its authored DLOG canvas.

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

The original targets a 1024x768 native menu and requires an 800x600 screen
minimum. The port defaults to native window-point sizing. Users with large
displays may want larger ships, HUD chrome, or reading windows independently
of how much world the window reveals. HiDPI rasterisation improves text detail
but does not enlarge its size in window points.

## 3. Axis A — full-game scale

Give the extending world a placement with scale `s` and authored visible extent
`window_size / s`, so **world, HUD and all text grow together**. The window
shows proportionally less of the system (or the window must grow to compensate).

* The placement owns both the draw transform and inverse mouse mapping.
* Must be reflected in `logical_playfield_size()`, `playfield_window_rect()`,
  `text_raster_scale()`, the probe's published geometry, and any place that
  converts window points to logical units by hand.
* Fixed screens need an explicit choice of requested scale and containment cap;
  the default native-size cap must not silently defeat a user-selected scale.
* This is the "everything is bigger" mode; it does **not** solve wanting a
  bigger HUD *and* a wider world at the same time.

## 4. Axis B — GUI scale (CE `ui_scale` equivalent)

Scale **only the HUD chrome and dialogs**, leaving the world in `PlaceWindow`
1:1 (so the same amount of world fits, but the UI is legible). This is what the
CE's `ui_scale` does and is the closest thing to a reference.

Would require:

* scaling the metrics in §1 (status-bar strip, grid cells/thumbs, list pitch)
  and the HUD text offsets that are currently raw constants;
* selecting a scaled placement for DLOG windows (`Dialog_CreateFromDlog`
  is `0x008730A1`), retaining their authored DITL geometry;
* selecting the HUD placement for its draw and input passes, using
  `ToAuthored` for hit-tests and `ToWindow` for probe rectangles;
* deciding whether dialogs follow the GUI scale or the full-game scale when
  both are on (precedence rule).

The shared placement model provides the coordinate mechanism. HUD/world
ownership, clipping, scale precedence, and user settings still need explicit
design; adding a multiplier to the renderer alone is insufficient.

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
and each control rect from its item offsets. Enlarging the whole authored
composition through its placement preserves relative geometry and maps input
back to those same coordinates. Remaining choices:

* choose whole-window enlargement versus larger text with reflow;
* for whole-window enlargement, retain authored text sizes and row pitches and
  let `text_raster_scale()` provide destination-resolution glyphs;
* for text-only enlargement, recompute wrapping, row pitch, scroll extent, and
  arrow positions together.

Frame art, controls, and input must use the same dialog placement. Resizing the
authored layout instead of scaling it requires re-anchoring the offer window's
top/bottom strips (the CE's `scaleAndShiftRect_bottom` does this).

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
3. How should user-selected enlargement interact with containment in small
   windows, and which scale takes precedence when A, B, and C overlap?
4. Keep C scoped to the mission offer/info dialogs, or generalise it to any
   DLOG-based text window once the mechanism exists?
5. How do probe screenshots/geometry and the `text_raster_scale` contract stay
   correct for independently scaled world, HUD, and modal placements?
