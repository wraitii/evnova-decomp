# The in-flight Player Info window ("special interaction" family)

Ground truth for the special-interaction family in the
Ghidra DB. Despite the names, this is the **Player Info modal** the manual
describes under "Player Info — press the P key": four screens (General /
Cargo / Extras / Honors) plus a Jettison Cargo action on the Cargo page.
All addresses are Ghidra DB addresses. Companion to
`docs/preferences_keybindings.md` (binding model) and
`docs/boarding_plunder_capture.md` (the modal idiom used by the port).

## Trigger

`Ship_HandlePlayerShipCore` (0x0044aa70) block 0x00451b91..0x00451c4f polls
`NovaInput_IsCommandActiveWithGameplayGuards(g_player_key_bindings[25])`
(default key `P`, `NovaPrefs_ResetKeyBindings` 0x004b4400 writes 0x19 into
slot 0x19). With the edge latch `DAT_007cab3f` clear and the usual
hold-timer / death-timer gates, it hides the travel-selection sprite, redraws
the viewport + radar, shows the cursor, runs
`NovaUi_RunPlayerSpecialInteractionWindow` (0x00499c10), then resets the
average-frame-time accumulator, rebuilds the stellar radar panel, replays the
cached overlay message and marks the travel/status panels dirty. The 0x19
binding is drained afterwards (`Input_PumpAndTestCommand` loop, also inside
the dispatch callback for the close keys).

## Window anatomy (DLOG 0x3f9, DITL 0x3f9)

- Backdrop: three PICTs 0x2146/0x2147/0x2148 (top / middle tile / bottom
  slice, `DAT_007d4be0/.4/.8`), blitted to the window rect with the middle
  stretched; the text-view rect is clamped between the top and bottom art.
- DITL entry 6 = the scrolling text view (a `NovaTextView_Create`
  0x004bcd90 read-only view, Geneva-9 via `DAT_00735684/86`).
- DITL entries 2..5 = the four page tabs across the window **top**
  (y 8..33); entries 1 (Done) and 7 (Jettison) sit on the bottom row
  (y 195..220). Labels are STR# 0x96 ("titles") pstrings fetched through the shared runtime label
  table `DAT_007d83aa` (256-byte pstring stride) with the icon-id/entry table
  `DAT_007d830e = {4, 0x23, 0x24, 0x25, 0x26, 0x3c}`:

  | strip idx | DITL entry | STR# 0x96 entry | text | maps to |
  |---|---|---|---|---|
  | 0 | 1 | 5 (0x04) | Done | action 1 (close) |
  | 1 | 2 | 36 (0x23) | General | action 2 → page 1 |
  | 2 | 3 | 37 (0x24) | Cargo | action 3 → page 2 |
  | 3 | 4 | 38 (0x25) | Extras | action 4 → page 3 |
  | 4 | 5 | 39 (0x26) | Honors | action 5 → page 4 |
  | 5 | 7 | 61 (0x3c) | Jettison Cargo | action 7 (cargo page only) |

  Buttons render through the shared `NovaUi_DrawThreeStateButton`
  (0x004a3340); the strip's special label bytes draw vector glyphs, so the
  buttons read as icon tabs in-game. Strip index 5 is only enabled while the
  cargo page is up and `Ship_HasAnyCargoLootOrActiveMission`
  (0x0046f140) is true (the draw arm passes the current page as param_1 and
  gates `acStack_20[5]` on `param_1 == 2`).

## Run flow — NovaUi_RunPlayerSpecialInteractionWindow (0x00499c10)

Guards (return without opening):
`BOOL_007354a8` (a modal already active), `g_ship_states->ai_station_hold_timer`
must be exactly +0.0 (the decompiler shows the float bits as `0x40`), and the
death-timer field must be <= the constant at `DAT_00575990` (0.0-scale float).

Body:
1. `Weapon_ReconcileOutfitPoolWithWeaponBanks` (0x00462ec0) — ledger refresh.
2. `g_player_special_page = 1`; window = `UiWindow_CreateFromDialogResource`
   (0x004cf760, DLOG 0x3f9, draw 0x0049a540, dispatch 0x0049a3a0).
