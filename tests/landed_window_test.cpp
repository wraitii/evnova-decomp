#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/government.hpp"
#include "game/landed_store.hpp"
#include "game/landed_window.hpp"
#include "game/mission.hpp"
#include "game/outfit.hpp"
#include "game/travel.hpp"
#include "game/weapon.hpp"

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

// Desc resources separate the "Requires: ..." paragraph with a double Mac
// return; the original DrawTextW(DT_WORDBREAK) renders that as a blank line and
// the port must not collapse it. A single return still just ends the line.
TEST_CASE("description wrap preserves explicit blank lines",
          "[landed_window]") {
  using game::WrapDescriptionLines;
  auto width = [](std::string_view s) { return static_cast<int>(s.size()); };

  const auto lines = WrapDescriptionLines(
      "The drive hums.\r\rRequires: Heavy Weapons License", 200, width);
  REQUIRE(lines.size() == 3);
  CHECK(lines[0] == "The drive hums.");
  CHECK(lines[1].empty());
  CHECK(lines[2] == "Requires: Heavy Weapons License");

  const auto single = WrapDescriptionLines("one\rtwo", 200, width);
  REQUIRE(single.size() == 2);
  CHECK(single[0] == "one");
  CHECK(single[1] == "two");
}

// Stellar_HandleStellarEntryAndExit's normal-arrival envelope is not the
// starmap travel-arm 250 (0x00459369); it is the target's spin-sprite span
// scaled by 1.75 (0x004587a3), with a 0x4b fallback when no sprite is prepared
// (0x00457f89).
TEST_CASE("landing arrival envelope follows the sprite span",
          "[landed_window]") {
  // No prepared sprite -> the original 0x4b fallback.
  CHECK(game::Stellar_MaxLandingDistance(0) == 75.0F);
  // round(height * 1.75), FIST/round-half-to-even semantics.
  CHECK(game::Stellar_MaxLandingDistance(32) == 56.0F);
  CHECK(game::Stellar_MaxLandingDistance(96) == 168.0F);
  CHECK(game::Stellar_MaxLandingDistance(150) == 262.0F);
}

// Regression: the gate must reject a target in the gap between the original
// 0x4b fallback and the old hardcoded 250 envelope when no sprite is prepared.
TEST_CASE("landing rejects a stellar outside the no-sprite envelope",
          "[landed_window]") {
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Testport";
  stellar.pos_x = 0;
  stellar.pos_y = 0;
  stellar.flags = 0x1;
  stellar.is_available = true;
  stellar.system_id = 0;
  state.player.current_system_id = 0;
  state.player.pos_x = 120.0F; // > 75 (no-sprite), < 250 (old port limit)
  state.player.pos_y = 0.0F;
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = 0x2ee; // approach armed

  game::LandedContext ctx;
  CHECK_FALSE(game::Stellar_Dock(state, ctx, 0));
  CHECK(ctx.denial == game::LandedDenial::kTooFar);
  // A prepared sprite whose depth spans the ship keeps the arrival in range.
  CHECK(game::Stellar_Dock(state, ctx, 96));
}

TEST_CASE("landing rejects a moving ship within range", "[landed_window]") {
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Testport";
  stellar.pos_x = 0;
  stellar.pos_y = 0;
  stellar.flags = 0x1;
  stellar.is_available = true;
  stellar.system_id = 0;
  state.player.current_system_id = 0;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.player.vel_x = 2.0F; // > 0.75, outside the docking envelope
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = 0x2ee;

  game::LandedContext ctx;
  CHECK_FALSE(game::Stellar_Dock(state, ctx, 96));
  CHECK(ctx.denial == game::LandedDenial::kTooFast);

  // Stopping the ship clears the last gate.
  state.player.vel_x = 0.0F;
  CHECK(game::Stellar_Dock(state, ctx, 96));
}

TEST_CASE("landing reports authorization denial before distance",
          "[landed_window]") {
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Restricted station";
  stellar.pos_x = 0;
  stellar.pos_y = 0;
  stellar.flags = 0x11; // targetable + station
  stellar.is_available = true;
  stellar.system_id = 0;
  stellar.government_id = 0;
  stellar.min_status = 100;
  state.scenario.governments.resize(1);
  state.player.current_system_id = 0;
  state.player.pos_x = 1'000.0F; // distance must not mask the permission result
  state.system_reputation.resize(1);
  state.system_reputation[0] = 0;
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = 0;

  game::LandedContext ctx;
  CHECK_FALSE(game::Stellar_Dock(state, ctx, 96));
  CHECK(ctx.denial == game::LandedDenial::kUnauthorized);

  state.system_reputation[0] = 100;
  CHECK_FALSE(game::Stellar_Dock(state, ctx, 96));
  CHECK(ctx.denial == game::LandedDenial::kTooFar);
}

