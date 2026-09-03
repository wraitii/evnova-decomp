#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "game/game_state.hpp"
#include "game/outfit.hpp"
#include "game/scenario_data.hpp"
#include "game/travel.hpp"

namespace {

using game::GameState;
using game::NovaSystem_OnSystemEntered;
using game::NovaTravel_CycleDestinationSystem;
using game::NovaTravel_PlotStarmapDestination;
using game::NovaTravel_Tick;

// Returns whether zero-based `id` is visited (the per-system fog record):
// discovery_state > 0 mirrored to the pilot's explored bitset. (SystemDef
// is_visible/has_explored_flag are load-time flags, not fog.)
bool IsVisited(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0 || static_cast<std::size_t>(zero_based_id) >=
                               state.scenario.systems.size()) {
    return false;
  }
  const std::size_t idx = static_cast<std::size_t>(zero_based_id);
  const auto &sys = state.scenario.systems[idx];
  return sys.discovery_state > 0 &&
         idx < state.control.explored_systems.size() &&
         state.control.explored_systems.test(idx);
}

// Returns whether `id` appears on the map view without being visited (the
// discovered_this_rebuild latch for one-hop neighbours of visited systems).
bool IsRevealedOnly(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0 || static_cast<std::size_t>(zero_based_id) >=
                               state.scenario.systems.size()) {
    return false;
  }
  return !IsVisited(state, zero_based_id) &&
         state.scenario.systems[static_cast<std::size_t>(zero_based_id)]
             .discovered_this_rebuild;
}

} // namespace

// The discovery pass that runs on a completed jump must mark the reached
// system visited and latch its linked neighbours as revealed (the one-jump-
// ahead window the starmap shows) WITHOUT marking those neighbours explored —
// the original's arrival block floods depth 0 only
// (System_RebuildSystemVisibilityMap(cur, 0, 1)) and the neighbour window is
// the transient discovered_this_rebuild latch.
TEST_CASE(
    "jump discovery marks the destination visited and neighbours revealed") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  REQUIRE(!state.scenario.systems.empty());
  // Exhaustively: clear the fog record (discovery_state; the load-time
  // is_visible/has_explored_flag flags stay as decoded), then pick one system
  // with outward links and confirm only it is visited while each of those
  // links is merely revealed.
  for (auto &sys : state.scenario.systems) {
    sys.discovery_state = 0;
    sys.discovered_this_rebuild = false;
  }
  state.control.explored_systems.reset();

  const std::int16_t start = 0;
  const auto *const sys =
      state.scenario.System(static_cast<std::int16_t>(start + 0x80));
  REQUIRE(sys != nullptr);

  NovaSystem_OnSystemEntered(state, start, 1);

  // The reached system is visited.
  CHECK(IsVisited(state, start));

  // Every outward link (stored as a system resource id >= 0x80) shows on the
  // map but is NOT explored itself.
  for (const std::int16_t link : sys->links) {
    if (link < 0x80) {
      continue;
    }
    const auto dest = static_cast<std::int16_t>(link - 0x80);
    CHECK(IsRevealedOnly(state, dest));
    CHECK_FALSE(IsVisited(state, dest));
  }

  // A system that was neither reached nor linked stays fully hidden.
  bool found_unrelated = false;
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const std::int16_t id = static_cast<std::int16_t>(i);
    if (id == start) {
      continue;
    }
    bool linked = false;
    for (const std::int16_t link : sys->links) {
      if (link == static_cast<std::int16_t>(id + 0x80)) {
        linked = true;
        break;
      }
    }
    if (!linked) {
      CHECK_FALSE(IsVisited(state, id));
      CHECK_FALSE(IsRevealedOnly(state, id));
      found_unrelated = true;
    }
  }
  CHECK(found_unrelated);
}

// Marking an out-of-range / negative system is a safe no-op.
TEST_CASE("jump discovery guards out-of-range systems") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const std::size_t before = state.control.explored_systems.count();
  NovaSystem_OnSystemEntered(state, -5, 1);
  NovaSystem_OnSystemEntered(
      state, static_cast<std::int16_t>(state.scenario.systems.size() + 10), 1);
  CHECK(state.control.explored_systems.count() == before);
}

