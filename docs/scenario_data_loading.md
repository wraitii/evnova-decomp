# Scenario data loading (ships / outfits / weapons / stellars / systems / governments / fleets)

Clean-room reconstruction of the Nova scenario resource tables that the new-game
flow and spaceflight loop consume. The original rebuilds them at startup in
`NovaData_LoadScenarioResourceTables` (Ghidra `0x004bd3c0`) by walking the
resource families by id `0x80..` and parsing a fixed big-endian field layout
(Macintosh resource heritage) into a set of globals.

## Where the data lives

The `.rez` archives are `BRGR` containers parsed by `src/brgr_archive.cpp`. Each
holds a big-endian `resource.map` (type dir of `[type_code][records_offset]
[count]`, then `0x10a`-byte records of `[index][type_code][u16 res_id][name]`).
The scenario families are spread across the `Nova Data *.rez` archives:

| type | FourCC | family | archive | count |
|------|--------|--------|---------|-------|
| `0x73689570` | `sh\x95p`  | ships   | Nova Data 1 | 288 |
| `0x6f9f7466` | `o\x9ftf`  | outfits | Nova Data 4 | 242 |
| `0x77916170` | `w\x91ap`  | weapons | Nova Data 4 | 81  |
| `0x73709a62` | `sp\x9ab`  | stellars| Nova Data 2 | 411 |
| `0x73d87374` | `s\xd8st`  | systems | Nova Data 2 | 545 |
| `0x679a7674` | `g\x9avt`  | governments| Nova Data 1 | 68 |
| `0x666c9174` | `fl\x91t`  | fleets     | Nova Data 1 | 128 |

`brgr_archive.cpp`'s `kArchiveFileNames` lists the Data archives alongside the
menu/splash archives. A robustness fix to `ParseArchive` was required: some
containers (Nova Data 4) carry an early bogus region whose first bytes read as a
plausible map header but whose record table overflows the region; the parse now
rejects any region whose every type entry's record table isn't fully in bounds,
so the genuine map is found.

## Clean-room model

`src/game/scenario_data.hpp` defines `game::ShipClass`, `Outfit`, `Weapon`,
`Stellar`, `System`, `Government`, `FleetDef`, plus `ScenarioData` which owns the
seven indexed tables (by `id - 0x80`, matching the original globals).
`ScenarioData::LoadFromArchives()` parses all families and stores them on
`GameState::scenario` so gameplay code has data keyed by id with no hidden
globals (AGENTS.md).

### Verified offsets (vs. payload + loader)

- **sh\x95p (ship)**: Holds0, Shield2, Accel4, Speed6, Maneuver8, Fuel_a,
  FreeMass_c, Armor_e, ShieldRech10. The 8 stock weapon banks are packed as two
  groups of 4 — banks 0-3 as `(type,count,ammo)` at `0x12/0x1a/0x22`, banks 4-7
  at `0x6ce/0x6d6/0x6de` — and the 8 DefaultItems likewise split at `0x4e/0x56`
  and `0x370/0x378`. MaxGun2a, MaxTur2c, TechLevel2e, Cost30(**int32**),
  DeathDelay34, ArmorRech36, Explode1/2 38/3a, DispWeight3c, Mass3e, Length40,
  InherentAI42, Crew44, Strength46, InherentGovt48, Flags(capability)4a,
  Flags2 62, Flags3 726. Verified against ship 0x80 (holds 10, shield 30,
  accel 500, speed 400, turn 40, fuel 300, mass 15, cost 10000).
- **w\x91ap (weapon)**: Reload0, Count2, MassDmg4, EnergyDmg6, Guidance8,
  Speed_a, AmmoType_c, Graphic_e, Inaccuracy10, Sound12, Impact14, ExplodType16,
  ProxRadius18, BlastRadius1a, Flags1c, Seeker1e, ... Verified against weapon
  0x80 (reload 10, count 13, mass 1, energy 4, unguided, speed 1500).
