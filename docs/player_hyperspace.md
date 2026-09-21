# Player hyperspace jump: fast-jump and inertialess parent control flow

Ghidra: `Ship_HandlePlayerShipCore` 0x0044AA70 (one Metrowerks-collapsed routine,
1530 basic blocks). This note records the **fast-jump** and **inertialess**
slices and the synthetic region boundaries used to isolate them. Regions below
are CFG views, not original functions; no tracker rows are created for them.

## Capability predicates

`Ship_CheckSpecialLoadoutCapability` 0x0046D080, ported as
`NovaOutfit_HasFastJumpCapability` (src/game/outfit.cpp/hpp):

- true when the ship class sets `Flags2` (`ShipClassDef` +0x9EA) bit `0x0020`
  (Bible: "Ship can jump without slowing down"), **or**
- when the ship carries an outfit whose any of its four ModTypes is `0x25`
  (37 decimal, `OutfitEffect::kFastJump`; Bible: "fast jumping").
- The loadout scan branches on `ship_instance_id == 0`: the player scans
  `g_outfit_owned_count` (owned means a **positive count**; the ModVal is
  irrelevant and no activation is required), NPCs scan the class default
  loadout (up to 8 entries).

The helper is ported with four direct callsite equivalents: the
`Ship_HandlePlayerShipCore` kBrake stop gate (0x0044C4DB) and kHold damp gate
(0x0044C726), plus `Ship_UpdateShipAiState` state-2 brake (0x004060D1) and
state-3 attack jump-departure arm (0x00406720). The fifth, the player mode-3
map-bearing re-aim gate (0x0044ED32), is approximated by the port's
engaged-`kHold` alignment rather than an exact callsite equivalent (one-frame
entry offset); the original has **no armed pre-engage re-aim path** (see the
gate below).

`Outfit_ShipIsInertialess` 0x0046DF70 selects the inertialess arms: class
`Flags2` `0x40`, or an owned inertial dampener (ModType 38,
`OutfitEffect::kInertialDampener`). Ported as `NovaPlayer_IsInertialess`
(player branch, src/game/outfit.cpp) and `NovaShip_IsInertialess` (NPC branch,
src/game/spaceflight.hpp).

## Synthetic regions (parent 0x0044AA70)

| Region | Entry | Stops | Semantics |
| --- | --- | --- | --- |
| FastJumpStopBypass | 0x0044C4DA | 0x0044C4E9, 0x0044F020 | `CALL 0x0046D080`; special -> fall through to hold begin 0x0044C4E9, ordinary -> per-axis `|trunc(vel)| < 2` stop gate at 0x0044F020. |
| BrakeEntry | 0x0044F275 | 0x0044F0E3, 0x0044F127 | Reached when the stop gate fails: stamp `ai_mode_start_time_ms` (0x0044F27A) and `ai_station_hold_timer = 1.0` (0x0044F280), then `Outfit_ShipIsInertialess` 0x0046DF70-selected arm -- inertialess scalar decay 0x0044F0E3, ordinary turnaround 0x0044F127. |
| HoldBeginFlags | 0x0044C4E9 | 0x0044C528 | Waypoint-marker reset only: if `sprite_behavior_flags & 2` and `!(sprite_behavior_flags & 0x80)` and `waypoint_arrival_marker_b > 0`, set `waypoint_arrival_marker_a = -1`. The hold seed/clock/audio are not here (see HoldTimerRamp). |
| HoldTimerRamp | 0x0044C528 | 0x0044C6D7 | Hold begin, guarded on `ai_station_hold_timer <= 1.0`: seed `ai_station_hold_timer = 2.0`, stamp `ai_mode_start_time_ms` with the 60 Hz tick, mark the travel-status panel dirty and latch the 0x7fff hint, then pre-stage the class-scaled 'Warp up' voice (the `g_x2_mode_active` branch picks the cue pair). |
| BrakeHoldJoinGate | 0x0044C6D7 | 0x0044C704, 0x0044C833 | Rejoin after either brake arm: `if (local_27a == 0 && ai_station_hold_timer <= 1.0) \|\| local_251 != 0` -> 0x0044C833 (skip the hold tick and the 0x0044F414 slow damp); else fall through to the 0x0044C704 hold tick. `local_27a` is 0 from the jump-command dispatch init 0x0044C19C and set 1 only at hold begin 0x0044C4E9, so the brake frame always skips. |
| HoldTickSpecialCheck | 0x0044C704 | 0x0044C734, 0x0044F414 | squad sync + hold clock, then `CALL 0x0046D080`; non-special damps velocity by `g_hyperspace_slow_phase_velocity_damp` (0.98006866) at 0x0044F414, special keeps momentum. |
| InertialessSteeringTail | 0x0044CFFE | 0x0044D05B | shared manual-flight inertialess tail: clamp scalar `speed` to `g_player_speed_cap_x`; fire-restricted decays it by the `g_inertialess_fire_restricted_speed_damp` double 0.985 (0x005755E0, single multiply per call, no tick scale) and decrements the glow, otherwise ramps the glow; then `Ship_SteerVelocityTowardShipHeading` 0x0043B020. Runs after the hold tick and the auto-turn, before position integration, so it is live during the engaged brake **and** hold. |

