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

TEST_CASE("hostile helper matches a class against the other's enemies",
          "[government][relation]") {
  ScenarioData data = TwoGovts();
  auto &b = data.governments[1];
  b.enemy_classes = {0, -1, -1, -1}; // b enemies class 0 (a's class)
  CHECK(NovaGovernment_AreGovtsHostileOrXenophobic(data, 0, 1));
  CHECK(NovaGovernment_AreGovtsHostileOrXenophobic(data, 1, 0));
  // Not hostile when no enemy / ally overlap.
  b.enemy_classes = {-1, -1, -1, -1};
  b.ally_classes = {-1, -1, -1, -1};
  CHECK(!NovaGovernment_AreGovtsHostileOrXenophobic(data, 0, 1));
  // Self is never hostile.
  CHECK(!NovaGovernment_AreGovtsHostileOrXenophobic(data, 0, 0));
}

TEST_CASE("derelict governments are excluded from relation checks",
          "[government][relation]") {
  ScenarioData data = TwoGovts();
  auto &a = data.governments[0];
  auto &b = data.governments[1];
  b.ally_classes = {0, -1, -1, -1};
  a.flags_primary |= 0x0800U; // a is derelict -> excluded
  // Even though b allies class 0, the derelict a is not checked against.
  CHECK(!NovaGovernment_AreGovtsAllied(data, 0, 1));
  CHECK(NovaGovernment_AreGovtsAllied(data, 1, 1)); // self still allied
}

TEST_CASE("xenophobic override marks hostile when not allied",
          "[government][relation]") {
  ScenarioData data = TwoGovts();
  auto &b = data.governments[1];
  // No ally/enemy overlap: not allied by class.
  b.ally_classes = {-1, -1, -1, -1};
  b.enemy_classes = {-1, -1, -1, -1};
  CHECK(!NovaGovernment_AreGovtsHostileOrXenophobic(data, 0, 1));
  // b becomes xenophobic -> hostile from sight, even with no class relation.
  b.flags_primary |= 0x0001U;
  CHECK(NovaGovernment_AreGovtsHostileOrXenophobic(data, 0, 1));
  // Unless they are allied: xenophobic does not override an alliance.
  b.ally_classes = {0, -1, -1, -1};
  CHECK(!NovaGovernment_AreGovtsHostileOrXenophobic(data, 0, 1));
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
