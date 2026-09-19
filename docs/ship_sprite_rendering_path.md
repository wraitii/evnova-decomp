# Ship sprite rendering path (metadata notes)

Clean-room notes that guide reimplementing the in-flight ship sprite renderer on
top of SDL3. These capture the decompiled `sh\x9an` (ship animation) resource
descriptor, the six per-class sprite-handle tables, and the typed `Sprite`
runtime object, all of which were named in the Ghidra DB during the ship-visual
metadata pass.

## Resource families

Ship art lives in the `Nova Ships *.rez` archives (NOT the Nova Data archives):

| archiver | rl\x91D (16-bit RLE) | sh\x9an (ship animation descriptor) |
|----------|----------------------|--------------------------------------|
| Nova Ships 1 | 41 | 16 |
| Nova Ships 2 | 37 | 17 |
| Nova Ships 3 | 30 | 10 |
| Nova Ships 4 | 43 | 22 |
| Nova Ships 7 | (some) | 223 |

Until `Nova Ships *.rez` are added to `brgr_archive.cpp`'s `kArchiveFileNames`,
the ship sprite sheets cannot be reached; that is a prerequisite for rendering.

## The `sh\x9an` descriptor (Bible `shän`)

`ShipClass_LoadShipClassVisualAndLaunchData` (0x004b4ee0) loads
`sh\x9an` id `0x80 + class` via the resource accessor and reads these
big-endian fields (offsets verified against the Shuttle class 0x80 payload):

| offset | Bible field | ShipClassDef / note |
|--------|-------------|---------------------|
| +0x00   | BaseImageID | 1000 (Shuttle) -> base sprite resource id |
| +0x02   | BaseMaskID  | 1001 |
| +0x04   | BaseSetCount | 3 (frame multiplier sVar4, clamp >=1) |
| +0x06/+0x08 | BaseX/BaseYSize | 24x24 |
| +0x0a   | BaseTransp  | -> `base_transparency` (0..32) |
| +0x0c   | AltImageID  | -> alt sprite (g_ship_sprite_alt) when >0 |
| +0x10   | AltSetCount | -> `alt_sprite_cycle_count` |
| +0x16   | GlowImageID | -> g_ship_sprite_glow (engine glow) |
| +0x1e   | LightImageID | -> g_ship_sprite_light (running lights) |
| +0x26   | WeapImageID | -> g_ship_sprite_weapon (weapon effects) |
| +0x2e   | Flags        | -> `sprite_behavior_flags` (bank/unfold/carry bits) |
| +0x30   | AnimDelay    | -> `combat_state_init_range` (cycle dwell)          |
| +0x30   | AnimDelay    | -> `combat_state_init_range` |
| +0x32   | WeapDecay    | -> `weapon_glow_decay_rate` = WeapDecay * binary64 0.333 |
| +0x34   | FramesPer    | -> `frames_per_rotation` (default 36) |
| +0x36..+0x3e | Gun/Turret/Guided exit pos | `gun_exit_pos_x/y`, `turret_exit_pos_x/y`, `guided_exit_pos_x` |
| +0x40   | ShieldImageID | -> g_ship_sprite_shield (unconditional) |

Each sprite-role block also has a MaskID and X/Y size pair (e.g. glow mask +0x18,
glow size +0x1a/+0x1c). The record name (resource.map) is the ship class display
name.

## Per-class sprite tables (Ghidata symbols)

Six parallel `Sprite *[0x300]` handle tables (owned by `GameState` in the
reimplementation) hold each class's graphic sets; each has a matching
`g_ship_sprite_*_resource_id` array (stride 6) recording the source `sh\x9an`
resource id so shared graphics are deduplicated via
`ShipClass_FindShipSpriteSetByResourceId`:

| handle table | role | resource-id table |
|--------------|------|-------------------|
| g_ship_sprite_base   | primary rotating ship sprite | g_ship_sprite_base_resource_id |
| g_ship_sprite_alt    | alternating sprite cycle     | g_ship_sprite_alt_resource_id |
| g_ship_sprite_glow   | engine glow                  | g_ship_sprite_glow_resource_id |
| g_ship_sprite_light  | running lights               | g_ship_sprite_light_resource_id |
| g_ship_sprite_weapon | weapon effect sprites        | g_ship_sprite_weapon_resource_id |
| g_ship_sprite_shield | shield bubble                | g_ship_sprite_shield_resource_id |