- **s\xd8st (system)**: xPos0, yPos2, Con1-16 at `0x04`, NavDef1-16 at `0x24`,
  AvgShips64, Govt66 (rebased to 0.. space; <0x80 / >0x17f -> -1), Message68,
  Asteroids6a, Interference6c, DudeTypes at `0x6e` (rebased -0x80; <0x80 />0x47e
  -> -1) with % Prob at `0x7e` (clamped 0..100), BkgndColor+0x8e, Murk+0x92,
  ReinfFleet/Time/Interval +0x196..+0x19a, Visibility +0x96. Verified against
  system 0x80 (links 199/200/202/129/135, avg 4, govt 128->0, message -1,
  asteroids 3, dude types 510/155/156/128, probs 50/1/1/10). The original also
  stores a per-system runtime random-encounter binding (SystemDef +0x6a ids /
  +0x7a weights / +0x8a count / +0x8c chance) feeding
  EncounterFleet_SelectRandomEncounterFleetDefWeighted (0x0046b6d0); the population
  pass that fills it from scenario data is not yet localized.
- **g\x9avt (government)**: header +00 voice, +02 flags_primary, +04
  scan_mask_short, +06 jam1, +08 flee, +0a disable_pen / +0c board / +0e kill /
  +10 shoot penalties, +12 max_odds, +14 bribe %, +16 combat_rating_src, +18
  class1-4, +20 ally1-4, +28 enemy1-4, +30 pilot_skill_src, +32 ai_skill,
  +34 comm name, +44 name-table name, +54 scan_lo, +58 scan_hi, +5c..+62
  jam2-4, +64 medium name, +a4 theme-color RGB24, +a8 ship-color RGB24, +ac
  interface_id, +ae news_pic_id. The loader recodes the voice code by range
  (raw 0..7 / +1000 / +2000 -> mode -1/1/0) and scales the skill shorts by
  0.01f (`DAT_00575e60`). Verified against the Federation (0x80): flags 0xe2b0,
  enemies 2/10/16/9, theme 0x2c2caf, interface 0x82.
- **fl\x91t (random-encounter fleet def)**: lead_ship_class +00, escort_ship_class_ids
  [4] +02, escort_min_count [4] +0a, escort_max_count [4] +12, government_id +1a,
  spawn_system_filter +1c, availability_expr +1e, arrival_message_id +11e,
  carry_cargo_flags +120. The loader (0x004bd3c0) walks up to 0x100 slots at a
  0x124 stride and rebases every id >= 0x80 down by 0x80, sentinel-invalidating
  a < 0x80 id (lead -> 0 / never spawns, government + escorts -> -1 with
  min/max zeroed). `is_available_runtime` is a runtime state derived from the
  availability expression, not payload. Verified against the shipped 128 defs
  (e.g. fleet 0x80 lead 13, govt 0, filter 10000, escorts 95/96; fleet 0x82
  filter -1 spawns anywhere).

### Provisional (not yet fully verified)

`o\x9ftf` (outfit) and `sp\x9ab` (stellars) decoders fill the positively-identified
header fields but not every offset; they are marked provisional in
`scenario_data.cpp`. The derived runtime fields the original computes at load
(purchase mass/price recompute from default outfits, control-expression
compilation for availability/purchase) are not reconstructed yet, and string
fields (names, availability/on-purchase expressions) come from the resource
record name / later string blocks rather than the numeric header.

## New-game flow integration

`new_pilot_flow.cpp` now:
- loads the scenario tables once (Step 4) before the ship reset;
- resets the player ship from the real default class stats (id 0x80);
- seeds the starting inventory from the class DefaultItems;
- resolves the initial travel destination from the starting system's first
  outward link (system adjacency now readable).

`progress.csv` row for `0x004BD3C0` was raised to 30% to reflect the
resource-family decoding slice.
