# Spaceflight timing and cadence

This note separates EV Nova's presentation rate, its normalized 30 Hz game
units, and its raw 21 ms outer-call cadence — three different clocks. A raw
per-call update must not be treated like a normalized movement tick.

## Original timing model

`Frame_MeasureFrameTiming` (`0x00432ea0`) waits until at least 21 ms have
elapsed since the preceding spaceflight iteration. Ordinary spaceflight
therefore runs no faster than approximately `1000 / 21 = 47.62` calls/s.

The same function publishes an exponentially smoothed
`g_avg_frame_tick_scale`:

```text
sample = elapsed_ms * 0.03
g_avg_frame_tick_scale = (old * 3 + sample) * 0.25
```

At the 21 ms floor its steady-state value is `0.63`. Thus 30 Hz is the unit
basis for scaled quantities, not the main loop's invocation rate: 47.62 calls/s
times 0.63 normalized ticks/call is 30 normalized ticks/s.

| Domain | Original expression | Constant-behavior port policy |
|---|---|---|
| Normalized simulation | Multiplies by `g_avg_frame_tick_scale` | Multiply by `elapsed_ticks` (`elapsed_ms / 33.333...`) |
| Raw spaceflight call | Fixed increment, decay, blend, or probability once per call | Advance by `elapsed_ticks / 0.63`, or use an equivalent 21 ms logical-tick accumulator |
| Wall-clock / 60 Hz clock | `NovaTime_GetTickCount60Hz` or elapsed timestamp | Preserve wall-clock duration |
| Presentation/display | Explicit sprite-world/present hook | Choose and document a fixed visual reference rate when stable behavior is desired |

## Port scheduling policy

The port has two deliberately different outer-step modes:

| Mode | Simulation quantum | Normalized delta | Purpose |
|---|---:|---:|---|
| Ordinary (`speed_multiplier = 1`, acceleration disabled) | 21 ms | 0.63 | Preserve the original maximum-rate outer calls while rendering at the host's 60/120 Hz cadence |
| Accelerated | 33.333 ms | 1.0 | Reduce scheduler overhead while executing virtual time faster than wall time |

The host loop polls input and renders independently. It accumulates virtual
gameplay time and executes as many simulation quanta as are owed before drawing
the latest state. At ordinary speed this yields approximately 47.6 outer calls
per second; the normalized quantities therefore still advance at 30 ticks/sec.
In accelerated mode, a multiplier of `M` executes approximately `30*M` outer
simulation ticks/sec. The 21 ms/raw-call consumers use their own accumulators,
so they execute approximately `47.6*M` raw calls/sec even though the accelerated
outer scheduler is only 30 Hz.

Rendering is intentionally not part of the simulation quantum. Drawing, HUD
composition, route-map painting, and `Present()` run once per host frame. An
uncapped accelerated renderer may therefore draw the same state repeatedly;
this is an explicit **divergence — uncapped presentation**, not an additional
gameplay tick.

The gameplay update body currently runs entirely inside the selected outer
quantum; continuous operations are not yet moved to the host/render cadence.

The raw-call conversion is a deliberate clean-room policy. The original raw
behavior still varied below its maximum rate; the port targets the original at
47.62 Hz while remaining stable at modern display rates. Probabilities and
discrete integer state machines generally need a logical-tick accumulator (or
a time-adjusted probability), not a fractional mutation that changes RNG
cadence.

## Full and reduced system ticks

`Frame_SpaceflightLoop` (`0x00417600`) runs its gameplay body on the selected
21 ms or accelerated 30 Hz virtual simulation quanta and presents the latest
state once per host frame. X2 mode adds `Frame_TickSystems(0)` after drawing in
the original. The reduced call is an extra simulation pass, not a display-only update: it still advances
the player, collision resolution, shots, ships, visual state, and scope-8
world effects. A full tick additionally does the less time-critical
bookkeeping and decision work: proximity/UI refresh, squad-leader setup,
mission/reaction and NPC-spawn maintenance, ship AI, combat-odds evaluation,
and the travel-countdown sprite. In short, X2 doubles the physics/action
cadence while leaving much of the UI, AI planning, and world maintenance at the
ordinary cadence. This scheduling distinction is separate from whether an
individual calculation uses `g_avg_frame_tick_scale`.