The shield-bubble layer remains deferred in the SDL port. The descriptor does
not yet decode `ShieldImageID`/mask/X/Y at `+0x40..+0x46`, and the renderer does
not yet reproduce `Ship_UpdateVisualState`'s `shield_bubble_flash_intensity`
branch. Non-bypass hits already seed that field to 32; the missing branch caps
it to `clamp(round(shields * 32 / base_shields), 4, 32)`, uses the result as
the shield sprite RGB intensity, and subtracts `g_avg_frame_tick_scale`.

`ShipClass_FindShipSpriteSetByResourceId` scans the resource-id table; when a
sprite is already loaded for the same id it is cloned
(`Sprite_Clone`) rather than re-decoded, and `clone_source_ship_class`
remembers the source class.

## Typed `Sprite` object (0xd4 bytes)

Allocated by `Sprite_Create` (0x004744a0) and grown by
`Sprite_AddFrame`/`Sprite_PrepareFramesAndAttachResourceData`. The named fields:

| offset | name | meaning |
|--------|------|---------|
| +0x4C  | `frames`            | pointer array, frame_capacity entries |
| +0x54  | `num_frames`        | total populated frames (loader sums base+alt) |
| +0x58  | `frame_capacity`    | allocated frame count |
| +0x5C  | `current_frame_index` | currently displayed frame (init -1) |
| +0xA2  | `brightness_level`  | 0..32 intensity, set from class base_transparency |

Frame index by heading for rendering: map the ship's heading (radians) onto the
nearest of `frames_per_rotation` rotation frames (frame 0 = ship pointing up).

## Stellar (planet) graphics: the `sp\x9an`/Spin path

Stellar bodies (stars/suns/planets) are NOT `sh\x9an` ships - they use the
simpler `sp\x9an` (Spin) sprite family (Bible `spïn`; reserved ids 1000-1255 =
stellar objects). Naming done in the DB:

- `Spin_ReadDescriptor` (0x004b4e10) reads `sp\x9an` for a spin id:
  +0x00 SpritesID, +0x02 MasksID, +0x04 xSize, +0x06 ySize, +0x08 xTiles,
  +0x0a yTiles (frame count = xTiles*yTiles).
- `Spin_LoadSpriteSet` (0x004b0a30) loads id+1000 into `g_spin_sprite_sets`.
- `g_stellar_ambient_sprites` (0x00596c64) = the 0x10 per-stellar ambient
  sprite handles for the active system.
- `Stellar_UpdateStellarSprites` (0x0042cd10) picks the spin sprite set by
  active zone (link_a vs link_b), advances the ambient animation through the
  StellarDef dwell/count/current/previous/accumulator fields, drives an
  engage/pulse highlight on the selected travel target, then runs
  `Frame_UpdateSpriteDistanceIntensity` and positions each body relative to the
  player/camera (`map_pos - player_pos + g_viewport_center`).
- `Frame_UpdateSpriteDistanceIntensity` (0x00438db0) computes
  `Sprite.distance_brightness` (+0xAA) from player distance and copies
  `SystemDef.space_color` (+0x1F8) into `Sprite.space_color` (+0xAC).

### Distance/fog (Frame_UpdateSpriteDistanceIntensity)

`Frame_UpdateSpriteDistanceIntensity` (0x00438db0) sets
`Sprite.distance_brightness` (+0xAA) and copies `SystemDef.space_color` (+0x1F8,
5-5-5 from BkgndColor) into `Sprite.space_color` (+0xAC):

    distance_brightness = trunc(g_distance_intensity_scale * distSq * 1.2e-05)

