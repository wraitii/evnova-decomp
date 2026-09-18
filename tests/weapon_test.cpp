#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
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
// {id 0x80, count 1, ammo -1=unlimited}) in bank 0, so weapon_count_by_class[0]
// must be 1 and the primary-fire path (the primary-fire arm of
// NovaWeapon_TickPlayerWeaponCommands) must be able to fire it.

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
// Fires the primary banks through the player weapon-command dispatch (the
// fire_primary_held arm), bypassing live input. The dispatch's fire arms are
// gated on the fire-restricted (disabled) state, so the hull is first brought
// to full armor -- the state every real flow produces (new-game meter
// finalize, pilot restore) but a bare GameState does not.
void FirePlayerPrimary(GameState &state) {
  const auto *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (ship_class != nullptr) {
    state.player.armor_points = static_cast<float>(ship_class->base_armor);
  }
  NovaWeapon_TickPlayerWeaponCommands(
      state, PlayerWeaponCommandInput{.fire_primary_held = true}, 0.0F);
}

void SeedStockWeaponBanks(GameState &state) {
  const auto *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (!ship) {
    return;
  }
  state.weapon_count_by_class.fill(0);
  state.weapon_secondary_count_by_class.fill(0);
  for (const ShipDefaultWeaponBank &stock : ship->stock_weapons) {
    if (stock.weapon_id < 0x80 || stock.weapon_id > 0x17f) {
      continue;
    }
    const std::size_t bank = static_cast<std::size_t>(stock.weapon_id - 0x80);
    state.weapon_count_by_class[bank * 100] =
        static_cast<std::int16_t>(stock.count > 0 ? stock.count : 0);
    if (stock.ammo_load > 0) {
      state.weapon_secondary_count_by_class[bank * 100] =
          static_cast<std::int16_t>(stock.ammo_load);
    }
  }
  state.weapon_bank_cooldown.fill(0.0F);
  state.active_shots.clear();
}

// The Light Blaster carries Inaccuracy10 = 9 (weapon 0x80), so the firing
// bearing is jittered by uniform [-9, +8] degrees before the velocity is
// projected (NovaWeapon fire path, apply_spread: NovaRandom_Range(spread*2)
// - spread; the original does not gate the spread on the caller either).
// The velocity is therefore only deterministic up to the spread envelope:
// assert the speed exactly and the bearing within the envelope, rather than
// exact components (those depend on the internal RNG roll sequence).
void CheckShotVelocity(const ActiveShot &shot,
                       float heading_deg,
                       float speed_px_per_frame) {
  CHECK(std::hypot(shot.vel_x, shot.vel_y) ==
        Catch::Approx(speed_px_per_frame));
  const float kRadToDeg = 180.0F / 3.14159265358979F;
  float bearing = std::atan2(shot.vel_x, -shot.vel_y) * kRadToDeg;
  float offset = bearing - heading_deg;
  // Wrap into (-180, 180].
  offset = std::fmod(std::fmod(offset, 360.0F) + 540.0F, 360.0F) - 180.0F;
  CHECK(std::abs(offset) <= 9.0F);
}
} // namespace

TEST_CASE("shot guidance preserves the shared bomb and rocket post-pass",
          "[weapon][guidance]") {
  GameState state;
  state.scenario.weapons.resize(1);
  Weapon &weapon = state.scenario.weapons[0];
  weapon.lifetime_ticks = 100;
  weapon.projectile_speed = 100.0F;
  ActiveShot shot;
  shot.weapon_id = 0;
  shot.life_ticks_remaining = 90.0F;
  shot.heading_deg = 0.0F;
  shot.vel_x = 10.0F;

  SECTION("freefall bomb weathervanes on a raw call") {
    weapon.weapon_mode_code = 5;
    NovaWeapon_UpdateShotGuidance(state, shot, 0.63F, 1);
    CHECK(shot.heading_deg == Catch::Approx(1.0F));
  }
  SECTION("freeflight rocket uses the executable's 95/5 blend") {
    weapon.weapon_mode_code = 6;
    NovaWeapon_UpdateShotGuidance(state, shot, 0.63F, 1);
    CHECK(shot.vel_x == Catch::Approx(9.5F));
    CHECK(shot.vel_y == Catch::Approx(-0.05F));
  }
  SECTION("lost-target state still reaches the rocket post-pass") {
    weapon.weapon_mode_code = 6;
    shot.guidance_state = 998;
    NovaWeapon_UpdateShotGuidance(state, shot, 0.63F, 1);
    CHECK(shot.vel_x == Catch::Approx(9.5F));
  }
}

