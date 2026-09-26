# Auto-abort CompReward + domination/"Avoid" mission investigation

Handoff for a side-investigation. Background and the pending code decision live
in `docs/known_original_bugs.md` ("Auto-aborted missions can omit their
legal-status reward") and the `Mission_ResolveMisnSlot` comment block in
`src/game/mission.cpp`. This note is the evidence collected so far and the open
questions.

## Why this matters

`Mission_ResolveMisnSlot` (`0x00447d90`) never applies the
`CompGovt`/`CompReward` record change that `Mission_ResolveMissionSuccess`
(`0x00440410`) applies. That is the real "legal-status reward" omission. The
`PayVal` legal-record variants (`-10128…`) are **not** the bug — the auto-abort
branch already calls `Government_ApplyReputationCreditDelta(PayVal)` when
`Flags2 0x0002`, and the disassembly at `0x00447e1e`–`0x00447e3a` plus
`0x00440750` confirm the `-10128` branch works.

The open decision is **which reward to apply on auto-abort**:

- **Option A** — mirror the success walk (`+CompReward` full to `CompGovt`
  systems, ±half to hostile/allied). Matches the forum text ("legal status that
  would be given if the mission were completed successfully").
- **Option B** — treat auto-abort as an abort: apply `comp_reward_delta * -5`
  when `Flags 0x0040`, else the success walk. Makes `0x0040` meaningful.
- **Option C** — apply `CompReward` only when `Flags2 0x0002`.

The 14 "Avoid" missions are the only auto-abort missions carrying
`Flags 0x0040`, so they are the crux of A-vs-B. (All 14 have `CanAbort = 0`: the `-5x` path is unreachable.)

## Confirmed facts

### Stock auto-abort set (decoded from `EV Nova/Nova Files/Nova Data *.rez`)

791 missions total, 44 with `Flags 0x0001`. Of those:

- **1** uses a legal-status `PayVal`: mission **896 "Clean Fed Record"**
  (`PayVal -10128`, `Flags2 0x0002`, `CompGovt -1`). Already handled correctly.
- **20** have `CompGovt != -1` and `CompReward != 0` (the fix's real blast
  radius). 2 more have `CompGovt` set but `CompReward == 0` (662, 748).

| group | missions | goal / trigger | CompGovt | CompReward |
|---|---|---|---|---|
| Refuel Trader | 141, 650, 651, 652 | `ShipGoal 5` rescue; board the 1 disabled trader | Civvies | +2 |
| Avoid family | 614–627 (see below) | `ShipGoal 0` destroy; kill the fleet | (varies) | +1…+30 |
| Thunderforge cron step | 747 | no goal/ships; resolves immediately on start | Family Heraan | +2 |
| Eamon boarding | 909 | no goal/ships; resolves immediately on boarding | Wild Geese | **−200** |

### The "Avoid" family (614–627)

All 14: `Flags 0x1445` (`0x1` auto-abort, `0x4` can't refuse, `0x40` −5x
CompReward on abort, `0x100` green arrow, `0x1000` undocumented), `Flags2 0`,
`CanAbort 0`, `PayVal 0`, **all text ids 0**, `AvailLoc 3` (spaceport),
`AvailRandom` 10–20%, `ShipGoal 0`.

| mission | gate | fleet düde | CompGovt | CompReward |
|---|---|---|---|---|
| 614 / 615 | `b6100` / `b6200` | 130 "Lone Big Federation Ship" | Federation | +2 / +5 |
| 616 / 617 | `b6101` / `b6201` | 171 "Big Rebels" | Rebellion | +4 / +10 |
| 618 / 619 / 620 / 621 | `b6102` / `b6202` | 138 "Large Polaris" | Rebellion / Polaris | +10 / +30 |
| 622 / 623 | `b6103` / `b6203` | 155 "Large Auroran War Ships" | Rebellion | +3 / +10 |
| 624 / 625 | `b6104` / `b6204` | 172 "Pirates" | Pirate | +1 / +5 |
| 626 / 627 | `b6105` / `b6205` | 151 "Wild Geese" | Wild Geese | +5 / +20 |

Every gate is `b61xx/b62xx & !b9812`.

- `b61xx` lives in the stellar's **`OnDominate`** script (spöb payload
  `+0x36`); `b62xx` in **`OnDestroy`** (`+0x246`).
- The original sets `OnDominate` on a successful **Demand Tribute**:
  `NovaUi_RunTravelDestinationInteractionWindow 0x00480030` sets
  `dominated=1, domination_days=0` and calls
  `Mission_ExecuteReactionScript(StellarDef +0x68)` (the `0x00480f…` block of
  the `function/0x00480030/decompile` output; runtime offsets: OnDominate
  `+0x68`, OnRelease `+0x167`, OnDestroy `+0x266`).
- On accept/abort/success/failure the missions run `G348 !b61xx` — grant
  **outfit 348 "Bureau Bomb Outfit"** and clear the trigger bit. `on_ship_done`
  sets `b9812`, retiring the whole family; mission **197 "Learn About Vell-os"**
  also sets `b9812`.
- The same G348 marker is used across the Bureau of Internal Investigation
  story set (438, 443, 447, 453–474, 596, 597, 613), so these read as Bureau /
  domination story beats, not tuned bounties.
- The sign is odd: the fleet is the *same* faction as the dominated planet and
  as the reward government, and the reward is positive (dominate a Pirate
  planet, kill Pirate ships, gain Pirate record).

### Eamon boarding (909)

- përs **443–448 "Eamon Flannigan"**: `Govt 144` (Wild Geese),
  `Flags 0x0303` (grudge `0x1`, escape pod `0x2`, deactivate-after-accept
  `0x100`, **offer LinkMission on boarding `0x200`**), `LinkMission` payload
  `+0x30 = 909` → mission resource 909 (`scenario_data.cpp:204` rebases to the
  0-based index; `scenario_data.hpp:276`).
- Mission 909: `Flags 0x0005` (auto-abort + can't refuse), `ShipCount 0`,
  `ShipGoal -1`, all text ids 0, `on_accept = b801`,
  `on_abort = K152 L138`, `CompGovt = Wild Geese`, `CompReward = −200`,
  `Flags2 0x0002`.
- `K152` = activate rank 152 **"Sworn Enemy of the Wild Geese"**;
  `L138` = deactivate rank 138 **"Knight of Red Branch; Wild Geese 1"**.
- `b801` blocks mission 634 "Take Michaleen to New Ireland"
  (`avail = !((b800|b801)|b9111) & …`), i.e. it closes the friendly Wild Geese
  branch. `b800` = Michaleen completed; `b9111` = Heraan-initiation alternate
  (mission 266).
- Because `ShipGoal -1`/`ShipCount 0`, the objective latch sets on the first
  reaction tick (`Mission_HandleMissionOrSurrenderShipReaction 0x00443c60`;
  the `ship_goal == -1 && mission_target_count < 1` arm sets the latch, and the
  first-completion arm near the tail reads `flags_primary & 1`), so 909 always
  resolves through the abort path. There is no success path.

## Open questions

1. **Are the Avoid missions actually player-facing?** They have zero text and
   `AvailLoc 3`. Determine whether the spaceport BBS presents a mission with
   BriefText 0, and whether `AvailRandom`/`AvailStel` make it appear. If they
   are never offered, they are internal bookkeeping and the fix's effect is
   cosmetic for them.
2. **What activates the Avoid missions?** `on_accept` clears `b61xx`, so
   something must start them while the bit is set. Search for `S614…S627`
   (record ids 614–627) and for the `0x1000` flag's meaning. Is `0x1000` a
   silent/auto-start flag?
3. **Are the "Avoid" fleets hostile?** Decode the düde composition and the
   spawned ships' përs/government. Do they attack the player, or just exist?
4. **Why a positive, same-faction CompReward?** Splinter groups using parent
   hulls, or scenario-data that was never tuned? Look for context in the Bureau
   plot missions and the `b9812` / `b9111` story branches.
5. **What is outfit 348 "Bureau Bomb Outfit" for?** Is it a usable outfit or a
   pure story marker? Which later mission consumes it?
6. **Eamon boarding direction.** Is boarding Eamon only reachable when already
   hostile, or does the friendly "Meet With Eamon Flannigan" (684/690, düde
   130) also spawn the 443–448 përs? If the latter, is the immediate enmity a
   design bug?

## Reproduction / tooling

Resource decode (no build needed):

```sh
# Single mission, all fields + text
python3 tools/mission_data.py --id 614 --full --text
python3 tools/mission_data.py --name 'Avoid' --full

# One-off scan used for the table above (Stock scenario only): import
#   parse_archive() from tools/mission_data.py (add tools/ to sys.path or
#   load the file by path), then filter by type code:
#   mission 0x6d95736e, spob 0x73709a62, dude 0x649f6465,
#   pers 0x70917273, rank 0x728a6e6b, outfit 0x6f9f7466
#   mïsn offsets: 0x1c PayVal, 0x20 ShipCount, 0x24 ShipDude,
#   0x26 ShipGoal, 0x2e CompGovt, 0x30 CompReward, 0x50/0x52 Flags,
#   0x04 AvailLoc, 0x5c ActiveOn
```

Ghidra:

```sh
./tools/ghidra_api "function/0x00443c60/decompile"     # objective latch
./tools/ghidra_api "function/0x00447d90/decompile"     # auto-abort resolve
./tools/ghidra_api "function/0x00440410/decompile"     # success walk
./tools/ghidra_api "function/0x00480030/decompile"     # Demand Tribute / OnDominate
./tools/ghidra_api "function/0x00446150/decompile"     # player abort / 0x40 reversal
curl -s 'http://127.0.0.1:8166/type/StellarDef/layout' # runtime offsets
```

Gameplay validation (ask the user first): use the probe harness
(`docs/probe_harness.md`) to dominate a stellar, watch the spaceport for an
"Avoid …" offer, and inspect spawned ships. A mission-script trace may be more
practical than screenshots.

## Deliverables when this closes

- Decide **A / B / C** above; record the rationale at the `BUGFIX(original)`
  site in `src/game/mission.cpp` (`Mission_ResolveMisnSlot`, ideally factoring
  the success walk into one helper used by both sites).
- Update `docs/known_original_bugs.md` — mark the entry `(fixed)` or re-scope it
  Mac-only if the investigation overturns the interpretation.
- Update the `0x00447D90` row in `decomp-progress.tsv` and the `mission.hpp`
  comment block.
- If the Avoid missions turn out to be internal, note that in the fix comment so
  the `+CompReward` effect on them is intentional/understood.
- Optional: a `tests/mission_test.cpp` case that drives the auto-abort arm with
  a `CompGovt`/`CompReward` mission and asserts the record change under the
  bug-fix policy.

## Resolution (closed)

The investigation is complete; the fix landed as an opt-in `BUGFIX(original)`.

**Open questions answered**

1. **Player-facing?** Yes. The offer text is a `dësc` loaded as
   `mission_def + 4000` (`NovaMission_RunOfferWindow`), not the mïsn's
   `BriefText` (which is 0 for the whole family). The 16 avoid missions map
   one-to-one to `dësc` 4486–4501 ("As soon as you land, you see a squad …").
   `AvailLoc 3` offers them in the spaceport dialog.
2. **What activates them?** `OnDominate` (`b61xx`) / `OnDestroy` (`b62xx`) of a
   stellar in the matching government group; the bit is global, so once set
   the mission is offered at any stellar of that government (per `AvailStel`),
   not only the dominated one. `b9812` (set by mission 197 "Learn About
   Vell-os") permanently retires the family.
3. **Hostile?** The fleet düdes carry the dominated government (Fed 0,
   Rebellion 13, Polaris 2 / Nil'kemorya 19, Auroran 1, Pirate 9, Wild Geese
   16, independent for 628/629) with `AI 3`–`4`, so they spawn as hostiles.
4. **Positive same-faction `CompReward`?** It is the *success* value; on abort
   `Flags 0x0040` reverses it (`CompReward * -5`, exact-government systems).
   The sign is not a data error.
5. **Outfit 348?** "Bureau Bomb Outfit", `ModType 0x2f`, `ModVal 3220` = its
   Called Desc. Only 614/615 grant it (via `G348`); 616–629 only clear the bit.
6. **Eamon direction.** Boarding a hostile Eamon links mission 909; it is
   `ShipGoal -1`, so it auto-aborts on the first tick. The rank payload
   (`K152 L138`) carries the plot consequence; the −200 `CompReward` is the
   reputation half.

**Corrections to the original note**

- The family is **16** missions, not 14: 628/629 "Avoid Bounty Hunters"
  (düde 150, `b6106`/`b6206`, 5% random, `CompGovt -1`, `CompReward 1`).
- Only 614/615 run `G348 !b61xx`; the rest run `!b61xx` (and `!b61xx b9812` in
  `OnShipDone`).
- `CompReward` was applied to **none** of them: all are `Flags 0x0001`, so
  `Mission_ResolveMisnSlot` (0x00447d90) resolves them and never ran the walk.
- `Flags 0x0040` was dead for them: its reversal lived only in the manual
  mission-computer abort (0x00446150), unreachable with `CanAbort 0`.
- The fleet restore (`System_RebuildInitialNpcAndMissionPopulation` 0x0041af90,
  port `NovaSystem_RestoreMissionFleets`) also calls `Mission_ResolveMisnSlot`
  for any restored `Flags 0x0001` mission, so the Avoid missions resolve on
  launch / system entry without the player finishing the destroy goal.

**Decision: Option B, narrowed to the abort-flag opt-ins**

Apply the walk on auto-abort only when the mission declares an auto-abort
consequence with either documented flag: `Flags2 0x0002` (pay on auto-abort)
or `Flags 0x0040` (reversal on abort). `0x0040` selects the manual `-5x`
reversal (exact-government systems); otherwise the normal success walk runs.
That covers every stock auto-abort `CompReward`: the Refuel Trader missions
(+2 Civvies) and Eamon (−200 Wild Geese) via `Flags2 0x0002`, and the 16 Avoid
missions via `Flags 0x0040`. It leaves the Thunderforge cron step (747, neither
flag) inert. Gated by `BugFixPolicy`; with it off the original
omission is reproduced. The observation that `0x0040` alone did not grant the
reward (it only fed the manual mission-computer abort) is what makes the
omission a bug rather than a deliberate "manual-only" feature.

Implementation: `ApplyCompetingGovernmentReputation` in `src/game/mission.cpp`
is shared by `Mission_ResolveMissionSuccess` and the opt-in arm of
`Mission_ResolveMisnSlot`; the `0x00447D90` tracker row, `mission.hpp`, and
`docs/known_original_bugs.md` are updated. Test:
`tests/mission_test.cpp` "auto-abort applies the competing-government reward
under the fix policy".
