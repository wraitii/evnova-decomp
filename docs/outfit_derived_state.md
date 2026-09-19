# Outfit-derived player state (`Outfit_RecomputeOutfitDerivedState`, 0x0046d4b0)

Ground truth for the "inventory / loadout changed → rebuild derived state" hook.
This is the single function the original calls after any code path that touches
cargo, outfits, licenses, or active ranks. Companion to
`docs/scenario_data_loading.md` (the `o\x9ftf` decoder that fills the outfit
table) and `docs/x87_precision.md` (the cargo-overflow scaling casts).

## Two mechanisms, not one

The binary mixes two different patterns, and the port mirrors both:

1. **Lazy sentinel caches (pure).** The `Ship_Compute*` getters
   (`Ship_ComputeShipMaxShieldPoints` 0x00463550, `...MaxArmorPoints` 0x004637a0,
   `...FuelCapacity` 0x00463a20, thrust/speed/turn helpers) cache their player
   result in globals (`_DAT_00735688/90/98...`, `g_player_max_shield_cache`, ...)
   and recompute only while the cached value is negative. The recompute writes
   those sentinels (`-1.0`, `0xffff`) — it does **not** compute the stats.
   The port models this with `GameState.stat_cache_valid` + `cached_stats`
   (`Outfit_ComputePlayerEffectiveStats`) and invalidates via
   `NovaOutfit_RecomputeOutfitDerivedState`.
2. **Eager side-effecting rebuild (not a cache).** The same function also
   mutates live state, consumes RNG, and writes flags other systems read. This
   part must run at the same call sites, in the same order, or timing and the
   shared RNG stream change.

### Eager arms (source, in order)

| Arm | Writes | Port status |
|---|---|---|
| License clamp | unlicensed → `max_shield/armor = 1.0` | TODO |
| Junk flags | `DAT_007356cc/cf/d0` from `g_junk_defs` | TODO |
| Owned-outfit scan | ModType `0x2f`/`0x32` → carried-bomb class + `g_bomb_detonation_timer` (rolls `NovaRandom_Range(100)`); `0x2c` (val −1) → `g_player_reinforcement_inhibit_all`; `0x2c` → matching governments' `reinforcement_inhibited` (+0x82); `0x30` → `iff_scrambler_active` (+0x83); `0x1f` → `mining_scoop_active` | partial (bomb TODO) |
| Mining-scoop gate | clears scoop when cargo+junk ≥ fleet capacity | done (`NovaOutfit_RefreshPlayerMiningScoopActive`) |
| Jamming reset | `jamming_score_1..4 = -1` | done |
| Cargo overflow | capacity < total → scale all 6 bins by `capacity/total` (x87 truncation) + inventory dirty | TODO |
| Negative clamps | bins (+0x7a) and `g_junk_defs` counts (+0x22) → 0 | done |
| Cloak latches | `cloak_scanner_reveal_screen/radar`, `cloak_damage_deactivate_latch` = -1 | done |
| Government clear | all 0x100 govts' `policy_flags[0..1] = 0` | done |
| Rank flags | active rank (flags 0x100/0x200, allied govt) → `policy_flags` | done |
| Tail | `recently_hit_timer = -1.0` (done); `distance_intensity_scale = System_GetEffectiveMurkPercent()` (port computes on demand in `NovaSystem_GetEffectiveMurkPercent`, see docs/system_murk_rendering.md) | done |

Note: the `policy_flags` writer here is the one `src/game/government.cpp` and
`src/game/scenario_data.hpp` currently mark as "writer not identified"; this
function is that writer.

## Callers

Direct calls (14):

