# Sprite pixel-mask collision (direct weapon contact)

Reverse-engineering notes for the original opaque-pixel overlap path used by
direct weapon contact, and the clean-room replacement in
`src/game/sprite_mask.{hpp,cpp}` / `src/game/collision.cpp`.

## Original path

Direct shot contact in `Frame_TickSystems` scope 9 does not use a distance
formula. The broad phase (`Ship_TestSpriteLayerOverlaps`, 0x004772d0) reports
each sprite pair whose AABBs overlap, and the per-entity callback then runs a
precise opaque-pixel test:

1. `Sprite_TestPixelMaskOverlap` (0x00475c80) intersects the two sprites'
   current-frame bounds and rewrites the intersection rectangle into each
   sprite's frame-local space. Sprite bounds fields (verified from
   disassembly): `+0x1a` left, `+0x1c` top, `+0x1e` right, `+0x20` bottom;
   prepared frame command stream at `+0x50`; current frame index at `+0x68`.
2. `BlitPixie_CopyRectRegionBetweenFrames` (0x00471800) asserts the rectangles
   have equal widths/heights, scales the row offsets by each frame's
   pixel-stride shift (`frame+0x5e`), and calls
   `SpriteRleCommandStream_SkipToRowCount` (0x00472190).
3. `SpriteRleCommandStream_SkipToRowCount` walks both RLE command streams and
   returns true the moment both current runs are opaque. Command bytes:
   `0` terminator, `1` end-of-row, `2` opaque/data run, `3` transparent run.

The result is a pure boolean: "is there any pixel that is opaque in both
frames once their anchors are aligned?".

### Callers of the shared helper

`Sprite_TestPixelMaskOverlap` is shared by four collision paths:

| caller | address | clean-room status |
|--------|---------|-------------------|
| `Ship_HandleSpritePairCollision` (shot layer) | 0x004374f0 | ported (mask-first) |
| `Ship_HandleSpritePairCollision` (freeflight scoop layer) | 0x004374f0 | TODO(decomp) |
| `Asteroid_HandleSpritePairCollision` | 0x00436f70 (internal label) | ported (mask-first) |
| `Stellar_HandleShipStellarCrash` | 0x0043aed0 | ported |
| `Shot_ResolveCollisions` stellar branch (flags_secondary 0x400) | 0x00437e20 | ported |

So the "other targets" that use the same sprite-mask path are **fatal
stellars** (`availability_flags & 0x100`), the **freeflight-object mining
scoop**, and the **flags_secondary 0x400 shot-vs-stellar** contact. The two
stellar arms are now reimplemented; only the freeflight-object scoop remains.

## Which test is used

`Ship_HandleSpritePairCollision` (0x004374f0) deliberately falls back to the
cheap bounding circle:

- `Sprite_TestBoundingCircleOverlap` (0x00475be0): `half = (right - left) / 2`
  (the full frame WIDTH, `Sprite_GetFrameFullWidth`), reused on both axes;
  frame centre = `(left + half, top + half)`, and a **strict** `<` on squared
  distance against `(halfA + halfB)^2`.
- The circle is used when the average frame tick scale is at/above
  `_DAT_005754c8` (2.0) **or** `Sprite_GetFrameFullWidth` returns `< 0x21`.
- `Sprite_GetFrameFullWidth` (0x00462390) actually returns the **full** frame
  width (`right - left`), default `0x20`. It is also the blast span source in
  `Shot_ResolveCollisions` (`blast_radius + span * 0.333`).
- The asteroid callback (0x00436f70) has no such threshold: it always uses the
  pixel-mask test.

The clean-room reproduces this decision: `ShipContactUsesPixelMask`
(`src/game/collision.cpp`) selects the mask iff
`GameState::last_frame_tick_scale < 2.0` **and** the target ship mask width
`> 0x20`; asteroids always use a resolved mask. `last_frame_tick_scale` is the
port's stand-in for `g_avg_frame_tick_scale` (0x00735448), re-asserted at the
top of `NovaFrame_TickSystems` and in `NovaWeapon_TickShots` (normalized 30 Hz
tick scale; default 1.0).

## Frame selection (rotation / scale / current frame)

The original rotates a sprite by choosing a pre-rendered frame, so the mask is
just the selected frame's opaque bitmap:

