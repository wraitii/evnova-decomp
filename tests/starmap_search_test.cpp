#include <catch2/catch_test_macros.hpp>

#include "game/starmap_internal.hpp"

#include <initializer_list>
#include <string>

namespace {

game::GameState MakeSearchState(std::initializer_list<std::string> names) {
  game::GameState state;
  state.scenario.systems.resize(names.size());
  std::size_t index = 0;
  for (const std::string &name : names) {
    game::System &system = state.scenario.systems[index++];
    system.name = name;
    system.is_visible = true;
    system.discovery_state = 1;
    system.discovered_this_rebuild = 1;
  }
  return state;
}

} // namespace

// Ghidra 0x004aab30 NovaUi_RunStarmapSearchDialog: names are lower-cased and
// stripped to [a-z0-9] before comparison.
TEST_CASE("starmap search normalizes names to lowercase alphanumerics",
          "[starmap][search]") {
  CHECK(game::starmap_detail::NormalizeSearchName("New Boston") == "newboston");
  CHECK(game::starmap_detail::NormalizeSearchName("R-7  Alpha!") == "r7alpha");
  CHECK(game::starmap_detail::NormalizeSearchName("") == "");
}

// The winner is the system sharing the longest normalized leading prefix.
TEST_CASE("starmap search prefers the longest name prefix",
          "[starmap][search]") {
  const game::GameState state =
      MakeSearchState({"New York", "New Boston", "Earth"});
  CHECK(game::starmap_detail::FindBestSystemMatch(state, "newb") == 1);
  CHECK(game::starmap_detail::FindBestSystemMatch(state, "newy") == 0);
  CHECK(game::starmap_detail::FindBestSystemMatch(state, "eart") == 2);
  CHECK(game::starmap_detail::FindBestSystemMatch(state, "zzz") == -1);
}

// Equal prefix depth is broken by the shorter normalized name.
TEST_CASE("starmap search tie-breaks on the shorter name",
          "[starmap][search]") {
  const game::GameState state = MakeSearchState({"Newer Boston", "New Boston"});
  CHECK(game::starmap_detail::FindBestSystemMatch(state, "new") == 1);
}

// A one-character query is only accepted when exactly one system matches.
TEST_CASE("starmap search rejects ambiguous single-character matches",
          "[starmap][search]") {
  const game::GameState ambiguous = MakeSearchState({"Alpha", "Aurora"});
  CHECK(game::starmap_detail::FindBestSystemMatch(ambiguous, "a") == -1);
  const game::GameState unique = MakeSearchState({"Alpha", "Beta"});
  CHECK(game::starmap_detail::FindBestSystemMatch(unique, "a") == 0);
}

// Candidates must be visible, visited, and latched by the last discovery
// rebuild (the original's three-way gate).
TEST_CASE("starmap search only sees visible visited systems",
          "[starmap][search]") {
  game::GameState state = MakeSearchState({"Sol", "Vega"});
  state.scenario.systems[0].discovery_state = 0; // unvisited
  state.scenario.systems[1].is_visible = false;
  CHECK(game::starmap_detail::FindBestSystemMatch(state, "sol") == -1);
  CHECK(game::starmap_detail::FindBestSystemMatch(state, "vega") == -1);
}