Blocking landing/service windows are simulation boundaries. When one returns,
the current virtual-tick batch is abandoned and the next outer frame resumes
flight. This lets docked-state observers and automation see the modal result
before another command edge is generated from the pre-modal batch.

The current port gates most scope-8 work on `run_full_tick`. This is benign in
the presently reachable frozen reduced path, but differs from original X2
scheduling. Stellar defense batteries (`0x0042d890`), gravity, crash handling,
and the random-encounter countdown belong to scope 8. Defense batteries have
their own frozen-time guard in the original; gravity and crash handling do not.

## Frame scopes and tick subsystems

`Frame_SpaceflightLoop` labels its execution buckets by scope id; the helpers are
`ProfileScope_SetLabel` (`0x0046fde0`), `ProfileScope_Begin` (`0x0046fe20`) and
`ProfileScope_End` (`0x0046fe80`).

| scope | bucket |
|---|---|
| 4 | `Ship_HandleShip` |
| 5 | ship display |
| 6 | AI routines |
| 7 | `Shot_HandleShot` |
| 8 | misc handlers |
| 9 | collisions |
| 10 | player |
| 12 | `DrawStatus` |
| 13 | `ProcessSpriteWorld` |
| 14 | `AnimateSpriteWorld` |
| 15 | `PostAnimate` |
| 16 | locking |
| 17 | erase |
| 18 | draw |
| 19 | update |
| 20 | calc AI odds |

In-flight runtime entrypoints: `0x00417600` `NovaGameplay_SpaceflightLoop`,
`0x004186b0` `NovaGameplay_TickSystems` (full/reduced), `0x00433050`
`NovaGameplay_HandleShip`, `0x00435830` `NovaGameplay_HandleShot`, `0x00437e20`
`NovaGameplay_ResolveCollisions`.

### Scope-8/9/0xb/0xc subsystem ground truth

- **Stellar defense batteries** (`0x0042d890`): the active system's 16 nav
  stellars (SystemDef `+0x2a`) must be `is_available` (`+0x44`), carry a
  defense-battery weapon in StellarDef `+0x2c` (spöb payload `+0x23a`, loader
  rebases `<0x80 -> -1` else `-0x80`), and be not `Stellar_IsStellarActive`
  (`0x0046e3c0`). The `+0x494` cooldown is normalized (`g_avg_frame_tick_scale`);
  on expiry it picks the nearest active ship (64 slots, squared distance) that is
  not cloak-hidden and passes `Government_IsCandidateHostileToTargeter`
  (`0x004629e0`) within `Weapon.range_scalar^2`, applies the disable-only skip,
  then reserves a shared 128-slot `ShotState` (muzzle at stellar, owner -1,
  target slot, `Shot_AimStellarBatteryShot` `0x0043ba30`, spread wrap, lock
  quality, sprite/sound) and wraps the `+0x2e` burst counter. Scheduling: the
  original calls scope 8 on reduced ticks and skips only while
  `g_gameplay_time_frozen`.
- **Gravity** (`0x0043adb0` `NovaGameplay_TickStellarGravityPull`) pulls
  unshielded active ships toward each stellar gravity well and sets
  `g_gravity_pull_active`; the gravity/body-immunity gate family is
  `0x0046df70` `ShipIsInertialess`, `0x0046e120` `ShipHasGravityShielding`,
  `0x0046e210` `ShipImmuneToStellarCrash` (outfits 0x26/0x29/0x2a + class flags).
- **Ship-stellar crash** (`0x0043aed0`) pixel-mask-tests active ships against the
  body sprite; on contact it kills the ship, resets the player target and spawns
  impact effects. The mask path is documented in
  `docs/sprite_pixel_mask_collision.md`.
