# In-space comm dialogs (ship hail + stellar destination interaction)

Ground-truth notes for the two target-action comm modals rendered over the
live flight scene. Both were re-aligned against the Ghidra decompile after
playtesting showed divergences (wrong picture, wrong panel colours, missing
text wrapping); this file records the draw-path facts so future changes don't
have to re-derive them from the decompiler.

- Stellar window: `src/game/negotiation_dialog.cpp` /
  `NovaNegotiation_RunDestinationDialog` (runner `0x00480030`
  NovaUi_RunTravelDestinationInteractionWindow, draw `0x004812c0`).
- Ship window: `src/game/ship_comm_dialog.cpp` / `NovaShipComm_RunShipDialog`
  (runner `0x0047e470` NovaUi_RunTargetShipCommWindow).

## The ship-comm draw function

`NovaUi_DrawTargetShipCommWindow` (**0x0047fb70**) draws the ship-comm
window, not the DLOG 0x3f0 payment window. It is the shared **ship-comm
window renderer** (backdrop PICT 0x213f, context buttons, ship portrait,
info block, prompt panel), called from
`NovaUi_RunTargetShipCommWindow 0x0047e470` on every poll tick and re-invoked
after each comm state change. The actual payment modal draw is `NovaUi_DrawTravelDestinationPaymentWindow`
(0x004826a0). Keep citations pointing at 0x0047fb70 for the ship-comm draw.

## DITL entry <-> item mapping (UiPanel_GetEntryInfo)

`UiPanel_GetEntryInfo(window, entry, ...)` takes a **1-based entry number**:
`entry = DITL item index + 1`. Observed in both windows:

| window      | entry | DITL item | role |
|-------------|-------|-----------|------|
| 0x3ef ship  | 0xb (11) | 10 | 200x200 ship portrait |
| 0x3ef ship  | 0xc (12) | 11 | Class:/comm-name/Status: block (black fill) |
| 0x3ef ship  | 10       | 9  | comm prompt panel (fill + InvertRect) |
| 0x3f1 stell | 4        | 3  | status/prompt panel (fill + InvertRect) |
| 0x3f1 stell | 5        | 4  | stellar ambient sprite thumbnail |
| 0x3f1 stell | 6        | 5  | name / description / Status: header |

## Text panel pattern: fill + InFilledRect + InvertRect

Both prompt panels (ship item 9, stellar item 3) are drawn with the same
three-step sequence:

1. `Rect_Inset(rect, 1, 1)`, fill with the *current* RGB colour (ship window:
   white PTR_DAT_00575ad8; stellar window: same white after 0x004812c0's
   SetRgbColor — both pre-invert fills are the **text** colour, black).
2. Inner rect `Rect_Inset(..., 4, 2)`,
   `DrawContext_DrawPascalStringInFilledRect` (0x004bcd30): fills the inner
   rect with the context **fill colour** (white, the DrawContext default from
   DrawContext_AllocateSurface 0x004bb870) and draws the text in the current
   colour, **word-wrapped** (the Windows path is `DrawTextW` with
   `DT_WORDBREAK | DT_NOPREFIX`, clipped to the rect).
3. `DrawContext_InvertRect(inset-1 rect)`.

White fill + black text cancel under the inversion, so the **net look is a
black panel with white left-aligned wrapped text** starting one font-height
below the inner top (baseline ~top+12, ~13px line step). The port draws that
net result directly; there is no separate invert step in SDL.

## Fonts and colours (shared UI globals)

- `DAT_00735684` / `DAT_00735686`: the shared UI font id + point size — the
  12-point screen font the rest of the port maps to Geneva 12 regular. Every
  text run in both windows (info block, prompt panels, headers) uses it; no
  bold except the "Hostile" word (weight flag via `FUN_004b6940(1)` on
  DrawContext +0x4a).