TEST_CASE("landing approach timer arms within 250 and expires", "[travel]") {
  game::GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  game::Stellar &stellar = state.scenario.stellars[0];
  stellar.name = "Testport";
  stellar.pos_x = 0;
  stellar.pos_y = 0;
  stellar.flags = 0x1;
  stellar.is_available = true;
  stellar.system_id = 0;
  state.player.current_system_id = 0;
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = -1;

  // Far away: initialises the timer but does not arm it (the original's
  // request radius is 0xfa on both axes).
  state.player.pos_x = 400.0F;
  game::NovaTravel_UpdateEngagementProgress(state);
  CHECK(state.travel.engage_timer == 0);

  // Within 250 on both axes: arms the request.
  state.player.pos_x = 100.0F;
  game::NovaTravel_UpdateEngagementProgress(state);
  CHECK(state.travel.engage_timer == 0x2ee);

  // Past 0x7ff the request expires and the selection clears.
  state.travel.engage_timer = 0x800;
  game::NovaTravel_UpdateEngagementProgress(state);
  CHECK(state.travel.engage_timer == -1);
  CHECK(state.travel.selected_stellar_id == -1);
}

TEST_CASE("normal landing arrival charges once; launch restores the ship",
          "[landed_window]") {
  // SDL-free core of Stellar_RunDockAndLaunchSequence's normal-arrival
  // bookkeeping. The target is deliberately an inactive (not currently
  // rendered) stellar: StellarTargetsSpriteSetActive accepts the matching
  // inactive state unless the stellar's 0x80 engaged bit is set.
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
  state.player.vel_x = 0.5F;
  state.player.vel_y = -0.5F;
  state.player.speed = 4.0F;
  state.player.shield_points = 1.0F;
  state.player.armor_points = 2.0F;
  state.travel.selected_stellar_id = 0x80;
  state.travel.engage_timer = 0x2ee; // approach armed

  game::LandedContext ctx;
  REQUIRE(game::Stellar_Dock(state, ctx, 96));
  CHECK(ctx.landed);
  CHECK(ctx.stellar_id == 0x80);
  CHECK(state.travel.landed_this_frame);
  CHECK(state.player.credits == 25);
  // Arrival does NOT touch the ship's meters or kinematics: the original
  // leaves them to the launch tail, after the interaction loop returns
  // (Stellar_RunDockAndLaunchSequence 0x00455f99..0x0045602f).
  CHECK(state.player.pos_x == 3.0F);
  CHECK(state.player.pos_y == -300.0F);
  CHECK(state.player.shield_points == 1.0F);
  CHECK(state.player.armor_points == 2.0F);

  // Launch tail (0x00455f99..0x00456268): reposition + velocity kill,
  // shield/armor refill to the effective maxima, daily world tick.
  game::Stellar_Launch(state, ctx.stellar_id);
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

// NovaUi_RunOutfitterInteractionLoop (0x0048ea70) clears the active weapon
// bank when the last unit of a mounted weapon outfit is sold, so a stale bank
// index can't point at an outfit the player no longer owns.
TEST_CASE("Outfitter sale clears the active weapon bank on the last unit",
          "[landed_store][outfitter]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // the starter Shuttle
  state.player.credits = 100'000;

  const game::Outfit *blaster = state.scenario.Outfit(0x80);
  REQUIRE(blaster != nullptr);
  REQUIRE(blaster->mod_type == 1); // a weapon outfit

  game::LandedStoreSession session =
      game::NovaLanded_OpenOutfitterSession(state, 0x80);
  REQUIRE(game::NovaLanded_BuyOutfit(state, 0x80, 0x80, 1) == 1);
  CHECK(state.inventory.outfit_owned_count[0] == 1);

  state.player.active_weapon_bank_slot = blaster->mod_val;
  const game::OutfitSaleResult sale =
      game::NovaLanded_SellOutfit(state, session, 0x80, 0x80, 1);
  CHECK(sale.sold == 1);
  CHECK(sale.block == game::OutfitSaleBlock::kNone);
  CHECK(state.inventory.outfit_owned_count[0] == 0);
  CHECK(state.player.active_weapon_bank_slot == -1);
}

TEST_CASE("launch rebuild includes missions accepted while docked") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  constexpr std::int16_t kRautherionSystem = 166 - 0x80;
  constexpr std::int16_t kRautherStellar = 191;
  constexpr std::int16_t kTutorial006Index = 754 - 0x80;
  constexpr std::int16_t kTutorialDerelictPers = 642 - 0x80;
  constexpr std::int16_t kPirateViperShipClass = 166 - 0x80;
  constexpr std::int16_t kDerelictsGovernment = 160 - 0x80;
  state.player.current_system_id = kRautherionSystem;
  state.player.ship_class_id = 0;

  REQUIRE(
      game::Mission_ActivateAtSlot(state, kTutorial006Index, kRautherStellar));
  REQUIRE(state.active_mission_runtime_flags[0].is_active);
  REQUIRE(state.active_mission_runtime_flags[1].is_active);
  REQUIRE(state.control.bits.test(9208));
  for (std::size_t slot = 1; slot < game::GameState::kMaxShips; ++slot) {
    CHECK_FALSE(state.ShipAt(slot).is_active);
  }

  game::Stellar_Launch(state, kRautherStellar);

  bool found_derelict = false;
  for (std::size_t slot = 1; slot < game::GameState::kMaxShips; ++slot) {
    const auto &ship = state.ShipAt(slot);
    if (ship.is_active && ship.pers_def_slot == kTutorialDerelictPers) {
      found_derelict = true;
      CHECK(ship.current_system_id == kRautherionSystem);
      CHECK(ship.ship_class_id == kPirateViperShipClass);
      CHECK(ship.faction_or_government_id == kDerelictsGovernment);
      CHECK(ship.mission_fleet_slot == -1);
    }
  }
  CHECK(found_derelict);
}

