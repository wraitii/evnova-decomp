# Weapon mode code mapping (EVN `wëap` Guidance)

Source baseline: EVN Wiki `wëap` guidance table.

Reference: https://evn.fandom.com/wiki/W%C3%ABap

## Canonical guidance values (wiki)

- `-1` unguided projectile
- `0` beam
- `1` homing weapon
- `2` unused
- `3` turreted beam
- `4` turreted unguided projectile
- `5` freefall bomb
- `6` freeflight rocket
- `7` front-quadrant turret
- `8` rear-quadrant turret
- `9` point-defense turret
- `10` point-defense beam
- `99` carried ship

## Observed in decompile (ship firing path)

From `NovaGameplay_FireShipWeapons` + `NovaGameplay_SpawnShotFromWeapon` + helpers:

- Directly branched/active in code: `-1, 0, 1, 3, 4, 5, 6, 7, 8, 9, 99`
- Not yet observed in this path: `2` (unused per wiki), `10` (PD beam; may be handled elsewhere)

## Mode-6 freeflight rocket — predictive lead at fire (~confirmed)

Contrary to "dumb straight-line" intuition, mode `-1`/`4`/`6`/`7`/`8`/`9` projectiles are **lead-aimed at fire time**. The three
lead-aim helpers (`NovaGameplay_AimWeaponPredictive`/`AimWeaponLeadVelocity`/`AimStellarBatteryShot`) return a predicted
intercept bearing; the returned bearing is stored to the ship desired-heading or shot heading, then jittered by `shot_random_spread`.

Mode-6 specifically uses a two-regime travel-time model with three double constants:

- `k_mode6_rocket_lead_threshold_factor` (19.59): branch threshold at `distance == speed*19.59`
- `k_mode6_rocket_near_speed_factor` (0.316): near target `t_left = distance/(speed*0.316)` (≈3.16x naive lead — rocket not yet at speed)
- `k_mode6_rocket_far_time_bonus` (2.06667): far target `t_left = (distance - speed*19.59)/speed + 2.06667` (small spool bonus)

In-flight homing is separate and only activates for **mode 1** (`NovaGameplay_UpdateShotGuidance` returns early for mode 9 and
tracks/retargets only when mode==1). So mode-6 rockets do NOT chase after launch; they fly straight along the fired lead.
The "chasing" missiles are mode 1 homing weapons.

## Homing / seek lock vs jamming (mode 1) — mapped (revised 2024 against 0x00431530/0x0041fd30 disasm+decomp)

Guided (mode-1) homing shots carry **four independent seek channels** `ShotState.lock_quality_0..3` (+0x38). The four channels
are the EVN Bible's four jamming types. **At spawn (Shot_SpawnShotFromWeapon):** `lock_quality[ch] = 0` when
`WeaponDef.jam_vuln[ch] < 1`, else `Random(JamVuln[ch]+1)` — i.e. a per-channel roll in `[0, JamVuln]`. Despite the name,
lock_quality is really a **jam-vulnerability roll**: higher JamVuln ⇒ larger roll ⇒ easier to defeat.

**Defense (target ship):** `ShipState.jamming_score_1..4` (+0xC926), lazily computed by `Ship_GetShipJammingScore`
(0x00464810): base = owning govt `InhJam1-4` (GovtDef +0x56) via the ship class's `InherentGovt`; outfit ModType opcodes
`0x21..0x24` (Bible "Jamming Type 1-4") add their ModVal — the player sums every owned outfit, an NPC sums each mounted
stock outfit once (not per count); NPC ships whose faction has govt flag `0x80` are halved; clamped 0-100; cached per ship
(reseeded -1 at ship-slot allocation).

**Lock gate (Shot_UpdateShotGuidance, per frame, first defeated channel only):** the channel is **DEFEATED** while
`jamming_score[ch] > 100 - lock_quality[ch]`. On defeat: turn rate → 0 (missile flies straight) unless Seeker flag
`0x0010` ("Turns away if jammed", flags_quaternary — the doc previously mislabeled it flags_tertiary) negates it
(`turn *= -1.0`, k_jammed_turn_sign_f32 @0x575350); with Seeker `0x8000` ("May attack parent ship if jammed") a 1/500 roll
retargets the shot onto its owner. A channel with `lock_quality == 0` is never defeated, so weapons with no JamVuln set
home unconditionally.

