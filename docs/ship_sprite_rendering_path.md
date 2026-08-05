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

## Decoder

`RleSpriteSheet_Decode16` (src/rle_sprite_sheet.cpp) already decodes the same
`rl\x91D` 16-bit RLE format the ship sheets use (header width/height/16/frame_count),
so reaching a renderable ship sprite is: add Ships archives -> read `sh\x9an` for
the class -> decode `rl\x91D` BaseImageID -> index frame by heading -> draw.
