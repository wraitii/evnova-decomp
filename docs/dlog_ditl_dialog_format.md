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

| item type         | tail                                             |
|-------------------|--------------------------------------------------|
| string types `4/5/6/8/0x10` | a Pascal string title starting at item+13: skip its length byte + that many chars |
| icon/pict/control `7/0x20/0x40` | skip 8 extra ushorts (16 bytes)       |
| everything else (buttons/plain) | skip 7 extra ushorts (14 bytes)      |

The even re-alignment is the gotcha that mis-aligns every subsequent rect if you
drop it. The `dlg` subcommand of `rez_extract.py` applies the same arithmetic.

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

If a future pass needs to render *any* dialog from its resource (rather than a
hard-coded one), the natural path is to parse the DITL once into `NovaDialogItem`
rects (already exposed by `brgr_archive.cpp`), offset each by the centred window
origin, and draw the controls by ordinal.

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