3. Load the three backdrop PICTs; save/set the window draw context.
4. `NovaUi_BuildPlayerSpecialInteractionStrings` (0x0049c050) fills
   `g_player_special_cargo_summary_text` (0x7d5278),
   `g_player_special_outfit_summary_text` (0x7d6278),
   `g_player_special_specialoutfit_summary_text` (0x7d7278).
5. Auto-grow: the three texts are measured through the text view (max content
   height `sVar11`); if the entry-6 rect is shorter than that, the window
   surface grows by `delta = content - view_height + 4`
   (`FUN_004d18b0` resize + `FUN_004d1a30` y-shift by `-delta * DAT_00575940`),
   and entries 1, 6, 7 are offset down by `delta` (Rect_Offset +
   `UiPanel_SetEntryRect`).
6. Release the main render-owner handle through
   `NovaRender_ReleaseMainWindowOwnerGuarded` (0x0046e8b0; a platform no-op
   in this build), then flush input and draw;
   modal loop.

Loop (`NovaUi_PollTravelScriptAction` 0x00492f10 over the window with
dispatch 0x0049a3a0, out action `local_2e`):

- action 1 → done (close).
- actions 2..5 → `g_player_special_page = action - 1` + `UiWindow_MarkDirty`.
- action 7 with `g_player_special_page == 2` (cargo page): if
  `Ship_HasAnyCargoLootOrActiveMission`, confirm STR# 0x7d2 entry 0x123
  ("Are you sure you want to jettison your cargo?") via
  `Ui_ShowConfirmDialog` (0x004977d0, DLOG 0xbba); on OK run
  `Player_RedistributeFleetCargoOverflow(true)` (0x0041f330 — jettisons all
  non-mission cargo/junk from the fleet) and close.
- `DAT_00596d39` latches done (script-driven close).

Teardown: restore context, drain the P binding, destroy the window
(`NovaUi_DestroyWindowGuarded` 0x0046e8e0), free the three PICT images,
restore context, flush input.

### Dispatch callback (0x0049a3a0, inside the run function's span)

Event record: +0 event type (word), +2 key code (byte), +0xa/+0xc mouse
x/y (words), +0xe shift flag (bit 0x200).

- Key events (type 3/5): Tab (0x09) cycles `g_player_special_page` 1..4
  (shift reverses, wraps), marks dirty and returns action `page + 1`.
  Enter (0x0d), Esc (0x1b) or a still-held P binding return action 1.
  Everything else returns 0 (not handled).
- Mouse-down (type 1): packs the point into (x | y<<16), redraws the window
  once, then calls `NovaUi_HandlePlayerSpecialInteractionTabs`
  (0x004a1ae0, page, point) and maps the released strip index:
  0 → 1 (close), 1..4 → 2..5 (page select), 5 → 7 (jettison),
  otherwise -1 (consumed, no action).

## Tab strip tracker — NovaUi_HandlePlayerSpecialInteractionTabs (0x004a1ae0)