TEST_CASE("shot guidance keeps normalized and raw age gates distinct",
          "[weapon][guidance]") {
  GameState state;
  state.scenario.weapons.resize(1);
  Weapon &weapon = state.scenario.weapons[0];
  weapon.weapon_mode_code = 1;
  weapon.lifetime_ticks = 100;
  weapon.projectile_speed = 100.0F;
  weapon.guided_turn_rate = 10.0F;
  Ship &target = state.ShipAt(1);
  target.is_active = true;
  target.current_system_id = 0;
  target.pos_x = 100.0F;
  ActiveShot shot;
  shot.weapon_id = 0;
  shot.target_ship_slot = 1;
  shot.system_id = 0;
  shot.heading_deg = 0.0F;

  SECTION("normal homing compares age with frame scale times fifteen") {
    shot.life_ticks_remaining = 90.0F;
    NovaWeapon_UpdateShotGuidance(state, shot, 0.5F, 0);
    CHECK(shot.heading_deg == Catch::Approx(5.0F));
  }
  SECTION("asteroid-decoy tracking turns by a full raw-call step") {
    state.asteroid_pool[0].active = true;
    state.asteroid_pool[0].target_pos_x = 100.0F;
    shot.guidance_state = 1;
    shot.target_ship_slot = 0;
    shot.life_ticks_remaining = 84.0F;
    NovaWeapon_UpdateShotGuidance(state, shot, 0.63F, 1);
    CHECK(shot.heading_deg == Catch::Approx(10.0F));
  }
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

  // A bank rebuild from owned outfits (the landed buy/sell/close path) keeps
  // the Light Blaster bank mounted, so firing survives any Outfitter
  // transaction.
  NovaWeapon_RebuildBanksFromOwnedOutfits(state);
  CHECK(state.weapon_count_by_class[0] == 1);
  CHECK(NovaWeapon_CanFireWeaponBank(state, state.player, 0));
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
  // Pin the barrel: the original re-rolls a random quadrant when the
  // per-ship state is unseeded, so leave it unpinned and the spawn x varies
  // +/- one barrel offset.
  state.player.muzzle_quadrant[0] = 0;

  FirePlayerPrimary(state);
  // One Light Blaster round spawned, from the group-0 barrel (not the hull
  // centre), moving upward (-y). Expected muzzle offset mirrors the
  // dedicated barrel test below: class geometry + near scale pair at
  // bearing 0 (acc_y = -forward < 0).
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].weapon_id == 0);
  const ShipClass *shuttle_cls =
      state.scenario.Ship(static_cast<std::int16_t>(0 + 0x80));
  REQUIRE(shuttle_cls != nullptr);
  CHECK(state.active_shots[0].pos_x ==
        Catch::Approx(100.0F +
                      static_cast<float>(shuttle_cls->muzzle_lateral[0][0]) *
                          shuttle_cls->muzzle_scale_near_x));
  CHECK(state.active_shots[0].pos_y ==
        Catch::Approx(200.0F -
                      static_cast<float>(shuttle_cls->muzzle_forward[0][0]) *
                          shuttle_cls->muzzle_scale_near_y -
                      static_cast<float>(shuttle_cls->muzzle_drop[0][0])));
  // WeaponDef Speed is px/frame * 100; the light blaster is 1500 -> 15
  // px/frame. The Inaccuracy10 = 9 bearing jitter (see CheckShotVelocity)
  // keeps the exact components random, so only the envelope is pinned.
  CheckShotVelocity(state.active_shots[0], 0.0F, 15.0F);
  // Life = WeaponDef Count (13 ticks).
  CHECK(state.active_shots[0].life_frames == 13);
  // The bank went into cooldown (reload 10 ticks) so a back-to-back fire is a
  // no-op while cooling down.
  CHECK(state.weapon_bank_cooldown[0] > 0.0F);
  FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1); // no second shot while cooling down
}

