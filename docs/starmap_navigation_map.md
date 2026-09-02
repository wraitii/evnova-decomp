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

Visibility / exploration (fog of war) — CORRECTED model (verified against the
binary, see "System discovery" below):

- `SystemDef.has_explored_flag` (+0x1ed) is a load-time "syst resource exists"
  flag: the scenario loader sets it (and `is_visible`) to 1 for every decoded
  syst (`NovaData_LoadScenarioResourceTables` `0x004bd3c0`;
  `Ship_InitGameplayDataTables` `0x004b0c20` only clears them during startup,
  BEFORE the loader runs, so the loader is the effective runtime writer). It
  is not a fog bit.
- `SystemDef.is_visible` (+0x1eb) is re-derived from it by
  `NovaResources_EvaluateAvailability` `0x00448090` (has_explored && the
  system's visibility NCB expression at +0xec).
- Stellar `is_available` (+0x44) is runtime state recomputed per tick by
  `System_UpdateSystemAndStellarDisplayState` `0x00432470` scope 3: defined
  (`+0x45`, set by the loader) stellars owned by a visible system become
  available, and `availability_flags & 0x20` sets the hazard marker. Because
  `is_visible` is re-filtered per system through its Visibility NCB (see
  below), stellars of NCB-hidden systems stay unavailable.
- **Visibility twins**: the scenario repeats `syst` resources at identical
  map coordinates for story-driven government swaps (Koria's
  Federation/rebel pair, the whole Vell-os `b330` region, the Polaris `b88`
  swaps, ...). The loader's twin-grouping pass (`0x004bd3c0` / `0x004beb4f`)
  groups every decoded system sharing a position: the lowest id becomes the
  group ROOT (`visibility_root_system_id`), later twins chain through
  `visible_parent_system_id`. `NovaResources_EvaluateAvailability` `0x00448090`
  then re-filters `is_visible` through each system's Visibility NCB, and
  `System_ResolveVisibleSystemForTravel` `0x0046b920` /
  `System_ResolveSystemDiscoverySlot` `0x0046b9b0` walk the chain to the twin
  whose expression currently holds. A follow-up loader pass also rewrites
  every Con link to point at the target's visibility ROOT (dropping self-slot
  and duplicate links), so the whole travel/discovery/graph machinery only
  ever sees group roots. Systems whose whole group fails its NCB at game
  start (the clones) are invisible: no marker, no link line, no label,
  unreachable by flood and travel.
- The real fog record is `SystemDef.discovery_state` (+0x90, short): 0 =
  unknown, >=1 = visited (in-flight hyperspace arrival writes 1, landed
  stellar travel and map-outfit reveals write 2), persisted per system as
  u16[0x800] in the pilot save (block1 + 0x1a, `PilotFile_SaveGameCore`
  `0x004c7dd0` / `LoadSave` `0x004cb260`). The debug reveal-all cheat writes 2
  everywhere.
- The starmap draws a marker when `discovery_state > 0` OR the transient
  `discovered_this_rebuild` latch (+0x1ec) is set; that latch is recomputed by
  `System_RebuildSystemVisibilityMap` `0x00467970` and the per-tick
  `System_UpdateSystemAndStellarDisplayState` `0x00432470` as: every VISIBLE
  visited system, plus every travel-resolvable link neighbour of one (links
  resolved through `System_ResolveVisibleSystemForTravel`). So the map shows
  exactly one jump ahead; unvisited neighbours appear but stay unexplored,
  and invisible twin clones never latch at all.

## Clean-room implementation (`src/game/starmap.cpp`)

`NovaStarmap_RunWindow(SdlPlatform&, GameState&)` is a modal loop modelled on
the negotiation/landed dialogs (SDL3, logical 640x480 centred playfield):

