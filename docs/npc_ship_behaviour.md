# NPC ship behaviour

This is a reference for the original NPC AI and the current reconstruction,
rather than an implementation plan.  The authoritative primary evidence is
Ghidra's `Ship_UpdateShipAI` (0x00401000), `Ship_UpdateShipAiState`
(0x00405590), `Ship_ApplyShipAiControls` (0x00408150), and
`Ship_HandleShip` (0x00433050).  The clean-room counterparts are primarily in
`src/game/ship_ai.cpp`, `src/game/spaceflight.cpp`, and
`src/game/game_state.hpp`.

`ai_state_code` is the high-level state machine.  It chooses an
`ai_control_mode`, which is a single-frame steering/throttle command.  The
movement integrator then consumes the command.  Do not infer a state meaning
from a control-mode number with the same value: state `0x0e`, for example, is
a fast arrival coast, while control mode `0x0e` is a combat brake/evasion.

## Per-frame flow

```text
Ship_UpdateShipAI
  -> behaviour supervisor (targeting, travel, escort policy)
  -> Ship_UpdateShipAiState (state -> control mode)
  -> Ship_ApplyShipAiControls (control mode -> heading/speed/thrust fields)
  -> Ship_HandleShip (turn, accelerate/coast, integrate position, visuals)
```

The reconstruction ticks active non-player ships in the current system by the
same frame cadence.  The core movement, wander/travel, combat pursuit, NPC
weapons, vacant-ship cleanup, and an initial NPC system-transfer slice are
present.  Mission/fleet policy, full formation handling, specialized weapon
guidance, and full jump presentation remain incomplete.

Fire-restricted/disabled ships are not frozen in place. `Ship_HandleShip`
(0x00433050) multiplies `vel_x`, `vel_y`, and scalar `speed` by `0.94`
(`DAT_00575448`) each frame before position integration. Their AI is suppressed
and they receive no new thrust orders, so they coast down gradually.

## State machine (`ai_state_code`)

The names below are descriptive; a few state arms still have deferred mission
or weapon side effects.  “Typical control” is a useful observation, not an
exclusive mapping.

