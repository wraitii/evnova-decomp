# Plan for scenario testing

Determinism is deferred.

## 1. Centralize gameplay time — complete

Replace direct gameplay `SDL_GetTicks()` access with a clock owned by the
platform/runtime. Preserve normalized ticks, original 21 ms logical calls, and
genuine wall-clock domains.

Implemented with `SdlPlatform::gameplay_ticks_ms()` and explicit wall-clock
access for presentation-only timing. Gameplay helpers receive the current
clock through `GameState::gameplay_now_ms` where passing the platform would
couple otherwise SDL-independent logic to the runtime.

## 2. Add accelerated gameplay-clock mode — complete

Add opt-in execution settings:

- An intuitive gameplay-time speed multiplier independent of render rate.
- No VSync.
- No `SDL_Delay(16)`.
- The same pacing policy across spaceflight and modal loops.
- Continue routing frame boundaries through `SdlPlatform::Present()`.

Implemented as the probe-only `accelerate` command with an integer
`speed_multiplier`. The scaled gameplay clock remains continuous when the
mode or multiplier changes, and probe pauses do not advance gameplay time.

This immediately benefits probe-driven testing, even without autopilot.
Because determinism is postponed, validation tests cannot be too exact, but
they can verify that:

- Equal simulated durations produce comparable displacement and velocity.
- Timers expire within the expected virtual-time window.
- Raw 21 ms cadence work runs the expected number of times.
- Landing and jump transitions reach the same outcome.
- No state depends on host execution speed.
- General statistical properties remain within expected bounds.

## 3. Add practical headless execution (optional except for audio) - done for audio

Initially use a hidden/offscreen SDL window and renderer:

- Suppress audio in accelerated mode, with logical completion timing wherever
  gameplay depends on it (primarily hyperspace jumps, based on current
  findings).
- Retain rendering, resource-loading, layout, and other render-path side
  effects.
- Optionally perform graphical presentation only at observation points while
  continuing to execute required side effects.

Avoid renderer-free operation until render-side effects have been audited.

## 4. Add an optional automation input seam

Invoke an automation controller after physical/probe input is collected. It
reads `GameState` and the shared player `Ship`, then produces or modifies
`FlightInput`.

Disabled automation must be a no-op.

## 5. Implement basic flight policies

Implement, in order:

- Face a point.
- Approach and stop.
- Select, approach, and land at a stellar.
- Select a jump destination and travel.
- Target and fire.
- Board or hail.

Reuse AI geometry, predicates, state transitions, and control dispatch where
feasible. Avoid invoking NPC behavior supervisors on the player until their
side effects are explicitly characterized.

## 6. Add semantic modal automation

Drive existing published UI elements and input channels:

- Accept or decline.
- Choose list entries.
- Buy or sell.
- Launch or land.
- Open mission information.

Do not mutate mission or inventory state directly.

## 7. Add the scenario runner

Express workflows as state machines with observable predicates,
simulation-time deadlines, deadlock detection, and diagnostic snapshots.

## 8. Add end-to-end scenarios

Milestones:

- Accelerated idle-flight equivalence test.
- Accelerated landing and jump test.
- Automated new-pilot flow.
- Tutorial at ordinary speed.
- Tutorial at accelerated gameplay-clock speed.
- Hidden/offscreen CI execution.

The key dependency chain is:

```text
virtual clock -> accelerated execution -> automation input -> scenario runner -> tutorial E2E
```

## Concrete first automation slice

Target the tutorial first; defer combat, boarding, and general-purpose
autopilot behavior.

### Controller structure

Mirror the AI's high-level/low-level split while producing only `FlightInput`:

- High-level goals: `landAt(stellar_name)`, `jumpTo(system_name)`.
- Low-level maneuvers: `moveAwayFrom(point)`, `stop()`,
  `moveToAndStopAt(point, radius)`.

Invoke the optional controller after physical/probe input and persisted key
bindings have populated `FlightInput`, but before player command latches are
processed. Disabled automation is an exact no-op and never writes gameplay
state directly.

Reuse the AI's bearing, alignment, approach, and turnaround/braking logic where
it can be factored into pure helpers. Do not run the NPC supervisor or NPC
movement integrator on the player: those paths use AI-only damping, weapon
banks, stats, targeting, boarding resolution, and other side effects.

`stop()` turns opposite the velocity vector without thrust, then thrusts once
aligned. It brakes velocity *relative* to an optional reference velocity, so a
zero reference is a full stop while a non-zero reference matches a moving
target.

`moveToAndStopAt()` is the general arrive-and-settle primitive: it drives to
the point and stops within `radius`, matching the target's velocity when one is
supplied. Speed is capped by the turnaround-drift-plus-braking distance and by
the turn radius (`kTurnRadiusSpeedFraction * omega * distance`), so a
slow-turning hull sheds speed to curve in instead of orbiting. It leads a
moving target by the estimated closing time, and a velocity-error law (thrust
along `desired velocity - relative velocity`) replaces the old phase switching
between "face target" and "face reverse velocity". All control is expressed
through `FlightInput`; the one-frame alignment gate scales with the frame's
normalized tick count so accelerated probe runs stay consistent.

### Goal transitions

`landAt(name)`:

1. Resolve an exact, case-insensitive stellar name in the current system;
   reject missing or ambiguous names.
2. Select it through edge-triggered stellar-cycle input.
3. Move inside the target's actual per-axis arrival envelope
   (`Stellar_MaxLandingDistance` of the displayed sprite) and settle below the
   per-axis landing velocity gate; the low-level maneuver drives to the
   stellar and stops once inside a tolerance of half the envelope.
4. Wait for the approach timer to arm, tap Land, and complete only when the
   landed/Spaceport modal is observed.

`jumpTo(name)`:

1. Resolve an exact, case-insensitive directly linked system name; reject
   missing, ambiguous, current, or non-adjacent systems.
2. Toggle hyperspace mode and cycle destinations through input until selected.
3. Tap Travel, then stop emitting steering while the existing jump state
   machine owns the player.
4. Complete when the requested system is current and jump engagement has
   ended.

The controller must generate one-frame taps followed by release and wait for
the corresponding observed state change before issuing another edge command.
It fails cleanly on player death, invalidated destinations, unexpected flight
exit, or a simulation-time deadline. Until save loading exists, death aborts
the attempt with state/log/screenshot diagnostics and the scenario runner may
restart from the new-pilot flow.

### Probe and UI surface

Add narrow semantic probe commands/status for starting, cancelling, and
observing these goals. This configures an input producer, like probe-held keys;
it does not grant direct writes to ship, mission, inventory, or travel state.

Publish the root Spaceport controls (`launch`, `refuel`, `trade_center`,
`outfitter`, `shipyard`, `bar`, `mission_bbs`) so landing completion and the
tutorial's docked transitions can be driven by intent. Continue using the
existing published controls for nested modals.

Unit-test steering wraparound, turnaround without thrust, braking transitions,
settling under real player movement integration, edge-command taps, name
resolution, and failure transitions. The first probe workflow is new pilot ->
`landAt` -> tutorial modal actions -> launch -> `jumpTo`.