TEST_CASE("the starter light blaster is never an eligible secondary",
          "[weapon][data]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0;
  SeedStockWeaponBanks(state);

  const Weapon *light_blaster = state.scenario.Weapon(0x80);
  REQUIRE(light_blaster != nullptr);
  // Bible wëap Flags 0x0002 = "Weapon fired by second trigger". The Light
  // Blaster's Flags1c does not set it, so the secondary-cycle predicate
  // (PlayerTick_WeaponCycleContinuation 0x0044eab4, Eligibility checks
  // WeaponDef+0x20 & 2) must never select bank 0.
  CHECK((light_blaster->flags & 0x0002U) == 0U);

  // A single ineligible primary weapon is "none": the cycle denies and leaves
  // the live selection untouched (denial cue is transition index 3).
  state.player.active_weapon_bank_slot = -1;
  NovaWeapon_TickPlayerWeaponCommands(
      state, PlayerWeaponCommandInput{.cycle_secondary = true}, 0.0F);
  CHECK(state.player.active_weapon_bank_slot == -1);
  REQUIRE(state.pending_ui_sounds.size() == 1);
  CHECK(state.pending_ui_sounds[0].transition_index == 3);

  // Secondary fire with no selection must not spawn the mounted primary.
  state.pending_ui_sounds.clear();
  state.player.armor_points = 100.0F;
  NovaWeapon_TickPlayerWeaponCommands(
      state, PlayerWeaponCommandInput{.fire_secondary_held = true}, 0.0F);
  CHECK(state.active_shots.empty());

  // Positive control: the fire gates are open; the same command fires the
  // Light Blaster when the (unpersisted) selection is forced to bank 0, which
  // is exactly the reload bug this guards against.
  state.weapon_bank_cooldown.fill(0.0F);
  state.player.active_weapon_bank_slot = 0;
  NovaWeapon_TickPlayerWeaponCommands(
      state, PlayerWeaponCommandInput{.fire_secondary_held = true}, 0.0F);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].weapon_id == 0);
}

TEST_CASE("secondary cycling wraps past ineligible banks and fires the "
          "selected weapon",
          "[weapon]") {
  GameState state;
  state.scenario.weapons.resize(0x30);
  Ship &player = state.player;
  player.armor_points = 100.0F;
  player.shield_points = 100.0F;

  auto make_secondary = [&](std::int16_t bank) {
    Weapon &weapon = state.scenario.weapons[static_cast<std::size_t>(bank)];
    weapon.name = "Secondary";
    weapon.flags = 0x0002U; // Bible "Weapon fired by second trigger"
    weapon.weapon_mode_code = -1;
    weapon.ammo_type = -1; // unlimited
    weapon.reload_ticks = 10;
    weapon.lifetime_ticks = 30;
    weapon.projectile_speed = 1000.0F;
    state.weapon_count_by_class[static_cast<std::size_t>(bank) * 100] = 1;
  };
  make_secondary(0x10);
  make_secondary(0x20);
  // A primary weapon between the two secondaries must be skipped by the walk.
  state.scenario.weapons[0x11].flags = 0;
  state.scenario.weapons[0x11].weapon_mode_code = -1;
  state.weapon_count_by_class[0x11 * 100] = 1;

  const auto cycle = [&](bool backwards) {
    NovaWeapon_TickPlayerWeaponCommands(
        state,
        PlayerWeaponCommandInput{.cycle_secondary = true,
                                 .cycle_secondary_backwards = backwards},
        0.0F);
  };

  player.active_weapon_bank_slot = -1;
  cycle(false);
  CHECK(player.active_weapon_bank_slot == 0x10);
  cycle(false);
  CHECK(player.active_weapon_bank_slot == 0x20); // skips 0x11, wraps forward
  cycle(false);
  CHECK(player.active_weapon_bank_slot == 0x10);
  cycle(true);
  CHECK(player.active_weapon_bank_slot == 0x20); // wraps backward

  // Clear-selection command deselects and cues acceptance (index 2).
  player.active_weapon_bank_slot = 0x10;
  state.pending_ui_sounds.clear();
  NovaWeapon_TickPlayerWeaponCommands(
      state, PlayerWeaponCommandInput{.clear_secondary = true}, 0.0F);
  CHECK(player.active_weapon_bank_slot == -1);
  REQUIRE(state.pending_ui_sounds.size() == 1);
  CHECK(state.pending_ui_sounds[0].transition_index == 2);

  // Fire the selected secondary: one shot, weapon id == bank index.
  player.active_weapon_bank_slot = 0x10;
  NovaWeapon_TickPlayerWeaponCommands(
      state, PlayerWeaponCommandInput{.fire_secondary_held = true}, 0.0F);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].weapon_id == 0x10);

  // A secondary with ammo_type in [0,255] stays cycle-selectable while its
  // ammo counter is empty (the 0x800 "must stay fireable" flag is clear), but
  // Weapon_CanFireWeaponBank suppresses the shot. Prove selection by cycling
  // onto it from 0x10, then fire and observe no shot.
  Weapon &ammo_secondary = state.scenario.weapons[0x20];
  ammo_secondary.ammo_type = 0x21;
  state.weapon_secondary_count_by_class[0x21 * 100] = 0;
  player.active_weapon_bank_slot = 0x10;
  cycle(false);
  CHECK(player.active_weapon_bank_slot == 0x20); // still selectable
  state.active_shots.clear();
  state.weapon_bank_cooldown.fill(0.0F);
  NovaWeapon_TickPlayerWeaponCommands(
      state, PlayerWeaponCommandInput{.fire_secondary_held = true}, 0.0F);
  CHECK(state.active_shots.empty());

  // With flags_secondary 0x800 set the depleted bank becomes ineligible and
  // the cycle skips it.
  ammo_secondary.flags_secondary = 0x0800U;
  player.active_weapon_bank_slot = 0x10;
  cycle(false);
  CHECK(player.active_weapon_bank_slot == 0x10); // 0x20 skipped, wrapped

  // Reloading the cost bank makes the 0x800 bank eligible again; the cycle
  // must re-select it, then firing spends exactly one round.
  state.weapon_secondary_count_by_class[0x21 * 100] = 3;
  cycle(false);
  CHECK(player.active_weapon_bank_slot == 0x20);
  state.active_shots.clear();
  state.weapon_bank_cooldown.fill(0.0F);
  NovaWeapon_TickPlayerWeaponCommands(
      state, PlayerWeaponCommandInput{.fire_secondary_held = true}, 0.0F);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].weapon_id == 0x20);
  CHECK(state.weapon_secondary_count_by_class[0x21 * 100] == 2);
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
  // mode-6 direct-fire selector must accept the unguided mode and preserve the
  // unlimited sentinel.
  NovaWeapon_EnsureNpcWeaponBanks(state, npc);
  NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(state, npc, false);
  REQUIRE(npc.active_weapon_bank_slot == 0);
  REQUIRE(npc.ai_fire_trigger_latch != 0);

  NovaWeapon_FireNpcWeaponBank(state, npc);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].owner_ship_slot == 1);
  CHECK(state.active_shots[0].weapon_id == 0);
  CHECK(npc.npc_weapon_secondary_count_by_class[0] == -1);
  CHECK(npc.active_weapon_bank_slot == -1);
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

