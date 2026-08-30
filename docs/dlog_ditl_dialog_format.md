# DLOG / DITL dialog resources (EV Nova's window format)

EV Nova builds **every in-game window** from two paired classic-Macintosh dialog
resources kept in the `Nova.rez` UI archive (the "DLOG"/"DITL" `FourCC`s,
`0x444c4f47` / `0x4449544c`):

* a **DLOG** declares a dialog: its window bounds and a link to a DITL by id;
* a **DITL** (Dialog ITem List) holds the individual ui controls (buttons, text,
  picture boxes, user items) and *their rects*.

Getting the coordinate model right is the part that repeatedly confuses readers
(after all, the ship-comm window's DITL items initially *look* like they fall
outside the window bounds). This document nails the file layouts, the parsing
trajectory, and — most importantly — how item rects map to screen space, so you
can read a DITL directly instead of guessing.

Reference implementation:
* C++ parser: `NovaResource_LoadDialogDefinition` / `NovaResource_LoadDialogItems`
  in `src/brgr_archive.cpp`.
* Original Ghidra: `UiWindow_CreateFromDialogResource` (`0x004cf760`),
  `Dialog_CreateFromDlog` (`0x008730a1`), `Dialog_ParseItemList` (`0x004cef50`),
  `UiPanel_GetEntryInfo`.
* Quick byte dump: `python3 tools/rez_extract.py dlg` (the `dlg` subcommand).

See also `docs/scenario_data_loading.md` for the wider resource-map (BRGR)
container format these DLOG/DITL resources ship inside.

---

## 1. DLOG (dialog declaration)

Big-endian shorts, all in the dialog's *authoring* coordinate space:

| offset | field             | notes                                        |
|--------|-------------------|----------------------------------------------|
| 0x00   | top               | DLOG bounds                                  |
| 0x02   | left              |                                              |
| 0x04   | bottom            |                                              |
| 0x06   | right             |                                              |
| 0x08   | procID / flags    | not needed by the recompilation              |
| 0x12   | **DITL id** (u16) | the DITL this dialog parses and lays out     |

The **window size is `right-left` × `bottom-top`**. For example
`DLOG 0x03ef` gives `423×215`, which is *exactly* the decoded size of its frame
PICT `0x213f` — the DLOG bounds and the backdrop PICT are the same box.

> Note: `tools/rez_extract.py dlg` prints DLOG `bounds=(top,left..bottom,right)`
> directly — ignore the older `-291x423` style line, that was from a stale parser.

## 2. Window centering (`Dialog_CreateFromDlog`)

The dialog is **centred on the current logical playfield** (640×480). The window
top-left is

```
left = (g_display_width  - (right - left)  width ) / 2
top  = (g_display_height - (bottom - top)  height) / 2
```

(`_g_ui_scale` multiplies the width/height measures first; the shipped build
uses 1.0.) So a 423×215 window sits at `(108, 132)` on the 640×480 playfield.

## 3. DITL (dialog item list)

### 3.1 Rect field order — the confusing part

Each item's four rect shorts are ordered

```
(top, left, bottom, right)
```

i.e. **`top` is the y-origin, `left` is the x-origin** — the standard Macintosh
`Rect`. When you read a rect, pair them as:

```
x = left .. right      (does NOT cross pair boundaries)
y = top  .. bottom
```

`(top,left)` is the top-left corner; `(bottom,right)` is the bottom-right corner
(bottom > top and right > left for a valid box).

> The reason the ship-comm DITL initially *looks* wrong is that a DLOG item
> rect is **not required to lie inside the DLOG window bounds**. The DLOG bounds
> only size/clip the *backdrop*; items can extend past it and are still drawn
> (over the scrim). See §6.

### 3.2 Item payload

| field        | size | notes                                        |
|--------------|------|----------------------------------------------|
| count (u16)  | 2    | big-endian; the walker processes `count + 1` items (the trailing zero item is a terminator) |
| per item     |      |                                              |
| rect         | 8    | `(top, left, bottom, right)` at item+4..+11 (`top`=item+4, `left`=+6, `bottom`=+8, `right`=+10) |
| type byte    | 1    | item `type` = low 7 bits; bit 7 = enabled/hilite flag (item+12) |
| variable tail| var  | skipped per type, see below                |

The item header is 14 bytes (rect + type). The parser starts the first item right
after the 2-byte count, so the first rect's `top` is at payload offset 6.

### 3.3 Tail / advance rule (`Dialog_ParseItemList`)

After each item's 14-byte header, skip its variable tail so the walker lands on
the next item (always re-aligned to an even offset):

