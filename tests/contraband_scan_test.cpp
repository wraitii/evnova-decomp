#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>

#include "game/game_state.hpp"
#include "game/government.hpp"
#include "game/nova_random.hpp"
#include "game/outfit.hpp"
#include "game/scenario_data.hpp"
#include "game/ship_ai.hpp"
#include "game/travel.hpp"

namespace {

using game::ActiveMission;
using game::GameState;
using game::Government;
using game::NovaShip_ScanPlayerForContraband;
using game::Outfit;
using game::RandomBelow;
using game::ScenarioData;
using game::Ship;

// Minimal but complete state: one government with a nonzero ScanMask and
// smuggling penalty, one ship class for the message prefix, and the player at
// the origin with 10,000 credits.
void MakeBasic(GameState &state) {
  Government gov;
  gov.smug_penalty = 10;
  gov.scan_mask = 0x0001;
  gov.scan_fine = 0;
  state.scenario.governments = {gov};
  state.scenario.ships.resize(1);
  state.scenario.ships[0].display_name = "Interceptor";
  state.scenario.pers_defs.clear();
  state.scenario.systems.resize(1);
  state.scenario.systems[0].government_id = 0;
  state.system_reputation.assign(1, 0);
  state.scenario.outfits.assign(2, Outfit{});
  state.scenario.junk_defs.assign(2, game::JunkDef{});

  state.player.ship_name = "Player Ship";
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.player.current_system_id = 0;
  state.player.credits = 10000;
  state.player.is_active = true;
  state.player.ship_class_id = 0;
  // A live player must not read as destroyed (armor <= 0) or the AI state
  // machine clears its primary target before the state-7 scan branch.
  state.player.armor_points = 100.0F;
}

Ship MakeScanner() {
  Ship ship;
  ship.ai_behavior_code = 3; // warship
  ship.faction_or_government_id = 0;
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.pers_def_slot = -1;
  ship.ship_class_id = 0;
  return ship;
}

// First [0,100) draw for a seed, so tests can pin the scanner's single RNG
// gate.
std::uint32_t FindSeed(bool pass) {
  for (std::uint32_t seed = 1;; ++seed) {
    std::mt19937 probe(seed);
    const int roll = std::uniform_int_distribution<int>{0, 99}(probe);
    if ((roll <= 75) == pass) {
      return seed;
    }
  }
}

// Places an active mission in slot 0 whose ScanMask matches the government.
void AddMatchingMission(GameState &state, std::uint16_t mission_flags = 0) {
  ActiveMission &mission = state.active_missions[0];
  mission.scan_mask = 0x0001;
  mission.carrying_resources = true;
  mission.cargo_type_id = 0x80;
  mission.flags_primary = mission_flags;
  mission.mission_fleet_name = "Fleet";
  state.active_mission_runtime_flags[0].is_active = true;
  state.active_mission_runtime_flags[0].is_failed = false;
}

// A crime-sensitive rank (flags 0x0040 "any crime", allied government 0) that
// event 0 must deactivate, giving a deterministic observable for the
// reputation event independent of the flood's visibility setup.
void AddCrimeRank(GameState &state) {
  game::RankDef rank;
  rank.active = true;
  rank.defined = true;
  rank.flags = 0x0040;
  rank.government_id = 0;
  state.scenario.ranks = {rank};
}

} // namespace

TEST_CASE("contraband scan gates reject ineligible scans",
          "[government][contraband]") {
  GameState state;
  MakeBasic(state);
  AddMatchingMission(state);
  Ship scanner = MakeScanner();
  state.rng.seed(FindSeed(true));

  SECTION("non-warship behaviour") {
    scanner.ai_behavior_code = 1;
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK_FALSE(state.active_mission_runtime_flags[0].is_failed);
    CHECK(state.player.credits == 10000);
  }
  SECTION("government-less ship") {
    scanner.faction_or_government_id = -1;
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK(state.player.credits == 10000);
  }
  SECTION("zero smuggling penalty") {
    state.scenario.governments[0].smug_penalty = 0;
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK(state.player.credits == 10000);
  }
  SECTION("out of range") {
    scanner.pos_x = 101.0F;
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK(state.player.credits == 10000);
  }
  SECTION("RNG gate fails") {
    state.rng.seed(FindSeed(false));
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK(state.player.credits == 10000);
  }
}

