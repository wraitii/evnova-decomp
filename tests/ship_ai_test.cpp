#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/scenario_data.hpp"
#include "game/ship_ai.hpp"
#include "game/ship_spawn.hpp"
#include "game/spaceflight.hpp"
#include "game/targeting.hpp"
#include "game/travel.hpp"

#include <algorithm>
#include <iterator>

namespace {

using game::GameState;
using game::NovaAi_FindBestAssistTargetForShip;
using game::NovaAi_UpdateShipAI;
using game::NovaAiShip_CanInterceptCurrentPrimaryTarget;
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
  ship.ai_maneuver_timer_ms = 0.0F;

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
    // The controls bridge wrote a concrete heading. Forward thrust is gated on
    // the original's alignment check (thrust only within turn_rate + 5 deg of
    // the bearing), so align the hull and run the pass again before expecting
    // a thrust command.
    CHECK(ship.ai_desired_heading_deg != 0);
    ship.heading =
        static_cast<float>(ship.ai_desired_heading_deg) * 3.14159265F / 180.0F;
    NovaAi_UpdateShipAI(state, ship, /*skip_heavy_ai=*/false, /*now_ms=*/0);
    if (ship.ai_state_code == 1) {
      CHECK(ship.ai_forward_thrust_cmd != 0.0F);
    }
  }
}

TEST_CASE("behavior-0x02 promotes an established hostile contact") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.player.armor_points = 30.0F;
  state.player.shield_points = 30.0F;

  const int slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_class_id = 0;
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.current_system_id = static_cast<std::int16_t>(sys_idx);
  ship.is_active = true;
  ship.ai_behavior_code = 2;
  ship.ai_state_code = 0;
  ship.ai_hostility_accumulator = 1;
  // This is a hostile contact, not an escort target. Ghidra's state-4 branch
  // intentionally clears a primary target when the two slots are identical.
  ship.squad_leader_ship_slot = -1;
  ship.primary_target_ship_slot = 0;
  ship.pos_x = 100.0F;
  ship.pos_y = 0.0F;
  ship.armor_points = 30.0F;

  NovaAi_UpdateShipAI(state, ship, /*skip_heavy_ai=*/false, /*now_ms=*/0);

  CHECK(ship.ai_state_code == 4);
  CHECK(ship.primary_target_ship_slot == 0);
  CHECK((ship.ai_control_mode == 5 || ship.ai_control_mode == 6));
}

TEST_CASE("behavior-0x03 acquires a hostile player and pursues") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.player.armor_points = 30.0F;
  state.player.shield_points = 30.0F;

  const int slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_class_id = 0;
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.current_system_id = static_cast<std::int16_t>(sys_idx);
  ship.is_active = true;
  ship.ai_behavior_code = 3;
  ship.ai_state_code = 0;
  ship.ai_hostility_accumulator = 1;
  ship.primary_target_ship_slot = -1;
  ship.pos_x = 100.0F;
  ship.pos_y = 0.0F;
  ship.armor_points = 30.0F;

  NovaAi_UpdateShipAI(state, ship, /*skip_heavy_ai=*/false, /*now_ms=*/0);

  CHECK(ship.primary_target_ship_slot == 0);
  CHECK(ship.ai_state_code == 4);
  CHECK(ship.ai_control_mode == 6);
  CHECK(ship.ai_desired_heading_deg != 0);
}

TEST_CASE(
    "intercept helper follows the relative-velocity and class-speed gates") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  state.player.is_active = true;
  state.player.ship_instance_id = 0;

  std::int16_t attacker_class = -1;
  std::int16_t faster_target_class = -1;
  for (std::size_t i = 0; i < state.scenario.ships.size(); ++i) {
    const auto &candidate = state.scenario.ships[i];
    if (candidate.mass_tons >= 100 && attacker_class == -1) {
      attacker_class = static_cast<std::int16_t>(i);
    }
    if (candidate.mass_tons >= 100 && attacker_class != -1 &&
        candidate.speed >
            state.scenario.ships[static_cast<std::size_t>(attacker_class)]
                .speed) {
      faster_target_class = static_cast<std::int16_t>(i);
      break;
    }
  }
  REQUIRE(attacker_class >= 0);
  REQUIRE(faster_target_class >= 0);

  const int slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(slot > 0);
  game::Ship &attacker = state.ShipAt(static_cast<std::size_t>(slot));
  attacker.ship_class_id = attacker_class;
  attacker.ship_instance_id = static_cast<std::int16_t>(slot);
  attacker.current_system_id = static_cast<std::int16_t>(sys_idx);
  attacker.primary_target_ship_slot = 0;
  attacker.is_active = true;
  attacker.armor_points = 100.0F;
  attacker.pos_x = 100.0F;
  attacker.pos_y = 0.0F;
  attacker.vel_x = 0.0F;
  attacker.vel_y = 0.0F;

  state.player.ship_class_id = faster_target_class;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  state.player.vel_x = 0.0F;
  state.player.vel_y = -10.0F;
  state.player.armor_points = 100.0F;

  CHECK(NovaAiShip_CanInterceptCurrentPrimaryTarget(state, attacker));
  state.player.vel_y = 0.0F;
  CHECK_FALSE(NovaAiShip_CanInterceptCurrentPrimaryTarget(state, attacker));
}

