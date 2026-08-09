#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/ship_spawn.hpp"

namespace {

using game::GameState;
using game::NovaEncounter_SpawnFleetLeadShip;
using game::NovaShip_AllocateShipSlot;

// Count active NPC ships in the current system (any AI target state). Used by
// the population tests below to assert ships actually spawned.
[[nodiscard]] int ActiveShipsInSystem(const GameState &state,
                                      std::int16_t system_id) {
  int n = 0;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const game::Ship &s = state.ShipAt(slot);
    if (s.is_active && s.current_system_id == system_id) {
      ++n;
    }
  }
  return n;
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

// System_TickNpcSpawnMaintenance (0x0041d6e0, ambience slice): over repeated
// full ticks the AvgShips cap for Tichel (system 0x81) is filled with dude
// ships. Tichel binds no encounter fleets, so the dude spawn dominates.
TEST_CASE("system maintenance populates Tichel toward avg_ships") {
  using game::NovaSystem_TickNpcSpawnMaintenance;
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0x81; // Tichel
  const auto *sys = state.scenario.System(0x81);
  REQUIRE(sys != nullptr);
  REQUIRE(sys->avg_ships > 0);
  REQUIRE(sys->encounter_fleet_count == 0); // dude-bound only

  // Tick many times; the maintenance should fill the system toward avg_ships.
  for (int i = 0; i < 4000; ++i) {
    NovaSystem_TickNpcSpawnMaintenance(state, 0x81);
  }
  const int spawned = ActiveShipsInSystem(state, 0x81);
  CHECK(spawned >= 1);
  // The maintenance should respect the avg_ships cap (allow a little slack
  // since the dude spawn loop may allocate slightly beyond on a lucky streak).
  CHECK(spawned <= sys->avg_ships + 2);
}

// Ship_DeactivateVacantShipsAndTally (0x0041ad50) clears the whole active NPC
// cohort of one system (slots 1..), leaving ships of other systems and the
// player (slot 0) untouched.
TEST_CASE("deactivate system ships clears only the target system's NPCs") {
  using game::NovaShip_DeactivateSystemShips;
  const std::int16_t target = 3;
  const std::int16_t other = 7;
  GameState st;
  // Two NPCs in the target system, one in another system (slot 0 stays the
  // player and is never cleared).
  (void)NovaShip_AllocateShipSlot(st, target, 0);
  (void)NovaShip_AllocateShipSlot(st, target, 0);
  (void)NovaShip_AllocateShipSlot(st, other, 0);

  NovaShip_DeactivateSystemShips(st, target);

  int active_target = 0, active_other = 0;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const game::Ship &s = st.ShipAt(slot);
    if (!s.is_active) {
      continue;
    }
    if (s.current_system_id == target) {
      ++active_target;
    } else if (s.current_system_id == other) {
      ++active_other;
    }
  }
  // All target-system NPC slots were cleared; the other-system ship remains.
  CHECK(active_target == 0);
  CHECK(active_other == 1);
}

// NovaRandom_Reseed (0x004ab970) reseeds the RNG so a fresh game draws a
// different spawn sequence than the default-42 pause.
TEST_CASE("reseed random changes the spawn stream") {
  using game::NovaGame_ReseedRandom;
  GameState a;
  GameState b;
  NovaGame_ReseedRandom(b);
  REQUIRE(a.rng != b.rng);
}

} // namespace
