# Trade Center (commodity exchange)

Ghidra driver: `NovaUi_RunTradeCenterWindow` **0x0048c730**, dispatched from the Spaceport
loop `0x00491f30` action `7` when `Stellar.travel_flags & 0x2`.

Supporting functions: input `0x0048d190` `NovaUi_HandleTradeCenterInput`,
redraw `0x0048d6f0` `NovaUi_RedrawTradeCenterWindow`, selection cycler
`0x0048d5a0` `NovaUi_TradeCenterCycleSelection`, button draw/hit-test
`0x004a06a0`/`0x004a04d0`, buy/sell gates `0x00465e50`
`TradeCenter_CanBuySelectedRow` / `0x00465ea0` `TradeCenter_HasSelectedRowStock`.

## Price table (8 rows)
- Rows 0..5: `STR# 0xfa4` base prices `{75, 350, 750, 900, 200, 550}` scaled by
  the per-stellar lane `Stellar_ExtractTravelFlagConnectiveValue` (0x00469d30):
  `1` = `round(base/scale)` (Low), `2` = base (Med), `4` = `round(base*scale)`
  (High), `0` = not traded (price 0, row disabled). Prices floor at 5.
- `scale` = 1.25; 1.1 when `government_id != -1` and
  `system_reputation[stellar.system_id] < 0`; 1.5 when `dominated`.
- Active `öops` (`g_disaster_defs`) bound to the landed stellar override
  `price = base + delta` (floor 5). Badge shows Higher/Lower.
- Rows 6/7: first `jünk` def whose **BoughtAt** contains the stellar and
  **BuyOn** passes (row 6, `base*scale`), and first whose **SoldAt** contains it
  and **SellOn** passes (row 7, `base/scale`). Junk prices are not floored.

Buy and Sell both use the single local price (no spread). Plain action moves
`min(10, affordable/carriable)`; shift opens `NovaUi_RunStoreQuantityPrompt`.

## Port
- `src/game/trade_center.{hpp,cpp}`: `JunkDef` decode lives in
  `scenario_data.{hpp,cpp}`; `TradeCenter_BasePrices`, `Stellar_TradeConnective`,
  `NovaTradeCenter_OpenSession`, `NovaTradeCenter_Buy/Sell` and the row gates.
- `src/game/docked_trade_dialog.cpp`: `LayoutTradeCenter` loads the DITL 0x3e9
  rects at runtime (frame from the DLOG bounds), `DrawTradeCenterScreen` +
  `RunTradeCenterDialog`, dispatched from `NovaLanded_RunSubWindowDialog`.
- Row colours come from the `c\x9alr` list palette (`NovaMainMenuStyle` in
  `brgr_archive.{hpp,cpp}`), matching the original's `DAT_0073566a`
  `list_background` / `DAT_00735670` `list_hilite` (shipped: black / dark red
  0x800000) / `DAT_00735664` `list_text`. The 0x733b5c grid lines and 0x733b50
  header text come from `Settings_InitColors` (0x004ad7c0).
- Button captions are the shared three-state label table entries the original
  indexes: STR# 0x96 entries 5/2/3 = `Done` / `Buy` / `Sell`
  (`NovaUi_InitThreeStateButtonArt` 0x004a2f50 + `DAT_007d82e8 = {4,1,2}`).
- All game strings are MacRoman; `NovaText_MacRomanToUtf8` (nova_font.cpp)
  converts at the font boundary so resource names like `Xtreem\xaa
  Rocket-Boards` (`0xAA` -> U+2122) render correctly.

## Mission interactions
- Entering the Trade Center switches the mission-offer context to `AvailLoc =
  4`, runs that offer pass immediately, and consumes the recheck timer. This
  is how the tutorial's trade follow-up is presented. Its offer dialog
  re-renders the live Trade Center beneath itself rather than dropping back to
  the Spaceport.

## Gaps
- Nested Starmap / Player Info / Mission Computer actions are `TODO(decomp)`.
- Footer `Other cargo:` mission-cargo clause is simplified to the free-space
  line; the original summary's filled-rect + `InvertRect` composition and
  word-wrap are not reproduced.
- Buttons are drawn/hit-tested inline rather than as standalone helpers; the
  three-state renderer's `^`/`&` vector-icon captions (used elsewhere) are not
  drawn, and the trade strip never draws the hover art (the redraw always
  passes -1, matching the original).
