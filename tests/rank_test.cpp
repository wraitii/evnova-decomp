#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/government.hpp"
#include "game/rank.hpp"
#include "game/scenario_data.hpp"

namespace {

using game::GameState;
using game::Government;
using game::Rank_Activate;
using game::Rank_Deactivate;
using game::Rank_HighestWeightedActiveSlot;
using game::Rank_HighestWeightedActiveSlotForGovernment;
using game::RankDef;

// Scenario with 0x80 empty rank slots and ids pre-assigned, matching what
// ScenarioData::LoadFromArchives and Ship_InitGameplayDataTables do.
GameState RankState() {
  GameState state;
  state.scenario.ranks.assign(0x80, {});
  for (std::size_t i = 0; i < state.scenario.ranks.size(); ++i) {
    state.scenario.ranks[i].id = static_cast<std::int16_t>(i);
  }
  return state;
}

TEST_CASE("rank activate clears same-government siblings", "[rank][activate]") {
  GameState state = RankState();
  auto &ranks = state.scenario.ranks;
  // Slot 0: flag 0x0001 (clear same-govt on activate), weight 1, govt 0.
  ranks[0].defined = true;
  ranks[0].government_id = 0;
  ranks[0].weight = 1;
  ranks[0].flags = 0x0001;
  // Same government, higher weight, non-permanent -> cleared.
  ranks[1].defined = true;
  ranks[1].government_id = 0;
  ranks[1].weight = 5;
  ranks[1].active = true;
  // Same government, non-permanent -> cleared.
  ranks[2].defined = true;
  ranks[2].government_id = 0;
  ranks[2].active = true;
  // Different government -> kept.
  ranks[3].defined = true;
  ranks[3].government_id = 1;
  ranks[3].weight = 5;
  ranks[3].active = true;
  // Same government but permanent (0x0008) -> kept.
  ranks[4].defined = true;
  ranks[4].government_id = 0;
  ranks[4].weight = 9;
  ranks[4].flags = 0x0008;
  ranks[4].active = true;

  Rank_Activate(state, 0);
  CHECK(ranks[0].active);
  CHECK_FALSE(ranks[1].active);
  CHECK_FALSE(ranks[2].active);
  CHECK(ranks[3].active);
  CHECK(ranks[4].active);
  CHECK(state.recently_activated_rank_id == 0);

  // Deactivate slot 0 (flag 0x0001 is activation-side; nothing sibling-side).
  Rank_Deactivate(state, 0);
  CHECK_FALSE(ranks[0].active);
  CHECK_FALSE(ranks[1].active); // stays cleared
  CHECK(ranks[3].active);
  CHECK(ranks[4].active);
  CHECK(state.recently_activated_rank_id == -1);
}

TEST_CASE("rank lower-weight activate/deactivate flags", "[rank]") {
  GameState state = RankState();
  auto &ranks = state.scenario.ranks;
  // Slot 0: weight 5, activation flag 0x0010 (clear lower-weight siblings).
  ranks[0].defined = true;
  ranks[0].government_id = 0;
  ranks[0].weight = 5;
  ranks[0].flags = 0x0010;
  ranks[1].defined = true;
  ranks[1].government_id = 0;
  ranks[1].weight = 3; // lower -> cleared
  ranks[1].active = true;
  ranks[2].defined = true;
  ranks[2].government_id = 0;
  ranks[2].weight = 5; // equal -> kept (strict <)
  ranks[2].active = true;
  ranks[3].defined = true;
  ranks[3].government_id = 0;
  ranks[3].weight = 7; // higher -> kept
  ranks[3].active = true;

  Rank_Activate(state, 0);
  CHECK(ranks[0].active);
  CHECK_FALSE(ranks[1].active);
  CHECK(ranks[2].active);
  CHECK(ranks[3].active);

  // Deactivation side: slot 0 now active; make it clear lower-weight siblings.
  ranks[0].flags = 0x0020;
  Rank_Deactivate(state, 0);
  CHECK_FALSE(ranks[0].active);
  CHECK(ranks[2].active); // equal weight stays
  CHECK(ranks[3].active); // higher weight stays
}

TEST_CASE("rank name scans pick the highest weight and follow recent",
          "[rank]") {
  GameState state = RankState();
  auto &ranks = state.scenario.ranks;
  ranks[0].defined = true;
  ranks[0].weight = 1;
  ranks[0].conv_name = "Commander";
  ranks[0].short_name = "Cdr";
  ranks[0].active = true;
  ranks[1].defined = true;
  ranks[1].weight = 5;
  ranks[1].conv_name = "Ambassador";
  ranks[1].short_name = "Amb";
  ranks[1].active = true;
  ranks[2].defined = true;
  ranks[2].weight = 9; // higher weight but no names -> ignored
  ranks[2].active = true;

  CHECK(Rank_HighestWeightedActiveSlot(state, false) == 1);
  CHECK(Rank_HighestWeightedActiveSlot(state, true) == 1);

  // Per-government scan: slot 0 is government 0, slot 1 government 3.
  ranks[0].government_id = 0;
  ranks[1].government_id = 3;
  CHECK(Rank_HighestWeightedActiveSlotForGovernment(state, 0, false) == 0);
  CHECK(Rank_HighestWeightedActiveSlotForGovernment(state, 3, false) == 1);
  CHECK(Rank_HighestWeightedActiveSlotForGovernment(state, 0, true) == 0);
  CHECK(Rank_HighestWeightedActiveSlotForGovernment(state, 3, true) == 1);
  CHECK(Rank_HighestWeightedActiveSlotForGovernment(state, 7, false) == -1);
}

// Crime revocation loop in Government_ProcessFactionCombatEvent: allied ranks
// marked any-crime (0x0040) or disable/kill (0x0004) are deactivated; a
// permanent rank is kept. The event flood is a no-op with an out-of-range
// system id, isolating the revocation pass.
TEST_CASE("faction combat event revokes crime-sensitive ranks",
          "[rank][government]") {
  GameState state = RankState();
  Government a; // government index 0
  Government b; // government index 1
  a.classes = {5, -1, -1, -1};
  b.ally_classes = {5, -1, -1, -1};
  state.scenario.governments.assign(0x100, {});
  state.scenario.governments[0] = a;
  state.scenario.governments[1] = b;

  auto &ranks = state.scenario.ranks;
  ranks[0].defined = true;
  ranks[0].government_id = 0;
  ranks[0].flags = 0x0040; // any crime
  ranks[0].active = true;
  ranks[1].defined = true;
  ranks[1].government_id = 0;
  ranks[1].flags = 0x0004; // disable/kill only
  ranks[1].active = true;
  ranks[2].defined = true;
  ranks[2].government_id = 0;
  ranks[2].flags = 0x0008; // permanent
  ranks[2].active = true;

  // Event 0 (smuggle): only the any-crime rank is revoked.
  game::NovaGovernment_ProcessFactionCombatEvent(state, -1, 1, 0, -1);
  CHECK_FALSE(ranks[0].active);
  CHECK(ranks[1].active);
  CHECK(ranks[2].active);

  // Event 3 (kill): the disable/kill rank is revoked too.
  game::NovaGovernment_ProcessFactionCombatEvent(state, -1, 1, 3, -1);
  CHECK_FALSE(ranks[1].active);
  CHECK(ranks[2].active);
}

// The propagate flood: a kill event on system 0 with faction == system
// government applies the full kill penalty there and the adjacency recursion
// applies 0.65x on the linked system.
TEST_CASE("faction combat event floods reputation through adjacency",
          "[rank][government][propagate]") {
  GameState state;
  state.scenario.governments.assign(0x100, {});
  state.scenario.governments[0].kill_penalty = 7;
  state.scenario.ranks.assign(0x80, {});
  state.scenario.systems.assign(2, {});
  state.scenario.systems[0].is_visible = true;
  state.scenario.systems[0].government_id = 0;
  state.scenario.systems[0].links[0] = 0x81; // linked resource id
  state.scenario.systems[1].is_visible = true;
  state.scenario.systems[1].government_id = 0;
  state.system_reputation.assign(2, 0);

  game::NovaGovernment_ProcessFactionCombatEvent(state, 0, 0, 3, -1);
  CHECK(state.system_reputation[0] == -7);
  // 7 * 0.65 = 4.55, rounded away from zero.
  CHECK(state.system_reputation[1] == -5);
}

} // namespace