TEST_CASE("assist helper chooses the lowest positive candidate score") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.armor_points = 100.0F;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;

  const int helper_slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  const int candidate_slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(helper_slot > 0);
  REQUIRE(candidate_slot > helper_slot);
  game::Ship &helper = state.ShipAt(static_cast<std::size_t>(helper_slot));
  game::Ship &candidate =
      state.ShipAt(static_cast<std::size_t>(candidate_slot));
  helper.is_active = true;
  helper.armor_points = 100.0F;
  helper.squad_leader_ship_slot = 0;
  helper.primary_target_ship_slot = 0;
  helper.pos_x = 500.0F;
  helper.pos_y = 0.0F;
  candidate.is_active = true;
  candidate.armor_points = 100.0F;
  candidate.squad_leader_ship_slot = 1;
  candidate.primary_target_ship_slot = 0;
  candidate.pos_x = 100.0F;
  candidate.pos_y = 0.0F;

  CHECK(NovaAi_FindBestAssistTargetForShip(state, helper, -1) ==
        candidate_slot);
}

TEST_CASE(
    "cloak engagement gate is target-aware and has close-range fallbacks") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Use class 0 for both synthetic ships and remove its default loadout so
  // the negative case cannot depend on the scenario's current outfit data.
  REQUIRE(!state.scenario.ships.empty());
  state.scenario.ships[0].default_outfit_counts.fill(0);

  game::Ship subject_ship;
  subject_ship.ship_instance_id = 1;
  subject_ship.ship_class_id = 0;
  subject_ship.ai_state_code = 4;
  subject_ship.cloak_fade_progress = 17.0F;
  subject_ship.cloak_transition_latch = 0;
  subject_ship.pos_x = 0.0F;
  subject_ship.pos_y = 0.0F;

  game::Ship other_ship;
  other_ship.ship_instance_id = 2;
  other_ship.ship_class_id = 0;
  other_ship.pos_x = 100.0F;
  other_ship.pos_y = 100.0F;

  CHECK_FALSE(game::NovaAiShip_CanEngageTargetUnderCloakRules(
      state, subject_ship, other_ship));

  other_ship.cloak_scanner_reveal_screen = 1;
  CHECK(game::NovaAiShip_CanEngageTargetUnderCloakRules(
      state, subject_ship, other_ship));
  other_ship.pos_x = 201.0F;
  CHECK_FALSE(game::NovaAiShip_CanEngageTargetUnderCloakRules(
      state, subject_ship, other_ship));

  other_ship.pos_x = 100.0F;
  other_ship.squad_leader_ship_slot = subject_ship.ship_instance_id;
  state.scenario.ships[0].default_outfit_ids[0] = 0x80;
  state.scenario.ships[0].default_outfit_counts[0] = 1;
  state.scenario.outfits[0].mod_type = 0x11;
  CHECK(game::NovaAiShip_CanEngageTargetUnderCloakRules(
      state, subject_ship, other_ship));

  other_ship.pers_def_slot = 0x3ff;
  subject_ship.ai_state_code = 0x15;
  CHECK_FALSE(game::NovaAiShip_CanEngageTargetUnderCloakRules(
      state, subject_ship, other_ship));
}

TEST_CASE("hidden combat ship brakes with a finite engagement patience timer") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(!state.scenario.ships.empty());

  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.ship_class_id = 0;
  state.player.armor_points = 100.0F;
  state.player.death_timer_active = -1.0F;
  // Ship_UpdateShipAiState calls the original cloak-engagement predicate as
  // Can(target, attacker): the target's hidden state makes the NPC brake.
  state.player.cloak_fade_progress = 17.0F;
  state.player.cloak_transition_latch = 0;

  game::Ship &attacker = state.ShipAt(1);
  attacker.is_active = true;
  attacker.ship_instance_id = 1;
  attacker.ship_class_id = 0;
  attacker.ai_behavior_code = 3;
  attacker.ai_state_code = 4;
  attacker.primary_target_ship_slot = 0;
  attacker.target_engagement_patience_timer = -1.0F;

  state.inventory.outfit_owned_count.fill(0);
  game::NovaAi_UpdateShipState(state, attacker, /*now_ms=*/0);

  CHECK(attacker.ai_state_code == 4);
  CHECK(attacker.ai_control_mode == 1);
  CHECK(attacker.ai_secondary_target_slot == -1);
  CHECK(attacker.target_engagement_patience_timer >= 100.0F);
  CHECK(attacker.target_engagement_patience_timer < 200.0F);
}

