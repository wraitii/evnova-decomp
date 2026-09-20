# Contraband scanning (`Ship_ScanPlayerForContraband` 0x00401800)

Reverse-engineering + reimplementation note for the government smuggling scan.

## Entry and scheduling

- `Ship_ScanPlayerForContraband(ShipState *ship)` is called from exactly one
  place: `Ship_UpdateShipAiState` state 7 (escort/follow primary at range), at
  `0x00407811`, when the primary target is the player (slot 0), within 100 px,
  the maneuver timer has expired, and `pers_def_slot != 0x3ff`. The caller
  clears the secondary/primary targets and state to 0 first, then scans.
- Per-ship gates, in order: `ai_behavior_code` is 3 (warship) or 4
  (interceptor); `faction_or_government_id != -1`; government `SmugPenalty`
  (GovtDef +0x48) non-zero; both axis distances ≤ 100 (`FLOAT_0057501c`);
  `Ship_CanShipEngageTargetUnderCloakRules(player, ship)`; then **one**
  `NovaRandom_Range(100)` draw, accepting ≤ 75.
- Hyperspace-committed guard: if the player's `ai_station_hold_timer > 0` and
  the player is not disabled, the scan is skipped once the jump tunnel has
  passed its onset. In the port the hold phase is `TravelState::JumpPhase::kHold`,
  which also seeds and advances `Ship::ai_station_hold_timer` (2.0 at hold-begin,
  incremented each 30 Hz tick, 0x0044c548/0x0044c70a), so gating on the phase is
  equivalent to the original's field test; it delegates to
  `NovaTravel_PlayerPastJumpOnset` (same ramp schedule as the tunnel movement
  block: `elapsed60hz * multiplier / (duration * 0.01) − 35 / multiplier > 0`).
  The class `jump_duration_multiplier` is decoded from the shp Flags word by the
  loader (`NovaData_LoadScenarioResourceTables` 0x004bd3c0): bit0 → 0.7, else
  bit1 → 1.3, else bit2 → 1.6, else 1.0; all ×1.3, floored at 0.5. It scales
  both the ramp clock and the 'Warp up' cue duration scale (0x0046ab00); the
  SDL call receives the multiplier as a playback speed.

## The three arms

A single call handles the first matching mission cargo **and then**
independently the first matching outfit or junk — the mission loop `break`s and
control falls through.

1. **Mission cargo** (slots 0..0xF): `is_active`, `ActiveMission.scan_mask`
   (+0x1a, from misn payload +0x18) AND government ScanMask, `carrying_resources`
   (+0x33), `cargo_type_id` (+0x12) `!= -1`.
   - `flags_primary & 0x20` (`Mission fails if you're scanned`) **clear**, or
     the mission already failed: warning + optional fine, mission not failed.
   - set and not failed: mark `is_failed`, then **always** run
     `Mission_FailMissionSlotQuick`; only the STR# 0x7d2/0x11e voice and text
     are suppressed when `flags_primary_at_accept & 0x400`.
   - **Original quirk: the mission arm never fires faction event 0** (no
     reputation change) — only outfit/junk do.
2. **Outfit** (0..0x1FF), only when no scannable junk latch is set and the
   owned-outfit latch is set: `outfit_owned_count > 0` and `Outfit::scan_mask`
   (OutfitDef +0x28, payload +0x3ee) AND government ScanMask. Fires faction
   event 0, prints STR# 0x177 plus a/an + singular/plural name, and fines
   **flat-positive only** (the percentage arm does not apply to outfits).
3. **Junk** (0..0x7F), run instead of outfits when the junk latch is set:
   `inventory.junk_counts > 0` and `JunkDef::scan_mask` (g_junk_defs +0x26) AND
   government ScanMask. Fires event 0, prints the LCName, and uses the full
   flat/percentage fine. **Original quirk:** a junk type with a non-zero
   ScanMask suppresses the outfit arm even when its mask does not match this
   government's.

The candidate latches mirror the original globals:

| Ghidra | meaning | port field |
|---|---|---|
| `DAT_007356cc` | any held junk with non-zero ScanMask | `PlayerInventory::has_scannable_junk` |
| `DAT_007356cd` | any owned outfit with non-zero ScanMask | `PlayerInventory::has_scannable_outfit` |

They are rebuilt by `NovaOutfit_RecomputeOutfitDerivedState`
(`NovaOutfit_RefreshContrabandScanLatches`) and cleared by a successful scan.

## Field-name corrections

The original's illegal-cargo mask is `GovtDef +0x24` (payload +0x32), the
Bible **ScanMask** — formerly misnamed `ai_skill_percent`. `GovtDef +0x22`
(payload +0x04) is the Bible **Flags2** (formerly `scan_mask_short`), and
`GovtDef +0x68/+0x6c` (payload +0x54/+0x58) are the Bible **Require** pair
(formerly `scan_mask_lo/hi`). The outfit payload's `+0x3ee` field — between
ItemClass (+0x3ec) and BuyRandom (+0x3f0) and mapped by the loader to
`OutfitDef +0x28` — is the Bible outfit **ScanMask**; the port's former
`sprite_id` name was wrong (there is no outfit Graphic field in the resource).

## Fines

- `scan_fine > 0`: flat credits, clamped so credits never go negative.
- `scan_fine == 0`: warning only.
- `scan_fine < 0` (mission/junk only): `-scan_fine` percent of the player's
  credits. The original computes `credits * -scan_fine * 1e-4` in double, runs
  its inlined x87 `_ftol` truncation sequence (FIST then a fraction correction;
  the intervening float store feeds only the sign test), and multiplies by 100
  in 32-bit. The port truncates toward zero and preserves the 32-bit `*100`
  wrap; the minimum is 1 credit.
- Message text comes from STR# 0x7d2 entries 0x177–0x17c / 0x189/0x18a /
  0x11e; the port builds the same sequence and uses `GroupThousands`.

## Voice cues

`g_nova_control_bits[164]` (fine/warning) is
`g_transition_sound_handle_table[4]` = snd 154 and `[152]` (mission failure)
is table[1] = snd 151. `NovaAudio_PreloadGameplayData` (0x004b0740) fills
`g_nova_control_bits + 0x94 + i*4` with `NovaSound_LoadDecodedById(0x96 + i)`
(snd 150+i), so byte offsets 0xa4/0x98 are indices 4/1. The scan queues them
on the same `pending_ui_sounds` channel as the boarding cues (width 1, the
original descriptor's `NovaAudio_FillVoiceSlotDescriptor` argument).

## Dirty-latch lifecycle

`g_playerInventoryAndLoadoutDirty` after a fine is **not** a recompute trigger.
Its only reader is `NovaUi_RefreshGameplayPanels` (0x0045d320),
`if (dirty) { NovaUi_DrawCargoMissionStatusPanel(); dirty = 0; }` — a
cargo/mission HUD render-cadence latch (the clean-room HUD redraws every frame,
so it has no port). The scan candidate latches therefore stay cleared until a
real inventory mutation calls `Outfit_RecomputeOutfitDerivedState`; repeated
scans against unchanged inventory do not re-fire.

## Remaining gap

Caller only: `g_ai_misc_event_flag = 1` and the `pers_def_slot == 0x3ff`
shareware/licence nag arm in the state-7 caller (`Ship_UpdateShipAiState`,
0x00405590) are unported. The scanner body has no known gaps.
