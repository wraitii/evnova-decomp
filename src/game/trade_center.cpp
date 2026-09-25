#include "trade_center.hpp"

#include "hud_overlay.hpp"
#include "mission.hpp"
#include "outfit.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>

namespace game {
namespace {

// Fallback Bible base prices (DAT_005979b2) used when STR# 0xfa4 is not
// available: Food, Industrial, Medical, Luxury, Metal, Equipment.
constexpr std::array<std::int16_t, kTradeCenterCommodityCount>
    kFallbackBasePrices{75, 350, 750, 900, 200, 550};

constexpr std::int16_t kResourceIdBase = 0x80;

[[nodiscard]] std::int16_t ParsePrice(std::string_view text,
                                      std::int16_t fallback) {
  if (text.empty()) {
    return fallback;
  }
  std::int32_t value = 0;
  const auto *begin = text.data();
  const auto *end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr == begin) {
    return fallback;
  }
  return static_cast<std::int16_t>(std::clamp<std::int32_t>(
      value, 0, std::numeric_limits<std::int16_t>::max()));
}

// The original truncates each scaled price toward zero via the x87 FIST +
// residual/sign correction (0x0048c730), not CRT round().
[[nodiscard]] std::int16_t RoundPrice(float value) {
  return static_cast<std::int16_t>(value);
}

[[nodiscard]] bool ContainsStellar(const std::array<std::int16_t, 8> &list,
                                   std::int16_t index) {
  return std::find(list.begin(), list.end(), index) != list.end();
}

} // namespace

std::array<std::int16_t, kTradeCenterCommodityCount> TradeCenter_BasePrices() {
  static const std::array<std::int16_t, kTradeCenterCommodityCount> prices =
      [] {
        std::array<std::int16_t, kTradeCenterCommodityCount> result =
            kFallbackBasePrices;
        for (std::size_t i = 0; i < result.size(); ++i) {
          const auto text =
              NovaHud_LoadStringEntry(0xfa4, static_cast<std::uint16_t>(i + 1));
          if (text) {
            result[i] = ParsePrice(*text, result[i]);
          }
        }
        return result;
      }();
  return prices;
}

std::int16_t Stellar_TradeConnective(const Stellar &stellar,
                                     std::size_t commodity) {
  if (commodity >= kTradeCenterCommodityCount) {
    return 0;
  }
  const std::uint32_t base = 0x10000000U >> (4 * commodity);
  if ((stellar.flags & base) != 0U) {
    return 1;
  }
  if ((stellar.flags & (base << 1)) != 0U) {
    return 2;
  }
  if ((stellar.flags & (base << 2)) != 0U) {
    return 4;
  }
  return 0;
}

const TradeCenterRow *TradeCenterSession::Row(int index) const {
  if (index < 0 || index >= static_cast<int>(rows.size())) {
    return nullptr;
  }
  return &rows[static_cast<std::size_t>(index)];
}

