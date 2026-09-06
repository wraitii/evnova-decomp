#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/landed_store.hpp"
#include "game/landed_window.hpp"

#include <string>
#include <string_view>

// The player's ship class supplies the baseline Contribute mask that almost
// every outfit's Require mask depends on (e.g. the starter Shuttle contributes
// Require high-word bit 0, matching the req_hi=1 on base weapons such as the
// Light Blaster). Without seeding the aggregate from the ship class a fresh
// player fails every Require gate and can buy nothing. Regression for the
// "can't buy outfits" bug.
TEST_CASE("Outfitter buying respects the ship class contribute baseline",
          "[landed_store][scenario]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // the starter Shuttle (id 0x80)
  state.player.credits = 10'000;

  // The starter Shuttle's Contribute mask comes from the shp payload: lo=0,
  // hi=1 (bit 0 of the high word), giving a fresh player the base that the
  // Light Blaster (req_hi=1) and friends require to be purchasable.
  const game::ShipClass *shuttle = state.scenario.Ship(0x80);
  REQUIRE(shuttle != nullptr);
  CHECK(shuttle->contribute_lo == 0);
  CHECK(shuttle->contribute_hi == 1);
  // The previously (mis)decoded require at shp +0x72a is the Shuttle's cost,
  // not a Require mask; the real Require (+0x380/+0x384) is empty.
  CHECK(shuttle->require_lo == 0);
  CHECK(shuttle->require_hi == 0);

  // With the baseline contribute mask the Light Blaster is purchasable and
  // buys cleanly.
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x80));
  game::LandedStoreSession session;
  CHECK(game::NovaLanded_BuyOutfit(state, 0x80, 0x80, 1) == 1);
  CHECK(state.inventory.outfit_owned_count[0] == 1);
  CHECK(state.player.credits == 5'000); // paid the 5000 credit list price
}

// The docked landing-description panel is word-wrapped. The wrap must NOT
// "cumulatively append": each new line must start fresh (with only its own
// words), never re-embed the already-drained earlier line. Regression for a
// bug where a wrapping line re-appended the drained text onto the next line,
// making every subsequent line repeat all prior words.
TEST_CASE("landing description word-wrap produces distinct lines",
          "[landed_window]") {
  using game::WrapDescriptionLines;
  auto width = [](std::string_view s) { return static_cast<int>(s.size()); };

  const std::string text =
      "The warriors on this station hold themselves ready to deal with any "
      "large-scale threat to the sovereignty of the Polaris.";
  const auto lines = WrapDescriptionLines(text, 40, width);
  REQUIRE(lines.size() >= 3);

  // Each wrapped line fits the row (except a single-word overflow, not the
  // case here) and no line is a substring of a later line.
  for (std::size_t i = 0; i < lines.size(); ++i) {
    CHECK(static_cast<int>(lines[i].size()) <= 40);
    for (std::size_t j = i + 1; j < lines.size(); ++j) {
      CHECK_FALSE(lines[j].starts_with(lines[i]));
      CHECK(lines[j].find(lines[i]) == std::string::npos);
    }
  }
  // Rejoining the wrapped lines with a single separating space must reproduce
  // the original text exactly (the wrap only ever drops the whitespace between
  // a line's last word and the next, never any word characters).
  {
    std::string rejoined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i != 0) {
        rejoined.push_back(' ');
      }
      rejoined += lines[i];
    }
    CHECK(rejoined == text);
  }

  // A single over-wide word is emitted on its own (unsplit) line.
  const auto long_word = WrapDescriptionLines("supercalifragilistic", 4, width);
  REQUIRE(long_word.size() == 1);
  CHECK(long_word[0] == "supercalifragilistic");
}

TEST_CASE("normal landing arrival charges once; launch restores the ship",
          "[landed_window]") {
  // SDL-free core of Stellar_TravelToSystem's normal-arrival bookkeeping.
  // The target is deliberately an inactive (not currently rendered) stellar:
  // StellarTargetsSpriteSetActive accepts the matching inactive state unless
  // the stellar's 0x80 engaged bit is set.
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Testport";
  stellar.pos_x = 123;
  stellar.pos_y = -456;
  stellar.flags = 0x1;
  stellar.is_available = true;
  stellar.system_id = 0;
  stellar.service_cost = 75;
  state.scenario.ships.resize(1);
  state.scenario.ships[0].base_shield = 300;
  state.scenario.ships[0].base_armor = 250;

  state.player.current_system_id = 0;
  state.player.ship_class_id = 0;
  state.player.credits = 100;
  state.player.pos_x = 3.0F;
  state.player.pos_y = -300.0F;
  state.player.vel_x = 2.0F;
  state.player.vel_y = -1.0F;
  state.player.speed = 4.0F;
  state.player.shield_points = 1.0F;
  state.player.armor_points = 2.0F;
  state.travel.selected_stellar_id = 0x80;

  game::LandedContext ctx;
  REQUIRE(game::NovaLanding_EnterDocked(state, ctx));
  CHECK(ctx.landed);
  CHECK(ctx.stellar_id == 0x80);
  CHECK(state.travel.landed_this_frame);
  CHECK(state.player.credits == 25);
  // Arrival does NOT touch the ship's meters or kinematics: the original
  // leaves them to the launch tail, after the interaction loop returns
  // (Stellar_TravelToSystem 0x00455f99..0x0045602f).
  CHECK(state.player.pos_x == 3.0F);
  CHECK(state.player.pos_y == -300.0F);
  CHECK(state.player.shield_points == 1.0F);
  CHECK(state.player.armor_points == 2.0F);

  // Launch tail (0x00455f99..0x00456268): reposition + velocity kill,
  // shield/armor refill to the effective maxima, daily world tick.
  game::NovaLanding_LaunchFromStellar(state, ctx.stellar_id);
  CHECK(state.player.pos_x == 123.0F);
  CHECK(state.player.pos_y == -456.0F);
  CHECK(state.player.vel_x == 0.0F);
  CHECK(state.player.vel_y == 0.0F);
  CHECK(state.player.speed == 0.0F);
  CHECK(state.player.shield_points == 300.0F);
  CHECK(state.player.armor_points == 250.0F);
  // 0x00456109: the launch heading is a fresh rand(0x168) roll.
  CHECK(state.player.heading >= 0.0F);
  CHECK(state.player.heading < 6.2831855F);
  // 0x00456158: the travel selection resets on launch.
  CHECK(state.travel.selected_stellar_id == -1);
}
