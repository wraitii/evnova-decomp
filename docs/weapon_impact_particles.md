# Weapon impact particles (SWParticles)

Reverse-engineering notes for the single-pixel soft-particle pool used by
weapon impacts, asteroid destruction debris, and (not yet ported) shot smoke
trails. Ground truth: `Weapon_SpawnWeaponImpactParticleBurst` 0x004274d0,
`SWParticles_*` 0x0047b7c0..0x0047c800, `Weapon_SpawnWeaponImpactEffectPackage`
0x00462550, and the `WeaponDef`/`AsteroidDef` loaders in 0x004bd3c0.

## Pool record (`SWParticle`, 0x2c stride)

| offset | field | meaning |
| --- | --- | --- |
| +0x00 | life | active while > 0; decremented once per tick, frees at 0 |
| +0x04/+0x08 | pos_x/pos_y | 8.8 fixed world position |
| +0x0c/+0x10 | prev_x/prev_y | previous screen position (dirty-pixel restore) |
| +0x14/+0x18 | vel_x/vel_y | 8.8 fixed velocity per tick |
| +0x1c/+0x20 | (unused) | initialised to -1 |
| +0x24 | blend_mode | surface blend selector (0x20 from all weapon callsites) |
| +0x28 | color | packed 0x00RRGGBB (palette index / 15-bit in 8/16-bit modes) |

`SWParticles_AllocatePool(100000, 0)` (from `SpriteWorld_InitializeFlightView-
SpritePools` 0x004abd8d) allocates a 100000-entry pool with **gravity 0**, so
`vel_y` is constant. The port replaces the fixed pool with a growable
`std::vector<SwParticle>` in `GameState`; slot-reuse order is not
gameplay-visible.

## Emitter `Weapon_SpawnWeaponImpactParticleBurst` (0x004274d0)

Signature: `(short x, short y, float speed, short scatter, short life_base,
short life_max, uint color, short blend_mode, short count, short position_scatter)`.

Per particle, in this exact RNG order:

1. If `0 < scatter < 100`: `speed *= (100 - scatter + rand(2*scatter+1)) * 0.01`.
2. heading = `rand(360)` game degrees (0 = up, clockwise; `Math_AddPolarVelocity`
   0x0043b4a0 applies `+sin` to x and `-cos` to y).
3. `life = rand(life_max - life_base + 1) + life_base` when `life_base < life_max`.
4. If `position_scatter > 0`: offset the spawn point by a vector whose
   magnitude is `rand(position_scatter * 100) * 0.01` at a second `rand(360)`
   heading.

Velocity and position are converted to the pool's 8.8 fixed point by
multiplying by 256.0f (`DAT_00575270`).

Weapon callers use the weapon's `HitParticles`/`HitPartLife`/`HitPartVel`/
`HitPartColor` fields (`impact_particle_count` / `impact_particle_frame_base` /
`impact_particle_speed` / `impact_particle_color`, payload +0x4c..+0x52) with
`life_max = round(frame_base * 1.25)` (double `DAT_005753e0`):

| callsite | scatter | function |
| --- | --- | --- |
| `Shot_ResolveShotCollisionHit` 0x00437780 | 0x14 | `ResolveShotCollisionHit` |
| `NovaUi_ResolveWeaponSplashImpact` 0x00436ff0 | 0x14 | `ResolveAsteroidSplashImpact` |
| `Shot_ResolveCollisions` 0x00437e20 (stellar) | 0x14 | `ResolveShotStellarContact` |
| `Shot_UpdateBeamHitQueue` 0x0042f270 | 0x19 | `NovaWeapon_ResolveDirectWeaponHit` |

## Asteroid destruction debris (0x00462550)

`ResolveAsteroidDestructionPackage` emits the debris burst between the
`YieldQty` resource-boxes and the destruction area effect, matching the original
RNG order:

* speed `DAT_00575740` = 0.2
* scatter 0x28
* life `[0xf0, 0x1e0]`
* color = the asteroid row tint at +0x18 (packed 555, expanded to 24-bit)
* count = `AsteroidDef.field_0x0c` (payload +0x08)
* position scatter = `Sprite_GetFrameFullHeight(asteroid) / 3`; every shipped
  asteroid set (800..815) is 50x50, so the port uses 16.

## Port

* `src/game/impact_effects.cpp` — `NovaEffects_SpawnWeaponImpactParticleBurst`,
  `NovaEffects_SpawnWeaponImpactBurstForWeapon`, `NovaEffects_TickSwParticles`.
  The original `SWParticles_Update` runs once per *rendered frame* from the
  present hook (`Frame_PresentViewportAndParticles` 0x00439d40) with no FPS cap
  (`SpriteWorld_SetTargetFps(surface, 0)`), so life/motion are frame-rate
  dependent. The port pins this to a fixed 60 updates/s (two per 30 Hz sim
  tick) so it is frame-rate independent and matches the original at 60 Hz:
  asteroid debris (240-480 ticks) lives ~4-8s.
* `src/game/spaceflight_view.cpp` — `SpaceflightView::DrawSwParticles` (1x1
  logical-pixel SDL point; SDL expands it by the display density, so retina
  gets a 2x2 backing block). The color is written opaque, matching the
  original's 24-bit branch.
* `GameState::sw_particles` + `sw_particle_tick_accumulator`; cleared by
  `NovaWeapon_ClearTransientCombatState`.

Cadence divergence: the port pins the per-frame update to 60/s instead of
following the actual refresh rate (the original at 120 Hz would age/move twice
as fast).

Divergences: the original's 8/16-bit branches blended the particle over the
saved backdrop with a 0..0x20 life weight; the 24-bit branch (the analog of
SDL's 32-bit target) writes the color opaquely, which is what the port does.
The dirty-pixel save/restore pass (`SWParticles_UpdateDirtyPixels`
0x0047c3a0 / `SWParticles_RestoreSavedPixels` 0x0047bb30) is not reproduced.

Note on apparent size: the particle is one *logical* pixel on the 1024x768
world. Because spaceflight extends the world 1:1 rather than upscaling the
1024x768 canvas, a large retina window shows more system and leaves the
particle small relative to the screen (the same applies to all 1:1 art).

## Not yet ported

`Shot_HandleShot` 0x00435830 smoke trails (`_DAT_...` weapon fields
`field_0x60`/`0x62`/`0x64` + the 8-entry color table at +0x8c, gated by the
"SmokeTrails" preference and `0x20 - sprite intensity`). Separate feature; the
`Weapon` struct does not decode those fields yet.
