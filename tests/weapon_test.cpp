#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <optional>

#include "game/game_state.hpp"
#include "game/pilot_file.hpp"
#include "game/scenario_data.hpp"
#include "game/weapon.hpp"

namespace game {

// The Shuttle's stock Light Blaster banking + the reconstructed primary firing
// path. These assert the weapon-bank population and fire/cooldown behaviour
// that get the player's main weapon shooting, against the shipped Nova data:
// the starter ship (class 0x80) mounts one Light Blaster (stock weapon
// {id 0x80, count 1, ammo -1=unlimited}) in bank 0, so weapon_bank_ammo[0]
// must be 1 and the primary-fire path (NovaWeapon_FirePlayerPrimary) must be
// able to fire it.

namespace {
bool ArchivesAvailable() {
  return std::filesystem::exists("EV Nova/Nova Files/Nova.rez") ||
         std::filesystem::exists("../../../EV Nova/Nova Files/Nova.rez") ||
         std::filesystem::exists("../../../EV Nova/Nova.rez");
}

// Replicates Stub_SeedStartingInventory's stock-weapon -> bank population
// (new_pilot_flow.cpp, mirroring Menu_RunNewGameFlow).
void SeedStockWeaponBanks(GameState &state) {
  const auto *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (!ship) {
    return;
  }
  state.weapon_bank_ammo.fill(0);
  state.weapon_bank_secondary.fill(0);
  for (const ShipDefaultWeaponBank &stock : ship->stock_weapons) {
    if (stock.weapon_id < 0x80 || stock.weapon_id > 0x17f) {
      continue;
    }
    const std::size_t bank = static_cast<std::size_t>(stock.weapon_id - 0x80);
    state.weapon_bank_ammo[bank * 100] =
        static_cast<std::int16_t>(stock.count > 0 ? stock.count : 0);
    if (stock.ammo_load > 0) {
      state.weapon_bank_secondary[bank * 100] =
          static_cast<std::int16_t>(stock.ammo_load);
    }
  }
  state.weapon_bank_cooldown.fill(0.0F);
  state.active_shots.clear();
}
} // namespace

TEST_CASE("shuttle light blaster is mounted and fireable", "[weapon][data]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // ship class id 0x80 = Shuttle

  const auto *ship = state.scenario.Ship(0x80);
  REQUIRE(ship != nullptr);
  CHECK(ship->display_name == "Shuttle");
  // The starter stock bank: one Light Blaster, unlimited ammo.
  CHECK(ship->stock_weapons[0].weapon_id == 0x80);
  CHECK(ship->stock_weapons[0].count == 1);
  CHECK(ship->stock_weapons[0].ammo_load == -1);

  SeedStockWeaponBanks(state);

  // Bank 0 (weapon 0x80, the Light Blaster) is mounted and >0 so the primary
  // loop touches it; it is not a secondary weapon.
  CHECK(state.weapon_bank_ammo[0] == 1);
  CHECK(NovaWeapon_CanFireBank(state, 0));
}

TEST_CASE("primary fire spawns a light blaster shot then cools down",
          "[weapon]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0;
  // Heading 0 = up (-y); shots fly along the heading at weapon speed.
  state.player.heading = 0.0F;
  state.player.pos_x = 100.0F;
  state.player.pos_y = 200.0F;
  SeedStockWeaponBanks(state);

  NovaWeapon_FirePlayerPrimary(state);
  // One Light Blaster round spawned, from the ship, moving upward (-y).
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].weapon_id == 0);
  CHECK(state.active_shots[0].pos_x == Catch::Approx(100.0F));
  CHECK(state.active_shots[0].pos_y == Catch::Approx(200.0F));
  // WeaponDef Speed is px/frame * 100; the light blaster is 1500 -> 15 px/frame,
  // so heading 0 projects to vel_y = -15.
  CHECK(state.active_shots[0].vel_x == Catch::Approx(0.0F));
  CHECK(state.active_shots[0].vel_y == Catch::Approx(-15.0F));
  // Life = WeaponDef Count (13 ticks).
  CHECK(state.active_shots[0].life_frames == 13);
  // The bank went into cooldown (reload 10 ticks) so a back-to-back fire is a
  // no-op while cooling down.
  CHECK(state.weapon_bank_cooldown[0] > 0.0F);
  NovaWeapon_FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1); // no second shot while cooling down
}

TEST_CASE("cooldown counts down and the bank can fire again", "[weapon]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0;
  SeedStockWeaponBanks(state);

  NovaWeapon_FirePlayerPrimary(state);
  const float initial_cooldown = state.weapon_bank_cooldown[0];
  REQUIRE(initial_cooldown > 0.0F);
  for (int i = 0; i < 1; ++i) {
    NovaWeapon_TickShots(state);
  }
  CHECK(state.weapon_bank_cooldown[0] == Catch::Approx(initial_cooldown - 1.0F));

  // After enough ticks the shot expires and the bank cools to 0, so fire again.
  for (int i = 0; i < static_cast<int>(initial_cooldown) + 2; ++i) {
    NovaWeapon_TickShots(state);
  }
  CHECK(state.active_shots.empty());
  CHECK(state.weapon_bank_cooldown[0] == 0.0F);
  NovaWeapon_FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1);
}

// Regression: the new-pilot flow seeds the weapon banks (Step 4) and then
// applies a freshly-built PilotFile record (Step 6). A fresh record's banks
// are zeroed, so unless the seeded banks are carried into the record before
// apply, PilotFileApply clobbers them and the Light Blaster becomes
// unfireable. Reproduces that exact two-step ordering and pins the fixed
// behaviour: the bank survives the round-trip and still fires.
TEST_CASE("fresh-pilot record round-trip keeps the light blaster fireable",
          "[weapon]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0;
  SeedStockWeaponBanks(state);
  REQUIRE(state.weapon_bank_ammo[0] == 1);

  // Step 6 (new_pilot_flow.cpp): build the fresh record and carry the banks.
  PilotFile record = PilotFile::Fresh();
  record.weapon_bank_ammo = state.weapon_bank_ammo;
  record.weapon_bank_secondary = state.weapon_bank_secondary;
  PilotFileApply(record, state);

  // The seeded Light Blaster must survive the record round-trip.
  CHECK(state.weapon_bank_ammo[0] == 1);
  CHECK(NovaWeapon_CanFireBank(state, 0));
  NovaWeapon_FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1);
}

} // namespace game