TEST_CASE("continuous NPC weapon handoff retains its bank and trigger",
          "[weapon][npc]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  std::int16_t continuous_bank = -1;
  for (std::size_t index = 0; index < state.scenario.weapons.size(); ++index) {
    const Weapon &weapon = state.scenario.weapons[index];
    if ((weapon.flags & 0x0002U) != 0U) {
      continuous_bank = static_cast<std::int16_t>(index);
      break;
    }
  }
  REQUIRE(continuous_bank >= 0);

  Ship &player = state.player;
  player.is_active = true;
  player.ship_instance_id = 0;
  player.current_system_id = 0;
  player.armor_points = 100.0F;
  player.pos_y = -200.0F;

  Ship &npc = state.ShipAt(1);
  npc.is_active = true;
  npc.ship_instance_id = 1;
  npc.ship_class_id = 0;
  npc.current_system_id = 0;
  npc.primary_target_ship_slot = 0;
  npc.armor_points = 100.0F;
  npc.active_weapon_bank_slot = continuous_bank;
  npc.ai_fire_trigger_latch = 1;
  npc.npc_weapon_count_by_class[static_cast<std::size_t>(continuous_bank)] = 1;
  npc.npc_weapon_secondary_count_by_class[static_cast<std::size_t>(
      continuous_bank)] = -1;
  npc.npc_weapon_bank_cooldown[static_cast<std::size_t>(continuous_bank)] =
      5.0F;

  NovaWeapon_FireNpcWeaponBank(state, npc);

  CHECK(state.active_shots.empty());
  CHECK(npc.active_weapon_bank_slot == continuous_bank);
  CHECK(npc.ai_fire_trigger_latch == 1);
}

TEST_CASE("NPC energy weapons do not need a secondary ammo counter",
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

  Ship &npc = state.ShipAt(1);
  npc.is_active = true;
  npc.ship_instance_id = 1;
  npc.ship_class_id = 0;
  npc.current_system_id = 0;
  npc.ai_behavior_code = 3;
  npc.primary_target_ship_slot = 0;
  npc.armor_points = 100.0F;
  npc.shield_points = 100.0F;

  NovaWeapon_EnsureNpcWeaponBanks(state, npc);
  NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(state, npc, false);
  REQUIRE(npc.active_weapon_bank_slot == 0);
  // The original treats the Light Blaster's ammo_type == -1 as an energy /
  // unlimited bank; a zero secondary counter must not suppress firing.
  npc.npc_weapon_secondary_count_by_class[0] = 0;
  NovaWeapon_FireNpcWeaponBank(state, npc);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(npc.npc_weapon_secondary_count_by_class[0] == 0);
}

