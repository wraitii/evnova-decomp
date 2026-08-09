#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/ship_spawn.hpp"

#include <algorithm>

namespace {

using game::GameState;
using game::NovaEncounter_SelectFleetDefWeighted;
using game::NovaEncounter_SpawnFleetLeadShip;
using game::NovaEncounter_TrySpawnRandomFleet;
using game::NovaShip_AllocateShipSlot;

TEST_CASE("allocator takes the first free slot below the reserved tail") {
  GameState state;
  state.ShipAt(1).is_active = true; // occupy slot 1
  state.ShipAt(2).is_active = true; // occupy slot 2

  const int slot =
      NovaShip_AllocateShipSlot(state, /*system_id=*/0x88, /*reserved_tail=*/8);
  // Slots 1 and 2 are taken; the first free slot from 1..55 is 3.
  REQUIRE(slot == 3);
}

TEST_CASE("reserved tail shrinks the allocatable range") {
  GameState state;
  // Reserve 62 tail slots: free_limit = 0x40-62 = 2, so only slot 1 lies in the
  // scan range [1, 2).
  const int slot = NovaShip_AllocateShipSlot(state, 0, /*reserved_tail=*/62);
  REQUIRE(slot == 1);

  // Slot 1 is now taken and no more are free within [1, 2).
  REQUIRE(NovaShip_AllocateShipSlot(state, 0, 62) == -1);
}

TEST_CASE("reserved tail consuming the whole array returns -1") {
  GameState state;
  // free_limit = 0x40 - 63 = 1, so the scan body never runs (1 < 1 is false).
  REQUIRE(NovaShip_AllocateShipSlot(state, 0, 63) == -1);
  // reserved_tail >= 0x40 also leaves nothing to scan.
  REQUIRE(NovaShip_AllocateShipSlot(state, 0, 64) == -1);
  REQUIRE(NovaShip_AllocateShipSlot(state, 0, 65) == -1);
}

TEST_CASE("allocated slot is baselined for the requested system") {
  GameState state;
  const int slot = NovaShip_AllocateShipSlot(state,
                                             /*system_id=*/0x8000 - 0x80 + 5,
                                             /*reserved_tail=*/8);
  REQUIRE(slot != -1);
  const game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  CHECK(ship.is_active);
  CHECK(ship.current_system_id == 0x8000 - 0x80 + 5);
  CHECK(ship.dude_class_id == -1);
  CHECK(ship.ship_class_id == 0);
  CHECK(ship.ai_behavior_code == 1);
  CHECK(ship.faction_or_government_id == -1);
  CHECK(ship.speed == Catch::Approx(0.0F));
  CHECK(ship.ai_state_code == 0);
  CHECK(ship.ai_control_mode == 0);
  CHECK(ship.primary_target_ship_slot == -1);
  CHECK(ship.ai_target_ship_slot == -1);
  CHECK(ship.mission_fleet_slot == -1);
  CHECK(ship.credits == 0);
  CHECK(ship.death_timer_active == Catch::Approx(0.0F));
  // Position scatter lands in [-750, 750).
  CHECK(ship.pos_x >= -750.0F);
  CHECK(ship.pos_x < 750.0F);
  CHECK(ship.pos_y >= -750.0F);
  CHECK(ship.pos_y < 750.0F);
}

TEST_CASE("allocator never touches the player slot (index 0)") {
  GameState state;
  state.player.ship_class_id = 42;
  // Reserve a range that only leaves slot 1 free, then exhaust it; the player
  // slot must never be taken as an allocatable NPC slot.
  REQUIRE(NovaShip_AllocateShipSlot(state, 0, /*reserved_tail=*/62) == 1);
  // All allocatable slots are taken; player slot 0 must still be ours.
  const game::Ship &player = state.ShipAt(0);
  CHECK(player.ship_class_id == 42);
  CHECK(player.is_active == false); // the player's activation is set elsewhere
}

// The fleet lead-ship spawner lays a fleet def's lead onto an allocated slot.
TEST_CASE("fleet lead spawner shapes the ship from the fleet def") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Fleet 0x80 (Confederacy patrol): lead ship class 13, govt 0. Mark it
  // available so the spawner accepts it.
  auto &def = state.scenario.fleets[0x80 - 0x80];
  def.is_available_runtime = true;

