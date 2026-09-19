# Faction combat events and ranks

Reverse-engineering + reimplementation notes for the crime → reputation →
rank-revocation chain:

| Address | Ghidra name |
|---|---|
| `0x00466FC0` | `Government_ProcessFactionCombatEvent` |
| `0x00467140` | `Government_PropagateFactionCombatInfluenceToNearbySystems` |
| `0x00427F40` | `Rank_Deactivate` |
| `0x00427DF0` | `Rank_Activate` |
| `0x0046F100` | `Government_IsShipGovernmentDerelict` |

## Call graph and event codes

`Government_ProcessFactionCombatEvent(system_id, faction, event_code,
mission_fleet_slot)` is the single entry point. Ten call sites:

| code | meaning | call sites | `mission_fleet_slot` |
|---|---|---|---|
| 0 | smuggling detected (`Ship_ScanPlayerForContraband`) | `0x00401A1D`, `0x00402399` | `-1` |
| 1 | ship **disabled** (`Ship_IsShipDisabled`) | `0x00419721` | victim's `+0xC8D2` |
| 2 | **boarded** | `0x0045A85F` | victim's `+0xC8D2` |
| 3 | **killed** / stellar destroyed / attack-stellar | `0x0041976F`, `0x004381EF` (×10), `0x00453670`, `0x00480E4B`/`0x00480F9B`/`0x0048102B` (×5) | kill victim's `+0xC8D2`; stellar/attack sites `-1` |

Kill/disable/board pass the victim's own mission-fleet slot, so a live mission
ship's event takes the `!= -1` arm (the flood and rank revocation are skipped).
Only the smuggle and stellar/attack sites pass `-1` unconditionally.

Kill/disable is the gate for rank flag `0x0004`; "any crime" is flag `0x0040`.
The stellar-destruction arm fires event 3 ten times and the travel-window arm
five times. `Government_ProcessFactionCombatEvent` clears its visit mask on
entry, so each pulse is an independent full flood (reputation shifts again) and
the rank loop re-runs every time.

