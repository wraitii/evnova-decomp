# x87 FIST truncation idiom - audit handoff

Status: first pass complete. This note records the idiom, the
confirmed fixes, and the remaining candidates so the sweep can resume.

## The idiom

MSVC compiles an implicit `(int)` / `(short)` cast of a float as an x87
`FIST` (round-to-nearest) followed by a residual/sign correction whose
integer form is `ADD reg,0x7fffffff; SBB out,0` plus a signed branch. The
net result is **truncation toward zero**, *not* round-to-nearest. The same
idiom is documented in `src/game/ship_ai.cpp` (`RoundedAxisDistanceSquared`).

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

## Fixed in commit f6abb9f

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

## Remaining candidates

Automated heuristic (rounding call whose nearest citation uses the idiom),
upper bound -- verify each against the dump before changing. `negotiation`
L885 is a false positive (helper now truncates).

Total: 32 sites across 7 files.

### src/game/ship_ai.cpp
- L2776 [00405590 Ship_UpdateShipAiState] `std::round((kScriptAlignAddend - base_turn) * kScriptTurnAddend)));`
- L3163 [00408150 Ship_ApplyShipAiControls] `static_cast<std::int16_t>(WrapDeg(std::round(ship.heading / kDegToRad)));`
- L3458 [00408150 Ship_ApplyShipAiControls] `static_cast<std::int16_t>(std::round(leader_heading_deg));`
- L3475 [00408150 Ship_ApplyShipAiControls] `std::round(leader_heading_deg),`
- L3636 [00408150 Ship_ApplyShipAiControls] `static_cast<int>(std::round(cur_deg) +`
- L3640 [00408150 Ship_ApplyShipAiControls] `wrap_deg_int(static_cast<int>(std::round(cur_deg)));`
- L4002 [00408150 Ship_ApplyShipAiControls] `std::remainder(std::round(target_heading_deg) - std::round(cur_deg),`
- L4010 [00408150 Ship_ApplyShipAiControls] `static_cast<std::int16_t>(std::round(target_heading_deg));`
- L4013 [00408150 Ship_ApplyShipAiControls] `std::remainder(std::round(target_heading_deg) - std::round(cur_deg),`
- L4015 [00408150 Ship_ApplyShipAiControls] `std::int16_t desired = static_cast<std::int16_t>(std::round(cur_deg));`
- L4100 [00408150 Ship_ApplyShipAiControls] `static_cast<std::int16_t>(std::round(target.heading / kDegToRad));`
- L5571 [0040ce00 Weapon_SelectWeaponBankForCurrentTarget] `static_cast<std::int16_t>(std::lround(heading_deg_f));`
- L5609 [0040ce00 Weapon_SelectWeaponBankForCurrentTarget] `std::lround(std::abs(static_cast<float>(bearing) - heading_deg_f));`

### src/game/weapon.cpp
- L1079 [0046c320 Weapon_SelectTurretQuadrant] `static_cast<int>(std::lround(`
- L1787 [00455150 Weapon_FirePlayerWeaponBank] `static_cast<std::int16_t>(std::lround(heading_deg)));`
- L1795 [00455150 Weapon_FirePlayerWeaponBank] `static_cast<std::int16_t>(std::lround(BearingDeg(`
- L1801 [00455150 Weapon_FirePlayerWeaponBank] `static_cast<std::int16_t>(std::lround(`
- L2446 [0042f270 Shot_UpdateBeamHitQueue] `std::lround(ship.heading * (180.0F / 3.14159265358979323846F))));`
- L2464 [0042f270 Shot_UpdateBeamHitQueue] `static_cast<std::int16_t>(std::lround(tb)));`

### src/game/spaceflight_view.cpp
- L1147 [00436910 Asteroid_UpdateSprites] `int frame = static_cast<int>(std::lround(wander));`
- L1528 [00436910 Asteroid_UpdateSprites] `std::clamp(static_cast<int>(std::lround(instance.anim_time)),`
- L1887 [0042ede0 NovaUi_UpdateShipTargetReticle] `const float off = std::ceil(full * 0.5F) + std::round(pulse);`
- L2006 [0042ede0 NovaUi_UpdateShipTargetReticle] `const float off = std::ceil(full * 0.5F) + std::round(pulse);`

### src/game/spaceflight.cpp
- L1048 [0044aa70 Ship_HandlePlayerShipCore] `const auto whole_ticks = static_cast<int>(std::round(countdown));`
- L1052 [0044aa70 Ship_HandlePlayerShipCore] `const auto whole_seconds = static_cast<int>(std::round(seconds));`
- L4117 [0044aa70 Ship_HandlePlayerShipCore] `int heading_deg = static_cast<int>(std::lround(bearing_deg));`

### src/game/collision.cpp
- L2102 [00437e20 Shot_ResolveCollisions] `std::lround(std::abs(asteroid.target_pos_x - shot.pos_x)));`
- L2104 [00437e20 Shot_ResolveCollisions] `std::lround(std::abs(asteroid.target_pos_y - shot.pos_y)));`
- L2176 [0042f270 Shot_UpdateBeamHitQueue] `static_cast<std::int16_t>(std::lround(`

### src/game/boarding_plunder.cpp
- L608 [0045a3d0 Ship_HandlePlayerBoardTargetCommand] `std::llround(radians * (180.0F / 3.14159265358979F)));`
- L1836 [00482940 NovaUi_RunBoardingPlunderWindow] `int fill = static_cast<int>(std::llround(available));`

### src/game/negotiation_dialog.cpp
- L885 [00480030 NovaUi_RunTravelDestinationInteractionWindow] `RoundDouble(static_cast<double>(bribe_cost) * kGovtBribeCostMultiplier);`

## Ghidra DB changes made this pass

- Renamed 0x00469100 -> `Ship_ComputeTradeInValue` (it includes the ship
  hull term) and corrected its plate comment.
- Added `OutfitDef +0x378 persistent_on_ship_swap` (byte; loader sets it
  from flags bit 0x0004).
- Renamed shared literal-pool doubles `kFloatConst_0_25` ->
  `g_dbl_shared_0p25` and `g_dbl_rocket_max_range_scale_0p5` ->
  `g_dbl_shared_0p5`, retyped `double`, added use-list pre-comments.
- Corrected plate comments on 0x0049d640 and 0x0046e950 (truncation, not
  rounding).