TEST_CASE("cloak traits enter and clear the NPC cloak transition") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(!state.scenario.ships.empty());

  auto &ship_class = state.scenario.ships[0];
  ship_class.default_outfit_ids[0] = 0x80;
  ship_class.default_outfit_counts[0] = 1;
  ship_class.flags_secondary = 0x2000;
  auto &cloaking_device = state.scenario.outfits[0];
  cloaking_device.mod_type = 0x11;
  cloaking_device.mod_val = 0x0004;

  game::Ship ship;
  ship.ship_instance_id = 1;
  ship.ship_class_id = 0;
  ship.ai_state_code = 0;
  ship.shield_points = 25.0F;
  ship.armor_points = 100.0F;
  game::NovaAi_UpdateShipCloakStateFromTraits(state, ship);

  CHECK(ship.cloak_transition_latch == 1);
  CHECK(ship.shield_points == 0.0F);

  ship_class.flags_secondary = 0;
  // Ghidra's baseline visibility comparison is strict (>16.0), so exactly
  // 16.0 has not crossed the clear gate yet.
  ship.cloak_fade_progress = 17.0F;
  game::NovaAi_UpdateShipCloakStateFromTraits(state, ship);
  CHECK(ship.cloak_transition_latch == -1);
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
  // A tankful ship may jump.
  npc.fuel_points = game::kJumpFuelCost;
  CHECK(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));

  // A hull that cannot carry a jump's fuel may not jump even with a full tank.
  npc.ship_class_id = zero_based(*no_fuel);
  npc.fuel_points = 500.0F;
  CHECK_FALSE(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));
}

TEST_CASE("state 0x15 hypergate emergence preserves its slower arrival speed") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(!state.scenario.stellars.empty());
  REQUIRE(!state.scenario.ships.empty());

  game::Ship ship;
  ship.ship_instance_id = 1;
  ship.ship_class_id = 0;
  ship.squad_leader_ship_slot = -1;

  const std::int16_t stellar_id = 0x80;
  const auto *stellar = state.scenario.Stellar(stellar_id);
  REQUIRE(stellar != nullptr);

  game::NovaAi_EnterState15JumpOutToSystem(state, ship, stellar_id);

  CHECK(ship.ai_state_code == 0x15);
  CHECK(ship.ai_control_mode == 0);
  CHECK(ship.ai_secondary_target_slot == stellar_id);
  CHECK(ship.primary_target_ship_slot == -1);
  CHECK(ship.ai_maneuver_timer_ms == Catch::Approx(60.0F));
  CHECK(ship.ai_station_hold_timer == Catch::Approx(-1.0F));
  CHECK(ship.ai_desired_speed == Catch::Approx(-30.0F));
  CHECK(ship.ai_forward_thrust_cmd == Catch::Approx(-3.0F));
  CHECK(ship.heading >= 0.0F);
  CHECK(ship.heading < 2.0F * 3.14159265358979323846F);
  if (stellar->emergence_angle_deg.has_value() &&
      *stellar->emergence_angle_deg >= 0 &&
      *stellar->emergence_angle_deg <= 359) {
    CHECK(ship.heading ==
          Catch::Approx(static_cast<float>(*stellar->emergence_angle_deg) *
                        (3.14159265358979323846F / 180.0F)));
  }

  const game::ShipClass *emergence_class =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  REQUIRE(emergence_class != nullptr);
  game::NovaShip_IntegrateNpcMovement(
      state, ship, *emergence_class, /*elapsed_ticks=*/1.0F);
  CHECK(ship.ai_maneuver_timer_ms == Catch::Approx(59.0F));
  CHECK(ship.pos_x == Catch::Approx(0.0F));
  CHECK(ship.pos_y == Catch::Approx(0.0F));

  // Simulate expiry of the 60-tick emergence hold. The first state pass changes
  // 0x15 to state 8; the next selects mode 0x0a, which must preserve the
  // already-negative -30 override instead of replacing it with -50.
  ship.ai_maneuver_timer_ms = 0.0F;
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  REQUIRE(ship.ai_state_code == 8);
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  REQUIRE(ship.ai_control_mode == 10);
  game::NovaAi_ApplyControls(state, ship, 1.0F, /*now_ms=*/0);
  CHECK(ship.ai_desired_speed == Catch::Approx(-30.0F));
  CHECK(ship.ai_forward_thrust_cmd == Catch::Approx(-1.165F));

  game::Ship player_follower;
  player_follower.ship_class_id = 0;
  player_follower.squad_leader_ship_slot = 0;
  game::NovaAi_EnterState15JumpOutToSystem(state, player_follower, stellar_id);
  CHECK(player_follower.ai_desired_speed == Catch::Approx(-15.0F));
}

