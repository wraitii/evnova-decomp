#include "game/docked_dialog.hpp"
#include "game/game_state.hpp"
#include "game/hud_overlay.hpp"

#include "brgr_archive.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
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
  CHECK(NovaLanded_BarDescriptionId(0x80) ==
        std::optional<std::uint16_t>{10000}); // Earth
  CHECK(NovaLanded_BarDescriptionId(0x89) ==
        std::optional<std::uint16_t>{10009}); // Port Kane
  CHECK_FALSE(NovaLanded_BarDescriptionId(0x7f).has_value());

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

// The trade center reads its geometry from DLOG/DITL 0x3e9 instead of a
// hardcoded table. Lock the ordinal -> rect mapping the redraw depends on:
// UiPanel entry N is DITL item N-1, so the header is item 2, rows 3..10, the
// summary item 11, the banner item 14 and the Leave/Buy/Sell buttons 0/12/13.
TEST_CASE("trade center DITL 0x3e9 exposes the expected control rects",
          "[docked][trade]") {
  const auto definition = NovaResource_LoadDialogDefinition(0x3e9);
  REQUIRE(definition.has_value());
  CHECK(definition->right - definition->left == 426);
  CHECK(definition->bottom - definition->top == 252);
  const auto items =
      NovaResource_LoadDialogItems(definition->dialog_item_list_id);
  REQUIRE(items.has_value());
  REQUIRE(items->size() >= 15);
  CHECK((*items)[2].top == 9);
  CHECK((*items)[2].left == 38);
  CHECK((*items)[3].top == 25);
  CHECK((*items)[10].bottom == 125);
  CHECK((*items)[11].top == 124);
  CHECK((*items)[14].top == 190);
  CHECK((*items)[0].left == 272);
  CHECK((*items)[12].left == 60);
  CHECK((*items)[13].left == 166);
}

// The trade center rows are filled from the c\x9alr list palette, not the
// store grid colors: the shipped resource has a black list_background and a
// dark-red list_hilite, which is what the selected row must use.
TEST_CASE("trade center row palette is the scenario list colors",
          "[docked][trade]") {
  const auto style = NovaResource_LoadMainMenuStyle();
  REQUIRE(style.has_value());
  CHECK(style->list_background.red == 0x00);
  CHECK(style->list_background.green == 0x00);
  CHECK(style->list_background.blue == 0x00);
  CHECK(style->list_hilite.red == 0x80);
  CHECK(style->list_hilite.green == 0x00);
  CHECK(style->list_hilite.blue == 0x00);
  CHECK(style->list_text.red == 0xff);
  CHECK(style->list_text.green == 0xff);
  CHECK(style->list_text.blue == 0xff);
  // Shipped startup loading-bar geometry and ProgBright/Dim/Outline colors
  // (c\xf6lr +0x5e..+0x64 and +0x66/+0x6a/+0x6e).
  CHECK(style->progress_bar_top == 280);
  CHECK(style->progress_bar_left == -100);
  CHECK(style->progress_bar_bottom == 290);
  CHECK(style->progress_bar_right == 100);
  CHECK(style->progress_fill.red == 0xff);
  CHECK(style->progress_fill.green == 0x00);
  CHECK(style->progress_fill.blue == 0x00);
  CHECK(style->progress_inner.red == 0x80);
  CHECK(style->progress_inner.green == 0x00);
  CHECK(style->progress_inner.blue == 0x00);
  CHECK(style->progress_outer.red == 0x40);
  CHECK(style->progress_outer.green == 0x40);
  CHECK(style->progress_outer.blue == 0x40);
}

// The Outfitter/Shipyard buttons now resolve their captions from the shared
// three-state label table (STR# 0x96, table index n -> entry n+1) instead of
// hardcoded all-caps strings. Lock the entries the store uses.
TEST_CASE("store button captions resolve from STR# 0x96", "[docked][store]") {
  CHECK(NovaHud_LoadStringEntry(0x96, 5).value_or("") == "Done");
  CHECK(NovaHud_LoadStringEntry(0x96, 2).value_or("") == "Buy");
  CHECK(NovaHud_LoadStringEntry(0x96, 3).value_or("") == "Sell");
  CHECK(NovaHud_LoadStringEntry(0x96, 4).value_or("") == "Buy Ship");
  CHECK(NovaHud_LoadStringEntry(0x96, 13).value_or("") == "Hire Escort");
  CHECK(NovaHud_LoadStringEntry(0x96, 48).value_or("") == "Info");
  CHECK(NovaHud_LoadStringEntry(0x96, 19).value_or("") == "^");
  CHECK(NovaHud_LoadStringEntry(0x96, 20).value_or("") == "&");
}