- **Ships**: `Ship_UpdateVisualState` (0x00428340) composes
  `row * frames_per_rotation + heading_frame`; banking classes
  (`sh\x8an` Flags bit 0) pick row 1/2 from `ai_turn_bias_dir`.
- **Shots**: `Shot_HandleShot` (0x00435830) uses the static heading frame
  (flags_primary bit 0 clear) or the stepped animation frame
  (`frame_cycle_index`).
- **Asteroids**: `Asteroid_UpdateSprites` (0x00436910) wraps
  `wander_frame_accumulator` by the spin set frame count.

Scaling is not applied to in-flight hull/shot/asteroid sprites in the shipped
game. Rotation is handled entirely by the pre-rendered frame, so no resampling
helper is on the gameplay path.

### Effective anchor

The prepared collision frame's stored anchor is `(0,0)` for the multi-frame
ship/asteroid sheets: `SpriteFrame_CreateFromRect` (0x00476400) zeroes
`+0x2a/+0x2c`. The callers pre-subtract the half-span, so the effective
top-left is `world - (half_a, half_b)`:

- `Ship_UpdateVisualState` (0x00428340) and `Asteroid_UpdateSprites`
  (0x00436910) pass `(ceil(width/2), ceil(height/2))` — the full width on x,
  the full height on y.
- `Shot_HandleShot` (0x00435830) passes `ceil(width/2)` on both axes.

For the shipped square frames (24x24 shuttle hull, 50x50 asteroids) this is the
frame centre. `BindEntityMask` sets these effective anchors from the decoded
mask dimensions, so the non-square case is represented too.

## Clean-room mapping

`src/game/sprite_mask.{hpp,cpp}`:

- `SpriteMask` — row-major `uint8` opaque bitmap, the clean-room analogue of
  the prepared-frame RLE stream.
- `SpriteMask_FromRgba` — alpha `>=` threshold (default 1) is opaque. This is
  how the decoded `rl\x91D` RGBA frames feed collision without SDL.
- `SpriteMask_TestOverlap` — intersects the anchored frame bounds and scans for
  a shared opaque pixel; mirrors 0x00475c80 + 0x00471800 + 0x00472190.
- `SpriteMask_TestBoundingCircleOverlap` — mirrors 0x00475be0.
- `CollisionMaskBinding` — a non-owning `const SpriteMask*` plus the effective
  frame anchor and selected frame index.
- `CollisionMask_TestContact` — mask overlap when both bindings are resolved
  (and `allow_pixel_mask` is true), else the circle fallback.
- `SpriteMaskStore` — non-SDL lazy cache of frame masks keyed by the renderer's
  resource ids (`Spin` for `sp\x9an` tiles, `Sheet` for bare `rl\x91D` ship
  sheets). A failed load is cached as empty.

`src/game/collision.cpp`:

- `RefreshCollisionMasks` resolves the current-frame mask for every active
  ship (via `sh\x8an` base image id), shot (weapon `sprite_id + 3000`), and
  asteroid (`kAsteroidSpinBase + type`) immediately before the direct-contact
  pass, mirroring the renderer's frame selection. `NovaCollision_RefreshCollisionMasks`
  is the public test seam for it.
- `ResolveDirectAsteroidContact` and the ship loop in
  `NovaWeapon_ResolveDirectShotCollisions` call `CollisionMask_TestContact`;
  the ship loop passes `ShipContactUsesPixelMask` as `allow_pixel_mask`.
- `GameState::collision_masks_enabled` (default true) lets collision *logic*
  tests stay on the circle envelope; the pixel-mask path has dedicated tests.

## Confirmed quirks / open questions

- Circle fallback uses a strict `<`, so a contact exactly at the sum of the
  half-spans is a miss.
- `Sprite_GetFrameFullWidth` returns the full frame width and ship/asteroid
  placement uses it on the x axis; `Sprite_GetFrameFullHeight` returns the full
  frame height and is used on the y axis.
- The mask is chosen only when `g_avg_frame_tick_scale < 2.0` and the target
  ship's frame width is `> 0x20`; both boundaries are reproduced (tests in
  `tests/sprite_mask_test.cpp`).
- The effective collision anchor is the pre-subtracted half-span described
  above, not a class-defined centre of mass: the ship/asteroid sheet frames
  carry a zero anchor and the callers subtract `(width/2, height/2)`.
- Ship masks cover the base hull only, not the engine-glow / weapon overlays.

