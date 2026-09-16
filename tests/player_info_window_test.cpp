#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/hud_overlay.hpp"
#include "game/outfit.hpp"
#include "game/player_info_window.hpp"
#include "game/starmap.hpp"
#include "game/weapon.hpp"

#include <filesystem>

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
  // Two entries join with " and " (no Oxford comma; 0x0049c050 only adds the
  // comma for 3+).
  CHECK(texts.extras.find("an alpha module and two beta modules.") !=
        std::string::npos);
}

TEST_CASE("player info extras include mounted stock weapons after reconcile") {
  game::GameState state;
  state.scenario.outfits.resize(state.inventory.outfit_owned_count.size());
  game::Outfit &medium_blaster = state.scenario.outfits[3];
  medium_blaster.mod_type =
      static_cast<std::int16_t>(game::OutfitEffect::kWeapon);
  medium_blaster.mod_val = 7;
  medium_blaster.lc_name = "medium blaster";
  medium_blaster.lc_plural = "medium blasters";
  medium_blaster.similar_to = 3;
  state.weapon_count_by_class[7 * 100] = 1;

  game::NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);

  CHECK(state.inventory.outfit_owned_count[3] == 1);
  CHECK(game::NovaPlayerInfo_BuildSummaryTexts(state).extras.find(
            "a medium blaster") != std::string::npos);
}

TEST_CASE("player info extras group same-named outfits through similar_to") {
  game::GameState state;
  state.scenario.outfits.resize(state.inventory.outfit_owned_count.size());

  // Slots 0 and 1 share an LCName and slot 1's similar_to points at slot 0, so
  // the owned counts sum into one entry. Slot 2's higher DisplayWeight sorts it
  // first.
  state.inventory.outfit_owned_count[0] = 1;
  state.scenario.outfits[0].lc_name = "alpha module";
  state.scenario.outfits[0].lc_plural = "alpha modules";
  state.scenario.outfits[0].similar_to = 0;
  state.inventory.outfit_owned_count[1] = 1;
  state.scenario.outfits[1].lc_name = "alpha module";
  state.scenario.outfits[1].lc_plural = "alpha modules";
  state.scenario.outfits[1].similar_to = 0;
  state.inventory.outfit_owned_count[2] = 1;
  state.scenario.outfits[2].lc_name = "omega coil";
  state.scenario.outfits[2].lc_plural = "omega coils";
  state.scenario.outfits[2].similar_to = 2;
  state.scenario.outfits[2].display_weight = 10;

  const auto texts = game::NovaPlayerInfo_BuildSummaryTexts(state);
  CHECK(texts.extras.find("an omega coil and two alpha modules.") !=
        std::string::npos);
}

TEST_CASE("player info extras use the two/three count words") {
  if (!std::filesystem::exists("EV Nova/Nova.rez")) {
    SKIP("retail archives not installed");
  }
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  // Nova.rez supplies STR# 0x89 ("one".."ten"); entries 0x1e/0x1f are
  // "two"/"three".
  state.scenario.outfits[0].lc_name = "alpha module";
  state.scenario.outfits[0].lc_plural = "alpha modules";
  state.scenario.outfits[0].similar_to = 0;
  state.inventory.outfit_owned_count[0] = 2;
  CHECK(game::NovaPlayerInfo_BuildSummaryTexts(state).extras.find(
            "two alpha modules") != std::string::npos);
  state.inventory.outfit_owned_count[0] = 3;
  CHECK(game::NovaPlayerInfo_BuildSummaryTexts(state).extras.find(
            "three alpha modules") != std::string::npos);
}

TEST_CASE("player info honors list active rank names by weight") {
  game::GameState state;
  state.scenario.ranks.assign(0x80, {});
  state.scenario.outfits.resize(state.inventory.outfit_owned_count.size());

  // Higher Weight is displayed first; equal weights keep slot order.
  state.scenario.ranks[1].defined = true;
  state.scenario.ranks[1].active = true;
  state.scenario.ranks[1].weight = 20;
  state.scenario.ranks[1].full_name = "Knight of Red Branch";
  state.scenario.ranks[3].defined = true;
  state.scenario.ranks[3].active = true;
  state.scenario.ranks[3].weight = 5;
  state.scenario.ranks[3].full_name = "United Shipping Courier";
  // Inactive ranks and ranks with an empty full name are skipped.
  state.scenario.ranks[4].defined = true;
  state.scenario.ranks[4].weight = 99;
  state.scenario.ranks[4].full_name = "Inactive";
  state.scenario.ranks[5].defined = true;
  state.scenario.ranks[5].active = true;
  state.scenario.ranks[5].weight = 100;

  const auto texts = game::NovaPlayerInfo_BuildSummaryTexts(state);
  CHECK(
      texts.honors.find("Knight of Red Branch, and United Shipping Courier") !=
      std::string::npos);
}

TEST_CASE("player info honors include 0x2000 outfits after ranks") {
  game::GameState state;
  state.scenario.ranks.assign(0x80, {});
  state.scenario.outfits.resize(state.inventory.outfit_owned_count.size());
  state.scenario.ranks[0].defined = true;
  state.scenario.ranks[0].active = true;
  state.scenario.ranks[0].weight = 1;
  state.scenario.ranks[0].full_name = "Courier";
  state.inventory.outfit_owned_count[0] = 1;
  state.scenario.outfits[0].flags = 0x2000;
  state.scenario.outfits[0].lc_name = "medal";
  state.scenario.outfits[0].lc_plural = "medals";

  const auto texts = game::NovaPlayerInfo_BuildSummaryTexts(state);
  CHECK(texts.honors.find("Courier, and a medal") != std::string::npos);

  // The original's outfit tail uses plain ", " (the "and" is only inserted
  // by the rank phase), so a rank followed by two outfits has no "and".
  state.inventory.outfit_owned_count[1] = 2;
  state.scenario.outfits[1].flags = 0x2000;
  state.scenario.outfits[1].lc_name = "ribbon";
  state.scenario.outfits[1].lc_plural = "ribbons";
  const auto two_outfits = game::NovaPlayerInfo_BuildSummaryTexts(state);
  CHECK(two_outfits.honors.find("Courier, a medal, two ribbons") !=
        std::string::npos);
}