// Plotting a starmap destination arms the travel slot for a directly-linked
// system so 'j' jumps there, and does not arm a slot when the destination
// cannot be reached in a single jump.
TEST_CASE("starmap plot arms the travel slot only for linked destinations") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Pick a starting system with at least one outward link to a real system.
  std::int16_t start = -1;
  std::int16_t linked_dest = -1;
  std::int16_t unrelated = -1;
  for (std::size_t i = 0; i < state.scenario.systems.size() && linked_dest < 0;
       ++i) {
    const auto *sys =
        state.scenario.System(static_cast<std::int16_t>(i + 0x80));
    if (!sys) {
      continue;
    }
    for (const std::int16_t link : sys->links) {
      if (link >= 0x80 &&
          static_cast<std::size_t>(link - 0x80) <
              state.scenario.systems.size() &&
          link - 0x80 != static_cast<std::int16_t>(i)) {
        start = static_cast<std::int16_t>(i);
        linked_dest = static_cast<std::int16_t>(link - 0x80);
        break;
      }
    }
  }
  REQUIRE(start >= 0);
  REQUIRE(linked_dest >= 0);
  state.player.current_system_id = start;

  // Find a system that is neither start nor linked_dest, for the no-link case.
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const std::int16_t id = static_cast<std::int16_t>(i);
    if (id == start || id == linked_dest) {
      continue;
    }
    bool linked = false;
    const auto *start_sys =
        state.scenario.System(static_cast<std::int16_t>(start + 0x80));
    for (const std::int16_t link : start_sys->links) {
      if (link == static_cast<std::int16_t>(id + 0x80)) {
        linked = true;
        break;
      }
    }
    if (!linked) {
      unrelated = id;
      break;
    }
  }
  REQUIRE(unrelated >= 0);

  // Directly-linked destination: arms the travel slot. Mode-3 plotted jumps
  // clear the stellar selection (the reticle hides; the nav panel switches to
  // the Hyperspace display).
  REQUIRE(NovaTravel_PlotStarmapDestination(state, linked_dest));
  CHECK(state.travel.starmap_destination_system_id == linked_dest);
  CHECK(state.travel.travel_slot >= 0);
  CHECK_FALSE(state.travel.selected_stellar_is_manual);
  CHECK(state.travel.selected_stellar_id == -1);

  // Reset for the negative cases, then check a non-linked destination.
  state.travel.starmap_destination_system_id = -1;
  state.travel.travel_slot = -1;
  const bool not_armed = NovaTravel_PlotStarmapDestination(state, unrelated);
  CHECK(not_armed == false);
  CHECK(state.travel.starmap_destination_system_id == unrelated);
  CHECK(state.travel.travel_slot == -1);

  // Plotting the current system records no jump.
  state.travel.starmap_destination_system_id = -1;
  state.travel.travel_slot = -1;
  NovaTravel_PlotStarmapDestination(state, start);
  CHECK(state.travel.starmap_destination_system_id == start);
  CHECK(state.travel.travel_slot == -1);
}

