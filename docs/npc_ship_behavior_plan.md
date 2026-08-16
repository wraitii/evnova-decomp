# NPC Ship Behavior: Reimplementation Plan

Goal: make NPC ships do things -- move around the system, wander toward / land on
stellars, escort, acquire combat targets, and jump between systems.

This is a living plan. Phases are listed in the recommended execution order and
marked as they land. Current state: **Phases 0-2 done** (NPC movement physics
integrator + gravity-shield steer helper, both wired into the per-frame tick);
**Phase 3 in progress** (AI decision layer): the ship_ai module dispatches the
behavior supervisors + state machine + controls bridge each frame. Behaviors
0x02/0x03 now acquire nearest same-system hostile contacts, promote them into
attack/assist states, and steer through combat control modes 5/6/7/8/0xc/0xf;
mission, weapon, and disable side effects remain deferred. The
**wander/travel milestone (Phases 3+4) is live** -- NPCs pick a random adjacent
travel stellar, steer toward it, and cycle to the next on arrival. NPC movement
uses each ship's real class stats; the thrust-units bug that made them ~50x too
fast is fixed (see the Diagnosis section below). **Phase 6 is done**: the
faithful `Ship_DeactivateVacantShipsAndTally` port sweeps vacant NPCs (with
stellar present-ship tallies) at every landing/jump system boundary and the
population maintenance reseeds -- the original keeps arrived ships active and
removes them only at system boundaries (see Phase 6).

## Diagnosis (2025-08-09): NPC ships do not use their ship's stats

Root cause found while chasing "NPCs move too fast". The NPC movement chain is
`NovaAi_ApplyControls` (bridge) -> `NovaShip_IntegrateNpcMovement` (integrator).
Both are faithful in shape but the bridge writes **`ai_forward_thrust_cmd = 1.0`**
where the original `Ship_ApplyShipAiControls` writes the ship's **raw effective
thrust value** (`Ship_ComputeShipEffectiveThrust` = `2*accel/10000` px/tick^2,
e.g. ~0.02 for accel=100). `Ship_HandleShip` forms the per-frame velocity step as
`ai_forward_thrust_cmd * frame_time`, so the port accelerated NPCs at ~1 px/tick
per frame instead of ~0.02 -- **~50x too fast**, hitting top speed in ~8 frames
instead of ~400. The `eff_thrust` the integrator computes was only used for the
engine-glow logic, never for thrust.

### Decoded movement constants (now typed in Ghidra, pre-commented)

The `_DAT_005750xx` globals were byte-mislabeled; values decoded from the raw
bytes at 0x00575000..0x00575200 (little-endian; float vs double from the actual
`FADD/FMUL/FCOMP float|double ptr` instructions):

| addr | type | value | meaning |
|------|------|-------|---------|
| 0x575008 | double | 0.0 | zero sentinel for AI fields (`FLOAT_ZERO`) |
| 0x575010 | double | 100.0 | velocity->bearing precision scale (modes 1/0xb/0xf/0xe) |
| 0x575018 | float | 1.0 | turn-alignment allowance addend (modes 1/0xb/0xf) |
| 0x57501c | float | 100.0 | hold/approach proximity gate (modes 6/0xb) |
| 0x575038 | double | 0.5 | reduced thrust factor (mode-1 slow brake, mode-0xb close) |
| 0x575068 | double | 8.0 | state-1 arrive-range turn-rate cap |
| 0x575070 | double | 9.0 | state-1 arrive-range base |
| 0x575078 | double | 32.0 | state-1 arrive-range offset; range = (9.0-min(turn,8))*8+32 |
| 0x575080 | double | 0.35 | "moving" / arrival-stopped velocity threshold (px/tick) |
| 0x575088 | double | 0.98 | state-1 arrival velocity damp |
| 0x5750b0 | float | 165.0 | combat proximity gate (modes 5/6/0x10/0x11) |
| 0x5750c0 | float | 30.0 | mode-0xd hold-release timer gate |
| 0x5750d4 | float | 15.0 | mode-0x12 chase speed factor / mode-6 alignment addend |
| 0x5750f0 | double | 0.95 | damp when nearly stopped (mode-1/0xe) / hold-match |
| 0x5750f8 | double | 0.94 | mode-1 aligned-slow + state-9 damp |
| 0x575100 | float | 5.0 | mode-2 (travel) turn-alignment addend |
| 0x575104 | float | 500.0 | mode-2 "close to stellar" per-axis gate (px) |
| 0x575108 | double | 0.25 | mode-2 arrival cruise fraction of eff_max_speed |
| 0x575110 | float | 20.0 | mode-5/0x12 turn-alignment addend |
| 0x575118 | double | 1.75 | mode-1 "still fast" threshold (px/tick) |
| 0x575120 | float | 3.0 | mode-3 addend; mode-6/7/0x10/0x11 turn multiplier |
| 0x575128 | float | 82.0 | mode-6/7 break-off per-axis gate |
| 0x575130 | double | 1.5 | mode-0x10 evasive thrust factor |
| 0x575138 | float | 135.0 | mode-6 evasive heading offset (deg) |
| 0x575140 | double | 2.75 | mode-0x11 boost thrust factor |
| 0x575148 | double | 1.8 | mode-0x11 cruise factor (eff_max_speed * 1.8) |
| 0x575150 | float | 4.0 | mode-7 turn-alignment multiplier |
| 0x575158 | double | 0.525 | mode-0xf velocity-match threshold |
| 0x575160 | float | 200.0 | mode-0xb/9 per-axis distance gate |

