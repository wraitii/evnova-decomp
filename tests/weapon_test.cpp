#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "game/game_state.hpp"
#include "game/outfit.hpp"
#include "game/pilot_file.hpp"
#include "game/scenario_data.hpp"
#include "game/ship_ai.hpp"
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

  // A bank rebuild from owned outfits (the landed buy/sell/close path) keeps
  // the Light Blaster bank mounted, so firing survives any Outfitter
  // transaction.
  NovaWeapon_RebuildBanksFromOwnedOutfits(state);
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

TEST_CASE("hostile NPC selects and fires an unlimited weapon bank",
          "[weapon][npc]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  Ship &player = state.player;
  player.is_active = true;
  player.ship_instance_id = 0;
  player.ship_class_id = 0;
  player.current_system_id = 0;
  player.armor_points = 100.0F;
  player.shield_points = 100.0F;
  player.pos_x = 200.0F;
  player.pos_y = 100.0F;

  Ship &npc = state.ShipAt(1);
  npc.is_active = true;
  npc.ship_instance_id = 1;
  npc.ship_class_id = 0;
  npc.current_system_id = 0;
  npc.ai_behavior_code = 3;
  npc.primary_target_ship_slot = 0;
  npc.armor_points = 100.0F;
  npc.shield_points = 100.0F;
  npc.pos_x = 200.0F;
  npc.pos_y = 300.0F;

  // The Shuttle's stock bank is Light Blaster {count 1, ammo -1}; the NPC
  // selector must accept the unguided mode and preserve the unlimited sentinel.
  NovaAi_UpdateAutoWeaponSelectionFromTarget(state, npc);
  REQUIRE(npc.active_weapon_bank_slot == 0);
  REQUIRE(npc.ai_fire_trigger_latch != 0);

  NovaWeapon_FireNpcWeaponBank(state, npc);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].owner_ship_slot == 1);
  CHECK(state.active_shots[0].weapon_id == 0);
  CHECK(npc.npc_weapon_bank_secondary[0] == -1);
  CHECK(npc.ai_fire_trigger_latch == 0);

  // A successful volley must latch the weapon's fire sound (Light Blaster
  // slot 8) sourced at the firing ship, mirroring Weapon_FireShipWeapons's
  // sVar9 > 0 gate around NovaAudio_PlaySpatialByDistance.
  REQUIRE(state.pending_fire_sounds.size() == 1);
  const GameState::PendingFireSound &pending = state.pending_fire_sounds[0];
  CHECK(pending.slot == 8);
  CHECK(pending.src_x == Catch::Approx(200.0F));
  CHECK(pending.src_y == Catch::Approx(300.0F));
  state.pending_fire_sounds.clear();
}

// The distance falloff mirrors NovaAudio_PlaySpatialByDistance (0x004692e0)
// with the sound-volume extent factored out: full volume within 200 px, then
// per-channel 1/d^2 falloff (loud channel full at 850 px, quiet channel at
// the 200-px reference), each channel floored at 1/8, averaged to the mono
// gain the mixer actually plays. Values below are hand-computed from the
// original's integer math at full extent (E = 0x100).
TEST_CASE("spatial fire gain matches the original distance falloff",
          "[weapon][audio]") {
  // src == listener (player firing): full volume.
  CHECK(NovaWeapon_ComputeSpatialFireGain(0.0F, 0.0F, 0.0F, 0.0F) ==
        Catch::Approx(1.0F));
  // Within the 200 px cutoff: full volume regardless of heading.
  CHECK(NovaWeapon_ComputeSpatialFireGain(0.0F, 0.0F, 200.0F, 0.0F) ==
        Catch::Approx(1.0F));
  CHECK(NovaWeapon_ComputeSpatialFireGain(0.0F, 0.0F, -120.0F, 160.0F) ==
        Catch::Approx(1.0F));

  // Directly north at 850 px, horizontally centered: the original feeds the
  // loud channel (full at 722500/d^2 = 1.0) to BOTH ears, so no attenuation.
  CHECK(NovaWeapon_ComputeSpatialFireGain(0.0F, 0.0F, 0.0F, 850.0F) ==
        Catch::Approx(1.0F));
  // Straight ahead at 2000 px, still centered: both channels truncate to
  // 256*722500/4e6 = 46, average 46/256.
  CHECK(NovaWeapon_ComputeSpatialFireGain(0.0F, 0.0F, 0.0F, 2000.0F) ==
        Catch::Approx(46.0F / 256.0F));

  // 1000 px to the left: loud channel (256*722500/1e6) truncates to 184,
  // quiet (256*40000/1e6) to 10 -> floored to 32. Average (184+32+1)>>1
  // = 108 of 256. Mirrored source to the right gives the same average.
  CHECK(NovaWeapon_ComputeSpatialFireGain(0.0F, 0.0F, -1000.0F, 0.0F) ==
        Catch::Approx(108.0F / 256.0F));
  CHECK(NovaWeapon_ComputeSpatialFireGain(0.0F, 0.0F, 1000.0F, 0.0F) ==
        Catch::Approx(108.0F / 256.0F));

  // 3000 px out: loud (256*722500/9e6) = 20 -> floored to 32, quiet floored
  // to 32; average stays 32/256 = 1/8 (the audible floor).
  CHECK(NovaWeapon_ComputeSpatialFireGain(0.0F, 0.0F, 3000.0F, 0.0F) ==
        Catch::Approx(32.0F / 256.0F));
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

} // namespace game
