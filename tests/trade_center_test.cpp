#include "game/game_state.hpp"
#include "game/scenario_data.hpp"
#include "game/trade_center.hpp"

#include <catch2/catch_test_macros.hpp>

namespace game {

TEST_CASE("scenario loads the junk commodity table", "[scenario][trade]") {
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());
  REQUIRE(data.junk_defs.size() == 0x80);
  // The shipped scenario defines 23 j\x9fnk records (ids 0x80..0x96).
  const JunkDef *first = data.Junk(0x80);
  REQUIRE(first != nullptr);
  CHECK(first->present);
  CHECK_FALSE(first->display_name.empty());
  CHECK(data.Junk(0x96) != nullptr);
  CHECK(data.Junk(0x96)->present);
  CHECK_FALSE(data.Junk(0x97)->present);
}

namespace {
GameState MakeTradeState() {
  GameState state;
  state.scenario.ships.assign(1, {});
  state.scenario.ships[0].cargo_holds = 10;
  state.scenario.stellars.assign(1, {});
  state.scenario.disaster_defs.assign(1, {});
  state.scenario.junk_defs.assign(1, {});
  state.system_reputation.assign(1, 0);
  state.player.ship_class_id = 0;
  state.player.credits = 1000;
  state.inventory.cargo_bins.fill(0);
  state.inventory.junk_counts.fill(0);
  state.cached_stats.cargo_capacity = 10.0F;
  state.stat_cache_valid = true;
  return state;
}
} // namespace

TEST_CASE("trade center builds per-stellar prices", "[trade]") {
  GameState state = MakeTradeState();
  state.scenario.stellars[0].flags = 0x10000000U;
  state.scenario.stellars[0].government_id = 0;
  state.scenario.stellars[0].system_id = 0;
  const TradeCenterSession session = NovaTradeCenter_OpenSession(state, 0x80);
  REQUIRE(session.Valid());
  CHECK(session.rows[0].trend == 1);
  CHECK(session.rows[0].price == 60);
  CHECK(session.rows[1].price == 0);
  CHECK(session.rows[6].junk_id == -1);
  CHECK(session.rows[7].junk_id == -1);
}

TEST_CASE("disaster and junk override the trade table", "[trade]") {
  GameState state = MakeTradeState();
  state.scenario.stellars[0].flags = 0x10000000U;
  state.scenario.disaster_defs[0].present = true;
  state.scenario.disaster_defs[0].days_remaining = 2;
  state.scenario.disaster_defs[0].active_stellar = 0;
  state.scenario.disaster_defs[0].commodity = 0;
  state.scenario.disaster_defs[0].price_delta = 100;
  JunkDef &junk = state.scenario.junk_defs[0];
  junk.present = true;
  junk.base_price = 100;
  junk.bought_at[0] = 0;
  junk.sold_at[0] = 0;
  const TradeCenterSession session = NovaTradeCenter_OpenSession(state, 0x80);
  CHECK(session.rows[0].price == 175);
  CHECK(session.rows[0].has_disaster);
  CHECK(session.rows[0].disaster_raised);
  REQUIRE(session.rows[6].junk_id == 0);
  CHECK(session.rows[6].price == 125);
  REQUIRE(session.rows[7].junk_id == 0);
  CHECK(session.rows[7].price == 80);
}

TEST_CASE("trade center buy and sell move cargo and credits", "[trade]") {
  GameState state = MakeTradeState();
  state.scenario.stellars[0].flags = 0x10000000U;
  const TradeCenterSession session = NovaTradeCenter_OpenSession(state, 0x80);
  REQUIRE(session.rows[0].price > 0);
  CHECK(NovaTradeCenter_Buy(state, session, 0, 3) == 3);
  CHECK(state.inventory.cargo_bins[0] == 3);
  CHECK(state.player.credits == 1000 - session.rows[0].price * 3);
  CHECK(NovaTradeCenter_Sell(state, session, 0, 2) == 2);
  CHECK(state.inventory.cargo_bins[0] == 1);
  CHECK(NovaTradeCenter_Buy(state, session, 0, 9) == 9);
  CHECK(state.inventory.cargo_bins[0] == 10);
  CHECK(NovaTradeCenter_BuyMax(state, session, 0) == 0);
  CHECK(NovaTradeCenter_Sell(state, session, 0, 20) == 10);
  CHECK(state.inventory.cargo_bins[0] == 0);
}

} // namespace game