## Mode-3 re-aim gate (the fifth call site)

0x0044ED32 sits in the per-frame `travel_transfer_mode == 3` continuation
reached from 0x0044C18A. The verified original gate is

```
travel_transfer_mode == 3
  && ai_secondary_target_slot != -1          (0x0044ED12 -> 0x0044ED1D)
  && ai_station_hold_timer > 0.0             (0x0044ED1D, DAT_00575538 = 0.0)
  && (fast_jump || |trunc(vel_x)| < 2 && |trunc(vel_y)| < 2)   (0x0044ED32)
```

While the hull is merely armed (timer not positive) the re-aim body is skipped,
so no armed pre-engage re-aim exists; the 0x0044ED00 prologue still clears the
travel-selected globals (`g_travel_selected_stellar_id` /
`g_travel_engage_timer`) before the gate. Inside the window, `CALL 0x0046D080`;
special (or a stopped ship) runs the map-bearing re-aim at 0x0044EDFD
(`ai_desired_heading_deg` from the current system to its linked destination)
and arms the manual auto-turn latch `local_265`; ordinary moving hulls return
to the command dispatch. The brake holds `ai_station_hold_timer == 1.0` and a
moving ordinary hull fails the `|trunc(vel)| < 2` test, and fast-jump hulls
never enter the brake, so the block is a real engaged brake/hold alignment,
never an armed one.

## Reproducible synthetic views

Read-only `POST /function/synthetic-decompile` payloads (parent
`Ship_HandlePlayerShipCore`); `names:"off"` keeps them fast, and stops are
exclusive boundaries.

```json
{"entry":"0x0044f0e3","stops":["0x0044c6d7"],"name":"InertialessBrakeArm","names":"off"}
{"entry":"0x0044cffe","stops":["0x0044d05b"],"name":"InertialessSteeringTail","names":"off"}
{"entry":"0x0044ed00","stops":["0x0044c195"],"name":"Mode3ReaimGate","names":"off"}
{"entry":"0x0044c6d7","stops":["0x0044c704","0x0044c833"],"name":"BrakeHoldJoinGate","names":"off"}
```

Gate anchors: `local_27a = 0` at 0x0044C19C (jump-command dispatch entry,
before the input check), `local_27a = 1` at
0x0044C4E9 (hold begin), the brake stamps `ai_mode_start_time_ms` at 0x0044F27A
and `ai_station_hold_timer = 1.0` at 0x0044F280, and the 0x0044C6D7 join test
sends the brake frame to 0x0044C833, skipping the 0x0044C704 squad-sync /
hold-clock block and the 0x0044F414 slow damp.

## Port mapping (src/game/travel.cpp, `NovaTravel_Tick`)

- `JumpPhase::kBrake`: `stopped = fast_jump || (|trunc(vel_x)| < 2 &&
  |trunc(vel_y)| < 2)`. A fast-jump hull enters the hold on the first tick
  after engage. A non-fast-jump inertialess hull takes the 0x0044F0E3 arm:
  decay the maintained scalar `speed` by `Ship_ComputeShipEffectiveThrust *
  tick_scale`, floor at zero, then steer velocity toward `heading * speed`
  through `NovaShip_SteerVelocityTowardShipHeading` (0x0043B020). The original
  arm writes no turnaround damping and no glow; the port's `fade_glow()` is an
  approximation of the unported shared-tail glow behavior. A non-fast-jump
  non-inertialess hull keeps the reverse-velocity turnaround.
