# Display and UI scaling (implementation design)

Status: **implemented; Stages 0-5 are done (doc resolution,
`requested_scale`, prefs + `PresentationScale`, `U` at the authored UI sites,
HUD chrome/overlay scaling, the mission `M` multiplier at the two mission
entrypoints, and the flight-scene `F` world/input/route-map work). Stage 6
(preferences widgets / optional dedicated `hud_scale`) is optional and not
done.** The port scales authored UI and the in-flight HUD chrome by `U`, the
two mission dialogs by `U * M`, and the free-flight world by `F`. This
document records the coordinate ownership and equations, the integration
sites, and the work sequencing. Fit caps, numeric limits and the settings UI
described below are selected implementation proposals, not previously agreed
facts.

Resolved during Stage 0:

- **Ancestor scale.** A centred modal stores the background's request as
  `Placement::anchor_requested_scale`, so `Reflow` rebuilds the whole ancestor
  chain at its own request (one `requested_scale` field was insufficient).
- **Centred-modal equation.** The anchored case fits against the background's
  `containing_size` and centres on the background `dst` centre (see §2), not
  the plain window-centred `dst` formula.
- **Scene placement is `Rule::window`.** `PlaceWindow(window, F)` gives
  `dst = (0,0,W,H)` with `authored_size = window/F`, so `Reflow` rebuilds it on
  resize; it is not an `explicit_rect` placement.
- **Declared divergences.** The HUD fit cap and the route map's live-width
  responsiveness are deliberate; see the notes in §2.
- **Settings plumbing (Stage 2).** `ui_scale` / `flight_scene_scale` /
  `mission_scale` are parsed from `EV Nova Extra Prefs.ini`, validated to a
  finite `[0.5, 4.0]` with a `1.0` fallback, and re-emitted on every save.
  `PresentationScale` is resolved once onto `SdlPlatform` (which owns the
  placement builders); `EVN_UI_SCALE` / `EVN_FLIGHT_SCENE_SCALE` /
  `EVN_MISSION_SCALE` override it for debug iteration.
- **General UI scale (Stage 3).** Every contained/centered authored UI
  composition requests `platform.ui_scale()`: splash/progress, main menu,
  landed/spaceport, starmap, preferences/key settings, comms, player info,
  docked store/trade/bar/mission dialogs, boarding/capture, negotiation,
  selection-text and locate-data. Full-window `PlaceWindow(...)` backgrounds
  and scrims stay neutral (scale 1) because they are not authored
  compositions, and the intro cinematic is deliberately excluded (timed media,
  not interactive UI).
- **HUD chrome scaling (Stage 3b).** `HudRenderer::Draw` pushes a full-window
  placement at `hud_scale = min(U, window/art_w, window/art_h)`, drawing the
  cockpit strip, radar and anchored `.ntf` panels in authored
  `window/hud_scale` coordinates. `CurrentViewport` reserves
  `194 * hud_scale` (its own copy of the same stock-art cap; a one-frame lag on
  resize). The escort panel and overlay message are extracted to
  `DrawOverlayAndEscort`, which nests its own full-window placement at `U`, so
  the strip fit cap never shrinks overlay text. The flight-scene scale `F`, the
  route map, the flash scope and the flight-input refactor are Stage 5 (below).
- **Mission `M` (Stage 4).** `PresentationScale::mission_dialog()` composes
  `U * M` before the single fit clamp; `NovaMission_RunOfferWindow` (DLOG
  `0x3f8`/`0x3fc`) and `NovaMission_RunMissionInfoWindow` (`0x3f4`) request it
  at both their initial and per-frame placements, and the shared
  `NovaUi_RunTextReaderDialog` takes a `mission_dialog` flag so the mission
  desc readers (Brief/QuickBrief/LoadCarg/DumpCargo/Comp/Fail/ShipDone and
  the mission-cargo denials) request `U * M` while non-mission readers stay at
  `U`. The mission BBS (`0x3ee`), comms (`0x3ef`/`0x3f1`) and every other
  authored UI site stay at `U`, and their full-window background/scrim
  `PlaceWindow` stays neutral.
