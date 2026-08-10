#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/scenario_data.hpp"
#include "game/travel.hpp"

namespace {

using game::GameState;
using game::NovaTravel_MarkSystemDiscovered;

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