- **Broad-phase collision** (scope 9, `0x004772d0`
  `NovaGameplay_TestSpriteLayerOverlaps`): pairwise AABB overlap between two
  `SpriteLayer`s; on overlap invokes the per-entity callback (`+0x94`) with both
  entities + overlap rect. `TickSystems` tests shot containers and freeflight
  objects against each other and the sector; the callback's precise opaque-pixel
  test is `Sprite_TestPixelMaskOverlap` (same doc).
- **Inbound threat tally** (scope 0xb, `0x00422210`
  `NovaWeapon_TallyInboundWeaponThreat`): each full tick resets active ships and
  adds `trunc((MassDmg + EnergyDmg) * 0.5)` per live normal-lock shot targeting
  the ship (signed-short wrap). `0x004221d0`
  `NovaAi_IsInboundThreatExceedingDefenses` returns true when the tally is at
  least `(shield + armor) * 1.05`; its caller conserves guided ammunition when a
  target is already expected to die from inbound ordnance.
- **Scan detection / radar** (scope 0xc): `0x0045d030`
  `NovaGameplay_RollProximityScanDetection` rolls detection odds from system
  scan-bonus minus player scanner strength (`0x0046abb0`
  `NovaGameplay_GetScannerStrength`, summed owned scanner outfits id 0x18) and
  latches `g_proximity_scan_detected` (`0x007caba0`), which drives the radar blip
  draw in `NovaUi_DrawStellarRadarPanel` (`0x0045d600`).
- **Travel/interaction UI**: `0x0044b120` destination-system cycle (command 0x60,
  Backslash / Shift+Backslash through directly-linked systems, mode 3);
  `0x00443760` `NovaGameplay_TickShipInteractionReactions` (16 interaction slots);
  `0x004444f0` `BuildTravelDestinationDescription`; `0x0042cc30`
  `TickTravelCountdownSprite` (countdown sprite armed at 30 in the escape-pod
  sequence, cleared on travel completion); `0x0042cbb0`
  `AnchorAuxHUDSpriteToOrigin`.

External mechanics baseline: weapon modes are data-driven (`wëap` guidance);
mission objectives include destroy/disable/board/escort/observe/rescue/chase-off;
and data is plugin-overridable (`Data` + plug-in layering).

References: `https://evn.fandom.com/wiki/W%C3%ABap`,
`https://escape-velocity.games/EVN_Walkthroughs/html/index.html`,
`https://escape-velocity.games/docs`.

## Confirmed normalized consumers

These use `g_avg_frame_tick_scale` in the original and should remain on
`elapsed_ticks`:

| Function | Behavior |
|---|---|
| `Ship_HandleShip` (`0x00433050`) | Position integration, most continuous ship motion/timers, and NPC player-aggro decay (`ShipState+0xC910 -= g_avg_frame_tick_scale * 0.5`, clamped at zero) |
| `Shot_HandleShot` (`0x00435830`) | Lifetime, position, ordinary mode-1 homing, animation dwell, and fuse |
| `Stellar_UpdateStellarSprites` (`0x0042cd10`) | Stellar animation |
| `Asteroid_UpdateSprites` (`0x00436910`) | Asteroid drift and animation |
| `Shot_UpdateImpactEffectSprites` (`0x0042e160`) | Impact animations |
| `Frame_UpdateFadingEffectSprites` (`0x0043b170`) | Fading effect lifetime and movement |
| `Frame_UpdateFreeflightObjectSprites` (`0x0042c1b0`) | Freeflight lifetime and movement |
| `Shot_UpdateWeaponSmokePuffs` (`0x0042c660`) | Weapon smoke-puff lifetime (`scale * 0.25`) |
| `Stellar_TickStellarDefenseBatteries` (`0x0042d890`) | Battery cooldown |
| `Stellar_TickStellarGravityPull` (`0x0043adb0`) | Gravity acceleration |
| `System_UpdateRandomEncounterCountdown` (`0x0043a020`) | Reinforcement countdown |