TEST_CASE("brave-trader weapon selection survives the post-state refresh",
          "[weapon][npc]") {
  GameState state;
  Ship &npc = state.ShipAt(1);
  npc.ai_behavior_code = 2;
  npc.active_weapon_bank_slot = 17;
  npc.ai_fire_trigger_latch = 1;

  // Ship_EscortFireAtUnprovokedTarget returns immediately for original
  // AI behaviors below 5, so a behavior-1/2 pending volley (armed in
  // ApplyShipAiControls) must survive the post-state refresh.
  NovaAi_EscortFireAtUnprovokedTarget(state, npc);

  CHECK(npc.active_weapon_bank_slot == 17);
  CHECK(npc.ai_fire_trigger_latch == 1);
}

TEST_CASE("Fed Destroyer selects and fires its long-range missile",
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
  player.current_system_id = 1;
  player.armor_points = 30.0F;
  player.shield_points = 30.0F;
  player.pos_x = 0.0F;
  player.pos_y = 0.0F;

  Ship &npc = state.ShipAt(1);
  npc.is_active = true;
  npc.ship_instance_id = 1;
  npc.ship_class_id = 0x8d - 0x80;
  npc.current_system_id = 1;
  npc.ai_behavior_code = 3;
  npc.ai_state_code = 4;
  npc.ai_control_mode = 6;
  npc.primary_target_ship_slot = 0;
  npc.armor_points = 750.0F;
  npc.shield_points = 800.0F;
  npc.pos_x = 0.0F;
  npc.pos_y = 1200.0F;

  NovaWeapon_EnsureNpcWeaponBanks(state, npc);
  NovaAi_SelectGuidedWeaponBankForPrimaryTarget(state, npc);
  REQUIRE(npc.active_weapon_bank_slot == 6); // IR Missile, resource 0x86.
  REQUIRE(npc.ai_fire_trigger_latch != 0);

  NovaWeapon_FireNpcWeaponBank(state, npc);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].weapon_id == 6);
  // IR Missile Flags includes 0x0002: Ship_HandleShip retains both fields so
  // the bank can keep firing while the AI request remains asserted.
  CHECK(npc.active_weapon_bank_slot == 6);
  CHECK(npc.ai_fire_trigger_latch == 1);
}

TEST_CASE("Abomination can select and fire its pulse cannon", "[weapon][npc]") {
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
  player.pos_x = 0.0F;
  // The Fusion Pulse Battery's post-load effective range is 55 * 9 = 495 px;
  // mode-4 turret firing gets the original additional 32-pixel envelope.
  player.pos_y = -450.0F;

  Ship &npc = state.ShipAt(1);
  npc.is_active = true;
  npc.ship_instance_id = 1;
  // Class resource 0xf2 is the standard Abomination with pulse cannons.
  npc.ship_class_id = 0xf2 - 0x80;
  npc.current_system_id = 0;
  npc.ai_behavior_code = 4;
  npc.primary_target_ship_slot = 0;
  npc.armor_points = 100.0F;
  npc.shield_points = 100.0F;
  npc.pos_x = 0.0F;
  npc.pos_y = 0.0F;
  npc.heading = 0.0F;

  NovaWeapon_EnsureNpcWeaponBanks(state, npc);
  NovaAi_SelectWeaponBankForCurrentTarget(state, npc);
  // The turret selector must keep the pulse cannon distinct from the loaded
  // mode-1 hailgun; the original does not let the latter win by damage score.
  REQUIRE(npc.active_weapon_bank_slot == 34);

  NovaWeapon_FireNpcWeaponBank(state, npc);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].weapon_id == 34);
}

TEST_CASE("destroyed NPCs neither select nor fire a weapon bank",
          "[weapon][npc]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  Ship &npc = state.ShipAt(1);
  npc.is_active = true;
  npc.ship_instance_id = 1;
  npc.ship_class_id = 0;
  npc.current_system_id = 0;
  npc.ai_behavior_code = 5; // behavior >4 reaches the escort refresh
  npc.primary_target_ship_slot = 0;
  npc.armor_points = 0.0F;
  npc.shield_points = 100.0F;
  npc.active_weapon_bank_slot = 0;
  npc.ai_fire_trigger_latch = 1;

  state.player.is_active = true;
  state.player.ship_instance_id = 0;
  state.player.ship_class_id = 0;
  state.player.current_system_id = 0;

  NovaAi_EscortFireAtUnprovokedTarget(state, npc);
  CHECK(npc.active_weapon_bank_slot == -1);
  CHECK(npc.ai_fire_trigger_latch == 0);

  NovaWeapon_FireNpcWeaponBank(state, npc);
  CHECK(state.active_shots.empty());
}

