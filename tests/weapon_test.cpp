#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <optional>

#include "brgr_archive.hpp"
#include "game/game_state.hpp"
#include "game/outfit.hpp"
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
  // The data archives resolve relative to the test working directory (the
  // repo root). The core UI archive Nova.rez sits directly in EV Nova/, and
  // the scenario data lives in EV Nova/Nova Files/*.rez (LoadFromArchives
  // searches both); check the same on-disk anchors the loader uses so the
  // data-dependant assertions actually run rather than report a stale miss.
  return std::filesystem::exists("EV Nova/Nova.rez") ||
         std::filesystem::exists("EV Nova/Nova Files/Nova Data 1.rez") ||
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

// Regression: the new-game flow must reconcile the seeded Light Blaster weapon
// bank into an owned outfit (Weapon_ReconcileOutfitPoolWithWeaponBanks,
// 0x00462ec0). Without it the starter blaster stays an owned-bank-only weapon:
// the Outfitter doesn't list it as owned, it can't be sold, and the first
// bank rebuild (buy/sell/close) wipes the bank so it stops firing.
TEST_CASE("starter light blaster becomes owned and survives a rebuild",
          "[weapon][data]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0; // ship class id 0x80 = Shuttle

  // outfit id 0x80 = Light Blaster has ModType 1 (kWeapon); after the loader's
  // ModVal -= 0x80 rebasing (DecodeOutfit) its mod_val is the zero-based bank
  // slot 0.
  const Outfit *lb = state.scenario.Outfit(0x80);
  REQUIRE(lb != nullptr);
  CHECK(lb->mod_type == static_cast<std::int16_t>(OutfitEffect::kWeapon));
  CHECK(lb->mod_val == 0);

  SeedStockWeaponBanks(state);
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);

  // The mounted stock weapon is now an owned outfit (index 0x80 - 0x80 = 0).
  CHECK(state.inventory.outfit_owned_count[0] == 1);

  // It is sellable in the Outfitter (not flagged with the 0x0008 no-sell bit).
  CHECK((state.scenario.Outfit(0x80)->flags & 0x0008U) == 0U);

  // A bank rebuild from owned outfits (the landed buy/sell/close path) keeps the
  // Light Blaster bank mounted, so firing survives any Outfitter transaction.
  NovaWeapon_RebuildBanksFromOwnedOutfits(state);
  CHECK(state.weapon_bank_ammo[0] == 1);
  CHECK(NovaWeapon_CanFireBank(state, 0));
}

// A shipyard purchase seeds the new ship's mounted stock weapons into the
// banks and reconciles them into owned outfits (Outfit_SwapPlayerShipWithEscort,
// 0x00423fa0: seed default_weapon_ammo/secondary, then
// Weapon_ReconcileOutfitPoolWithWeaponBanks). Buying the Shuttle again therefore
// yields both an owned Light Blaster and a fireable bank 0, even though the class
// has no DefaultItem entry.
TEST_CASE("buying a ship registers its stock weapons as owned",
          "[weapon][data]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  // Simulate swapping into ship class 0x80 (Shuttle) via the shipyard path:
  // switch class, clear non-persistent outfits, seed stock banks, reconcile.
  state.player.ship_class_id = 0;
  state.inventory.outfit_owned_count.fill(0);
  NovaWeapon_SeedBanksFromShipStock(state, state.player.ship_class_id);
  CHECK(state.weapon_bank_ammo[0] == 1);
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
  CHECK(state.inventory.outfit_owned_count[0] == 1);
  CHECK(NovaWeapon_CanFireBank(state, 0));
}

// Ground truth for the Light Blaster's on-screen shot behaviour (verified
// against its raw payload and shot sprite set, sp.x9an id shot_sprite_set_id +
// 3000 = 3000):
//  * The shot sprite set is a 35x35 tile, 6x6 grid = 36-frame rotation sheet,
//    so the bolt is heading-oriented (like the ship), not a single image.
//  * WeaponDef flags_primary bit 0 is clear -> Shot_HandleShot takes the
//    *static/heading* branch: the frame is picked from the firing bearing, not
//    time-stepped. This is why the bolt must be rotated by its velocity.
//  * shot_anim_frame_dwell (resource +0x32, Ghidra
//  homing_strength_or_turn_rate)
//    is 0, so even an animated frame-stepper would advance every frame.
TEST_CASE("light blaster shot is heading-oriented, not time-animated",
          "[weapon]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  const Weapon *w = state.scenario.Weapon(0x80);
  REQUIRE(w != nullptr);
  CHECK((w->flags & 0x0001U) == 0U); // static/heading shot-frame path
  CHECK(w->shot_anim_frame_dwell == 0);
  CHECK(w->sprite_id == 0); // shot sprite set spin id 0 + 3000 = 3000

  auto spin = NovaResource_Load(kResourceTypeSprites, 3000);
  REQUIRE(spin.has_value());
  auto def = NovaSpriteDefinition_Parse(*spin);
  REQUIRE(def.has_value());
  CHECK(def->tile_width == 35);
  CHECK(def->tile_height == 35);
  CHECK(def->tiles_x == 6);
  CHECK(def->tiles_y == 6);

  // The fired shot must carry a velocity whose direction picks the matching
  // heading frame (up = frame 0).
  state.player.ship_class_id = 0;
  state.player.heading = 0.0F;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 0.0F;
  SeedStockWeaponBanks(state);
  NovaWeapon_FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1);
  const ActiveShot &shot = state.active_shots[0];
  // atan2(vel_x, -vel_y) = heading-bearing in the Math_AddPolarVelocity
  // convention; heading 0 (up) projects to vel_y < 0, so bearing ~ 0.
  const float bearing = std::atan2(shot.vel_x, -shot.vel_y);
  CHECK(std::fabs(bearing) < 0.01F);
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
  // WeaponDef Speed is px/frame * 100; the light blaster is 1500 -> 15
  // px/frame, so heading 0 projects to vel_y = -15.
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
  CHECK(state.weapon_bank_cooldown[0] ==
        Catch::Approx(initial_cooldown - 1.0F));

  // After enough ticks the shot expires and the bank cools to 0, so fire again.
  for (int i = 0; i < static_cast<int>(initial_cooldown) + 2; ++i) {
    NovaWeapon_TickShots(state);
  }
  CHECK(state.active_shots.empty());
  CHECK(state.weapon_bank_cooldown[0] == 0.0F);
  NovaWeapon_FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1);
}