### Secondary fidelity gaps (found and fixed 2025-08-09)

All five below are fixed in the current code (see "Fixes landed"); kept as a
record of the bugs found while chasing the speed issue.

1. **Mode 2 (travel) does not throttle at arrival**: original writes
   `ai_desired_speed = 0` when far (coast at max-speed clamp) and
   `eff_max_speed * 0.25` within 500 px of the stellar; the port always writes
   `max_speed`.
2. **Mode 2 has no alignment gate**: original only thrusts when the hull is
   within `turn_rate + 5.0` deg of the bearing; the port always thrusts.
3. **Mode 1 (damp/brake) is a no-op in the port**: original steers to the
   reverse bearing and writes thrust = `eff_thrust` (or `*0.5` when slow, with a
   0.94 damp), dropping to 0.95 damp + idle mode once below 0.35 px/tick. The
   port writes `desired = -max(1,max_speed)` with `thrust_cmd = 0`, which the
   integrator's `ai_forward_thrust_cmd != 0` gate skips entirely -- ships in
   state 6/2/0x16 never slow down.
4. **State-1 arrival constants were provisional**: the port used
   `(10-turn)*50+20` range, `0.9` damp, `20` px/tick stopped threshold; the
   original is `(9.0-min(turn,8))*8+32`, `0.98`, `0.35`.
5. **Top-of-function desired-speed reset missing**: the original resets
   `ai_desired_speed = eff_max_speed` whenever it is `>= 0` at entry; mode 1
   relies on this (it never writes desired itself).

### Fixes landed (2025-08-09)

- `NovaAi_ApplyControls` writes `ai_forward_thrust_cmd = eff_thrust` (not 1.0)
  in every thrusting mode, and ports the mode-2 alignment gate + arrival
  fraction, the mode-1 braking ladder, and the entry desired-speed reset.
- `NovaAi_UpdateShipState` arrival uses the decoded constants
  (`(9.0-min(turn,8))*8+32` range, `0.98` damp, `0.35` stopped threshold).
- `NovaShip_IntegrateNpcMovement` keeps `thrust_step = ai_forward_thrust_cmd *
  elapsed_ticks` (now correct since the bridge writes the thrust value); the
  coast branch also adopts the original's `ai_station_hold_timer <= 0` gate.

### AI control-mode reference (`ai_control_mode`, +0xC8CA)

Decoded from `Ship_ApplyShipAiControls` (0x00408150). Modes 0-4/0xd/0xb/9 are
ported faithfully; 5-0x17 are provisional cruise stand-ins until the
combat/formation/jump systems land (Phase 5-7).

