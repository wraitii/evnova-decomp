# Spaceflight timing and cadence

This note separates EV Nova's actual frame scheduling from the units used by
its simulation variables. They are not both 30 Hz, and a raw per-call update
must not automatically be treated like a normalized movement tick.

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

The raw-call conversion is a deliberate clean-room policy. The original raw
behavior still varied below its maximum rate; the port targets the original at
47.62 Hz while remaining stable at modern display rates. Probabilities and
discrete integer state machines generally need a logical-tick accumulator (or
a time-adjusted probability), not a fractional mutation that changes RNG
cadence.

## Full and reduced system ticks

`Frame_SpaceflightLoop` (`0x00417600`) calls `Frame_TickSystems(1)` once per
ordinary loop. X2 mode adds `Frame_TickSystems(0)` after drawing. The reduced
call still runs the player, collision, shot, ship, and scope-8 passes; it omits
status/reactions, AI, and AI-odds work. This scheduling distinction is separate
from whether an individual calculation uses `g_avg_frame_tick_scale`.

The current port gates most scope-8 work on `run_full_tick`. This is benign in
the presently reachable frozen reduced path, but differs from original X2
scheduling. Stellar defense batteries (`0x0042d890`), gravity, crash handling,
and the random-encounter countdown belong to scope 8. Defense batteries have
their own frozen-time guard in the original; gravity and crash handling do not.

## Confirmed normalized consumers

These use `g_avg_frame_tick_scale` in the original and should remain on
`elapsed_ticks`:

| Function | Behavior |
|---|---|
| `Ship_HandleShip` (`0x00433050`) | Position integration and most continuous ship motion/timers |
| `Shot_HandleShot` (`0x00435830`) | Lifetime, position, ordinary mode-1 homing, animation dwell, and fuse |
| `Stellar_UpdateStellarSprites` (`0x0042cd10`) | Stellar animation |
| `Asteroid_UpdateSprites` (`0x00436910`) | Asteroid drift and animation |
| `Shot_UpdateImpactEffectSprites` (`0x0042e160`) | Impact animations |
| `Frame_UpdateFadingEffectSprites` (`0x0043b170`) | Fading effect lifetime and movement |
| `Frame_UpdateFreeflightObjectSprites` (`0x0042c1b0`) | Freeflight lifetime and movement |
| `Shot_UpdateDirectionalWeaponEffects` (`0x0042c660`) | Directional-effect lifetime (`scale * 0.25`) |
| `Stellar_TickStellarDefenseBatteries` (`0x0042d890`) | Battery cooldown |
| `Stellar_TickStellarGravityPull` (`0x0043adb0`) | Gravity acceleration |
| `System_UpdateRandomEncounterCountdown` (`0x0043a020`) | Reinforcement countdown |

## Confirmed raw-call consumers and port status