TEST_CASE("contraband scan mission arm fines and percentage boundaries",
          "[government][contraband]") {
  GameState state;
  MakeBasic(state);
  AddMatchingMission(state);
  Ship scanner = MakeScanner();
  state.rng.seed(FindSeed(true));

  SECTION("flat positive fine") {
    state.scenario.governments[0].scan_fine = 25;
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK(state.player.credits == 9975);
    CHECK_FALSE(state.active_mission_runtime_flags[0].is_failed);
  }
  SECTION("percentage fine") {
    state.scenario.governments[0].scan_fine = -5; // 5% of 10,000
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK(state.player.credits == 9500);
  }
  SECTION("percentage fine truncates toward zero, minimum 1 credit") {
    state.player.credits = 1150; // 5% = 0.575 -> trunc 0 -> min 1
    state.scenario.governments[0].scan_fine = -5;
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK(state.player.credits == 1149);
  }
  SECTION("fine clamps credits at zero") {
    state.player.credits = 0;
    state.scenario.governments[0].scan_fine = -5;
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK(state.player.credits == 0);
  }
}

TEST_CASE("a scanning mission failure still falls through to the outfit arm",
          "[government][contraband]") {
  GameState state;
  MakeBasic(state);
  AddMatchingMission(state, 0x0020); // Mission fails if scanned
  state.scenario.governments[0].scan_fine = 50;
  state.inventory.outfit_owned_count[1] = 1;
  state.scenario.outfits[1].scan_mask = 0x0001;
  state.inventory.has_scannable_outfit = true;
  Ship scanner = MakeScanner();
  state.rng.seed(FindSeed(true));

  NovaShip_ScanPlayerForContraband(state, scanner, 0);
  // The mission-fail arm does not itself fine; the outfit arm does, and both
  // ran in a single call (the mission loop breaks, then the outfit scan runs).
  CHECK(state.active_mission_runtime_flags[0].is_failed);
  CHECK(state.player.credits == 9950);
}

TEST_CASE("outfit arm fines flat-positive only but always fires event 0",
          "[government][contraband]") {
  GameState state;
  MakeBasic(state);
  state.inventory.outfit_owned_count[1] = 1;
  state.scenario.outfits[1].scan_mask = 0x0001;
  state.inventory.has_scannable_outfit = true;
  Ship scanner = MakeScanner();
  state.rng.seed(FindSeed(true));

  state.scenario.governments[0].scan_fine = -5; // percentage arm is NOT used
  AddCrimeRank(state);
  NovaShip_ScanPlayerForContraband(state, scanner, 0);
  CHECK(state.player.credits == 10000);
  CHECK_FALSE(state.scenario.ranks[0].active); // event 0 fired

  state.player.credits = 10000;
  state.system_reputation.assign(1, 0);
  state.scenario.governments[0].scan_fine = 25;
  state.inventory.has_scannable_outfit = true;
  state.rng.seed(FindSeed(true));
  NovaShip_ScanPlayerForContraband(state, scanner, 0);
  CHECK(state.player.credits == 9975);
}

