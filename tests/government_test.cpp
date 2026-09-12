#include <catch2/catch_test_macros.hpp>

#include "game/collision.hpp"
#include "game/government.hpp"
#include "game/scenario_data.hpp"

namespace {

using game::GameState;
using game::Government;
using game::NovaGovernment_AreGovtsAllied;
using game::NovaGovernment_AreGovtsHostileOrXenophobic;
using game::NovaGovernment_IsCandidateHostileToTargeter;
using game::NovaStellar_TickStellarDefenseBatteries;
using game::ScenarioData;
using game::Ship;
using game::Stellar;

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
// Ghidra 0x004629e0 Government_IsCandidateHostileToTargeter: the stellar
// defense battery's target filter. Synthetic data keeps this deterministic
// (no archives); the relation ladder is covered by the helpers above, so this
// asserts the targeter-specific gates.
TEST_CASE("candidate-hostility applies the stellar targeter gates",
          "[government][targeter]") {
  GameState state;
  Government a; // index 0
  Government b; // index 1
  a.classes = {0, -1, -1, -1};
  b.enemy_classes = {0, -1, -1, -1};
  state.scenario.governments = {a, b};
  state.scenario.ships.resize(1);
  state.scenario.ships[0].inherent_combat_govt = 1;
  state.system_reputation.assign(1, 0);
  state.player.current_system_id = 0;

  Stellar stellar;
  stellar.government_id = 0;
  stellar.availability_flags = 0;
  Ship ship;
  ship.is_active = true;
  ship.ship_class_id = 0;
  ship.faction_or_government_id = 1;

  SECTION("ordinary NPC is hostile only through the relation tables") {
    ship.ship_instance_id = 1;
    ship.squad_leader_ship_slot = -1;
    CHECK(NovaGovernment_IsCandidateHostileToTargeter(
        state, ship, stellar, 0x80));
    // A selected travel destination never suppresses the NPC branch; the
    // ordinary NPC row has no travel/leader special case.
    state.travel.selected_stellar_id = 0x80;
    CHECK(NovaGovernment_IsCandidateHostileToTargeter(
        state, ship, stellar, 0x80));
    state.travel.selected_stellar_id = -1;
  }

  SECTION("player/leader ladder suppresses the selected travel destination") {
    ship.ship_instance_id = 0; // player (always a leader)
    CHECK(NovaGovernment_IsCandidateHostileToTargeter(
        state, ship, stellar, 0x80));
    state.travel.selected_stellar_id = 0x80;
    CHECK(!NovaGovernment_IsCandidateHostileToTargeter(
        state, ship, stellar, 0x80));
    state.travel.selected_stellar_id = -1;
  }

  SECTION("hazard marker and AI state 8 reject a candidate") {
    ship.ship_instance_id = 1;
    ship.squad_leader_ship_slot = -1;
    stellar.hazard_marker = true;
    CHECK(!NovaGovernment_IsCandidateHostileToTargeter(
        state, ship, stellar, 0x80));
    stellar.hazard_marker = false;
    ship.ai_state_code = 8;
    CHECK(!NovaGovernment_IsCandidateHostileToTargeter(
        state, ship, stellar, 0x80));
  }

  SECTION("special (0x200) stellars admit only the derelict sentinel") {
    ship.ship_instance_id = 0; // player/leader arm
    stellar.availability_flags = 0x200;
    CHECK(!NovaGovernment_IsCandidateHostileToTargeter(
        state, ship, stellar, 0x80));
    stellar.field_0x47 = 1;
    CHECK(NovaGovernment_IsCandidateHostileToTargeter(
        state, ship, stellar, 0x80));
  }
}

TEST_CASE("stellar defense battery fires, reloads, and respects the shot pool",
          "[government][targeter][stellar-defense]") {
  GameState state;
  state.player.current_system_id = 0;
  state.player.is_active = true;
  state.player.pos_x = 100.0F;
  state.player.pos_y = 0.0F;
  state.last_frame_tick_scale = 1.0F;
  state.system_reputation.assign(1, 0);

  Government govt;
  govt.crime_tol = -1;
  state.scenario.governments = {govt};
  game::System system;
  system.nav_defs[0] = 0x80;
  state.scenario.systems = {system};
  Stellar battery;
  battery.is_available = true;
  battery.government_id = 0;
  battery.weapon_id = 0x80;
  state.scenario.stellars = {battery};
  game::Weapon weapon;
  weapon.reload_ticks = 7;
  weapon.lifetime_ticks = 10;
  weapon.mass_damage = 1;
  weapon.projectile_speed = 100.0F;
  weapon.range_scalar = 200.0F;
  state.scenario.weapons = {weapon};

  NovaStellar_TickStellarDefenseBatteries(state);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].owner_ship_slot == -1);
  CHECK(state.active_shots[0].target_ship_slot == 0);
  CHECK(state.active_shots[0].weapon_id == 0);
  CHECK(state.scenario.stellars[0].defense_battery_cooldown == 7.0F);

  state.active_shots.assign(0x80, game::ActiveShot{});
  state.scenario.stellars[0].defense_battery_cooldown = 0.0F;
  NovaStellar_TickStellarDefenseBatteries(state);
  CHECK(state.active_shots.size() == 0x80);
  CHECK(state.scenario.stellars[0].defense_battery_cooldown == 0.0F);
}

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
