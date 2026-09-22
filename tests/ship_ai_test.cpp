#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/nova_math.hpp"
#include "game/scenario_data.hpp"
#include "game/ship_ai.hpp"
#include "game/ship_ai_internal.hpp"
#include "game/ship_spawn.hpp"
#include "game/spaceflight.hpp"
#include "game/targeting.hpp"
#include "game/travel.hpp"
#include "game/weapon.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <random>

namespace {

using game::GameState;
using game::NovaAi_FindBestAssistTargetForShip;
using game::NovaAi_IsInboundThreatExceedingDefenses;
using game::NovaAi_IssueEscortOrders;
using game::NovaAi_UpdateShipAI;
using game::NovaAiShip_CanInterceptCurrentPrimaryTarget;
using game::NovaShip_AllocateShipSlot;
using game::NovaTargeting_UpdateStellarAvailability;

TEST_CASE("inbound threat uses the original defensive-budget boundary") {
  game::Ship ship;

  ship.shield_points = 60.0F;
  ship.armor_points = 40.0F;
  ship.inbound_weapon_threat = 104;
  CHECK_FALSE(NovaAi_IsInboundThreatExceedingDefenses(ship));

  // The binary64 1.05 constant is slightly greater than decimal 1.05, and the
  // original retains the product in an x87 register. Thus 105 is still below
  // the budget for 100 points of current defenses.
  ship.inbound_weapon_threat = 105;
  CHECK_FALSE(NovaAi_IsInboundThreatExceedingDefenses(ship));
  ship.inbound_weapon_threat = 106;
  CHECK(NovaAi_IsInboundThreatExceedingDefenses(ship));

  // Equality is accepted (FCOMPP's equality bit is not part of the rejection
  // mask), while unordered comparisons reject the threat.
  ship.shield_points = 0.0F;
  ship.armor_points = 0.0F;
  ship.inbound_weapon_threat = 0;
  CHECK(NovaAi_IsInboundThreatExceedingDefenses(ship));
  ship.shield_points = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(NovaAi_IsInboundThreatExceedingDefenses(ship));

  // The tally is a signed 16-bit field and the predicate does not clamp it.
  ship.shield_points = 0.0F;
  ship.inbound_weapon_threat = -1;
  CHECK_FALSE(NovaAi_IsInboundThreatExceedingDefenses(ship));
}

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

// First nav stellar in the system that is available, travel-flagged, and not a
// restricted hypergate/wormhole -- a target the state-1 travel arm accepts.
[[nodiscard]] std::int16_t FindUsableTravelStellar(GameState &state,
                                                   int sys_idx) {
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  NovaTargeting_UpdateStellarAvailability(state);
  const auto *sys =
      state.scenario.System(static_cast<std::int16_t>(sys_idx + 0x80));
  if (!sys) {
    return -1;
  }
  for (const auto nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const game::Stellar *st = state.scenario.Stellar(nav);
    if (st && st->is_available && (st->flags & 1U) != 0U &&
        (st->availability_flags & 0x3000) == 0U) {
      return nav;
    }
  }
  return -1;
}

// Deactivate every ship slot so a hand-built test owns the whole pool (the
// scenario loader may have populated the player's system).
void ClearAllShips(GameState &state) {
  for (std::size_t i = 0; i < GameState::kMaxShips; ++i) {
    state.ShipAt(i).is_active = false;
  }
}

} // namespace

TEST_CASE("escort order tiers use each side's stocked weapon reach") {
  GameState state;
  state.scenario.ships.resize(6);
  state.scenario.weapons.resize(2);

  state.scenario.ships[0].base_shield = 100;
  state.scenario.ships[0].base_armor = 100;
  state.scenario.ships[0].stock_weapons[0] = {0x80, 1, 0};
  state.scenario.ships[1].base_shield = 100;
  state.scenario.ships[1].base_armor = 100;
  state.scenario.ships[1].stock_weapons[0] = {0x81, 1, 0};
  for (std::size_t category = 0; category < 4; ++category) {
    state.scenario.ships[category + 2].class_category =
        static_cast<std::int16_t>(category);
  }
  state.scenario.weapons[0].weapon_mode_code = 1;
  state.scenario.weapons[1].weapon_mode_code = 1;
  state.scenario.weapons[0].range_scalar = 30.0F;
  state.scenario.weapons[1].range_scalar = 100.0F;

  game::Ship &leader = state.ShipAt(1);
  leader.ship_instance_id = 1;
  leader.ship_class_id = 0;
  leader.ai_behavior_code = 3;
  leader.ai_state_code = 4;
  leader.primary_target_ship_slot = 2;
  leader.shield_points = 20.0F;
  leader.pos_x = 0.0F;
  leader.pos_y = 0.0F;
  for (std::size_t slot = 3; slot <= 6; ++slot) {
    game::Ship &escort = state.ShipAt(slot);
    escort.ship_instance_id = static_cast<std::int16_t>(slot);
    escort.ship_class_id = static_cast<std::int16_t>(slot - 1);
    escort.ai_behavior_code = 5;
    escort.squad_leader_ship_slot = 1;
  }

  game::Ship &target = state.ShipAt(2);
  target.ship_instance_id = 2;
  target.ship_class_id = 1;
  target.ai_state_code = 4;
  target.primary_target_ship_slot = 1;
  target.armor_points = 100.0F;
  target.pos_x = 100.0F;
  target.pos_y = 0.0F;

  NovaAi_IssueEscortOrders(state, leader);

  // The target's stocked weapon reaches the leader, while the leader's does
  // not reach the target. Category 1 keeps the corrected asymmetric low-shield
  // tier and attacks. Category 0 (fighters) uses the fixed 382 px BUGFIX
  // radius; this 100 px target is inside it, so the fighters Defend.
  CHECK(state.ShipAt(3).escort_command_code == 1);
  CHECK(state.ShipAt(4).escort_command_code == 2);
  CHECK(state.ShipAt(5).escort_command_code == 1);
  CHECK(state.ShipAt(6).escort_command_code == 0);
  CHECK(state.ShipAt(3).escort_command_pending == 1);

  SECTION("fighters attack a locked target beyond the fixed radius") {
    target.pos_x = 500.0F; // beyond the 382 px reference reach
    NovaAi_IssueEscortOrders(state, leader);
    CHECK(state.ShipAt(3).escort_command_code == 2);
  }

  SECTION("fighters attack a target that is not engaging the leader") {
    target.primary_target_ship_slot = -1;
    target.pos_x = 500.0F;
    NovaAi_IssueEscortOrders(state, leader);
    CHECK(state.ShipAt(3).escort_command_code == 2);
  }

  SECTION("fighters attack a disabled target inside the radius") {
    target.armor_points = 0.0F;
    NovaAi_IssueEscortOrders(state, leader);
    CHECK(state.ShipAt(3).escort_command_code == 2);
  }

  SECTION("leaders that can reach their attacker keep damaged escorts close") {
    state.scenario.weapons[0].range_scalar = 100.0F;
    NovaAi_IssueEscortOrders(state, leader);
    CHECK(state.ShipAt(3).escort_command_code == 1);
    CHECK(state.ShipAt(4).escort_command_code == 1);
  }

  SECTION("range asymmetry matters only when the target attacks the leader") {
    target.primary_target_ship_slot = -1;
    NovaAi_IssueEscortOrders(state, leader);
    // Category 0 attacks a target that is not engaging the leader; category 1
    // keeps the asymmetric tier and Defends (neither side is in range).
    CHECK(state.ShipAt(3).escort_command_code == 2);
    CHECK(state.ShipAt(4).escort_command_code == 1);
  }

  SECTION("passive leaders use the healthy fighter branch") {
    leader.ai_behavior_code = 1;
    leader.shield_points = 70.0F;
    NovaAi_IssueEscortOrders(state, leader);
    CHECK(state.ShipAt(3).escort_command_code == 2);
    CHECK(state.ShipAt(4).escort_command_code == 1);
    CHECK(state.ShipAt(5).escort_command_code == 1);
    CHECK(state.ShipAt(6).escort_command_code == 0);
    leader.shield_points = 50.0F;
    NovaAi_IssueEscortOrders(state, leader);
    CHECK(state.ShipAt(3).escort_command_code == 1);
  }

  SECTION("disabled targets recall combat escorts in state 0x0d") {
    leader.ai_behavior_code = 3;
    leader.ai_state_code = 0xd;
    target.armor_points = 0.0F;
    NovaAi_IssueEscortOrders(state, leader);
    CHECK(state.ShipAt(3).escort_command_code == 3);
    CHECK(state.ShipAt(4).escort_command_code == 3);
    CHECK(state.ShipAt(5).escort_command_code == 3);
    CHECK(state.ShipAt(6).escort_command_code == 0);
  }

  SECTION("negative combat odds do not attack with warships") {
    leader.ai_behavior_code = 3;
    leader.ai_state_code = 4;
    leader.shield_points = 80.0F;
    leader.ai_odds_score = -1.0F;
    target.armor_points = 100.0F;
    NovaAi_IssueEscortOrders(state, leader);
    // Category 0 Defends (the 100 px target is inside the fixed radius); the
    // warship category keeps the odds gate and stays in Formation.
    CHECK(state.ShipAt(3).escort_command_code == 1);
    CHECK(state.ShipAt(4).escort_command_code == 2);
    CHECK(state.ShipAt(5).escort_command_code == 0);
    CHECK(state.ShipAt(6).escort_command_code == 0);
  }
}

TEST_CASE("escort orders obey caller cadence and bypasses") {
  GameState state;
  state.scenario.ships.resize(2);
  state.scenario.weapons.resize(1);
  state.scenario.ships[0].base_shield = 100;
  state.scenario.ships[0].base_armor = 100;
  state.scenario.ships[0].stock_weapons[0] = {0x80, 1, 0};
  state.scenario.ships[1].class_category = 0;
  state.scenario.ships[1].base_shield = 100;
  state.scenario.ships[1].base_armor = 100;
  state.scenario.weapons[0].weapon_mode_code = 1;
  state.scenario.weapons[0].range_scalar = 100.0F;

  game::Ship &leader = state.ShipAt(1);
  leader.ship_instance_id = 1;
  leader.ship_class_id = 0;
  leader.ai_behavior_code = 3;
  leader.ai_state_code = 4;
  leader.ai_control_mode = 0;
  leader.shield_points = 80.0F;
  leader.armor_points = 0.0F;
  leader.is_any_ships_squad_leader = true;

  game::Ship &escort = state.ShipAt(2);
  escort.ship_instance_id = 2;
  escort.ship_class_id = 1;
  escort.ai_behavior_code = 5;
  escort.squad_leader_ship_slot = 1;
  escort.escort_command_code = -7;

  // The order pass runs before the disabled auto-guard clears the leader.
  state.spaceflight_frame_counter = 0;
  NovaAi_UpdateShipAI(state, leader, 0);
  // No primary target (not locked): the fixed-radius fighter rule attacks.
  CHECK(escort.escort_command_code == 2);
  CHECK(leader.squad_leader_ship_slot == -1);

  // A nonmatching frame phase skips the order pass.
  leader.armor_points = 100.0F;
  leader.ai_state_code = 4;
  leader.ai_control_mode = 0;
  escort.escort_command_code = -7;
  state.spaceflight_frame_counter = 1;
  NovaAi_UpdateShipAI(state, leader, 0);
  CHECK(escort.escort_command_code == -7);

  // Entry control mode 4 bypasses the order pass even when its later state
  // handling would otherwise change the mode.
  leader.ai_control_mode = 4;
  leader.ai_state_code = 0xb;
  leader.squad_leader_ship_slot = 0;
  state.player.armor_points = 0.0F;
  escort.escort_command_code = -8;
  state.spaceflight_frame_counter = 0;
  NovaAi_UpdateShipAI(state, leader, 0);
  CHECK(escort.escort_command_code == -8);

  // The arrival sentinel has the same bypass and installs control mode 10.
  leader.ai_control_mode = 0;
  leader.ai_station_hold_timer = -999.0F;
  escort.escort_command_code = -9;
  NovaAi_UpdateShipAI(state, leader, 0);
  CHECK(escort.escort_command_code == -9);
}

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

  NovaAi_UpdateShipAI(state, ship, /*now_ms=*/0);

  // The wander supervisor must take the ship out of idle into a defined
  // state. The faithful selector can also pick a restricted hypergate/wormhole
  // point (a legitimate wander target), which the state machine turns into
  // state 0x14 (jump entry), so accept that route too.
  const bool took_route =
      (ship.ai_state_code == 1 || ship.ai_state_code == 0x14 ||
       ship.ai_state_code == 2 || ship.ai_state_code == 6);
  CHECK(took_route);
  if (ship.ai_state_code == 1 || ship.ai_state_code == 0x14) {
    // Traveling: must have selected a real stellar resource id.
    REQUIRE(ship.ai_secondary_target_slot >= 0x80);
  }
  if (ship.ai_state_code == 1) {
    // Traveling: must have selected a real stellar resource id and armed the
    // controls bridge to steer toward it.
    CHECK(ship.ai_control_mode == 2);
    // The controls bridge wrote a concrete heading. Forward thrust is gated on
    // the original's alignment check (thrust only within turn_rate + 5 deg of
    // the bearing), so align the hull and run the pass again before expecting
    // a thrust command.
    CHECK(ship.ai_desired_heading_deg != 0);
    ship.heading =
        static_cast<float>(ship.ai_desired_heading_deg) * 3.14159265F / 180.0F;
    NovaAi_UpdateShipAI(state, ship, /*now_ms=*/0);
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

  NovaAi_UpdateShipAI(state, ship, /*now_ms=*/0);

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

  const auto *system =
      state.scenario.System(static_cast<std::int16_t>(sys_idx + 0x80));
  REQUIRE(system != nullptr);
  REQUIRE(system->government_id >= 0);

  const int slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_class_id = 0x8d - 0x80;
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.current_system_id = static_cast<std::int16_t>(sys_idx);
  ship.faction_or_government_id = system->government_id;
  ship.is_active = true;
  ship.ai_behavior_code = 3;
  ship.ai_state_code = 0;
  ship.ai_hostility_accumulator = 1;
  ship.primary_target_ship_slot = -1;
  ship.pos_x = 100.0F;
  ship.pos_y = 0.0F;
  ship.armor_points = 750.0F;
  ship.shield_points = 800.0F;
  // Ghidra 0x0040e020 near-player reputation gate: a criminal record in the
  // ship's own system flags the player within random_ai_render_cadence * 600.
  state.system_reputation.assign(state.scenario.systems.size(), 0);
  state.system_reputation[static_cast<std::size_t>(sys_idx)] = -30000;
  ship.random_ai_render_cadence = 2;

  NovaAi_UpdateShipAI(state, ship, /*now_ms=*/0);

  CHECK(ship.primary_target_ship_slot == 0);
  CHECK(ship.ai_state_code == 4);
  // The Fed Destroyer carries Flags2 0x0082, so the state-4 standoff arm
  // selects close engagement mode 5 (the pre-fix port ignored Flags2 0x0002
  // and wrote mode 6).
  CHECK(ship.ai_control_mode == 5);
  CHECK(ship.ai_desired_heading_deg != 0);
}