TEST_CASE("junk arm fines, fires event 0, and suppresses the outfit scan",
          "[government][contraband]") {
  GameState state;
  MakeBasic(state);
  // A matching illegal outfit would be found if the outfit arm ran.
  state.inventory.outfit_owned_count[1] = 1;
  state.scenario.outfits[1].scan_mask = 0x0001;
  state.inventory.has_scannable_outfit = true;

  SECTION("percentage fine applies to junk") {
    state.scenario.governments[0].scan_fine = -5;
    state.inventory.junk_counts[1] = 1;
    state.scenario.junk_defs[1].scan_mask = 0x0001;
    state.inventory.has_scannable_junk = true;
    Ship scanner = MakeScanner();
    state.rng.seed(FindSeed(true));
    AddCrimeRank(state);
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK(state.player.credits == 9500);
    CHECK_FALSE(state.scenario.ranks[0].active); // event 0 fired
  }
  SECTION("non-matching junk still blocks the matching outfit scan") {
    state.scenario.governments[0].scan_fine = 25;
    state.inventory.junk_counts[1] = 1;
    state.scenario.junk_defs[1].scan_mask = 0x0002; // govt scans 0x0001 only
    state.inventory.has_scannable_junk = true;
    Ship scanner = MakeScanner();
    state.rng.seed(FindSeed(true));
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    CHECK(state.player.credits == 10000);
  }
}

TEST_CASE("contraband scan consumes exactly one RNG draw",
          "[government][contraband]") {
  GameState state;
  MakeBasic(state);
  AddMatchingMission(state);
  state.scenario.governments[0].scan_fine = 25;
  Ship scanner = MakeScanner();
  const std::uint32_t seed = FindSeed(true);
  state.rng.seed(seed);

  std::mt19937 probe(seed);
  (void)std::uniform_int_distribution<int>{0, 99}(probe); // scanner's draw
  const int expected_next = std::uniform_int_distribution<int>{0, 99}(probe);

  NovaShip_ScanPlayerForContraband(state, scanner, 0);
  CHECK(RandomBelow(state, 100) == expected_next);
}

TEST_CASE("state-7 escort scans the player through the AI state machine",
          "[government][contraband][integration]") {
  // NovaAi_UpdateShipState draws an unknown number of RNG values before the
  // state-7 scan, so try seeds until the scan's own 100-sided gate passes and
  // assert the wired side-effect (the scan ran from inside the state machine).
  bool fired = false;
  for (std::uint32_t seed = 1; seed <= 256 && !fired; ++seed) {
    GameState state;
    MakeBasic(state);
    AddMatchingMission(state);
    state.scenario.governments[0].scan_fine = 25;
    state.scenario.ships.resize(2);
    state.rng.seed(seed);

    Ship scanner = MakeScanner();
    scanner.is_active = true;
    scanner.ai_state_code = 7;
    scanner.primary_target_ship_slot = 0;
    scanner.ai_maneuver_timer_ms = 0.0F;
    scanner.pos_x = 10.0F;
    scanner.pos_y = 10.0F;

    game::NovaAi_UpdateShipState(state, scanner, 0);
    if (state.player.credits == 9975) {
      fired = true;
      CHECK(scanner.ai_state_code == 4);
    }
  }
  CHECK(fired);
}

TEST_CASE("a scan clears its latch and only re-fires after a real recompute",
          "[government][contraband]") {
  GameState state;
  MakeBasic(state);
  state.scenario.governments[0].scan_fine = 25;
  state.inventory.outfit_owned_count[1] = 1;
  state.scenario.outfits[1].scan_mask = 0x0001;
  state.inventory.has_scannable_outfit = true;
  Ship scanner = MakeScanner();
  state.rng.seed(FindSeed(true));

  NovaShip_ScanPlayerForContraband(state, scanner, 0);
  CHECK(state.player.credits == 9975);
  CHECK_FALSE(state.inventory.has_scannable_outfit);

  // Ghidra 0x00401800 sets g_playerInventoryAndLoadoutDirty after the fine,
  // but that global's only consumer is NovaUi_RefreshGameplayPanels
  // (0x0045d320), which redraws the cargo/mission status panel. It does NOT
  // call Outfit_RecomputeOutfitDerivedState, so the cleared scan latch stays
  // clear: a second scan against the same inventory must not re-fire.
  state.pending_ui_sounds.clear();
  state.rng.seed(FindSeed(true));
  NovaShip_ScanPlayerForContraband(state, scanner, 0);
  CHECK(state.player.credits == 9975);
  CHECK(state.pending_ui_sounds.empty());

  // A real inventory mutation recomputes derived state and rebuilds the latch.
  NovaOutfit_RecomputeOutfitDerivedState(state);
  CHECK(state.inventory.has_scannable_outfit);
  state.rng.seed(FindSeed(true));
  NovaShip_ScanPlayerForContraband(state, scanner, 0);
  CHECK(state.player.credits == 9950);
}