| Mode | Name | Behavior (original) |
|------|------|---------------------|
| 0x00 | Idle | no thrust, hold heading; auto-weapon refresh |
| 0x01 | Brake-to-stop | steer to reverse of velocity bearing; full thrust -> 0.5x + 0.94 damp -> 0.95 damp + idle (<0.35 px/tick); g-shield: -0.5x thrust |
| 0x02 | Travel to stellar | thrust only when aligned <= turn_rate+5 deg; desired 0 (coast at max clamp) far, 0.25*max within 500 px/axis |
| 0x03 | Approach centre | point away from centre; thrust when aligned <= turn_rate+3 deg |
| 0x04 | Jump spin-up | point away from centre; ramp hold timer; jump bookkeeping when timer >= duration/class (Phase 7) |
| 0x05 | Pursue target | formation offset (leader); steer at target; thrust when aligned <= turn_rate+20 deg; close (<165 px) -> 0x11 |
| 0x06 | Combat pursuit | predictive/straight aim; thrust gate turn_rate+15 deg, 2nd gate turn_rate*3 -> direct-fire; break-off (135 deg, 82 px) -> 0x10; g-shield speed-matching |
| 0x07 | Combat strafe | aim (guided predictive); weapon select when aligned turn_rate*3; thrust gate turn_rate*4 |
| 0x08 | Escort follow | steer at lead; thrust when aligned <= turn_rate+1 deg; outside escort half-span creep at 10x thrust; inside -> launch/handoff |
| 0x09 | Hold at distance | steer at target (>200 px) or velocity-bearing match; thrust when aligned <= turn_rate+1 deg |
| 0x0b | Formation hold | formation offset; steer at target / velocity-bearing; thrust <= turn_rate+1 deg; desired 0 far, 0.5*max within 100 px |
| 0x0c | Velocity-match | match target vel/heading within 0.525 px/tick; heading-lerp window 25/|vel| |
| 0x0d | Formation hold (timer) | copy leader heading/offset; leader timer >30 -> release to default behavior/mode 4; aligned <=11 deg -> damp 0.95, desired -4.0 (reverse), timer ramp |
| 0x0e | Evade/break | brake like mode 1 aimed at target; g-shield -thrust; slow -> damp 0.95 + steer at target |
| 0x0f | Velocity-match pursuit | brake on RELATIVE velocity (mode-1 style); <0.525 -> match target vel + damp; boarding/capture on arrival |
| 0x10 | Evasive break | heading = current +/-135 deg (ship_instance_id parity); thrust 1.5x, desired 0; -> 0x6 when aligned |
| 0x11 | Boost to target | thrust 2.75x, desired 1.8*max (over-speed); formation offset; -> 0x6 on alignment |
| 0x12 | Chase leader | head at leader + 15*max polar offset; thrust <= turn_rate+20 deg; guided-weapon select in state 4 |
| 0x13 | Scripted maneuver | steer at scripted target; thrust <= turn_rate+15 deg |
| 0x14 | Scripted velocity-match | ramp vel to scripted target vel at 1.5x thrust; positional creep (+/-150/80 px); weapon lead-aim |
| 0x15 | Jump-in | pick nearest freeflight anchor; steer at it; thrust <= turn_rate+15 deg, else damp 1.75x thrust (Phase 7) |
| 0x16 | Jump-out/retreat | steer at stellar map coords; weapon select when aligned <= turn_rate+15 deg |
| 0x17 | (no block) | thrust stays 0 -- effectively hold (port parks it too) |

Notes: 0x575118 doubles as mode-1's 1.75 px/tick threshold AND mode-0x15's
1.75x damp factor. 0x0a is unused/reserved (no block in the original). The
port's mode 1/2/3/4/0xd implementations in `NovaAi_ApplyControls` mirror the
table; mode 4/0xd keep placeholder hold behavior until jump/formation land.

## Current architecture snapshot

- **Frame loop**: `NovaFrame_SpaceflightLoop` (src/game/spaceflight.cpp) ->
  `NovaFrame_TickSystems` (src/game/spaceflight.cpp) mirrors Ghidra
  `Frame_TickSystems` (0x004186b0). NPC per-frame work is split across stubs:
  - `Stub_AiRoutines` (scope 6, twice: targeting setup + per-ship AI) <- **where
    `Ship_UpdateShipAI` (0x00401000) lives** -- now wired to
    `NovaAi_UpdateShipAI` (src/game/ship_ai.cpp).
  - `Stub_HandleShips` (scope 4/5) <- **where `Ship_HandleShip` (0x00433050)
    lives** (per-ship integrate movement/targets/fire).
- **NPC population**: `NovaSystem_TickNpcSpawnMaintenance` +
  `NovaEncounter_SpawnFleetLeadShip` + `NovaDude_SpawnRandomDudeShipInSystem`
  (src/game/ship_spawn.cpp) already create active NPC `Ship`s in
  `GameState::ships_`.
