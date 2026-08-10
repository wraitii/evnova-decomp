#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/scenario_data.hpp"
#include "game/travel.hpp"

namespace {

using game::GameState;
using game::NovaTravel_MarkSystemDiscovered;
using game::NovaTravel_PlotStarmapDestination;
using game::NovaTravel_Tick;

// Returns whether the scenario marks zero-based `id` visible/explored and the
// pilot's explored bitset holds it.
bool IsDiscovered(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0 ||
      static_cast<std::size_t>(zero_based_id) >= state.scenario.systems.size()) {
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

  // Every outward link (stored as a system resource id >= 0x80) is revealed too.
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
    const auto *sys = state.scenario.System(static_cast<std::int16_t>(i + 0x80));
    if (!sys) {
      continue;
    }
    for (const std::int16_t link : sys->links) {
      if (link >= 0x80 &&
          static_cast<std::size_t>(link - 0x80) < state.scenario.systems.size() &&
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
  const bool not_armed =
      NovaTravel_PlotStarmapDestination(state, unrelated);
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

  // Pressing 'j' to completion must land in Tichel (system id 1).
  state.player.fuel_points = 500;
  for (int f = 0; f < 200 && !state.travel.just_completed; ++f) {
    NovaTravel_Tick(state, /*travel_input=*/true, 16.67F);
  }
  CHECK(state.travel.just_completed);
  CHECK(state.player.current_system_id == 1);
}
