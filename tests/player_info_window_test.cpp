#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/hud_overlay.hpp"
#include "game/player_info_window.hpp"
#include "game/starmap.hpp"

TEST_CASE("combat rank gives positive scores the first ability rank") {
  CHECK(game::NovaPlayerInfo_CombatRankIndex(0) == 0);
  CHECK(game::NovaPlayerInfo_CombatRankIndex(1) == 1);
  CHECK(game::NovaPlayerInfo_CombatRankIndex(99) == 1);
  CHECK(game::NovaPlayerInfo_CombatRankIndex(100) == 2);
  CHECK(game::NovaPlayerInfo_CombatRankIndex(25600) == 10);
}

TEST_CASE("legal status reflects current system reputation") {
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.governments.resize(1);
  state.scenario.systems[0].government_id = 0;
  state.scenario.governments[0].crime_tol = 10;
  state.system_reputation = {1};

  const auto expected = game::NovaHud_LoadStringEntry(0x86, 11);
  REQUIRE(expected);
  CHECK(game::NovaUi_SystemFactionConflictStatusText(state, 0) == *expected);
  CHECK(*expected != "N/A");
}

TEST_CASE("player info extras separate consecutive outfits") {
  game::GameState state;
  state.scenario.outfits.resize(state.inventory.outfit_owned_count.size());

  state.inventory.outfit_owned_count[0] = 1;
  state.scenario.outfits[0].lc_name = "alpha module";
  state.scenario.outfits[0].lc_plural = "alpha modules";

  state.inventory.outfit_owned_count[1] = 2;
  state.scenario.outfits[1].lc_name = "beta module";
  state.scenario.outfits[1].lc_plural = "beta modules";

  const auto texts = game::NovaPlayerInfo_BuildSummaryTexts(state);
  CHECK(texts.extras.find("an alpha module, 2 beta modules.") !=
        std::string::npos);
}