### Stellar callers (ported)

`Stellar_HandleShipStellarCrash` (0x0043aed0) and the `flags_secondary 0x400`
arm of `Shot_ResolveCollisions` (0x00437e20) are now ported to
`NovaStellar_HandleShipStellarCrash` and `ResolveShotStellarContact`
(`src/game/collision.cpp`). Both use the same decoded pixel masks via
`SpriteMask_TestOverlap` with no bounding-circle fallback, because the original
tests the prepared frames directly:

- **Stellar ambient sprite source.** StellarDef+0 is the live ambient `Sprite*`
  (set by the renderer); StellarDef+0x40 is the Strength *capacity* and +0x3c is
  the *live* Strength (Bible "Strength", combined mass+energy damage). The
  loader seeds both from `sp\x9ab` payload +0x23c. The clean-room maps these to
  `Stellar.strength_capacity` and `Stellar.strength`; the earlier
  `sprite_population` / `sprite_handle_active` names were a misreading of those
  same words and have been removed. Masks bind from spin set
  `NovaTargeting_StellarSpriteLinkId(st) + 1000`: `link_a_id` normally,
  `link_b_id` when the body is active (destroyed/engaged) and link_b is set.
  `Stellar.sprite_current_frame` (Ghidra +0x476) is published by
  `SpaceflightView::AdvanceStellarAnimation`, which mirrors
  `Stellar_UpdateStellarSprites` frame selection.
- **Visible/collision coherence.** `NovaTargeting_StellarSpriteLinkId`
  (targeting.cpp) centralises the link_a/link_b choice. The view's animation
  advance, `DrawStellarBodies`, `PickStellarAt` and the collision refresh all
  call it, so the sprite the player sees and the collision mask can never
  select different zones. A single animation state drives both.
- **Anchor.** Stellar placement pre-subtracts the same
  `(ceil(width/2), ceil(height/2))` half-span as ships/asteroids
  (`BindEntityMask`'s non-shot placement).

#### Fatal stellar crash (0x0043aed0)

Eligibility: `availability_flags & 0x100` (Bible "Stellar is deadly") *and* a
resolved ambient mask. Ships are tested in slot order against each of the 16
current-system nav stellars. Excluded: inactive, already destroyed
(`death_timer_active > 0 || armor <= 0`) and crash-immune ships.

`Stellar_ShipImmuneToStellarCrash` (0x0046e210, ported): class
`availability_flags & 0x20`, or (player only) an owned outfit with ModType
0x2a. Distinct from stellar gravity shielding - the NPC `flags_secondary 0x40`
(inertialess) gate and ModType 0x26 (inertial dampener)/0x29 (gravity
resistance) do *not* count.

Consequences are an instant, animation-free kill: `is_active = 0`,
`armor_points = -1000.0f` (0xc47a0000), `death_timer_active = 0`; the player's
primary target is cleared when it was the victim; and the class's **final**
explosion effect (`ShipClassDef+0xa1e`, i.e. `destruction_effect_final`) is
spawned at the ship. The ship loop does not early-out.

#### Planet-type weapon vs. stellar (0x00437e20)

Gate: `weapon.flags_secondary & 0x0400` (Bible "planet-type weapon"). A body is
eligible when `strength_capacity > 0` and `NovaTargeting_IsStellarActive` is
false (i.e. not destroyed and not engaged). A hit subtracts
`energy_damage + mass_damage` from `strength`, spawns the weapon area impact,
and kills the shot. When strength goes negative the destruction package runs:
ExplodType effect at the map position, the OnDestroy reaction script, then
`strength = -1` and `destroyed_days_remaining = schedule_days`; player-owned shots also
fire 10 `Government_ProcessFactionCombatEvent` events (still deferred in the
clean-room). `NovaTargeting_IsStellarActive` reads `strength < 0 ||
destroyed_days_remaining > 0` with `strength_capacity > 0`, matching 0x0046e3c0. Daily
regeneration (`Mission_TickDailyWorldUpdate` 0x00466f26) restores `strength`
from the capacity. `availability_flags 0x40` (Bible "starts the game
Destroyed") is applied at new-game reset by
`NovaNewPilot_ResetStellarStrengthForNewGame` (Ghidra `Game_ResetNewGameState`
0x004b4690, 0x004b46bc..0x004b4760).