TradeCenterSession NovaTradeCenter_OpenSession(const GameState &state,
                                               std::int16_t stellar_id) {
  TradeCenterSession session;
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr) {
    return session;
  }
  session.stellar_id = stellar_id;

  const std::int16_t stellar_index =
      static_cast<std::int16_t>(stellar_id - kResourceIdBase);

  // Reputation/hazard price tint (0x0048c730 prologue). The reputation arm
  // only applies to a stellar that has a government; hazard overrides it.
  double scale = 1.25;
  if (stellar->government_id != -1 && stellar->system_id >= 0 &&
      static_cast<std::size_t>(stellar->system_id) <
          state.system_reputation.size() &&
      state.system_reputation[static_cast<std::size_t>(stellar->system_id)] <
          0) {
    scale = 1.1;
  }
  if (stellar->dominated) {
    scale = 1.5;
  }
  const float scale_f = static_cast<float>(scale);
  const auto base = TradeCenter_BasePrices();

  for (std::size_t i = 0; i < kTradeCenterCommodityCount; ++i) {
    const std::int16_t connective = Stellar_TradeConnective(*stellar, i);
    session.rows[i].trend = connective;
    if (connective <= 0) {
      continue;
    }
    float priced = static_cast<float>(base[i]);
    if (connective == 1) {
      priced = static_cast<float>(base[i]) / scale_f;
    } else if (connective == 4) {
      priced = static_cast<float>(base[i]) * scale_f;
    }
    std::int16_t price = RoundPrice(priced);
    if (price < 5) {
      price = 5;
    }
    session.rows[i].price = price;
  }

  // Active disasters at this stellar override the base price (last match wins,
  // matching the prologue's scan order). The badge records the last sign.
  for (const DisasterDef &disaster : state.scenario.disaster_defs) {
    if (!disaster.present || disaster.days_remaining <= 0 ||
        disaster.active_stellar != stellar_index) {
      continue;
    }
    const int commodity = disaster.commodity;
    if (commodity < 0 ||
        commodity >= static_cast<int>(kTradeCenterCommodityCount)) {
      continue;
    }
    std::int16_t price = static_cast<std::int16_t>(
        base[static_cast<std::size_t>(commodity)] + disaster.price_delta);
    if (price < 5) {
      price = 5;
    }
    TradeCenterRow &row = session.rows[static_cast<std::size_t>(commodity)];
    row.price = price;
    row.has_disaster = true;
    row.disaster_raised = disaster.price_delta > 0;
  }

  // Row 6: first junk def bought here with BuyOn passing (base * scale).
  for (std::size_t i = 0; i < state.scenario.junk_defs.size(); ++i) {
    const JunkDef &junk = state.scenario.junk_defs[i];
    if (!junk.present || !ContainsStellar(junk.bought_at, stellar_index) ||
        !Mission_CheckReactionConditionSatisfied(state, junk.buy_on)) {
      continue;
    }
    session.rows[6].junk_id = static_cast<std::int16_t>(i);
    session.rows[6].trend = 4;
    session.rows[6].price =
        RoundPrice(static_cast<float>(junk.base_price) * scale_f);
    break;
  }

  // Row 7: first junk def sold here with SellOn passing (base / scale).
  for (std::size_t i = 0; i < state.scenario.junk_defs.size(); ++i) {
    const JunkDef &junk = state.scenario.junk_defs[i];
    if (!junk.present || !ContainsStellar(junk.sold_at, stellar_index) ||
        !Mission_CheckReactionConditionSatisfied(state, junk.sell_on)) {
      continue;
    }
    session.rows[7].junk_id = static_cast<std::int16_t>(i);
    session.rows[7].trend = 1;
    session.rows[7].price =
        RoundPrice(static_cast<float>(junk.base_price) / scale_f);
    break;
  }

  return session;
}

std::string NovaTradeCenter_RowName(const GameState &state,
                                    const TradeCenterSession &session,
                                    int row) {
  if (row < 0 || row >= static_cast<int>(kTradeCenterRowCount)) {
    return {};
  }
  if (row < static_cast<int>(kTradeCenterCommodityCount)) {
    // STR# 0xfa0 (All Cargo), 1-based entry row + 1.
    const auto text =
        NovaHud_LoadStringEntry(0xfa0, static_cast<std::uint16_t>(row + 1));
    return text.value_or(std::string{});
  }
  const TradeCenterRow *slot = session.Row(row);
  if (slot == nullptr || slot->junk_id < 0) {
    return {};
  }
  const JunkDef *junk = state.scenario.Junk(
      static_cast<std::int16_t>(slot->junk_id) + kResourceIdBase);
  return junk != nullptr ? junk->display_name : std::string{};
}

std::int16_t NovaTradeCenter_HeldCount(const GameState &state,
                                       const TradeCenterSession &session,
                                       int row) {
  if (row < 0 || row >= static_cast<int>(kTradeCenterRowCount)) {
    return 0;
  }
  if (row < static_cast<int>(kTradeCenterCommodityCount)) {
    return state.inventory.cargo_bins[static_cast<std::size_t>(row)];
  }
  const TradeCenterRow *slot = session.Row(row);
  if (slot == nullptr || slot->junk_id < 0) {
    return 0;
  }
  return state.inventory.junk_counts[static_cast<std::size_t>(slot->junk_id)];
}

// @port 0x00465e50 100%
// Ghidra 0x00465e50 TradeCenter_CanBuySelectedRow: credits >= the row price
// (session.Row price) and fleet cargo capacity > cargo+junk total.
bool NovaTradeCenter_CanBuyRow(const GameState &state,
                               const TradeCenterSession &session,
                               int row) {
  const TradeCenterRow *slot = session.Row(row);
  if (slot == nullptr || slot->price <= 0) {
    return false;
  }
  if (state.player.credits < slot->price) {
    return false;
  }
  const std::int32_t capacity = Player_ComputeFleetCargoCapacity(state);
  const std::int32_t used = Player_ComputeCargoAndJunkTotal(state);
  return capacity > used;
}

// @port 0x00465ea0 100%
// Ghidra 0x00465ea0 TradeCenter_HasSelectedRowStock: the selected row holds
// at least one unit (cargo bin for rows 0..5, junk count otherwise).
bool NovaTradeCenter_HasRowStock(const GameState &state,
                                 const TradeCenterSession &session,
                                 int row) {
  return NovaTradeCenter_HeldCount(state, session, row) > 0;
}

