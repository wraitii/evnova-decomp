#include "game/docked_dialog.hpp"
#include "game/game_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace game {

namespace {

// Builds a single active öops record for the report tests. `active` and
// `target` are the DisasterDef runtime/static target fields: active is a
// 0-based g_stellar_defs index (or -1), target the 0x80-based resource id (or
// -1/-2).
void SetOnlyDisaster(GameState &state,
                     std::int16_t active,
                     std::int16_t target,
                     std::int16_t days_remaining,
                     std::int16_t price_delta,
                     std::int16_t commodity,
                     std::string name) {
  state.scenario.disaster_defs.assign(0x100, {});
  DisasterDef &disaster = state.scenario.disaster_defs[0];
  disaster.present = true;
  disaster.active_stellar = active;
  disaster.target_stellar = target;
  disaster.days_remaining = days_remaining;
  disaster.price_delta = price_delta;
  disaster.commodity = commodity;
  disaster.display_name = std::move(name);
}

} // namespace

// The disaster-report arm of Ghidra 0x0047d600 NovaUi_RedrawTravelNewsHeader
// (NovaBar_ComposeNewsTexts) is exposed as Bar_ComposeDisasterReport for
// tests. It folds the STR# 0x7d2 fragments around a record's display name,
// price direction, commodity (STR# 0xfa1) and host stellar name.
TEST_CASE("disaster report composes the price-shock news body",
          "[docked][news][disaster]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // A surplus at stellar 0x89 (Port Kane), lowering the food price.
  SetOnlyDisaster(state,
                  /*active=*/0x89 - 0x80,
                  /*target=*/0x89,
                  /*days_remaining=*/5,
                  /*price_delta=*/-15,
                  /*commodity=*/0,
                  "An enormous food surplus");

  const auto body = Bar_ComposeDisasterReport(state, 0x89);
  REQUIRE(body.has_value());
  CHECK(*body == "Today's top news: An enormous food surplus has lowered the "
                 "price of food on Port Kane.");

  // A positive delta reads "raised"; commodity 1 is industrial goods.
  SetOnlyDisaster(state,
                  /*active=*/0x89 - 0x80,
                  /*target=*/0x89,
                  /*days_remaining=*/5,
                  /*price_delta=*/70,
                  /*commodity=*/1,
                  "A shortage in supply");
  const auto raised = Bar_ComposeDisasterReport(state, 0x89);
  REQUIRE(raised.has_value());
  CHECK(*raised == "Today's top news: A shortage in supply has raised the "
                   "price of industrial goods on Port Kane.");

  // The landed stellar is irrelevant when no record is bound to it: the
  // original falls back to any active record, so a report still shows.
  const auto elsewhere = Bar_ComposeDisasterReport(state, 0x80);
  REQUIRE(elsewhere.has_value());
  CHECK(elsewhere->find("Port Kane") != std::string::npos);

  // A target_stellar of -2 counts as "everywhere" (the original's second
  // on-stellar test) even when the active index is unrelated, so the report is
  // still drawn from this record rather than the generic pool.
  SetOnlyDisaster(state,
                  /*active=*/50,
                  /*target=*/-2,
                  /*days_remaining=*/5,
                  /*price_delta=*/10,
                  /*commodity=*/2,
                  "A good harvest of bio-agents");
  const auto everywhere = Bar_ComposeDisasterReport(state, 0x80);
  REQUIRE(everywhere.has_value());
  CHECK(everywhere->find("A good harvest of bio-agents") != std::string::npos);
}

TEST_CASE("disaster report only covers records with more than one day left",
          "[docked][news][disaster]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // days_remaining <= 1 is not news-worthy in the original (strict `1 <`).
  SetOnlyDisaster(state,
                  /*active=*/0x89 - 0x80,
                  /*target=*/0x89,
                  /*days_remaining=*/1,
                  /*price_delta=*/-15,
                  /*commodity=*/0,
                  "A minor drought");
  CHECK_FALSE(Bar_ComposeDisasterReport(state, 0x89).has_value());

  SetOnlyDisaster(state,
                  /*active=*/0x89 - 0x80,
                  /*target=*/0x89,
                  /*days_remaining=*/2,
                  /*price_delta=*/-15,
                  /*commodity=*/0,
                  "A minor drought");
  CHECK(Bar_ComposeDisasterReport(state, 0x89).has_value());

  // A record with no resolved active stellar still counts toward the tally
  // but produces no body (the original only composes when +0x02 != -1).
  SetOnlyDisaster(state,
                  /*active=*/-1,
                  /*target=*/0x89,
                  /*days_remaining=*/5,
                  /*price_delta=*/-15,
                  /*commodity=*/0,
                  "A minor drought");
  CHECK_FALSE(Bar_ComposeDisasterReport(state, 0x89).has_value());

  // An unnamed record likewise produces nothing.
  SetOnlyDisaster(state,
                  /*active=*/0x89 - 0x80,
                  /*target=*/0x89,
                  /*days_remaining=*/5,
                  /*price_delta=*/-15,
                  /*commodity=*/0,
                  "");
  CHECK_FALSE(Bar_ComposeDisasterReport(state, 0x89).has_value());

  // No present records at all -> nullopt (the caller uses generic news).
  state.scenario.disaster_defs.assign(0x100, {});
  CHECK_FALSE(Bar_ComposeDisasterReport(state, 0x89).has_value());
}

} // namespace game
