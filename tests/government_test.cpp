#include <catch2/catch_test_macros.hpp>

#include "game/government.hpp"
#include "game/scenario_data.hpp"

namespace {

using game::Government;
using game::NovaGovernment_AreGovtsAllied;
using game::NovaGovernment_AreGovtsHostileOrXenophobic;
using game::ScenarioData;

// Build a ScenarioData carrying just two governments with controllable
// flags/class/ally/enemy tables, indexed 0-based like the real table.
ScenarioData TwoGovts() {
  ScenarioData data;
  Government a;                     // index 0
  Government b;                     // index 1
  b.ally_classes = {0, -1, -1, -1}; // b allies class 0 (a's class)
  b.enemy_classes = {-1, -1, -1, -1};
  a.classes = {0, -1, -1, -1};
  data.governments = {a, b};
  return data;
}

TEST_CASE("allied helper matches a class against the other's allies",
          "[government][relation]") {
  const ScenarioData data = TwoGovts();
  // a (0) is class 0; b (1) allies class 0 -> allied.
  CHECK(NovaGovernment_AreGovtsAllied(data, 0, 1));
  CHECK(NovaGovernment_AreGovtsAllied(data, 1, 0));
  // Self is always allied.
  CHECK(NovaGovernment_AreGovtsAllied(data, 0, 0));
  CHECK(NovaGovernment_AreGovtsAllied(data, 1, 1));
}

// Ground truth from the real scenario data (see scenario_data_test.cpp: the
// Federation 0x80 flags 0xe2b0, classes {1}, enemy list {2,10,16,9}; govt 0x81
// classes {2}, enemies {1,12,...}; govt 0x84 class {4} allies {2,5}; govt 0x85
// class {5} allies {2,4}).
TEST_CASE("government relation helpers agree with the Federation scenario data",
          "[government][relation][scenario]") {
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());

  // Federation (id 0) vs govt 0x81 (id 1): not allied, hostile by enemy-class.
  CHECK(!NovaGovernment_AreGovtsAllied(data, 0, 1));
  CHECK(NovaGovernment_AreGovtsHostileOrXenophobic(data, 0, 1));

  // govt 0x84 (id 4) and govt 0x85 (id 5) share ally/class relations: allied,
  // not hostile.
  CHECK(NovaGovernment_AreGovtsAllied(data, 4, 5));
  CHECK(!NovaGovernment_AreGovtsHostileOrXenophobic(data, 4, 5));

  // Self-allied, self never hostile.
  CHECK(NovaGovernment_AreGovtsAllied(data, 0, 0));
  CHECK(!NovaGovernment_AreGovtsHostileOrXenophobic(data, 0, 0));
}

} // namespace