| item type         | tail starting at item+13                         |
|-------------------|--------------------------------------------------|
| string types `4/5/6/8/0x10` | a Pascal string: the length byte at item+13, then that many chars |
| control `7/0x20/0x40` | 2 bytes: `[subtype u8][refcon BE u16]` at item+13..+15 (the refcon names a MENU / PICT resource) |
| everything else (buttons/plain) | nothing — item+13 is a single pad byte |

The even re-alignment is the gotcha that mis-aligns every subsequent rect if you
drop it. (An older revision of this table claimed controls skip 16 tail bytes
and plain items 14; that came from a stale parser — the validated rule above
walks DITL 0xc1d/0xc1e/0xbb9 to an exact end-of-payload fit and agrees with
`NovaResource_LoadDialogItems`. `tools/rez_extract.py dlg` applies the same
arithmetic.)

---

## 4. Items are addressed by **ordinal**, not geometry

Dialog code addresses controls by their **1-based index into the DITL**, not by
their rect. `UiPanel_GetEntryInfo(window, entry, ...)` returns the rect for list
entry `1..N` (i.e. DITL item `0..N-1`).

Example — the ship-comm window (`NovaUi_HandleTravelDestinationContextButtons`,
`0x004a09e0`):

```c
UiPanel_GetEntryInfo(window, 1, ..., &rect0);  // DITL item 0 -> Close Channel
UiPanel_GetEntryInfo(window, 2, ..., &rect1);  // DITL item 1 -> Request Assistance
UiPanel_GetEntryInfo(window, 3, ..., &rect2);  // DITL item 2 -> Greetings
```

The "->" role is the slot's *action*, and it matches the label drawn on that
slot: item 1 (middle) is the assistance button (Request Assistance | Beg For
Mercy | Release) which runs the assistance dialogue; item 2 (top) is the
Greetings button which shows the hail-info text (DAT_007d190c) or a refusal
prompt. (The Ghidra plate comments on NovaUi_PollTargetShipCommWindow call
action 2 "Greetings" and action 3 "secondary", which is swapped relative to
the buttons and is the historical source of confusion about this window.)

So the button layout of a dialog is whatever rects DITL items 0,1,2 (usually)
happen to carry — if the designer placed them vertically, the buttons are
vertical.

---

## 5. The in-flight dialog family (a worked reference)

The four travel/interaction dialogs share one house style: **buttons stacked
vertically on the lower-left, big picture box on the right, text up the left**.
All values are window-local (0,0 = window top-left, matching the backdrop).

| DLOG | window | DITL items (x=left..right, y=top..bottom) |
|------|--------|---------------------------------------------|
| `0x03ef` ship-comm (PICT `0x213f`) | 423×215 | btn0 Close Channel `(21,181)-(187,207)` (bottom), btn1 Request Assistance / Beg For Mercy / Release `(21,153)-(187,179)` (middle), btn2 Greetings `(21,125)-(187,151)` (top); name `(11,8)-(203,66)`; status `(40,73)-(174,119)`; portrait `(216,7)-(416,207)`. The three button labels come from the STR# 0x96 indices DAT_007d82ee (0x14 Close Channel) / f0 (0x16 Request Assistance, 0x18 Beg For Mercy, 0x1f Release) / f2 (0x15 Greetings) in NovaUi_DrawTravelDestinationContextButtons; each label matches its slot's action (slot 1 runs the assistance dialogue, slot 2 shows the hail-info text). |
| `0x03f0` payment/bribe (PICT `0x2142`) | 262×107 | btn0 `(58,74)-(204,100)`, btn1 `(58,39)-(204,65)`, head `(7,6)-(255,31)` |
| `0x03f1` destination/negotiation (PICT `0x2140`) | 540×295 | btn0 Leave `(27,244)-(173,270)`, btn1 Land/Bribe `(27,184)-(173,210)`, btn2 Attack `(27,214)-(173,240)`; text `(5,5)-(205,65)`; target picture `(222,5)-(532,288)` |
| `0x03e8` spaceport (PICT `0x2134`) | 618×517 | large body `(3,3)-(615,288)`; content panel `(160,327)-(461,512)`; eight 145×25 buttons down both sides + bottom bars |

The 200×200 portrait PICTs (`0x1388..`, i.e. 5000 + zero-based ship-class id,
`ShipClass.pict_fallback_sprite_resource_id`) slot straight into the ship-comm
`portrait` rect — a strong sanity check that the coordinate model is right.

## 6. Items outside the DLOG bounds are legal (and common)

The ship-comm DITL `0x03ef` actually has **12 items**; its extras (3..8) sit at
`y` past the 215-tall backdrop (up to `y=360`) and would want a taller window:

```
[3] x46..246 y241..266    [4] x7..207 y320..345   [5] x199..399 y335..360
[6] x178..378 y261..286   [7] x178..378 y289..314 [8] x34..146 y299..315
```