TEST_CASE("a newly disabled NPC cannot retain a latched firing bank",
          "[weapon][npc]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  Ship &npc = state.ShipAt(1);
  npc.is_active = true;
  npc.ship_instance_id = 1;
  npc.ship_class_id = 0;
  npc.current_system_id = 0;
  const ShipClass *cls = state.scenario.Ship(0x80);
  REQUIRE(cls != nullptr);
  npc.armor_points = static_cast<float>(cls->base_armor) * 0.2F;
  REQUIRE(npc.armor_points > 0.0F);
  REQUIRE(NovaAiShip_IsDisabled(state, npc));
  npc.active_weapon_bank_slot = 0;
  npc.ai_fire_trigger_latch = 1;
  npc.weapon_sprite_flash_level = 16.0F;

  NovaWeapon_FireNpcWeaponBank(state, npc);

  CHECK(state.active_shots.empty());
  CHECK(npc.active_weapon_bank_slot == -1);
  CHECK(npc.ai_fire_trigger_latch == 0);
  // The firing path no longer resets this to 32; Ship_UpdateVisualState can
  // now decay the already-visible flash normally on subsequent frames.
  CHECK(npc.weapon_sprite_flash_level == Catch::Approx(16.0F));
}

TEST_CASE("an unsuccessful NPC fire request clears its stale bank latch",
          "[weapon][npc]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  Ship &npc = state.ShipAt(1);
  npc.is_active = true;
  npc.ship_instance_id = 1;
  npc.ship_class_id = 0;
  npc.current_system_id = 0;
  npc.armor_points = 100.0F;
  // Light Blaster bank 0 has no continuous-fire flag, so even an unsuccessful
  // handoff is consumed by Ship_HandleShip after Weapon_FireShipWeapons.
  npc.active_weapon_bank_slot = 0;
  npc.ai_fire_trigger_latch = 1;
  npc.npc_weapon_count_by_class[0] = 0;

  NovaWeapon_FireNpcWeaponBank(state, npc);

  CHECK(state.active_shots.empty());
  CHECK(npc.active_weapon_bank_slot == -1);
  CHECK(npc.ai_fire_trigger_latch == 0);
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
// fire cadence by weapon_count_by_class, the number of weapons in the bank)
// rather than being a no-op. Pins the earlier behaviour where the bank always
// cooled down at the full reload regardless of mount count, so buying a second
// Light Blaster changed nothing.
TEST_CASE("second mounted weapon halves the bank cooldown", "[weapon]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  state.player.ship_class_id = 0;
  SeedStockWeaponBanks(state);

  // One Light Blaster mounted: cooldown = reload(10) / ammo(1) = 10 ticks.
  REQUIRE(state.weapon_count_by_class[0] == 1);
  FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1);
  const float single_cooldown = state.weapon_bank_cooldown[0];
  REQUIRE(single_cooldown == Catch::Approx(10.0F));

  // A second identical weapon in the same bank halves the cooldown.
  state.weapon_count_by_class[0] = 2;
  state.weapon_bank_cooldown[0] = 0.0F; // back off cooldown
  FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 2); // previous shot still flying
  CHECK(state.weapon_bank_cooldown[0] == Catch::Approx(single_cooldown / 2.0F));
}

// Regression: the light blaster (a turret-group-0 weapon) should exit the
// nose barrel of the ship rather than dead-centre. The muzzle geometry is
// decoded from the class's sh\x8an descriptor into ShipClass.muzzle_* at
// scenario load (ShipClass.muzzle_ready); here we read the REAL Shuttle
// group-0 quadrant-0 barrel and check the fired shot is offset off the centre
// by the faithful Weapon_ApplyTurretSpreadVelocity vector: acc = polar(
// bearing, forward) + polar(bearing + 90, lateral), scaled by the near pair
// when acc_y < 0, then pos.y -= drop.
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

  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(0 + 0x80));
  REQUIRE(cls != nullptr);
  REQUIRE(cls->muzzle_ready);
  state.player.muzzle_quadrant[0] = 0; // pin the first barrel

  FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1);
  const auto &shot = state.active_shots[0];
  // Heading 0 -> displayed rotation frame 0 -> muzzle bearing 0:
  //   acc = (lateral, -forward); acc_y < 0 selects the NEAR scale pair.
  const float lateral = static_cast<float>(cls->muzzle_lateral[0][0]);
  const float forward = static_cast<float>(cls->muzzle_forward[0][0]);
  const float drop = static_cast<float>(cls->muzzle_drop[0][0]);
  CHECK(shot.pos_x ==
        Catch::Approx(100.0F + lateral * cls->muzzle_scale_near_x));
  CHECK(shot.pos_y ==
        Catch::Approx(200.0F - forward * cls->muzzle_scale_near_y - drop));
  // Velocity: 15 px/frame within the Inaccuracy10 = 9 spread envelope.
  CheckShotVelocity(shot, 0.0F, 15.0F);
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
  REQUIRE(state.weapon_count_by_class[0] == 1);

  // Step 6 (new_pilot_flow.cpp): build the fresh record and carry the banks.
  PilotFile record = PilotFile::Fresh();
  record.weapon_count_by_class = state.weapon_count_by_class;
  record.weapon_secondary_count_by_class =
      state.weapon_secondary_count_by_class;
  PilotFileApply(record, state);

  // The seeded Light Blaster must survive the record round-trip.
  CHECK(state.weapon_count_by_class[0] == 1);
  CHECK(NovaWeapon_CanFireWeaponBank(state, state.player, 0));
  FirePlayerPrimary(state);
  REQUIRE(state.active_shots.size() == 1);
}

