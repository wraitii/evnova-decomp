# x87 FIST truncation idiom

## The idiom

MSVC compiles an implicit `(int)` / `(short)` cast of a float as an x87
`FIST` (round-to-nearest) followed by a residual/sign correction whose
integer form is `ADD reg,0x7fffffff; SBB out,0` plus a signed branch. The
net result is **truncation toward zero**, *not* round-to-nearest. The same
idiom is documented in `src/game/ship_ai_behaviors.cpp` (`RoundedAxisDistanceSquared`).

A genuinely different pattern, `ROUND(f) + (0 < frac)`, nets to ceil and
must not be confused with this one. `ROUND(f)` alone (no correction) is
round-half-even.

## Detection recipe

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

## Confirmed sites

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

## Ghidra names

- Renamed 0x00469100 -> `Ship_ComputeTradeInValue` (it includes the ship
  hull term) and corrected its plate comment.
- Added `OutfitDef +0x378 persistent_on_ship_swap` (byte; loader sets it
  from flags bit 0x0004).
- Renamed shared literal-pool doubles `kFloatConst_0_25` ->
  `g_dbl_shared_0p25` and `g_dbl_rocket_max_range_scale_0p5` ->
  `g_dbl_shared_0p5`, retyped `double`, added use-list pre-comments.
- Corrected plate comments on 0x0049d640 and 0x0046e950 (truncation, not
  rounding).

## Verified sites

Original functions confirmed to compile the conversion with the FIST
truncation idiom:

### Sites using the idiom

- `ship_ai_state.cpp` `Ship_UpdateShipAiState` 0x00405590 (scripted-asteroid
  threshold); `ship_ai_controls.cpp` `Ship_ApplyShipAiControls` 0x00408150
  (entry desired-heading sync, leader copy/delta, evasive +/-135,
  velocity-match mode 0xc, mode-0xf
  copy); `ship_ai_weapons.cpp` `Weapon_SelectWeaponBankForCurrentTarget`
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
  `Frame_AddCombatRatingPoints` 0x0046f1e0; `Shot_ResolveShipHitFromWeapon`
  0x004192d0 (disable-armor); `Weapon_ApplyWeaponOnHitEffects` 0x0046f3f0
  (ionization points); `Weapon_SpawnWeaponImpactEffectPackage` 0x00462550
  (yield boxes); and the collision-mask mirrors in the local
  `RefreshCollisionMasks` (matching 0x00436910 / 0x0042c1b0).
- `boarding_plunder.cpp` `Player_HandleBoardTargetCommand` 0x0045a3d0
  (heading gate) and `NovaUi_RunBoardingPlunderWindow` 0x00482940 (fuel fill).

### False positives

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

### Detection caveat

The full-dump grep plus nearest-citation heuristic misses sites whose function
header names the original without the literal `Ghidra 0x` prefix, and it
mis-tags functions whose nearest citation differs from the code being ported.
Verify against the specific expression in the dump, not just the enclosing
function.