TEST_CASE("state 0x14 NPC jump transfers to the linked system") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  std::int16_t source_system = -1;
  std::int16_t target_stellar = -1;
  std::int16_t destination_system = -1;
  for (std::size_t system_index = 0;
       system_index < state.scenario.systems.size() && source_system < 0;
       ++system_index) {
    const auto &system = state.scenario.systems[system_index];
    for (std::size_t slot = 0; slot < system.nav_defs.size(); ++slot) {
      const auto *stellar = state.scenario.Stellar(system.nav_defs[slot]);
      if (stellar == nullptr || (stellar->availability_flags & 0x3000) == 0 ||
          system.links[slot] < 0x80) {
        continue;
      }
      source_system = static_cast<std::int16_t>(system_index);
      target_stellar = system.nav_defs[slot];
      destination_system = static_cast<std::int16_t>(system.links[slot] - 0x80);
      break;
    }
  }
  REQUIRE(source_system >= 0);

  std::int16_t fuel_class = -1;
  for (std::size_t i = 0; i < state.scenario.ships.size(); ++i) {
    if (state.scenario.ships[i].base_fuel >= 100) {
      fuel_class = static_cast<std::int16_t>(i);
      break;
    }
  }
  REQUIRE(fuel_class >= 0);

  game::Ship ship;
  ship.is_active = true;
  ship.ship_instance_id = 1;
  ship.ship_class_id = fuel_class;
  ship.current_system_id = source_system;
  ship.ai_state_code = 0x14;
  ship.ai_secondary_target_slot = target_stellar;
  ship.fuel_points = 100.0F;

  REQUIRE(game::NovaAi_CompleteNpcJump(state, ship));
  CHECK(ship.current_system_id == destination_system);
  CHECK(ship.fuel_points == Catch::Approx(0.0F));
  CHECK(ship.ai_state_code == 0x15);
  CHECK(ship.ai_control_mode == 0);
  CHECK(ship.vel_x == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(0.0F));
}

// The jump gate also requires CURRENT fuel: a jumping-capable class with an
// empty tank is refused, mirroring Stellar_HandlePlayerShipCore's
// `fuel_points < _DAT_005755a4 (100)` jump block.
TEST_CASE("jump gate requires current fuel") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const game::ShipClass *with_fuel = nullptr;
  for (const auto &sc : state.scenario.ships) {
    if (sc.base_fuel >= static_cast<std::int16_t>(game::kJumpFuelCost)) {
      with_fuel = &sc;
      break;
    }
  }
  REQUIRE(with_fuel != nullptr);
  const game::ShipClass *first = &state.scenario.ships.front();
  const std::int16_t cls_zero = static_cast<std::int16_t>(
      std::distance(first, static_cast<const game::ShipClass *>(with_fuel)));

  game::Ship npc;
  npc.ship_class_id = cls_zero;
  npc.fuel_points = 0.0F; // empty tank
  CHECK_FALSE(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));
  npc.fuel_points = game::kJumpFuelCost - 1.0F;
  CHECK_FALSE(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));
  npc.fuel_points = game::kJumpFuelCost;
  CHECK(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));
}

// ---------------------------------------------------------------------------
// Ship_ApplyShipAiControls (0x00408150) combat/formation mode fidelity.
// ---------------------------------------------------------------------------

namespace {

using game::NovaAi_ApplyControls;
using game::NovaShip_ComputeEffectiveStats;

// Spawn a healthy NPC in a real system and point it at the player (slot 0).
[[nodiscard]] game::Ship &
SpawnCombatTestShip(GameState &state, int sys_idx, std::int16_t class_id) {
  const int slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.ship_class_id = class_id;
  ship.shield_points = static_cast<float>(
      state.scenario.Ship(static_cast<std::int16_t>(class_id + 0x80))
          ->base_shield);
  ship.armor_points = static_cast<float>(
      state.scenario.Ship(static_cast<std::int16_t>(class_id + 0x80))
          ->base_armor);
  ship.ai_secondary_target_slot = -1;
  ship.primary_target_ship_slot = -1;
  ship.squad_leader_ship_slot = -1;
  ship.faction_or_government_id = -1;
  ship.target_stellar_object_id = -1;
  return ship;
}

void ActivatePlayer(GameState &state, float pos_x, float pos_y) {
  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.pos_x = pos_x;
  state.player.pos_y = pos_y;
  state.player.armor_points = 30.0F;
  state.player.shield_points = 30.0F;
}

} // namespace

// The original's mode-5 bearing call reverses the argument order
// (Math_BearingFromPointToPoint(target_pos, ship_pos)), so the close-range
// attack mode steers AWAY from the target -- the ship backs off while its
// guns keep the target in front. Every other combat mode uses ship->target.
TEST_CASE("ApplyControls mode 5 steers away from the target (original quirk)") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  ActivatePlayer(state, 200.0F, 0.0F);

  game::Ship &ship = SpawnCombatTestShip(state, sys_idx, 0);
  ship.pos_x = 100.0F;
  ship.pos_y = 0.0F;
  ship.ai_control_mode = 5;
  ship.primary_target_ship_slot = 0;
  ship.ai_state_code = 4;
  ship.heading = 0.0F; // currently pointing up

  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);

  // Player is east of the ship: mode 5 must steer west (270 deg), not east.
  REQUIRE(ship.ai_desired_heading_deg == 270);
  // Once aligned, mode 5 applies the raw effective thrust.
  ship.heading = static_cast<float>(ship.ai_desired_heading_deg) *
                 (3.14159265358979323846F / 180.0F);
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  const game::ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const game::NpcEffectiveStats eff =
      NovaShip_ComputeEffectiveStats(state, ship, *cls);
  CHECK(ship.ai_forward_thrust_cmd ==
        Catch::Approx(eff.thrust_px_per_tick2).margin(1e-4F));
}