// Regression: queued beam hits must expire at variable frame rates. The port
// drives the sim once per rendered frame with a fractional elapsed_ticks
// (frame_time_ms / 33.33 -> ~0.5 at 60fps); the original counts lifetime down
// by one whole tick per fixed 30-tick/s TickSystems call. Truncating that
// fraction to int16 before subtracting stalled the countdown at 0, leaving
// every beam on screen forever. Exercise the exact 60fps cadence: a 13-tick
// beam must still free its slot after ~26 frames of 0.5-tick steps.
TEST_CASE("queued beam hits expire at sub-tick frame rates", "[weapon][npc]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  Ship &owner = state.ShipAt(1);
  owner.is_active = true;
  owner.ship_instance_id = 1;
  owner.ship_class_id = 0;
  owner.current_system_id = 0;
  owner.armor_points = 100.0F;
  owner.shield_points = 100.0F;
  owner.pos_x = 100.0F;
  owner.pos_y = 100.0F;

  Ship &target = state.ShipAt(2);
  target.is_active = true;
  target.ship_instance_id = 2;
  target.ship_class_id = 0;
  target.current_system_id = 0;
  target.armor_points = 100.0F;
  target.shield_points = 100.0F;
  target.pos_x = 300.0F;
  target.pos_y = 100.0F;

  // Shot_QueueBeamHit (0x00427a90) stores the weapon def's lifetime without
  // checking the guidance mode, so the Light Blaster (bank 0, Count 13)
  // queues a 13-tick beam record.
  REQUIRE(NovaWeapon_QueueBeamHit(state, 1, 2, 0, -1));
  const BeamHit &beam = state.beam_hit_queue[0];
  REQUIRE(beam.lifetime_ticks == 13);
  REQUIRE(beam.lifetime_remainder == Catch::Approx(0.0F));

  // 60fps frame -> elapsed_ticks ~= 16.7 / 33.3 = 0.5. The pre-fix code
  // subtracted (int16)0.5 == 0 every frame, so this loop never terminated.
  int frames = 0;
  while (beam.lifetime_ticks >= 0 && frames < 200) {
    NovaWeapon_TickBeamHitQueue(state, 0.5F);
    ++frames;
  }
  // 13 ticks of life at 0.5 ticks/frame: lifetime reaches 0 after 26 frames
  // and the slot frees on the next whole-tick step (~frame 28).
  CHECK(frames <= 30);
  CHECK(beam.lifetime_ticks == -2); // reset to the inactive sentinel
}

TEST_CASE("inbound weapon threat tallies only live normal-lock shots",
          "[weapon][threat]") {
  GameState state;
  state.scenario.weapons.resize(2);
  state.scenario.weapons[0].mass_damage = 3;
  state.scenario.weapons[0].energy_damage = 2;
  state.scenario.weapons[1].mass_damage = -3;

  Ship &active = state.ShipAt(1);
  active.is_active = true;
  active.inbound_weapon_threat = 99;
  Ship &inactive = state.ShipAt(2);
  inactive.is_active = false;
  inactive.inbound_weapon_threat = 77;

  const auto add_shot = [&](std::int16_t weapon_id,
                            std::int16_t target,
                            float life,
                            std::int16_t guidance_state,
                            bool consumed = false) {
    ActiveShot shot;
    shot.weapon_id = weapon_id;
    shot.target_ship_slot = target;
    shot.life_ticks_remaining = life;
    shot.guidance_state = guidance_state;
    shot.consumed = consumed;
    state.active_shots.push_back(shot);
  };
  add_shot(0, 1, 1.0F, 0);       // +(3 + 2) / 2 = 2
  add_shot(1, 1, 1.0F, 0);       // -3 / 2 = -1, toward zero
  add_shot(0, 2, 1.0F, 0);       // inactive target is not reset/tallied
  add_shot(0, 1, 0.0F, 0);       // inactive lifetime
  add_shot(0, 1, -1.0F, 0);      // inactive lifetime
  add_shot(0, 1, 1.0F, 998);     // inert/lost-lock shot
  add_shot(0, 1, 1.0F, 1);       // asteroid-targeting shot
  add_shot(0, 1, 1.0F, 0, true); // already consumed by collision

  NovaWeapon_TallyInboundWeaponThreat(state);

  CHECK(active.inbound_weapon_threat == 1);
  CHECK(inactive.inbound_weapon_threat == 77);
}