// A ship holding a stellar target runs the dedicated player-threat supervisor
// (Ship_DefenseFleetPrioritizePlayerThreat) instead of its behavior supervisor,
// so a nearby player-side contact is locked as the primary target (state 4).
TEST_CASE("stellar-target ship prioritizes a nearby player-side contact") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  const std::int16_t stellar_id = FindUsableTravelStellar(state, sys_idx);
  REQUIRE(stellar_id >= 0x80);
  ClearAllShips(state);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);

  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.pos_x = 50100.0F;
  state.player.pos_y = 50000.0F;
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
  ship.ai_behavior_code = 1;
  ship.ai_state_code = 0;
  ship.pos_x = 50000.0F;
  ship.pos_y = 50000.0F;
  ship.armor_points = 30.0F;
  ship.shield_points = 30.0F;
  ship.defense_fleet_home_stellar_id = stellar_id;
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = -1;

  NovaAi_UpdateShipAI(state, ship, /*now_ms=*/0);

  CHECK(ship.primary_target_ship_slot == 0);
  CHECK(ship.ai_state_code == 4);
}

// With a stellar target and no player-side contact, the supervisor hands the
// ship back to travel (state 1) with the stellar in the secondary slot.
TEST_CASE("stellar-target ship returns to stellar travel with no contact") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  const std::int16_t stellar_id = FindUsableTravelStellar(state, sys_idx);
  REQUIRE(stellar_id >= 0x80);
  ClearAllShips(state);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  // No active player means no candidate qualifies (slot 0 is skipped).
  state.player.is_active = false;

  const int slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_class_id = 0;
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.current_system_id = static_cast<std::int16_t>(sys_idx);
  ship.is_active = true;
  ship.ai_behavior_code = 1;
  ship.ai_state_code = 0;
  ship.pos_x = 50000.0F;
  ship.pos_y = 50000.0F;
  ship.armor_points = 30.0F;
  ship.shield_points = 30.0F;
  ship.defense_fleet_home_stellar_id = stellar_id;
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = -1;

  NovaAi_UpdateShipAI(state, ship, /*now_ms=*/0);

  CHECK(ship.ai_state_code == 1);
  CHECK(ship.ai_secondary_target_slot == stellar_id);
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

  // Equal base speeds are where the guided-bank walk matters: the original
  // returns the non-strict `ship <= target` when no bank is found (0x00411183)
  // and the strict `ship < target` when one is (0x004111e0). Clearing the
  // attacker's banks forces the no-bank path, which must return true here.
  state.player.ship_class_id = attacker_class;
  state.player.vel_y = -10.0F;
  attacker.npc_weapon_count_by_class.fill(0);
  attacker.npc_weapon_secondary_count_by_class.fill(0);
  CHECK(NovaAiShip_CanInterceptCurrentPrimaryTarget(state, attacker));
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

  // 0x00412090 has no separate destroyed-candidate rejection. A stellar-bound
  // ship is exempt from the disabled armor gate, so it remains scoreable while
  // active even with zero armor.
  candidate.armor_points = 0.0F;
  candidate.defense_fleet_home_stellar_id = 0x80;
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

TEST_CASE(
    "follow and assist states use the original distance and cloak bands") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  game::Ship &ship = state.ShipAt(1);
  game::Ship &leader = state.ShipAt(2);
  ship.is_active = true;
  ship.ship_instance_id = 1;
  ship.ship_class_id = 0;
  ship.squad_leader_ship_slot = 2;
  ship.defense_fleet_home_stellar_id = -1;
  leader.is_active = true;
  leader.ship_instance_id = 2;
  leader.ship_class_id = 0;
  leader.armor_points = 100.0F;

  // State 10 uses velocity/formation approach from 300..600 px and the
  // long-range pursuit outside 600 px.
  ship.ai_state_code = 10;
  leader.pos_x = 400.0F;
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 0xb);
  leader.pos_x = 700.0F;
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 9);

  // A cloaked leader that this follower cannot engage forces state 5 to
  // brake, and state 10 switches to pursuit beyond the 300 px inner band.
  leader.cloak_fade_progress = 17.0F;
  leader.pos_x = 400.0F;
  ship.ai_state_code = 5;
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 1);
  ship.ai_state_code = 10;
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 9);
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

TEST_CASE("cloak maintain gate honors the device ModVal drain bits") {
  // BUGFIX(original): the original reads the fuel/shield gate nibbles from the
  // matched ModType word (always 0x11), so every cloak demands fuel and none is
  // gated on shields. Under kApplyOriginalBugFixes the real ModVal nibbles are
  // used; see docs/known_original_bugs.md.
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(!state.scenario.ships.empty());

  auto &ship_class = state.scenario.ships[0];
  ship_class.default_outfit_ids[0] = 0x80;
  ship_class.default_outfit_counts[0] = 1;
  auto &cloaking_device = state.scenario.outfits[0];
  cloaking_device.mod_type = 0x11;

  game::Ship ship;
  ship.ship_instance_id = 1; // NPC: ship class default loadout
  ship.ship_class_id = 0;
  ship.armor_points = 100.0F;
  ship.fuel_points = 0.0F;
  ship.shield_points = 0.0F;

  // Neither drain nibble set: both gates are off, so empty resources pass.
  cloaking_device.mod_val = 0x0000;
  CHECK(game::NovaAiShip_CanMaintainCloakState(state, ship));

  // Fuel-drain bit set with no fuel: cannot maintain.
  cloaking_device.mod_val = 0x0010;
  CHECK_FALSE(game::NovaAiShip_CanMaintainCloakState(state, ship));

  // Shield-drain bit set with no shields: cannot maintain, even with fuel.
  cloaking_device.mod_val = 0x0100;
  ship.fuel_points = 100.0F;
  CHECK_FALSE(game::NovaAiShip_CanMaintainCloakState(state, ship));
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
  NovaAi_UpdateShipAI(state, ship, /*now_ms=*/0);

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

  game::NovaAi_EnterState15EmergeFromHypergate(state, ship, stellar_id);

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

  // Simulate expiry of the 60-tick emergence hold. The original falls through
  // to the state-8 arm in the same pass, immediately installing the -999
  // sentinel and mode 0x0a. Mode 0x0a must preserve the already-negative -30
  // override instead of replacing it with -50.
  ship.ai_maneuver_timer_ms = 0.0F;
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  REQUIRE(ship.ai_state_code == 8);
  CHECK(ship.ai_station_hold_timer == Catch::Approx(-999.0F));
  REQUIRE(ship.ai_control_mode == 10);
  game::NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_desired_speed == Catch::Approx(-30.0F));
  CHECK(ship.ai_forward_thrust_cmd == Catch::Approx(-1.165F));

  game::Ship player_follower;
  player_follower.ship_class_id = 0;
  player_follower.squad_leader_ship_slot = 0;
  game::NovaAi_EnterState15EmergeFromHypergate(
      state, player_follower, stellar_id);
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
  // A jump neither requires nor consumes NPC fuel: a fleet lead spawned with
  // an empty tank still transfers, and the tank is unchanged afterward.
  ship.fuel_points = 0.0F;

  REQUIRE(game::NovaAi_CompleteNpcJump(state, ship));
  CHECK(ship.current_system_id == destination_system);
  CHECK(ship.fuel_points == Catch::Approx(0.0F));
  CHECK(ship.ai_state_code == 0x15);
  CHECK(ship.ai_control_mode == 0);
  CHECK(ship.vel_x == Catch::Approx(0.0F));
  CHECK(ship.vel_y == Catch::Approx(0.0F));
}

// The shared NPC jump gate (0x00415b80) does NOT require current fuel for
// ordinary ships: a class able to carry a jump may initiate one with an empty
// tank. Only personalities with Flags2 0x0001 ("starts with zero fuel") are
// held back until the tank holds a jump's worth. The velocity-match lock also
// refuses the jump.
TEST_CASE("jump gate only requires current fuel for zero-fuel personalities") {
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
  npc.ship_instance_id = 3;
  npc.ship_class_id = cls_zero;
  npc.pers_def_slot = -1;
  npc.fuel_points = 0.0F; // empty tank, ordinary ship
  CHECK(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));

  // Velocity-matched to another ship: refused; matched to itself: allowed.
  npc.velocity_match_target_ship_slot = 4;
  CHECK_FALSE(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));
  npc.velocity_match_target_ship_slot = npc.ship_instance_id;
  CHECK(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));
  npc.velocity_match_target_ship_slot = -1;

  REQUIRE(!state.scenario.pers_defs.empty());
  state.scenario.pers_defs.front().flags_secondary = 0x0001;
  npc.pers_def_slot = 0;
  CHECK_FALSE(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));
  npc.fuel_points = game::kJumpFuelCost - 1.0F;
  CHECK_FALSE(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));
  npc.fuel_points = game::kJumpFuelCost;
  CHECK(game::NovaTravel_CanShipInitiateJumpSequence(state, npc));
}

// Behavior 0x03 (0x00402e50) state-1/0x14/2 arm: a traveling warship must
// re-scan for a hostile contact each tick instead of ignoring it until the
// travel ladder settles. A Flags-1 grudge personality makes acquisition
// deterministic against the player.
TEST_CASE("warship behavior reacquires a target from travel states") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Free-energy weapon so the supervisor's trailing ammo-out arm does not
  // immediately clear the freshly acquired attack state.
  int weapon_bank = -1;
  for (std::size_t w = 0; w < state.scenario.weapons.size(); ++w) {
    if (state.scenario.weapons[w].ammo_type >= -1000 &&
        state.scenario.weapons[w].ammo_type <= -1) {
      weapon_bank = static_cast<int>(w);
      break;
    }
  }
  REQUIRE(weapon_bank >= 0);

  state.scenario.pers_defs.resize(1);
  game::PersDef &pers = state.scenario.pers_defs[0];
  pers.alive = true;
  pers.grudge = true;
  pers.flags_primary = 0x0001;

  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.current_system_id = 0;
  state.player.armor_points = 30.0F;
  state.player.shield_points = 30.0F;

  const int slot = NovaShip_AllocateShipSlot(state, 0, 0);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.current_system_id = 0;
  ship.pers_def_slot = 0;
  ship.ai_behavior_code = 3;
  ship.armor_points = 1000.0F;
  ship.npc_weapon_count_by_class[static_cast<std::size_t>(weapon_bank)] = 1;

  for (const std::int16_t state_code : {1, 0x14, 2}) {
    ship.primary_target_ship_slot = -1;
    ship.ai_state_code = state_code;
    game::NovaAi_UpdateBehavior0x03(state, ship);
    CHECK(ship.primary_target_ship_slot == 0);
    CHECK(ship.ai_state_code == 4);
  }
}

