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
  current / selected systems.
- Colour coding: current system = amber; explored = light blue; unexplored =
  dim blue (hidden label). Markers are additionally tinted toward their owning
  government's theme colour (`Government.theme_red/green/blue`, the political-map
  affiliation drawn by `NovaUi_DrawStarmapPoliticalOverlay`), so explored
  systems read as their faction's territory at a glance.
- Pan (arrow keys) and zoom (`+`/`-`); `h` re-fits the view; mouse click or
  Tab selects/cycles a system.
- Reachable-jump accent: the links fanning out of the *selected* system are
  redrawn in a bright green so the player sees every single-jump route
  candidate from the highlighted node (mirrors the original's selected-stellar
  route emphasis).
- Inspector footer shows the selected system's name, explored/current state,
  owning government, outward jump count, and a `[plotted target]` flag when it
  is the active starmap-plotted destination.
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

- No DITL resource allocation / frame PICT; the map is a direct SDL3 render.
- No political/government **overlay grid** (`NovaUi_DrawStarmapPoliticalOverlay`
  per-cell strength tint); the per-system marker government-colouring above is a
  lightweight stand-in.
- No plotted-route editing or multi-hop route → travel-target sync (only the
  immediate first hop is resolved to a travel slot).
- No mission-highlight icons / mission jump planning.
- No starmap search dialog (`0x004aab30`).
- No licence-seed easter-egg branch.
- Pan/zoom are keyboard-driven; no drag-to-pan / wheel zoom yet.
- The map only inspects/selects; the actual jump stays with `NovaTravel_Tick`.