## Raw-call consumers and their constant-rate treatment

| Original behavior | Constant-rate treatment |
|---|---|
| State-8 negative-speed decay in `Ship_HandleShip` (`0x00433b03`) | Advance by `elapsed_ticks / 0.63`; position-before-thrust ordering preserved |
| Death timers: NPC, active-player, and inactive-player post-finale timers (`0x00433050`, `0x0044aa70`) subtract `1.0` per call | Advance by `elapsed_ticks / 0.63`, matching the original 47.62 Hz maximum-rate duration; the NPC midpoint crossing retains the odd-`DeathDelay` no-hit quirk |
| Fire-restricted velocity damping: multiplies by `0.995` per call | `pow(0.995, elapsed_ticks / 0.63)`, matching the original maximum-rate damping while remaining refresh-independent |
| Passive cloak recovery: `Ship_UpdateVisualState` (`0x00428340`) subtracts `1.0` per call while armed transitions use `g_avg_frame_tick_scale` | Advance passive recovery by `elapsed_ticks / 0.63`; active transitions stay normalized, preserving the armed transition domain |
| Player engine-glow integer rise/fall | Not cadence-wrapped: the player state machine is a clean-room target interpolation pending reconstruction of its distinct normal-thrust, inertialess, afterburner, banking, turnaround, and hyperspace branches. The NPC `Ship_HandleShip` path banks normalized time and replays its ordered jump `+3`, banking `+2`, and thrust/settle/fade mutations at the 21 ms raw-call cadence. Revisit only after reconstructing the player's original ordered integer mutations |
| Control-mode-`0x0d` formation-release counter: `Ship_ApplyShipAiControls` (`0x00408150`, increment at `0x00408d67`) adds `1.0` per raw call while an NPC follower waits for its departing leader | Advance by `elapsed_ticks / 0.63`, retaining the pre-increment `> 30` release test |
| Mission reactions and NPC maintenance (`0x00443760`, `0x0041d6e0`) | Scope 0xb banks normalized elapsed time and replays whole 21 ms original-rate calls, preserving countdown, spawn, and RNG cadence at ~47.62 calls/s |
| Mission spawn-state refresh (`0x00448910`) | Event-triggered, not a cadence consumer: run on jump arrival, stellar landing, and launch. It arms Bible `ShipStart = 1` arrivals: 30 calls for friendly escort fleets (`ShipGoal = 3`, `ShipBehav = 1`), otherwise 100–199 |
| HUD overlay duration in `Frame_TickHudOverlayAndRouteMapTimers` (`0x0042f1b0`; formerly misnamed `Frame_UpdateScreenFlashTimers`) | Convert each raw-call countdown unit to 21 ms; cached overlays use the original Chicago 12 face. Route-map deadlines on the 60 Hz clock stay wall-clock based. This function does not update the hyperspace screen flash |
| SWParticles (`0x0047c800`) | Bank and replay whole updates at the original 21 ms outer-loop cadence (47.62/s), preserving discrete lifetime/movement ordering without following the display refresh rate |

## Dirty-gated UI cadence (stellar radar)

The stellar radar panel (`NovaUi_DrawStellarRadarPanel` 0x0045d600) is not
drawn per loop; it is dirty-gated on `DAT_00596d25`, which
`NovaUi_RefreshGameplayPanels` (0x0045d320) sets on each `+ 0xf <=
NovaTime_GetTickCount60Hz` poll (250 ms), plus on `RebuildStellarRadarPanel`
(0x0045d0a0) state changes. The panel body is composed into an offscreen
buffer, so between draws every contact, the far-from-origin arrow, and the
target-blink phase hold their poll-time values; `g_proximity_scan_detected`
(0x007caba0) is re-rolled every simulation tick by
`Frame_RollProximityScanDetection` (0x0045d030, scope 0xc) but sampled (and its
static pattern re-rolled with `NovaRandom_Range(10)`) only inside the draw.

