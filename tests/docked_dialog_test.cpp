#include "game/docked_dialog.hpp"
#include "game/game_state.hpp"

#include "brgr_archive.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
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

// The bar's prompt panel is the destination desc at (0-based stellar index +
// 10000), not the raw-id landing description. The original passes
// g_ship_states->ai_secondary_target_slot + 10000 in
// NovaUi_RunTravelDestinationServicesWindow 0x0047c8e0, and that slot is the
// 0-based g_stellar_defs index.
TEST_CASE("bar prompt resolves the destination desc family",
          "[docked][bar][disaster]") {
  CHECK(NovaDocked_BarDescriptionId(0x80) ==
        std::optional<std::uint16_t>{10000}); // Earth
  CHECK(NovaDocked_BarDescriptionId(0x89) ==
        std::optional<std::uint16_t>{10009}); // Port Kane
  CHECK_FALSE(NovaDocked_BarDescriptionId(0x7f).has_value());

  const auto bar_desc = NovaResource_LoadDescription(10009);
  REQUIRE(bar_desc.has_value());
  CHECK(bar_desc->text.find("Hypergate") != std::string::npos);

  // The raw-id landing description is a different resource/text.
  const auto landing = NovaResource_LoadStellarDescription(0x89);
  REQUIRE(landing.has_value());
  CHECK(landing->text != bar_desc->text);
}

// The Holovid window renders from the DLOG 0x3f6 bounds plus hardcoded text
// panels (NovaUi_DrawTravelNewsWindow 0x0047d370). Its DITL is the 2-byte
// 0xffff placeholder in Nova.rez, so the window must not require a parseable
// item list.
TEST_CASE("news window DLOG exists while its DITL is a placeholder",
          "[docked][news]") {
  REQUIRE(NovaResource_LoadDialogDefinition(0x3f6).has_value());
  CHECK_FALSE(NovaResource_LoadDialogItems(0x3f6).has_value());
}

// The Holovid text panels are window-relative (NovaUi_DrawTravelNewsWindow
// 0x0047d370): headline (10,140)-(w-10,180), body (10,170)-(w-10,h-4). For the
// shipped 300x230 DLOG 0x3f6 this is a 280x40 headline band and a 280x56 body;
// the headline must have non-zero height (it used to be computed as h-180-140).
TEST_CASE("news text panels use the original window-relative rects",
          "[docked][news]") {
  const NewsTextPanels panels = NovaBar_NewsTextPanelRects(300.0F, 230.0F);
  CHECK(panels.headline[0] == 10.0F);
  CHECK(panels.headline[1] == 140.0F);
  CHECK(panels.headline[2] == 290.0F);
  CHECK(panels.headline[3] == 180.0F);
  CHECK(panels.headline[3] - panels.headline[1] == 40.0F); // non-zero band
  CHECK(panels.body[0] == 10.0F);
  CHECK(panels.body[1] == 170.0F);
  CHECK(panels.body[2] == 290.0F);
  CHECK(panels.body[3] == 226.0F);
  CHECK(panels.body[3] - panels.body[1] == 56.0F);
}

} // namespace game
