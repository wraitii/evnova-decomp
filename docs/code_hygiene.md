# Code hygiene — open work items

Scope: comment accuracy, reference integrity, and divergence logging across
`src/` and the Ghidra DB. Name/address reference integrity is complete and
machine-checkable; the items below are the remaining manual work.

## Status

| Area | State |
|---|---|
| Ghidra address citations for all reimplemented functions | ✅ complete (audit: 0 missing) |
| Ghidra ↔ progress.csv name agreement | ✅ complete (audit: 0 drift) |
| Citation / divergence-marker format | ✅ settled, documented in AGENTS.md |
| Stale "Phase N" / narration comments | 🟧 non-ship_ai done; ship_ai deferred to item 1 |
| Comment-only `if` blocks in ship_ai.cpp | ☐ open (deferred, item 1) |
| Divergence-marker sweep (existing TODO(decomp)s) | ✅ complete (77 markers reviewed; 2 deliberate-skips converted; citations normalized) |
| 0x004b8ed0 color-remap gameplay question | ✅ resolved — remap is dead code at runtime |

Verification: `python3 tools/ref_audit.py` — rerun after any rename (code,
csv, or Ghidra) to catch drift.

## Open items

### 1. ship_ai.cpp stale deferral markers
~30 comments say "deferred, Phase 5/7/8"; several contradict reality
(`NovaAi_UpdateAutoWeaponSelectionFromTarget` is 45% ported and wired into
modes 1/2). Six comment-only `if` blocks (~lines 2083, 2114, 2356, 2525,
2570, 2933) must each be resolved:
- decompile `0x0044AA70` (Ship_HandlePlayerShipCore) dispatch for the mode in
  question; wire the helper in if the original calls it there
- otherwise replace with an explicit divergence note

### 2. Narration comments (~46 sites)
Pattern: `//.*\b(now|previously|no longer|reintegrated|Phase \d)\b`,
plus date stamps. Rewrite as descriptions of current behavior; history
belongs in commits. Worst: ship_ai.cpp header block.

Progress (this pass): rewrote the stale staging/history/date-stamp narration
in non-ship_ai files (game_state.hpp, scenario_data.cpp/.hpp, spaceflight.cpp,
new_pilot_flow.cpp, ship_spawn.cpp, landed_window.cpp, weapon.hpp,
spaceflight_view.cpp). Surviving `now/previously/no longer` hits in non-
ship_ai files are legitimate current-behavior phrasings ("item no longer
offered", "may fire right now", "for now", "def no longer exists") and were
left. ship_ai.cpp's "Phase 5/7/8" narration is deferred to item 1 (it is
intertwined with the stale-deferral cleanup there).

### 3. Divergence-marker sweep — ✅ complete
Format is settled in AGENTS.md: plain `TODO(decomp)` = unported scope,
`TODO(decomp(0xaabbccdd)) skipped: <reason>` = deliberate skip of known
behavior. Remaining work: sweep the ~77 existing TODO(decomp) markers into
the convention and normalize older citations to `// Ghidra 0x… Name.` where
they use pre-sweep variants (`Mirrors X (0x…)`, bare `// 0x…`).

Progress (this pass): reviewed all 79 `TODO(decomp)` markers. The vast
majority are genuine unported scopes (depend on not-yet-reconstructed
systems/fields) and correctly stay plain `TODO(decomp)`. Two were converted
to the deliberate-skip form:
- weapon.cpp `Weapon_FireShipWeapons` combat-rating cooldown scale →
  `TODO(decomp(0x00414550)) skipped:` (g_player_combat_rating_points untracked).
- government.cpp `Government_IsShipEligibleForGovernmentAid` +0x83 gate →
  `TODO(decomp(0x0040fd20)) skipped:` (no clean-room +0x83 field).

Citation normalization (this pass):
- All primary-citation `Mirrors X (0x…)` plate comments (directly above a
  ported function, in targeting.hpp/.cpp, travel.hpp/.cpp, outfit.hpp/.cpp,
  asteroid.hpp/.cpp, ship_spawn.hpp, gameplay_interface.cpp) normalized to
  `// Ghidra 0x… Name.` (descriptions preserved).
- Bare `// 0x…` head annotations in landed_store.hpp normalized to
  `// Ghidra 0x…`.
- Left as-is (per “many need no change”): mid-prose `Mirrors`/`// 0x`
  references and `DAT_…`/data-address citations (pilot_file.hpp,
  hud_renderer.cpp, spaceflight.cpp, ship_ai.cpp, preferences.hpp,
  preferences.cpp, starmap.cpp, landed_store.cpp, brgr_archive.cpp), and the
  module headers already phrased as “mirroring Ghidra 0x… Name.”
  (new_pilot_flow.hpp, spaceflight.hpp, intro_cinematic.hpp).

`ref_audit.py` clean after the sweep (0 drift, 0 missing, 0 non-function).

### 4. 0x004b8ed0 color-remap tail — ✅ resolved
`Resource_LoadPictAsImage` omits the optional post-load color remap
(DAT_0085faee table, 8/16-bit paths). Determined: **no runtime caller triggers
it.** The WithColorRemap variant `0x004b8ed0` has exactly two callers, both of
which pass a zero remap flag: `0x004b9050` (the plain convenience wrapper)
and `FUN_004b9060`. The post-load remap branch (`param_2 != '\0'`) is dead
code at runtime, so omitting it introduces no visual divergence. (The
`DAT_0085faee` table itself is a general render color-lookup table also used
by `DrawContext_FillRect16`, `DrawContext_FrameRect16WithCurrentColor`, and
the sprite RLE code — those paths are separate from the PICT remap.) Source
comments in `pict_image.cpp`/`.hpp` updated to note this.