`force_empty` in that draw is the caller passing `g_is_system_transition_active`
(0x007354a9), which is set only around the docked/landing visit (writes at
0x00455e19/0x0045612d/0x00489241/0x004b32bc) and never by a hyperspace jump;
the radar therefore keeps drawing contacts and static through the jump
brake/hold/tunnel.

**Divergence:** the radar panel is composited directly each presentation frame.
The blink phase and the interference static are latched on the 250 ms poll so
their cadence matches the original; contacts and the arrow are recomputed per
frame (smooth blips rather than ~4 Hz steps). This is cosmetic only -- no
gameplay state reads the painted panel.

## Weapon-specific findings

`Shot_HandleShot` motion is normalized, but several guidance submodes in
`Shot_UpdateShotGuidance` (`0x00431530`) deliberately use raw calls:

- **Asteroid-decoy tracking:** internal guidance state `1` (distinct
  from the Bible's `Guidance = 1` homing mode) turns by the raw guided turn
  rate after shot age exceeds 15;
- **Interference:** guidance state `999` uses raw turning and a raw
  300-call phase counter;
- **Mode-6 rocket acceleration:** velocity uses
  `(velocity * 95 + polar * 5) * 0.01` once per raw call;
- **Mode-5 freefall bomb/mine weathervaning:** the nose turns one degree
  per raw call toward the inherited velocity vector;
- **Guidance randomness:** jammer owner-retarget, cloak owner-retarget,
  interference recovery, and asteroid-decoy rolls are replayed once per raw
  shot call.

The port banks normalized time and replays these operations on logical 21 ms
calls. Normal state-0 homing remains continuous: it turns by
`guided_turn_rate * elapsed_ticks` after the executable's strict
`shot_age > elapsed_ticks * 15` gate.

There are also non-policy defects to keep distinct from cadence choices:

- **NPC player-aggro pressure:** incidental player hits test the
  pre-hit accumulator against `50.0`, then add `weapon reload ticks * 1.75`;
  successful retargeting resets it. Targeted hits bypass accumulation and the
  threshold. `Ship_HandleShip` decays retained pressure by `0.5` per normalized
  tick and clamps a positive crossing to zero;
- **Animated shot frame delay:** `NovaWeapon_TickShots` uses the Bible's
  `BeamWidth` unit for spinning sprite weapons: one unit is 1/30 second. The
  port accumulates normalized ticks regardless of its current update rate,
  matching the original `g_avg_frame_tick_scale` comparison;
- **Projectile damage decay:** the Bible's `Decay` interval is measured
  in 1/30-second ticks. `NovaWeapon_TickShots` accumulates normalized ticks,
  preserves the original strict comparison and reset/no-catch-up behavior, and
  direct hits subtract the elapsed decay points from both damage types;
- **Weapon smoke trails and shield-bubble flash decay** are not modelled (not
  merely at the wrong rate). The smoke functions are
  `Shot_SpawnWeaponSmokePuff` (`0x004215d0`) and
  `Shot_UpdateWeaponSmokePuffs` (`0x0042c660`): Bible Flags `0x0200`/`0x0400`
  choose small/big smoke, `0x0800` chooses the persistent animation variant,
  and `SmokeSet` selects its graphics. None of the 256 stock Nova weapons uses
  these flags (and every stock `SmokeSet` is `-1`), so this is a deferred
  plug-in-compatibility feature.

**Destruction cadence:** the per-ship accumulator is clean-room scheduling
machinery, not a recovered `ShipState` field. It banks normalized elapsed time
and invokes the original discrete destruction body once per logical 21 ms
outer-loop call. NPC calls preserve the `Ship_HandleShip` timer/timed-debris step
followed by `Ship_UpdateVisualState`; the player uses the same visual-body
scheduler after its player-core checks, so death timers, Explode1 RNG
opportunities, the NPC timed-action modulo, and the Explode2 finale are not
driven by SDL presentation frequency. Player respawn and eject paths clear the
accumulator and one-shot latches before the replacement hull can enter a later
destruction sequence.
