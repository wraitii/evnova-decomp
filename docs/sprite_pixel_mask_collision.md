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
| `Stellar_HandleShipStellarCrash` | 0x0043aed0 | not ported (shares the helper) |
| `Shot_ResolveCollisions` stellar branch (flags_secondary 0x400) | 0x00437e20 | not ported (shares the helper) |

So the "other targets" that use the same sprite-mask path are **fatal
stellars** (`availability_flags & 0x100`), the **freeflight-object mining
scoop**, and the **flags_secondary 0x400 shot-vs-stellar** contact. Only the
ship and asteroid arms are reimplemented so far.

## Which test is used

`Ship_HandleSpritePairCollision` (0x004374f0) deliberately falls back to the
cheap bounding circle:

- `Sprite_TestBoundingCircleOverlap` (0x00475be0): `half = (bottom - top) / 2`,
  frame centre = `(left + half, top + half)`, and a **strict** `<` on squared
  distance against `(halfA + halfB)^2`.
- The circle is used when the average frame tick scale is at/above
  `_DAT_005754c8` (2.0) **or** `Sprite_GetShotHalfSpan` returns `< 0x21`.
- `Sprite_GetShotHalfSpan` (0x00462390) actually returns the **full** frame
  height (`bottom - top`), default `0x20`. It is also the blast span source in
  `Shot_ResolveCollisions` (`blast_radius + span * 0.333`).
- The asteroid callback (0x00436f70) has no such threshold: it always uses the
  pixel-mask test.

The clean-room reproduces this decision: `ShipContactUsesPixelMask`
(`src/game/collision.cpp`) selects the mask iff
`GameState::last_frame_tick_scale < 2.0` **and** the target ship mask height
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
  (0x00436910) pass `(ceil(height/2), ceil(width/2))` — the two span helpers
  are used on the opposite axes.
- `Shot_HandleShot` (0x00435830) passes `ceil(height/2)` on both axes.

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
- `Sprite_GetShotHalfSpan` returns the full frame height despite the name, and
  ship/asteroid placement uses it on the x axis and
  `Sprite_GetFrameVerticalHalfSpan` (full width) on the y axis.
- The mask is chosen only when `g_avg_frame_tick_scale < 2.0` and the target
  ship's frame height is `> 0x20`; both boundaries are reproduced (tests in
  `tests/sprite_mask_test.cpp`).
- The effective collision anchor is the pre-subtracted half-span described
  above, not a class-defined centre of mass: the ship/asteroid sheet frames
  carry a zero anchor and the callers subtract `(height/2, width/2)`.
- Ship masks cover the base hull only, not the engine-glow / weapon overlays.

## Validation

```sh
cmake --build build/release
ctest --test-dir build/release --output-on-failure
ctest --test-dir build/release --output-on-failure -R 'pixel mask|collision mask|asteroid mask|transparent'
cmake --build build/debug
```
