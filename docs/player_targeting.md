# Player target selection (ship target + travel/landing stellar)

Ground truth for the two independent player target channels, read from
`Ship_HandlePlayerShipCore` (0x0044aa70 — one Metrowerks-collapsed routine;
`PlayerTick_TargetAndTravelCommands` 0x0044b7c4 is the relevant internal
label) and the cycle helpers. Port side: `src/game/targeting.cpp/hpp`,
`src/game/spaceflight.cpp`.

## Channel 1: the primary target ship (`primary_target_ship_slot`)

- **Selection** is persistent — a slot stored on the player ShipState. It is
  set by the backquote cycle (`Ship_FindNextPlayerCycleTarget` 0x00461bd0 /
  `_Previous` 0x00461f60), the nearest-hostile/engaged commands
  (0x00462bd0 / 0x00462850) or a mouse click.
- **Cycle has no wrap.** Both finders scan from `current_slot + 1` (or -1 →
  slot 1) to the end of the table and return `current_slot` unchanged when no
  candidate exists. The caller (`PlayerTick_TargetAndTravelCommands`) then
  treats a no-op — or a self result — as **clear to -1 ("none selected")**.
  So cycling past the last eligible ship deselects; the next press starts
  over from the first. Combat-relevance is an *equality* filter with the
  held modifier (`relevant == modifier`), so ` and Alt+` cycle disjoint
  halves of the population.
- **Per-frame validation** (0x0044aa70 prologue) drops the selection when the
  target ship is inactive, destroyed (`Ship_IsShipDestroyed`), in **AI state
  0x15 (jump-out)**, or cloaked past `Ship_IsShipCloakVisibilityThresholdActive`
  while the target has its own AI target — unless the player owns the
  cloak-scanner outfit. There is **no same-system check**: a targeted ship is
  not dropped merely for moving; the 0x15/inactive gates are what release a
  ship that jumps away.
- **System arrival clears it** (`PlayerTick_SystemTransitionAndArrival`
  0x0044f660: `travel_transfer_mode = -1; ai_secondary_target_slot = -1;
  primary_target_ship_slot = -1;`).

## Channel 2: the travel/landing stellar (`ai_secondary_target_slot`)

- **There is no per-frame auto-seed.** `Stellar_FindNearestAvailableTravel-
  Stellar` (0x00462db0) is called only from the land command, the
  target-nearest command and the new-game flow. Between explicit commands the
  selection simply holds (or is empty): the original does not keep a planet
  bracketed all the time.
- Explicit setters: the **number keys 1-4** (`nav_stellar_ids[0..3]` of the
  current system), a **click** on a stellar's sprite (only when the click did
  not change the ship target — the ship scan runs first and the stellar scan
  is gated on the ship target being unchanged from its pre-click value; a
  click on the player's own sprite clears the ship target), the
  **target-nearest** command, the **land command** (auto-picks the nearest
  available travel stellar when `travel_transfer_mode != 2`), the starmap
  route sync, and the **destination-system cycle** (which sets mode 3 =
  adjacent *system* for jumping, not a planet). The stellar hit test uses the
  ambient sprite's rect, expanded 16px per side when shorter than 0x30.
- **Clears**: system arrival (above), the clear-target command (mode -1),
  player death, and the click-empty path. No availability re-validation per
  frame — the gates run when the selection is *used* (landing, jump
  direction, target action).
- The original's cycle-travel-target command rotates adjacent **systems**
  (mode 3), not planets. The port's Tab-cycles-planets binding is a
  clean-room addition; it follows the ship-cycle semantics (no wrap,
  clear-to-"none" at either end) so the player can always deselect.

## Both channels are independent

The original happily shows a ship reticle *and* a stellar selection at once:
clicking a ship leaves an existing planet selection alone (the stellar click
scan only runs when no ship was hit), and cycling ships never touches
`ai_secondary_target_slot`. The port previously auto-seeded a nearest planet
every frame, which made a bracket permanently visible — that divergence is
removed; the trade-off is that the land command now performs the original's
nearest-stellar auto-pick when nothing is targeted.