- `PTR_DAT_00575acc` → RGBColor at 0x0085f6e8 (.bss, runtime-initialised):
  the "space background" colour, black in practice; the text colour inside
  the inverted panels and the fill of the ship info block.
- `PTR_DAT_00575ad8`: white (65535,65535,65535). `PTR_DAT_00575ae4`: red.
- `SHORT_ARRAY_00733b56`: 50% grey (labels). `SHORT_ARRAY_00733b32`: the
  warm orange used by the Escort/Status words (unreadable .bss; approximated).

## Stellar window picture: the ambient system sprite (not the dock PICT)

`0x004812c0` scans `g_stellar_ambient_sprites` for the entry whose +200
(stellar id) matches the secondary target, then blits the sprite's **current
animation frame** centred in the item-4 entry rect at **native size** (the
rect is collapsed to the entry centre and expanded by
`Sprite_GetFrameFullHeight` / `Sprite_GetFrameFullWidth`; clipping is to
the window surface, so oversized sprites overflow the entry rect).

The port resolves the same frame via `SpaceflightView::sprite_store().Spin(`
`link_a_id + 1000)` + `SpaceflightView::StellarCurrentFrame(stellar_id)`.
The docked-screen PICT (`CustPicID >= 0x80` else
`link_a_id + 0x2710`) is only a logged fallback for missing spin sets — it is
NOT what the original shows here (that PICT is the *docked/landing* screen
art).

## Ship window item-11 info block contents (0x0047fb70)

Black-filled rect, three left-aligned Geneva-12 lines:

- y+12: grey `Class:` (STR# 0x7d2 **0xc3**) label; white value at x+0x23:
  the ship class name (DAT_006bd2cc class-name table), or the literal
  "Ambrosia Mascot" (DAT_0056cc30) when `pers_def_slot == 0x3ff`.
- y+0x19: white `(<govt comm name>)` at x+0x23 — the DAT_0056cc40/44 `(`/`)`
  pstrings around the government comm-name table entry (DAT_007d1d0c);
  skipped for faction-less ships (empty pstring DAT_0056cc2c) and pers-0x3ff.
- y+0x28: grey `Status:` (STR# 0x7d2 **0xc4**), then either
  - keep-pressing (`Ship_ShouldShipKeepPressingTarget`): "  " (DAT_0056cc48)
    + red "Hostile" (0xae) continuing after the label, or
  - no AI target && escort-release latch (DAT_007d17f5) clear: orange
    "Escort" (STR# 0x7d2 **0xa8**) / "Hired Escort" (**0xa6**, set when
    ShipState +0xbb — the port's `escort_origin_mark` — is non-zero),
    at x+0x23; otherwise the line is omitted.

Note the earlier port draft drew STR# 0x7d2 0xa9 "Fighter" / 0xa7 "Captured
Escort" here — those entries belong to other windows; the comm window uses
0xa8/0xa6.

## Ship portrait PICT selection (0x0047e470)

`g_ship_class_defs[class].pict_fallback_sprite_resource_id`, overridden by
the pers personality's HailPict (`g_pers_defs[slot] + 0x12`) when it is a
real PICT id (> 0x7f). Loaded once per window open into
`g_escort_management_ship_image` and blitted into the item-10 rect.

## Related resource ids

- Backdrop PICTs: ship 0x213f (Communications frame), stellar 0x2140,
  payment 0x2142. DLOGs 0x3ef / 0x3f1 / 0x3f0.
- Button labels: STR# 0x96 (see NovaUi_DrawTravelDestinationContextButtons
  0x004a0bc0 / PrimaryButtons 0x004a0f90 plate comments for the slot table).
- Comm prompt/status pools: STR# 0xbb8 / 0xbb9 (ship prompts,
  `index*5+random+1`), STR# 0xbba (stellar status). These pools are plain
  text — no crln (0xd9) expansion is involved in either window; the wraps
  come purely from DT_WORDBREAK at the panel width.