// Mode 6 (combat pursuit) steers ship->target and thrusts within turn+15 deg;
// mode 0x10 (evasive break) flies the stored evasive heading at 1.5x thrust;
// mode 0x11 (boost) over-cruises at 2.75x thrust / 1.8x max speed.
TEST_CASE("ApplyControls combat modes 6/0x10/0x11 movement fidelity") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  ActivatePlayer(state, 200.0F, 0.0F);

  game::Ship &ship = SpawnCombatTestShip(state, sys_idx, 0);
  const game::ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const game::NpcEffectiveStats eff =
      NovaShip_ComputeEffectiveStats(state, ship, *cls);

  // Mode 6: pursue east.
  ship.pos_x = 100.0F;
  ship.pos_y = 0.0F;
  ship.ai_control_mode = 6;
  ship.primary_target_ship_slot = 0;
  ship.heading = 90.0F * (3.14159265358979323846F / 180.0F); // already aligned
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  REQUIRE(ship.ai_desired_heading_deg == 90);
  CHECK(ship.ai_forward_thrust_cmd ==
        Catch::Approx(eff.thrust_px_per_tick2).margin(1e-4F));
  CHECK(ship.ai_desired_speed == 0.0F);

  // Mode 0x10: evasive break on the stored heading at 1.5x thrust.
  ship.ai_control_mode = 0x10;
  ship.ai_evasive_heading_deg = 200;
  ship.heading = 200.0F * (3.14159265358979323846F / 180.0F);
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  REQUIRE(ship.ai_desired_heading_deg == 200);
  CHECK(ship.ai_forward_thrust_cmd ==
        Catch::Approx(eff.thrust_px_per_tick2 * 1.5F).margin(1e-4F));
  CHECK(ship.ai_desired_speed == 0.0F);

  // Mode 0x11: boost at 2.75x thrust, cruise 1.8x max speed.
  ship.ai_control_mode = 0x11;
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  CHECK(ship.ai_forward_thrust_cmd ==
        Catch::Approx(eff.thrust_px_per_tick2 * 2.75F).margin(1e-4F));
  CHECK(ship.ai_desired_speed ==
        Catch::Approx(eff.max_speed_px_per_tick * 1.8F).margin(1e-3F));
}

// Mode 0xc (velocity match) copies the target's velocity once the relative
// velocity is within the 0.525 px/tick tolerance, and eases the desired
// heading toward the target's heading.
TEST_CASE("ApplyControls mode 0xc velocity match") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  ActivatePlayer(state, 50.0F, 0.0F);

  game::Ship &ship = SpawnCombatTestShip(state, sys_idx, 0);
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.ai_control_mode = 0xc;
  ship.ai_secondary_target_slot = 0;
  state.player.vel_x = 0.4F;
  state.player.vel_y = -0.2F;
  state.player.heading = 30.0F * (3.14159265358979323846F / 180.0F);
  // Large relative velocity on BOTH axes (the original's slow-damp arm is an
  // OR across axes): the brake arm runs (no velocity copy).
  ship.vel_x = 3.0F;
  ship.vel_y = 2.0F;
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  CHECK(ship.vel_x == 3.0F); // not copied while outrunning
  // Align with the reverse of the relative-velocity bearing and brake:
  // relative (2.6, 2.2) points ~130.2 deg; reverse is ~310.2 deg.
  ship.heading = 310.2F * (3.14159265358979323846F / 180.0F);
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  CHECK(ship.ai_forward_thrust_cmd > 0.0F); // braking on the relative velocity

  // Match the velocity: the copy arm runs and snaps velocity to the target.
  ship.vel_x = 0.4F;
  ship.vel_y = -0.2F;
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  CHECK(ship.vel_x == 0.4F);
  CHECK(ship.vel_y == -0.2F);
}

// Mode 0xf (disabled-target pursuit) brakes on the RELATIVE velocity, then
// copies the target's velocity and creeps position toward it.
TEST_CASE("ApplyControls mode 0xf velocity-match pursuit") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  ActivatePlayer(state, 10.0F, 0.0F);

  game::Ship &ship = SpawnCombatTestShip(state, sys_idx, 0);
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.ai_control_mode = 0xf;
  ship.ai_secondary_target_slot = 0;
  state.player.vel_x = 0.1F;
  state.player.vel_y = 0.0F;
  state.player.heading = 0.0F;

  // Relative velocity 2.0 px/tick: brake arm, steer at the reverse of the
  // relative-velocity bearing (relative vel (2,0) -> bearing 90 -> +180 = 270).
  ship.vel_x = 2.0F;
  ship.vel_y = 0.0F;
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  REQUIRE(ship.ai_desired_heading_deg == 270);
  ship.heading = static_cast<float>(ship.ai_desired_heading_deg) *
                 (3.14159265358979323846F / 180.0F);
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  CHECK(ship.ai_forward_thrust_cmd > 0.0F);

  // Relative velocity small: match the target's velocity.
  ship.vel_x = 0.1F;
  ship.vel_y = 0.05F;
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  CHECK(ship.vel_x == 0.1F);
  CHECK(ship.vel_y == 0.0F);
}

