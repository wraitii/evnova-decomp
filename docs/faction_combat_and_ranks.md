# Faction combat events and ranks

Reverse-engineering + reimplementation notes for the crime → reputation →
rank-revocation chain:

| Address | Ghidra name | Port tracker |
|---|---|---|
| `0x00466FC0` | `Government_ProcessFactionCombatEvent` | 0% |
| `0x00467140` | `Government_PropagateFactionCombatInfluenceToNearbySystems` | 0% |
| `0x00427F40` | `Rank_Deactivate` | 0% |
| `0x00427DF0` | `Rank_Activate` | 0% (needed for the K opcode / `<RRK>`) |
| `0x0046F100` | `Government_IsReputationTrackingGovernment` | unported (name suspect) |

The ränk (`r\x8ank` 0x728a6e6b) definition table `g_rank_defs` is not modelled
at all yet; `GameState.rank_active_flags` (save only) and
`PilotControlState.active_ranks` (K/L bitset) are ad-hoc stand-ins.

## Call graph and event codes

`Government_ProcessFactionCombatEvent(system_id, faction, event_code,
mission_fleet_slot)` is the single entry point. Ten call sites, all passing
`mission_fleet_slot == -1` (the `!= -1` arm is inert in the shipped data):

| code | meaning | call sites |
|---|---|---|
| 0 | smuggling detected (`Government_HandlePlayerFactionExtortion`) | `0x00401A1D`, `0x00402399` |
| 1 | ship **disabled** (`Ship_IsShipDisabled`) | `0x00419721` |
| 2 | **boarded** | `0x0045A85F` |
| 3 | **killed** / stellar destroyed / attack-stellar | `0x0041976F`, `0x004381EF` (×10), `0x00453670`, `0x00480E4B`/`0x00480F9B`/`0x0048102B` (×5) |

Kill/disable is the gate for rank flag `0x0004`; "any crime" is flag `0x0040`.
The stellar-destruction arm fires event 3 ten times and the travel-window arm
five times (amplified reputation pulses; the propagate visited-mask suppresses
re-propagation, the rank loop re-runs).

The event-0 caller is the smuggle-fine path: it composes STR# 0x7d2 entries
0x178–0x17c ("your attempt to smuggle illegal cargo through this system has
been detected. You have been fined …") and gates on the government's
SmugPenalty at runtime `+0x48`.

## ProcessFactionCombatEvent shape (decoded)

1. Early-out when `faction != -1 && (govt.flags_primary & 0x0800)` — the Bible
   "derelict" bit, the same test as `Government_IsReputationTrackingGovernment`.
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
| `+0x0C`/`+0x10` | Contribute (64-bit) | payload `+0x06`/`+0x0A` |
| `+0x14` | Salary | payload `+0x0E` |
| `+0x18` | SalaryCap | payload `+0x12` |
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
the legal-status tolerance, `Game_ResetNewGameReputation` clamps system
reputation up to `0x52` (InitialRec), `Ship_ComputeShipEffectiveMaxSpeed`
scales by `0x64` (SkillMult), and `Government_TryTriggerGovtAssistanceEncounter`
compares `0x60` (MaxOdds) against `ai_odds_score`.

## Working doc

Living checklist to drive all four functions to 100%. Grouped so each phase is
independently testable and tracker rows can be updated in the same change.

### Phase A — rank model + activate/deactivate
- [ ] Add `RankDef` to `ScenarioData` (active/defined/id/weight/govt/flags/
      salary/salary_cap/contribute/price_mod/full_name/conv_name/short_name)
      and decode `r\x8ank` in the scenario loader (mirror the loader offsets
      above; id = slot).
- [ ] Port `Rank_Activate` (`0x00427DF0`) and `Rank_Deactivate`
      (`0x00427F40`): sibling clearing on `0x0001/0x0002/0x0010/0x0020`,
      permanent `0x0008` skip, and `g_recently_activated_rank_id` maintenance.
- [ ] Replace the `PilotControlState.active_ranks` bitset with the real table
      and reconcile `GameState.rank_active_flags` save/load with it.
- [ ] Point the K/L mission-script opcodes at the new activate/deactivate.
- [ ] Rows: `0x00427DF0`, `0x00427F40`.

### Phase B — PropagateFactionCombatInfluenceToNearbySystems (`0x00467140`)
- [ ] Port the visibility-chain walk, relation-dependent delta, 16-way
      adjacency recursion (`×0.65`), and ±32000 clamp.
- [ ] Use a per-call visited mask matching `g_stellar_flood_visit_mask`
      semantics (clear at process entry, as in the original).
- [ ] Row: `0x00467140`.

### Phase C — Government_ProcessFactionCombatEvent (`0x00466FC0`)
- [ ] Port the derelict early-out, mask clear, propagate call, and rank
      revocation loop.
- [ ] Give `mission_fleet_slot` real semantics (currently only `-1` is
      reachable; document/branch the `!= -1` arm).
- [ ] Wire the four port sites: `collision.cpp` kill + stellar-destroy,
      `boarding_plunder.cpp` board, `negotiation_dialog.cpp` demand-tribute
      branch.
- [ ] Row: `0x00466FC0`.

### Ghidra improvements (do alongside)
- [ ] Set `0x00466FC0` and `0x00467140` return types to `void`.
- [ ] Rename `Government_IsReputationTrackingGovernment` — it returns true on
      `flags_primary & 0x0800` (derelict), and callers use it to *skip* the
      kill event; the name is inverted. Add a plate comment.

### Validation notes
- Pure data/logic: add a scenario-data test for `r\x8ank` decode (Federation
  Commander id `0x80`: weight 1, govt 0x80, PriceMod 85, flags 0xB08) and unit
  tests for activate/deactivate flag semantics.
- In-engine: triggering an event 3 and observing `system_reputation` deltas +
  rank revocation needs gameplay validation (ask before using the probe).