// Behavior 0x03 state-6 arm: a parked warship with no target and no attached
// behavior-5 fighter returns to state 0 so the next supervisor pass can travel;
// an attached fighter with no stellar home holds it in state 6.
TEST_CASE("warship behavior leaves state 6 once no fighters remain") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  state.player.is_active = false;
  const int slot = NovaShip_AllocateShipSlot(state, 0, 0);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.current_system_id = 0;
  ship.faction_or_government_id = -1;
  ship.ai_behavior_code = 3;
  ship.ai_state_code = 6;
  ship.primary_target_ship_slot = -1;
  ship.armor_points = 1000.0F;

  game::NovaAi_UpdateBehavior0x03(state, ship);
  CHECK(ship.ai_state_code == 0);

  const int fighter_slot = NovaShip_AllocateShipSlot(state, 0, 0);
  REQUIRE(fighter_slot > 0);
  game::Ship &fighter = state.ShipAt(static_cast<std::size_t>(fighter_slot));
  fighter.current_system_id = 0;
  fighter.faction_or_government_id = -1;
  fighter.ai_behavior_code = 5;
  fighter.defense_fleet_home_stellar_id = -1;
  fighter.squad_leader_ship_slot = ship.ship_instance_id;
  fighter.armor_points = 1000.0F;

  ship.ai_state_code = 6;
  game::NovaAi_UpdateBehavior0x03(state, ship);
  CHECK(ship.ai_state_code == 6);
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
  ship.defense_fleet_home_stellar_id = -1;
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

// Pins scenario class 0 to ShipClass Flags2 0x0001 (swarming, no standoff) so
// the swarm-mate tests do not depend on which shipped classes carry the bit.
// Each test owns its freshly loaded ScenarioData.
void MarkClass0Swarming(GameState &state) {
  state.scenario.ships[0].flags_secondary = 0x0001;
}

// Spawns an active, in-system ship whose target/faction/leader context the
// caller can then shape.
game::Ship &SpawnSwarmTestShip(GameState &state, int sys_idx) {
  game::Ship &ship = SpawnCombatTestShip(state, sys_idx, 0);
  ship.is_active = true;
  ship.current_system_id = static_cast<std::int16_t>(sys_idx);
  return ship;
}

} // namespace

TEST_CASE("ApplyControls mode 0xd formation release uses raw-call cadence") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);

  game::Ship &leader = SpawnCombatTestShip(state, sys_idx, 0);
  game::Ship &follower = SpawnCombatTestShip(state, sys_idx, 0);
  follower.ai_control_mode = 0x0d;
  follower.squad_leader_ship_slot = leader.ship_instance_id;
  follower.ai_station_hold_timer = 1.0F;
  leader.ai_station_hold_timer = 2.0F;
  leader.heading = 0.0F;
  leader.ai_desired_heading_deg = 0;

  // Half of an original 21 ms call advances half a raw-call unit. The release
  // test precedes the increment in the original, so crossing 30 does not
  // release until the following update.
  NovaAi_ApplyControls(state, follower, /*elapsed_ticks=*/0.315F);
  CHECK(follower.ai_station_hold_timer == Catch::Approx(1.5F));

  follower.ai_station_hold_timer = 30.0F;
  NovaAi_ApplyControls(state, follower, /*elapsed_ticks=*/0.63F);
  CHECK(follower.ai_station_hold_timer == Catch::Approx(31.0F));
  CHECK(follower.ai_control_mode == 0x0d);
  NovaAi_ApplyControls(state, follower, /*elapsed_ticks=*/0.63F);
  CHECK(follower.squad_leader_ship_slot == -1);
  CHECK(follower.ai_state_code == 2);
  CHECK(follower.ai_control_mode == 4);
}

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

  NovaAi_ApplyControls(state, ship, 1.0F);

  // Player is east of the ship: mode 5 must steer west (270 deg), not east.
  REQUIRE(ship.ai_desired_heading_deg == 270);
  // Once aligned, mode 5 applies the raw effective thrust.
  ship.heading = static_cast<float>(ship.ai_desired_heading_deg) *
                 (3.14159265358979323846F / 180.0F);
  NovaAi_ApplyControls(state, ship, 1.0F);
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
  NovaAi_ApplyControls(state, ship, 1.0F);
  REQUIRE(ship.ai_desired_heading_deg == 90);
  CHECK(ship.ai_forward_thrust_cmd ==
        Catch::Approx(eff.thrust_px_per_tick2).margin(1e-4F));
  CHECK(ship.ai_desired_speed == 0.0F);

  // Mode 0x10: evasive break on the stored heading at 1.5x thrust.
  ship.ai_control_mode = 0x10;
  ship.ai_evasive_heading_deg = 200;
  ship.heading = 200.0F * (3.14159265358979323846F / 180.0F);
  NovaAi_ApplyControls(state, ship, 1.0F);
  REQUIRE(ship.ai_desired_heading_deg == 200);
  CHECK(ship.ai_forward_thrust_cmd ==
        Catch::Approx(eff.thrust_px_per_tick2 * 1.5F).margin(1e-4F));
  CHECK(ship.ai_desired_speed == 0.0F);

  // Mode 0x11: boost at 2.75x thrust, cruise 1.8x max speed.
  ship.ai_control_mode = 0x11;
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_forward_thrust_cmd ==
        Catch::Approx(eff.thrust_px_per_tick2 * 2.75F).margin(1e-4F));
  CHECK(ship.ai_desired_speed ==
        Catch::Approx(eff.max_speed_px_per_tick * 1.8F).margin(1e-3F));
}

// Regression: Ship_ApplyShipAiControls (0x00408150) mode 6 unconditionally
// calls Weapon_FireTurretAtTarget (0x0040ce00) at 0x00409163 --
// after the aim block and before the turn+15 thrust gate -- so the primary
// target's bank is armed even when the hull only carries turrets. The port
// omitted that call, leaving a turret-only mode-6 ship (e.g. the Pirate
// Enterprise "Stolen Tech" mode-7 railgun) with active_weapon_bank_slot == -1:
// the direct-fire selector (0x0040d470) accepts only modes -1/0/6 (+1 guided)
// and so can never arm a mode-7/8 bank.
TEST_CASE("ApplyControls mode 6 arms a turret bank via the current-target "
          "selector") {
  GameState state;
  state.scenario.ships.resize(1);
  state.scenario.weapons.resize(1);
  state.scenario.weapons[0].name = "SyntheticTurret";
  state.scenario.weapons[0].weapon_mode_code = 7;
  state.scenario.weapons[0].ammo_type = -1; // energy: no secondary counter
  state.scenario.weapons[0].mass_damage = 10;
  state.scenario.weapons[0].energy_damage = 10;
  state.scenario.weapons[0].range_scalar = 1000.0F;

  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.ship_class_id = 0;
  state.player.current_system_id = 0;
  state.player.armor_points = 30.0F;
  state.player.shield_points = 30.0F;
  state.player.pers_def_slot = 0x3ff; // bypass the cloak-engagement gate
  state.player.pos_x = 0.0F;
  state.player.pos_y = -100.0F; // due north of the shooter

  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.ship_instance_id = 1;
  ship.ship_class_id = 0;
  ship.current_system_id = 0;
  ship.ai_control_mode = 6;
  ship.primary_target_ship_slot = 0;
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.heading = 0.0F; // already on the target bearing
  ship.armor_points = 100.0F;
  ship.shield_points = 100.0F;
  ship.defense_fleet_home_stellar_id = -1;
  ship.active_weapon_bank_slot = -1;
  ship.ai_fire_trigger_latch = 0;
  ship.npc_weapon_count_by_class.fill(0);
  ship.npc_weapon_secondary_count_by_class.fill(0);
  ship.npc_weapon_bank_cooldown.fill(0.0F);
  ship.npc_weapon_count_by_class[0] = 1;

  NovaAi_ApplyControls(state, ship, 1.0F);

  // The mode-7 bank is armed and the fire latch raised by the current-target
  // selector; the direct-fire selector would leave both untouched.
  REQUIRE(ship.active_weapon_bank_slot == 0);
  CHECK(ship.ai_fire_trigger_latch != 0);
}

// Ship_CanShipUseAfterburner (0x0046b260): capability 0x0020 rolls
// NovaRandom_Range(0x540) and passes when roll + 0x100 <= rating / base
// strength. The base unit is pinned to GameState::kCombatRatingBaseStrength
// (2; the original reads class-0 Strength, 0x0046b303-0x0046b316). With base
// 2 the gate is deterministic at the ends: always false below rating 512,
// always true at rating >= 3200 (roll+0x100 in [256,1599]).
TEST_CASE("afterburner rating gate uses the pinned base strength") {
  const auto can_use_afterburner = [](std::int32_t rating,
                                      std::int16_t own_class_strength,
                                      std::int16_t class0_strength) {
    GameState state;
    state.scenario.ships.resize(2);
    state.scenario.ships[0].strength = class0_strength;
    state.scenario.ships[1].strength = own_class_strength;
    state.scenario.ships[1].capability_flags = 0x0020U;
    game::Ship &ship = state.ShipAt(1);
    ship.is_active = true;
    ship.ship_instance_id = 1;
    ship.ship_class_id = 1; // distinct from class 0 so own vs base differ
    ship.armor_points = 100.0F;
    ship.shield_points = 100.0F;
    state.player_combat_rating_points = rating;
    return game::NovaShip_CanShipUseAfterburner(state, ship);
  };

  CHECK_FALSE(can_use_afterburner(511, 500, 2)); // 511/2 = 255 < 256
  CHECK(can_use_afterburner(3200, 500, 2));      // 3200/2 = 1600 >= 1599
  // The divisor is the pinned base, not the ship's own class strength: with
  // the old own-class read, 3200/10000 would truncate to 0 and fail.
  CHECK(can_use_afterburner(3200, 10000, 2));
  // Pinned divergence: editing class-0 Strength must not move the gate.
  CHECK_FALSE(can_use_afterburner(511, 500, 100));
  CHECK(can_use_afterburner(3200, 10000, 100));
}

// Ship_UpdateShipCombatOddsScore (0x004133f0) scales the player's contribution
// to the hostile strength by clamp(rating / (base * 6400), 1, 2), with the
// base unit pinned to GameState::kCombatRatingBaseStrength (2; the original
// reads class-0 Strength at 0x0041343c). Player class and NPC class both have
// Strength 100, so the resulting odds equal the rating scale directly.
TEST_CASE("combat-odds player scaling uses the pinned base strength") {
  const auto odds = [](std::int32_t rating, std::int16_t class0_strength) {
    GameState state;
    state.scenario.ships.resize(3);
    state.scenario.ships[0].strength = class0_strength;
    state.scenario.ships[1].strength = 100; // NPC
    state.scenario.ships[2].strength = 100; // player

    game::Ship &npc = state.ShipAt(1);
    npc.is_active = true;
    npc.ship_instance_id = 1;
    npc.ship_class_id = 1;
    npc.primary_target_ship_slot = 0;
    npc.ai_state_code = 4;
    npc.armor_points = 100.0F;
    npc.shield_points = 100.0F;
    npc.mission_fleet_slot = -1;
    npc.defense_fleet_home_stellar_id = -1;

    state.player.is_active = true;
    state.player.ship_instance_id = 0;
    state.player.ship_class_id = 2;
    state.player.armor_points = 100.0F;
    state.player.shield_points = 100.0F;
    state.player_combat_rating_points = rating;

    game::NovaAi_UpdateShipCombatOddsScore(state, npc);
    return npc.ai_odds_score;
  };

  // Divisor = 2 * 6400 = 12800, clamped scale [1,2].
  CHECK(odds(0, 100) == Catch::Approx(1.0F));
  CHECK(odds(12800, 100) == Catch::Approx(1.0F));
  CHECK(odds(25600, 100) == Catch::Approx(2.0F));
  CHECK(odds(100000, 100) == Catch::Approx(2.0F));
  // Pinned: with class-0 Strength 50 the original divisor would be 320000
  // and rating 25600 would clamp to scale 1 (odds 1.0).
  CHECK(odds(25600, 50) == Catch::Approx(2.0F));
}

// The 0x004133f0 gate passes (candidate = ship, acquirer = player) to
// Ship_IsShipAcquirableAsTarget and tests its low byte: the player's rating-
// scaled strength is added to the hostile numerator only when the ship's
// government carries no player rank/commission (policy flag 0 clear) AND the
// ship is threatening the player squad.
TEST_CASE("combat-odds player contribution is suppressed for rank-friendly "
          "governments") {
  GameState state;
  state.scenario.ships.resize(3);
  state.scenario.ships[0].strength = 2;   // class-0 base (pinned divisor)
  state.scenario.ships[1].strength = 100; // NPC
  state.scenario.ships[2].strength = 100; // player

  // Player holds a rank/commission with government 0 -> policy flag 0 set.
  state.scenario.governments.resize(1);
  state.scenario.governments[0].policy_flags[0] = 1;

  game::Ship &npc = state.ShipAt(1);
  npc.is_active = true;
  npc.ship_instance_id = 1;
  npc.ship_class_id = 1;
  npc.faction_or_government_id = 0;
  npc.primary_target_ship_slot = 0; // actively threatening the player
  npc.ai_state_code = 4;
  npc.armor_points = 100.0F;
  npc.shield_points = 100.0F;
  npc.mission_fleet_slot = -1;
  npc.defense_fleet_home_stellar_id = -1;

  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.ship_class_id = 2;
  state.player.armor_points = 100.0F;
  state.player.shield_points = 100.0F;
  state.player_combat_rating_points = 100000; // would clamp to 2x if counted

  game::NovaAi_UpdateShipCombatOddsScore(state, npc);
  // Flag set -> ship not acquirable by the player -> player term suppressed.
  CHECK(npc.ai_odds_score == Catch::Approx(0.0F));

  // Clearing the flag re-enables it: 2 * 100 / 100 = 2.0.
  state.scenario.governments[0].policy_flags[0] = 0;
  game::NovaAi_UpdateShipCombatOddsScore(state, npc);
  CHECK(npc.ai_odds_score == Catch::Approx(2.0F));
}

