# Display and UI scaling (implementation design)

Status: **architecture agreed; not yet implemented.** The port today has a
per-screen `Placement` model and a working extra-preferences file, but no
user-visible scale controls. This is the implementation task: it fixes
coordinate ownership and equations, maps the integration sites, and sequences
the work. Fit caps, numeric limits and the settings UI described below are
selected implementation proposals, not previously agreed facts.

Related: [dlog_ditl_dialog_format.md](dlog_ditl_dialog_format.md) (authored
DLOG/DITL geometry), [preferences_keybindings.md](preferences_keybindings.md)
(`EV Nova Prefs.prf` and the sibling `EV Nova Extra Prefs.ini`),
[probe_harness.md](probe_harness.md) (probe geometry contract).

## 1. Current state (verified)

- [placement.hpp](../src/util/placement.hpp) owns `dst` (window points),
  `scale`, authored/containing sizes and the reflow rule; `ToWindow` /
  `ToAuthored` / `ToWindowRect` are the forward/inverse mappings.
  `PlaceContained` fits at `min(1, W/Aw, H/Ah)`; `PlaceCenteredIn` centres a
  modal on its background; `Reflow` rebuilds a placement after a resize.
- [sdl_platform.hpp](../src/sdl_platform.hpp) /
  [sdl_platform.cpp](../src/sdl_platform.cpp) own the active placement and a
  push/pop stack, apply `placement.scale * WindowPixelDensity()` to the render
  scale, set the SDL viewport from `ToRenderViewport()`, map mouse input
  through `ToAuthored`, and expose `logical_playfield_size()` (**window points,
  unchanged semantics**), `playfield_window_rect()` (`placement.dst`) and
  `text_raster_scale()` (`placement.scale * density`). `mouse_position()` and
  `mouse_window_point()` are existing getters.
- There are no render targets; modals re-render a live background callback
  ([ui_dialog.cpp](../src/game/ui_dialog.cpp),
  [docked_mission_dialog.cpp](../src/game/docked_mission_dialog.cpp)), so a
  scaled modal keeps the background placement on the stack and pushes its own.
- Fixed screens and dialogs build their own `PlaceContained` with authored
  sizes (e.g. [nova_app.cpp](../src/nova_app.cpp),
  [landed_window.cpp](../src/game/landed_window.cpp),
  [starmap.cpp](../src/game/starmap.cpp),
  [preferences.cpp](../src/game/preferences.cpp),
  [docked_dialog.cpp](../src/game/docked_dialog.cpp),
  [docked_mission_dialog.cpp](../src/game/docked_mission_dialog.cpp)).
- Free flight ([spaceflight_view.cpp](../src/game/spaceflight_view.cpp))
  draws the world under `PlaceWindow(logical_playfield_size())` at 1:1 across
  the **full window**, then draws the HUD in the same 1:1 window-point space;
  larger windows show more system. `CurrentViewport` = full window minus 194,
  and `SpaceflightView::SyncGameplayViewport` writes `viewport_center_x/y`,
  read by asteroid scatter and mission visibility checks.
- [hud_renderer.cpp](../src/game/hud_renderer.cpp) loads the cockpit PICT and
  `.ntf` layout at `Install` time (`cockpit_w_`/`cockpit_h_`, `layout_` panel
  rects; the stock art is 194x767). `HudRenderer::Draw` draws the strip, radar
  and anchored panels, then calls `DrawEscortCommandsPanel` (a 160x120 box at
  absolute window `(15,150)`) and draws the transient overlay message
  (left `25`, bottom-anchored from `logical_playfield_size()`); those two are
  window/flight-anchored, not strip-anchored.
- [route_map.cpp](../src/game/route_map.cpp) `RouteMap_OverlayRect` is
  `max(200, round(W * 0.25))` in window-point-derived authored geometry,
  top-left anchored; `RouteMapView::Draw` runs after `DrawGameFrame`.
- Flight input (`SdlPlatform::PollFlightInput`) maps mouse through the active
  placement into `FlightInput::mouse_x/y` (floats). The flight loop runs
  `SyncGameplayViewport` **before** `PollFlightInput`, and resize events are
  consumed inside `PollFlightInput` (`RefreshPlacementAfterResize`);
  `DrawGameFrame` sets the world placement only at draw time.
- Text raster density already multiplies the active placement scale
  ([nova_font.cpp](../src/game/nova_font.cpp)).