`int NovaUi_HandlePlayerSpecialInteractionTabs(short page, uint packed_point)`
where `packed_point` is x in the low half and y in the high half
(window-relative; `NovaMouse_GetWindowRelativePosition` 0x004d17e0 subtracts
the current window record's origin). Fetches the rects of DITL entries 1..5
and 7, finds the strip index containing the point, and if any, draws the
pressed state (`NovaUi_DrawPlayerSpecialInteractionTabs(page, hover)` +
`NovaUi_BlitDirtyWindowSurfaces(0)` 0x004d16f0), then loops while
`NovaInput_PumpAndHasPendingEvents` (0x004b68f0 — pump, then return
`DAT_008701a0`): re-reads the mouse, re-hit-tests, and redraws whenever the
hover index changes (a change from the release index or the original press
index redraws once more). Returns the final hover index (initial press index
if the pointer never leaves the button, -1 if the press started outside).
Confidence: high.

## Tab strip painter — NovaUi_DrawPlayerSpecialInteractionTabs (0x004a1c40)

`void (short page, short pressed)` — draws the six strip buttons from the
`DAT_007d830e` icon/entry table: enabled always for 0..4, index 5 only when
`page == 2` and the cargo/junk/mission scan passes. The button matching
`pressed` *or* `page` draws with the pressed flag (so the active page's tab
latches down); the rest draw normal. Each button gets its rect from the DITL
(`UiPanel_GetEntryInfo` entries 1..5, 7) and the label pstring from
`DAT_007d83aa + id * 0x100`, drawn via `NovaUi_DrawThreeStateButton` on the
window's draw context (`UiWindow_GetDrawContextHandle` 0x004d17c0) after
`DrawContext_SetRgbColor(PTR_DAT_00575acc)`.

## Draw — NovaUi_DrawPlayerInfoWindow (0x0049a540)

Fills the window with the panel colour, blits the three backdrop PICTs (top
at the window top, middle tile stretched to cover the text area, bottom at
`window_height - bottom_art_height`), then per page into entry 6:

- **Page 1 — General.** Two-column stat grid (labels in the dim colour
  `SHORT_ARRAY_00733b50`, values in `PTR_DAT_00575ad8`, rows every 0x10):
  left: STR# 0x7d2 0xfb Pilot Name (g_player_name), 0xfc Current Date
  (`NovaText_FormatDateString`), 0xfd System (system def display name;
  cheat mode appends the raw id), 0x146 government/legal row
  (`NovaUi_DrawSystemFactionConflictStatus` or STR# 0x18c "N/A"),
  0xfe Combat Rating (`NovaUi_DrawCombatRankLabel`; cheat appends
  `g_player_combat_rating_points`), 0x5c-shield row (DAT_0072decc label;
  "N/A" without shields, "Shields Down" when empty, percent + `DAT_0056d16c`
  otherwise), 0x6c armor row (DAT_0072e4cc; destroyed → STR# 0x105), 0x7c
  fuel row (percent + remaining jumps; 0xef/0x106/0x107 fragments).
  right: 0xff Ship Name (g_player_ship_name), 0x100 Ship Class (class def
  field_0x6c), 0x101 Turn Rate (`Ship_ComputeShipMaxTurnRateDeg *
  DAT_005759b8`, rounded, + STR# 0x102 " deg/sec"), 0x103 Thrust
  (`Ship_ComputeShipEffectiveThrust * DAT_005759c0`), 0x104 Max Speed
  (`Ship_ComputeShipEffectiveMaxSpeed * DAT_005759c8` [* `DAT_005759d0` when
  `g_strict_play`], i.e. velocity-capped variants), credits row (0x5c right,
  `DrawContext_DrawGroupedUInt(g_ship_states->credits)` prefixed by the
  translated key glyph `DAT_0072f1cc` + `DAT_0056d168`), plus the fleet
  value block: for each of the 0x40 escort slots (stride 0xc948) with
  `field_0xbb` set and not a mission fleet spawn (goal 1), sum
  `ship_class_defs[class].base_cost * DAT_00575948`; the page tail prints
  the stellar-tribute/asset totals (`uVar10 - iVar16`) and the
  "press <key> for key settings" hint (STR# 0x7d2 0x10b).
- **Page 2 — Cargo.** If any cargo/junk/mission cargo: draw the prebuilt
  cargo summary text inverted-filled at the view rect top-left; else STR#
  0x7d2 0x10c/0x10d ("Your cargo hold is empty" variants by
  mass < capacity) at `view_y + 0x37`.
- **Page 3 — Extras.** If any owned non-0x2000 outfit with a name: draw the
  prebuilt outfit text; else STR# 0x7d2 0x10e.
- **Page 4 — Honors.** If any rank badge (both active bytes set with a
  name string at rank+0x1e) or any owned 0x2000-flag outfit: draw the prebuilt
  honors text; else STR# 0x7d2 0x10f.

Tail: `NovaUi_DrawPlayerSpecialInteractionTabs(g_player_special_page, -1)`,
blit the window surface to the gameplay context, restore.

## Text builders — NovaUi_BuildPlayerSpecialInteractionStrings (0x0049c050)

All wording comes from STR# 0x7d2 (misc strings) / 0x89 (number words);
counts pluralise through the "ton"/"tons" pool `DAT_0072d3cc/.4cc` and
`Ship_FormatLocalizedCountWord`. Key bindings inside the first sentence are
translated through `NovaCommand_TranslateByInputMap`.

- **Cargo text** (0x7d5278): header STR# 0x16a "Other cargo:" + mass check
  (`Ship_ComputeShipTotalCargoCapacity` vs `Player_ComputeFleetCargoCapacity` → 0x16e
  "free space"/0x16d) + ":": one entry per commodity (0x100-stride name
  table `DAT_0069d2cc`, `*` marks hidden) with summed bins — the 6 player
  cargo bins plus every active mission's cargo (mission cargo type/quantity
  from the 0x14-stride active-mission runtime block) — then the junk
  entries (0x526-stride `g_junk_defs`, count at +0x22, name at +0x228) and
  the "and"/"a"/"of" list glue (0x187/0x188), terminated ".";
  finally the free-space footer via `Player_ComputeCargoAndJunkTotal`
  and `Player_ComputeRemainingCargoSpace` (0x16c/0x16e/0x171, "full" note
  STR# 0x150).
- **Extras text** (0x7d6278): header STR# 0x110 + "\r\r"; owned outfits
  grouped by `similar_to` (def +(-10) chain, self when invalid), ordered by
  cost descending; each row is a count word ("a/an" 0x189/0x18a via the
  first letter vowel test `MWRuntime_FUN_004d6230`, "two"/"three" STR# 0x89
  0x1e/0x1f, digits above) + name (singular/plural pstrings at
  def +(-0x10) family), comma/"and" glue; footer with
  `Ship_ComputeTradeInValue` (0x00469100; 25% of ship base cost + 50% of
  non-persistent owned outfits) credits (STR# 0x111 + grouped quantity +
  credits word).
- **Honors text** (0x7d7278): header STR# 0x112 + "\r\r" (built only when
  something exists); the rank badges (active bytes set, name at rank+0x1e,
  ordered by the u16 Weight at rank+0x04, smallest first, each printed once)
  followed by the owned 0x2000-flag ("Ranks section") outfits with the same
  count-word machinery.

## Port status

The combat ladder has a distinct positive-score first rank: 0 points uses
rank index 0, 1..99 uses index 1, then 100/200/400/.../25600 advance through
indices 2..10. Legal status is derived rather than stored separately: the
current system's saved reputation is graded against its government's CrimeTol
by the shared 0x00468d90 ladder after the usable-destination gate.

Ported in `src/game/player_info_window.cpp` (see module comments; the
`NovaUi_*` addresses above map to `NovaPlayerInfo_*`). Divergences are
marked with `TODO(decomp)`/`NovaLog::Todo` at the port sites and tracked in
`decomp-progress.tsv` rows 0x00499c10 / 0x0049a540 / 0x0049c050 /
0x004a1ae0 / 0x004a1c40.

The Cargo-page Jettison execution (`Player_RedistributeFleetCargoOverflow`
0x0041f330) is ported in `src/game/outfit.cpp`. The window returns
`PlayerInfoWindowResult::jettison_confirmed`; the flight loop applies it with
the sim clock in scope (the original runs it inside the window loop). The
in-flight cargo-dump channel (`Player_RedistributeFleetCargoOverflow` from
`0x0044aa70` block 0x00451907) is wired to the arm-modifier + slot 0x0f
binding. The visible jettisoned-cargo pods are spawned through the
`FreeflightObjectState` pool (`src/game/freeflight_objects.cpp`,
`Ship_SpawnFreeflightObjectForShip` 0x0041f800).