- **Rendering**: `SpaceflightView::DrawNpcShips` already draws active NPC ships
  in the current system, including a per-class **engine-glow layer** (loaded
  from `GlowImageID` in `ShipClassSprite`, drawn over the hull with a
  thrust-driven alpha read from `Ship::engine_glow_level`, which
  `NovaShip_IntegrateNpcMovement` drives faithfully to Ghidra
  `Ship_HandleShip`'s `field_0xc8d4`).
- **Ship data**: `Ship` struct (src/game/game_state.hpp) already has kinematics,
  AI slots (`ai_state_code`, `ai_control_mode`, `primary_target_ship_slot`,
  `ai_secondary_target_slot`, `jump_destination_stellar_id`, ...), and identity.
- **Player movement**: `NovaPlayer_IntegrateMovement` is a faithful integrator
  (turn, thrust, inertia-less special case, max-speed clamp).

The AI decision code (`Ship_UpdateShipAI`, the `Behavior0x01/0x02/0x03`
supervisors) is fully documented in Ghidra with plate comments, and the
clean-room `Ship` struct carries the `ShipState` field names/offsets, so the
remaining porting is largely unblocked.

---

## Phase 0 -- Foundation (low effort, do first)

- [x] **NPC movement integrator**: port the movement block of `Ship_HandleShip`
      (0x00433050) into `NovaShip_IntegrateNpcMovement` (src/game/spaceflight.cpp,
      declared in spaceflight.hpp): continuous turn toward `ai_desired_heading_deg`
      at the raw `Ship_ComputeShipMaxTurnRateDeg` rate (continuous; the player
      keyboard path rounds instead); three-branch `ai_desired_speed`
      thrust (free-coast / forward / reverse absolute-set, gated on
      `ai_forward_thrust_cmd != 0`); `reverse_speed_bias` coast-through-reversal
      timer; inertia-less pin (accel==0 && speed==0); position integration;
      shield/armor regen; engine-glow level (`ShipState +0xc8d4`: full-burn 32 /
      low-throttle 24 / fade-to-zero, turn-bias +2 -- the +2 bank bump is driven
      by `ai_turn_bias_dir` (+0xC8F8), written by the integrator's turn block
      from the turn direction, matching Ship_HandleShip's tilt-derived signal).
      Unit-tested
      (tests/movement_test.cpp). TODO(decomp): NPC outfit/status/government-
      effective stats (the `Ship_ComputeShipMaxTurnRateDeg` 1.0-deg/frame NPC
      floor clamp is ported but inert until disable/status damping lowers a
      clean base below its floor).
- [x] **Add missing `Ship` AI fields** on the clean-room struct
      (src/game/game_state.hpp): `ai_forward_thrust_cmd` (+0x30),
      `ai_desired_speed` (+0x34), `ai_desired_heading_deg` (+0x68),
      `ai_station_hold_timer` (+0x50), `ai_mode_start_time_ms` (+0xA4),
      `ai_turn_bias_dir` (+0xC8F8), `ai_fire_trigger_latch` (+0xBA),
      `reverse_speed_bias` (+0x4C).

## Phase 1 -- Ship slot iteration (wire the physics)

- [x] **Tick active ships each frame**: `Stub_HandleShips` (scope 4/5 of
      `NovaFrame_TickSystems`) now iterates active, non-player ships in the
      current system and calls `NovaShip_IntegrateNpcMovement` with the
      per-frame elapsed ticks (mirroring the player's cadence normalization).
- [x] **Object crash / deactivation guards**: `Stub_HandleShips` now ports the
      `Ship_HandleShip` validation prologue: deactivates any active NPC whose
      class id falls outside [0,0x2ff] or whose class carries the -9999
      `kShipClassNonexistentTechLevel` (0xd8f1) sentinel, and range-resets
      drifted slot fields (faction / dude_class / mission_ship / ai_target_ship
      / target_stellar_object / mission_fleet / primary_target) to -1
      (including Ghidra's quirk that the out-of-range `target_stellar_object_id`
      check resets `ai_target_ship_slot`). Exposed for tests via
      `NovaShip_TickNpcShips`; unit-tested (tests/movement_test.cpp).

## Phase 2 -- Steering math helpers (low effort)

- [x] **`Math_AddPolarVelocity`** (0x0043b4a0) and
      **`Math_AddPolarVelocityWithClamp`** (0x0043b4e0). The clamped variant is
      shared (`NovaPlayer_AddPolarVelocityClamped`, promoted in Phase 1); the
      unclamped polar add is inlined in the NPC integrator's reverse path and
      the steer helper.
- [x] **`Ship_SteerVelocityTowardShipHeading`** (0x0043b020) as
      `NovaShip_SteerVelocityTowardShipHeading`: the momentum/turn integrator
      for gravity-shield ships (scalar `speed` snapped toward heading*speed,
      then relaxed toward the prior velocity at `eff_thrust * 4.0 * frame_time`
      per axis). Added `NovaShip_HasGravityShield` (NPC branch of
      `Outfit_ShipHasGravityShieldOutfit` 0x0046df70: class `flags_secondary`
      bit 0x40, excluding ai_control_mode 0x0c) and wired the gravity-shield
      scalar-speed thrust path into `NovaShip_IntegrateNpcMovement`. Unit-tested
      (tests/movement_test.cpp).

## Phase 3 -- AI decision layer (high effort, the content)

The Ghidra plates already enumerate all state semantics. Port one
Behavior/supervisor at a time. Each is a self-contained state-machine update.

- [ ] **Dispatcher**: reimplement `Ship_UpdateShipAiState` (0x00405590). The
      plate defines states 0-0x16 (idle/travel/attack/pursue/escort/board/
      drift/jump/disengage/defunct etc.) and the companion `ai_control_mode`
      writer. This is the heart.
  - **In progress**: `NovaAi_UpdateShipState` (src/game/ship_ai.cpp) ports the
    core travel (1/0x14/2), hold (0xb), pursuit(5)/assist(10)/escort(7), drift
    (0xe), disengage(0x15/8), defunct (0x16), and partial attack/assist
    transitions (3/4/0xc/0xd). States 3/4 now select target-bearing pursuit
    modes 5/6; weapon selection, disable transitions, formation offsets, and
    HUD/mission flavor remain deferred. The travel-arrival `reverse_speed_bias`
    band matches Ghidra (300..499 / 100..174); the jump-fallback gate now uses
    the ship's own class fuel
    (`NovaTravel_CanShipInitiateJumpSequence`, mirroring
    `Stellar_CanShipInitiateJumpSequence` 0x00415b80, not the player-only
    `NovaTravel_CanStartJump`). The `_DAT_005750xx` turn/arrive constants are
    now decoded and typed in Ghidra (see the Diagnosis section); the state-1
    arrival uses the real range/damp/threshold values.
- [ ] **Shared AI helpers** the supervisors call (small, reusable). Derive the
      concrete list from callees of 0x00401000 / 0x00405590 (batch-decompile
      them). Likely: `Ship_SelectNearestDisabledShipForBoarding`,
      `Ship_FindBestAssistTargetForShip`, `Ship_IsShipFireRestricted`,
      target-validity checks, travel-stellar reacquire.
  - **Started**: `NovaAiShip_IsDestroyed` (0x004688e0, faithful),
    `NovaAiShip_IsFireRestricted` (0x004687b0, partial),
    `NovaAi_EnterState2ClearPrimaryTarget` (0x00410670, faithful),
    `NovaAi_SelectRandomAdjacentTravelStellar` (0x0040c790, partial),
    all in src/game/ship_ai.cpp. `NovaAi_AcquirePrimaryTarget` (0x0040e020)
    retains the executable same-system hostile/engaged-contact core; mission/
    scripted target priority, perceived-strength weighting, and reputation/
    policy gates remain deferred.
  - **Implemented partially**: `Ship_CanShipInterceptCurrentPrimaryTarget`
    (0x00410f20) -- exact active/system/mass/relative-bearing and class-speed
    gate, with the available guided-bank walk.
  - **Implemented partially**: `Ship_FindBestAssistTargetForShip`
    (0x00412030) -- exact 64-slot lowest-positive score scan and category/range
    score; mission/escort command context remains provisional.
  - **Implemented partially**: `Ship_UpdateAutoWeaponSelectionFromTarget`
    (0x00411540) -- stale-target cleanup and class-loadout bank selection are
    wired after the state/control pass; NPC firing and full turret priority
    remain Phase 5 work.
- [ ] **Behavior supervisors**, in dependency order:
  - **Started**: `Ship_UpdateShipAiBehavior0x01` (0x00402860) -- normal travel
    (**the "wander / travel to stellar" behavior**, faithful port:
    `NovaAi_UpdateBehavior0x01`).
  - **Implemented partially**: `Ship_UpdateShipAiBehavior0x02` (0x00402bd0) --
    basic local-ship travel plus hostile-contact promotion.
  - **Implemented partially**: `Ship_UpdateShipAiBehavior0x03` (0x00402e50) --
    hostile target acquisition, validation, pursuit, and travel fallback.
  - **Shared fallback only**: `Ship_UpdateShipAiBehavior0x03CaptureVariant`
    (0x004038b0) -- capture-specific disabled-ship selection is deferred.
  - **Shared target path only**: `Ship_UpdateShipAiCombatState` (0x00403de0),
    `Ship_UpdateShipAiAssistResponseBehavior` (0x004048a0),
    `Ship_UpdateShipAiAvailabilityBehavior` (0x00402980).
  - Respect `skip_heavy_ai` gating from `Ship_UpdateShipAI` where the original
    does.
- [ ] **`Ship_UpdateShipAI`** (0x00401000) -- the top-level per-ship AI entry
      that dispatches the supervisors. **In progress**: `NovaAi_UpdateShipAI`
      (src/game/ship_ai.cpp) implements the dispatch + state machine + controls
      bridge, and is wired into `Stub_AiRoutines` (scope 6). The
      movement bridge `Ship_ApplyShipAiControls` (0x00408150, `NovaAi_ApplyControls`)
      turns `ai_control_mode` into the movement fields the Phase 0 integrator
      consumes, so NPCs now visibly wander toward travel stellars and pursue
      reconstructed hostile contacts. Unit-tested (tests/ship_ai_test.cpp).

## Phase 4 -- Wander in the world (movement toward stellars)

- [x] **Steering toward a travel target**: the AI writes `ai_secondary_target_slot`
      (travel stellar). `NovaAi_ApplyControls` (Ship_ApplyShipAiControls 0x00408150)
      + the Phase 0 integrator now steer a state-1 ship toward the stellar's map
      coordinates (control mode 2), and on arrival it damp-to-stops / re-arms the
      coast-through-reversal timer and re-enters idle so the wander supervisor
      picks the next leg. This is the "make them move around" milestone; the
      eventual despawn/land + population respawn awaits Phase 6.

## Phase 5 -- Firing / combat (optional but natural next)

- [ ] **`Ship_UpdateShipDisableStateFromTraits`** (0x00411d00) and disable-state
      transitions -- needed for combat realism and capture.
- [ ] **Weapon/bank firing for NPCs**: extend the per-bank ammo/cooldown
      (currently player-centralized) onto `Ship` (+0xC8 row) and implement NPC
      fire selection from `active_weapon_bank_slot`. Larger; can be deferred.

## Phase 6 -- Land on stellars

- [x] **State-1 travel arrival settle** (ported with Phase 3/4): on arrival the
      ship damp-stops at the stellar, records `jump_destination_stellar_id =`
      the arrived stellar, re-enters state 0 and arms the coast-through-reversal
      timer (300..499 ms open-space, 100..174 ms route-limited). Then the
      wander supervisor (Behavior 0x01/0x02, state 0 "still at point") either
      routes the ship toward the system centre for a jump spin-up (state 2,
      `Stellar_CanShipInitiateJumpSequence` fuel gate) or parks it at the
      stellar (state 6). **The original does NOT despawn NPCs on arrival** --
      ships stay active and parked/wandering until the player leaves the system.
- [x] **`Ship_DeactivateVacantShipsAndTally` (0x0041AD50)** -- faithful port
      (`NovaShip_DeactivateVacantShipsAndTally`, src/game/ship_spawn.cpp): the
      64-slot scan deactivates every "vacant" NPC -- i.e. all ships except
      non-fire-restricted ships actively engaging the player (behavior > 4,
      `ai_target_ship_slot == 0`, not docked, no mission fleet, flag == 0).
      Docked ships (`target_stellar_object_id` set) are tallied into their
      stellar's `present_ship_count` (capped at `max_ship_count`) before the
      slot is cleared; mission ships would tally into their fleet's current-
      ship count (mission fleets not reconstructed, TODO(decomp)).
- [x] **Wired at both system-boundary paths**: the normal-arrival dock path
      (Stellar_ProcessTravelAndLanding 0x00457580) and the player jump
      completion (NovaMainLoop_Run 0x00486880 system-entry latch), each
      followed by `NovaSystem_TickNpcSpawnMaintenance` to reseed toward
      `System.avg_ships`. Before this wiring, a player jump left the old
      system's ships stale-but-active so they reappeared when jumping back.

**Phase 6 correction (2025-08-09)**: the original plan claimed arriving NPCs
"despawn (land)". Ghidra shows the opposite: `Ship_UpdateShipAiState` state-1
arrival keeps the ship active (stop + record + re-idle), and NPCs are removed
from the world only by the vacant-ship cleanup at the system boundary. The
visible "ships disappear when they reach a planet" effect is these parked
ships leaving the player's view (they stop at the stellar; most then head to
the system centre for a jump spin-up, Phase 7), plus the population reseed at
every landing/jump.

## Phase 7 -- Jump between systems (high effort, save for last)

- [ ] **Hyperspace jump behavior**: state 0x14 ("traveling/jumping to a system",
      sets `jump_destination_stellar_id`, propagates to escorts). Port:
  - jump-in / jump-out positioning (spin-out AI 0x08 / jump-in AI 0x15,
    deferred in `NovaEncounter_SpawnFleetLeadShip`);
  - `Ship_TurnShipTowardHeading` (0x0044c8d0) hyperspace alignment path
    (partially decomposed);
  - state-machine checks that terminate travel and trigger a jump (from
    `Ship_UpdateShipAiBehavior0x01`).
  - Most complex: involves system change / despawn-here / respawn-there and
    escort propagation.

## Phase 8 -- Escorts & formations (polish)

- [ ] **`Ship_UpdateEscortFormations`** (0x00413990) +
      `Ship_MoveShipTowardFormationOffset` (0x00414390) -- makes escort fleets
      fly in formation around their lead.

---

## Prioritized roadmap

| Phase | Deliverable milestone | Relative effort |
|-------|----------------------|-----------------|
| 0 | NPCs move/coast under physics | Low |
| 1 | Active-ship tick loop live | Low |
| 2 | Steering math helpers | Low |
| 3 | AI decision layer | High |
| 4 | Wander toward stellars | Medium |
| 5 | Combat/fire | High |
| 6 | Land/settle on stellars + system-boundary cleanup | Medium |
| 7 | Jump systems | High |
| 8 | Escort formations | Medium |

**Phases 0-4 and the Phase-6 system-boundary cleanup are live**: NPC ships
spawn, move around the system, wander toward travel stellars, and settle there;
on every landing/jump the vacant-ship cleanup sweeps the cohort and population
maintenance reseeds it (so NPCs leave the world at system boundaries, exactly
like the original). Jump (Phase 7) and full combat (Phase 5) are the later,
bigger lifts.

## Cross-cutting reminders (per AGENTS.md)

- Keep `progress.csv` updated surgically (grep the address, edit in place) as
  each function gets reimplemented.
- Each reimpl function keeps a comment referencing its Ghidra address.
- Work one Ghidra function at a time; decompile + callees first before porting.
- NPC movement stats are per-ship-class in this build: the Phase 0 integrator
  and `NovaAi_ApplyControls` share `NovaShip_ComputeEffectiveStats` (the NPC
  branch of `Ship_ComputeShipEffectiveThrust`/`Ship_ComputeShipEffectiveMax-`
  `Speed`), i.e. class `accel`/`speed`/`turn_rate` scaled by the government
  `combat_rating_scale` (faction != -1; speed/accel only, not turn). The
  remaining NPC-branch gaps are the per-ship **skill_variance_scale** (+0x40,
  decoded formula: `(NovaRandom_Range(pct*2+1) + (100-pct)) * 0.01` from
  `ShipClass_ComputeShipClassSkillVarianceScale` 0x0046b870; needs a `Ship`
  field + spawner init) and the status-effect/disable damping (needs
  combat/status state, TODO(decomp)). The `Ship_ComputeShipMaxTurnRateDeg`
  1.0-deg/frame NPC floor clamp is ported but inert until damping lowers a
  clean base below its floor. Keep these in sync if NPC outfit/status
  modelling is added.
- Build both debug and release; format with clang-format; treat warnings as errors.