- Draws the galaxy graph: `System.links` as lines (deduplicated by drawing
  from the lower index), system nodes as filled circles, and labels for
  visited systems. The drawn set is the original's (`NovaUi_DrawStarmapRoutes
  AndMarkers` 0x004a8100): the whole pass sits behind the outer
  `is_visible && has_explored_flag` gate, so NCB-hidden twin clones never
  draw; markers draw only for the current system, visited
  (discovery_state > 0) systems, or systems latched by the last discovery
  rebuild (visited plus one-hop neighbours; mission targets and the selection
  always draw, compared through discovery slots 0x0046b9b0), a link line
  radiates only FROM a visited system and resolves each link through
  `NovaSystem_ResolveVisibleForTravel` (0x0046b920 — an invisible twin group
  ends the line, so lines still reach one hop into unrevealed space), labels
  draw for visited systems only (zoom-gated, plus always for the selected
  system — the label pass does NOT accept the reveal latch), and unknown
  systems (invisible twin groups, or groups whose NCB fails) have no marker,
  no label, cannot be clicked and are excluded from search. There is NO
  keyboard cycle on the original's map (Tab/Backslash do nothing — the pane
  action's 0x2a/0x36 commands are LShift/RShift, i.e. Shift+click route
  editing). Far-flung unknown systems therefore don't stagger the view either.
- Markers are true solid discs (filled triangle-fan via the geometry path), so the
  node sits exactly on its system position; link endpoints share the same panel
  origin as the markers (the raw world projection is offset by the panel origin),
  so link lines land precisely on the centre of each circle.
- Colour coding: current system = amber; explored = light blue. Markers are
  additionally tinted toward their owning government's theme colour
  (`Government.theme_red/green/blue`, the political-map
  affiliation drawn by `NovaUi_DrawStarmapPoliticalOverlay`), so explored
  systems read as their faction's territory at a glance. Merely-revealed
  (latched, unvisited) systems draw dim grey: the original's marker colour
  gate `Stellar_ComputeStellarDisplayColor` `0x00466260` returns the 0x4000
  grey (SHORT_ARRAY_00733b5c, 64/255 per channel) whenever
  `discovery_state < 1`, so a map reveal reads as full-colour visited nodes
  out to its ModVal rank plus a dim one-jump-ahead ring.
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
  (Right shows what lies to the right of the current view); clicking a system
  selects it subject to the acceptance gate below (the hit is then resolved
  through the visibility twin chain, 0x0046b920).
- Selection reticle: the selected system's green marker is 8 disjoint 4px
  corner ticks around the 13x13 reticle rect (0x004a5a40's eight independent
  SetCursorPos/DrawLineTo pairs) — a partial square with gaps at the edge
  midpoints, never joined into full edges or a cross inside. (An earlier
  clean-room version fed all 16 endpoints to one SDL_RenderLines call, which
  polylines them into full edges plus diagonal connector lines — fixed.)
- The two 16px corner arrows are mission-driven, not selection-driven
  (EVN bible misn flags 0x2 / 0x100, confirmed against vanilla screenshots):
  CICN 0x3a98 (red, pointing down-right from up-left of the marker, box
  corner on the inset marker-rect corner) marks mission-target systems and is
  suppressed on the selected system (0x004a8e5c clears the mission flag on a
  selection-slot match). CICN 0x3a99 (green, pointing down-left from
  up-right) is the misn 0x100 "show green arrow on map in initial briefing"
  highlight: it only draws when the map is opened by the mission-briefing
  flow with the mission's system preselected (DAT_007dc745) — never in the
  plain flight map, so the clean-room does not render it (TODO(decomp): the
  briefing starmap sub-flow). The selection itself is marked by the green
  corner-tick reticle only.
- Click acceptance gate (0x004a4773): a click only LANDS on a system that is
  latched by the last discovery rebuild, is a mission target (raw id compare,
  0x004a4659), or is already selected -- otherwise only when it is a
  travel-resolvable adjacency of the current system (plain click) or of the
  plotted route's tail (shift-click, which additionally accepts positional
  twins of the tail, 0x004a4899; with no hops plotted the shift source is
  the current system and the twin check is skipped). Every other click reads
  as empty space and begins a drag-pan -- distant visited systems are NOT
  clickable (an earlier clean-room revision accepted any visible system).
  The accepted hit is then resolved through the visibility twin chain
  (0x0046b920).
- Shift+click edits the *plotted route* (Ghidra 0x004a47cc; the pane-action
  commands 0x2a/0x36 are LShift/RShift — Tab/Backslash have NO map function,
  an earlier clean-room "destination-ring cycle" was an invention and was
  removed): reset when the hit is on the current system's discovery slot,
  truncate when it slot-matches a plotted hop (that hop and everything after
  are cleared, then the hit is re-appended by the fall-through tail/append
  logic, so the route ends AT the clicked hop — verified against the
  disassembly: the truncate branch at 0x004a4a70 falls through to the tail
  recompute at 0x004a4b1c), pop the tail when it is a twin of it, and
  otherwise append when the tail is visited (or the hit is latched) and the
  hit is a travel-resolvable adjacency of the tail. The normalizer
  (0x004a7e80) runs first, and the map selection moves only when the click
  appends. Every comparison runs through the visibility chain, so hidden
  twin clones can never enter a route.
- Committed-jump accent (faithful to the original): the *active* plotted jump
  is drawn only while the travel-transfer latch is 3 (0x004a8100 line-225
  gate + line-290 accent pass): the current system's armed slot link tints
  dark green (DAT_00733b38) and one thick dark-green line runs from the
  current system to the travel-resolvable destination of the armed slot.
  The latch is set by the starmap plain-click arm (0x004a491e), the route
  re-arm (0x004a8080), the hyperspace-mode toggle and the Backslash cycle
  (0x0044de28 / 0x0044dea7); the Clear Route button clears the armed slot
  (0x004a3aa0 action 8) which kills the accent, and a shift-click route
  reset deliberately leaves the armed jump alone (verified: the reset branch
  only clears the route array). An earlier clean-room revision drove this
  accent from the selection with no mode gate, which let it linger with no
  relationship to the armed jump.
- Rendering fidelity: link lines render as thick quads (2px at the current
  system, brighter for current-adjacent links), marker nodes scale up with
  zoom (mirroring the original's zoom-dependant marker insets), and visited-
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

Jump discovery (clean-room): the original books the entered system as visited
(`discovery_state >= 1` on the discovery slot + current system, in-flight
arrival) then rebuilds the reveal latch — only the arrival system is flooded
(`System_RebuildSystemVisibilityMap(cur, 0, 1)`); the one-jump-ahead window
comes from the transient latch, so neighbours show on the map WITHOUT becoming
explored. Clean-room wiring (travel.cpp):

- `NovaSystem_OnSystemEntered(state, id, level)` — arrival helper: marks slot
  + current visited at `level` and rebuilds. Level 1 on hyperspace completion
  (`FireJump`) and new-game start; level 2 on landed stellar travel
  (`NovaLanding_EnterDocked`), mirroring `Stellar_TravelToSystem` `0x00455e10`.
- `NovaSystem_FloodDiscoverAdjacentSystems` — `0x00467ab0`; depth-gated
  recursion used by the rebuild and (with depth = ModVal) by map outfits;
  neighbours recurse through `NovaSystem_ResolveVisibleForTravel` (0x0046b920)
  like the original, so an invisible twin group blocks the flood.
  TODO(decomp(0x00467bd0)): the per-system region-event trigger is skipped.
- `NovaSystem_RebuildDiscoveryState` — `0x00467970`; rebuild + latch pass.
- The clean-room mirrors the original's flag semantics: the scenario decoder
  sets `is_visible`/`has_explored_flag` for every decoded system (an earlier
  revision kept them as a per-visit fog record; that diverged from the binary
  and left every far system's stellars unavailable - grey starmap rings for
  map reveals - so it was reverted), and `NovaResources_EvaluateAvailability`
  (0x00448090, mission.cpp) then re-filters `is_visible` through each
  system's Visibility NCB - at new-game start, per flight frame, on starmap
  open and on mission-list evaluation - relocating the player via the twin
  chain if the current system's group goes invisible. The loader's
  twin-grouping + link-normalization passes (0x004beb4f) are ported in
  `scenario_data.cpp`: same-position story clones (Koria;Rebs, the b330
  Vell-os region, the b88 Polaris swaps, ...) group under one root and every
  Con link is rewritten to point at the root, so the invisible clones are
  unreachable and unrendered until their NCB holds. Every visited system is
  mirrored into `GameState.control.explored_systems`, the bitset the NCB
  `has_explored` test and save format read.
- The pilot save's discovery block (u16[0x800]) is still TODO(decomp) in
  `pilot_file.cpp`.

## The map outfit (`Outfit_GrantOutfitToPlayer` `0x00427770`)

Outfits with **ModType 16** are one-shot "map" purchases: the original's grant
function runs on every shop take (outfitter `0x0048ea70`, mission script engine
`0x00449370`, boarding window `0x00482940`) and NEVER increments the owned
count for them — the outfit is consumed by its effect. ModVal selects the
reveal:

- `>= 1`: flood-reveals every system within ModVal links of the current system
  (`System_RebuildSystemVisibilityMap(cur, ModVal, 2)`).
- `== -1`: reveals every neutral (government -1) system that has a usable
  travel destination (`System_HasUsableTravelDestination` `0x00468af0`:
  nav stellar with travel_flags 0x20 clear and availability 0x3000 clear).
- `<= -1000`: reveals every system whose government lists `-(ModVal + 1000)`
  in any of its Class 1-4 fields (GovtDef +0x26..+0x2c).

The same function also consumes **ModType 43** (paint: 15-bit ModVal decoded to
5-bit RGB tint, rendering not modelled in the clean-room yet) and **ModType 21**
(clean record: clears negative system reputation — ModVal -1 targets every
system, otherwise the government id in ModVal). Any other mod combination just
stacks the outfit in the owned inventory.

Clean-room: `NovaOutfit_GrantOutfitToPlayer` (outfit.cpp) ports all four paths;
wired at the outfitter buy (`NovaLanded_BuyOutfit` — a consumed item returns
after one unit instead of stacking) and the mission script 'G' grant opcode.
The `DAT_007d4c08/09` dirty latches (map/record UI refresh) have no clean-room
consumer and are skipped.

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
- Multi-hop plotted routes exist (Shift+click route editing, green route
  chain, route → travel-target sync on close and on jump arrival), but the
  mission-info window's destination-window sub-flow (DAT_007354a6 route mode)
  is still not reconstructed.
- No mission-highlight icons / mission jump planning.
- No licence-seed easter-egg branch.
- Pan/zoom are keyboard-driven; no drag-to-pan / wheel zoom yet.
- The map only inspects/selects; the actual jump stays with `NovaTravel_Tick`.