| Original behavior | Port status | Recommended treatment |
|---|---|---|
| State-8 negative-speed decay in `Ship_HandleShip` (`0x00433b03`) | Normalized to `elapsed_ticks / 0.63`; position-before-thrust ordering restored | Current reference implementation |
| **DONE — death timers:** NPC, active-player, and inactive-player post-finale timers (`0x00433050`, `0x0044aa70`) subtract `1.0` per call | Port advances by `elapsed_ticks / 0.63` | Matches the original 47.62 Hz maximum-rate duration; the NPC midpoint crossing retains the odd-`DeathDelay` no-hit quirk |
| **DONE — fire-restricted velocity damping:** multiplies by `0.995` per call | Port uses `pow(0.995, elapsed_ticks / 0.63)` | Matches the original 47.62 Hz maximum-rate damping while remaining refresh-independent |
| **DONE — passive cloak recovery:** `Ship_UpdateVisualState` (`0x00428340`) subtracts `1.0` per call while armed transitions use `g_avg_frame_tick_scale` | Port advances passive recovery by `elapsed_ticks / 0.63`; active transitions remain normalized | Matches the original 47.62 Hz maximum-rate recovery without changing the armed transition domain |
| **SKIPPED — player engine-glow integer rise/fall:** the player state machine remains a clean-room target interpolation pending reconstruction of its distinct normal-thrust, gravity-shield, afterburner, banking, turnaround, and hyperspace branches. | **DONE — NPC:** `Ship_HandleShip` banks normalized time and replays its ordered jump `+3`, banking `+2`, and thrust/settle/fade mutations at the 21 ms raw-call cadence. Player mutations still run once per port update and are already marked `TODO(decomp(0x0044aa70)) skipped` in code. | Do not cadence-wrap the player approximation. Revisit only after reconstructing its original ordered integer mutations. |
| **DONE — control-mode-`0x0d` formation-release counter:** `Ship_ApplyShipAiControls` (`0x00408150`, increment at `0x00408d67`) adds `1.0` per raw call while an NPC follower waits for its departing leader | Port advances by `elapsed_ticks / 0.63`, retaining the pre-increment `> 30` release test | Matches the original 47.62 Hz maximum-rate delay without making the release display-rate dependent |
| Mission/NPC maintenance clocks and per-call rolls (`0x00448910`, `0x00443760`, `0x0041d6e0`) | Port invokes them at display rate | Use a 21 ms logical-tick accumulator to preserve integer/RNG cadence |
| Screen-flash duration in `Frame_UpdateScreenFlashTimers` (`0x0042f1b0`) | Port converts frames using 33.333 ms | Use 21 ms per original maximum-rate frame; route-map deadlines using the 60 Hz clock remain wall-clock based |
| SWParticles (`0x0047c800`) | Deliberately fixed at 60 updates/s | Reconsider 47.62 updates/s; current sparks/debris run about 26% faster than the original maximum cadence |

## Weapon-specific findings

`Shot_HandleShot` motion is normalized, but several guidance submodes in
`Shot_UpdateShotGuidance` (`0x00431530`) deliberately use raw calls:

- asteroid-decoy mode-1 turning uses the raw guided turn rate; the port
  currently scales it with `elapsed_ticks`;
- interference state `999` uses raw turning and a raw 300-call phase counter;
- mode-6 rocket velocity uses `(velocity * 94 + polar * 5) * 0.01` once per
  call, which needs an exponentiated/time-adjusted blend for stable behavior;
- mode-5 bomb steering turns one degree per call;
- jammer, retarget, and asteroid-decoy random rolls occur once per shot call.

Guidance probabilities should be converted as probabilities over time or
driven by logical 21 ms ticks. Scaling random bounds directly would alter both
probability and RNG-stream behavior.

There are also non-policy defects to keep distinct from cadence choices:

- **DONE — animated shot frame delay:** `NovaWeapon_TickShots` uses the Bible's
  `BeamWidth` unit for spinning sprite weapons: one unit is 1/30 second. The
  port accumulates normalized ticks regardless of its current update rate,
  matching the original `g_avg_frame_tick_scale` comparison;
- **DONE — projectile damage decay:** the Bible's `Decay` interval is measured
  in 1/30-second ticks. `NovaWeapon_TickShots` accumulates normalized ticks,
  preserves the original strict comparison and reset/no-catch-up behavior, and
  direct hits subtract the elapsed decay points from both damage types;
- directional weapon effects, ship hit-reaction decay, and player-aggro decay
  are missing rather than merely using the wrong rate.

The death-timer duration fix does not yet reproduce the discrete destruction
effect cadence. `Ship_UpdateVisualState` still makes one Explode1 random roll
per display call, and the NPC timed-action cascade still uses the display-rate
frame counter. Preserving their RNG stream and modulo behavior requires
replaying the ordered destruction passes on a logical 21 ms cadence;
fractional timer stepping alone is intentionally not presented as that larger
scheduling fix.

## Review checklist

For each timing change, record the original instruction or decompile expression
and answer separately:

1. Is the function invoked on full ticks, reduced ticks, presentation frames,
   or a wall-clock callback?
2. Does the value multiply by `g_avg_frame_tick_scale`?
3. Is it continuous state, a discrete counter, or a random opportunity?
4. Does constant behavior target 30 normalized ticks/s or 47.62 raw calls/s?
5. Does update ordering affect the visible result?
