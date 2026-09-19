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
- `5` freefall bomb (commonly used as a mine)
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

## Homing / seek lock vs jamming (mode 1)

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
- `0x0002` decoyed by asteroids: 1/10 per raw call, an active asteroid within 200 px on both axes and within 16 deg of the
  shot heading latches internal `guidance_state = 1` (distinct from the Guidance mode; target_ship_slot becomes an asteroid pool index).
- `0x0008` confused by sensor interference: at spawn, `Random(100 / frame_scale) + 1 <= SystemDef.interference (+0x96,
  payload +0x6c)` latches `guidance_state = 999` — the weave state (heading zig-zags on a 300-raw-call phase,
  `g_spaceflight_frame_counter % 300 < 150` selects the direction; 1/1000 owner-retarget escape with 0x8000).
- `0x4000` loses lock if target not directly ahead: within 250 px on both axes with the target more than 45 deg off the
  nose, the lock drops (target = -1).
- `guidance_state == 998` is the inert latch Shot_HandleShot sets when a state-0 shot's target dies.

**Turn rate:** `WeaponDef.guided_turn_rate` (+0x58 float) = wëap payload +0x6a (Bible "GuidedTurn") * 0.1. Normal homing
starts once shot age (`Count - life_time`) is strictly greater than `frame_scale * 15` and turns by
`guided_turn_rate * frame_scale`. The asteroid-decoy and interference states use a bare age gate of 15 and raw per-call turns.

**Mode-6 rockets** do not chase: they fire along the lead bearing (Ship_AimWeaponPredictive two-regime model) and then
blend velocity toward the heading each raw call: `vel = (vel*95 + polar(heading,speed)*5) * 0.01` (0x57540c/0x575408/0x575368).
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

## Beam endpoint geometry and lightning rendering

`Shot_UpdateBeamHitQueue` (0x0042f270) derives every visible endpoint per frame; `Shot_QueueBeamHit` (0x00427a90)
stores no coordinates. Source = owner position + owner turret-exit offset (now modelled by the port:
`weapon_detail::ApplyTurretSpreadVelocity`, applied at queue time and every tick).

- **Mode 0 (fixed beam)** keeps the owner's current heading, even when a target slot was recorded. It scans for the
  nearest eligible ship within `BeamLength + ceil(trunc(frame_span*0.66)/2)` and inside a
  forward cone of `trunc(frame_span*0.66)*10/32` degrees (frame_span =
  `Sprite_GetShipClassEscortFrameWidth` 0x004624c0 returns the full frame width, fallback 0x4b=75). A hit
  truncates the endpoint to `trunc(distance - frame_span*0.2)`; nothing in reach ends at exactly `BeamLength`. The 0.66 and 0.2 scales are the doubles
  DAT_005753d0 / DAT_005753d8; all three conversions use the x87 FIST truncation idiom, not round-to-nearest. A valid
  contact near the outer reach can truncate to longer than `BeamLength` (e.g. 120 px target -> 105 px beam); the original
  does not re-clamp to `BeamLength`.
- **Mode 3 (turreted) / mode 10 (PD)** take `Math_BearingFromPointToPoint(source,target)` instead.
- A mode-0 `BeamLength` is the visible/hit range; the AI fire gate's `BeamLength + 0x20` (`Weapon_FireShipWeapons`
  0x00414550, NPC only; the player path 0x00455150 has no reach gate) is a separate reach envelope.

`SWBeams_DrawThickFadingBeam` (0x0047a410, the mode-0 `LiDensity > 0` lightning plotter) splits the span into
`trunc(max(|dx|,|dy|) * LiDensity * 0.01)` segments (DAT_00575888 is the double 0.01, not 0.5; the conversion is the
x87 truncation idiom). Every segment is jittered by `LiAmplitude` except the single final segment, which lands exactly
on the target and gets the 0x20 end fade; a step count below 2 draws nothing. `SWBeams_DrawShortBeam` (0x00479fe0) is the
straight `LiDensity == 0` plotter.

### Turret-exit quadrant selection (beam records)

Every beam callsite passes `turret_quadrant = -1`, so `Shot_QueueBeamHit` chooses the quadrant from the per-ship
per-group rotation state `Ship.muzzle_quadrant` (`g_ship_states +0xc8fe`, one slot per ExitType): a still-unset slot is
randomized in `[0,4)` (`NovaRandom_Range`), and the stored slot then advances `+1 mod 4` once per successfully queued
beam. `flags_tertiary 0x10` redirects the stored selection to the target-nearest barrel
(`Weapon_ChooseBestTurretQuadrantForTarget` 0x0046c4e0) when a target is passed and targeting is not forced; the
original feeds that helper the raw hull heading in **radians** truncated to a short where degrees are expected
(disasm 0x00427c98 `FLD [owner+0x44]` / 0x00427cd9 `MOVSX ECX,AX` / 0x00427ce2 `PUSH ECX`), preserved in the port.
The quadrant is stored per beam record (not per ship), so it persists across the owner's later rotations. The source
offset itself is re-derived every frame in `Shot_UpdateBeamHitQueue` using the stored quadrant and the current displayed
rotation frame (`Weapon_ApplyTurretSpreadVelocity` 0x0046c5c0; near/far scale selection and drop). Thunderhead / Pirate
Thunderhead mount the Thunderhead Lance (weapon 0xa6, ExitType 3): sh\x8an group 3 lateral is `+7/-7/+7/-7`
(Thunderhead) and `+9/+9/-9/-9` (Pirate), forward `+9`, so the Lance fires from the two side exits alternately.

### Beam port gaps

- The mode-0 scan uses the original inline eligibility from 0x0042f270 (ported as `BeamCandidateEligible`): active,
  same system, not the owner, ship_class != 0x2ff, weapon `flags_primary 0x400` == target-class
  `capability_flags 0x400`, not the owner's direct subordinate / own squad leader / player-squad mate, and the
  mission-critical dude `booty_flags 0x100` exclusions. It deliberately does **not** reject same-government non-squad
  ships, matching the original (unlike the projectile `Weapon_CanWeaponHitTarget` 0x00426ef0 gate).
- The scan lacks the original's asteroid arm and applies a hit once rather than per tick; `shot_random_spread`
  (Bible Inaccuracy) is not added to the beam bearing per tick.
- Twin-surface plotters `SWBeams_DrawKinkedBeam` 0x0047AC50 / `SWBeams_DrawBeamWithFlare` 0x0047AFD0 (the
  `Shot_DrawBeamHitQueueForSurface` 0x00438810 `field_0xec != 0` path) are not ported.