- `game::NovaExtraPrefs`
  ([extended_prefs.hpp](../src/game/extended_prefs.hpp) /
  [extended_prefs.cpp](../src/game/extended_prefs.cpp)) currently stores only
  `[paths] install_root`; its parser ignores section headers and unknown keys.
  `_SaveToSystemStore` truncates the file, so new fields must be emitted there
  or they are erased by the install-root save ([nova_app.cpp](../src/nova_app.cpp)).

## 2. Target architecture

Three independent multipliers, all defaulting to `1.0`:

- **`U` — general UI scale.** Uniformly scales authored UI compositions:
  menus, splash/HUD chrome, fixed screens, dialogs, in-flight UI overlays.
  Centred compositions stay centred; the HUD stays right-anchored. Authored
  grid/list/font/HUD constants are unchanged; the transform scales them.
- **`F` — flight-scene scale.** Scales world rendering only (ships, sprites,
  starfield, effects, asteroids); less world is visible. HUD and UI are
  unaffected. Simulation units are not rescaled.
- **`M` — mission-dialog multiplier**, applied **on top of `U`**. Two
  entrypoints: `NovaMission_RunOfferWindow` (DLOG `0x3f8`, variant `0x3fc`)
  and `NovaMission_RunMissionInfoWindow` (DLOG `0x3f4`), which request
  `U * M`. The mission BBS (`0x3ee`) and ship/planet comms (`0x3ef`/`0x3f1`)
  request only `U`. The generic mission text-reader dialog defaults to `U`,
  not `M`, unless a later decision widens the mission DLOG scope.

This replaces the old full-game scale idea: `F` enlarges the world without
enlarging the UI.

### Equations

Let `W x H` be window points, `A = (Aw, Ah)` a composition's authored size,
`d = WindowPixelDensity()`. Arithmetic is float, but the camera viewport and
`state.viewport_center_x/y` are integers today. Preserve the existing
cast/order and derive **one** rounded authored viewport used by rendering,
picking and `SyncGameplayViewport`; do not mix a float `vp` with integer
centers. `Canonicalized()` / `ToRenderViewport()` keep their current rounding
for the placement rect; exactness is never assumed before snapping.

**Contained UI composition** (menus, fixed screens, dialogs, overlays):

```
r         = U             ordinary UI
r         = U * M         mission offer/variant and mission info only
fit       = min(W/Aw, H/Ah)
scale_ui  = min(r, fit)   single clamp after composing U and M
dst       = ((W - Aw*scale_ui)/2, (H - Ah*scale_ui)/2, Aw*scale_ui, Ah*scale_ui)
```

Composing `U * M` before one clamp avoids a double shrink. `r < 1` shrinks.

**HUD strip** (right-anchored; authored bounds from loaded art/layout):

```
s_hud     = min(U, W/art_w, H/art_h)   art_w/art_h = loaded nonzero art/layout bounds
origin_x  = W - kGameplayHudStripWidth * s_hud   native 194 anchor, unchanged at 1x
reserve_x = kGameplayHudStripWidth * s_hud
```

The origin stays `W - 194*s_hud`, **not** `W - art_w*s_hud`: the current code
anchors at `W - 194`, so nonstock wider art overflows right exactly as it does
today and a custom layout does not shift at 1x. Art is drawn at its loaded
dimensions; panel resource coordinates are unchanged. The strip-only drawing
scope may use a full-window scaled placement (`dst = (0,0,W,H)`, `scale =
s_hud`) with `render_right = W/s_hud` and the existing `HudPanel_AnchorTopRight`
helper, or an equivalent HUD-specific `dst` origin; either way the window stays
drawable so panels are not clipped to an art-sized box. Fit uses loaded
nonzero art height/layout bounds; if art is missing or zero-sized, fall back
to the logical `.ntf` layout bounds so fitting never divides by zero. The
native 194 anchor and `CurrentViewport` reserve stay the gameplay contract.
Custom art larger than the stock 194x767 is not promised to fit.

**Flight scene** (world fills the window; camera extent is separate):

```
play_w = max(kViewportWidth,  W) - reserve_x     window points
play_h = max(kViewportHeight, H)                 window points
vp     = (play_w / F, play_h / F)                authored world extent
scene_placement = window rule, dst = (0, 0, W, H), scale = F
```