TEST_CASE("scan voice cues map to the original transition-table slots",
          "[government][contraband]") {
  // g_nova_control_bits[164] (fine/warning) == g_transition_sound_handle_table
  // [4] == snd 154; [152] (mission failure) == table[1] == snd 151.
  // NovaAudio_PreloadGameplayData (0x004b0740) fills the table at
  // g_nova_control_bits+0x94+i*4 with NovaSound_LoadDecodedById(0x96+i).
  GameState state;
  MakeBasic(state);
  state.scenario.governments[0].scan_fine = 0;
  Ship scanner = MakeScanner();
  state.rng.seed(FindSeed(true));

  SECTION("mission failure uses [152] = table[1] = snd 151") {
    AddMatchingMission(state, 0x0020); // fails on scan -> message 0x11e
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    REQUIRE(state.pending_ui_sounds.size() == 1);
    CHECK(state.pending_ui_sounds[0].transition_index == 1);
  }
  SECTION("warning/fine uses [164] = table[4] = snd 154") {
    AddMatchingMission(state, 0); // warning arm
    NovaShip_ScanPlayerForContraband(state, scanner, 0);
    REQUIRE(state.pending_ui_sounds.size() == 1);
    CHECK(state.pending_ui_sounds[0].transition_index == 4);
  }
}

TEST_CASE("hyperspace-committed player is not scanned (NovaTravel_Tick)",
          "[government][contraband][integration]") {
  // The original guard gates on ShipState.ai_station_hold_timer > 0, the jump
  // hold clock. The port tracks the hold as TravelState::JumpPhase::kHold, so
  // drive a real engage through NovaTravel_Tick rather than touching the
  // dormant Ship field.
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.scenario.governments[0].smug_penalty = 10;
  state.scenario.governments[0].scan_mask = 0x0001;
  state.scenario.governments[0].scan_fine = 25;
  AddMatchingMission(state);

  state.player.current_system_id = 0;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F; // beyond the no-jump radius
  state.player.fuel_points = 500;
  state.player.armor_points = 100.0F;
  state.player.credits = 10000;
  state.player.ship_class_id = 0;
  state.cached_stats = game::Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(game::NovaTravel_PlotStarmapDestination(state, 1));

  game::NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.engaging);
  // Hold the 'Warp up' voice active so the jump cannot fire before the ramp
  // onset; advance to the stationary hold and then past the onset threshold.
  for (int f = 0; f < 1200 && state.travel.jump_phase !=
                                  game::TravelState::JumpPhase::kHold;
       ++f) {
    game::NovaTravel_Tick(state, false, 16.67F, /*warp_up_sound_active=*/true);
  }
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  for (int f = 0; f < 1200 && !game::NovaTravel_PlayerPastJumpOnset(state);
       ++f) {
    game::NovaTravel_Tick(state, false, 16.67F, true);
  }
  REQUIRE(game::NovaTravel_PlayerPastJumpOnset(state));
  REQUIRE_FALSE(state.travel.just_completed);

  Ship scanner = MakeScanner();
  scanner.pos_x = state.player.pos_x;
  scanner.pos_y = state.player.pos_y;
  state.rng.seed(FindSeed(true));
  NovaShip_ScanPlayerForContraband(state, scanner, 0);
  CHECK(state.player.credits == 10000);
  CHECK(state.pending_ui_sounds.empty());
}