// Mode 0x12 (chase leader) falls back to control mode 0 without a leader and
// steers at the point 15x max-speed ahead of the leader's heading otherwise.
TEST_CASE("ApplyControls mode 0x12 chase leader") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  ActivatePlayer(state, 0.0F, 0.0F);

  game::Ship &ship = SpawnCombatTestShip(state, sys_idx, 0);
  ship.ai_control_mode = 0x12;
  ship.formation_leader_ship_slot = -1;
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  REQUIRE(ship.ai_control_mode == 0); // no leader: idle control

  ship.ai_control_mode = 0x12;
  // The original treats leader slot < 1 (including the player slot 0) as "no
  // leader", so the chased leader must occupy a real NPC slot.
  const int leader_slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(leader_slot > 0);
  game::Ship &leader = state.ShipAt(static_cast<std::size_t>(leader_slot));
  leader.is_active = true;
  leader.ship_instance_id = static_cast<std::int16_t>(leader_slot);
  ship.formation_leader_ship_slot = static_cast<std::int16_t>(leader_slot);
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.heading = 0.0F;
  leader.pos_x = 100.0F;
  leader.pos_y = 0.0F;
  leader.heading = 0.0F; // leader facing up: lead point = north of leader
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  // Lead point is (100, -15*max) north of the leader; the ship at the origin
  // must steer roughly north-east.
  const game::ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const game::NpcEffectiveStats eff =
      NovaShip_ComputeEffectiveStats(state, ship, *cls);
  const float lead_x = 100.0F;
  const float lead_y = -eff.max_speed_px_per_tick * 15.0F; // up in -y
  const float expected =
      std::atan2(lead_x, -lead_y) / (3.14159265358979323846F / 180.0F);
  CHECK(std::abs(ship.ai_desired_heading_deg - expected) < 1.0F);
}

// Mode 10 begins the heading-aligned arrival override at 50 px/tick and
// reduces it by 1.165 per movement tick (the original's raw float commands).
TEST_CASE("ApplyControls mode 10 arrival slowdown") {
  GameState state;
  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.ship_instance_id = 1;
  ship.ai_control_mode = 10;
  ship.ai_desired_speed = 0.0F;
  NovaAi_ApplyControls(state,
                       ship,
                       1.0F,
                       /*now_ms=*/0);
  CHECK(ship.ai_desired_speed == Catch::Approx(-50.0F));
  CHECK(ship.ai_forward_thrust_cmd == Catch::Approx(-1.165F));
}

// State 0x08 is the NPC arrival slowdown. Ghidra's state updater selects
// control mode 0x0a, whose speed override is then consumed by the
// Ship_HandleShip movement/glow path (0x00433050).
TEST_CASE("state 0x08 drives visible high-speed NPC arrival") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0;

  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.ship_instance_id = 1;
  ship.current_system_id = 0;
  ship.ship_class_id = 0;
  ship.armor_points = 30.0F;
  ship.shield_points = 30.0F;
  ship.ai_state_code = 8;
  ship.ai_control_mode = 0;
  ship.ai_desired_speed = 0.0F;
  ship.ai_forward_thrust_cmd = 0.0F;
  ship.heading = 0.0F;
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;

  const game::ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  REQUIRE(cls != nullptr);
  const float max_speed =
      NovaShip_ComputeEffectiveStats(state, ship, *cls).max_speed_px_per_tick;
  const int expected_slowdown_ticks =
      static_cast<int>(std::ceil((50.0F - max_speed) / 1.165F));

  float previous_speed = 51.0F;
  int slowdown_ticks = 0;
  while (ship.ai_state_code == 8 && slowdown_ticks < 64) {
    game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
    REQUIRE(ship.ai_control_mode == 10);
    game::NovaAi_ApplyControls(state, ship, 1.0F, /*now_ms=*/0);
    game::NovaShip_TickNpcShips(state, 1.0F);
    ++slowdown_ticks;

    const float current_speed = std::hypot(ship.vel_x, ship.vel_y);
    CHECK(current_speed < previous_speed);
    previous_speed = current_speed;
    if (ship.ai_state_code == 8) {
      CHECK(ship.ai_maneuver_timer_ms <= 0.0F);
    }
  }

  CHECK(slowdown_ticks == expected_slowdown_ticks);
  CHECK(ship.ai_state_code == 0);
  // Ship_HandleShip performs its ordinary end-of-frame timer decrement after
  // arming the 30..59-tick coast, so the stored value has lost one normalized
  // reference tick here.
  CHECK(ship.ai_maneuver_timer_ms >= 29.0F);
  CHECK(ship.ai_maneuver_timer_ms <= 58.0F);
  CHECK(ship.pos_y < -500.0F);
  CHECK(ship.engine_glow_level == 0);
}