These are drawn below the clipped backdrop and are vestigial / for an alternate
taller presentation, but item 3 has one real use: for **special-scan-mask
governments** (target govt `scan_mask & 1`) `NovaUi_HandleTravelDestinationContextButtons`
and `NovaUi_DrawTravelDestinationContextButtons` fetch the button rects as
`UiPanel_GetEntryInfo(window, 1/4/2, ...)` instead of `1/2/3`. That again maps
to DITL items 0 / 3 / 1 — i.e. slot 0 = Close Channel (item 0, bottom y=181),
slot 1 = the assistance button (item 3, *offscreen* y=241..266 and not drawn),
slot 2 = Greetings (item 1, the middle y=153). The net effect is that the
offscreen/unknown assistance button is hidden and the Greetings button drops
from the top (item 2, y=125) to the middle (item 1, y=153), leaving the top
empty. The shipped 423×215 window only shows DITL items 0/1/2/9/10/11 for
normal govts. **Don't assume every DITL item must fit the DLOG bounds.** The
robust way to find the "real" window content is:

1. Decide the window size from the **DLOG bounds / backdrop PICT** (they agree).
2. Look at which item rects fall inside that box; those are the visible controls.
3. Address them by **ordinal**, matching `UiPanel_GetEntryInfo` call sites in the
   Ghidra decomp of the dialog's poll/handle functions.

---

## 7. How the recompilation consumes this

`src/game/ship_comm_dialog.cpp` hard-codes the DITL `0x03ef` rects as layout
constants rather than re-parsing the DITL each frame (fast, cheap, deterministic):

```cpp
constexpr int kCommFrameWidth = 423;                 // DLOG 0x3ef = PICT 0x213f
constexpr float kCommWindowX = (640 - kCommFrameWidth) / 2.0F;  // centring
constexpr SDL_FRect kCommPictureRect{216, 7, 200, 200};          // DITL item 10
// ... buttons stacked at kCommButtonYClose=181 / YMiddle=153 / YTop=125
```

The **destination-interaction (negotiation) window** gets the same treatment in
`src/game/negotiation_dialog.cpp`: DLOG 0x3f1 / PICT 0x2140 is a fixed 540x295
frame centred on the playfield, with the three primary buttons stacked
vertically down the lower-left column (DITL items 0/1/2 -> Leave bottom,
Attack middle, Land/Bribe top), the destination planet PICT in the item-4 image
frame on the right, the status/prompt text in the item-3 panel, and the stellar
header name in the item-5 block. The rects were cross-checked against the
`NovaResource_LoadDialogItems` parser output for DITL 0x3f1 (items:
`{146x26 buttons at x=27..173 y=244/184/214}`, `text 5,5..205,65`,
`image 222,5..532,288`, `header 16,82..136,132`).

**Current policy: load DITLs at runtime.** The hard-coded rects above were the
setup the recompilation started with; the going-forward approach is to consume
the DLOG/DITL resources exactly like the game does. `src/game/ui_dialog.cpp`
implements the SDL-backed port of the original's dialog runtime
(`UiWindow_CreateFromDialogResource` 0x004cf760, `UiWindow_Draw` 0x004d0d00,
`UiWindow_RunInteractionLoop` 0x004cfdd0 and the `UiPanel_*`/`UiControl_*`
accessors): it loads a DLOG + DITL through `NovaResource_LoadDialogDefinition` /
`NovaResource_LoadDialogItems`, centres the window per `Dialog_CreateFromDlog`,
draws the controls from the parsed item rects, and reports interactions as the
1-based ordinal of the activated control (that is what the `local_130` codes in
every `UiWindow_RunInteractionLoop` call site are — OK = its ordinal, Cancel =
its ordinal, a checkbox = its ordinal). Game-specific dialog ports (e.g. the
new-pilot dialog, `src/game/new_pilot_flow.cpp`) should be written against this
layer near-verbatim, not against hard-coded geometry. The hard-coded layouts
in `preferences.cpp` / `ship_comm_dialog.cpp` / `negotiation_dialog.cpp` are
legacy and migrate to `ui_dialog` opportunistically.

One extra resource linkage the parser must surface: a **type-7 item's tail
carries a MENU resource id in its first tail short**, and the engine turns that
item into a popup filled from that MENU (e.g. DITL 0xc1d item 10 → MENU 0x1f4
"Gender", item 12 → MENU 0x1f5 "Character", whose entries are filled at
runtime from the pilot/châr family). `NovaResource_LoadDialogItems` now exposes
this as `NovaDialogItem::menu_resource_id`, and `NovaResource_LoadMenuDefinition`
parses the MENU payload (title + entries).

> Type-7 vs type-0x80: the generic control drawer `UiWindow_Draw` handles
> popups natively only for type 0x80; type-7 items are skipped by the generic
> drawer and handled by the stock dialog callback installed via
> `UiWindow_SetDrawCallback(window, 1)`. Both carry a list of Pascal entries
> addressed through `UiPanel_GetEntryTextPascalIndexed`.