| Address | Function |
|---|---|
| `0x0041f330` | `Player_RedistributeFleetCargoOverflow` |
| `0x004374f0` | `Ship_HandleSpritePairCollision` (freeflight mining-scoop pickup) |
| `0x00440370` | `Mission_TryConsumeMissionInteractionResources` |
| `0x004438d0` | `Mission_ProcessInteractionReactionSlotResources` (dump-cargo dialog arm) |
| `0x00448020` | `Mission_ExecuteReactionScript` |
| `0x00448050` | `Mission_RunMisnScriptPayload` |
| `0x0044aa70` | `Ship_HandlePlayerShipCore` |
| `0x00457580` | `Stellar_HandleStellarEntryAndExit` |
| `0x00462ec0` | `Weapon_ReconcileOutfitPoolWithWeaponBanks` |
| `0x00463260` | `Weapon_RebuildWeaponBankPoolsFromOwnedOutfits` |
| `0x00469810` | `Player_TransferCargoAndJunkToEscortByRatio` |
| `0x00482940` | `NovaUi_RunBoardingPlunderWindow` (window teardown) |
| `0x00489210` | `Ship_RunSpaceflightMode` (after license-token evaluation) |
| `0x004b3350` | `Ship_ResetPlayerShipState` |

Interior sites not visible as separate callers: `Ship_HandlePlayerShipCore`
fires it **4×** (status-panel/outfit refresh; system-init/`Asteroid_InitSystem`;
after `Rank_Activate` + `Weapon_RebuildWeaponBankPoolsFromOwnedOutfits`;
after `Weapon_ReconcileOutfitPoolWithWeaponBanks` + UI-install on ship swap),
and `Stellar_HandleStellarEntryAndExit` once after
`Ship_DeactivateVacantShipsAndTally`.

## Port model and coverage

`NovaOutfit_RecomputeOutfitDerivedState` (`src/game/outfit.cpp`) is the
clean-room hook (renamed from `OutfitMarkStatsDirty`). It currently does the
stat-cache invalidation, negative clamps, the jamming reset, the cloak-latch
reset, the mining-scoop arm, and the recently-hit timer reset. The remaining
eager arms above are TODO and tracked at `0x0046D4B0` in
`decomp-progress.tsv`.

Sites that call the full hook: `NovaWeapon_ResolveFreeflightScoop`,
`Mission_ExecuteReactionScript`, `Mission_RunMisnScriptPayload`,
`Outfit_AddInstalledOutfit` / `Outfit_RemoveOutfit`, `NovaLanded_*` outfitter,
`Stub_SeedStartingInventory`, and `NovaWeapon_RebuildBanksFromOwnedOutfits`
(currently conditional on `materialized`; the original calls it
unconditionally).

Sites that only set `stat_cache_valid = false` directly and should be unified
onto the hook (each stands in for an original recompute call):
`mission.cpp` (`Mission_RefreshActiveMissionSpawnState`, `Mission_RerollOfferingRolls`,
`Player_CollectStellarTribute`), `boarding_plunder.cpp`
(`SelfDestructTarget` arms), `landed_store.cpp` (`NovaLanded_*` close paths),
`spaceflight.cpp` (`DetonateCarriedBomb`, `RespawnResetPlayerShipState`,
`RunPlayerEjectTransform`), `new_pilot_flow.cpp` (`RecomputePlayerMeters`),
`outfit.cpp` (`Player_ComputeRemainingCargoSpace`). Unifying will start
consuming the bomb-timer RNG at those sites — that is *more* faithful but is a
deliberate behaviour change that needs test coverage.

## Rule: eager vs. lazy

Mirror the decomp by default. A value may be recomputed at the reader site
only when all hold: it is a pure function of current state, it consumes no RNG
(shifting the shared `state.rng` stream changes later rolls), no other
subsystem observes the timing, and the original itself uses the sentinel-cache
pattern for it. Otherwise keep it eager at the original call site. Every
deliberate laziness divergence gets a marker and a tracker note.

## Remaining work (in priority order)

1. Cargo-overflow scaling (use `docs/x87_precision.md`).
2. Carried-bomb class + detonation timer (gives `bomb_outfit_class` its writer;
   note the shared-RNG timing change).
3. Junk flags (`DAT_007356cc/cf/d0`) and the license clamp (the murk
   `distance_intensity_scale` cache is instead computed on demand in
   `NovaSystem_GetEffectiveMurkPercent`; see docs/system_murk_rendering.md).
4. Unify the bare `stat_cache_valid = false` sites onto the hook, per site
   (only those that map to an original recompute call).

Note on the sticky government latches: the original sets `reinforcement_inhibited`
(+0x82) and `iff_scrambler_active` (+0x83) but the recompute's clear loop only
zeroes `policy_flags` (+0x84/+0x85). The port reproduces that quirk.
