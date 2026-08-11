# Starmap / Galaxy Map (in-flight navigation) — reverse-engineering note

## Purpose

The starmap is the in-game galaxy navigation window. In EV Nova the player
opens it during flight to inspect the explored galaxy, identify reachable
systems and plan the next hyperspace jump. It is the primary non-HUD binding
between the world map (`syst` records) and the jump/travel system.

## Ghidra touchpoints

The starmap window lives in `NovaUi_*` (0x004a...):

- `0x004a3aa0` `NovaUi_RunStarmapWindow` — modal entry: creates the window from
  dialog resource 2000 (0x7d0), runs the interaction loop, restores gameplay
  UI on exit.
- `0x004a4353` `NovaUi_StarmapWindowInnerLoop` — action dispatch / click
  hit-testing / route editing / overlay toggles / exit cleanup. (Ghidra split
  this out of the run loop.)
- `0x004a4fd0` `NovaUi_CloseStarmapWindow` — teardown + presentation restore.
- `0x004a51f0` `NovaUi_RedrawStarmapWindow` — composites background, routes,
  system markers, mission highlights, political overlay, status text.
- `0x004a7470` `NovaUi_RedrawStarmapAfterPan` — pan-driven redraw.
- `0x004a7e80` `NovaUi_NormalizeStarmapRoutePlan` / `0x004a8080`
  `NovaUi_SyncTravelSelectionFromStarmapRoute` — the plotted route → active
  travel target handoff.
- `0x004a8100` `NovaUi_DrawStarmapRoutesAndMarkers` — link lines, markers,
  selected-stellar arrows, mission icons, zoom-dependent labels.
- `0x004a9d10` `NovaUi_EnableStarmapPoliticalOverlay` / `0x004aa620`
  `NovaUi_DrawStarmapPoliticalOverlay` — per-system government tint overlay.
- `0x004aab30` `NovaUi_RunStarmapSearchDialog` — name-prefix search.

Opened from:

- `0x0044b120` `Ship_HandlePlayerShip` — while an in-flight map command is
  active and the ship is not landing / hyperspacing / dead.
- `0x0043c470` `NovaUi_RunTravelDestinationMainWindow` — destination window's
  starmap sub-flow.
- `0x00456480` `Stellar_SelectLinkedDestinationViaStarmap` — linked-destination
  (hypergate/wormhole) travel through the map.

## Data model

The map is driven by the scenario `syst` table (`ScenarioData.systems`, the
`System` structs). Each `System` provides:

- `name` — map label.
- `pos_x` / `pos_y` — the node position on the galaxy map.
- `links[16]` — `Con1-16` adjacency: each is a system *resource* id (>= 0x80)
  the player can jump to. The map draws a link line for each.
- `nav_defs[16]` — `NavDef1-16` travel-stellar ids (departure-point stellars).
  NOT paired 1:1 with every `links` entry in the scenario data; a jump's target
  is a `System.links` hyperlink, resolved purely against `links` (travel.cpp).

Visibility / exploration (fog of war):

- `System.is_visible` / `System.has_explored_flag` (mirror `SystemDef`) gate
  which systems the player may target/draw as known.
- `GameState.control.explored_systems` — persistent bitset (0x800) of explored
  zero-based system ids; `landed_store.cpp` reads it for availability
  expressions.

The original floods discovery on system entry (`System_RebuildSystemVisibilityMap`
`0x00467970`, `System_FloodDiscoverAdjacentSystems` `0x00467ab0`,
`System_ResolveSystemDiscoverySlot` `0x0046b9b0`).

## Clean-room implementation (`src/game/starmap.cpp`)

`NovaStarmap_RunWindow(SdlPlatform&, GameState&)` is a modal loop modelled on
the negotiation/landed dialogs (SDL3, logical 640x480 centred playfield):