where `distSq` is the squared truncated per-axis distance to the player and
`g_distance_intensity_scale` (0x7356bc) is the **effective system murk percent**
(0-100) rebuilt by `Outfit_RecomputeOutfitDerivedState` from
`System_GetEffectiveMurkPercent` (System murk clamped >=0 + murk-modifier
outfits ModType 0x1c, clamp [0,100]). The tiny constant
`g_distance_intensity_scale_const2` (`_DAT_005754d0`, double 1.2e-05) makes
brightness vary only at large distances in high-murk systems; the clamp is
0..0x1F (0x18 at pixel depth 8). Kania has murk 0, so this is a no-op in the
starting system. The full fog math, caller list, and the SDL two-pass masked-fog
port are documented in `docs/system_murk_rendering.md`.
- `SpaceflightView::AdvanceStellarAnimation` (src/game/spaceflight_view.cpp)
  reimplements the ambient frame-stepping part: the ordinary ping-pong/
  alternate/random cycler (availability_flags bit clear) and the hypergate
  (bit 0x1000) opening/working/closing around the CustPicID transition frame;
  player engagement and NPC states 0x01/0x14/0x15 drive the open state. The
  animator consumes normalized 30 Hz ticks, and Flags2 0x0080 swaps animation
  from the normal sprite zone to the destroyed/active zone
  (StellarDef +0x26, payload +0x18). Reconstructed runtime state is kept per
  stellar id in the view (sprite_current/previous_frame + frame_accumulator,
  mirrored from StellarDef +0x476/+0x478/+0x490). Dwell/frame multiplier decode
  from the sp\x6fb payload +0x22/+0x24 (StellarDef +0x470/+0x472) as Bible
  AnimDelay / Frame0Bias.

### State-0x15 hypergate emergence

`Ship_UpdateVisualState` does not synchronize the arriving hull to the gate's
current animation frame. The ship's 60-tick state-0x15 hold is its clock: it is
hidden while `ai_maneuver_timer_ms > 16`, then the final 16 ticks set hull
transparency to `2 * timer` (32 clear down to 0 opaque), set distance brightness
to `min(4 * timer, 32)`, and use pure white as the space/fog colour. Thus the
visible hull fades in white, then regains its normal colours over the last eight
ticks. All sprite layers are hidden during the earlier hold. During the reveal,
colored effect layers return with the hull colour so they cannot obscure the
initial pure-white silhouette. The SDL renderer uses an alpha-preserving white
silhouette texture for the hull tint.

## Decoder

`RleSpriteSheet_Decode16` (src/rle_sprite_sheet.cpp) already decodes the same
`rl\x91D` 16-bit RLE format the ship sheets use (header width/height/16/frame_count),
so reaching a renderable ship sprite is: add Ships archives -> read `sh\x9an` for
the class -> decode `rl\x91D` BaseImageID -> index frame by heading -> draw.

## NPC ship rendering (provisional)

`SpaceflightView` now renders every active non-player ship in the current system:

- `ShipSprites(platform, class_id)` lazily loads and caches a per-class
  `rl\x91D` sprite set (base + alt + engine glow + running lights + weapon
  effects + shield, keyed by ship class resource id) so each distinct class in
  the system is decoded/uploaded once. The player's `EnsureShipSprite` uses the
  same cache.
- `DrawShipsInLayer` iterates the ship slots (player slot 0 included) and draws
  each active ship in the current system at its world position with its
  heading/flag-selected frame (`ComposeShipFrameIndex`), using the same
  camera/DrawSprite path for the player and NPCs. See the layer-order and ship
  flags sections below for the original's exact compositing.
- To make the renderer's output visible, `SpawnRoamingFleetsStandIn`
  (spaceflight.cpp) spawns a small set of random-encounter fleet-lead ships on
  system entry via `NovaEncounter_SpawnFleetLeadShip`, capped at the system's
  `avg_ships`. This is a PROVISIONAL stand-in for the deferred
  `System_InitRoamingShips` / Step 5 encounter maintenance (it does not yet
  honour each def's spawn_system_filter, the availability expression, or spawn
  escorts).

## Original sprite-world layer order (verified)

The original never had a single ordered ship list. `NovaUi_InitializeFlightView-
Surfaces` (0x004ab9d4) creates 14 `SpriteLayer` objects and links them with
`SpriteWorld_AppendLayerTail`; `SpriteWorld_RenderLayers` (0x00477fa0) walks the
chain head-to-tail, invoking each sprite's draw proc inline, so **append order
is draw order** (last appended = on top). The chain:

| # | layer global | contents |
|---|--------------|----------|
| 1 | `g_ambient_star_particle_sprite_layer` | ambient star particles |
| 2 | `g_stellar_sprite_layer` | ambient stellars; also runs `LAB_00438c40` (flags-0x2000 under-ships beams) via `SpriteLayer_SetId` |
| 3 | `g_asteroid_sprite_layer` | drifting asteroids |
| 4 | `g_disabled_ship_sprite_layer` | ships with `Ship_IsShipDisabled` true |
| 5 | `g_freeflight_and_fading_effect_sprite_layer` | freeflight objects + fading destruction effects |
| 6 | `g_weapon_smoke_puff_sprite_layer` | weapon smoke-trail puffs |
| 7 | `g_shot_container_mode9` | shots |
| 8 | `g_shot_container_mode1` | shots |
| 9 | `g_shot_container_default` | shots |
| 10 | `g_escort_ship_sprite_layer` | ships with `squad_leader_ship_slot == 0` (player escorts) |
| 11 | `g_ship_sprite_layer` | player + non-escort NPC ships (all six per-ship sprites) |
| 12 | `g_shot_container_mode4_alt` | shots |
| 13 | `g_impact_effect_sprite_layer` | impact / destruction effect sprites |
| 14 | `g_beam_sprite_layer` | directional weapon effects; also runs `Shot_DrawBeamHitQueueForSurface` (0x00438810), the visible beam pass |

A ship is moved between layers 4/10/11 at runtime by
`Sprite_SetContainerIfChanged` in `Ship_UpdateVisualState` (0x00428340). Key
consequences: impact/explosion effects (layer 13) composite **over ships**
(layer 11), and normal beams (layer 14) over everything. Shots draw below
ships; the freeflight-object and fading-effect pools share one layer.

The SDL port has no runtime layer objects; `SpaceflightView::Draw` reproduces
the layer precedence as a fixed draw sequence (see the ordering comment in
`spaceflight_view.cpp`). As of 2026 it also reproduces the ship sub-layers
(`DrawShipsInLayer` Disabled/Escort/Normal, driven by `NovaAiShip_IsDisabled`
and `squad_leader_ship_slot == 0`) and all four shot containers in their
original layer order (`DrawShots(ShotDrawLayer)`: mode9 -> mode1 -> default on
layers 7-9, then mode4_alt on layer 12, the latter fixed at spawn from weapon
mode 4 + owner Flags3 0x0040). Remaining divergence:
the layer-6 weapon smoke-puff pool is not ported (no stock weapon enables it),
and the layer-14 beam pass still lacks the twin-surface branches.

## Ship sprite row flags, alt and shield (ported 2026)

`Ship_UpdateVisualState` (0x00428340) composes the base frame as
`row * FramesPer + heading_frame`, where the row is selected by the class's
Bible Flags *only when the base sheet has more than one set* (`BaseSetCount`
>= 2):

| Flags | row source |
|-------|------------|
| 0x0001 | banking: `ai_turn_bias_dir` (-1 -> row 1 left, +1 -> row 2 right) |
| 0x0002 | fold/unfold: `waypoint_arrival_marker_b` via `turn_bank_animation_phase`, AnimDelay per step; Flags 0x0080 re-triggers the unfold 45 ticks (0x2d) after the last fire and the fire sites fold (`marker_a = -1`) |
| 0x0004 | carry: row 1 while `Weapon_HasLoadedLaunchBayAmmo` (no stock class) |
| 0x0008 | combat/part sequence: `sprite_animation_cycle_index` via `sprite_animation_timer`, AnimDelay per step, wrapped at `animation_cycle_count` (BaseSetCount, or 1 with the ship-animations preference off) |

Flags 0x0001-0x0008 are mutually exclusive. `NovaShip_TickSpriteAnimation`
(src/game/ship_visual.cpp) advances this state for every active hull;
`ComposeShipBaseRow` derives the row at draw time. The stock users (verified
against the shipped `sh\x8an` payloads) are Cargo Drone (0x58), Leviathan
(0x58), Argosy (0x02), Manticore (0x18), Auroran Cruiser (0x78), Hyperioid
(0x48), Asteroid Miner (0xc2) and the Auroran Thunderforge (0x58 with the alt
sheet).

Pref handling: the renderer currently forces sprite animation ON. The loader
pref-gates `animation_cycle_count`/`alt_sprite_cycle_count` (1 when the pref
is off), but `ShipSprites` always loads the full base sheet and the row guard
uses `base_set_count`, so a pref-off run would still row-animate. TODO(decomp):
thread `g_pref_ship_animations` into the renderer / use `animation_cycle_count
>= 2` as the guard.