// Regression: Kania (0) links to Tichel (1) at hyperlink slot 3, but has no
// travel-nav stellar there (nav_defs[3] is empty). A plotted jump to Tichel
// must still arm (destination resolved purely against links) and 'j' must land
// in Tichel, not fall back to the nearest travel point (which would go to a
// different system). This locked in the fix where PlotStarmapDestination no
// longer required a NavDef paired with the hyperlink slot.
TEST_CASE("plot to a hyperlink without a paired nav-def stellar still jumps") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Verify the scenario shape that exercises the fix: Kania links to Tichel at
  // a slot whose nav_defs entry is empty.
  state.player.current_system_id = 0; // Kania
  const game::System *kania = state.scenario.System(0x80);
  REQUIRE(kania != nullptr);
  int tichel_slot = -1;
  for (std::size_t i = 0; i < kania->links.size(); ++i) {
    if (kania->links[i] == 0x81 /* Tichel resource */) {
      tichel_slot = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(tichel_slot >= 0);
  CAPTURE(tichel_slot);
  REQUIRE(static_cast<std::size_t>(tichel_slot) < kania->nav_defs.size());
  // Historically this slot had an empty nav_def, exercising the no-paired-
  // NavDef branch; arm regardless.
  const bool armed = NovaTravel_PlotStarmapDestination(state, 1);
  REQUIRE(armed);
  CHECK(state.travel.starmap_destination_system_id == 1);
  CHECK(state.travel.travel_slot == tichel_slot);
  CHECK(state.travel.destination_system_id == 1);

  // Pressing 'j' to completion must land in Tichel (system id 1). The ship
  // starts beyond the no-jump radius so the engage passes; the brake is short
  // (velocity 0) and the fire lands once the hold passes 30 ticks with no
  // 'Warp up' voice active (~1 s of frames at 16.67 ms).
  state.player.fuel_points = 500;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F; // beyond the 1000 px no-jump radius
  for (int f = 0; f < 800 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.current_system_id == 1);
}

// The destination-system cycle (the Backslash / command-0x60 channel) must
// advance through the current system's directly-linked systems, wrapping, and
// arm the travel slot + destination so 'j' jumps there. On a system with no
// travelable links it must return -1 and clear the armed destination.
TEST_CASE("destination-system cycle steps and wraps") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Pick a starting system with at least two distinct outward links.
  std::int16_t start = -1;
  std::vector<std::int16_t> dests;
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const game::System *sys =
        state.scenario.System(static_cast<std::int16_t>(i + 0x80));
    if (!sys) {
      continue;
    }
    dests.clear();
    for (const std::int16_t link : sys->links) {
      if (link >= 0x80 &&
          static_cast<std::size_t>(link - 0x80) <
              state.scenario.systems.size() &&
          link - 0x80 != static_cast<std::int16_t>(i)) {
        dests.push_back(static_cast<std::int16_t>(link - 0x80));
      }
    }
    if (dests.size() >= 2) {
      start = static_cast<std::int16_t>(i);
      break;
    }
  }
  REQUIRE(start >= 0);
  REQUIRE(dests.size() >= 2);
  state.player.current_system_id = start;

  // First press selects the first destination (travel_slot was -1).
  const std::int16_t d0 =
      NovaTravel_CycleDestinationSystem(state, /*forward=*/true);
  REQUIRE(d0 == dests[0]);
  CHECK(state.travel.starmap_destination_system_id == d0);
  CHECK(state.travel.travel_slot >= 0);
  CHECK(state.travel.destination_system_id == d0);

  // Next press moves to the second destination (not back to the first).
  const std::int16_t d1 =
      NovaTravel_CycleDestinationSystem(state, /*forward=*/true);
  REQUIRE(d1 == dests[1]);
  CHECK(d1 != d0);

  // Backward wraps from the second to the first.
  const std::int16_t back =
      NovaTravel_CycleDestinationSystem(state, /*forward=*/false);
  REQUIRE(back == d0);

  // A system with no outward links clears the armed destination and returns -1.
  std::int16_t dead_end = -1;
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const game::System *sys =
        state.scenario.System(static_cast<std::int16_t>(i + 0x80));
    if (!sys) {
      continue;
    }
    bool has_link = false;
    for (const std::int16_t link : sys->links) {
      if (link >= 0x80 &&
          static_cast<std::size_t>(link - 0x80) <
              state.scenario.systems.size() &&
          link - 0x80 != static_cast<std::int16_t>(i)) {
        has_link = true;
        break;
      }
    }
    if (!has_link) {
      dead_end = static_cast<std::int16_t>(i);
      break;
    }
  }
  REQUIRE(dead_end >= 0);
  state.player.current_system_id = dead_end;
  const std::int16_t no_dest =
      NovaTravel_CycleDestinationSystem(state, /*forward=*/true);
  REQUIRE(no_dest == -1);
  CHECK(state.travel.starmap_destination_system_id == -1);
  CHECK(state.travel.travel_slot == -1);
}