| State | Descriptive role | Typical control | Key transition / outcome |
|---:|---|---:|---|
| `0x00` | Idle / behaviour dispatch | `0x00` | Supervisor acquires a travel point, contact, or escort task. |
| `0x01` | Travel to stellar | `0x02`, then `0x01` | Arrive, damp below 0.35 px/tick, record stellar, re-enter idle. Restricted travel points enter `0x14`. |
| `0x02` | Jump departure staging | `0x01`, `0x03`, `0x04` | This state does not seek the centre: while moving it brakes; inside the centre envelope mode `0x03` thrusts outward, otherwise stopped ships arm outward jump spin-up with mode `0x04`. |
| `0x03` | Hold/approach with a primary target | varies | Station-hold timer and target validity determine the next combat or hold action. |
| `0x04` | Attack engagement | `0x05`/`0x06` | Cloak-aware eligibility can brake/wait, clear the target, or fall back to travel. |
| `0x05` | Pursue / follow a target | `0x0b`, `0x08` | Approaches target range, then follows/holds. |
| `0x06` | Park / settle | `0x01` | Brakes to a stop. |
| `0x07` | Escort / follow primary | `0x08`/`0x09` | Drops to idle if target becomes invalid; detailed formation offsets are incomplete. |
| `0x08` | Arrival slowdown | `0x0a` | Applies the stepped high-speed slowdown command to a newly arriving NPC; the ordinary ship handler later resets it to state 0. |
| `0x09` | Refuel / transfer service | `0x0b`, `0x01` | Approaches the primary target, stops, then transfers fuel while the target has capacity. |
| `0x0a` | Assist response | `0x09`, `0x01`, `0x0c` | Long-range pursuit, approach, or velocity-match engagement. |
| `0x0b` | Station hold / follow target | `0x04`, `0x0d` | Holds near a player/leader; exits if leader's hold state ends. |
| `0x0c` | Player-oriented assist / hold | `0x09`, `0x0b`, `0x0c` | Clears ordinary targets and selects distance/velocity matching around the player. |
| `0x0d` | Cloak-engagement wait variant | `0x01` | Uses finite patience before abandoning an unengageable target. |
| `0x0e` | Timed coast / combat break | `0x00` | Clears targets and directly advances position along heading at `elapsed_ticks * 0.7`; returns idle when the normalized-tick timer expires. This is not an NPC jump-in state. |
| `0x0f` | Boarding / disabled-target approach | `0x0b`, `0x0f` | Rejects non-disabled targets, then approaches and velocity-matches. Capture side effects are deferred. |
| `0x10` | Scripted asteroid manoeuvre | `0x13`, `0x14` | Uses asteroid-pool slot 0 as the scripted position/velocity target. |
| `0x11` | Freeflight-anchor positioning | `0x15` | Chooses a freeflight anchor; the anchor model is not reconstructed. |
| `0x12` | Stellar approach/retreat staging | `0x02`, `0x03`, `0x01`, `0x16` | Uses a selected stellar and weapon range to approach, settle, or retreat. |
| `0x13` | No dedicated handler | — | No state-specific arm was found in `Ship_UpdateShipAiState`; preserve this neutral label pending caller evidence. |
| `0x14` | Hypergate/wormhole entry | `0x02`, `0x17` | Negotiates entry by approaching the selected restricted stellar/link. At the entry point it enters mode `0x17` and transfers/vanishes; there is no local mode-`0x04` hyperjump sequence. |
| `0x15` | Hypergate/wormhole emergence | `0x00` | Spawn/arrival setup at the destination stellar: seeds emergence heading, a 60-tick hold (~2 s at 30 Hz), and a slower state-8 entry speed (30 px/tick normally; 15 when targeting the player). Random dudes and encounter-fleet leads use this path when an adjacent restricted stellar is selected. |
| `0x16` | Defunct cleanup | `0x01` | Clears targets/firing; returns idle after the manoeuvre timer. |

State `0x13` is deliberately neutral: its lack of a state-specific arm is
evidence, whereas its callers have not yet established a reliable role.

Arrival clarification: the original random-dude and encounter-fleet spawners
call `Stellar_SelectRandomAdjacentDestination`. Its 1-in-3 roll selects a
candidate-pool flavor; it is not a direct 1-in-3 emergence gate. The selected
travel point is accepted for emergence only when it is a restricted stellar.
On success they place the ship there and enter state `0x15`; on failure they call
the state-0x08 slowdown entry after placing the ship on a random polar
offset. State `0x0e` is a separate timed coast/break state selected by combat
logic and is not the occasional jump-in branch.

## Movement controls (`ai_control_mode`)

These are low-level commands issued by the state machine.  Modes marked
“partial” have their motion arm reconstructed, while a related weapon,
formation, bay-launch, or script side effect is still absent.

