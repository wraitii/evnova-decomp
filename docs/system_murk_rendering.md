# System murkiness rendering

Clean-room notes for the in-flight murk (visibility/distance fog) system. Murk
is a per-system property (`s\xd8st` +0x92 → runtime `SystemDef` +0xbc, port
`System::murk`, signed 0..100); a negative value hides the ambient starfield.
It has three visible effects: a distance fog on world sprites, a haze tint on
the ambient starfield, and a cap on NPC engine glow. There is **no** full-screen
murk overlay. The `SWParticle` point system (weapon sparks, asteroid debris) is
left unfogged by the original; the port adds a gated `BUGFIX(original)` for it
documented with the pool in `docs/weapon_impact_particles.md`. The starmap `n\x91bu` "nebula" regions are unrelated (they are
map backdrops, see `starmap_render.cpp`).

## Effective murk

`System_GetEffectiveMurkPercent` (0x0046c250):

    murk = max(SystemDef.murk, 0)
    for each outfit slot with owned_count > 0:
        for each of the four (ModType, ModVal) pairs:
            if ModType == 0x1c:            # 28 = MurkMod
                murk += owned_count * ModVal
    return clamp(murk, 0, 100)

The original caches the result in `g_distance_intensity_scale` (0x7356bc) from
`Outfit_RecomputeOutfitDerivedState` (0x0046d4b0). The port recomputes it on
demand (`NovaSystem_GetEffectiveMurkPercent`, `src/game/travel.cpp`) once per
`SpaceflightView::Draw`, so it follows the current system without depending on
the original recompute call sites. ModType 0x1c is `OutfitEffect::kMurkMod`
(`src/game/outfit.hpp`).

## Per-sprite distance fog

`Frame_UpdateSpriteDistanceIntensity` (0x00438db0) sets two `Sprite` fields
consumed by the blitter:

    distSq = trunc(|player.x - (int)sprite.x|)^2 + trunc(|player.y - (int)sprite.y|)^2
    distance_brightness = clamp(trunc(effective_murk * distSq * 1.2e-05), 0, 0x1f)

(The original receives the sprite's world position as shorts and truncates each
coordinate before the subtraction; the player position stays float.)

- `g_distance_intensity_scale_const2` (0x005754d0) = `1.2e-05` (binary64).
- The `ROUND()` markers are the x87 FIST + residual/sign idiom, i.e. truncation
  toward zero (`docs/x87_precision.md`).
- At 8-bit colour depth the ceiling is 0x18; every SDL texture is 32-bit, so the
  port uses 0x1f.
- It also copies `SystemDef.space_color` (+0x1f8, RGB555 packed from
  `BkgndColor`) into `Sprite.space_color` (+0xac).

The fog itself is applied by `BlitPixel_TintRgb15Span` (0x004736c0) through the
tinted RLE path (`SpriteRleCommandStream_BlitTintedRgb15` 0x00472900) and the
per-frame dispatch in `BlitPixie_BlitRectRawCopy` (0x004711e0). Per pixel, with
`d = distance_brightness`, `br = brightness_level`, `sc = space_color`:

    f   = src*(32-d)/32 + sc*d/32          # fog the source toward space colour
    out = f*(tint+32-br)/32 + dst*br/32    # then the ordinary tint/alpha blend

For an opaque hull (`br == 0`) this collapses to `out = f`, so a distant object
becomes the system background colour; at `d == 31` it is essentially invisible.
Effect layers set `br == 32`, so `out = dst + f*tint/32` (additive).

Callers of `Frame_UpdateSpriteDistanceIntensity` (all sprite layers get fog):
`Ship_UpdateVisualState`, `Stellar_UpdateStellarSprites`,
`Frame_UpdateFreeflightObjectSprites`, `Shot_HandleShot`,
`Shot_UpdateWeaponSmokePuffs`, `Shot_UpdateImpactEffectSprites`,
`Asteroid_UpdateSprites`, `Frame_UpdateFadingEffectSprites`.

## Ambient starfield tint

`Frame_UpdateViewportWrapBackgroundSprites` (0x0042e590) writes a raw-murk tint
(not the effective murk) into each star sprite:

    murk == 0 -> brightness 0x20, tint 0x20 (plain opaque-copy fast path)
    else      -> t = clamp(trunc(murk * 0.9), 2, 29)   # 0.9 at 0x005753c0
                 brightness 0x20, distance 0, tint r=g=b=t

At 16-bit depth the tinted path makes this additive (`dst + src*t/32`); at
8-bit depth it instead sets brightness `t` with tint 0 (an indexed dim). The
port uses the 16-bit additive path. `murk < 0` clears the starfield at spawn
(`NovaEffects_ClearAmbientStarParticles` 0x0046ede0).

## NPC engine-glow cap

`Ship_UpdateVisualState` (0x00428340) reduces NPC glow by the hull sprite's
distance brightness:

    level = engine_glow_level + NovaRandom_Range(6) - 4
    if level < 2: hide
    level = min(level, trunc(32 - distance_brightness*1.5)); clamp level >= 0

(`1.5` at 0x00575340, `32.0` at 0x00575348.) The player is always at distance 0,
so the cap only bites NPCs. The glow sprite also inherits the hull's
distance_brightness/space_color, so the draw fog applies on top of the cap.

## Weapon/impact particles (BUGFIX)

The single-pixel `SWParticle` pool (`SWParticles_DrawParticles` 0x0047bdd0) gets
no murk treatment in the original; the port's gated `BUGFIX(original)` alpha dim
is documented in `docs/weapon_impact_particles.md`.

## Rendering divergences

**SDL rendering.** The fog mixes toward a *constant* (`space_color`), not toward
`dst`, so for an opaque sprite it replaces whatever is behind it. The port
reproduces that in `BlitFrame` with a two-pass draw over the frame's alpha
silhouette (`SpriteFrame.white_silhouette`, uploaded for ship sheets and, for
the distance fog, every spin set):

1. draw the silhouette tinted with `space_color` (occludes `dst`), then
2. draw the source at `(1 - d/32)` of its own alpha over it.

For an opaque pixel this is exactly `src*(1-d/32) + space_color*(d/32)`. When no
silhouette is available it degrades to a source-alpha fade over `dst` (the
overlap divergence). Additive layers (glow / lights / weapon effects) never use
the silhouette: the original's `dst + f*intensity/32` is reproduced as an
additive draw with `(1-d/32)` folded into the intensity.

**Snap at the ceiling.** `distance_brightness` is clamped to `0x1f` (31). The
original's `BlitPixel_TintRgb15Span` stage-1 is `(src*(32-d) + sc*d) >> 5` in
5-bit channels, so at `d == 0x1f` the `src*(1) >> 5` term truncates to 0 and the
pixel collapses to `space_color` (a ship at maximum fog is invisible against
space). The port's 8-bit pipeline would otherwise leave a ~1/32 residual, so
`BlitFrame` snaps `d >= 0x1f` to fully fogged.

**Approximation.** The exact 5-bit `>> 5` per-channel truncation is not
reproduced (SDL_Renderer exposes no custom shader); the port does the mix in
8-bit, so mid-range fog is at most one 5-bit step brighter than the original.
The endpoint is exact via the snap. `kApplyOriginalBugFixes` does not apply to
any of this (ordinary SDL/platform divergences); it only gates the particle
`BUGFIX(original)` in `docs/weapon_impact_particles.md`.

**Not modelled:** the 8-bit-depth star tint branch. The `space_color` field
itself is not stored; the fog targets `System::bkgnd_color`, which is what the
backdrop is drawn with, so a fully fogged sprite matches the background.
