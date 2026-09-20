# x87 numeric behavior: FIST truncation and accepted precision divergences

Two related x87 topics:

1. The **FIST truncation idiom** MSVC emits for implicit `(int)` / `(short)`
   casts of floats — needed to read decompiled expressions correctly and to
   reproduce their rounding at call sites.
2. **Accepted double-vs-extended-precision divergences** where the original's
   80-bit x87 accumulators are not bit-reproduced by the port.

## FIST truncation idiom

### The idiom

MSVC compiles an implicit `(int)` / `(short)` cast of a float as an x87
`FIST` (round-to-nearest) followed by a residual/sign correction whose
integer form is `ADD reg,0x7fffffff; SBB out,0` plus a signed branch. The
net result is **truncation toward zero**, *not* round-to-nearest. The same
idiom is documented in `src/game/ship_ai_behaviors.cpp` (`RoundedAxisDistanceSquared`).

A genuinely different pattern, `ROUND(f) + (0 < frac)`, nets to ceil and
must not be confused with this one. `ROUND(f)` alone (no correction) is
round-half-even.

### Detection recipe

1. Ghidra decompile text marker (both branches present):

   `-(uint)(0x80000000 < (uint)...` next to `(uint)(0 < (int)...`.

2. Full-dump grep (fastest, off-tree):

   `grep -rl '-(uint)(0x80000000 <' /tmp/ghidra_full_decompile`

3. Per-site: `./tools/ghidra_api 'operand_search?op=0x7fffffff&kind=imm'`

   (the endpoint under-reports; the dump grep is authoritative).

4. Cross-check a port line by finding the nearest preceding `Ghidra 0x...`
   citation and confirming that function's dump has the correction idiom.

   This is a heuristic: the citation may cover a large function and the
   specific conversion can still be a different pattern.

### Confirmed sites

- `Ship_ComputeTradeInValue` (0x00469100), `Outfit_ComputeScaledPurchasePrice`
  (0x0049d640), `Outfit_ComputeOutfitPurchaseMass` (0x0046e950).
- Trade center prices (0x0048c730), negotiation bribe (0x00480030),
  ship-comm payment (0x00482280), shipyard hire (0x00498dc0), outfitter
  resale (0x0048ea70).
- Government credits (0x00440750) and reputation flood (0x00467140),
  mission reputation (0x00440410), boarding/plunder shares (0x00484230,
  0x00482940, 0x00412550), dude-class spawn weights (0x004bd3c0).
- Hull blast radius/damage (0x00428340), impact burst counts (0x004211d0),
  cargo-pod count (0x0041f330), freeflight launch scatter (0x0041f800),
  asteroid ring spread (0x00421830), nearest-target distance (0x0046ba30),
  guided turn rate (0x00431530).
- Pilot shield/fuel u16 save (0x004c7dd0).

### Verified sites

Original functions confirmed to compile the conversion with the FIST
truncation idiom:

#### Sites using the idiom

- `ship_ai_state.cpp` `Ship_UpdateShipAiState` 0x00405590 (scripted-asteroid
  threshold); `ship_ai_controls.cpp` `Ship_ApplyShipAiControls` 0x00408150
  (entry desired-heading sync, leader copy/delta, evasive +/-135,
  velocity-match mode 0xc, mode-0xf
  copy); `ship_ai_weapons.cpp` `Weapon_FireTurretAtTarget`
  0x0040ce00 (mode-7/8 arc);
  `Ship_ScoreAssistTargetForShip` 0x00412090 (helper renamed
  `RoundedDistanceSquared` -> `TruncatedDistanceSquared`, score sum truncates).
- `weapon_shots.cpp` `Shot_SpawnShotFromWeapon` 0x0041fd30 (interference roll);
  `weapon.cpp` `Weapon_FirePlayerWeaponBank` 0x00455150 (mode-0 heading, blind-spot
  heading); `Weapon_FireShipWeapons` 0x00414550 (mode-0 heading); the shared
  `RoundHeadingDeg` helper (`Shot_HandleShot` 0x00435830 /
  `Shot_UpdateShotGuidance` 0x00431530); `TurnShotToward`'s turn-rate floor.
- `spaceflight_view.cpp` `Asteroid_UpdateSprites` 0x00436910;
  `Shot_UpdateImpactEffectSprites` 0x0042e160;
  `Frame_UpdateFreeflightObjectSprites` 0x0042c1b0; the reticle pulses
  (`NovaUi_UpdateShipTargetReticle` 0x0042ede0 and
  `NovaUi_UpdateTravelTargetReticle` 0x0042eac0).
- `spaceflight.cpp` `Ship_HandlePlayerShipCore` 0x0044aa70 (self-destruct tick
  and seconds) and `Ship_HandleShip` 0x00433050 (debris timed-action interval).
- `collision.cpp` `Shot_ResolveCollisions` 0x00437e20 (asteroid dist^2);
  `Frame_AddCombatRatingPoints` 0x0046f1e0; `Ship_ApplyDamageToShip`
  0x004192d0 (disable-armor); `Weapon_ApplyWeaponOnHitEffects` 0x0046f3f0
  (ionization points); `Weapon_SpawnWeaponImpactEffectPackage` 0x00462550
  (yield boxes); and the collision-mask mirrors in the local
  `RefreshCollisionMasks` (matching 0x00436910 / 0x0042c1b0).