## 8. Quick recipe to read an unknown DLOG/DITL

1. `python3 tools/rez_extract.py dlg` — list all DLOGs/DITLs and sizes.
2. Match the window size to its backdrop PICT (decode via `src/pict_image.cpp`
   `Resource_LoadPictAsImage`).
3. For each item, decode `(top, left, bottom, right)` → `x=left..right`,
   `y=top..bottom`.
4. Find the items that fit the window box — those are the visible controls; line
   them up by ordinal with the `UiPanel_GetEntryInfo(...)` entries used in the
   dialog's input handler in Ghidra.
5. Cross-check by overlaying the item boxes on the backdrop (as was done to
   verify the ship-comm layout).

## 9. Worked example: the new-pilot dialogs (0xc1d / 0xc1e / 0xbb9)

The new-game pilot dialogs are fully decoded; use them as the reference for how
DITL ordinals map to `UiPanel_*` rows and how popups get their content.

### DLOG 0xc1d (multi-entry census) and 0xc1e (single) — both -> DITL 0xc1d/0xc1e

Same 14-item template, two window sizes: 0xc1d is 326x247, 0xc1e is 326x213.
Rows below are the 1-based ordinals used by
`Menu_RunPilotSelectionDialog` (0x0048a7e0) — row N = DITL item N-1:

| Row | Item | Control | Role |
|-----|------|---------|------|
| 1 | 0 | button `OK` (70x20, bottom-right) | activation code 1 |
| 2 | 1 | button `Cancel` (70x20, bottom-left) | activation code 2 |
| 3 | 2 | type-0x40 image 182x22 | decorative plate |
| 4 | 3 | checkbox `Strict Play` | toggles the new-pilot flag (code 4); persisted to DAT_00596d2f by the flow |
| 5 | 4 | static `Full Name:` | label |
| 6 | 5 | static `Nickname:` | label |
| 7 | 6 | static `name3:` (y=357, offscreen both variants) | vestigial third field |
| 8 | 7 | edit text 170x16 | **Full Name**; prefilled from STR# 0x80 rows 1-3 (random), copied back to DAT_007d20b7; the `.plt` file is named `<Full Name>.plt` |
| 9 | 8 | edit text 170x16 | **Nickname**; prefilled from STR# 0x80 rows 4-6, copied back to DAT_007d21b7 |
| 10 | 9 | edit text (offscreen) | vestigial |
| 11 | 10 | type-7 popup 200x20 -> **MENU 0x1f4 `Gender`** (Male/Female) | selection -> DAT_007d23b7; the flow lowercases its first char and compares to 'm' (0x6d) to latch DAT_00734c1c (male) |
| 12 | 11 | static `Create a new pilot:` | title |
| 13 | 12 | type-7 popup 271x20 -> **MENU 0x1f5 `Character`** (entries filled at runtime from the 0x63688a72 pilot/châr family) | selection -> DAT_007d22b7 = character-template key for `PilotData_InitializePlayerState`; only inside 0xc1d's window (0xc1e pushes it to y=277, offscreen) |
| 14 | 13 | type-0x40 image 32x32 (top-left) | pilot icon |

The variant pick: `ResourceData_CountEntries(0x63688a72)` minus hidden entries
(name starts with `.`) — stock Nova only ships the hidden `.Trader` châr, so
the census is 0 and **stock always shows 0xc1e** (no Character popup). Mods
adding visible châr entries get 0xc1d. Preselection scans for the entry marked
active/default (`PilotData_FindActivePilotName`, bit 0 of the flags at block
+0x132) and `UiControl_SetValue`s the popup to its index.

### DLOG 0xbb9 — the shared text-entry modal

`NovaUi_ShowTextConfirmCodeDialog` (0x00497900) opens DLOG 0xbb9, sets row 3 to
the prompt Pascal string and row 5 to the initial edit text (select-all), then
loops on activation codes: **1 = OK** (accept if the text is <= the max-length
arg, else re-select the field and keep the loop open), **6 = Cancel**. The final
row-5 text lands in DAT_007d4c0e. The new-game flow uses it as the ship
christening box: prompt = STR# 0x7d2 row 0x79 ("Now, please christen your
brand-new") + ` ` + `<ship class long name>` + `: ` (class name from the
DAT_005a9bcc 0x100-stride Pascal table, indexed by the châr's ShipType),
initial text = a random STR# 0x80 row 7-9 ship name ('Ring of Glory', 'Snowy
Owl', 'Cardinal Virtue'), max 0x40 chars. The result, article-stripped, becomes
the ship name (DAT_00599acc, the .plt trailer string).