  const int slot =
      NovaEncounter_SpawnFleetLeadShip(state, /*system_id=*/0x88, 0x80 - 0x80);
  REQUIRE(slot != -1);
  const game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  CHECK(ship.is_active);
  CHECK(ship.current_system_id == 0x88);
  // ShipState stores the zero-based ShipClassDef index, matching Ghidra.
  CHECK(ship.ship_class_id == 13);
  CHECK(ship.faction_or_government_id == 0);
  // AI behavior takes the lead ship class's default.
  const game::ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  REQUIRE(cls != nullptr);
  CHECK(ship.ai_behavior_code == cls->default_ai_behavior);
  CHECK(ship.shield_points ==
        Catch::Approx(static_cast<float>(cls->base_shield)));
  CHECK(ship.armor_points ==
        Catch::Approx(static_cast<float>(cls->base_armor)));
  CHECK(ship.mission_fleet_slot == -1);
  CHECK(ship.mission_ship_slot == -1);
  CHECK(ship.jump_destination_stellar_id == -2);
  CHECK(ship.credits == 0);
  // Position/heading are the neutral defaults for now (AI-entry deferred).
  CHECK(ship.pos_x == Catch::Approx(0.0F));
  CHECK(ship.pos_y == Catch::Approx(0.0F));
  CHECK(ship.ai_state_code == 0);
}

TEST_CASE("fleet lead spawner rejects unavailable / lead-less defs") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Fleet 0x80 not marked available -> refused.
  state.scenario.fleets[0x80 - 0x80].is_available_runtime = false;
  CHECK(NovaEncounter_SpawnFleetLeadShip(state, 0x88, 0x80 - 0x80) == -1);

  // A fleet def with no lead ship (lead -1 = "no fleet") -> refused.
  auto &empty = state.scenario.fleets[0x90 - 0x80];
  empty.is_available_runtime = true;
  empty.lead_ship_class_id = -1;
  CHECK(NovaEncounter_SpawnFleetLeadShip(state, 0x88, 0x90 - 0x80) == -1);
}

// EncounterFleet_SelectRandomEncounterFleetDefWeighted (0x0046b6d0) clean-room
// counterpart: NovaEncounter_SelectFleetDefWeighted. A system with no bound
// encounter fleets yields no candidate.
TEST_CASE("weighted fleet select returns -1 when no fleets are bound") {
  GameState state;
  state.scenario.systems.push_back(game::System{}); // index 0 = system id 0x80
  const game::System &s = state.scenario.systems[0];
  CHECK(s.encounter_fleet_count == 0);
  CHECK(s.encounter_chance_percent == 0);
  CHECK(NovaEncounter_SelectFleetDefWeighted(s, state.scenario, state.rng) ==
        -1);
}

TEST_CASE("weighted fleet select picks among available bound defs") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Build a synthetic System binding three fleet defs (ids 0/1/2) with weights
  // 10/20/70. The caller passes a System by value here (pure selection does not
  // mutate scenario state).
  game::System sys;
  sys.encounter_fleet_count = 3;
  sys.encounter_fleet_ids = {0, 1, 2};
  sys.encounter_fleet_weights = {10, 20, 70};

  // Mark the three target fleet defs available with valid leads.
  for (const int idx : {0, 1, 2}) {
    auto &def = state.scenario.fleets[static_cast<std::size_t>(idx)];
    def.is_available_runtime = true;
    if (def.lead_ship_class_id < 0) {
      def.lead_ship_class_id = 0;
    }
  }

  // Distribution sanity: with weights 10/20/70 the 70-weight def (pick 2)
  // should dominate. The rng seed is fixed so the run is reproducible.
  std::mt19937 rng(12345);
  int hits0 = 0, hits1 = 0, hits2 = 0;
  constexpr int kDraws = 2000;
  for (int i = 0; i < kDraws; ++i) {
    const int pick =
        NovaEncounter_SelectFleetDefWeighted(sys, state.scenario, rng);
    REQUIRE(pick >= 0);
    REQUIRE(pick < 3);
    if (pick == 0)
      ++hits0;
    if (pick == 1)
      ++hits1;
    if (pick == 2)
      ++hits2;
  }
  INFO("hits 0/1/2 = " << hits0 << "/" << hits1 << "/" << hits2);
  CHECK(hits0 + hits1 + hits2 == kDraws);
  CHECK(hits2 > hits0);
  CHECK(hits2 > hits1);
}

