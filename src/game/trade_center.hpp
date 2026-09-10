#pragma once

#include "game_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace game {

// The commodity exchange (Trade Center) is one of the Spaceport services.
// Ghidra's driver is NovaUi_RunTradeCenterWindow (0x0048c730), with the input
// handler 0x0048d190, the redraw 0x0048d6f0 and the selection cycler
// 0x0048d5a0. This module is the SDL-free data/transaction layer those UI
// routines consume.

// Six "regular" commodities (Food, Industrial, Medical, Luxury, Metal,
// Equipment) plus two per-stellar j\x9fnk slots, for eight display rows.
constexpr std::size_t kTradeCenterCommodityCount = 6;
constexpr std::size_t kTradeCenterRowCount = 8;

// Bible base prices, loaded at startup from STR# 0xfa4 entries 1..6
// (DAT_005979b2). The shipped scenario values are the fallback when the
// resource pool is unavailable.
[[nodiscard]] std::array<std::int16_t, kTradeCenterCommodityCount>
TradeCenter_BasePrices();

// The per-stellar commodity trade lane from Stellar_ExtractTravelFlagConnective
// Value (0x00469d30): 0 = not traded, 1 = low (base/scale), 2 = flat, 4 = high
// (base*scale). `commodity` is 0..5.
[[nodiscard]] std::int16_t Stellar_TradeConnective(const Stellar &stellar,
                                                   std::size_t commodity);

// One built trade-center row (index 0..5 commodity, 6/7 junk).
struct TradeCenterRow {
  // DAT_005979c6[row]; 0 disables the row (not traded / no junk here).
  std::int16_t price = 0;
  // DAT_005979a6[row]: commodity connective 0/1/2/4; junk rows mirror their
  // trend (row 6 high, row 7 low) for the Low/Med/High badge.
  std::int16_t trend = 0;
  // 0-based g_junk_defs index for rows 6/7 (DAT_007d4bb2/bb4), else -1.
  std::int16_t junk_id = -1;
  // Active öops price-shock badge for the row: only meaningful when
  // `has_disaster`; `disaster_raised` is the sign of the applied delta.
  bool has_disaster = false;
  bool disaster_raised = false;
};

// A price snapshot for one landed stellar, mirroring the prologue of
// NovaUi_RunTradeCenterWindow (0x0048c730).
struct TradeCenterSession {
  std::int16_t stellar_id = -1; // raw 0x80-based resource id
  std::array<TradeCenterRow, kTradeCenterRowCount> rows{};
  // Selected row index 0..7 (_DAT_005979a4).
  std::int16_t selected = 0;

  [[nodiscard]] bool Valid() const { return stellar_id >= 0; }

  [[nodiscard]] const TradeCenterRow *Row(int index) const;
};

[[nodiscard]] TradeCenterSession
NovaTradeCenter_OpenSession(const GameState &state, std::int16_t stellar_id);

// Row label. Rows 0..5 are STR# 0xfa0 entries 1..6 (All Cargo); rows 6/7 use
// the junk def's display name. Empty when the row has no source.
[[nodiscard]] std::string NovaTradeCenter_RowName(
    const GameState &state, const TradeCenterSession &session, int row);

// Held quantity for a row (cargo bin for 0..5, junk count for 6/7).
[[nodiscard]] std::int16_t NovaTradeCenter_HeldCount(
    const GameState &state, const TradeCenterSession &session, int row);

// Triton buy/sell gates (Ghidra TradeCenter_CanBuySelectedRow 0x00465e50 and
// TradeCenter_HasSelectedRowStock 0x00465ea0).
[[nodiscard]] bool NovaTradeCenter_CanBuyRow(const GameState &state,
                                             const TradeCenterSession &session,
                                             int row);
[[nodiscard]] bool NovaTradeCenter_HasRowStock(
    const GameState &state, const TradeCenterSession &session, int row);

// Transaction maxima used by the shift-quantity prompt.
[[nodiscard]] std::int16_t NovaTradeCenter_BuyMax(
    const GameState &state, const TradeCenterSession &session, int row);
[[nodiscard]] std::int16_t NovaTradeCenter_SellMax(
    const GameState &state, const TradeCenterSession &session, int row);

// Applies a transaction, mirroring the 0xd/0xe action arms (credits + cargo /
// junk counts). `quantity` is clamped to what is held/affordable/carriable.
// Returns the quantity actually moved. Ignores invalid rows.
std::int16_t NovaTradeCenter_Buy(GameState &state,
                                 const TradeCenterSession &session,
                                 int row,
                                 std::int16_t quantity);
std::int16_t NovaTradeCenter_Sell(GameState &state,
                                  const TradeCenterSession &session,
                                  int row,
                                  std::int16_t quantity);

// Advances the selection, skipping rows whose price is 0 and wrapping 0..7
// (Ghidra NovaUi_TradeCenterCycleSelection 0x0048d5a0). `previous` true moves
// backward.
void NovaTradeCenter_CycleSelection(const TradeCenterSession &session,
                                    std::int16_t &selected,
                                    bool previous);

} // namespace game