The event-0 caller is the smuggle-fine path: it composes STR# 0x7d2 entries
0x178–0x17c ("your attempt to smuggle illegal cargo through this system has
been detected. You have been fined …") and gates on the government's
SmugPenalty at runtime `+0x48`.

## ProcessFactionCombatEvent shape (decoded)

1. Early-out when `faction != -1 && (govt.flags_primary & 0x0800)` — the Bible
   "derelict" bit, the same test as `Government_IsShipGovernmentDerelict`.
2. Clear `g_stellar_flood_visit_mask` (`0x007CC590`, `0x800` bytes).
3. When `mission_fleet_slot == -1`:
   - `Government_PropagateFactionCombatInfluenceToNearbySystems(system,
     faction, event_code, 1.0)` (`DAT_00575778..7C` is the double `1.0`).
   - For every rank slot `0..0x7F`: active (`+0x00`) **and** defined (`+0x01`)
     **and** not permanent (`flags & 0x0008`) **and** allied to `faction`
     **and** (`flags & 0x0040` or (`flags & 0x0004` and event ∈ {1,3})) →
     `Rank_Deactivate`.
4. Returns `void` (Ghidra's `float` return is a stray `ST0`; every exit is a
   plain pop/`RET`).

## Rank table (`g_rank_defs`, 0x80 × `0x120`)

Loaded by `NovaData_LoadScenarioResourceTables` (`0x004BD3C0`) from `r\x8ank`
resources (`id 0x80 + slot`; 31 records live in `Nova Data 2.rez`). Field map
verified from the loader:

| offset | field | source |
|---|---|---|
| `+0x00` | active | runtime only |
| `+0x01` | defined/loaded | loader |
| `+0x02` | rank id = slot index (`<RRK>` key) | `Ship_InitGameplayDataTables` `0x004B0C20` |
| `+0x04` | weight | payload `+0x00` |
| `+0x06` | affiliated govt (rebased, `<0x80 → -1`) | payload `+0x02` |
| `+0x08` | PriceMod (min 100) | payload `+0x04` |
| `+0x0C`/`+0x10` | Salary / SalaryCap | payload `+0x06`/`+0x0A` |
| `+0x14`/`+0x18` | Contribute (64-bit) | payload `+0x0E`/`+0x12` |
| `+0x1C` | flags | payload `+0x16` |
| `+0x1E` | full name | record name |
| `+0x9E` | ConvName | payload `+0x18` |
| `+0xDE` | ShortName | payload `+0x58` |

Rank flags (Bible): `0x0001` deactivate same-govt on activate, `0x0002`
same-govt on deactivate, `0x0004` destroy/disable, `0x0008` permanent,
`0x0010` lower-weight on activate, `0x0020` lower-weight on deactivate,
`0x0040` any crime, `0x0100` no auto-attack, `0x0200` always land,
`0x0400` battle assistance, `0x0800` allied repair/refuel.

## PropagateFactionCombatInfluenceToNearbySystems

Recursive reputation flood (`0x400` bytes). Per call:

1. System-id guard `0x000..0x7FF`; `g_stellar_flood_visit_mask` re-entry guard.
2. `xenophobic = (faction != -1) && (govt.flags_primary & 1)`.
3. Resolve the visibility root; walk the `visible_parent_system_id` chain
   (only `is_visible` systems contribute).
4. Per visible system compute a signed delta from the **penalty array**
   `GovtDef + 0x48 + event_code*2`, scaled `0.5`/`0.25` per relation arm and by
   `param_4`; relations use `AreGovtsAllied` / `AreGovtsHostileOrXenophobic`
   against the system's government.
5. Apply to `g_system_reputation[system]` (clamp ±32000); if anything changed,
   recurse into the 16 adjacency systems with `param_4 * 0.65`
   (`DAT_005757E8`).

The penalty array is indexed by event code: smuggle → `0x48` (SmugPenalty),
disable → `0x4A` (DisabPenalty), board → `0x4C` (BoardPenalty), kill → `0x4E`
(KillPenalty), with the unused fifth slot `0x50` ShootPenalty. The gövt payload
lays the crime/reputation block out as ScanFine `+0x06`, CrimeTol `+0x08`,
SmugPenalty `+0x0A`, DisabPenalty `+0x0C`, BoardPenalty `+0x0E`, KillPenalty
`+0x10`, ShootPenalty `+0x12`, InitialRec `+0x14`, MaxOdds `+0x16`. The
`NovaData_LoadScenarioResourceTables` government pass (`0x004C5FF6`) maps
payload → runtime `+0x06→0x54`, `+0x08→0x46`, `+0x0A→0x48`, `+0x0C→0x4A`,
`+0x0E→0x4C`, `+0x10→0x4E`, `+0x12→0x50`, `+0x14→0x52`, `+0x16→0x60`, with
SkillMult at payload `+0x30` → runtime `+0x64` and InhJam1–4 at payload
`+0x5C..+0x62` → runtime `+0x56..+0x5C`. The semantics are pinned by the
consumers: `NovaUi_DrawSystemFactionConflictStatus` reads `0x46` (CrimeTol) as
the legal-status tolerance, `Game_ResetReputationAndAvailability` clamps system
reputation up to `0x52` (InitialRec), `Ship_ComputeShipEffectiveMaxSpeed`
scales by `0x64` (SkillMult), and `Government_TryTriggerGovtAssistanceEncounter`
compares `0x60` (MaxOdds) against `ai_odds_score`.

## Working doc

`<PRKnnn>` / `<SRKnnn>` per-government rank names are reconstructed. The
original splits this across a pre-scan (`Ship_ExpandStringPlaceholders`
`0x0044a4d0` latches the government id in `g_expanded_psrk_ship_class` /
`_ssrk`) and the later substitution (`Stellar_BuildTravelDestinationDescription`
`0x004444f0`, which writes `<PRK` + `(government + 0x80)` + `>`). The port
parses each token directly in `Mission_ExpandMissionWildcards` and calls
`Rank_HighestWeightedActiveSlotForGovernment`.
Deliberate divergence: the original's two per-government buffers are crossed
and both use ShortName (byte-verified: `<PRKnnn>` reads the ssrk-gated frame at
`+0x384`, `<SRKnnn>` reads the psrk-gated frame at `+0x344`, both `+0xde`), so
a lone `<PRK128>` expands to the "captain" fallback and the shipped Federation
mission text breaks. The port uses the Bible semantics (ConvName for
`<PRKnnn>`, ShortName for `<SRKnnn>`, each on its own government).

### Demand-tribute middle-button branch (0x00480030)

Per pulse it fires event 3, spawns defense-fleet ships up to the stellar
max/present bookkeeping, latches domination (`dominated` 1, tribute STR#
0xbba 25/26; release mirrors it with 35/36 and clears `dominated`), and
runs the stellar reaction scripts. TODO(decomp): the branch is not ported.

Pilot save/load persists the `g_rank_defs` active flags (FleetState +0x5dde,
one word per slot); see `docs/pilot_save_file_format.md`.

## Rank effects

- **Contribute mask** — `Mission_AccumulatePlayerContributeMask` 0x0046cca0
  ORs every active+defined rank's `+0x14/+0x18` into the player mask.
- **Daily salary** — `Mission_TickDailyWorldUpdate` 0x00466d63 pays `+0x0c`
  per game-day while active, stopping at a positive `+0x10` SalaryCap (0/-1
  uncapped).
- **Price modifier** — `NovaLanded_RankPriceScale` 0x00491f9b folds
  `PriceMod * 0.01` into `DAT_007d4bbc`/`DAT_007d4bc0` for every active+defined
  allied rank; consumed by outfit/ship/trade-in/hire prices.
- **Honors** — `NovaUi_BuildPlayerSpecialInteractionStrings` 0x0049c050 lists
  active+defined rank full names by Weight descending before the 0x2000-flag
  outfits.

Deliberately unported: the demand-tribute event-3 branch (above) and
`Ship_HandlePlayerShipCore` 0x00453670 event 3 (skipped debug/cheat command arm).