- **Flight scene `F` (Stage 5, WP3).** `FlightSceneGeometryFor` builds the
  full-window scene placement at `F` (`PlaceWindow(window, F)`) and the authored
  gameplay viewport (window minus the `194 * hud_scale` strip reserve, divided
  by `F`); `CurrentViewport` and `SyncGameplayViewport` derive from it, and
  `DrawGameFrame` installs its placement. Flight input now consumes the raw
  `FlightInput::window_mouse_x/y` snapshot and maps it per consumer: the scene
  picks through `FlightSceneGeometryFor(platform).placement.ToAuthored`, the
  route map through its own shared overlay placement. The hyperspace flash is
  drawn under a neutral full-window placement so it covers HUD and UI
  regardless of `F`.

Related: [dlog_ditl_dialog_format.md](dlog_ditl_dialog_format.md) (authored
DLOG/DITL geometry), [preferences_keybindings.md](preferences_keybindings.md)
(`EV Nova Prefs.prf` and the sibling `EV Nova Extra Prefs.ini`),
[probe_harness.md](probe_harness.md) (probe geometry contract).

## 1. Current state (verified)

> This section records the pre-implementation baseline that motivated the
> design. Stages 1-5 have since changed the scale-sensitive sites (the top
> status and §4 carry the current state).

- [placement.hpp](../src/util/placement.hpp) owns `dst` (window points),
  `scale`, `requested_scale` (the caller's request before the fit clamp),
  authored/containing sizes and the reflow rule; `ToWindow` / `ToAuthored` /
  `ToWindowRect` are the forward/inverse mappings. `PlaceContained` fits at
  `min(requested_scale, W/Aw, H/Ah)`; `PlaceCenteredIn` centres a modal on its
  background; `Reflow` rebuilds a placement after a resize and preserves each
  placement's request (`anchor_requested_scale` for the ancestor chain).
- [sdl_platform.hpp](../src/sdl_platform.hpp) /
  [sdl_platform.cpp](../src/sdl_platform.cpp) own the active placement and a
  push/pop stack, apply `placement.scale * WindowPixelDensity()` to the render
  scale, set the SDL viewport from `ToRenderViewport()`, map mouse input
  through `ToAuthored`, and expose `logical_playfield_size()` (**window points,
  unchanged semantics**), `playfield_window_rect()` (`placement.dst`) and
  `text_raster_scale()` (`placement.scale * density`). `mouse_position()` and
  `mouse_window_point()` are existing getters. It also carries the resolved
  `PresentationScale` (`ui_scale()` / `flight_scene_scale()` /
  `mission_scale()`), which the placement builders read.
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
  [extended_prefs.cpp](../src/game/extended_prefs.cpp)) stores `[paths]
  install_root` and a `[display]` section with `ui_scale`,
  `flight_scene_scale` and `mission_scale`. The parser accepts keys flatly
  (section headers and unknown keys are ignored) and emits every field on
  save, so the truncating install-root save no longer erases them. Values are
  validated by `PresentationScale_Parse` (finite, `[0.5, 4.0]`) and fall back
  to `1.0`; `NovaExtraPrefs_ResolvePresentationScale` applies the debug
  `EVN_UI_SCALE` / `EVN_FLIGHT_SCENE_SCALE` / `EVN_MISSION_SCALE` overrides.
  The resolved `PresentationScale` lives on `NovaRuntime`.

## 2. Target architecture

Three independent multipliers, all defaulting to `1.0`:

- **`U` — general UI scale.** Uniformly scales authored UI compositions:
  menus, splash/HUD chrome, fixed screens, dialogs, in-flight UI overlays.
  Centred compositions stay centred; the HUD stays right-anchored. Authored
  grid/list/font/HUD constants are unchanged; the transform scales them.
- **`F` — flight-scene scale.** Scales world rendering only (ships, sprites,
  starfield, effects, asteroids); less world is visible. HUD and UI are
  unaffected. Simulation units are not rescaled.
- **`M` — mission-dialog multiplier**, applied **on top of `U`**. The two
  mission entrypoints `NovaMission_RunOfferWindow` (DLOG `0x3f8`, variant
  `0x3fc`) and `NovaMission_RunMissionInfoWindow` (`0x3f4`) request `U * M`,
  as does every mission desc text reader: the acceptance Brief/LoadCarg
  readers, the QuickBrief shown in the mission-info window, the
  success/failure/load/dump/ShipDone debriefs and the mission-cargo denial
  texts (all through `NovaUi_RunTextReaderDialog(..., mission_dialog=true)`).
  The mission BBS (`0x3ee`), ship/planet comms (`0x3ef`/`0x3f1`) and every
  non-mission text reader (about, intro, store/bar notices, escort payroll)
  request only `U`.

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

