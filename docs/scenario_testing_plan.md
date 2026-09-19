# Plan for scenario testing

Determinism is deferred (no seed control). Prerequisites in place: centralized
gameplay clock (`SdlPlatform::gameplay_ticks_ms()`), probe-only accelerated
clock, headless/audio suppression, probe harness. See `docs/probe_harness.md`.

## 1. Automation input seam

An optional controller runs after physical/probe input is collected, reads
`GameState`/player `Ship`, and produces or modifies `FlightInput`. Disabled
automation must be an exact no-op and never write gameplay state directly.

## 2. Basic flight policies

In order: face a point; approach and stop; select/approach/land at a stellar;
select a jump destination and travel; target and fire; board or hail. Reuse AI
geometry, predicates, state transitions and control dispatch where feasible,
but do not run the NPC supervisor or movement integrator on the player (AI-only
damping, weapon banks, targeting, boarding side effects).

## 3. Semantic modal automation

Drive published UI elements and input channels: accept/decline, list choice,
buy/sell, launch/land, open mission info. Do not mutate mission or inventory
state directly.

## 4. Scenario runner

State machines with observable predicates, simulation-time deadlines, deadlock
detection and diagnostic snapshots.

## 5. End-to-end scenarios

Milestones: accelerated idle-flight equivalence; accelerated landing and jump;
automated new-pilot flow; tutorial at ordinary and accelerated clock speed;
hidden/offscreen CI. Dependency chain:
`virtual clock -> accelerated execution -> automation input -> scenario runner -> tutorial E2E`.

## Concrete first automation slice

Target the tutorial first; defer combat, boarding and general-purpose autopilot.

The controller mirrors the AI high/low split while emitting only `FlightInput`:
high-level goals `landAt(stellar_name)` / `jumpTo(system_name)`; low-level
maneuvers `moveAwayFrom(point)`, `stop()`, `moveToAndStopAt(point, radius)`. It
runs after physical/probe input and persisted bindings populate `FlightInput`,
but before player command latches.

- `stop()` turns opposite the velocity vector without thrust, then thrusts once
  aligned, braking relative to an optional reference velocity (zero = full stop).
- `moveToAndStopAt()` drives to the point and settles within `radius`, matching a
  supplied target velocity; speed is capped by turnaround-drift-plus-braking
  distance and turn radius (`kTurnRadiusSpeedFraction * omega * distance`); it
  leads a moving target by estimated closing time and uses a velocity-error law.
  All control goes through `FlightInput`; the one-frame alignment gate scales
  with the normalized tick count.

**`landAt(name)`**: resolve an exact case-insensitive current-system stellar
(reject missing/ambiguous); select it via edge-triggered cycle input; move
inside the per-axis arrival envelope (`Stellar_MaxLandingDistance`) and settle
below the landing velocity gate; wait for the approach timer, tap Land, complete
when the Spaceport modal is observed.

**`jumpTo(name)`**: resolve an exact directly linked, non-current system (reject
missing/ambiguous/non-adjacent); toggle hyperspace and cycle destinations via
input; tap Travel, stop steering while the jump state machine owns the player;
complete when the system is current and jump engagement has ended.

Emit one-frame taps then release, waiting for the observed state change before
the next edge command. Fail cleanly on player death, invalidated destinations,
unexpected flight exit, or a simulation-time deadline (until save loading
exists, death aborts with diagnostics).

**Probe/UI surface**: narrow semantic commands/status to start, cancel and
observe goals (configures an input producer, no direct state writes). Publish
root Spaceport controls (`launch`, `refuel`, `trade_center`, `outfitter`,
`shipyard`, `bar`, `mission_bbs`); keep existing nested-modal controls.

Unit-test steering wraparound, turnaround without thrust, braking transitions,
settling under real movement integration, edge-command taps, name resolution and
failure transitions. First workflow: new pilot -> `landAt` -> tutorial modal
actions -> launch -> `jumpTo`.