// The Holovid background is the landed stellar's government NewsPic, else the
// generic PICT 9000 (NovaUi_RunTravelNewsWindow 0x0047d180). Stellar government
// ids are zero-based, so the lookup must not re-subtract 0x80: Federation
// planets were showing the generic ICN art (9000) instead of PICT 9001.
TEST_CASE("news window background resolves the stellar government NewsPic",
          "[docked][news]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  CHECK(NovaBar_NewsPictId(state, 0x80) == 9001); // Earth -> Federation
  CHECK(NovaBar_NewsPictId(state, 0x81) == 9000); // Reflex-ion -> ungoverned

  // Guard the exact regression: the 0x80-based resource-id lookup misses a
  // zero-based stellar government (returns null) and so fell back to 9000.
  const Stellar *earth = state.scenario.Stellar(0x80);
  REQUIRE(earth != nullptr);
  CHECK(state.scenario.Government(earth->government_id) == nullptr);
  CHECK(state.scenario.Government(earth->government_id + 0x80) != nullptr);
}

// The crön-news arm (NovaUi_ComposeTravelNewsTexts 0x0047d600) picks the
// news STR# for an active, past-holdoff event: an allied NewsGovt/GovtNewsStr
// pair's string when one matches, else IndNewsStr, with local news winning.
// The per-slot string is the LAST allied pair (the original overwrites it in
// place), and an allied pair with a non-positive string id suppresses the
// independent fallback (slot_str < -1 is required to arm it).
TEST_CASE("cron news selection obeys local/independent precedence",
          "[docked][news][cron]") {
  GameState state;
  state.scenario.governments.assign(2, {});
  state.scenario.governments[0].present = true;
  state.scenario.governments[1].present = true;
  state.scenario.stellars.assign(1, {});
  state.scenario.stellars[0].government_id = 0; // landed stellar's govt
  state.scenario.cron_events.assign(1, {});
  CronEventDef &cron = state.scenario.cron_events[0];
  cron.present = true;
  cron.news_govts = {0, -1, -1, -1};
  cron.govt_news_strs = {15000, -1, -1, -1};
  cron.independent_news_str = 15007;
  GameState::CronEventState &runtime = state.cron_event_states[0];
  runtime.is_active = true;
  runtime.holdoff_counter = 0;

  // Allied local news beats the independent pool.
  CHECK(Bar_SelectCronNewsStr(state, 0x80) == 15000);

  // Pre-holdoff (and inactive / absent) events do not contribute.
  runtime.holdoff_counter = 2;
  CHECK_FALSE(Bar_SelectCronNewsStr(state, 0x80).has_value());
  runtime.holdoff_counter = 0;
  runtime.is_active = false;
  CHECK_FALSE(Bar_SelectCronNewsStr(state, 0x80).has_value());
  runtime.is_active = true;
  cron.present = false;
  CHECK_FALSE(Bar_SelectCronNewsStr(state, 0x80).has_value());
  cron.present = true;

  // An allied pair whose GovtNewsStr is unset does NOT arm the independent
  // fallback: the original arms it only while no pair matched at all.
  cron.govt_news_strs = {-1, -1, -1, -1};
  CHECK_FALSE(Bar_SelectCronNewsStr(state, 0x80).has_value());

  // With no allied government, IndNewsStr supplies the news.
  cron.news_govts = {1, -1, -1, -1};
  cron.govt_news_strs = {15000, -1, -1, -1};
  CHECK(Bar_SelectCronNewsStr(state, 0x80) == 15007);

  // The last allied pair wins when several match.
  cron.news_govts = {0, 0, -1, -1};
  cron.govt_news_strs = {15000, 15001, -1, -1};
  CHECK(Bar_SelectCronNewsStr(state, 0x80) == 15001);
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