// Ship_ApplyShipAiControls (0x00408150) reaches for the +0xC8DA last
// lead-fired bank in the mode-6/7/0xe aim blocks: mode 6 leads with the active
// bank when its weapon mode is {-1,6} and otherwise falls back to +0xC8DA,
// mode 7 leads with the active bank only when +0xC8DA is set, and the mode-0xe
// fast branch leads with +0xC8DA when set, else reverses the velocity bearing.
TEST_CASE("ApplyControls lead aim uses the last lead-fired bank") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);

  // Player (target) due north of the shooter, moving east; the shooter starts
  // stationary. A lead-capable bank therefore aims off the due-north bearing.
  ActivatePlayer(state, 0.0F, -100.0F);
  state.player.vel_x = 100.0F;
  state.player.vel_y = 0.0F;

  REQUIRE(state.scenario.weapons.size() >= 2);
  constexpr std::int16_t kLeadBank = 0;  // resolves to weapons[0]
  constexpr std::int16_t kOtherBank = 1; // resolves to weapons[1]
  state.scenario.weapons[kLeadBank].weapon_mode_code =
      -1; // straight projectile
  state.scenario.weapons[kLeadBank].projectile_speed = 100.0F;
  state.scenario.weapons[kOtherBank].weapon_mode_code = 1; // no lead
  state.scenario.weapons[kOtherBank].projectile_speed = 100.0F;

  game::Ship &ship = SpawnCombatTestShip(state, sys_idx, 0);
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;
  ship.primary_target_ship_slot = 0;
  ship.heading = 0.0F;
  ship.ai_desired_heading_deg = 0;

  const std::int16_t straight =
      game::NovaAi_AimWeaponPredictive(state, ship, state.player, -1);
  const std::int16_t lead =
      game::NovaAi_AimWeaponPredictive(state, ship, state.player, kLeadBank);
  REQUIRE(lead != straight);

  // Mode 6: no active bank leads with +0xC8DA when set, else straight.
  ship.ai_control_mode = 6;
  ship.active_weapon_bank_slot = -1;
  ship.last_fired_weapon_bank_slot = -1;
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_desired_heading_deg == straight);

  ship.last_fired_weapon_bank_slot = kLeadBank;
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_desired_heading_deg == lead);

  // Mode 6: a non-lead active bank wins over +0xC8DA; a lead active bank
  // leads even with +0xC8DA unset.
  ship.active_weapon_bank_slot = kOtherBank;
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_desired_heading_deg == straight);

  ship.active_weapon_bank_slot = kLeadBank;
  ship.last_fired_weapon_bank_slot = -1;
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_desired_heading_deg == lead);

  // Mode 7: aims with the active bank but only when +0xC8DA is set.
  ship.ai_control_mode = 7;
  ship.active_weapon_bank_slot = kLeadBank;
  ship.last_fired_weapon_bank_slot = -1;
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_desired_heading_deg == straight);

  ship.last_fired_weapon_bank_slot = kLeadBank;
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_desired_heading_deg == lead);

  // Mode 0xe fast branch (force the class off the inertialess model): +0xC8DA
  // selects the lead, else the reverse-velocity bearing.
  state.scenario.ships[static_cast<std::size_t>(ship.ship_class_id)]
      .flags_secondary &= ~0x40U;
  ship.ai_control_mode = 0xe;
  ship.vel_x = 1.0F; // >= the 0.35 px/tick fast gate
  ship.vel_y = 0.0F;
  ship.last_fired_weapon_bank_slot = -1;
  NovaAi_ApplyControls(state, ship, 1.0F);
  const std::int16_t reverse = static_cast<std::int16_t>(game::WrapDeg(
      game::BearingDeg(0.0F, 0.0F, ship.vel_x, ship.vel_y) + 180.0F));
  CHECK(ship.ai_desired_heading_deg == reverse);

  const std::int16_t lead_moving =
      game::NovaAi_AimWeaponPredictive(state, ship, state.player, kLeadBank);
  ship.last_fired_weapon_bank_slot = kLeadBank;
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_desired_heading_deg == lead_moving);
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
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.vel_x == 3.0F); // not copied while outrunning
  // Align with the reverse of the relative-velocity bearing and brake:
  // relative (2.6, 2.2) points ~130.2 deg; reverse is ~310.2 deg.
  ship.heading = 310.2F * (3.14159265358979323846F / 180.0F);
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_forward_thrust_cmd > 0.0F); // braking on the relative velocity

  // Match the velocity: the copy arm runs and snaps velocity to the target.
  ship.vel_x = 0.4F;
  ship.vel_y = -0.2F;
  NovaAi_ApplyControls(state, ship, 1.0F);
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
  NovaAi_ApplyControls(state, ship, 1.0F);
  REQUIRE(ship.ai_desired_heading_deg == 270);
  ship.heading = static_cast<float>(ship.ai_desired_heading_deg) *
                 (3.14159265358979323846F / 180.0F);
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.ai_forward_thrust_cmd > 0.0F);

  // Relative velocity small: match the target's velocity.
  ship.vel_x = 0.1F;
  ship.vel_y = 0.05F;
  NovaAi_ApplyControls(state, ship, 1.0F);
  CHECK(ship.vel_x == 0.1F);
  CHECK(ship.vel_y == 0.0F);
}

// Mode 0x12 (chase swarm mate) falls back to control mode 0 without a mate
// and steers at the point 15x max-speed ahead of the mate's heading otherwise.
TEST_CASE("ApplyControls mode 0x12 chase swarm mate") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  ActivatePlayer(state, 0.0F, 0.0F);

  game::Ship &ship = SpawnCombatTestShip(state, sys_idx, 0);
  ship.ai_control_mode = 0x12;
  ship.swarm_mate_ship_slot = -1;
  NovaAi_ApplyControls(state, ship, 1.0F);
  REQUIRE(ship.ai_control_mode == 0); // no mate: idle control

  ship.ai_control_mode = 0x12;
  // The original treats mate slot < 1 (including the player slot 0) as "no
  // mate", so the chased mate must occupy a real NPC slot.
  const int mate_slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(mate_slot > 0);
  game::Ship &mate = state.ShipAt(static_cast<std::size_t>(mate_slot));
  mate.is_active = true;
  mate.ship_instance_id = static_cast<std::int16_t>(mate_slot);
  ship.swarm_mate_ship_slot = static_cast<std::int16_t>(mate_slot);
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.heading = 0.0F;
  mate.pos_x = 100.0F;
  mate.pos_y = 0.0F;
  mate.heading = 0.0F; // mate facing up: lead point = north of the mate
  NovaAi_ApplyControls(state, ship, 1.0F);
  // Lead point is (100, -15*max) north of the mate; the ship at the origin
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

TEST_CASE("swarm-mate finder pairs lower-indexed swarming hulls") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  ActivatePlayer(state, 0.0F, 0.0F);
  MarkClass0Swarming(state);

  game::Ship &first = SpawnSwarmTestShip(state, sys_idx);
  game::Ship &second = SpawnSwarmTestShip(state, sys_idx);
  game::Ship &third = SpawnSwarmTestShip(state, sys_idx);
  REQUIRE(first.ship_instance_id < second.ship_instance_id);
  REQUIRE(second.ship_instance_id < third.ship_instance_id);

  const std::int16_t shared_target = 0; // the player
  for (game::Ship *ship : {&first, &second, &third}) {
    ship->primary_target_ship_slot = shared_target;
    ship->faction_or_government_id = 5;
  }

  // The highest ship scans strictly downward and caches the first (lowest)
  // lower-indexed match; the starting value is cleared first.
  third.swarm_mate_ship_slot = 99;
  CHECK(game::NovaAi_FindSwarmMate(state, third) == first.ship_instance_id);
  CHECK(third.swarm_mate_ship_slot == first.ship_instance_id);

  // No strictly lower match means the cache is reset to -1.
  first.swarm_mate_ship_slot = 42;
  CHECK(game::NovaAi_FindSwarmMate(state, first) == -1);
  CHECK(first.swarm_mate_ship_slot == -1);

  // A different primary target breaks the pairing.
  second.primary_target_ship_slot = -1;
  CHECK(game::NovaAi_FindSwarmMate(state, second) == -1);

  // A shared squad leader also qualifies when no faction is set.
  second.primary_target_ship_slot = shared_target;
  second.faction_or_government_id = -1;
  first.faction_or_government_id = -1;
  second.squad_leader_ship_slot = 7;
  first.squad_leader_ship_slot = 7;
  CHECK(game::NovaAi_FindSwarmMate(state, second) == first.ship_instance_id);
}

TEST_CASE("swarm-mate validator rejects a drifted cache") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  ActivatePlayer(state, 0.0F, 0.0F);
  MarkClass0Swarming(state);

  game::Ship &mate = SpawnSwarmTestShip(state, sys_idx);
  game::Ship &follower = SpawnSwarmTestShip(state, sys_idx);
  mate.primary_target_ship_slot = 0;
  mate.faction_or_government_id = 5;
  follower.primary_target_ship_slot = 0;
  follower.faction_or_government_id = 5;
  follower.swarm_mate_ship_slot = mate.ship_instance_id;
  CHECK(game::NovaAiShip_IsSwarmMateStillValid(state, follower));

  // The cached swarm mate must stay on the same target.
  mate.primary_target_ship_slot = -1;
  CHECK_FALSE(game::NovaAiShip_IsSwarmMateStillValid(state, follower));
  mate.primary_target_ship_slot = 0;

  // A cache that is not a strictly lower slot is invalid.
  follower.swarm_mate_ship_slot = follower.ship_instance_id;
  CHECK_FALSE(game::NovaAiShip_IsSwarmMateStillValid(state, follower));
  follower.swarm_mate_ship_slot = -1;
  CHECK_FALSE(game::NovaAiShip_IsSwarmMateStillValid(state, follower));

  // A non-swarming hull always reports the cache valid (nothing to maintain).
  state.scenario.ships[0].flags_secondary = 0;
  follower.swarm_mate_ship_slot = -1;
  CHECK(game::NovaAiShip_IsSwarmMateStillValid(state, follower));
}

TEST_CASE("ShouldFollowSwarmMate forces mode 0x12 only for a distinct mate") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  MarkClass0Swarming(state);
  game::Ship &ship = SpawnSwarmTestShip(state, sys_idx);

  ship.swarm_mate_ship_slot = 1;
  ship.squad_leader_ship_slot = -1;
  ship.defense_fleet_home_stellar_id = -1;
  ship.ai_control_mode = 0;
  CHECK(game::NovaAiShip_ShouldFollowSwarmMate(state, ship));
  CHECK(ship.ai_control_mode == 0x12);

  // A cached swarm mate equal to the squad leader is left to the squad modes.
  ship.ai_control_mode = 0;
  ship.squad_leader_ship_slot = 1;
  CHECK_FALSE(game::NovaAiShip_ShouldFollowSwarmMate(state, ship));
  CHECK(ship.ai_control_mode == 0);

  // Defense-fleet ships are excluded even with a distinct swarm mate.
  ship.squad_leader_ship_slot = -1;
  ship.defense_fleet_home_stellar_id = 9;
  CHECK_FALSE(game::NovaAiShip_ShouldFollowSwarmMate(state, ship));
  CHECK(ship.ai_control_mode == 0);

  // Non-swarming hulls never enter the swarm mode.
  ship.defense_fleet_home_stellar_id = -1;
  state.scenario.ships[0].flags_secondary = 0;
  CHECK_FALSE(game::NovaAiShip_ShouldFollowSwarmMate(state, ship));
  CHECK(ship.ai_control_mode == 0);
}

TEST_CASE("state 0x15 emergence skips the swarm-mate refresh") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  ActivatePlayer(state, 0.0F, 0.0F);
  MarkClass0Swarming(state);

  game::Ship &swarm_mate = SpawnSwarmTestShip(state, sys_idx);
  game::Ship &ship = SpawnSwarmTestShip(state, sys_idx);
  CHECK(game::NovaAiShip_IsShipInAiState0x15(ship) == false);
  swarm_mate.primary_target_ship_slot = 0;
  swarm_mate.faction_or_government_id = 5;
  ship.primary_target_ship_slot = 0;
  ship.faction_or_government_id = 5;
  ship.swarm_mate_ship_slot = -1;
  ship.ai_maneuver_timer_ms = 0.0F;
  ship.ai_state_code = 0;

  // Ordinary idle state: the refresh finds the lower-indexed swarm mate.
  game::NovaAi_UpdateShipAI(state, ship, /*now_ms=*/0);
  CHECK(ship.swarm_mate_ship_slot == swarm_mate.ship_instance_id);

  // State 0x15 skips the whole ordinary behavior branch, so even a cache that
  // would be refreshed is left untouched (here deliberately cleared).
  ship.swarm_mate_ship_slot = -1;
  ship.ai_state_code = 0x15;
  CHECK(game::NovaAiShip_IsShipInAiState0x15(ship));
  game::NovaAi_UpdateShipAI(state, ship, /*now_ms=*/0);
  CHECK(ship.swarm_mate_ship_slot == -1);
}