World rendering computes an authored viewport-relative coordinate
`a = (world - camera) + vp/2`; the scene placement maps it to
`window = a * F`. The scene `dst` is the **full window**, exactly as today:
the 194 reserve only informs the camera/play extent, so at `F = 1` no black or
clipped band appears behind the HUD. Round `vp` once (preserving today's
truncation) and use that single integer value for rendering, picking and
`SyncGameplayViewport`; the latter publishes `viewport_center_x/y = vp/2`
(authored world units), so asteroid scatter and mission spawn bounds still
track the rendered frustum. Positions, velocities and other simulation
quantities are untouched. `kViewportWidth`/`kViewportHeight` (640x400) and the
1024x768 minimum window remain floors applied in window points before
dividing by `F`.

**Fullscreen effects** (hyperspace flash) are drawn under a neutral full-window
placement (scale 1) so they cover HUD and UI; they remain inside
`DrawGameFrame`, so the route map still draws after and above them.

### Coordinate ownership and mixed input

- `logical_playfield_size` keeps returning window points and is not redefined
  as world size.
- `FlightInput::mouse_x/y` are in the **active placement's** authored space.
  On resize, the event drain runs `RefreshPlacementAfterResize`; consumers must
  not assume `current_placement()` is unchanged after a modal pushes/pops.
- Robust recommendation: snapshot the raw window-point float (add
  `window_mouse_x/y` to `FlightInput`, captured beside the existing values from
  `mouse_window_point()`) and map per consumer. Avoiding a raw snapshot is
  acceptable only if each consumer uses the same placement that produced the
  snapshot, not an arbitrary later `current_placement()`.
- With the raw window-point snapshot, map it **directly** to each target via
  that target's `ToAuthored` (route-map overlay placement, scene placement,
  dialog placement). Only consumers still holding an already-mapped
  `FlightInput::mouse_x/y` need `ToWindow` first. Do not run raw window points
  through the inverse of a placement that did not produce them.
- `PlayerTick_MouseTargetAndControlCommands`
  ([spaceflight.cpp](../src/game/spaceflight.cpp)) feeds one snapshot to
  `RouteMap_HandleClick` and to `ClickInPlayerSprite` / `PickShipAt` /
  `PickStellarAt`; once `F != 1` those are different target placements.
- HUD/overlay click handling must be audited with the same rigor; absence of a
  handler in reviewed draw paths is not proof the HUD is non-interactive.

### Overlay scopes and z-order

- Only `DrawRadarPanel`, the cockpit strip, and the anchored `.ntf` panels use
  the right-anchored HUD placement. `DrawEscortCommandsPanel` and the overlay
  message are drawn in a separate **full-window UI placement** (scale `U`,
  top-left anchor) and must derive any bottom anchor from that placement's
  authored extent, not from raw `logical_playfield_size()`.
- Route-map fit/anchor (its own simple overlay rule): baseline
  `b = max(200, round(W*0.25))` is treated as the **authored** side, request
  `U`, then `s_map = min(U, W/b, H/b)`; the overlay placement is
  `dst = (0, 0, b*s_map, b*s_map)`, `authored = (0, 0, b, b)`, top-left
  anchored. `RouteMapView::Draw` and `RouteMap_HandleClick` must share this one
  placement and authored rect (no `ToAuthored` inverse cancellation). The
  map's existing internal zoom is independent of `F`. The route map is drawn
  after `DrawGameFrame`.
- Keep the existing z-order: world, HUD strip/panels, overlays, flash inside
  `DrawGameFrame`, then route map. The flash gets full-window coverage only.

## 3. Integration map