The separate `AltImageID` overlay is drawn over the hull in per-ship container
append order (`base 0x0, glow 0x4, light 0x8, weapon 0xc, alt 0x10, shield
0x14`) and cycles at AnimDelay; it is the Thunderforge's only animation (the
only shipped class with an alt sheet). Flags 0x0020 + a disabled ship makes the
original skip the alt update (assignment and cycle); it does \*not\* hide the
sheet, which keeps its last drawn frame. The `ShieldImageID` bubble sheet is
loaded into `SpaceflightView::ShipSpriteSet::shield` but its draw stays
deferred (the `shield_bubble_flash_intensity` tint/frame arm is not
reconstructed).

## Sprite rows / banking (verified 2026, data + Bible)

All basic sprite sets live in the SAME `rl\x91D` resource named by
`BaseImageID` (Bible `BaseSetCount`: "the graphics for all of a ship's basic
sprite sets are stored in the same PICT/rleD/rle8 resource"). Verified from
the data: the shuttle sheet (`rl\x91D` 1000) is 24x24 with a header count of
108 = 3 rows x FramesPer 36. `Ship_UpdateVisualState` (0x00428340) composes
the displayed frame as

    frame = bias_row * frames_per_rotation + heading_frame

with `bias_row` from `ai_turn_bias_dir` (+0xc8f8) for classes whose sh\x8an
Flags have bit 0 (Bible 0x0001: "The first set of sprites is used for level
flight, the second for banking left, and the third for banking right"). The
`AltImageID`/`AltSetCount` descriptor fields (+0x0c/+0x10) name a SEPARATE
overlay sheet (Ghidra 0x004b4ee0 gates its creation on AltImageID > 0 &&
AltSetCount > 0); it is never appended to the base rows.
`Ship_UpdateVisualState` draws it over the hull whenever the sheet exists,
cycling `alternate_sprite_cycle_index` (ShipState +0xC8F4) through AltSetCount
sets at the AnimDelay rate, displayed frame
`index * FramesPer + heading_frame`. Only the Auroran Thunderforge ships an
alt sheet in the shipped data (a single base set + a 6-set alt). The renderer
loads/draws it (`SpaceflightView::ShipSpriteSet::alt`);
`NovaShip_TickSpriteAnimation` advances the cycle. `ShipClass.sprite_behavior_
flags` is decoded in scenario_data.cpp from sh\x8an +0x2e.

## Running lights + weapon effects (ported 2026)

Two more per-class layers from the sh\x8an descriptor are now rendered over the
hull in `SpaceflightView` (player and NPCs alike):

| role | sh\x8an fields | ShipClass / Ship state | driver |
|------|----------------|------------------------|--------|
| running lights | LightImageID/mask/x/y +0x1e/+0x20/+0x22/+0x24 | `ShipClass.light_image_id`, `blink_mode`, `blink_val_a..d`; `Ship.light_intensity` (+0x60), `light_blink_phase` (+0xC8D6), `light_blink_timer` (+0xC8EC) | `NovaShip_TickWeaponSpriteAndRunningLights` |
| weapon effects | WeapImageID/mask/x/y +0x26/+0x28/+0x2a/+0x2c, WeapDecay +0x32 | `ShipClass.weapon_glow_decay_rate` (WeapDecay * binary64 0.333), `Ship.weapon_sprite_flash_level` (+0xC8E8) | same tick + fire sites |

The loader's names for sh\x8an +0x36..+0x3e are wrong: they are Bible
BlinkMode/BlinkValA..D, not gun/turret/guided exit positions. Ground truth is
the only consumer, the blink machine in `Ship_UpdateVisualState` (0x00428340),
plus the loader's 0x1f intensity clamps for BlinkMode 2/3. The real exit
geometry begins at +0x48 (already decoded as `ShipClass.muzzle_*`).

Weapon exit geometry is four Bible `ExitType` families (Gun, Turret, Guided,
Beam), with four quadrant barrels each. The loader interleaves each family's
X/Y data into runtime pairs: for family `g` and quadrant `q`, X is `sh\x8an
+0x48 + 16*g + 2*q` and Y is `+0x50 + 16*g + 2*q`; Z is `+0x90 + 8*g +
2*q`. Do not use an 8-byte X/Y family stride: that reads the preceding
family's Y values as the next family's X values. The Gjinchar-class Aurora
Cruiser exposes this visibly: its Fusion Pulse Battery (ExitType 1) is
`(X=0,Y=-35)`, whereas the erroneous 8-byte stride yields `(X=56,Y=0)`.

* `Ship.weapon_sprite_flash_level` is raised to 32 at the fire site when the
  fired WeaponDef carries `flags_secondary` 0x200
  (`Weapon_FirePlayerWeaponBank` 0x00455150 / `Weapon_FireShipWeapons`
  0x00414550) and decays by `weapon_glow_decay_rate` per normalized 30 Hz tick.
  The binary scale is 0.333; a Fed Destroyer (`WeapDecay=5`) fades in about
  0.64 seconds.
  An overshoot below -1.0 clamps to -1.0. It is independent of
  WeapDecay 0 (which never decays, matching the original).
* `Ship.light_intensity` (the original's unnamed float at ShipState +0x60) is
  driven by the Bible `BlinkMode` program: 0/-1 steady full, 1 square wave
  (BlinkValA off, B on, C blinks/group, D group delay), 2 triangle pulse
  (A min, B rise x100, C max, D fall x100), 3 random pulse (A/B min/max, C
  delay). It is only run for classes with a light layer.
* Both tick for every active current-system hull: `NovaShip_TickWeaponSpriteAnd
  RunningLights` is called beside the cloak-fade slice in `Stub_HandleShips`
  (NPCs) and after the player's destruction slice in Frame_TickSystems scope 10.

Rendering: the light/weapon sheets share the base rotation grid
(`frames_per_rotation * base_set_count`) and are drawn over the hull/glow at
the same composed frame index. The original writes `round(intensity)` into the
Sprite's RGB tint channels at brightness 32 and draws them through
`BlitPixel_TintRgb15Span` (0x004736c0), whose brightness-32 branch computes
`dst + src*intensity/32` — i.e. additive. The port passes
`SpriteDrawOptions.additive` (SDL_BLENDMODE_ADD) with `alpha_mod =
intensity/32` for the engine glow, running lights and weapon effects alike.
Visibility gates match the original: the light layer hides at intensity <= 1.0,
the weapon layer at <= 0.

The engine glow is special-cased: `Ship_UpdateVisualState` (0x0042a383) first
forms `level = engine_glow_level + NovaRandom_Range(6) - 4`, hides the layer
when `level < 2`, clamps it to 32, and writes that value into the RGB tint
channels at brightness 32 (so alpha = `level/32`, not `engine_glow_level/24`).
The port reproduces this in `NovaShip_TickWeaponSpriteAnd
RunningLights` (the visual-state tick), storing the fraction in
`Ship.engine_glow_intensity`; the movement functions' provisional `level/24`
write is overwritten before the frame is drawn. The same original block folds
the per-ship fog into NPC glow (`level = min(level, 32 -
distance_brightness*1.5)`). The port applies it in
`NovaShip_TickWeaponSpriteAndRunningLights` (the player is always at distance 0,
so the cap only bites NPCs).

One original nuance is not reproduced: `BlitPixie_BlitRectRawCopy` (0x004711e0)
fast-paths the case `tint==0x20 && brightness==0x20 && distance_brightness==0`
to a plain opaque copy instead of the additive formula. That is exactly a
full-intensity (32) effect layer in a no-murk system, so the original draws
those as a normal overlay while the port always adds. The base hull is not
affected: it uses `brightness = ShipClassDef.base_transparency` and
`tint = Ship_ResolveShipTintColor() - 0x20`, so an opaque hull
(brightness 0) is an ordinary tinted copy.

Not yet honored: the class-level load gates `g_pref_running_lights` /
`g_pref_weapon_effects` / `g_pref_engine_glows` (the preferences are not on
`GameState`, so all three layers load unconditionally). The per-ship
distance-brightness/space-color tint the original applies to these layers is
modelled through `SpriteDrawOptions.fog_murk` (a fade toward the backdrop).

Player glow-level drive is a clean-room approximation inside
`PlayerTick_ManualFlightAndRegeneration`: it steps the level toward 24
(thrust), 32 (afterburner) or 0 (coast), while the original adds +1/frame
under thrust (<24), +2/frame while banking (no clamp), fades once while
coasting and again under a maneuver/station hold, and ramps the afterburner
level at up to +2/frame to 32; the inertialess arm ramps toward
`round(speed*32*0.75/eff_max)` capped 24. The NPC `Ship_HandleShip` drive is
faithful.