TEST_CASE("state 4 swarm-mate switch outranks the strafe fallback") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  ActivatePlayer(state, 0.0F, 0.0F);
  MarkClass0Swarming(state);

  // Keep the swarm-mate slot distinct from both the ship and the target so
  // the primary-target slot and squad-leader slot can never collide.
  game::Ship &swarm_mate = SpawnSwarmTestShip(state, sys_idx);
  game::Ship &target = SpawnSwarmTestShip(state, sys_idx);
  // A non-player target that is not in state 3 makes the intercept test fail.
  target.ai_state_code = 0;
  game::Ship &ship = SpawnSwarmTestShip(state, sys_idx);
  ship.ai_state_code = 4;
  ship.ai_behavior_code = 3;
  ship.primary_target_ship_slot = target.ship_instance_id;
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  target.pos_x = 1000.0F;
  target.pos_y = 0.0F;
  ship.swarm_mate_ship_slot = swarm_mate.ship_instance_id;
  ship.squad_leader_ship_slot = -1;
  ship.defense_fleet_home_stellar_id = -1;
  ship.ai_control_mode = 0;

  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 0x12);

  // Once the swarm mate is the ship's own squad leader, the ordinary strafe
  // fallback applies instead.
  ship.ai_control_mode = 0;
  ship.ai_state_code = 4;
  ship.primary_target_ship_slot = target.ship_instance_id;
  ship.squad_leader_ship_slot = swarm_mate.ship_instance_id;
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 7);
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
  NovaAi_ApplyControls(state, ship, 1.0F);
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
  constexpr float kOriginalMaxRateFrameTicks = 21.0F * 0.03F;
  const int expected_slowdown_ticks = static_cast<int>(
      std::ceil((50.0F - max_speed) / (1.165F / kOriginalMaxRateFrameTicks)));

  float previous_speed = 51.0F;
  int slowdown_ticks = 0;
  while (ship.ai_state_code == 8 && slowdown_ticks < 64) {
    game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
    REQUIRE(ship.ai_control_mode == 10);
    game::NovaAi_ApplyControls(state, ship, 1.0F);
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

TEST_CASE("state 0x08 without its arrival sentinel returns to idle") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.ship_instance_id = 1;
  ship.ship_class_id = 0;
  ship.armor_points = 30.0F;
  ship.ai_behavior_code = 0;
  ship.ai_state_code = 8;
  ship.ai_control_mode = 10;
  ship.ai_station_hold_timer = 0.0F;

  game::NovaAi_UpdateShipAI(state,
                            ship,
                            /*now_ms=*/0,
                            /*elapsed_ticks=*/1.0F);

  CHECK(ship.ai_state_code == 0);
  CHECK(ship.ai_control_mode == 0);
}

// Mission-fleet jump-in placement leaves the ship in its existing state and
// signals the arrival through station-hold timer -999. Frame_TickSystems must
// call Ship_UpdateShipAI, whose <-900 prologue promotes the ship to state 8
// and bypasses behavior dispatch until the slowdown finishes. Behavior 3 is
// important here: without that bypass it immediately steals state 8 for its
// travel/departure fallback and leaves the 50 px/tick velocity untouched.
TEST_CASE("mission arrival sentinel keeps behavior NPC in slowdown") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.current_system_id = 0;

  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.ship_instance_id = 1;
  ship.current_system_id = 0;
  ship.ship_class_id = 0;
  ship.armor_points = 30.0F;
  ship.ai_behavior_code = 3;
  ship.ai_state_code = 0;
  ship.ai_station_hold_timer = -999.0F;
  ship.heading = 0.0F;
  ship.vel_y = -50.0F;

  game::NovaShip_TickNpcAi(state, 1.0F);

  CHECK(ship.ai_state_code == 8);
  CHECK(ship.ai_control_mode == 10);
  CHECK(ship.ai_desired_speed == Catch::Approx(-50.0F));
  CHECK(ship.ai_forward_thrust_cmd == Catch::Approx(-1.165F));

  game::NovaShip_TickNpcShips(state, 1.0F);
  game::NovaShip_TickNpcAi(state, 1.0F);
  CHECK(ship.ai_state_code == 8);
  CHECK(ship.ai_control_mode == 10);
  CHECK(ship.ai_desired_speed < -40.0F);
}

TEST_CASE("state 2 fast-jump class and default outfit skip the outward brake") {
  GameState state;
  state.scenario.ships.resize(1);
  state.scenario.ships[0].flags_secondary = 0x0020;
  // A default-loadout outfit whose ModType 37 sits in an alternate slot and
  // whose count is initially 0 (not owned/equipped).
  state.scenario.outfits.resize(1);
  state.scenario.outfits[0].alt_mod_types[0] = 37;
  state.scenario.ships[0].default_outfit_ids[0] = 0x80;
  state.scenario.ships[0].default_outfit_counts[0] = 0;

  game::Ship &ship = state.ShipAt(1);
  ship.ship_instance_id = 1;
  ship.ship_class_id = 0;
  ship.ai_state_code = 2;
  ship.ai_control_mode = 1;
  ship.pos_x = 1100.0F;
  ship.vel_x = 10.0F;

  // Class Flags2 0x0020: fast jump, brake bypassed.
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 4);

  // Flag cleared and default outfit count 0: ordinary outward brake.
  state.scenario.ships[0].flags_secondary = 0;
  ship.ai_control_mode = 1;
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 1);

  // Default outfit count 1 (alternate ModType 37): fast jump.
  state.scenario.ships[0].default_outfit_counts[0] = 1;
  ship.ai_control_mode = 1;
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 4);
}

// State 3's attack jump-departure arm (Ghidra 0x00406720): a moving attacker
// with a valid target far from the system centre normally holds control mode 1
// (seek); the fast-jump capability selects mode 4 (outward thrust) instead.
TEST_CASE("state 3 fast-jump arm selects outward thrust while moving") {
  GameState state;
  state.scenario.ships.resize(1);
  state.scenario.ships[0].base_fuel = 100; // one jump
  state.scenario.ships[0].flags_secondary = 0x0020;

  state.player.is_active = true;
  state.player.armor_points = 100.0F;
  state.player.death_timer_active = -1.0F;
  state.player.pos_x = 5000.0F; // outside the combat station range

  game::Ship &ship = state.ShipAt(1);
  ship.ship_instance_id = 1;
  ship.ship_class_id = 0;
  ship.ai_state_code = 3;
  ship.ai_control_mode = 1;
  ship.primary_target_ship_slot = 0;
  ship.pos_x = 1100.0F; // far from the centre
  ship.vel_x = 10.0F;   // still moving

  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 4);

  // Without the capability the moving attacker keeps seeking (mode 1).
  state.scenario.ships[0].flags_secondary = 0;
  ship.ai_control_mode = 1;
  game::NovaAi_UpdateShipState(state, ship, /*now_ms=*/0);
  CHECK(ship.ai_control_mode == 1);
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

namespace {

// Hand-built carrier scenario: class 0 is the carrier (optionally Flags2
// 0x0002 standoff), class 1 is the carried fighter/target, and the weapon
// table holds a mode-99 bay (bank 0) plus a mode -1 free-energy standoff
// weapon (bank 1). Only two classes avoid the stock-archive dependency.
constexpr std::int16_t kBayBank = 0;
constexpr std::int16_t kStandoffBank = 1;

void BuildCarrierScenario(GameState &state,
                          bool standoff_class,
                          float standoff_range_scalar) {
  state.scenario.ships.resize(4);
  state.scenario.ships[0].base_shield = 100;
  state.scenario.ships[0].base_armor = 100;
  state.scenario.ships[0].turn_rate = 10.0F;
  if (standoff_class) {
    state.scenario.ships[0].flags_secondary |= 0x0002U;
  }
  state.scenario.ships[1].base_shield = 10;
  state.scenario.ships[1].base_armor = 10;

  state.scenario.weapons.resize(2);
  state.scenario.weapons[kBayBank].weapon_mode_code = 99;
  state.scenario.weapons[kBayBank].ammo_type = 0x80 + 1; // fighter class 1
  state.scenario.weapons[kBayBank].reload_ticks = 2;
  state.scenario.weapons[kStandoffBank].weapon_mode_code = -1;
  state.scenario.weapons[kStandoffBank].ammo_type = -1; // free energy
  state.scenario.weapons[kStandoffBank].range_scalar = standoff_range_scalar;
}

// Allocate a live carrier carrying two fighters in the loaded bay.
game::Ship &MakeLoadedCarrier(GameState &state) {
  const int slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  game::Ship &carrier = state.ShipAt(static_cast<std::size_t>(slot));
  carrier.ship_class_id = 0;
  carrier.ai_state_code = 4;
  carrier.ai_behavior_code = 3;
  carrier.armor_points = 100.0F;
  carrier.shield_points = 100.0F;
  carrier.npc_weapon_count_by_class[kBayBank] = 1;
  carrier.npc_weapon_secondary_count_by_class[kBayBank] = 2;
  return carrier;
}

[[nodiscard]] int ActiveShipCount(const GameState &state) {
  int n = 0;
  for (std::size_t i = 1; i < GameState::kMaxShips; ++i) {
    if (state.ShipAt(i).is_active) {
      ++n;
    }
  }
  return n;
}

} // namespace

// Ghidra 0x00405590 state-4 valid-target tail (LAB_00406bc2): the carrier-bay
// launch runs every frame regardless of the selected control mode. A close
// target selects mode 6 (non-standoff class); the original also launched in
// modes 5/7. Regression for the misplaced call that used to live in the
// state-0xd boarding block.
TEST_CASE("state-4 carrier launches fighters for a non-0xe control mode",
          "[ai][carrier]") {
  GameState state;
  BuildCarrierScenario(state,
                       /*standoff_class=*/false,
                       /*standoff_range_scalar=*/0.0F);
  game::Ship &carrier = MakeLoadedCarrier(state);
  carrier.pos_x = 0.0F;
  carrier.pos_y = 0.0F;

  const int target_slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  game::Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  target.ship_class_id = 1;
  target.armor_points = 10.0F;
  target.pos_x = 10.0F; // inside 165 px -> mode 6
  target.pos_y = 0.0F;
  carrier.primary_target_ship_slot = static_cast<std::int16_t>(target_slot);

  const int before = ActiveShipCount(state);
  game::NovaAi_UpdateShipState(state, carrier, /*now_ms=*/0);

  CHECK(carrier.ai_control_mode == 6);
  CHECK(ActiveShipCount(state) == before + 1);
  CHECK(carrier.npc_weapon_secondary_count_by_class[kBayBank] == 1);
  bool spawned_fighter = false;
  for (std::size_t i = 1; i < GameState::kMaxShips; ++i) {
    const game::Ship &s = state.ShipAt(i);
    if (s.is_active && s.ai_behavior_code == 5 &&
        s.squad_leader_ship_slot == carrier.ship_instance_id) {
      spawned_fighter = true;
    }
  }
  CHECK(spawned_fighter);
}

// The original state-0xd capture approach has no launch call; an armed carrier
// boarding a disabled ship must not deploy its bay.
TEST_CASE("state-0xd boarding approach does not launch carried fighters",
          "[ai][carrier]") {
  GameState state;
  BuildCarrierScenario(state,
                       /*standoff_class=*/false,
                       /*standoff_range_scalar=*/0.0F);
  game::Ship &carrier = MakeLoadedCarrier(state);
  carrier.ai_state_code = 0xd;
  carrier.pos_x = 0.0F;
  carrier.pos_y = 0.0F;

  const int target_slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  game::Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  target.ship_class_id = 1;
  target.armor_points = 1.0F; // below 1/3 of the fighter class's 10
  target.pos_x = 5.0F;
  target.pos_y = 0.0F;
  carrier.primary_target_ship_slot = static_cast<std::int16_t>(target_slot);
  REQUIRE(game::NovaAiShip_IsDisabled(state, target));

  const int before = ActiveShipCount(state);
  game::NovaAi_UpdateShipState(state, carrier, /*now_ms=*/0);

  CHECK(carrier.ai_control_mode == 0xf);
  CHECK(ActiveShipCount(state) == before);
  CHECK(carrier.npc_weapon_secondary_count_by_class[kBayBank] == 2);
}

