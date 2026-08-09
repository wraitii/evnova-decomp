#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/scenario_data.hpp"
#include "game/ship_ai.hpp"
#include "game/ship_spawn.hpp"
#include "game/targeting.hpp"
#include "game/travel.hpp"

#include <algorithm>
#include <iterator>

namespace {

using game::GameState;
using game::NovaAi_UpdateShipAI;
using game::NovaShip_AllocateShipSlot;
using game::NovaTargeting_UpdateStellarAvailability;

// Find a zero-based system index whose nav list holds at least one stellar that
// is available (after the availability refresh), map-visible (< 1000) and
// travel-flagged -- a candidate the wander supervisor can select.
[[nodiscard]] int FindWanderSuitableSystem(GameState &state) {
  for (std::size_t sys_idx = 0; sys_idx < state.scenario.systems.size();
       ++sys_idx) {
    state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
    NovaTargeting_UpdateStellarAvailability(state);
    const auto *sys =
        state.scenario.System(static_cast<std::int16_t>(sys_idx + 0x80));
    if (!sys) {
      continue;
    }
    std::size_t usable = 0;
    for (const auto nav : sys->nav_defs) {
      if (nav < 0x80) {
        continue;
      }
      const game::Stellar *st = state.scenario.Stellar(nav);
      if (st && st->is_available && (st->flags & 1U) != 0U &&
          st->pos_x < 1000 && st->pos_y < 1000) {
        ++usable;
      }
    }
    if (usable >= 2) {
      return static_cast<int>(sys_idx);
    }
  }
  return -1;
}

} // namespace

// End-to-end wiring check for the Phase 3/4 wander milestone: a behavior-0x01
// NPC spawned idle in a system with travel points should, after one
// NovaAi_UpdateShipAI pass, either have entered state 1 (travel) with a valid
// stellar resource target, or settled into a defined fallback (2/6). This
// exercises the dispatcher + state machine + controls bridge against real
// scenario data (not a hand-built mock world).
TEST_CASE("behavior-0x01 spawn wanders to a travel stellar when idle") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0); // a wander-suitable real system exists
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);

  // Spawn a behaviour-0x01 wanderer directly (the fleet-lead spawner is
  // gated on fleet availability, which is system-dependent and not what we
  // are testing here). Use a simple, well-known starter class (0x80) with
  // default behavior overridden to 1.
  const int slot = NovaShip_AllocateShipSlot(state,
                                             static_cast<std::int16_t>(sys_idx),
                                             /*reserved_tail=*/8);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_class_id = 0x80 - 0x80; // starter class (zero-based 0)
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.current_system_id = static_cast<std::int16_t>(sys_idx);
  ship.shield_points = 30.0F;
  ship.armor_points = 30.0F;
  ship.ai_behavior_code = 1;
  ship.faction_or_government_id = -1;
  ship.ai_state_code = 0;
  ship.jump_destination_stellar_id = -1;
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = -1;
  ship.travel_transfer_mode = 0;
  ship.reverse_speed_bias = 0.0F;

  // Prime the availability refresh for this system.
  NovaTargeting_UpdateStellarAvailability(state);

  NovaAi_UpdateShipAI(state, ship, /*skip_heavy_ai=*/false, /*now_ms=*/0);

  // The wander supervisor must take the ship out of idle into a defined state.
  const bool took_route = (ship.ai_state_code == 1 || ship.ai_state_code == 2 ||
                           ship.ai_state_code == 6);
  CHECK(took_route);
  if (ship.ai_state_code == 1) {
    // Traveling: must have selected a real stellar resource id and armed the
    // controls bridge to steer toward it.
    REQUIRE(ship.ai_secondary_target_slot >= 0x80);
    CHECK(ship.ai_control_mode == 2);
    // The controls bridge wrote a concrete heading and forward thrust.
    CHECK(ship.ai_forward_thrust_cmd != 0.0F);
  }
}

// A fire-restricted (derelict-government) ship must NOT engage the heavy AI:
// it holds state 0 / control 0 instead of picking a travel target.
TEST_CASE("fire-restricted ship does not initiate travel") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);

  const int slot = NovaShip_AllocateShipSlot(state,
                                             static_cast<std::int16_t>(sys_idx),
                                             /*reserved_tail=*/8);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_class_id = 0x80 - 0x80;
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.current_system_id = static_cast<std::int16_t>(sys_idx);
  ship.shield_points = 30.0F;
  ship.armor_points = 30.0F;
  ship.ai_behavior_code = 1;
  ship.ai_state_code = 0;
  ship.jump_destination_stellar_id = -1;
  ship.ai_secondary_target_slot = -1;

  // A derelict government marks the ship fire-restricted (0x800 derelict bit).
  // Find one from the loaded scenario.
  const game::Government *derelict = nullptr;
  for (const auto &g : state.scenario.governments) {
    if ((g.flags_primary & 0x800) != 0) {
      derelict = &g;
      break;
    }
  }
  if (!derelict || derelict->name.empty()) {
    return; // no derelict government in this data; skip
  }
  // faction_or_government_id is the zero-based government id.
  const std::ptrdiff_t idx = &*derelict - &state.scenario.governments[0];
  ship.faction_or_government_id = static_cast<std::int16_t>(idx);

  NovaTargeting_UpdateStellarAvailability(state);
  NovaAi_UpdateShipAI(state, ship, /*skip_heavy_ai=*/false, /*now_ms=*/0);

  // Fire-restricted ships are held idle (no travel target picked).
  CHECK(ship.ai_state_code == 0);
  CHECK(ship.ai_secondary_target_slot == -1);
}

// The NPC jump gate uses the ship's OWN class fuel, not the player's
// (Stellar_CanShipInitiateJumpSequence 0x00415b80 on the NPC, not the player's
// ship). This catches the old bug where NovaAi called the player-only
// NovaTravel_CanStartJump so an NPC's jump decision reflected the player's
// hull class.
TEST_CASE("npc jump gate uses the npc's own class fuel") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Find a class that cannot carry jump fuel and one that can.
  const game::ShipClass *no_fuel = nullptr;
  const game::ShipClass *with_fuel = nullptr;
  for (const auto &sc : state.scenario.ships) {
    if (sc.base_fuel < static_cast<std::int16_t>(game::kJumpFuelCost)) {
      if (!no_fuel) {
        no_fuel = &sc;
      }
    } else if (!with_fuel) {
      with_fuel = &sc;
    }
    if (no_fuel && with_fuel) {
      break;
    }
  }
  REQUIRE(no_fuel != nullptr);   // a low-fuel class exists in the data
  REQUIRE(with_fuel != nullptr); // and a jumping-capable one

  // sc is const (range-for over a const ref); base on a const ShipClass* so
  // the std::distance argument types match.
  const auto zero_based = [&state](const game::ShipClass &sc) -> std::int16_t {
    const game::ShipClass *first = &state.scenario.ships.front();
    return static_cast<std::int16_t>(
        std::distance(first, static_cast<const game::ShipClass *>(&sc)));
  };

  game::Ship npc;
  npc.ship_class_id = zero_based(*with_fuel);
  CHECK(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));

  npc.ship_class_id = zero_based(*no_fuel);
  CHECK_FALSE(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));
}