| Mode | Name | Movement behaviour |
|---:|---|---|
| `0x00` | Idle | No thrust; holds heading. Refreshes automatic weapons. |
| `0x01` | Brake to stop | Turns toward reverse velocity; full thrust, then 0.5x thrust + 0.94 damp, then 0.95 damp + idle below 0.35 px/tick. Gravity shields use -0.5x thrust. |
| `0x02` | Travel to stellar | Heads toward stellar/map point; thrust only inside `turn_rate + 5°`. Far away desired speed is 0 (normal max-speed clamp); within 500 px on both axes it is 0.25x max speed. |
| `0x03` | Depart from centre | Computes the bearing from the system centre `(0,0)` to the ship and thrusts outward when within `turn_rate + 3°`. At the exact centre the bearing helper's zero-vector convention supplies the initial heading. |
| `0x04` | Outward jump spin-up | Points from the system centre toward the ship, advances a hold timer, and lets `Ship_HandleShip` ramp movement along that outward heading. After a 350 ms base duration, the original deactivates/clears system identity (class multiplier unresolved). |
| `0x05` | Attack strafe-out | **Heads away from the primary target**: its bearing helper arguments are reversed in the original. Thrust gate is `turn_rate + 20°`; close-range boost can follow. |
| `0x06` | Combat pursuit | Predictive/direct aim; normal thrust gate `turn_rate + 15°`. Can break to evasive `0x10` or boost `0x11`; gravity-shield ships may match speed close in. |
| `0x07` | Combat strafe | Guided predictive aim; thrust gate `turn_rate * 4`; may boost to `0x11`. |
| `0x08` | Escort follow | Heads at lead; thrust gate `turn_rate + 1°`. Formation/launch handoff is partial. |
| `0x09` | Hold at distance | Steers at target while distant, otherwise matches velocity bearing in a 15° window. |
| `0x0a` | Arrival slowdown command | Physics override: desired speed starts at -50.0 and advances by 1.165 per tick. Its magnitude is the heading-aligned arrival speed, producing the visible fast entry and gradual slowdown rather than reverse acceleration. |
| `0x0b` | Formation hold | Uses formation offset; desired speed 0 far away and 0.5x max nearby. Formation positioning is partial. |
| `0x0c` | Velocity match | Matches target velocity/heading within 0.525 px/tick, otherwise brakes on relative velocity. |
| `0x0d` | Timed formation hold | Copies leader heading/offset and can release after leader hold timer >30. |
| `0x0e` | Combat evade/brake | Mode-1-style brake aimed at target; distinct from state `0x0e`. |
| `0x0f` | Velocity-match pursuit | Brakes on relative velocity, then copies target velocity/position creep below 0.525 px/tick. Boarding/capture is deferred. |
| `0x10` | Evasive break | Heading is current ±135° (ship-instance parity), 1.5x thrust; returns to pursuit on alignment. |
| `0x11` | Boost to target | 2.75x thrust and 1.8x max desired speed; returns to pursuit close in or on a random roll. |
| `0x12` | Chase leader | Heads at leader plus a `15 * max-speed` polar offset; no leader means idle. |
| `0x13` | Scripted manoeuvre | Heads at asteroid-pool slot-0 target; thrust gate `turn_rate + 15°`. |
| `0x14` | Scripted velocity match | Ramps toward scripted velocity and creeps toward scripted position; weapon merge behaviour is partial. |
| `0x15` | Freeflight anchor | Heads to nearest freeflight anchor; no anchor means idle. Anchor model is not implemented. |
| `0x16` | Jump-out / retreat | Heads at stellar map coordinates but does not thrust. |
| `0x17` | Hypergate/wormhole entry handoff | No dedicated movement block | State `0x14` uses this marker after reaching the gate. The control-mode switch does no steering; transfer/vanish is completed by the surrounding jump path. |

## Relevant `ShipState` values

Offsets are original-binary `ShipState` offsets.  The C++ model intentionally
does not reproduce its memory layout; consult `game_state.hpp` for its current
field definitions and uncertainties.

| Offset | Field | Meaning / consumer |
|---:|---|---|
| `+0x30` | `ai_forward_thrust_cmd` | Effective raw thrust applied by the integrator. It is **not** a normalized 0–1 throttle. |
| `+0x34` | `ai_desired_speed` | Positive: normal accelerated target speed. Negative: heading-aligned physics override that moves toward zero. |
| `+0x4c` | `ai_maneuver_timer_ms` | Coast-through-reversal timer in normalized ticks despite the legacy `_ms` name: while positive, suppresses normal turn/thrust. It is not a braking timer. |
| `+0x50` | `ai_station_hold_timer` | Holds/approaches a station or leader; meaningful in states `0x0b`, `0x02`, and `0x03`. |
| `+0x64` | `cloak_fade_progress` | Cloak visual/visibility progress, 0–32; not weapon disable pressure. |
| `+0x68` | `ai_desired_heading_deg` | Integer degrees: 0 is up, values increase clockwise. |
| `+0x6c` | `ai_secondary_target_slot` | Polymorphic secondary ship slot or selected travel stellar resource ID. |
| `+0x70` | `primary_target_ship_slot` | Principal combat target. |
| `+0x8e` | `ai_evasive_heading_deg` | Stored `±135°` escape heading for mode `0x10`; name remains provisional. |
| `+0x92` | `jump_destination_stellar_id` | Stellar reached/selected for a possible jump. |
| `+0xa4` | `ai_mode_start_time_ms` | Wall-clock timestamp used by jump/formation timing. |
| `+0xba` | `ai_fire_trigger_latch` | Requests a fire of the selected weapon bank. |
| `+0xbd` | `ai_brake_to_boost_latch` | Unidentified producer; gates several close-range mode-`0x11` boosts. Provisional. |
| `+0xc8c8` | `ai_state_code` | High-level AI state. |
| `+0xc8ca` | `ai_control_mode` | Low-level movement order. |
| `+0xc8d8` | `cloak_transition_latch` | Signed cloak enter/exit request. |
| `+0xc8dc` | `velocity_match_target_ship_slot` | Velocity-match target; also changes effective-stat handling. |
| `+0xc8f8` | `ai_turn_bias_dir` | `-1/0/+1` visual banking direction produced by turning. |
| `+0xc906` | `formation_leader_ship_slot` | Formation leader; mode `0x12` idles when empty. |