TEST_CASE("weighted fleet select skips unavailable / lead-less defs") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  game::System sys;
  sys.encounter_fleet_count = 3;
  sys.encounter_fleet_ids = {0, 1, 2};
  sys.encounter_fleet_weights = {10, 20, 70};

  // Only fleet 2 is available; fleets 0 and 1 are filtered from the bucket.
  state.scenario.fleets[2].is_available_runtime = true;
  if (state.scenario.fleets[2].lead_ship_class_id < 0) {
    state.scenario.fleets[2].lead_ship_class_id = 0;
  }
  state.scenario.fleets[0].is_available_runtime = false;
  state.scenario.fleets[1].is_available_runtime = false;

  std::mt19937 rng(77);
  int hits = 0;
  constexpr int kDraws = 50;
  for (int i = 0; i < kDraws; ++i) {
    const int pick =
        NovaEncounter_SelectFleetDefWeighted(sys, state.scenario, rng);
    CHECK(pick == 2); // only eligible candidate
    if (pick == 2)
      ++hits;
  }
  CHECK(hits == kDraws);
}

TEST_CASE("weighted fleet select returns -1 when no candidate is available") {
  GameState state;
  state.scenario.systems.push_back(game::System{});
  auto &sys = state.scenario.systems[0];
  sys.encounter_fleet_count = 2;
  sys.encounter_fleet_ids = {3, 4};
  sys.encounter_fleet_weights = {50, 50};
  // Both bound defs remain unavailable -> no eligible candidate.
  std::mt19937 rng(99);
  CHECK(NovaEncounter_SelectFleetDefWeighted(sys, state.scenario, rng) == -1);
}

// EncounterFleet_TrySpawnRandomEncounterFleet (0x00425280) clean-room
// counterpart: NovaEncounter_TrySpawnRandomFleet. With every fleet-def slot
// marked eligible (valid lead, available, matching a government-specific
// filter), any draw lands on an eligible def so the spawn is deterministic.
TEST_CASE("try-spawn picks an eligible def matching the system government") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  // system index 0 == resource id 0x80; give it government id 5 (0-based).
  auto &sys = state.scenario.systems[0];
  sys.government_id = 5;

  // Mark ALL 0x100 fleet slots eligible with lead 0 + filter "specific govt 5".
  for (auto &def : state.scenario.fleets) {
    def.lead_ship_class_id = 0;
    def.is_available_runtime = true;
    def.spawn_system_filter = 10000 + 5;
  }

  // Every draw lies inside the 0x100 range and lands on an eligible def, so
  // the spawn is deterministic (returns a slot, never -1).
  for (int i = 0; i < 20; ++i) {
    const int slot = NovaEncounter_TrySpawnRandomFleet(state, 0, false);
    REQUIRE(slot != -1);
    // The spawned ship carries the synthetic lead class 0.
    CHECK(state.ShipAt(static_cast<std::size_t>(slot)).ship_class_id == 0);
  }
}

// A filter naming a different government than the system's means no def is
// eligible, so the try-spawn always yields -1.
TEST_CASE("try-spawn refuses when no def matches the system government") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  auto &sys = state.scenario.systems[0];
  sys.government_id = 5;
  for (auto &def : state.scenario.fleets) {
    def.lead_ship_class_id = 0;
    def.is_available_runtime = true;
    def.spawn_system_filter = 10000 + 6; // only govt 6, not 5
  }
  for (int i = 0; i < 30; ++i) {
    CHECK(NovaEncounter_TrySpawnRandomFleet(state, 0, false) == -1);
  }
}

// The "anywhere" filter (-1) makes any available lead-valid def eligible.
TEST_CASE("try-spawn with anywhere filter spawns for any draw") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  auto &sys = state.scenario.systems[0];
  sys.government_id = 5;
  for (auto &def : state.scenario.fleets) {
    def.lead_ship_class_id = 0;
    def.is_available_runtime = true;
    def.spawn_system_filter = -1;
  }
  for (int i = 0; i < 20; ++i) {
    REQUIRE(NovaEncounter_TrySpawnRandomFleet(state, 0, false) != -1);
  }
}

// Ineligible defs (no lead, or unavailable) are never spawned even when the
// filter matches: with only lead-less/unavailable defs the draw always misses.
TEST_CASE("try-spawn skips lead-less and unavailable defs") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  auto &sys = state.scenario.systems[0];
  sys.government_id = 5;
  for (auto &def : state.scenario.fleets) {
    def.lead_ship_class_id = -1; // no lead
    def.is_available_runtime = true;
    def.spawn_system_filter = -1; // anywhere
  }
  for (int i = 0; i < 30; ++i) {
    CHECK(NovaEncounter_TrySpawnRandomFleet(state, 0, false) == -1);
  }
}

} // namespace