TEST_CASE("TEMP seed sweep derelict spawn") {
  constexpr std::int16_t kRautherionSystem = 166 - 0x80;
  constexpr std::int16_t kRautherStellar = 191;
  constexpr std::int16_t kTutorial006Index = 754 - 0x80;
  constexpr std::int16_t kTutorialDerelictPers = 642 - 0x80;
  int failures = 0;
  int first_failure = -1;
  for (unsigned seed = 1; seed <= 400; ++seed) {
    game::GameState state;
    REQUIRE(state.scenario.LoadFromArchives());
    state.rng.seed(seed);
    state.player.current_system_id = kRautherionSystem;
    state.player.ship_class_id = 0;
    if (!game::Mission_ActivateAtSlot(state, kTutorial006Index, kRautherStellar)) {
      ++failures;
      continue;
    }
    game::Stellar_Launch(state, kRautherStellar);
    bool found = false;
    for (std::size_t slot = 1; slot < game::GameState::kMaxShips; ++slot) {
      const auto &ship = state.ShipAt(slot);
      if (ship.is_active && ship.pers_def_slot == kTutorialDerelictPers) {
        found = true;
      }
    }
    if (!found) {
      ++failures;
      if (first_failure < 0) {
        first_failure = static_cast<int>(seed);
      }
    }
  }
  INFO("failures=" << failures << " first=" << first_failure);
  CHECK(failures == 0);
}

// DAT_007d4c0d (0x0048ea70): a stellar with a zero TechLevel and no positive
// SpecialTech has nothing to sell, which disables the Buy button.
TEST_CASE("stellar outfit availability follows tech level",
          "[landed_store][outfitter]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  int checked = 0;
  for (std::int16_t id = 0x80; id < 0x100; ++id) {
    const game::Stellar *stellar = state.scenario.Stellar(id);
    if (stellar == nullptr)
      continue;
    ++checked;
    bool expected = stellar->tech_level != 0;
    for (const std::int16_t tech : stellar->special_tech) {
      if (tech > 0) {
        expected = true;
        break;
      }
    }
    CHECK(game::NovaLanded_StellarSellsOutfits(state, id) == expected);
  }
  CHECK(checked > 0);
}

