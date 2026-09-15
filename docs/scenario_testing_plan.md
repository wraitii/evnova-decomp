# Plan for scenario testing

Determinism is deferred.

## 1. Centralize gameplay time

Replace direct gameplay `SDL_GetTicks()` access with a clock owned by the
platform/runtime. Preserve normalized ticks, original 21 ms logical calls, and
genuine wall-clock domains.

## 2. Add fixed-step accelerated mode

Add opt-in execution settings:

- Fixed virtual timestep.
- No VSync.
- No `SDL_Delay(16)`.
- The same pacing policy across spaceflight and modal loops.
- Continue routing frame boundaries through `SdlPlatform::Present()`.

This immediately benefits probe-driven testing, even without autopilot.
Because determinism is postponed, validation tests cannot be too exact, but
they can verify that:

- Equal simulated durations produce comparable displacement and velocity.
- Timers expire within the expected virtual-time window.
- Raw 21 ms cadence work runs the expected number of times.
- Landing and jump transitions reach the same outcome.
- No state depends on host execution speed.
- General statistical properties remain within expected bounds.

## 3. Add practical headless execution (optional except for audio)

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
- Tutorial at accelerated fixed-step speed.
- Hidden/offscreen CI execution.

The key dependency chain is:

```text
virtual clock -> accelerated execution -> automation input -> scenario runner -> tutorial E2E
```