std::int16_t NovaTradeCenter_BuyMax(const GameState &state,
                                    const TradeCenterSession &session,
                                    int row) {
  const TradeCenterRow *slot = session.Row(row);
  if (slot == nullptr || slot->price <= 0) {
    return 0;
  }
  const std::int32_t capacity = Player_ComputeFleetCargoCapacity(state);
  const std::int32_t used = Player_ComputeCargoAndJunkTotal(state);
  const std::int32_t free_space = capacity - used;
  if (free_space <= 0) {
    return 0;
  }
  const auto affordable = static_cast<std::int32_t>(
      std::lround(static_cast<float>(state.player.credits) /
                  static_cast<float>(slot->price)));
  std::int32_t max_qty =
      std::min(free_space, std::max<std::int32_t>(0, affordable));
  max_qty = std::min<std::int32_t>(max_qty, 32000);
  return static_cast<std::int16_t>(std::max<std::int32_t>(0, max_qty));
}

std::int16_t NovaTradeCenter_SellMax(const GameState &state,
                                     const TradeCenterSession &session,
                                     int row) {
  const std::int32_t owned = NovaTradeCenter_HeldCount(state, session, row);
  return static_cast<std::int16_t>(std::clamp<std::int32_t>(owned, 0, 32000));
}

std::int16_t NovaTradeCenter_Buy(GameState &state,
                                 const TradeCenterSession &session,
                                 int row,
                                 std::int16_t quantity) {
  const TradeCenterRow *slot = session.Row(row);
  if (slot == nullptr || slot->price <= 0) {
    return 0;
  }
  const std::int32_t qty = std::clamp<std::int32_t>(
      quantity, 0, NovaTradeCenter_BuyMax(state, session, row));
  if (qty <= 0) {
    return 0;
  }
  if (row < static_cast<int>(kTradeCenterCommodityCount)) {
    state.inventory.cargo_bins[static_cast<std::size_t>(row)] =
        static_cast<std::int16_t>(
            state.inventory.cargo_bins[static_cast<std::size_t>(row)] + qty);
  } else if (slot->junk_id >= 0) {
    state.inventory.junk_counts[static_cast<std::size_t>(slot->junk_id)] =
        static_cast<std::int16_t>(
            state.inventory
                .junk_counts[static_cast<std::size_t>(slot->junk_id)] +
            qty);
  } else {
    return 0;
  }
  state.player.credits -= slot->price * qty;
  state.InvalidateDerivedStatCaches();
  return static_cast<std::int16_t>(qty);
}

std::int16_t NovaTradeCenter_Sell(GameState &state,
                                  const TradeCenterSession &session,
                                  int row,
                                  std::int16_t quantity) {
  const TradeCenterRow *slot = session.Row(row);
  if (slot == nullptr || slot->price <= 0) {
    return 0;
  }
  const std::int32_t qty = std::clamp<std::int32_t>(
      quantity, 0, NovaTradeCenter_SellMax(state, session, row));
  if (qty <= 0) {
    return 0;
  }
  if (row < static_cast<int>(kTradeCenterCommodityCount)) {
    state.inventory.cargo_bins[static_cast<std::size_t>(row)] =
        static_cast<std::int16_t>(
            state.inventory.cargo_bins[static_cast<std::size_t>(row)] - qty);
  } else if (slot->junk_id >= 0) {
    state.inventory.junk_counts[static_cast<std::size_t>(slot->junk_id)] =
        static_cast<std::int16_t>(
            state.inventory
                .junk_counts[static_cast<std::size_t>(slot->junk_id)] -
            qty);
  } else {
    return 0;
  }
  state.player.credits += slot->price * qty;
  state.InvalidateDerivedStatCaches();
  return static_cast<std::int16_t>(qty);
}

// @port 0x0048d5a0 95% gameplay
// Ghidra 0x0048d5a0 NovaUi_TradeCenterCycleSelection: skip price-0 rows, wrap
// 0..7. TODO(decomp): the port adds an all-disabled guard the original lacks
// (its `while (price == 0)` spins forever on an all-zero table).
void NovaTradeCenter_CycleSelection(const TradeCenterSession &session,
                                    std::int16_t &selected,
                                    bool previous) {
  const auto step = [&]() {
    selected = static_cast<std::int16_t>(selected + (previous ? -1 : 1));
    if (selected > 7) {
      selected = 0;
    }
    if (selected < 0) {
      selected = 7;
    }
  };
  step();
  // Skip rows whose price is 0. Bail out if no row is tradeable to avoid the
  // original's unbounded loop over an all-disabled table.
  bool any_enabled = false;
  for (const TradeCenterRow &row : session.rows) {
    if (row.price != 0) {
      any_enabled = true;
      break;
    }
  }
  if (!any_enabled) {
    return;
  }
  while (session.rows[static_cast<std::size_t>(selected)].price == 0) {
    step();
  }
}

} // namespace game
