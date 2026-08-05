#include "game/scenario_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "brgr_archive.hpp"

namespace game {

// These tests run against the shipped Nova Data archives; they assert values
// that were verified directly from the raw payload bytes, so they pin both the
// BRGR map resolution and the clean-room decoders to the exact game data.

TEST_CASE("scenario tables load ships, outfits and weapons", "[scenario][data]") {
  ScenarioData data;
  REQUIRE(data.LoadFromArchives());

  // Verifiable against the resources: the default starter ship (id 0x80) has
  // the stats observed in its payload.
  const ShipClass *ship = data.Ship(0x80);
  REQUIRE(ship != nullptr);
  CHECK(ship->cargo_holds == 10);
  CHECK(ship->base_shield == 30);
  CHECK(ship->accel == 500.0F);
  CHECK(ship->speed == 400.0F);
  CHECK(ship->turn_rate == 40.0F);
  CHECK(ship->base_fuel == 300);
  CHECK(ship->free_mass == 8);
  CHECK(ship->base_armor == 30);
  CHECK(ship->mass_tons == 15);
  // Cost is repacked as a 4-byte big-endian field at 0x30 (0x00002710 = 10000).
  CHECK(ship->cost == 10000);

  // The starter ship's stock weapon banks: bank 0 has a single stock weapon of
  // weapon id 0x80 (the weapon defined at resource id 128).
  CHECK(ship->stock_weapons[0].weapon_id == 0x80);
  CHECK(ship->stock_weapons[0].count == 1);
  CHECK(ship->stock_weapons[0].ammo_load == -1); // no ammo (unlimited)

  // Weapon id 0x80 (a projectile): values verified from its payload.
  const Weapon *w = data.Weapon(0x80);
  REQUIRE(w != nullptr);
  CHECK(w->reload_ticks == 10);
  CHECK(w->lifetime_ticks == 13);
  CHECK(w->mass_damage == 1);
  CHECK(w->energy_damage == 4);
  CHECK(w->guidance_mode == -1); // unguided projectile
  CHECK(std::fabs(w->projectile_speed - 1500.0F) < 0.001F);
  CHECK(w->ammo_type == -1); // unlimited ammo
  CHECK(w->sprite_id == 0);

  // Outfits load; the first outfit is a weapon-type (ModType 1 -> weapon).
  CHECK(data.Outfit(0x80) != nullptr);
  CHECK(data.Outfit(0x80)->mod_type == 1);
  CHECK(data.Outfit(0x80)->mod_val == 0x80); // references weapon id 128
}

TEST_CASE("scenario resource families resolve through the BRGR adapter",
          "[scenario][brgr]") {
  // The five scenario families live across the Nova Data archives; the adapter
  // must find them all (regression: Nova Data 4's w\x91ap / o\x9ftf records
  // were previously missed by a too-loose resource.map scan).
  for (std::uint32_t type : {0x73689570U, 0x6f9f7466U, 0x77916170U,
                             0x73709a62U, 0x73d87374U}) {
    const auto first = NovaResource_Load(type, 0x80);
    CHECK(first.has_value());
  }
}

} // namespace game