// NovaLanded_CanBuyOutfit (0x00491950) mode-99 bay arm: buying a
// carried-ship outfit requires a mounted bay with room for one more craft.
// The Viper (0x9e) is bound to weapon bank 21; its bay weapon is "Viper Bay"
// (0x9d, MaxAmmo 4) and its Require needs the Fighter Bay License (0x103).
TEST_CASE("Outfitter gate enforces fighter-bay capacity",
          "[landed_store][outfitter]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // the starter Shuttle
  state.player.credits = 5'000'000;

  const game::Outfit *viper = state.scenario.Outfit(0x9e);
  REQUIRE(viper != nullptr);
  REQUIRE(viper->mod_type ==
          static_cast<std::int16_t>(game::OutfitEffect::kAmmo));
  REQUIRE(viper->mod_val == 21);
  // The stock Viper carries a system/planet Availability expression; clear it
  // to isolate the bay-capacity gate under test.
  auto *mutable_viper = const_cast<game::Outfit *>(viper);
  const std::string saved_availability = mutable_viper->availability_expr;
  mutable_viper->availability_expr.clear();

  // Without the bay weapon mounted there is no holding capacity.
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x9e));

  // Grant the Fighter Bay License (require_lo bit 2) and mount the Viper Bay
  // directly, bypassing the ownership clamps (the license is a fixed grant).
  state.inventory.outfit_owned_count[0x103 - 0x80] = 1;
  state.inventory.outfit_owned_count[0x9d - 0x80] = 1;
  game::NovaWeapon_RebuildBanksFromOwnedOutfits(state);
  state.inventory.outfit_owned_count[0x9e - 0x80] = 0;
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x9e));

  // With the bay full (one mount x MaxAmmo 4) the gate closes.
  state.inventory.outfit_owned_count[0x9e - 0x80] = 4;
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x9e));

  mutable_viper->availability_expr = saved_availability;
}

// NovaLanded_CanBuyOutfit (0x00491950) step 5: RequireGovt scopes an
// outfit's Require bits to one of four government-keyed outfit-id bands. The
// helper is a direct port of the decompiled band chain.
TEST_CASE("RequireGovt scopes outfit requirements to government bands",
          "[landed_store][outfitter][government]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  using game::NovaLanded_RequireGovtAllows;

  // -1 and any out-of-band value apply in every shop.
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, -1, 0));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, -1, -1));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x7f, 0));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x1234, 7));

  // Locate a government allied with 0 (Federation) and one that is not, so the
  // ally branches are exercised against the shipped relations.
  std::int16_t ally = -1;
  std::int16_t foe = -1;
  for (std::int16_t g = 1;
       g < static_cast<std::int16_t>(state.scenario.governments.size());
       ++g) {
    if (game::NovaGovernment_AreGovtsAllied(state.scenario, 0, g)) {
      if (ally == -1)
        ally = g;
    } else if (foe == -1) {
      foe = g;
    }
  }
  REQUIRE(foe != -1);

  // 0x80..0x17f licenses: own govt or an ally only; independent (govt -1) is
  // denied. Target govt = 0 -> require_govt 0x80.
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x80, 0));
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0x80, -1));
  if (ally != -1)
    CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x80, ally));
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0x80, foe));

  // 0x468..0x567 conditional: own govt, independent, or an ally.
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x468, 0));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x468, -1));
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0x468, foe));

  // 0x850..0x94f contraband: anything except own govt or an ally.
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0x850, 0));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x850, -1));
  if (ally != -1)
    CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0x850, ally));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0x850, foe));

  // 0xc38..0xd37 strict contraband: denied for own govt, independent, or ally.
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0xc38, 0));
  CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0xc38, -1));
  if (ally != -1)
    CHECK_FALSE(NovaLanded_RequireGovtAllows(state.scenario, 0xc38, ally));
  CHECK(NovaLanded_RequireGovtAllows(state.scenario, 0xc38, foe));
}

// Integration through the real purchase gate: Earth (0x80) belongs to
// government 0 (Federation), so the outfit field alone flips the decision.
TEST_CASE("Outfitter gate enforces RequireGovt at the landed stellar",
          "[landed_store][outfitter][government]") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // the starter Shuttle
  state.player.credits = 10'000'000;

  auto *outfit =
      const_cast<game::Outfit *>(state.scenario.Outfit(0x80)); // Light Blaster
  REQUIRE(outfit != nullptr);

  outfit->require_govt = 0x80; // license band, target govt 0
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x80));

  outfit->require_govt = 0xc38; // strict contraband: denied at govt 0
  CHECK_FALSE(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x80));

  outfit->require_govt = -1; // applies everywhere again
  CHECK(game::NovaLanded_CanBuyOutfit(state, 0x80, 0x80));
}