// The Flags2 0x0002 standoff arm selects the control mode from the truncated
// 0.85x max weapon reach (0x00406a54..0x00406b4d); a disabled target halves
// that reach before the mode decision. The 404/172 case pins the signed-short
// truncation of the halved envelope (trunc(343*0.5)=171, not 172).
TEST_CASE("state-4 standoff class selects mode from weapon range",
          "[ai][carrier]") {
  GameState state;
  BuildCarrierScenario(state,
                       /*standoff_class=*/true,
                       /*standoff_range_scalar=*/1000.0F);

  const int carrier_slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  game::Ship &carrier = state.ShipAt(static_cast<std::size_t>(carrier_slot));
  carrier.ship_class_id = 0;
  carrier.ai_state_code = 4;
  carrier.ai_behavior_code = 3;
  carrier.armor_points = 100.0F;
  carrier.shield_points = 100.0F;
  carrier.npc_weapon_count_by_class[kStandoffBank] = 1;
  carrier.pos_x = 0.0F;
  carrier.pos_y = 0.0F;

  const int target_slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  game::Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  target.ship_class_id = 1;
  target.armor_points = 10.0F;
  target.pos_x = 400.0F;
  target.pos_y = 0.0F;
  carrier.primary_target_ship_slot = static_cast<std::int16_t>(target_slot);

  SECTION("reach beyond the target selects mode 0xe") {
    game::NovaAi_UpdateShipState(state, carrier, /*now_ms=*/0);
    CHECK(carrier.ai_control_mode == 0xe);
  }

  SECTION("short reach selects mode 7") {
    state.scenario.weapons[kStandoffBank].range_scalar = 100.0F;
    game::NovaAi_UpdateShipState(state, carrier, /*now_ms=*/0);
    CHECK(carrier.ai_control_mode == 7);
  }

  SECTION("disabled target halves the truncated reach") {
    state.scenario.weapons[kStandoffBank].range_scalar = 404.0F;
    target.armor_points = 1.0F;
    REQUIRE(game::NovaAiShip_IsDisabled(state, target));
    target.pos_x = 172.0F;
    game::NovaAi_UpdateShipState(state, carrier, /*now_ms=*/0);
    CHECK(carrier.ai_control_mode == 7);
  }
}

// End-to-end capture-approach drive (Ghidra 0x004038b0 supervisor -> 0x00405590
// state 0xd -> 0x00408150 mode 0xf -> Boarding_BoardShipAndTransferCargo): a
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
  boarder
      .npc_weapon_count_by_class[static_cast<std::size_t>(free_energy_bank)] =
      1;
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
  bool armed_at_disabled_victim = false;
  std::uint32_t now_ms = 0;
  for (int t = 0; t < 600 && !handed_off; ++t) {
    game::NovaAi_UpdateShipAI(state, boarder, now_ms);
    now_ms += 33;
    // Regression: the behavior-3 capture drive must never arm its weapons
    // against the disabled boarding victim. Ship_EscortFireAtUnprovokedTarget
    // returns for ai_behavior_code < 5 and clears disabled targets.
    if (boarder.ai_fire_trigger_latch != 0) {
      armed_at_disabled_victim = true;
    }
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
  CHECK_FALSE(armed_at_disabled_victim);

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

TEST_CASE("control-mode-4 jump spin-up is not re-stamped by the capture "
          "supervisor",
          "[ai][boarding]") {
  // Regression for the mode-4 jump spin-up wedge: the original bypasses the
  // behavior supervisor entirely while the entry control mode is 4/0xd
  // (Ship_UpdateShipAI 0x00401000, disasm 0x004011de -> 0x004016b4), so a
  // capture/plunder behavior-3 ship cannot re-stamp ai_mode_start_time_ms every
  // frame and the spin-up completes. Before the fix the capture supervisor ran
  // on every frame and reset the timer, leaving the ship parked forever.
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Plunderer government (flags_primary 0x1000), arming govt 0 if the stock
  // data carries none.
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

  const int ship_class = FindClassIndex(state, [](const game::ShipClass &c) {
    return c.base_fuel >= 100 && c.turn_rate > 0.0F;
  });
  REQUIRE(ship_class >= 0);

  // The targetless capture travel ladder would call
  // Ship_EnterShipAiState0x02_ClearPrimaryTarget (which re-stamps
  // ai_mode_start_time_ms) on every frame if it were allowed to run, so keep
  // the player inactive and place a secondary target to force that arm.
  state.player.is_active = false;

  const int slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_class_id = static_cast<std::int16_t>(ship_class);
  ship.faction_or_government_id = static_cast<std::int16_t>(govt_idx);
  ship.ai_behavior_code = 3;
  ship.ai_state_code = 2;
  ship.ai_control_mode = 4;
  ship.ai_maneuver_timer_ms = -1.0F;
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = 1;
  ship.ai_station_hold_timer = 2.0F;
  ship.ai_mode_start_time_ms = 0;
  ship.armor_points = 1000.0F;
  ship.fuel_points = 1000.0F;
  // Outside the 1000 px centre envelope so the state machine keeps control 4.
  ship.pos_x = 5000.0F;
  ship.pos_y = 0.0F;
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;

  // 1000 60 Hz ticks after the recorded start is well past the 364-tick
  // (Stellar_GetJumpSequenceDuration60Hz) spin-up.
  state.tick_60hz = 1000;
  game::NovaAi_UpdateShipAI(state,
                            ship,
                            /*now_ms=*/1000);

  CHECK_FALSE(ship.is_active);
  CHECK(ship.current_system_id == -1);
}

TEST_CASE("capture warship abandons only a disabled high-AI target",
          "[ai][boarding]") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int ship_class = FindClassIndex(state, [](const game::ShipClass &c) {
    return c.base_armor >= 30 && c.default_ai_behavior > 2;
  });
  REQUIRE(ship_class >= 0);

  int secondary_free_energy_bank = -1;
  for (std::size_t w = 0; w < state.scenario.weapons.size(); ++w) {
    if (state.scenario.weapons[w].ammo_type >= -1000 &&
        state.scenario.weapons[w].ammo_type <= -1) {
      secondary_free_energy_bank = static_cast<int>(w);
      break;
    }
  }
  REQUIRE(secondary_free_energy_bank >= 0);
  state.scenario.weapons[static_cast<std::size_t>(secondary_free_energy_bank)]
      .flags_secondary |= 0x1000U;

  const int attacker_slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  const int target_slot = game::NovaShip_AllocateShipSlot(state, 0, 0);
  REQUIRE(attacker_slot > 0);
  REQUIRE(target_slot > 0);

  game::Ship &attacker = state.ShipAt(static_cast<std::size_t>(attacker_slot));
  attacker.ship_class_id = static_cast<std::int16_t>(ship_class);
  attacker.armor_points = static_cast<float>(
      state.scenario.ships[static_cast<std::size_t>(ship_class)].base_armor);
  attacker.ai_state_code = 4;
  attacker.primary_target_ship_slot = static_cast<std::int16_t>(target_slot);
  attacker.npc_weapon_count_by_class[static_cast<std::size_t>(
      secondary_free_energy_bank)] = 1;

  game::Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  target.ship_class_id = static_cast<std::int16_t>(ship_class);
  const float target_max_armor = static_cast<float>(
      state.scenario.ships[static_cast<std::size_t>(ship_class)].base_armor);
  target.armor_points = target_max_armor;

  REQUIRE_FALSE(
      game::NovaWeapon_HasAnyFireableNonSecondaryWeapon(state, attacker));
  REQUIRE(game::NovaWeapon_ClassifyAmmoReadiness(state, attacker) == 0);
  REQUIRE_FALSE(game::NovaAiShip_IsDisabled(state, target));
  game::NovaAi_UpdateBehavior0x03CaptureVariant(state,
                                                attacker,
                                                /*now_ms=*/0);
  CHECK(attacker.ai_state_code == 4);
  CHECK(attacker.primary_target_ship_slot == target_slot);

  target.armor_points = target_max_armor * 0.25F;
  REQUIRE(game::NovaAiShip_IsDisabled(state, target));
  game::NovaAi_UpdateBehavior0x03CaptureVariant(state,
                                                attacker,
                                                /*now_ms=*/0);
  CHECK(attacker.ai_state_code == 0);
  CHECK(attacker.primary_target_ship_slot == -1);
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
    game::NovaAi_UpdateShipAI(state, helper, now_ms);
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

// Ship_AcquirePrimaryTargetForShip (0x0040e020) regression slice: the early
// retention gate keeps a state-3/4 primary target purely on activity (no
// same-system check), while the weapon-readiness gate and the mission-fleet
// goal-0 arm are the faithful gates reconstructed in this session.
TEST_CASE(
    "acquire retention gate keeps an active state-4 target cross-system") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  const std::int16_t other_sys =
      static_cast<std::int16_t>(sys_idx == 0 ? 1 : 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  state.player.is_active = true;

  const int slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_class_id = 0;
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.current_system_id = static_cast<std::int16_t>(sys_idx);
  ship.is_active = true;
  ship.ai_behavior_code = 3;
  ship.ai_state_code = 4;
  ship.ai_hostility_accumulator = 0;
  // Arm the hull so the weapon-readiness gate is not the reason the second
  // call stops early.
  game::NovaWeapon_EnsureNpcWeaponBanks(state, ship);

  const int target_slot =
      NovaShip_AllocateShipSlot(state, other_sys, /*reserved_tail=*/8);
  REQUIRE(target_slot > 0);
  game::Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  target.is_active = true;
  target.current_system_id = other_sys;
  ship.primary_target_ship_slot = static_cast<std::int16_t>(target_slot);

  game::NovaAi_AcquirePrimaryTarget(state, ship);
  // Retained despite the target living in another system (the original gate
  // only tests slot activity).
  CHECK(ship.primary_target_ship_slot == target_slot);

  // Outside state 3/4 the gate does not fire. A factionless ship runs no
  // government target pass (the whole 0x0040e020 government block is guarded
  // by faction != -1), so with a cleared primary the inactive cross-system
  // target is never re-acquired. (The original leaves a stale primary
  // untouched here; the behavior supervisor clears inactive targets.)
  ship.ai_state_code = 0;
  ship.primary_target_ship_slot = -1;
  target.is_active = false;
  game::NovaAi_AcquirePrimaryTarget(state, ship);
  CHECK(ship.primary_target_ship_slot == -1);
}

TEST_CASE("acquire refuses a ship with no ready weapons") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.armor_points = 30.0F;

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
  ship.ai_hostility_accumulator = 1; // would otherwise target the player
  ship.primary_target_ship_slot = -1;
  ship.npc_weapon_count_by_class.fill(0); // readiness bucket 2

  game::NovaAi_AcquirePrimaryTarget(state, ship);
  CHECK(ship.primary_target_ship_slot == -1);
}

TEST_CASE("acquire honors a Flags-1 personality grudge") {
  GameState state;
  state.scenario.pers_defs.resize(1);
  game::PersDef &pers = state.scenario.pers_defs[0];
  pers.alive = true;
  pers.grudge = true;
  pers.flags_primary = 0x0001;

  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.current_system_id = 0;
  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.ship_instance_id = 1;
  ship.current_system_id = 0;
  ship.pers_def_slot = 0;
  ship.primary_target_ship_slot = -1;

  game::NovaAi_AcquirePrimaryTarget(state, ship);

  CHECK(ship.primary_target_ship_slot == 0);
  CHECK(ship.ai_state_code == 4);
}

TEST_CASE("acquire uses the player's hull inherent combat govt for the roll") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  state.player.faction_or_government_id = -1;
  state.player.armor_points = 30.0F;
  state.player.shield_points = 30.0F;
  // Far from the NPC so the near-player reputation gate stays out of the way:
  // the inherent-combat roll is deliberately not distance-gated (0x0040e57f).
  state.player.pos_x = 1.0e6F;
  state.player.pos_y = 1.0e6F;

  game::Ship &ship = state.ShipAt(1);
  ship.is_active = true;
  ship.ship_instance_id = 1;
  // Fed Destroyer (res 141): its own inherent combat govt is Federation, so an
  // own-class read of the roll could never fire against a Federation NPC.
  ship.ship_class_id = 0x8d - 0x80;
  ship.current_system_id = static_cast<std::int16_t>(sys_idx);
  ship.faction_or_government_id = 0; // Federation, non-xenophobic
  ship.ai_behavior_code = 3;
  ship.ai_state_code = 0;
  ship.primary_target_ship_slot = -1;
  ship.random_ai_render_cadence = 0;
  ship.armor_points = 750.0F;
  ship.shield_points = 800.0F;
  game::NovaWeapon_EnsureNpcWeaponBanks(state, ship);

  // The first NovaRandom_Range(0x32) call in this path is the roll, so pick the
  // lowest seed whose first [0,50) draw is 0 rather than hard-coding a
  // distribution-implementation-specific constant.
  std::uint32_t seed = 1;
  for (;; ++seed) {
    std::mt19937 probe(seed);
    if (std::uniform_int_distribution<int>{0, 49}(probe) == 0) {
      break;
    }
  }

  // Rebel Dragon (res 180, inherent combat govt Rebellion 13), which the
  // Federation government is class-hostile to -> the roll may flag the player.
  state.player.ship_class_id = 0xb4 - 0x80;
  state.rng.seed(seed);
  game::NovaAi_AcquirePrimaryTarget(state, ship);
  CHECK(ship.primary_target_ship_slot == 0);

  // A Federation hull (res 141, inherent combat govt Federation 0) is not
  // hostile to a Federation ship, so the same roll must not flag the player.
  ship.primary_target_ship_slot = -1;
  state.player.ship_class_id = 0x8d - 0x80;
  state.rng.seed(seed);
  game::NovaAi_AcquirePrimaryTarget(state, ship);
  CHECK(ship.primary_target_ship_slot != 0);
}