- `boarding_plunder.cpp` `Player_HandleBoardTargetCommand` 0x0045a3d0
  (heading gate) and `NovaUi_RunBoardingPlunderWindow` 0x00482940 (fuel fill).

#### False positives

- `RoundDouble` in `negotiation_dialog.cpp` (0x00480030 bribe scale) already
  truncates.
- `lround(BearingDeg(...))` in `weapon.cpp` (0x00455150, 0x00414550): the
  original returns a short from `Math_BearingFromPointToPoint`; the conversion
  lives inside that helper, not at the call site.
- `spaceflight_player_state.cpp` face-target bearing inlines `Math_BearingFromPointToPoint`
  likewise; `weapon.cpp` `RotationFrameForShip` and `spaceflight_view.cpp`
  `FrameForHeading` derive a sprite frame the original reads from the sprite's
  integer rotation counter.
- `std::lround(reload_ticks)` in `collision.cpp` (and the `weapon.cpp` /
  `collision.cpp` equivalents) is an identity no-op on an integer field.
- `spaceflight_movement.cpp` `NovaPlayer_IntegrateMovement`'s turn-rate floor and the
  `spaceflight_view.cpp` world-to-screen / starfield helpers are port-local
  render approximations, not call-site FIST conversions.

#### Detection caveat

The full-dump grep plus nearest-citation heuristic misses sites whose function
header names the original without the literal `Ghidra 0x` prefix, and it
mis-tags functions whose nearest citation differs from the code being ported.
Verify against the specific expression in the dump, not just the enclosing
function.

## Accepted double-vs-extended-precision divergences

Covers `Ship_ComputeIonizationDecayRate` (0x0046c080) and
`Ship_GetIonizationIntensity` (0x0046c160), plus the shared ionized-velocity
ramp constants.

`PlayerIonizationDecayFromOutfits` (`src/game/outfit.cpp`) accumulates the
ModType 39 (ion dissipator) bonus in `double`. The original evaluates the same
expression in x87 extended precision (64-bit significand). This is an accepted
platform-precision divergence: the port is not bit-exact here, and the
difference is not an original-game bug.

### Startup control-word evidence

- `PE_EntryPoint` (0x008713e0) jumps to `CRT_StartupMain` (0x00871160), the
  process entry path.
- `CRT_StartupMain` calls `FUN_00881060` at 0x0087123a (`FNINIT; RET`), which
  loads control word **0x037F**: extended precision (64-bit), round-to-nearest.
- No immediate `0x027F` (53-bit precision) exists in the image, and no
  `_controlfp`/`_control87` use was found. The only located `FLDCW`
  (0x004f4bba) temporarily changes the rounding mode for a truncating `FISTP`
  and restores the saved word.
- Uncertainty: a third-party DLL initialised later (for example QuickTime/qtml)
  could in principle change the control word at runtime. This cannot be
  resolved statically from the executable, and the SDL probe cannot inspect the
  original executable's x87 state.

### Concrete divergence

| input | original (x87 64-bit) | port (double) |
|---|---|---|
| base = 1.0F, ModVal = -100, owned = 1 | ~ -2.0817e-17 | 0.0 |

`(double)(-100) * 0.01` rounds to exactly `-1.0`; x87 extended retains the
double `0.01` offset (0.01_double = 0.010000000000000000208...), so adding
`1.0` leaves a tiny non-zero residual. Other values where `val * 0.01` loses
low bits can shift a later cancellation.

### Accepted scope

The cancellation example has a tiny absolute difference; no general ULP bound
or gameplay impact has been established. Software extended-precision emulation
is deliberately omitted under the accepted scope. Revisit only if bit-exact
reproduction is later required; that would need software 64-bit-significand
arithmetic (or an equivalent double-double emulation).

### `Ship_GetIonizationIntensity` capacity and division (0x0046c160)

The player ModType 40 (ion absorber) capacity scan adds each owned outfit's
`mod_val * owned` as an exact 32-bit integer product to the class
`ionization_capacity` (sign-extended int16), accumulating in x87 extended
precision. On the first (uncached) player call the original:

1. stores the **rounded float** total to `DAT_007356a8` (`FST float`), then
2. divides `ionization_points` by the **unrounded x87 accumulator** for that
   same call (`FDIV ST0,ST1` on the retained x87 value).

Subsequent calls read the rounded float cache and divide by it. The port
(`NovaOutfit_ComputeIonizationCapacity` / `NovaOutfit_GetIonizationIntensity`)
accumulates in `double` and always returns/divides by the rounded float, so its
first-call division can differ from the original by one rounding step. Accepted
platform-precision divergence under the same scope as the decay rate above; no
gameplay impact has been established. The comment on
`NovaOutfit_GetIonizationIntensity` records this inline.

### Ionized-velocity ramp constants

The post-decay ramp caps intensity with the double 0.7 (`DAT_00575478` NPC,
`DAT_00575668` player) and steps each axis by the double 0.025 (`DAT_00575480`
NPC, `DAT_00575670` player) times `g_avg_frame_tick_scale`. The port
(`NovaShip_UpdateIonizationCharge`) keeps these as `float` literals, the same
class of accepted precision divergence. The player caller also scales the decay
rate by a float tick scale rather than the original double
`g_avg_frame_tick_scale`.