// Regression: mounting a second identical weapon in a bank doubles the fire
// rate by halving the per-shot cooldown (the original divides the weapon's
// fire cadence by weapon_bank_ammo, the number of weapons in the bank) rather
// than being a no-op. Pins the earlier behaviour where the bank always cooled
// down at the full reload regardless of mount count, so buying a second Light
// Blaster changed nothing.
TEST_CASE("second mounted weapon halves the bank cooldown", "[weapon]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0;
  SeedStockWeaponBanks(state);

  // One Light Blaster mounted: cooldown = reload(10) / ammo(1) = 10 ticks.
  REQUIRE(state.weapon_bank_ammo[0] == 1);
  NovaWeapon_FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1);
  const float single_cooldown = state.weapon_bank_cooldown[0];
  REQUIRE(single_cooldown == Catch::Approx(10.0F));

  // A second identical weapon in the same bank halves the cooldown.
  state.weapon_bank_ammo[0] = 2;
  state.weapon_bank_cooldown[0] = 0.0F; // back off cooldown
  NovaWeapon_FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 2); // previous shot still flying
  CHECK(state.weapon_bank_cooldown[0] == Catch::Approx(single_cooldown / 2.0F));
}

// Regression: the light blaster (a turret-group-0 weapon) should exit the
// nose barrel of the ship rather than dead-centre. The muzzle geometry is
// decoded from the ship's sh\x8an descriptor into GameState::player.muzzle_*
// (see ShipVisualDescriptor.turret_muzzles and SpaceflightView::Ensure-
// ShipSprite); here we reproduce the Shuttle's actual group-0 quadrant data
// (lateral = 3/-3, forward = 10, drop = -2, compress scale = 1.0/0.71) and
// check the fired shot is offset off the centre by the muzzle vector.
TEST_CASE("light blaster exits the nose barrel, not the hull centre",
          "[weapon]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0;
  state.player.heading = 0.0F; // pointing up (-y)
  state.player.pos_x = 100.0F;
  state.player.pos_y = 200.0F;
  SeedStockWeaponBanks(state);

  // Populate muzzle geometry as EnsureShipSprite would from the Shuttle sh\x8an
  // payload (weapon turret group 0).
  state.player.muzzle_ready = true;
  state.player.muzzle_scale_x = 1.0F;
  state.player.muzzle_scale_y = 0.71F;
  state.player.muzzle_forward[0] = {10, 10, 10, 10};
  state.player.muzzle_lateral[0] = {3, -3, 3, -3};
  state.player.muzzle_drop[0] = {-2, -2, -2, -2};
  state.player.muzzle_quadrant[0] = 0; // pin the first barrel

  NovaWeapon_FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1);
  const auto &shot = state.active_shots[0];
  // Heading 0: forward offset F=10 along -y (nose), lateral L=3 along +x;
  //   x += (±3) * 1.0
  //   y += (F=10 along -y => -10*0.71) - drop(-2) = -7.1 + 2 = -5.1
  CHECK(shot.pos_x == Catch::Approx(100.0F + 3.0F));
  CHECK(shot.pos_y == Catch::Approx(200.0F - 10.0F * 0.71F + 2.0F)); // -5.1
  // Velocity still points up (-y) along the heading regardless of the offset.
  CHECK(shot.vel_x == Catch::Approx(0.0F));
  CHECK(shot.vel_y == Catch::Approx(-15.0F));
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

// The Light Blaster (weapon 0x80) carries fire_sound slot 8, which maps to
// the snd resource id 200 + 8 = 208 ("Light Blaster.sfil"). Firing a round
// must queue that slot for playback (GameState.pending_fire_sound_slots), and
// the preload helper must decode it into the cache so the spaceflight loop can
// play it. This pins the audio side of the firing path to the real data.
TEST_CASE("firing queues the weapon's fire sound and preload decodes it",
          "[weapon][audio]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0;
  SeedStockWeaponBanks(state);

  const Weapon *w = state.scenario.Weapon(0x80);
  REQUIRE(w != nullptr);
  CHECK(w->fire_sound == 8);
  CHECK(NovaWeapon_FireSoundResourceId(w->fire_sound) == 208);

  // Firing a round queues the fire-sound slot.
  NovaWeapon_FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1);
  REQUIRE(state.pending_fire_sound_slots.size() == 1);
  CHECK(state.pending_fire_sound_slots[0] == 8);

  // Preloading that slot decodes the Light Blaster fire sound into the cache.
  NovaWeapon_PreloadFireSound(state, 8);
  REQUIRE(state.weapon_fire_sounds[8].has_value());
  CHECK(state.weapon_fire_sounds[8]->sample_rate == 11127);
  CHECK(state.weapon_fire_sounds[8]->channel_count == 1);
}
} // namespace game