TEST_CASE("Federation warship shares an ally's aggression against the player") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  state.player.ship_class_id = 0;
  state.player.armor_points = 30.0F;
  state.player.shield_points = 30.0F;

  game::Ship &acquirer = state.ShipAt(1);
  acquirer.is_active = true;
  acquirer.ship_instance_id = 1;
  acquirer.ship_class_id = 0x8d - 0x80;
  acquirer.current_system_id = static_cast<std::int16_t>(sys_idx);
  acquirer.faction_or_government_id = 0;
  acquirer.ai_behavior_code = 3;
  acquirer.ai_state_code = 0;
  acquirer.primary_target_ship_slot = -1;
  acquirer.armor_points = 750.0F;
  acquirer.shield_points = 800.0F;
  game::NovaWeapon_EnsureNpcWeaponBanks(state, acquirer);

  game::Ship &ally = state.ShipAt(2);
  ally.is_active = true;
  ally.ship_instance_id = 2;
  ally.ship_class_id = 0x8d - 0x80;
  ally.current_system_id = static_cast<std::int16_t>(sys_idx);
  ally.faction_or_government_id = 0;
  ally.ai_behavior_code = 3;
  ally.ai_state_code = 4;
  ally.primary_target_ship_slot = 0;

  game::NovaAi_AcquirePrimaryTarget(state, acquirer);

  // Ghidra 0x0040e3c0 redirects allied support onto the ally's target; it
  // never marks the allied attacker itself as hostile.
  CHECK(acquirer.primary_target_ship_slot == 0);
  CHECK(acquirer.ai_state_code == 4);
}

TEST_CASE("mission-fleet goal 0 forces hostility to the player") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.armor_points = 30.0F;

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
  ship.primary_target_ship_slot = -1;
  ship.mission_fleet_slot = 0;
  state.active_missions[0].ship_behavior = 0;
  state.active_mission_runtime_flags[0].is_active = true;

  game::NovaAi_AcquirePrimaryTarget(state, ship);
  CHECK(ship.primary_target_ship_slot == 0);
  CHECK(ship.ai_state_code == 4);
}

TEST_CASE("perceived combat strength counts allied support") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);

  const game::ShipClass *cls = state.scenario.Ship(0x80);
  REQUIRE(cls != nullptr);

  const int slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(slot > 0);
  game::Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  ship.ship_class_id = 0;
  ship.ship_instance_id = static_cast<std::int16_t>(slot);
  ship.current_system_id = static_cast<std::int16_t>(sys_idx);
  ship.is_active = true;
  ship.faction_or_government_id = -1;
  ship.pers_def_slot = -1;
  ship.ai_behavior_code = 0;
  ship.shield_points = static_cast<float>(cls->base_shield);

  const int solo = game::NovaAiShip_ComputePerceivedCombatStrength(state, ship);
  CHECK(solo == cls->strength); // full shields, no followers/allies

  const int ally_slot = NovaShip_AllocateShipSlot(
      state, static_cast<std::int16_t>(sys_idx), /*reserved_tail=*/8);
  REQUIRE(ally_slot > 0);
  game::Ship &ally = state.ShipAt(static_cast<std::size_t>(ally_slot));
  ally.ship_class_id = 0;
  ally.ship_instance_id = static_cast<std::int16_t>(ally_slot);
  ally.current_system_id = static_cast<std::int16_t>(sys_idx);
  ally.is_active = true;
  ally.faction_or_government_id = -1; // allied with `ship` (equal ids)
  ally.pers_def_slot = -1;
  ally.ai_behavior_code = 0;
  ally.shield_points = static_cast<float>(cls->base_shield);
  ally.armor_points = static_cast<float>(cls->base_armor);

  const int with_ally =
      game::NovaAiShip_ComputePerceivedCombatStrength(state, ship);
  CHECK(with_ally > solo);
}

// 0x00411800 FIST + residual/sign correction (0x0041187f..0x004118b7) truncates
// each shield-scaled strength toward zero; it is not std::lround.  A 5-strength
// hull at a 0.5 shield ratio yields 2.5, and the non-positive-max arm seeds the
// ratio from raw shield points (0x00411a90) rather than a fixed 0.25.
TEST_CASE("perceived combat strength truncates fractional shield-scaled "
          "strength") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(!state.scenario.ships.empty());

  state.scenario.ships[0].strength = 5;
  state.scenario.ships[0].base_shield = 100;
  state.player.is_active = false;

  game::Ship &ship = state.ShipAt(1);
  ship = game::Ship{};
  ship.ship_class_id = 0;
  ship.ship_instance_id = 1;
  ship.is_active = true;
  ship.faction_or_government_id = -1;
  ship.pers_def_slot = -1;
  ship.ai_behavior_code = 0;
  ship.shield_points = 50.0F; // ratio 0.5 -> 5 * 0.5 = 2.5 -> trunc 2
  CHECK(game::NovaAiShip_ComputePerceivedCombatStrength(state, ship) == 2);

  state.scenario.ships[0].base_shield = 0; // non-positive max
  ship.shield_points = 0.5F; // raw ratio 0.5 after clamp -> 2.5 -> trunc 2
  CHECK(game::NovaAiShip_ComputePerceivedCombatStrength(state, ship) == 2);
}

// 0x00411a1a re-reads the running total through its low 16 bits before each
// add (MOVSX EAX,DI).  base 5 + ally 32767 = 32772, then the next candidate's
// (empty) contribution re-reads it as (int16_t)32772 = -32764.
TEST_CASE("perceived combat strength wraps the running total at 16 bits") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(state.scenario.ships.size() >= 3);

  state.scenario.ships[0].strength = 5;
  state.scenario.ships[0].base_shield = 100;
  state.scenario.ships[1].strength = 32767;
  state.scenario.ships[1].base_shield = 100;
  state.scenario.ships[2].strength = 0;
  state.scenario.ships[2].base_shield = 100;
  state.player.is_active = false;

  game::Ship &ship = state.ShipAt(1);
  ship = game::Ship{};
  ship.ship_class_id = 0;
  ship.ship_instance_id = 1;
  ship.is_active = true;
  ship.faction_or_government_id = -1;
  ship.pers_def_slot = -1;
  ship.ai_behavior_code = 0;
  ship.shield_points = 100.0F;

  game::Ship &ally = state.ShipAt(2);
  ally = game::Ship{};
  ally.ship_class_id = 1;
  ally.ship_instance_id = 2;
  ally.is_active = true;
  ally.faction_or_government_id = -1;
  ally.pers_def_slot = -1;
  ally.ai_behavior_code = 0;
  ally.shield_points = 100.0F;
  ally.armor_points = 100.0F;

  game::Ship &zero = state.ShipAt(3);
  zero = game::Ship{};
  zero.ship_class_id = 2;
  zero.ship_instance_id = 3;
  zero.is_active = true;
  zero.faction_or_government_id = -1;
  zero.pers_def_slot = -1;
  zero.ai_behavior_code = 0;
  zero.shield_points = 100.0F;
  zero.armor_points = 100.0F;

  CHECK(game::NovaAiShip_ComputePerceivedCombatStrength(state, ship) == -32764);
}

// 0x0040e202 mission-fleet goal 1: drop a player primary, then, with no random
// combat candidate available, park in state 0x0c with secondary 0.
TEST_CASE("mission-fleet goal 1 parks with no random candidate") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const int sys_idx = FindWanderSuitableSystem(state);
  REQUIRE(sys_idx >= 0);
  state.player.current_system_id = static_cast<std::int16_t>(sys_idx);
  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.armor_points = 30.0F;

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
  ship.primary_target_ship_slot = 0; // player primary to drop
  ship.mission_fleet_slot = 0;
  state.active_missions[0].ship_behavior = 1;
  state.active_mission_runtime_flags[0].is_active = true;

  game::NovaAi_AcquirePrimaryTarget(state, ship);
  CHECK(ship.primary_target_ship_slot == -1);
  CHECK(ship.ai_state_code == 0xc);
  CHECK(ship.ai_secondary_target_slot == 0);
}

// Ship_AimWeaponLeadVelocity (0x0043b8c0) shares its intercept core with
// Ship_AimWeaponPredictive (0x0043b740) but excludes mode 6 from the lead
// gate, so a rocket falls back to the straight bearing while the ship-target
// path still leads it. Build a synthetic weapon to pin both behaviours.
TEST_CASE("AimWeaponLeadVelocity shares the predictive lead but skips mode 6") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(!state.scenario.weapons.empty());

  game::Weapon &weapon = state.scenario.weapons[0];
  weapon.projectile_speed = 100.0F; // shot_speed = 1 px/tick in port units

  // Shooter at the origin, stationary; target due north (-y), moving east.
  game::Ship ship;
  ship.pos_x = 0.0F;
  ship.pos_y = 0.0F;
  ship.vel_x = 0.0F;
  ship.vel_y = 0.0F;
  game::Ship target;
  target.pos_x = 0.0F;
  target.pos_y = -100.0F;
  target.vel_x = 100.0F;
  target.vel_y = 0.0F;

  constexpr std::int16_t kBank = 0; // resolves to weapons[0]

  // Straight projectile (-1): both entry points lead and must agree.
  weapon.weapon_mode_code = -1;
  const std::int16_t straight_shot =
      game::NovaAi_AimWeaponLeadVelocity(state,
                                         ship,
                                         target.pos_x,
                                         target.pos_y,
                                         target.vel_x,
                                         target.vel_y,
                                         kBank,
                                         ship.pos_x,
                                         ship.pos_y);
  CHECK(straight_shot ==
        game::NovaAi_AimWeaponPredictive(state, ship, target, kBank));
  CHECK(straight_shot != 0); // led away from the due-north bearing

  // Front-quadrant turret (7) is also led by both.
  weapon.weapon_mode_code = 7;
  CHECK(game::NovaAi_AimWeaponLeadVelocity(state,
                                           ship,
                                           target.pos_x,
                                           target.pos_y,
                                           target.vel_x,
                                           target.vel_y,
                                           kBank,
                                           ship.pos_x,
                                           ship.pos_y) ==
        game::NovaAi_AimWeaponPredictive(state, ship, target, kBank));

  // Freeflight rocket (6): only the ship-target function leads; LeadVelocity
  // rejects mode 6 and returns the straight due-north bearing.
  weapon.weapon_mode_code = 6;
  const std::int16_t rocket_straight =
      game::NovaAi_AimWeaponLeadVelocity(state,
                                         ship,
                                         target.pos_x,
                                         target.pos_y,
                                         target.vel_x,
                                         target.vel_y,
                                         kBank,
                                         ship.pos_x,
                                         ship.pos_y);
  CHECK(rocket_straight == 0);
  CHECK(game::NovaAi_AimWeaponPredictive(state, ship, target, kBank) != 0);
}

TEST_CASE("player-squad predicate matches Ship_IsInPlayerSquad 0x0046b8d0") {
  // The original reads only ship_instance_id (+0x86), squad_leader_ship_slot
  // (+0x9A), and the leader's own +0x9A; no scenario data or liveness is used.
  GameState state;

  // Slot 0, instance id 0: the player is always in its own squad.
  game::Ship &player = state.ShipAt(0);
  player.ship_instance_id = 0;
  player.squad_leader_ship_slot = -1;
  CHECK(game::NovaShip_IsInPlayerSquad(state, player));

  // Attached directly to the player (leader slot 0).
  game::Ship &escort = state.ShipAt(1);
  escort.ship_instance_id = 1;
  escort.squad_leader_ship_slot = 0;
  CHECK(game::NovaShip_IsInPlayerSquad(state, escort));

  // Attached to a player-attached ship: two hops to the player.
  game::Ship &wingman = state.ShipAt(2);
  wingman.ship_instance_id = 2;
  wingman.squad_leader_ship_slot = 1;
  CHECK(game::NovaShip_IsInPlayerSquad(state, wingman));

  // A leader that is itself subgrouped (three hops) is not player squad.
  game::Ship &deep = state.ShipAt(3);
  deep.ship_instance_id = 3;
  deep.squad_leader_ship_slot = 2;
  CHECK_FALSE(game::NovaShip_IsInPlayerSquad(state, deep));

  // Leader slot 0x3F is the last valid slot; only its own leader decides.
  game::Ship &edge = state.ShipAt(0x3F);
  edge.ship_instance_id = 0x3F;
  edge.squad_leader_ship_slot = 0x3F;
  CHECK_FALSE(game::NovaShip_IsInPlayerSquad(state, edge));
  edge.squad_leader_ship_slot = 0;
  CHECK(game::NovaShip_IsInPlayerSquad(state, edge));

  // Unattached and out-of-range leaders are not player squad.
  game::Ship &lone = state.ShipAt(4);
  lone.ship_instance_id = 4;
  lone.squad_leader_ship_slot = -1;
  CHECK_FALSE(game::NovaShip_IsInPlayerSquad(state, lone));
  lone.squad_leader_ship_slot = 0x40;
  CHECK_FALSE(game::NovaShip_IsInPlayerSquad(state, lone));

  // A zero instance id short-circuits even with a poisoned leader slot.
  lone.ship_instance_id = 0;
  CHECK(game::NovaShip_IsInPlayerSquad(state, lone));
}