// The pre-fire turn-around: engaging 'j' while the ship is still moving must
// turn the hull (toward the reverse of the velocity, i.e. back toward the jump
// vector) and brake the velocity to the |round(vel)| < 2 stop before the hold
// fires -- the original's jump dispatch in Ship_HandlePlayerShipCore
// (0x0044c195 / 0x0044fff0). The jump still completes to the plotted
// destination.
TEST_CASE("jump engages with a moving ship: turns around and brakes") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Kania (0) -> Tichel (1); give the ship a healthy outward drift so the
  // turn-around actually has something to brake, parked beyond the 1000 px
  // no-jump radius so the engage passes.
  state.player.current_system_id = 0;
  state.player.pos_x = 3000.0F;
  state.player.pos_y = 3000.0F;
  state.player.heading = 0.0F; // up
  state.player.vel_x = 8.0F;   // flying rightward (heading 90 deg)
  state.player.vel_y = 0.0F;
  state.player.fuel_points = 500;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  const float initial_speed =
      std::hypot(state.player.vel_x, state.player.vel_y);
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  // Engage and let the brake run: after a few frames the heading must
  // have changed (turning around) and the speed must have decayed.
  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.engaging);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  const float heading_after_engage = state.player.heading;
  for (int f = 0; f < 30; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.player.heading != heading_after_engage);
  CHECK(std::hypot(state.player.vel_x, state.player.vel_y) < initial_speed);

  // Let the whole sequence run; it must still land in Tichel. The brake needs
  // ~240 ticks to slow |vel| 8 -> the |round(vel)| < 2 stop at 0.99204/tick,
  // then the hold fires once it passes 30 ticks (~1 s) with no 'Warp up'
  // voice active in tick-only tests.
  for (int f = 0; f < 1200 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.current_system_id == 1);
}

// The stationary hold starts the 'Warp up' cue the moment the brake hands
// off (the original pre-stages the sound at the stop), then the fire lands
// once the hold passes the 30-tick engage threshold with the cue finished.
TEST_CASE("jump hold starts Warp up then fires past the engage threshold") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F; // beyond the no-jump radius
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  // The stopped handoff enters the hold and latches the cue immediately.
  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.engaging);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kBrake);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  CHECK(state.travel.warp_up_started);
  CHECK(state.warp_up_sound_pending);
  state.warp_up_sound_pending = false; // consumed by the audio loop

  // With no cue voice active the fire lands just past the 30-tick threshold
  // (0.5 ticks per 16.67 ms frame -> ~62 frames).
  for (int f = 0; f < 200 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.current_system_id == 1);
}

TEST_CASE("jump heading uses the linked systems' map vector") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 123456.0F;
  state.player.pos_y = -654321.0F;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  const auto *source = state.scenario.System(0x80);
  const auto *destination = state.scenario.System(0x81);
  REQUIRE(source != nullptr);
  REQUIRE(destination != nullptr);
  const float expected =
      std::atan2(static_cast<float>(destination->pos_x - source->pos_x),
                 -static_cast<float>(destination->pos_y - source->pos_y));
  CHECK(std::abs(std::remainder(state.travel.jump_heading_rad - expected,
                                6.28318530717958646F)) < 0.001F);
}

// The fire lands while the ship is still in the ORIGIN system and hurls it
// 1350 px from the destination center along the reverse of the jump bearing,
// moving at max speed along its heading; there is no separate in-tunnel coast
// phase after the fire.
TEST_CASE("jump fire hurls the ship 1350 px past the destination center") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F; // beyond the no-jump radius
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  const float max_speed = state.cached_stats.speed_raw / 100.0F;
  REQUIRE(max_speed > 0.0F);

  for (int f = 0; f < 400 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  }
  REQUIRE(state.travel.just_completed);
  REQUIRE(state.player.current_system_id == 1);
  CHECK_FALSE(state.travel.engaging);
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kIdle);

  // Arrived at max speed along the heading, exactly 1350 px from the
  // destination system center (g_hyperspace_engage_velocity_hurl).
  const float arrival_speed =
      std::hypot(state.player.vel_x, state.player.vel_y);
  CHECK(arrival_speed == Catch::Approx(max_speed).epsilon(0.01F));
  const auto *dest = state.scenario.System(0x81);
  REQUIRE(dest != nullptr);
  const float hurl = std::hypot(state.player.pos_x - dest->pos_x,
                                state.player.pos_y - dest->pos_y);
  CHECK(hurl == Catch::Approx(1350.0F).epsilon(0.01F));
}

// The fire moment must arm the full-screen flash (the original's centered
// effect 0x32, the 'boom' white frame). NovaTravel_Tick sets the intensity;
// the spaceflight loop decays it, so in this tick-only test it stays set.
TEST_CASE("jump fire arms the screen flash") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  bool flash_armed = false;
  // Engage with the ship beyond the no-jump radius; the brake + hold then
  // lead to the fire.
  state.player.pos_x = 0.0F;
  state.player.pos_y = -3000.0F;
  for (int f = 0; f < 420 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
    if (state.screen_flash_intensity > 0.0F) {
      flash_armed = true;
    }
  }
  CHECK(state.travel.just_completed);
  CHECK(flash_armed);
}