- Draws the galaxy graph: `System.links` as lines (deduplicated by drawing from
  the lower index), system nodes as filled circles, and labels for explored /
  current / selected systems. Only *discovered* systems are drawn at all: each
  marker is gated on the explored/visible flag (matching the original's
  `is_visible && has_explored_flag` marker gate in `NovaUi_DrawStarmapRoutesAndMarkers`),
  a link line draws only when BOTH its endpoints are explored, and undiscovered
  systems have no marker, no label, cannot be clicked and are excluded from the
  Tab/Backslash cycle. Undiscovered far-flung systems therefore don't stagger the
  view either.
- Markers are true solid discs (filled triangle-fan via the geometry path), so the
  node sits exactly on its system position; link endpoints share the same panel
  origin as the markers (the raw world projection is offset by the panel origin),
  so link lines land precisely on the centre of each circle.
- Colour coding: current system = amber; explored = light blue. Markers are
  additionally tinted toward their owning government's theme colour
  (`Government.theme_red/green/blue`, the political-map
  affiliation drawn by `NovaUi_DrawStarmapPoliticalOverlay`), so explored
  systems read as their faction's territory at a glance.
- Zoom is stepped through a handful of fixed levels (a modest geometric series,
  x1.5 per level, over the whole-galaxy fit: level 0 = whole galaxy, higher =
  closer), matching the original's stepped zoom rather than a continuous slider.
  The top level is capped so the closest view stays a useful regional scale
  instead of blowing up to a handful of systems. The map opens on a *middle*
  level centred on the pilot's current system -- not pinned to the discovered
  cluster -- so the local neighbourhood (and a good span of the galaxy) is
  visible immediately; `+`/`-` step between levels and `h` snaps back to that
  opening view (re-centring on the current system). Zooming pivots about the
  current system's on-screen position (like the original's fixed pan reference),
  so the current system stays put rather than racing to a corner of the panel.
  Pan (arrow keys) scrolls the camera, content moving opposite the key direction
  (Right shows what lies to the right of the current view); mouse click or
  Tab/Backslash selects/cycles a system.
- Selection/jump accent: when the highlighted system is a directly-linked jump
  destination of the current system, a thick green line is drawn from the
  current system to it (plus a small green ring at that node), making the
  planned next hop obvious before committing.