| Area | Site | Change |
|---|---|---|
| Placement algebra | [placement.hpp](../src/util/placement.hpp) | Add `requested_scale` to `Placement`, `PlaceContained`, `PlaceCenteredIn` and the `window` rule; persist it through `Reflow` for contained/window/centered and for **ancestor** placements used by nested modals. |
| Platform | [sdl_platform.hpp](../src/sdl_platform.hpp) / [sdl_platform.cpp](../src/sdl_platform.cpp) | Apply/refresh unchanged; expose active presentation scales; keep window-point getters. Rebuild the scaled scene placement on resize (the explicit-rect rule does not reflow). |
| Frame defaults | `NovaMainLoop_UpdateFrame`, `NovaRender_RedrawAndPresentFrame` ([nova_app.cpp](../src/nova_app.cpp)) | Request `U` for the 1024x768 default and menu placement. |
| Fixed screens | [landed_window.cpp](../src/game/landed_window.cpp), [starmap.cpp](../src/game/starmap.cpp), [preferences.cpp](../src/game/preferences.cpp), [docked_dialog.cpp](../src/game/docked_dialog.cpp), [docked_mission_dialog.cpp](../src/game/docked_mission_dialog.cpp), [ui_dialog.cpp](../src/game/ui_dialog.cpp) | Pass `U` as requested scale; `PlaceCenteredIn(background)` keeps the **background anchor center** distinct from window center. |
| Mission dialogs | `NovaMission_RunOfferWindow`, `NovaMission_RunMissionInfoWindow` ([docked_mission_dialog.cpp](../src/game/docked_mission_dialog.cpp)) | Request `U * M`; authored DITL/list pitch/text sizes unchanged. |
| HUD strip | `HudRenderer::Draw` strip/panel scope, `HudPanel_AnchorTopRight` ([hud_renderer.cpp](../src/game/hud_renderer.cpp), [gameplay_interface.cpp](../src/game/gameplay_interface.cpp)) | Right-anchored strip at `W - 194*s_hud` from loaded art/layout bounds; scene reserve `194 * s_hud`. |
| HUD overlays | `DrawEscortCommandsPanel`, overlay message ([hud_renderer.cpp](../src/game/hud_renderer.cpp)) | Separate full-window UI placement; scale by `U`; top-left/bottom anchors from placement extent. |
| Flight world/camera | `DrawGameFrame`, `CurrentViewport`, `SyncGameplayViewport` ([spaceflight_view.cpp](../src/game/spaceflight_view.cpp)) | Full-window scene placement scale `F`; authored `vp`; recompute on resize. |
| Flight input | `PlayerTick_MouseTargetAndControlCommands`, `PollFlightInput` ([spaceflight.cpp](../src/game/spaceflight.cpp), [sdl_platform.cpp](../src/sdl_platform.cpp)) | Raw window-point snapshot; per-consumer mapping; re-sync viewport after event drain before consumers. |
| Route map | `RouteMap_HandleClick`, `RouteMap_OverlayRect`, `RouteMapView::Draw` ([route_map.cpp](../src/game/route_map.cpp)) | Shared top-left overlay placement with `authored = (0,0,b,b)`, `b = max(200, round(W*0.25))`, `s_map = min(U, W/b, H/b)`; one rect for draw and hit-test. |
| Prefs | `game::NovaExtraPrefs` ([extended_prefs.hpp](../src/game/extended_prefs.hpp), [extended_prefs.cpp](../src/game/extended_prefs.cpp)), [nova_app.cpp](../src/nova_app.cpp) | Add/parse/save three keys; preserve on install-root save. |

## 4. Work packages (dependency order)

**WP0 — Placement requested scale (S).** Add `requested_scale`; honour it in
`PlaceContained`, `PlaceCenteredIn` and the window rule; persist through
`Reflow` for contained, window and centered placements, including the ancestor
placement chain a nested modal relies on. Tests assert **geometry/behavior is
unchanged** at `r = 1` (not byte-for-byte equality before snapping).

**WP1 — Preferences and settings plumbing (S/M).** Add `ui_scale`,
`flight_scene_scale`, `mission_scale`; parse and emit them; keep all fields on
the truncating install-root save; strict finite-positive validation. Default,
absent or invalid values fall back to `1.0`. Thread the values into placement
construction (a small `PresentationScale` value is preferred over hidden
globals).

**WP2 — General UI scale `U` (M).** Pass `U` at the fixed-screen/menu/dialog
sites; verify defaults.

**WP3 — Flight/HUD geometry and input (M).** Integrate the coupled HUD and
scene transforms together: right-anchored HUD
placement and scene reserve, full-window scene placement scale `F`, authored
`vp`/camera, resize recomputation and frame ordering, separated HUD overlay
scopes, route-map draw/input, flash full-window coverage, and the raw
window-point input snapshot with per-consumer mapping.

**WP4 — Mission `M` (S).** Request `U * M` at the two entrypoints only; leave
BBS/comms at `U`. Update the affected functions' tracker comments in the same
change (comment only, no percentage inflation: the original had no scaling).

**WP5 — Validation and docs.** Extend the tests in §6.