- `JumpPhase::kHold`: the slow-phase damp runs only when the hull is **not**
  fast-jump; inertialess hulls keep the maintained `speed` scalar rather than
  deriving it with `hypot`. The hold writes the integer map bearing into
  `ai_desired_heading_deg`, turns onto it at the class turn rate, then, for
  inertialess hulls, calls `NovaShip_SteerVelocityTowardShipHeading` (the
  steering step of the 0x0044CFFE tail; the tail's scalar cap/decay/glow parts
  remain unported) before integrating position. Fast-jump non-inertialess
  hulls keep their momentum until `FireJump` overwrites it.
- The kHold re-aim corresponds to the 0x0044ED32 gate above: the port only
  reaches `kHold` after a plotted destination is engaged, so the mode-3 and
  secondary-target conditions hold, and the engaged state is the timer
  condition. The port's unconditional engaged-hold alignment therefore covers
  the block; the remaining difference is the one-frame entry offset below.
- The no-jump radius gate (0x0044C220 + `Stellar_ComputeTravelRangeSq`) is
  **not** bypassed by fast jump; only the brake stop gate is.

## Remaining gaps

- **One-frame entry offset**: the original re-aims/positions on the
  brake->hold crossing frame (re-aim at the top of the mode-3 dispatch, then
  0x0044F020 -> 0x0044C4E9 hold begin in the same frame); the port runs the
  `kHold` body on the following tick. Port equivalence of the surrounding
  turn/latch path beyond this offset was not established.
- `HoldBeginFlags`' `waypoint_arrival_marker` write is not modelled.
- **Inertialess shared-tail coverage is partial in the suspended-movement jump
  path (both brake and hold).** The port calls
  `NovaShip_SteerVelocityTowardShipHeading` (0x0043B020) from both, but the rest
  of the 0x0044CFFE tail's effects are unported for both: the
  `g_player_speed_cap_x` scalar clamp, the fire-restricted
  `g_inertialess_fire_restricted_speed_damp` (0x005755E0) double 0.985 decay,
  and the speed-proportional engine-glow ramp (`fade_glow()` is a placeholder,
  marked `TODO(decomp)` in travel.cpp). The brake entry also does not stamp
  `ai_mode_start_time_ms` (0x0044F27A) or `ai_station_hold_timer = 1.0`
  (0x0044F280) as the original does.
- **The port's ordinary brake omits one original write.** The original
  0x0044F127 arm writes `ai_desired_heading_deg` (+0x68) = the reverse bearing
  at 0x0044F184. The port's ordinary `kBrake` computes that same angle for its
  turn but never assigns it to `player.ai_desired_heading_deg`. The only known
  consumer of a leader's field is the mode-0xD escort heading mirror, which
  requires `leader.ai_station_hold_timer > 1.0`; the brake holds the timer at
  exactly 1.0 (original) or `<= 0` (port), so the mirror never reads it during
  the brake. No escort effect is established -- this is an unmodelled field
  write, not a known behavioral regression.

## Tests

`tests/travel_test.cpp`: "fast-jump class skips the brake and keeps momentum",
"owned fast-jump outfit grants the capability", "defined but unowned fast-jump
outfit does not grant", "fast-jump in an alternate ModType with ModVal 0
grants", "fast-jump is still denied inside the no-jump range", "inertialess
hull decays scalar speed without turning in the jump brake", "owned inertial
dampener selects the jump-brake speed-decay arm", "inertialess jump brake
floors scalar speed and steers off-heading velocity", "fast-jump wins over
inertialess in the jump brake", "fast-jump inertialess hold steers off-heading
velocity", "fast-jump inertialess hold turns while steering velocity", "ordinary
hold applies the slow-phase velocity damp", "inertialess hold keeps its
maintained scalar through the damp"; the existing moving-ship brake test covers
ordinary braking unchanged.

`tests/movement_test.cpp`: "inertialess fire-restricted decay is one 0.985
multiply per call" pins the shared-tail scalar decay constant and per-call
cadence with a fixed speed 10 (10 -> 9.85 -> 9.85 * 0.985).

`tests/ship_ai_test.cpp`: "state 2 fast-jump class and default outfit skip the
outward brake" (class flag, default-outfit count 0 vs 1, alternate ModType),
"state 3 fast-jump arm selects outward thrust while moving".
