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
| +0x32   | WeapDecay    | -> `weapon_glow_decay_rate` = WeapDecay * 0.003484 |
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

### Distance/fog (Frame_UpdateSpriteDistanceIntensity) - decoded

`distance_brightness = round(g_distance_intensity_scale * distSq * 1.2e-05)`
where `distSq` is squared rounded distance to the player on both axes and
`g_distance_intensity_scale` (0x7356bc) is actually the **effective system murk
percent** (0-100), rebuilt each recompute by
`Outfit_RecomputeOutfitDerivedState` via `System_GetEffectiveMurkPercent()`
(System `murk` clamped >=0 + murk-modifier outfits modtype 0x1c, clamp [0,100]).
The tiny fog constant `g_distance_intensity_scale_const2` (= `_DAT_005754d0`,
double 1.2e-05) makes brightness only visibly vary for large distances near a
high-murk system. Clamp is 0..0x1F (0x18 in pixel-depth-8). Then copies
`SystemDef.space_color` (5-5-5 packed from BkgndColor) into the sprite.

**Consequence for the starting-system demo:** Kania has murk 0, so
`g_distance_intensity_scale` is 0 and `distance_brightness` stays 0 -- this fog
is a no-op for the current default system. It only materialises in high-murk
systems, so it is recorded here for later correctness rather than implemented
in the SDL renderer yet (TODO).
- `SpaceflightView::AdvanceStellarAnimation` (src/game/spaceflight_view.cpp)
  reimplements the ambient frame-stepping part: the ordinary ping-pong/
  alternate/random cycler (availability_flags bit clear) and the hypergate
  (bit 0x1000) non-engaged drift toward/around `engage_highlight_frame`
  (StellarDef +0x26, payload +0x18). Reconstructed runtime state is kept per
  stellar id in the view (sprite_current/previous_frame + frame_accumulator,
  mirrored from StellarDef +0x476/+0x478/+0x490). Dwell/frame multiplier decode
  from the sp\x6fb payload +0x22/+0x24 (StellarDef +0x470/+0x472) as Bible
  AnimDelay / Frame0Bias. The engage-highlight pulse and per-frame
  distance-intensity positioning are not yet reimplemented (TODO(decomp)).

## Decoder

`RleSpriteSheet_Decode16` (src/rle_sprite_sheet.cpp) already decodes the same
`rl\x91D` 16-bit RLE format the ship sheets use (header width/height/16/frame_count),
so reaching a renderable ship sprite is: add Ships archives -> read `sh\x9an` for
the class -> decode `rl\x91D` BaseImageID -> index frame by heading -> draw.

## Step 4 status: NPC ships rendered (provisional)

`SpaceflightView` now renders every active non-player ship in the current system:

- `ShipClassSprite(platform, class_id)` lazily loads and caches a per-class
  `rl\x91D` heading-rotation sheet (keyed by ship class resource id) so each
  distinct class in the system is decoded/uploaded once. It mirrors the player's
  `EnsureShipSprite` base load but skips the engine-glow layer (TODO: per-NPC
  glow).
- `DrawNpcShips` iterates the ship slots (1..) and draws each active ship in the
  current system at its world position with its heading-selected frame
  (`FrameForHeading`), using the same camera/DrawSprite path as the player.
  NPC ships composite below the player ship (layer order: backdrop -> stellars
  -> shots -> NPC ships -> player).
- To make the renderer's output visible, `SpawnRoamingFleetsStandIn`
  (spaceflight.cpp) spawns a small set of random-encounter fleet-lead ships on
  system entry via `NovaEncounter_SpawnFleetLeadShip`, capped at the system's
  `avg_ships`. This is a PROVISIONAL stand-in for the deferred
  `System_InitRoamingShips` / Step 5 encounter maintenance (it does not yet
  honour each def's spawn_system_filter, the availability expression, or spawn
  escorts).

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
alternate sheet used by the Flags 0x0002 set-cycling animation (Ghidra
0x004b4ee0 gates it on AltImageID > 0 && AltSetCount > 0); it is never
appended to the base rows. The clean-room renderer mirrors this in
`ComposeShipFrameIndex` (spaceflight_view.cpp); `ShipClass.sprite_behavior_
flags` is decoded in scenario_data.cpp from sh\x8an +0x2e.