namespace {

// Hand-build a two-candidate system for the adjacency selector. Both slots are
// eligible travel points (!travel_usable, sprite-active, non-hostile); slot 0
// is plain and slot 1 carries availability 0x2000. Returns true when the
// scenario tables are large enough to overwrite.
bool BuildTwoSlotTravelSystem(GameState &state) {
  if (state.scenario.systems.empty() || state.scenario.stellars.size() < 2 ||
      state.scenario.governments.empty()) {
    return false;
  }
  auto &sys = state.scenario.systems[0];
  sys.nav_defs.fill(-1);
  sys.nav_defs[0] = 0x80;
  sys.nav_defs[1] = 0x81;

  for (std::size_t i = 0; i < 2; ++i) {
    game::Stellar &st = state.scenario.stellars[i];
    st = game::Stellar{};
    st.pos_x = static_cast<std::int16_t>(10 + 10 * i);
    st.pos_y = static_cast<std::int16_t>(10 + 10 * i);
    st.flags = 1; // control bit set; !travel_usable, !engaged
    st.government_id = -1;
    st.strength_capacity = 0; // IsStellarActive false => sprite-active
  }
  state.scenario.stellars[1].availability_flags = 0x2000;
  state.scenario.governments[0].flags_secondary = 0;
  return true;
}

} // namespace

// Ghidra 0x0040c790 fallback pool. With no ScanMask preference the eligible
// plain slot must always win; the avail_2000 slot is rejected because the
// predicate is (!avail_2000 || prefer_2000). The old inverse term would let
// the RNG return the 0x2000 slot here.
TEST_CASE("adjacency selector fallback rejects unpreferred avail_2000") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(BuildTwoSlotTravelSystem(state));

  game::Ship ship;
  ship.current_system_id = 0;
  ship.faction_or_government_id = 0;

  for (int i = 0; i < 64; ++i) {
    CHECK(game::NovaAi_SelectRandomAdjacentTravelStellar(
              state,
              ship,
              /*strict_mode=*/false,
              /*unrestricted_only=*/false) == 0x80);
  }
}

// ScanMask 0x80 (prefer avail_2000) routes the same system to the 0x2000 slot
// whenever one exists, regardless of the RNG draw.
TEST_CASE("adjacency selector prefers avail_2000 when ScanMask says so") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(BuildTwoSlotTravelSystem(state));
  state.scenario.governments[0].flags_secondary = 0x0080;

  game::Ship ship;
  ship.current_system_id = 0;
  ship.faction_or_government_id = 0;

  for (int i = 0; i < 64; ++i) {
    CHECK(game::NovaAi_SelectRandomAdjacentTravelStellar(
              state,
              ship,
              /*strict_mode=*/false,
              /*unrestricted_only=*/false) == 0x81);
  }
}

// unrestricted_only (the behavior-0x04 interceptor's flag=1) selects only the
// plain pool even when the government prefers avail_2000.
TEST_CASE("adjacency selector unrestricted_only takes the plain slot") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(BuildTwoSlotTravelSystem(state));
  state.scenario.governments[0].flags_secondary = 0x0080;

  game::Ship ship;
  ship.current_system_id = 0;
  ship.faction_or_government_id = 0;

  for (int i = 0; i < 64; ++i) {
    CHECK(game::NovaAi_SelectRandomAdjacentTravelStellar(
              state,
              ship,
              /*strict_mode=*/false,
              /*unrestricted_only=*/true) == 0x80);
  }
}

// Ghidra 0x0046e9e0 wrapper: only restricted (availability & 0x3000) travel
// points are returned; plain travel points always yield -1.
TEST_CASE("spawn destination wrapper accepts only restricted travel points") {
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  REQUIRE(BuildTwoSlotTravelSystem(state));

  game::Ship ship;
  ship.current_system_id = 0;
  ship.faction_or_government_id = 0;

  // No preference bits: the selector's pools keep the plain slot and the
  // wrapper declines it every time.
  for (int i = 0; i < 32; ++i) {
    CHECK(game::NovaAi_SelectRandomAdjacentDestination(state, ship) == -1);
  }

  // With ScanMask 0x80 the selector routes to the 0x2000 slot, which the
  // wrapper then accepts as a wormhole.
  state.scenario.governments[0].flags_secondary = 0x0080;
  for (int i = 0; i < 64; ++i) {
    CHECK(game::NovaAi_SelectRandomAdjacentDestination(state, ship) == 0x81);
  }
}

TEST_CASE("mission stellar attack directive selects a hostile stellar") {
  GameState state;
  state.scenario.systems.resize(1);
  state.scenario.systems[0].nav_defs[0] = 0x80;
  state.scenario.stellars.resize(1);
  state.scenario.stellars[0].government_id = 1;
  state.scenario.stellars[0].strength_capacity = 100;
  state.scenario.stellars[0].strength = 100;
  state.scenario.governments.resize(2);
  state.scenario.governments[0].classes[0] = 10;
  state.scenario.governments[0].enemy_classes[0] = 20;
  state.scenario.governments[1].classes[0] = 20;
  state.scenario.weapons.resize(1);
  state.scenario.weapons[0].flags_secondary = 0x400;
  state.scenario.weapons[0].weapon_mode_code = 1;
  state.scenario.weapons[0].ammo_type = -1;

  game::Ship ship;
  ship.ship_instance_id = 1;
  ship.current_system_id = 0;
  ship.faction_or_government_id = 0;
  ship.armor_points = 100.0F;
  ship.npc_weapon_count_by_class[0] = 1;

  game::Mission_UpdateShipMissionStellarAttackDirective(state, ship);

  CHECK(ship.ai_state_code == 0x12);
  CHECK(ship.ai_secondary_target_slot == 0x80);
  CHECK(ship.primary_target_ship_slot == 0x80);
}

// NovaAi_ComputeMaxArmorPoints (0x004637a0) and the mission-fleet arm of
// Ship_IsShipDisabled (0x004687b0): the max folds in the personality
// shield_armor_scale and the behavior-5 x1.333 float store, and a mission
// ShipGoal-5 special ship counts disabled until its runtime ship-objective-
// complete latch is set or it is boarded.
TEST_CASE(
    "Ship_IsShipDisabled uses scaled max armor and the mission-fleet arm") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  int cls = -1;
  for (std::size_t i = 0; i < state.scenario.ships.size(); ++i) {
    const auto &candidate = state.scenario.ships[i];
    if (candidate.base_armor >= 30.0F &&
        (candidate.capability_flags & 0x10U) == 0U) {
      cls = static_cast<int>(i);
      break;
    }
  }
  REQUIRE(cls >= 0);
  const float base =
      state.scenario.ships[static_cast<std::size_t>(cls)].base_armor;

  game::Ship ship;
  ship.ship_instance_id = 1;
  ship.ship_class_id = static_cast<std::int16_t>(cls);
  ship.pers_def_slot = -1;
  ship.mission_fleet_slot = 0;
  ship.armor_points = base;
  ship.ai_behavior_code = 0;
  CHECK(game::NovaAi_ComputeMaxArmorPoints(state, ship) == Catch::Approx(base));

  // Behavior 5 stores the x1.333 product to float.
  ship.ai_behavior_code = 5;
  const float behavior5_max =
      static_cast<float>(static_cast<double>(base) * 1.333);
  CHECK(game::NovaAi_ComputeMaxArmorPoints(state, ship) ==
        Catch::Approx(behavior5_max));
  ship.ai_behavior_code = 0;

  // A positive personality ShieldMod scales the NPC maximum.
  int pers = -1;
  for (std::size_t i = 0; i < state.scenario.pers_defs.size(); ++i) {
    if (state.scenario.pers_defs[i].shield_armor_scale > 0.0F) {
      pers = static_cast<int>(i);
      break;
    }
  }
  if (pers >= 0) {
    ship.pers_def_slot = static_cast<std::int16_t>(pers);
    const double scale = static_cast<double>(
        state.scenario.pers_defs[static_cast<std::size_t>(pers)]
            .shield_armor_scale);
    CHECK(game::NovaAi_ComputeMaxArmorPoints(state, ship) ==
          Catch::Approx(static_cast<float>(static_cast<double>(base) * scale))
              .margin(0.01F));
  }
  ship.pers_def_slot = -1;

  // Mission-fleet ShipGoal-5, active, objective incomplete, unboarded: the
  // ship counts disabled even at full armor (the damage gate would not trip).
  state.active_missions[0].ship_goal = 5;
  state.active_mission_runtime_flags[0].is_active = true;
  state.active_mission_runtime_flags[0].objective_complete = false;
  ship.boarded_target_latch = 0;
  CHECK(game::NovaAiShip_IsDisabled(state, ship));

  // Completing the objective, boarding it, or dropping the mission clears the
  // arm.
  state.active_mission_runtime_flags[0].objective_complete = true;
  CHECK_FALSE(game::NovaAiShip_IsDisabled(state, ship));
  state.active_mission_runtime_flags[0].objective_complete = false;
  ship.boarded_target_latch = 1;
  CHECK_FALSE(game::NovaAiShip_IsDisabled(state, ship));
  ship.boarded_target_latch = 0;
  state.active_mission_runtime_flags[0].is_active = false;
  CHECK_FALSE(game::NovaAiShip_IsDisabled(state, ship));
}

// 0x00410900 Ship_EnterShipAiState0x04_TargetRandomRelativeToSquadLeader passes
// the SQUAD LEADER as Ship_IsShipAcquirableAsTarget's `candidate` and the
// randomly picked ship as its `acquirer` (disasm 0x0041099a): eligibility means
// the pick is already engaged with the leader's squad, not that the leader
// targets the pick. The two cases pin each direction -- a pick attacking the
// leader (admitted) and a pick the leader targets (rejected).
TEST_CASE("state-4 relative-to-leader target gate keeps the original "
          "acquirer/candidate order") {
  constexpr std::int16_t kSystemId = 5;

  // Seed whose first [0,64) draw lands on slot 3 (the only eligible pick), so
  // the random selection terminates deterministically on that slot.
  std::uint32_t pick_seed = 1;
  for (;; ++pick_seed) {
    std::mt19937 probe(pick_seed);
    if (std::uniform_int_distribution<std::int32_t>{0, 63}(probe) == 3) {
      break;
    }
  }

  auto setup = [&](GameState &state,
                   std::int16_t leader_target,
                   std::int16_t pick_target) {
    state.player.is_active = false;

    game::Ship &subject = state.ShipAt(1);
    subject.is_active = true;
    subject.ship_instance_id = 1;
    subject.current_system_id = kSystemId;
    subject.squad_leader_ship_slot = 2;
    subject.primary_target_ship_slot = -1;
    subject.ai_behavior_code = 6;
    subject.ai_state_code = 0;

    game::Ship &leader = state.ShipAt(2);
    leader.is_active = true;
    leader.ship_instance_id = 2;
    leader.current_system_id = kSystemId;
    leader.squad_leader_ship_slot = -1;
    leader.primary_target_ship_slot = leader_target;
    leader.ai_state_code = 4;

    game::Ship &pick = state.ShipAt(3);
    pick.is_active = true;
    pick.ship_instance_id = 3;
    pick.current_system_id = kSystemId;
    pick.squad_leader_ship_slot = -1;
    pick.primary_target_ship_slot = pick_target;
    pick.ai_state_code = 4;

    state.rng.seed(pick_seed);
  };

  // The pick is attacking the leader (pick.primary_target == leader): the
  // original admits it (acquirer = pick, candidate = leader), so the subject
  // acquires slot 3.
  {
    GameState state;
    setup(state, /*leader_target=*/-1, /*pick_target=*/2);
    game::NovaAi_EnterState4TargetRandomRelativeToSquadLeader(state,
                                                              state.ShipAt(1));
    CHECK(state.ShipAt(1).primary_target_ship_slot == 3);
    CHECK(state.ShipAt(1).ai_state_code == 4);
  }

  // The leader is attacking the pick but the pick targets nobody (leader.
  // primary_target == pick, pick.primary_target == -1): the swapped order
  // would admit it, but the original rejects it (the pick is not engaged with
  // the leader's squad) and clears the primary target without a state change.
  {
    GameState state;
    setup(state, /*leader_target=*/3, /*pick_target=*/-1);
    game::NovaAi_EnterState4TargetRandomRelativeToSquadLeader(state,
                                                              state.ShipAt(1));
    CHECK(state.ShipAt(1).primary_target_ship_slot == -1);
    CHECK(state.ShipAt(1).ai_state_code == 0);
  }
}