Affected cited original functions across **all** packages get tracker comment
updates in the same change. Port-only placement helpers and extra preferences
do not acquire invented original addresses or tracker rows. Scaling itself is
a supported port presentation choice, so it is not
logged as `NovaLog::Todo`; genuinely unported reconstruction remains `Todo` per
AGENTS.

## 5. Settings and data flow

First supported control: **edit `EV Nova Extra Prefs.ini` and restart** — no
runtime activation, no Preferences-screen work required. The parser already
ignores section headers and unknown keys, so a `[display]` section is safe:

```
[display]
ui_scale=1.0
flight_scene_scale=1.0
mission_scale=1.0
```

Loading reuses the existing startup extra-prefs load; saving must emit these
keys alongside `install_root`. Validation is strict: parse the complete value
(reject trailing junk) and accept only finite floats in the selected proposal
range `[0.5, 4.0]` per factor; `nan`, `inf`, `0`, negatives and out-of-range
values fall back to `1.0` with a warning naming the offending value. The
product `U * M` may reach `16`, then the single fit clamp applies; this bounds
tiny-`F` overflow and huge-`U` raster cost. The range is a selected
proposal, not a user-agreed fact. A later optional package may add
Preferences-screen widgets; the file-edit path must keep working.

These are presentation settings, not original bug fixes, so they are **not**
gated through `kApplyOriginalBugFixes`
([compatibility.hpp](../src/game/compatibility.hpp)); that switch is reserved
for confirmed original bugs. No speculative compatibility gate is needed for
ordinary presentation settings.

## 6. Validation and acceptance criteria

This documentation-only edit requires no build. For the future code work,
follow AGENTS: build release and debug, run targeted `ctest`, format changed
C++ with `clang-format`, run clang-tidy on affected translation units, and run
`tools/ref_audit.py` and inspect `analysis/ref_audit.txt`. Ordinary gameplay
launches do not require approval; only the external probe does. Probe geometry
is published by `SdlPlatform::PumpProbe` / `SetGeometry`, not by `Present`.
Restore a neutral full-window flight placement at the frame boundary and
publish UI rects via `ToWindowRect`; keep `SdlPlatform::Present` for
screenshots/control. Do not expand the probe protocol.

- **Defaults regression:** `U = F = M = 1` reproduces current geometry and
  behavior for every screen; existing `tests/placement_test.cpp` passes
  unmodified; `logical_playfield_size` still returns window points; the scene
  occupies the full window with no HUD band at 1x.
- **Transforms:** fractional `r > 1` and `r < 1` round-trips, `ToWindowRect`,
  `text_raster_scale` composition with density.
- **HiDPI:** e.g. a 4K panel presenting a 1920x1080-point window; assert
  `scale * density` viewport/raster behavior and that points, not physical
  pixels, bound layout.
- **Combinations:** `U * M` clamps once; `F` does not affect UI scale.
- **Fit / resize / nesting:** requested scale survives shrink-to-fit and grow
  back for contained, window and centered placements; nested modal reflow keeps
  both ancestor and child requested scales; the scaled scene placement is
  rebuilt on resize.
- **HUD/overlays:** strip origin stays `W - 194*s_hud` with the 194 reserve;
  fit cap avoids vertical clipping at the 1024x768 minimum; escort panel and
  overlay message are not moved or clipped by the strip placement; route map
  uses its own top-left fit/anchor rule with matching draw and hit rect.
- **Flight:** at `U = F = M = 1` camera, viewport, asteroid scatter and
  mission spawn match today; at `F > 1` ships/world are larger, less world is
  visible, UI is unchanged, world clicks pick the correct ship/stellar, route
  map clicks hit markers; the flash covers the window.
- **Prefs:** round-trip, missing/invalid/`nan`/`inf`/zero/negative values,
  install-root save preserving the scale fields.

## 7. Scope exclusions / non-goals

- No responsive reflow, text re-wrapping for font-only scaling, art
  rearrangement, or per-widget metric edits. Authored constants stay authored.
- No BBS/comms `M`; `M` is only the two mission entrypoints listed above.
- No physical-pixel sizing; window points only.
- No change to `logical_playfield_size` semantics, no rescaling of simulation
  units, and no behavior claims beyond camera/spawn extent.
- No CE parity requirement; the CE is not an implementation contract.
- No new scaling framework: the placement algebra plus three explicit factors.

Preferences-screen widgets can follow separately; they do not block this task.
