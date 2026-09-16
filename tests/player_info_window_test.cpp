#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/player_info_window.hpp"

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