**Anchored modal** (centred on a background placement `B`, not the window):

```
available = B.containing_size            full window, or fallback B.dst size
fit       = min(available.x/Aw, available.y/Ah)
scale_m   = min(r, fit)                  single clamp, same as contained
center    = B.dst centre
x         = clamp(center.x - Aw*scale_m/2, 0, max(0, available.x - Aw*scale_m))
y         = clamp(center.y - Ah*scale_m/2, 0, max(0, available.y - Ah*scale_m))
dst       = (x, y, Aw*scale_m, Ah*scale_m)
```

The modal keeps the **background anchor centre** distinct from the window
centre; the background is rebuilt first by `Reflow` at
`anchor_requested_scale`, then the modal re-centres. This is the
`PlaceCenteredIn(const Placement &, ...)` overload, distinct from the plain
window-centred `PlaceCenteredIn(SDL_FRect, ...)` used by fixed screens.

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
to the stock `194x767`. The
native 194 anchor and `CurrentViewport` reserve stay the gameplay contract.
Custom art larger than the stock 194x767 is not promised to fit.

DIVERGENCE(original): the fit clamp itself has no original counterpart.
The original strip width is `DAT_0088c020 = 194 * ui_scale` with no window
cap, so at large `U` the port's reserve differs (e.g. `U = 4` at 1024px:
original play width 248 vs port 830). The cap is a selected robustness choice;
mark the port site `divergence`. The final fallback when both art and layout
are `0`-sized is the stock `194x767`.

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
- Required: snapshot the raw window-point float (add `window_mouse_x/y` to
  `FlightInput`, captured beside the existing values from
  `mouse_window_point()`) and map per consumer. The frame order makes the
  "same placement that produced the snapshot" fallback unworkable: input is
  polled before `DrawGameFrame` installs the scene placement, and
  `RefreshPlacementAfterResize` runs inside the drain, so the active placement
  at poll time is not the one any target was drawn with.
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
  after `DrawGameFrame`. Note this rule is intentionally **responsive**: the
  original derives the side from the live render-owner width
  (`DAT_00575a80 = 0.25`), so at `U = 1` it reproduces today's
  window-proportional square, and `U > 1` only enlarges within the
  `W/b` / `H/b` cap. That is a divergence from the otherwise-authored model,
  but it matches the original; mark the port site `divergence`.
- Keep the existing z-order: world, HUD strip/panels, overlays, flash inside
  `DrawGameFrame`, then route map. The flash gets full-window coverage only.

## 3. Integration map

