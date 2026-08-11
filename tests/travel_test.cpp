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
using game::NovaTravel_CycleDestinationSystem;
using game::NovaTravel_MarkSystemDiscovered;
using game::NovaTravel_PlotStarmapDestination;
using game::NovaTravel_Tick;

// Returns whether the scenario marks zero-based `id` visible/explored and the
// pilot's explored bitset holds it.
bool IsDiscovered(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0 || static_cast<std::size_t>(zero_based_id) >=
                               state.scenario.systems.size()) {
    return false;
  }
  const std::size_t idx = static_cast<std::size_t>(zero_based_id);
  const auto &sys = state.scenario.systems[idx];
  return sys.is_visible && sys.has_explored_flag &&
         idx < state.control.explored_systems.size() &&
         state.control.explored_systems.test(idx);
}

} // namespace

// The discovery flood that runs on a completed jump must reveal the reached
// system AND its linked neighbours (the immediate neighbourhood the starmap
// shows as progressed), across both the scenario per-system flag and the
// pilot's explored bitset.
TEST_CASE("jump discovery reveals the destination and its neighbours") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  REQUIRE(!state.scenario.systems.empty());
  // Exhaustively: clear every system, then pick one with outward links and
  // confirm the flood marks it and each of those links.
  for (auto &sys : state.scenario.systems) {
    sys.is_visible = false;
    sys.has_explored_flag = false;
  }
  state.control.explored_systems.reset();

  const std::int16_t start = 0;
  const auto *const sys =
      state.scenario.System(static_cast<std::int16_t>(start + 0x80));
  REQUIRE(sys != nullptr);

  NovaTravel_MarkSystemDiscovered(state, start);

  // The reached system is revealed.
  CHECK(IsDiscovered(state, start));

  // Every outward link (stored as a system resource id >= 0x80) is revealed
  // too.
  for (const std::int16_t link : sys->links) {
    if (link < 0x80) {
      continue;
    }
    CHECK(IsDiscovered(state, static_cast<std::int16_t>(link - 0x80)));
  }

  // A system that was neither reached nor linked stays hidden.
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
      CHECK_FALSE(IsDiscovered(state, id));
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
  NovaTravel_MarkSystemDiscovered(state, -5);
  NovaTravel_MarkSystemDiscovered(
      state, static_cast<std::int16_t>(state.scenario.systems.size() + 10));
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

  // Directly-linked destination: arms the travel slot.
  REQUIRE(NovaTravel_PlotStarmapDestination(state, linked_dest));
  CHECK(state.travel.starmap_destination_system_id == linked_dest);
  CHECK(state.travel.travel_slot >= 0);
  CHECK(state.travel.selected_stellar_is_manual);

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

  // Pressing 'j' to completion must land in Tichel (system id 1). The hold
  // lasts the warp-up cue (fallback 6 s in tick-only tests) plus the ~2.5 s
  // in-tunnel coast, so allow ~13 s of frames.
  state.player.fuel_points = 500;
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
// vector) and brake the velocity to a stop before the tunnel fires -- the
// original's Ship_HandlePlayerShip travel_transfer_mode == 3 block
// (0x0044b120). The jump still completes to the plotted destination.
TEST_CASE("jump engages with a moving ship: turns around and brakes") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Kania (0) -> Tichel (1); give the ship a healthy outward drift so the
  // turn-around actually has something to brake.
  state.player.current_system_id = 0;
  state.player.pos_x = 300.0F;
  state.player.pos_y = 300.0F;
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

  // Let the whole sequence run; it must still land in Tichel. The brake takes
  // ~345 frames to slow |vel| 8 -> 0.5 at 0.992/frame, the alignment hold a
  // fixed 550 ms, and the zoom 700 ms: ~1.5 s total (much faster than the old
  // ~13 s cue/tunnel cadence).
  for (int f = 0; f < 800 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.current_system_id == 1);
}

TEST_CASE("jump starts Warp up when alignment hands off to launch") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));

  // The stopped handoff only enters the alignment hold.
  NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  REQUIRE(state.travel.engaging);
  NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kHold);
  CHECK_FALSE(state.warp_up_sound_pending);
  CHECK_FALSE(state.travel.warp_up_started);

  // The sound is latched precisely when turn completion starts the zoom.
  bool cue_latched = false;
  for (int f = 0; f < 240 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
    if (state.warp_up_sound_pending) {
      cue_latched = true;
      state.warp_up_sound_pending = false;
      break;
    }
  }
  REQUIRE(cue_latched);
  CHECK(state.travel.warp_up_started);
  CHECK_FALSE(state.warp_up_sound_pending);
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kWarmup);

  // The longer zoom then lands the boom/arrival.
  bool flash_armed = false;
  bool warp_out_latched = false;
  for (int f = 0; f < 420 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
    if (state.screen_flash_intensity > 0.0F) {
      flash_armed = true;
    }
    if (state.warp_out_sound_pending) {
      warp_out_latched = true;
    }
  }
  CHECK(state.travel.just_completed);
  CHECK(flash_armed);
  CHECK(warp_out_latched);
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

// The zoom advances the ship at increasing speed along the jump heading
// (the origin system parallaxes away) and the jump completes AT the end of
// the zoom with the arrival already in the new system; there is no separate
// in-tunnel coast phase after the fire.
TEST_CASE("jump zoom accelerates the ship then arrives (no post-fire tunnel)") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0;
  state.player.fuel_points = 500;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
  REQUIRE(NovaTravel_PlotStarmapDestination(state, 1));
  // Populate the effective-stats cache the spaceflight loop maintains so the
  // zoom's thrust (PlayerThrust) is nonzero (cached_stats defaults to 0).
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;

  // Burn through the hold to the zoom.
  for (int f = 0; f < 200 && state.travel.jump_phase !=
                                 game::TravelState::JumpPhase::kZoom;
       ++f) {
    NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  }
  REQUIRE(state.travel.jump_phase == game::TravelState::JumpPhase::kZoom);
  REQUIRE(state.travel.engaging);
  REQUIRE(state.player.current_system_id == 0); // still in the ORIGIN system

  const float x0 = state.player.pos_x;
  const float y0 = state.player.pos_y;
  for (int f = 0; f < 20; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  // The ship is actually accelerating forward: position advances and the
  // heading points along the jump bearing, so the origin system falls away.
  CHECK(std::hypot(state.player.pos_x - x0, state.player.pos_y - y0) > 1.0F);
  const float align_delta = std::abs(
      std::remainder(state.travel.jump_heading_rad - state.player.heading,
                     6.28318530717958646F));
  CHECK(align_delta < 0.5F);

  // The zoom ends in the boom/arrival: the jump completes at the fire, already
  // coiling into the NEW system at max speed.
  for (int f = 0; f < 200 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/false, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.current_system_id == 1);
  CHECK_FALSE(state.travel.engaging);
  CHECK(state.travel.jump_phase == game::TravelState::JumpPhase::kIdle);
  // Arrived at max speed along the jump heading.
  const float arrival_speed =
      std::hypot(state.player.vel_x, state.player.vel_y);
  CHECK(arrival_speed > 1.0F);
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
  // Alignment and the longer zoom precede the fire.
  for (int f = 0; f < 420 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
    if (state.screen_flash_intensity > 0.0F) {
      flash_armed = true;
    }
  }
  CHECK(state.travel.just_completed);
  CHECK(flash_armed);
}
