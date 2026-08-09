#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/ship_spawn.hpp"

namespace {

using game::GameState;
using game::NovaEncounter_SpawnFleetLeadShip;
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

} // namespace