| Area | Site | Change |
|---|---|---|
| Placement algebra | [placement.hpp](../src/util/placement.hpp) | **Stage 1 done:** `requested_scale` on `Placement`, `PlaceContained`, both `PlaceCenteredIn` overloads and the `window` rule; `anchor_requested_scale` persists the ancestor request through `Reflow` for contained/window/centered chains. Default `1.0` is neutral. |
| Platform | [sdl_platform.hpp](../src/sdl_platform.hpp) / [sdl_platform.cpp](../src/sdl_platform.cpp) | Apply/refresh unchanged; expose active presentation scales; keep window-point getters. The scene uses `Rule::window` (`PlaceWindow(window, F)`), so `Reflow` rebuilds it on resize. |
| Frame defaults | `NovaMainLoop_UpdateFrame`, `NovaRender_RedrawAndPresentFrame` ([nova_app.cpp](../src/nova_app.cpp)) | Request `U` for the 1024x768 default and menu placement. |
| Fixed screens | all authored UI sites (e.g. [landed_window.cpp](../src/game/landed_window.cpp), [starmap.cpp](../src/game/starmap.cpp), [preferences.cpp](../src/game/preferences.cpp), [docked_dialog.cpp](../src/game/docked_dialog.cpp), [docked_mission_dialog.cpp](../src/game/docked_mission_dialog.cpp), [docked_store_dialog.cpp](../src/game/docked_store_dialog.cpp), [docked_trade_dialog.cpp](../src/game/docked_trade_dialog.cpp), [docked_bar_dialog.cpp](../src/game/docked_bar_dialog.cpp), [ship_comm_dialog.cpp](../src/game/ship_comm_dialog.cpp), [player_info_window.cpp](../src/game/player_info_window.cpp), [boarding_plunder.cpp](../src/game/boarding_plunder.cpp), [negotiation_dialog.cpp](../src/game/negotiation_dialog.cpp), [selection_text_dialog.cpp](../src/game/selection_text_dialog.cpp), [locate_data_dialog.cpp](../src/game/locate_data_dialog.cpp), [ui_dialog.cpp](../src/game/ui_dialog.cpp)) | **Stage 3 done:** every authored `PlaceContained`/`PlaceCenteredIn` passes `platform.ui_scale()`; `PlaceWindow` backgrounds stay neutral; the intro cinematic stays native; `PlaceCenteredIn(background)` keeps the **background anchor center** distinct from window center. |
| Mission dialogs | `NovaMission_RunOfferWindow`, `NovaMission_RunMissionInfoWindow`, `NovaUi_RunTextReaderDialog(mission_dialog=true)` ([docked_mission_dialog.cpp](../src/game/docked_mission_dialog.cpp), [selection_text_dialog.cpp](../src/game/selection_text_dialog.cpp)) | **Stage 4 done:** the two DLOGs and every mission desc text reader request `platform.mission_dialog_scale()` (= `U * M`, composed in `PresentationScale`); authored DITL/list pitch/text sizes unchanged; BBS/comms and non-mission readers stay `U`. |
| HUD strip | `HudRenderer::Draw` strip/panel scope, `HudPanel_AnchorTopRight` ([hud_renderer.cpp](../src/game/hud_renderer.cpp), [gameplay_interface.cpp](../src/game/gameplay_interface.cpp)) | **Stage 3b done:** full-window placement at `min(U, window/art)`; strip at authored `window/hud_scale - 194`; `CurrentViewport` reserve `194 * hud_scale`. Independent of `F`. |
| HUD overlays | `DrawOverlayAndEscort` ([hud_renderer.cpp](../src/game/hud_renderer.cpp)) | **Stage 3b done:** separate full-window placement at `U`; escort panel and overlay message scale independently of the strip fit cap. |
| Flight world/camera | `DrawGameFrame`, `CurrentViewport`, `FlightSceneGeometryFor`, `SyncGameplayViewport` ([spaceflight_view.cpp](../src/game/spaceflight_view.cpp), [spaceflight_view.hpp](../src/game/spaceflight_view.hpp)) | **Stage 5 done:** one `FlightSceneGeometryFor` builds the full-window `PlaceWindow(window, F)` placement and the authored viewport `(window - reserve)/F`; draw and picking share it. |
| Flight input | `PlayerTick_MouseTargetAndControlCommands`, `PollFlightInput` ([spaceflight.cpp](../src/game/spaceflight.cpp), [sdl_platform.cpp](../src/sdl_platform.cpp), [sdl_platform.hpp](../src/sdl_platform.hpp)) | **Stage 5 done:** `FlightInput::window_mouse_x/y` raw snapshot; scene picks map through the scene placement, route-map clicks through the overlay placement; viewport re-synced after the event drain. |
| Route map | `RouteMap_HandleClick`, `RouteMap_OverlayRect`, `RouteMap_OverlayPlacement`, `RouteMapView::Draw` ([route_map.cpp](../src/game/route_map.cpp), [route_map.hpp](../src/game/route_map.hpp)) | **Stage 5 done:** one shared top-left overlay placement (`authored = (0,0,b,b)`, `b = max(200, round(W*0.25))`, `s_map = min(U, W/b, H/b)`) consumed by both draw and hit-test; clicks arrive as raw window points and map in. |
| Prefs | `game::NovaExtraPrefs` ([extended_prefs.hpp](../src/game/extended_prefs.hpp), [extended_prefs.cpp](../src/game/extended_prefs.cpp)), [nova_app.cpp](../src/nova_app.cpp) | Add/parse/save three keys; preserve on install-root save. |

## 4. Work packages (dependency order)