- Tab / Backslash step the selection through the *destination ring* computed
  once per session: each system *directly linked* to the player's current
  system (the EV Nova manual: "press Tab or Backslash to cycle through all the
  systems that are linked to your current system"). The ring contains ONLY those
  destination systems -- never the current system itself (jumping to where you
  already are is meaningless). Shift+Tab / Shift+Backslash step backwards.
  Anchored to the player's current system, so a single-link system offers just
  that one destination rather than chain-walking outward; when the current
  selection isn't in the ring (e.g. the current system) the first forward step
  lands on the first destination and the first backward step on the last. The
  committed plotted jump to the highlighted system is shown as the thick green
  current-path line + small green target marker.
- Committed-jump accent (faithful to the original): the *active* plotted jump
  (travel_transfer_mode == 3) is drawn as a single thick green line from the
  current system to its directly-linked destination, with a small green link
  marker at that destination. This deliberately does NOT fan green over every
  reachable link from the selected node (the original only highlights the jump
  the player has committed to). Distinct from the *selection* accent above, which
  previews the currently highlighted destination on every redraw.
- Rendering fidelity: link lines render as thick quads (2px at the current
  system, brighter for current-adjacent links), marker nodes scale up with
  zoom (mirroring the original's zoom-dependant marker insets), and explored-
  system name labels appear only once the map is zoomed in past a threshold
  (mirroring the original's zoom-gated labels, Dat_00575a10) while the
  current / selected system names stay always visible.
- The two info panes the DITL defines around the graph are rendered: the
  right-hand detail column (DITL item 5 / entry 6) shows the selected system's
  name, explored/current state, owning government, outward jump count and a
  `- plotted target` flag when it is the active starmap-plotted destination
  (long names truncated to the narrow column); the bottom status bar (DITL
  item 1 / entry 2) shows a `Selected System:` line plus the keyboard hints.
- The bottom button row (DITL items 8/7/9/3/4/0) uses the house three-state
  PICT button art (ServicesButtonArt strips 0x1d4c..0x1d54, the same the
  ship-comm / negotiation / landed windows use) with STR# 0x96 labels, and is
  clickable matching the original's action dispatch:
  Show/Hide Borders toggles the political tint, Clear Route clears the plotted
  destination, Find starts an inline name-prefix search (typed characters
  select the first explored system matching, Esc cancels, Enter commits),
  '-'/'+' step the zoom (dimmed at the limits, mirroring DAT_007dc742/743) and
  Done closes the map.
- Esc / Enter / q / x close the map and return to flight.

Wired from `spaceflight.cpp`: the `FlightInput.starmap` ('m', edge-latched in
the loop) opens the modal. On return the loop calls
`NovaTravel_PlotStarmapDestination`, which resolves the map's highlighted
destination to a current-system travel slot + stellar (when directly linked),
arms the travel state and marks the selection manual; the HUD travel-status
panel then shows "JUMP > <destination>" and `NovaTravel_Tick` engages that
jump on 'j' (falling back to the nearest travel point when no direct slot). On
completion `CompleteJump` clears the plot.

Jump discovery: `NovaTravel_MarkSystemDiscovered` (travel.cpp) marks a reached
system + its `links` neighbours explored/visible. Called on jump completion
(`CompleteJump`) and on new-game start (`new_pilot_flow.cpp`), so the map shows
the explored blobs grow as the player jumps.

## Divergences / deferred (TODO)

- The starmap renders the real dialog resources: the 601x513 window frame
  (DLOG 0x7d0) with the PICT 0x213d "Map" starfield backdrop blitted as the
  window base (the original's `DAT_007dc73c`), DITL 0x7d0 item 2 as the
  galaxy-graph viewport (`UiPanel_GetEntryInfo(window, 3)`), item 5 as the
  right-hand detail column (entry 6), item 1 as the bottom status bar (entry 2)
  and items 8/7/9/3/4/0 as the bottom button row (entries 9/8/10/4/5/1: Show
  Borders, Clear Route, Find, zoom out, zoom in, Done). Because the 513-tall
  window is taller than the fixed 640x480 field, the whole dialog is scaled
  down uniformly (x0.936) so every element -- including the button row -- is
  on screen; the graph is scissored to its viewport rect.
- The bottom buttons use the house three-state PICT button art with STR# 0x96
  labels and dispatch the original's actions (zoom in/out at the zoom limits,
  Done closes, Show/Hide Borders toggles the political overlay, Clear Route
  clears the plotted destination, Find starts an inline name-prefix search).
  Find is the inline stand-in for the original's modal search dialog
  (`0x004aab30`).
- The political/government **overlay** (`NovaUi_DrawStarmapPoliticalOverlay`
  per-cell strength tint) is implemented as smooth fading government discs
  behind the graph: one disc per discovered, travel-reachable system with a
  valid government, radius `round(22/zoom)+12` (or `round(11/zoom)+9` for
  `scan_mask` bit-1 governments) half-pixel cells, per-cell strength
  `(r^2-d^2)*fade*zoom` clamped [1,255], `theme_*0.5` at the centre fading to
  fully transparent at the disc rim (the galaxy backdrop shows through). It is
  built to the actual panel on every toggle/zoom/pan rebuild, so discs are
  never dropped or truncated at the panel edge at any resolution (a deliberate
  fix of the original's fixed-size grid + hard viewport clamp, which could cut
  edge discs). Deliberate rendering improvement over the original's flat 16px
  opaque overlay blocks: the same strength field is drawn per-pixel (a texture)
  so the discs read smooth at any window scale. The overlay is ON by default
  (the original persists a default-Off preference; the clean-room has no prefs
  store yet) and the Show/Hide Borders button toggles it; while active the
  markers drop to their neutral base.
- No plotted-route editing or multi-hop route → travel-target sync (only the
  immediate first hop is resolved to a travel slot).
- No mission-highlight icons / mission jump planning.
- No licence-seed easter-egg branch.
- Pan/zoom are keyboard-driven; no drag-to-pan / wheel zoom yet.
- The map only inspects/selects; the actual jump stays with `NovaTravel_Tick`.