## Decoded constants

Names belong in code/Ghidra when a constant has a single stable use; this table
collects values that explain AI behaviour and intentionally omits raw data
addresses.  Values came from instruction operand widths, not the prior byte
labels.

| Value | Uses |
|---:|---|
| `0.35 px/tick` | Arrival/stopped threshold; final brake-to-idle threshold. |
| `0.94`, `0.95`, `0.98` | Respectively aligned-slow brake, near-stop/hold damping, and stellar-arrival damping. |
| `(9 - min(turn_rate, 8)) * 8 + 32 px` | Per-axis state-`0x01` stellar arrival range. |
| `500 px`, `0.25x max speed` | Mode-`0x02` near-stellar gate and arrival cruise speed. |
| `165 px`, `82 px`, `123 px` | Combat boost/break-off proximity gates. |
| `±135°` | Mode-`0x10` evasive heading offset. |
| `1.5x`, `2.75x`, `1.8x` | Mode-`0x10` thrust, mode-`0x11` thrust, and mode-`0x11` desired-speed multipliers. |
| `0.525 px/tick` | Modes `0x0c`/`0x0f` velocity-match threshold. |
| `30` | Mode-`0x0d` leader hold-release timer gate. |
| `300..499 ticks` / `100..174 ticks` | Open-space / route-limited departure coast timer after stellar arrival. |

## Travel and lifecycle notes

- State `0x01` is local travel to a selected stellar or coordinate, not a
  universal jump point.  NPCs stop there and remain active.
- State `0x14` is the hypergate/wormhole entry state. It is reached when a
  selected travel stellar carries the restricted-lane flags; mode `0x02`
  approaches that stellar, then the state branch hands off through mode
  `0x17`. The handoff may clear velocity for bookkeeping, but has no visible
  brake/spin-up phase: the NPC enters/transfers and vanishes.
- For hypergates/wormholes, `StellarDef +0x28` is the Bible's `CustSndID`:
  ordinary stellars use it as ambient sound; gate lanes interpret valid 0–359
  values as emergence heading (otherwise random).
- State `0x08` does not have a fixed timeout in the state handler.  Its normal
  exit is the handler's target reset; `Ship_DeactivateVacantShipsAndTally`
  (0x0041ad50) clears vacant NPCs at system boundaries and tallies docked ships
  against their stellar before `System_RebuildInitialNpcAndMissionPopulation`
  immediately
  seeds the scattered `AvgShips` population. Per-tick maintenance subsequently
  replaces losses through the polar/hypergate arrival paths.

## Documentation practice

For new evidence, prefer a Ghidra plate comment on the function that owns the
decision and a field comment for a confirmed offset.  Keep code comments for
constants whose name clarifies an expression; update this document only when
the relationship is useful across states or modes.  Mark inferred names and
unimplemented side effects as **Provisional** / `TODO(decomp)` rather than
turning this reference back into a roadmap.