**WP0 — Placement requested scale (S). DONE (Stage 1).** `requested_scale`
composes with the fit in `PlaceContained`, both `PlaceCenteredIn` overloads
and the window rule; `anchor_requested_scale` persists the ancestor request
through `Reflow`. Geometry is unchanged at `r = 1` (existing placement tests
pass unmodified), and new cases cover the fit composition, the window-rule
authored extent, contained-reflow persistence, and nested-modal ancestor +
child requests.

**WP1 — Preferences and settings plumbing (S/M). DONE (Stage 2).**
`ui_scale`, `flight_scene_scale` and `mission_scale` parse from the `[display]`
section through `PresentationScale_Parse` (complete, finite, `[0.5, 4.0]`;
malformed falls back to `1.0` with a warning), are emitted on every save, and
survive the install-root save. `NovaExtraPrefs_ResolvePresentationScale`
applies the debug env overrides. The resolved `PresentationScale` is set on
`SdlPlatform`, which owns the placement builders. Call sites read
`platform.ui_scale()` (wired in WP2); `F`/`M` stay `1.0` until WP3+.

**WP2 — General UI scale `U` (M). DONE (Stage 3).** Every
authored contained/centered UI composition passes `platform.ui_scale()`:
splash/progress, main menu, landed/spaceport, starmap, preferences/key
settings, comms, player info, docked store/trade/bar, mission dialogs (BBS and
comms at `U`, the two offer/info entrypoints now at `U * M` via WP4),
boarding/capture, negotiation, selection-text and locate-data. Full-window
`PlaceWindow` backgrounds stay neutral, and the intro cinematic is deliberately
excluded (timed media, not interactive UI). At `U = 1` geometry is unchanged
(full suite passes unmodified).

**WP3 — Flight/HUD geometry and input (M). DONE (Stages 3b + 5).**
`HudRenderer::Draw` scales the strip/radar/panels with the art-fit cap;
`CurrentViewport` reserves `194 * hud_scale`; `DrawOverlayAndEscort` nests a
`U` placement for the escort panel and overlay message. Stage 5 adds the
full-window scene placement at `F`, the authored `vp`/camera and
`SyncGameplayViewport`, the raw window-point input snapshot with per-consumer
mapping, the shared route-map draw/hit placement, and the full-window flash
scope.

**WP4 — Mission `M` (S). DONE (Stage 4).** `PresentationScale::mission_dialog()`
composes `U * M`. The two DLOG entrypoints request it at every placement they
build (`NovaMission_RunOfferWindow` initial + draw frame, and
`NovaMission_RunMissionInfoWindow` initial + draw frame), and
`NovaUi_RunTextReaderDialog` takes a `mission_dialog` flag so the mission desc
readers (acceptance Brief/LoadCarg, QuickBrief via the info window, the
debriefs and the cargo denials) use it too; BBS/comms and non-mission readers
stay `U`. At `M = 1` the request is exactly `U`, so geometry is unchanged
(full suite passes, plus a composition unit test). Scaling is
presentation-only, so the cited tracker rows keep their existing pct/tags (no
new original work is implemented).

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

Loading reuses the existing startup extra-prefs load; saving emits these keys
alongside `install_root`. Validation is strict: parse the complete value
(reject trailing junk) and accept only finite floats in the selected proposal
range `[0.5, 4.0]` per factor; `nan`, `inf`, `0`, negatives and out-of-range
values fall back to `1.0` with a warning naming the offending value. The
load/validate/save path is implemented (Stage 2); the values are resolved onto
the runtime and applied at the placement sites (WP2+). For debug iteration
`EVN_UI_SCALE`, `EVN_FLIGHT_SCENE_SCALE` and `EVN_MISSION_SCALE` override the
file at startup. The
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

Stages 0-5 are implemented and validated. Per AGENTS, changes were built in
release and debug, exercised with targeted and full `ctest`, formatted with
`clang-format`, checked with clang-tidy on affected translation units, and
verified with `tools/ref_audit.py` against `analysis/ref_audit.txt`. Ordinary
gameplay
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
- No BBS/comms `M`; `M` covers the two mission DLOG entrypoints and the
  mission desc text readers listed above.
- No physical-pixel sizing; window points only.
- No change to `logical_playfield_size` semantics, no rescaling of simulation
  units, and no behavior claims beyond camera/spawn extent.
- No CE parity requirement; the CE is not an implementation contract.
- No new scaling framework: the placement algebra plus three explicit factors.

Preferences-screen widgets can follow separately; they do not block this task.