TEST_CASE("inbound threat truncates each shot and honors the fixed pool",
          "[weapon][threat]") {
  GameState state;
  state.scenario.weapons.resize(1);
  state.scenario.weapons[0].mass_damage = 1;
  Ship &target = state.ShipAt(1);
  target.is_active = true;

  // Each half-point is truncated separately, rather than summing to one.
  for (int i = 0; i < 2; ++i) {
    ActiveShot shot;
    shot.weapon_id = 0;
    shot.target_ship_slot = 1;
    shot.life_ticks_remaining = 1.0F;
    state.active_shots.push_back(shot);
  }
  NovaWeapon_TallyInboundWeaponThreat(state);
  CHECK(target.inbound_weapon_threat == 0);

  state.active_shots.clear();
  state.active_shots.resize(0x81);
  for (ActiveShot &shot : state.active_shots) {
    shot.weapon_id = 0;
    shot.target_ship_slot = -1;
    shot.life_ticks_remaining = 1.0F;
  }
  state.active_shots[0x80].target_ship_slot = 1;
  state.scenario.weapons[0].mass_damage = 20;
  NovaWeapon_TallyInboundWeaponThreat(state);
  CHECK(target.inbound_weapon_threat == 0);
}

TEST_CASE("point defense prioritizes and damages an inbound guided shot",
          "[weapon][point-defense]") {
  GameState state;
  state.scenario.weapons.resize(2);
  Weapon &pd = state.scenario.weapons[0];
  pd.weapon_mode_code = 10;
  pd.beam_length_px = 200;
  pd.ammo_type = -1;
  pd.mass_damage = 2;
  pd.energy_damage = 3;
  pd.lifetime_ticks = 2;
  pd.reload_ticks = 10;
  state.scenario.weapons[1].weapon_mode_code = 1;

  state.scenario.ships.resize(1);
  Ship &defender = state.player;
  defender.is_active = true;
  defender.ship_instance_id = 0;
  defender.ship_class_id = 0;
  defender.armor_points = 100.0F;
  defender.pos_x = 0.0F;
  defender.pos_y = 0.0F;
  state.weapon_count_by_class[0] = 2;

  ActiveShot incoming;
  incoming.weapon_id = 1;
  incoming.target_ship_slot = 0;
  incoming.life_ticks_remaining = 20.0F;
  incoming.pos_x = 0.0F;
  incoming.pos_y = -100.0F;
  incoming.point_defense_durability = 4;
  state.active_shots.push_back(incoming);

  NovaWeapon_SelectTurretTargetWithinArc(state, defender);

  REQUIRE(state.beam_hit_queue[0].forced_targeting == 1);
  CHECK(state.beam_hit_queue[0].target_shot_slot == 0);
  CHECK(state.weapon_bank_cooldown[0] == Catch::Approx(5.0F));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(state.active_shots[0].point_defense_durability == 0);
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(state.active_shots[0].consumed);
}

TEST_CASE("mode-4 shots follow the owner class turreted-above container flag",
          "[weapon][render]") {
  // Bible Ship Flags3 0x0040: "ship's turreted shots appear above the ship".
  // Shot_SpawnShotFromWeapon 0x0041fd30 places a mode-4 shot in the layer-12
  // mode4_alt container when the owner class sets it, otherwise the default
  // (below-ships) container.
  GameState state;
  state.scenario.ships.assign(1, ShipClass{});
  state.scenario.weapons.assign(1, Weapon{});
  state.scenario.weapons[0].weapon_mode_code = 4;
  state.scenario.weapons[0].sprite_id = 0;
  state.scenario.weapons[0].projectile_speed = 100.0F;
  state.scenario.weapons[0].lifetime_ticks = 10;
  state.player.ship_class_id = 0;
  state.player.ship_instance_id = 0;

  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  REQUIRE(state.active_shots.size() == 1);
  CHECK_FALSE(state.active_shots[0].draws_above_ships);

  state.scenario.ships[0].availability_flags = 0x0040;
  state.active_shots.clear();
  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].draws_above_ships);

  // A non-mode-4 weapon never uses the above-ships container.
  state.scenario.weapons[0].weapon_mode_code = 1;
  state.active_shots.clear();
  REQUIRE(NovaWeapon_SpawnProjectile(state, 0, -1, 0) == 0);
  REQUIRE(state.active_shots.size() == 1);
  CHECK_FALSE(state.active_shots[0].draws_above_ships);
}

} // namespace game
