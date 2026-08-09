# NPC Ship Behavior: Reimplementation Plan

Goal: make NPC ships do things -- move around the system, wander toward / land on
stellars, escort, acquire combat targets, and jump between systems.

This is a living plan. Phases are listed in the recommended execution order and
marked as they land. Current state: **Phases 0-2 done** (NPC movement physics
integrator + gravity-shield steer helper, both wired into the per-frame tick);
next up is **Phase 3** (the AI decision layer).

## Current architecture snapshot (start state)

- **Frame loop**: `NovaFrame_SpaceflightLoop` (src/game/spaceflight.cpp) ->
  `NovaFrame_TickSystems` (src/game/spaceflight.cpp) mirrors Ghidra
  `Frame_TickSystems` (0x004186b0). NPC per-frame work is split across stubs:
  - `Stub_AiRoutines` (scope 6, twice: targeting setup + per-ship AI) <- **where
    `Ship_UpdateShipAI` (0x00401000) lives**.
  - `Stub_HandleShips` (scope 4/5) <- **where `Ship_HandleShip` (0x00433050)
    lives** (per-ship integrate movement/targets/fire).
- **NPC population**: `NovaSystem_TickNpcSpawnMaintenance` +
  `NovaEncounter_SpawnFleetLeadShip` + `NovaDude_SpawnRandomDudeShipInSystem`
  (src/game/ship_spawn.cpp) already create active NPC `Ship`s in
  `GameState::ships_`.
- **Rendering**: `SpaceflightView::DrawNpcShips` already draws active NPC ships
  in the current system (base-only, no engine glow yet).
- **Ship data**: `Ship` struct (src/game/game_state.hpp) already has kinematics,
  AI slots (`ai_state_code`, `ai_control_mode`, `primary_target_ship_slot`,
  `ai_secondary_target_slot`, `jump_destination_stellar_id`, ...), and identity.
- **Player movement**: `NovaPlayer_IntegrateMovement` is a faithful integrator
  (turn, thrust, inertia-less special case, max-speed clamp).

### Key realization

The AI decision code (`Ship_UpdateShipAI`, the `Behavior0x01/0x02/0x03`
supervisors) is **already fully documented in Ghidra** with plate comments. The
missing piece is reimplementing it in C++ plus the NPC movement integrator
(`Ship_HandleShip` movement block). The clean-room `Ship` struct already carries
most field names with `+0x` offsets matching Ghidra `ShipState`, so this is
mostly unblocked porting work.

---

## Phase 0 -- Foundation (low effort, do first)

- [x] **NPC movement integrator**: port the movement block of `Ship_HandleShip`
      (0x00433050) into `NovaShip_IntegrateNpcMovement` (src/game/spaceflight.cpp,
      declared in spaceflight.hpp): continuous turn toward `ai_desired_heading_deg`
      at `round(Ship_ComputeShipMaxTurnRateDeg)`; three-branch `ai_desired_speed`
      thrust (free-coast / forward / reverse absolute-set, gated on
      `ai_forward_thrust_cmd != 0`); `reverse_speed_bias` coast-through-reversal
      timer; inertia-less pin (accel==0 && speed==0); position integration;
      shield/armor regen. Unit-tested (tests/movement_test.cpp). TODO(decomp):
      gravity-shield scalar-speed path, NPC outfit/status/government-effective
      stats.
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
- [ ] **Object crash / deactivation guards**: ports the validation prologue from
      `Ship_HandleShip` (the `is_active = false` returns for bad class_id,
      `tech_level == -9999`, etc.).

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
- [ ] **Shared AI helpers** the supervisors call (small, reusable). Derive the
      concrete list from callees of 0x00401000 / 0x00405590 (batch-decompile
      them). Likely: `Ship_SelectNearestDisabledShipForBoarding`,
      `Ship_FindBestAssistTargetForShip`, `Ship_IsShipFireRestricted`,
      target-validity checks, travel-stellar reacquire.
  - `Ship_UpdateAutoWeaponSelectionFromTarget` (0x00411540) -- ties into
    weapon-bank selection once firing exists.
- [ ] **Behavior supervisors**, in dependency order:
  - `Ship_UpdateShipAiBehavior0x01` (0x00402860) -- normal travel; **the "wander /
    travel to stellar / fall back to jump" behavior**.
  - `Ship_UpdateShipAiBehavior0x02` (0x00402bd0) -- basic "dude"/local ships:
    reacquire travel, promote nearby targets to attack.
  - `Ship_UpdateShipAiBehavior0x03` (0x00402e50) -- hostile attack/flee/regroup
    (pirates).
  - `Ship_UpdateShipAiBehavior0x03CaptureVariant` (0x004038b0) -- capture flavor
    (board disabled ships).
  - `Ship_UpdateShipAiCombatState` (0x00403de0),
    `Ship_UpdateShipAiAssistResponseBehavior` (0x004048a0),
    `Ship_UpdateShipAiAvailabilityBehavior` (0x00402980).
  - Respect `skip_heavy_ai` gating from `Ship_UpdateShipAI` where the original
    does.
- [ ] **`Ship_UpdateShipAI`** (0x00401000) -- the top-level per-ship AI entry
      that dispatches the supervisors. Wire it into `Stub_AiRoutines` (scope 6).
      Once live, NPC ships stop being static sprites and start chasing travel
      targets, escorting, and picking combat.

## Phase 4 -- Wander in the world (movement toward stellars)

- [ ] **Steering toward a travel target**: the AI writes `ai_secondary_target_slot`
      (travel stellar). Implement steering-to-map-coords so state 1 (travel to
      system, steers to map coords) actually moves the ship toward an in-system
      stellar. This is the "make them move around" milestone.

## Phase 5 -- Firing / combat (optional but natural next)

- [ ] **`Ship_UpdateShipDisableStateFromTraits`** (0x00411d00) and disable-state
      transitions -- needed for combat realism and capture.
- [ ] **Weapon/bank firing for NPCs**: extend the per-bank ammo/cooldown
      (currently player-centralized) onto `Ship` (+0xC8 row) and implement NPC
      fire selection from `active_weapon_bank_slot`. Larger; can be deferred.

## Phase 6 -- Land on stellars

- [ ] **NPC arrival/landing path**: the AI reaches its travel stellar. Key funcs:
      the arrival handling that deactivates/despawns a ship when it docks +
      `Ship_DeactivateVacantShipsAndTally` (0x0041AD50, partially in
      ship_spawn.cpp). In state 1 travel, arriving at the target stellar should
      despawn the NPC ("land"), and population maintenance respawns others.

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
| 6 | Land on stellars | Medium |
| 7 | Jump systems | High |
| 8 | Escort formations | Medium |

First visible, achievable milestone: **Phases 0-4** -- NPC ships spawn, move
around the system, and wander toward / land on stellars. Jump + full combat are
the later, bigger lifts.

## Cross-cutting reminders (per AGENTS.md)

- Keep `progress.csv` updated surgically (grep the address, edit in place) as
  each function gets reimplemented.
- Each reimpl function keeps a comment referencing its Ghidra address.
- Work one Ghidra function at a time; decompile + callees first before porting.
- NPC-only paths in `Ship_ComputeShipEffective*` and `Ship_ComputeShipMaxTurnRateDeg`
  are currently player-gated -- extend them (Phase 0) or NPC stats use defaults.
- Build both debug and release; format with clang-format; treat warnings as errors.