TEST_CASE("state 2 special departure classes skip the outward brake") {
  GameState state;
  game::ShipClass cls;
  cls.flags_secondary = 0x0020;
  state.scenario.ships.push_back(cls);

  game::Ship &ship = state.ShipAt(1);
  ship.ship_class_id = 0;
  ship.ai_state_code = 2;
  ship.ai_control_mode = 1;
  ship.pos_x = 1100.0F;
  ship.vel_x = 10.0F;

  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);

  CHECK(ship.ai_control_mode == 4);
}

namespace {

// First ship-class index matching a predicate over the loaded stock scenario.
[[nodiscard]] int FindClassIndex(const game::GameState &state,
                                 const auto &pred) {
  for (std::size_t i = 0; i < state.scenario.ships.size(); ++i) {
    if (pred(state.scenario.ships[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

} // namespace

// End-to-end capture-approach drive (Ghidra 0x004038b0 supervisor -> 0x00405590
// state 0xd -> 0x00408150 mode 0xf -> Outfit_BoardShipAndTransferCargo): a
// behavior-3 plunderer of a flags_primary 0x1000 government acquires a
// disabled low-AI victim, latches it inside 3 px and arms the 100..179-tick
// boarding pause, then hands off to the boarding resolution. The capture
// conversion roll is forced through state.cheat_mode_active so the victim's
// escort-conversion post-conditions are deterministic.
TEST_CASE("capture-approach drive boards a disabled ship end-to-end",
          "[ai][boarding]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.cheat_mode_active = true;

  // Plunderer government: first with the Bible "warships will plunder non-
  // mission, trader-type enemies" flag; if stock data carries none, arm it on
  // govt 0 (test-only scenario mutation).
  std::size_t govt_idx = state.scenario.governments.size();
  for (std::size_t i = 0; i < state.scenario.governments.size(); ++i) {
    if ((state.scenario.governments[i].flags_primary & 0x1000U) != 0U) {
      govt_idx = i;
      break;
    }
  }
  if (govt_idx == state.scenario.governments.size()) {
    govt_idx = 0;
    state.scenario.governments[0].flags_primary |= 0x1000U;
  }
  const auto govt_id = static_cast<std::int16_t>(govt_idx);

  // Boarder class without sprite flag 2 (the port's provisional heavy-AI
  // cadence gate then runs the supervisor every tick) and with a moderate
  // maneuver rating, so the state-0xd keep-away range (10 - turn*0.1)*30
  // stays positive and wide.
  const int boarder_class = FindClassIndex(state, [](const game::ShipClass &c) {
    return (c.sprite_behavior_flags & 2U) == 0U && c.turn_rate > 0.0F &&
           c.turn_rate < 60.0F;
  });
  REQUIRE(boarder_class >= 0);
  // A free-energy armed bank keeps Weapon_ClassifyShipWeaponAmmoReadiness off
  // the stand-down arm (readiness 2 resets states 4/0xd).
  int free_energy_bank = -1;
  for (std::size_t w = 0; w < state.scenario.weapons.size(); ++w) {
    if (state.scenario.weapons[w].ammo_type >= -1000 &&
        state.scenario.weapons[w].ammo_type <= -1) {
      free_energy_bank = static_cast<int>(w);
      break;
    }
  }
  REQUIRE(free_energy_bank >= 0);

  // Victim class: low default AI (boardable "capturable kind") with capture
  // power (crew > 0) and enough armor to sit clearly under the 1/3 disable
  // threshold when crippled.
  const int victim_class = FindClassIndex(state, [](const game::ShipClass &c) {
    return c.default_ai_behavior < 3 && c.crew > 0 && c.base_armor >= 30;
  });
  REQUIRE(victim_class >= 0);

  const int boarder_slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  REQUIRE(boarder_slot > 0);
  game::Ship &boarder = state.ShipAt(static_cast<std::size_t>(boarder_slot));
  boarder.ship_class_id = static_cast<std::int16_t>(boarder_class);
  boarder.faction_or_government_id = govt_id;
  boarder.ai_behavior_code = 3;
  boarder.armor_points = static_cast<float>(
      state.scenario.ships[static_cast<std::size_t>(boarder_class)].base_armor);
  boarder.npc_weapon_bank_ammo[static_cast<std::size_t>(free_energy_bank)] = 1;
  boarder.pos_x = 0.0F; // override the allocator's spawn scatter
  boarder.pos_y = 0.0F;

  const int victim_slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  REQUIRE(victim_slot > 0);
  game::Ship &victim = state.ShipAt(static_cast<std::size_t>(victim_slot));
  victim.ship_class_id = static_cast<std::int16_t>(victim_class);
  victim.faction_or_government_id = -1;
  victim.pos_x = 2.0F; // within 3 px: the mode-0xf arm fires on arrival
  victim.pos_y = 0.0F;
  victim.armor_points = static_cast<float>(
      state.scenario.ships[static_cast<std::size_t>(victim_class)].base_armor /
      10);
  REQUIRE(game::NovaAiShip_IsDisabled(state, victim));

  bool handed_off = false;
  std::uint32_t now_ms = 0;
  for (int t = 0; t < 600 && !handed_off; ++t) {
    game::NovaAi_UpdateShipAI(state, boarder, /*skip_heavy_ai=*/false, now_ms);
    now_ms += 33;
    if (t == 5) {
      // Mid-approach: victim latched, boarding pause armed, velocity-match
      // hold (control 0xf) active.
      CHECK(victim.boarded_target_latch == 1);
      CHECK(boarder.ai_maneuver_timer_ms > 0.0F);
      CHECK(boarder.ai_maneuver_timer_ms <= 179.0F);
      CHECK(boarder.ai_control_mode == 0xf);
    }
    if (boarder.ai_state_code == 0xe &&
        boarder.ai_maneuver_timer_ms == 100.0F) {
      handed_off = true; // the supervisor's boarding handoff just fired
    }
    // Ship_HandleShip's coast-timer countdown (spaceflight.cpp).
    boarder.ai_maneuver_timer_ms =
        std::max(0.0F, boarder.ai_maneuver_timer_ms - 1.0F);
  }
  REQUIRE(handed_off);

  // Conversion post-conditions (cheat-forced): the victim becomes a
  // behavior-6 follower of the boarder at 66% armor with dropped shields.
  const auto &vcls =
      state.scenario.ships[static_cast<std::size_t>(victim_class)];
  CHECK(victim.ai_behavior_code == 6);
  CHECK(victim.faction_or_government_id == govt_id);
  CHECK(victim.squad_leader_ship_slot == boarder.ship_instance_id);
  CHECK(victim.boarded_target_latch == 0);
  CHECK(victim.shield_points == 0.0F);
  CHECK(
      victim.armor_points ==
      Catch::Approx(static_cast<float>(vcls.base_armor) * 0.66F).margin(0.5F));
  CHECK(!game::NovaAiShip_IsDisabled(state, victim));
}

// End-to-end assist approach (Ghidra 0x00410c70 entry -> 0x00405590 state 0xf
// -> 0x00408150 mode 0xf): a hailed helper targeting a disabled player
// velocity-matches, latches the player on arrival, then the assist arm clears
// the latch and repairs the player back above the disable threshold before
// leaving state 0xf.
TEST_CASE("hailed helper repairs a disabled player via assist state 0xf",
          "[ai][boarding]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int player_class = FindClassIndex(state, [](const game::ShipClass &c) {
    return c.base_armor >= 30 && (c.capability_flags & 0x10U) == 0U;
  });
  REQUIRE(player_class >= 0);
  const float player_max_armor = static_cast<float>(
      state.scenario.ships[static_cast<std::size_t>(player_class)].base_armor);

  game::Ship &player = state.player;
  player.is_active = true;
  player.ship_class_id = static_cast<std::int16_t>(player_class);
  player.armor_points = 1.0F; // well under the 1/3 disable threshold
  REQUIRE(game::NovaAiShip_IsDisabled(state, player));

  const int helper_slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  REQUIRE(helper_slot > 0);
  game::Ship &helper = state.ShipAt(static_cast<std::size_t>(helper_slot));
  helper.ship_class_id = static_cast<std::int16_t>(player_class);
  helper.armor_points = player_max_armor; // helper itself fully operational
  helper.pos_x = 2.0F;
  helper.pos_y = 2.0F; // within 3 px: the arrival arm fires immediately
  game::NovaAi_EnterState0FTargetPlayerForAssist(helper);
  CHECK(helper.ai_state_code == 0xf);
  CHECK(helper.ai_maneuver_timer_ms == -1.0F);

  bool saw_assist_approach = false;
  bool saw_player_latched = false;
  bool saw_helper_released = false;
  std::uint32_t now_ms = 0;
  for (int t = 0; t < 1200; ++t) {
    game::NovaAi_UpdateShipAI(state, helper, /*skip_heavy_ai=*/false, now_ms);
    now_ms += 33;
    if (helper.ai_state_code == 0xf && helper.ai_secondary_target_slot == 0) {
      saw_assist_approach = true;
    }
    if (player.boarded_target_latch == 1) {
      // The arrival arm latches the player before the assist arm clears it.
      saw_player_latched = true;
    }
    if (!game::NovaAiShip_IsDisabled(state, player) &&
        helper.ai_state_code == 0 && helper.ai_control_mode == 0 &&
        helper.primary_target_ship_slot == -1) {
      saw_helper_released = true;
      break;
    }
    helper.ai_maneuver_timer_ms =
        std::max(0.0F, helper.ai_maneuver_timer_ms - 1.0F);
  }
  CHECK(saw_assist_approach);
  CHECK(saw_player_latched);
  CHECK(saw_helper_released);

  CHECK(!game::NovaAiShip_IsDisabled(state, player));
  CHECK(player.armor_points >= player_max_armor / 3.0F);
  CHECK(player.boarded_target_latch == 0);
  CHECK(helper.primary_target_ship_slot == -1);
}