**Other seeker-flag behaviors (flags_quaternary / Bible Seeker, verified against 0x00431530):**
- `0x0002` decoyed by asteroids: 1/10 per frame, an active asteroid within 200 px on both axes and within 16 deg of the
  shot heading latches `retarget_cooldown = 1` (target_ship_slot becomes an asteroid pool index).
- `0x0008` confused by sensor interference: at spawn, `Random(100 / frame_scale) + 1 <= SystemDef.interference (+0x96,
  payload +0x6c)` latches `retarget_cooldown = 999` — the weave state (heading zig-zags on a 300-frame phase,
  `g_license_check_frame_counter % 300 < 150` selects the direction; 1/1000 owner-retarget escape with 0x8000).
- `0x4000` loses lock if target not directly ahead: within 250 px on both axes with the target more than 45 deg off the
  nose, the lock drops (target = -1).
- `retarget_cooldown == 998` is the inert latch Shot_HandleShot sets when a state-0 shot's target dies.

**Turn rate:** `WeaponDef.guided_turn_rate` (+0x58 float) = wëap payload +0x6a (Bible "GuidedTurn") * 0.1, in degrees per
tick. Homing only runs while remaining life > frame_scale * 30 (k_guidance_life_gate_f64 @0x5754c0) — guided weapons fly
straight over roughly their final second; the 999/1 states use a bare 15-tick gate (0x575400).

**Mode-6 rockets** do not chase: they fire along the lead bearing (Ship_AimWeaponPredictive two-regime model) and then
blend velocity toward the heading each frame: `vel = (vel*94 + polar(heading,speed)*5) * 0.01` (0x57540c/0x575408/0x575368).
Mode-6 shots launched by the player keep pure inherited velocity at spawn (no polar add); NPC bays get the full polar
vector. Player mode-5 bombs keep inherited velocity * 0.8.

Point-defense mode 9 shots are targetless and fly straight — `Shot_UpdateShotGuidance` returns immediately for mode 9;
`NovaWeapon_SelectTurretTargetWithinArc` (0x0043a310) chooses the nearest eligible inbound guided shot before considering
PD-targetable attacking ships, and applies straight or predictive lead at fire time. Mode 10 applies beam PD damage to
the guided shot's durability counter.

## Linked submunitions (Bible SubCount/SubType/SubTheta/SubLimit)

`Shot_SpawnLinkedShotsOnImpact` (0x00420d30) is the only consumer of the `wëap` linkage fields. Resource mapping
(loader 0x004bd3c0): `SubCount` +0x3e → `WeaponDef.range_link_gate`, `SubType` +0x40 → `range_link_weapon_id`
(zero-based after the loader subtracts 0x80; out-of-range becomes -1), `SubTheta` +0x42 → `range_link_spread`
(previously Ghidra `field_0x7e`; resource -1 is normalized to 0), `SubLimit` +0x44 → `range_link_extra_count`.

The spawner runs on a **collision impact only when the blast-proximity pass (Shot_ResolveCollisions 0x00437e20) calls
`Shot_ResolveShotCollisionHit` (0x00437780) with its trailing flag = 1; the direct sprite-contact pass
(`Ship_HandleSpritePairCollision` 0x004374f0) passes 0**. It also runs from the lifespan-expiry arm of `Shot_HandleShot`
(0x00435830), gated on `range_link_gate > 0` and `flags_secondary 0x20` (Flags2 "don't launch submunitions when the shot
expires") clear; that callsite remains unported. Each child is spawned through `Shot_SpawnShotFromWeapon` with
`spawn_without_owner = 1` (owner slot is still stored, only the owner-relative kinematics are suppressed) and then has
its position/velocity/heading taken from the parent. `SubTheta < 0` fans the children deterministically; `SubTheta > 0`
randomizes; `SubLimit` caps recursion via `ShotState.linked_shot_generation` (+0x36). See `src/game/weapon.cpp`
`NovaWeapon_SpawnLinkedShotsOnImpact` and `tests/collision_test.cpp` `[linked]`.

## Notes for future RE

- Treat wiki semantics as naming guidance, not absolute truth.
- If a branch behavior differs from wiki terminology, executable behavior wins and mapping should be updated.
- Closest code touchpoints:
  - `NovaGameplay_FireShipWeapons`
  - `NovaGameplay_SpawnShotFromWeapon`
  - `NovaGameplay_CanFireWeaponBank`
  - `NovaGameplay_GetWeaponBurstAttempts`
