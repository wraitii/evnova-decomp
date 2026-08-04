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

## Homing / seek lock vs jamming (mode 1) — mapped

Guided (mode-1) homing shots carry **four independent seek channels** `ShotState.lock_quality_0..3` (+0x38). The four channels
are the EVN Bible's "four jamming types" (the IR/radar/etc. seek-and-jam counters).

- **Attack power (weapon, seeding):** `WeaponDef.jam_vuln_1..4` (+0xBA) seed the shot's lock quality at spawn
  (`SpawnShotFromWeapon`): `lock_quality[i] = 0` if `jam_vuln[i] < 1`, else `Random(0..jam_vuln[i])`.
- **Defense (ship, target jamming):** `ShipState.jamming_score_1..4` (+0xC926), lazily computed by
  `NovaGameplay_GetShipJammingScore(ship, ch)` = owning-govt `InhJam1-4` (govt def +0x56) + outfit effect opcodes
  `0x21..0x24` (EVN Bible outfit "Jamming Type 1-4") bonuses, halved when the ship faction has govt flag `0x80`, clamped 0-100.
- **Lock gate:** in `NovaGameplay_UpdateShotGuidance`, a seek channel grants a usable lock only while
  `lock_quality[i] > 100 - jamming_score[i]`. Higher target jamming raises this threshold, so a jammed missile loses its
  usable lock and flies straight (misses / goes dumb). The loop breaks on the first viable channel.

Point-defense interplay and the "turns away if jammed" (flags_tertiary 0x0010) / "may attack parent if jammed"
(flags_quaternary 0x8000) behaviors live in the projectile-guidance and AI routing; the 0x8000 re-target-to-owner penalty is
visible in `UpdateShotGuidance`.

## Notes for future RE

- Treat wiki semantics as naming guidance, not absolute truth.
- If a branch behavior differs from wiki terminology, executable behavior wins and mapping should be updated.
- Closest code touchpoints:
  - `NovaGameplay_FireShipWeapons`
  - `NovaGameplay_SpawnShotFromWeapon`
  - `NovaGameplay_CanFireWeaponBank`
  - `NovaGameplay_GetWeaponBurstAttempts`
