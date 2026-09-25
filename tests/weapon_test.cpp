#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>

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

// Synthetic NPC (slot 1) / player (slot 0) pair for the direct-fire selector
// (Weapon_SelectDirectFireWeaponBankForPrimaryTarget 0x0040d470) that does not
// depend on the shipped archives. Both ships sit in system 0 one 100-px step
// apart; pers_def_slot 0x3ff bypasses the cloak-engagement gate.
void SetUpDirectFirePair(GameState &state) {
  state.scenario.ships.resize(1);

  Ship &target = state.player;
  target.is_active = true;
  target.ship_instance_id = 0;
  target.ship_class_id = 0;
  target.current_system_id = 0;
  target.pos_x = 0.0F;
  target.pos_y = 0.0F;
  target.shield_points = 100.0F;

  Ship &npc = state.ShipAt(1);
  npc.is_active = true;
  npc.ship_instance_id = 1;
  npc.ship_class_id = 0;
  npc.current_system_id = 0;
  npc.primary_target_ship_slot = 0;
  npc.pers_def_slot = 0x3ff;
  npc.pos_x = 0.0F;
  npc.pos_y = 100.0F;
  npc.active_weapon_bank_slot = -1;
  npc.ai_fire_trigger_latch = 0;
  npc.npc_weapon_count_by_class.fill(0);
  npc.npc_weapon_secondary_count_by_class.fill(0);
  npc.npc_weapon_bank_cooldown.fill(0.0F);
}

// Arms one synthetic NPC direct-fire bank. mode -1/0/6 are unguided; 1 is
// guided. ammo_type -1 is the unlimited/energy case, so no secondary counter
// is required by NovaWeapon_CanFireWeaponBank.
void ArmNpcDirectFireBank(GameState &state,
                          std::int16_t bank,
                          std::int16_t mode,
                          std::int16_t mass_damage,
                          std::int16_t energy_damage,
                          float range_scalar,
                          std::int16_t blast_radius = 0) {
  const auto index = static_cast<std::size_t>(bank);
  if (state.scenario.weapons.size() <= index) {
    state.scenario.weapons.resize(index + 1);
  }
  Weapon &weapon = state.scenario.weapons[index];
  weapon.name = "SyntheticDirectFire";
  weapon.weapon_mode_code = mode;
  weapon.ammo_type = -1;
  weapon.mass_damage = mass_damage;
  weapon.energy_damage = energy_damage;
  weapon.range_scalar = range_scalar;
  weapon.blast_radius = blast_radius;
  weapon.beam_length_px = 0;
  weapon.flags_secondary = 0;
  state.ShipAt(1).npc_weapon_count_by_class[index] = 1;
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

// Ghidra Weapon_FireShipWeapons (0x00414550): when the primary target is the
// player (slot 0), the post-volley bank cooldown is scaled by a combat-rating
// ladder (1.75/1.5/1.25/1.1) as g_player_combat_rating_points climbs through
// base_strength * 100/400/800/1600. The original reads the base unit from
// g_ship_class_defs[0].strength (class resource 0x80, shipped Shuttle = 2);
// the port pins it (GameState::kCombatRatingBaseStrength) so a mod editing
// class 0 cannot rescale the rating system. Non-player targets and ratings at
// or above 1600x take the unscaled baseline.
TEST_CASE("NPC fire cooldown scales with the player combat-rating ladder",
          "[weapon][npc]") {
  const auto fire_and_read_cooldown = [](std::int32_t rating,
                                         std::int16_t class0_strength,
                                         std::int16_t target_slot) {
    GameState state;
    SetUpDirectFirePair(state);
    state.scenario.ships[0].strength = class0_strength;
    Ship &npc = state.ShipAt(1);
    npc.primary_target_ship_slot = target_slot;
    npc.armor_points = 100.0F;
    npc.shield_points = 100.0F;
    ArmNpcDirectFireBank(state, 0, -1, 5, 5, 500.0F);
    state.scenario.weapons[0].reload_ticks = 10;
    state.scenario.weapons[0].flags = 0;
    state.scenario.weapons[0].projectile_speed = 100.0F;
    state.scenario.weapons[0].lifetime_ticks = 30;
    npc.active_weapon_bank_slot = 0;
    npc.ai_fire_trigger_latch = 1;
    state.player_combat_rating_points = rating;
    NovaWeapon_FireNpcWeaponBank(state, npc);
    return npc.npc_weapon_bank_cooldown[0];
  };

  // Pinned base unit 2 -> thresholds 200/800/1600/3200.
  CHECK(fire_and_read_cooldown(0, 2, 0) == Catch::Approx(17.5F));
  CHECK(fire_and_read_cooldown(199, 2, 0) == Catch::Approx(17.5F));
  CHECK(fire_and_read_cooldown(799, 2, 0) == Catch::Approx(15.0F));
  CHECK(fire_and_read_cooldown(800, 2, 0) == Catch::Approx(12.5F));
  CHECK(fire_and_read_cooldown(1600, 2, 0) == Catch::Approx(11.0F));
  CHECK(fire_and_read_cooldown(3200, 2, 0) == Catch::Approx(10.0F));
  // Pinned divergence: editing class-0 Strength must not move the ladder.
  CHECK(fire_and_read_cooldown(0, 50, 0) == Catch::Approx(17.5F));
  CHECK(fire_and_read_cooldown(800, 50, 0) == Catch::Approx(12.5F));
  // A non-player primary target (slot -1) never scales, even at rating 0.
  CHECK(fire_and_read_cooldown(0, 2, -1) == Catch::Approx(10.0F));
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

TEST_CASE("direct-fire selector picks energy for shielded targets and mass "
          "for bare hulls",
          "[weapon][npc]") {
  GameState state;
  SetUpDirectFirePair(state);
  // Bank 0 is mass-heavy, bank 1 energy-heavy; both unguided (mode -1) and in
  // range. The original switches on target.shield_points >= 0.0.
  ArmNpcDirectFireBank(state, 0, -1, 50, 1, 1000.0F);
  ArmNpcDirectFireBank(state, 1, -1, 1, 50, 1000.0F);

  SECTION("a shielded target takes the best energy bank") {
    state.player.shield_points = 100.0F;
    NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(
        state, state.ShipAt(1), false);
    CHECK(state.ShipAt(1).active_weapon_bank_slot == 1);
    CHECK(state.ShipAt(1).ai_fire_trigger_latch == 1);
  }
  SECTION("an unshielded target takes the best mass bank") {
    // Negative shield_points is the original's "no shields" sentinel.
    state.player.shield_points = -1.0F;
    NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(
        state, state.ShipAt(1), false);
    CHECK(state.ShipAt(1).active_weapon_bank_slot == 0);
    CHECK(state.ShipAt(1).ai_fire_trigger_latch == 1);
  }
}

TEST_CASE("mode-6 direct fire requires the target outside the blast envelope",
          "[weapon][npc]") {
  GameState state;
  SetUpDirectFirePair(state);
  // blast_radius 8 * 2.5 = a 20-px-per-axis placement gate.
  ArmNpcDirectFireBank(state, 0, 6, 10, 10, 1000.0F, 8);

  SECTION("target inside the blast ellipse is not armed") {
    // NPC at (0, 100): |dx| = 0 < 20, so the gate rejects the bank.
    NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(
        state, state.ShipAt(1), false);
    CHECK(state.ShipAt(1).active_weapon_bank_slot == -1);
    CHECK(state.ShipAt(1).ai_fire_trigger_latch == 0);
  }
  SECTION("target clear on both axes is armed") {
    state.ShipAt(1).pos_x = 100.0F;
    state.ShipAt(1).pos_y = 100.0F;
    NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(
        state, state.ShipAt(1), false);
    CHECK(state.ShipAt(1).active_weapon_bank_slot == 0);
    CHECK(state.ShipAt(1).ai_fire_trigger_latch == 1);
  }
}

TEST_CASE("direct-fire selector retries once with guided banks when no "
          "unguided bank qualified",
          "[weapon][npc]") {
  GameState state;
  SetUpDirectFirePair(state);
  ArmNpcDirectFireBank(state, 0, 1, 10, 10, 1000.0F); // guided, in range

  NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(
      state, state.ShipAt(1), false);
  CHECK(state.ShipAt(1).active_weapon_bank_slot == 0);
  CHECK(state.ShipAt(1).ai_fire_trigger_latch == 1);
}

TEST_CASE("seeing an out-of-range unguided bank suppresses the guided retry",
          "[weapon][npc]") {
  GameState state;
  SetUpDirectFirePair(state);
  // Bank 0 is unguided but too short (mode 0 uses beam_length_px + 32 = 32 px
  // against a 100-px separation); bank 1 is a guided bank that would win the
  // relaxed retry if it were allowed to run.
  ArmNpcDirectFireBank(state, 0, 0, 10, 10, 0.0F);
  ArmNpcDirectFireBank(state, 1, 1, 10, 10, 1000.0F);

  NovaAi_SelectDirectFireWeaponBankForPrimaryTarget(
      state, state.ShipAt(1), false);
  CHECK(state.ShipAt(1).active_weapon_bank_slot == -1);
  CHECK(state.ShipAt(1).ai_fire_trigger_latch == 0);
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
  NovaAi_FireTurretAtTarget(state, npc);
  // The turret selector must keep the pulse cannon distinct from the loaded
  // mode-1 hailgun; the original does not let the latter win by damage score.
  REQUIRE(npc.active_weapon_bank_slot == 34);

  NovaWeapon_FireNpcWeaponBank(state, npc);
  REQUIRE(state.active_shots.size() == 1);
  CHECK(state.active_shots[0].weapon_id == 34);
}

TEST_CASE("a destroyed NPC cannot fire a weapon bank", "[weapon][npc]") {
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

  // The refresh revalidates the target and delegates to the turret selector,
  // whose own disabled gate returns without touching a stale latched bank
  // (matching Weapon_FireTurretAtTarget 0x0040ce00).
  NovaAi_EscortFireAtUnprovokedTarget(state, npc);
  CHECK(npc.active_weapon_bank_slot == 0);
  CHECK(npc.ai_fire_trigger_latch == 1);

  // The firing handoff is what suppresses the destroyed ship's bank.
  NovaWeapon_FireNpcWeaponBank(state, npc);
  CHECK(state.active_shots.empty());
  CHECK(npc.active_weapon_bank_slot == -1);
  CHECK(npc.ai_fire_trigger_latch == 0);
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

// Thunderhead Lance geometry (wëap 0x00a6: Guidance 0, BeamLength 100): a
// mode-0 beam is fired along the owner's heading and ends at BeamLength, with
// no target-following. Shot_UpdateBeamHitQueue (0x0042f270) gates the target
// endpoint on weapon_mode_code != 0, so a fixed beam must stay straight even
// when a target is recorded. This pins that exact geometry: an off-axis target
// is ignored, and an on-axis target is hit/truncated.
TEST_CASE("mode-zero beams stay on the owner heading and stop at BeamLength",
          "[weapon][beam]") {
  GameState state;
  state.scenario.weapons.resize(1);
  Weapon &beam = state.scenario.weapons[0];
  beam.weapon_mode_code = 0;
  beam.beam_length_px = 100;
  beam.lifetime_ticks = 10;
  beam.beam_falloff = 0x10;
  // NovaWeapon_CanProjectileHitShip resolves the target class through the
  // scenario table; one zero-capability class makes owner and target eligible.
  state.scenario.ships.resize(1);
  state.player.current_system_id = 0;

  Ship &owner = state.ShipAt(0);
  owner.is_active = true;
  owner.ship_instance_id = 0;
  owner.ship_class_id = 0;
  owner.current_system_id = 0;
  owner.armor_points = 100.0F;
  owner.pos_x = 100.0F;
  owner.pos_y = 200.0F;
  owner.heading = 0.0F; // heading 0 points toward -y

  Ship &target = state.ShipAt(1);
  target.is_active = true;
  target.ship_instance_id = 1;
  target.ship_class_id = 0;
  target.current_system_id = 0;
  target.armor_points = 100.0F;

  // On-axis but past the reach (BeamLength 100 + ceil(trunc(75*0.66)/2) = 125):
  // the straight beam ignores it and still ends at BeamLength, 100 px up.
  target.pos_x = 100.0F;
  target.pos_y = 0.0F;
  REQUIRE(NovaWeapon_QueueBeamHit(state,
                                  0,
                                  1,
                                  0,
                                  /*forced_targeting=*/-1,
                                  /*firing_bearing_deg=*/0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  const BeamHit &queued = state.beam_hit_queue[0];
  CHECK(queued.target_x == Catch::Approx(100.0F));
  CHECK(queued.target_y == Catch::Approx(100.0F));
  CHECK_FALSE(queued.impact_resolved);

  // Nearby (56 px) but 45 deg off the heading, well outside the 15 deg cone:
  // still ignored, still ends at BeamLength.
  target.pos_x = 140.0F;
  target.pos_y = 160.0F;
  state.beam_hit_queue[0] = BeamHit{};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, 1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(queued.target_x == Catch::Approx(100.0F));
  CHECK(queued.target_y == Catch::Approx(100.0F));
  CHECK_FALSE(queued.impact_resolved);

  // Same-government, non-squad target directly ahead and in range: the
  // original beam scan does NOT reject same-government contacts, so it is hit
  // (unlike the projectile Weapon_CanWeaponHitTarget gate).
  owner.faction_or_government_id = 0;
  target.faction_or_government_id = 0;
  target.pos_x = 100.0F;
  target.pos_y = 140.0F;
  state.beam_hit_queue[0] = BeamHit{};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, 1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(queued.target_y == Catch::Approx(200.0F - 45.0F));
  CHECK(queued.impact_resolved);
  owner.faction_or_government_id = -1;
  target.faction_or_government_id = -1;

  // Friendly squad exclusion: the candidate is the owner's direct subordinate
  // (its squad leader is the owner), so the scan skips it.
  target.squad_leader_ship_slot = 0;
  state.beam_hit_queue[0] = BeamHit{};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, 1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(queued.target_y == Catch::Approx(100.0F));
  CHECK_FALSE(queued.impact_resolved);
  target.squad_leader_ship_slot = -1;

  // Friendly squad exclusion: the candidate is the owner's own squad leader.
  owner.squad_leader_ship_slot = 1;
  state.beam_hit_queue[0] = BeamHit{};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, 1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(queued.target_y == Catch::Approx(100.0F));
  CHECK_FALSE(queued.impact_resolved);
  owner.squad_leader_ship_slot = -1;

  // Directly ahead at 60 px: the beam is truncated at distance minus
  // 0.2*frame_height (the 0x0042f270 DAT_005753d8 scale). This target has no
  // collision mask, so frame_height uses the 0x4b = 75 fallback: 60 - 15 = 45
  // px, and the hit resolves.
  target.pos_x = 100.0F;
  target.pos_y = 140.0F;
  state.beam_hit_queue[0] = BeamHit{};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, 1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(queued.target_x == Catch::Approx(100.0F));
  CHECK(queued.target_y == Catch::Approx(200.0F - 45.0F));
  CHECK(queued.impact_resolved);

  // A valid target near the outer reach (120 px) truncates to 120 - 15 = 105
  // px, which is longer than BeamLength: the original does not re-clamp the
  // visible beam to BeamLength after the contact truncation, so preserve that.
  target.pos_x = 100.0F;
  target.pos_y = 80.0F;
  state.beam_hit_queue[0] = BeamHit{};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, 1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(queued.target_x == Catch::Approx(100.0F));
  CHECK(queued.target_y == Catch::Approx(200.0F - 105.0F));
  CHECK(200.0F - queued.target_y > static_cast<float>(beam.beam_length_px));
  CHECK(queued.impact_resolved);

  // The source follows the live owner: a moving muzzle must not leave the
  // beam behind at its queue-time position.
  state.beam_hit_queue[0] = BeamHit{};
  target.is_active = false;
  owner.pos_x = 250.0F;
  owner.pos_y = 400.0F;
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, 0, -1, 0));
  owner.pos_x = 260.0F;
  owner.pos_y = 410.0F;
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(queued.source_x == Catch::Approx(260.0F));
  CHECK(queued.source_y == Catch::Approx(410.0F));
  CHECK(queued.target_x == Catch::Approx(260.0F));
  CHECK(queued.target_y == Catch::Approx(410.0F - 100.0F));
}

// Ghidra Shot_UpdateBeamHitQueue (0x0042f270): the incidental mode-0 sweep
// picks the aggro-suppression flag from the impact site. A stray player beam
// that clips an NPC without the player locking it (no primary target and no
// recorded beam target) must pass suppress=false, so Ship_ApplyDamageToShip's
// 50-point player-aggro accumulator gates the response instead of the first
// contact immediately turning the NPC hostile. When the swept hit is the
// owner's live primary target, or the beam's recorded target, suppress=true and
// the lock-on path bypasses the accumulator.
TEST_CASE("stray mode-zero beam contact is gated by the player-aggro threshold",
          "[weapon][beam][ai]") {
  GameState state;
  state.scenario.weapons.resize(1);
  Weapon &beam = state.scenario.weapons[0];
  beam.weapon_mode_code = 0;
  beam.beam_length_px = 100;
  beam.lifetime_ticks = 10;
  beam.mass_damage = 10;
  beam.reload_ticks = 20.0F;
  state.scenario.ships.resize(1);
  state.player.current_system_id = 0;

  Ship &owner = state.ShipAt(0);
  owner.is_active = true;
  owner.ship_instance_id = 0;
  owner.ship_class_id = 0;
  owner.current_system_id = 0;
  owner.pos_x = 100.0F;
  owner.pos_y = 200.0F;
  owner.heading = 0.0F; // heading 0 points toward -y
  owner.primary_target_ship_slot = -1;

  Ship &target = state.ShipAt(1);
  target.is_active = true;
  target.ship_instance_id = 1;
  target.ship_class_id = 0;
  target.current_system_id = 0;
  target.armor_points = 1000.0F;
  target.ai_behavior_code = 1;
  target.pos_x = 100.0F;
  target.pos_y = 140.0F; // 60 px directly ahead, inside the cone

  // First untargeted contact: accumulator is 0, so the NPC takes damage but
  // does not switch to the player.
  REQUIRE(target.player_aggro_accumulator == 0.0F);
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(state.beam_hit_queue[0].impact_resolved);
  CHECK(target.primary_target_ship_slot == -1);
  CHECK(target.ai_state_code != 4);
  // The hit still banks aggro pressure for the next contact.
  CHECK(target.player_aggro_accumulator > 0.0F);

  // Once the pressure crosses 50 the next untargeted contact retaliates.
  target.player_aggro_accumulator = 50.0F;
  state.beam_hit_queue[0] = BeamHit{};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(target.primary_target_ship_slot == 0);
  CHECK(target.ai_state_code == 4);

  // Locking the NPC as the player's primary target marks the sweep as
  // targeted even though the beam record itself has target -1.
  target.primary_target_ship_slot = -1;
  target.ai_state_code = 0;
  target.player_aggro_accumulator = 0.0F;
  owner.primary_target_ship_slot = 1;
  state.beam_hit_queue[0] = BeamHit{};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(target.primary_target_ship_slot == 0);
  CHECK(target.ai_state_code == 4);
}

// Ghidra Shot_UpdateBeamHitQueue (0x0042f270) negative-impact-impulse arm:
// a tractor/repulsor beam arms a velocity-match lock. When the target is at
// most 4/3 the source's mass the TARGET locks onto the source; otherwise the
// SOURCE self-locks, is stamped and pulled toward the target. A mass < 1 ton or
// capability Flags 0x400 target suppresses the impulse entirely.
TEST_CASE("negative beam impulse arms the velocity-match producer",
          "[weapon][beam]") {
  GameState state;
  state.scenario.weapons.resize(1);
  Weapon &beam = state.scenario.weapons[0];
  beam.weapon_mode_code = 3; // non-zero, so the recorded target is used direct
  beam.beam_length_px = 100;
  beam.lifetime_ticks = 10;
  beam.impact_impulse = -20;
  state.scenario.ships.resize(2);
  state.scenario.ships[0].mass_tons = 100;
  state.scenario.ships[0].speed = 300.0F;
  state.scenario.ships[0].base_shield = 100;
  state.scenario.ships[0].base_armor = 100;
  state.scenario.ships[1].mass_tons = 10;
  state.scenario.ships[1].speed = 300.0F;
  state.scenario.ships[1].base_shield = 100;
  state.scenario.ships[1].base_armor = 100;
  state.player.current_system_id = 0;
  state.tick_60hz = 1234;

  Ship &owner = state.ShipAt(0);
  owner.is_active = true;
  owner.ship_instance_id = 0;
  owner.ship_class_id = 0;
  owner.current_system_id = 0;
  owner.armor_points = 100.0F;
  owner.pos_x = 100.0F;
  owner.pos_y = 100.0F;
  Ship &target = state.ShipAt(1);
  target.is_active = true;
  target.ship_instance_id = 1;
  target.ship_class_id = 1;
  target.current_system_id = 0;
  target.armor_points = 100.0F;
  target.pos_x = 300.0F;
  target.pos_y = 100.0F;

  // Target lighter (10 * 0.75 = 7.5 <= 100): the target locks onto the source.
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, 1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(target.velocity_match_target_ship_slot == 0);
  CHECK(target.velocity_match_start_tick_60hz == 1234);
  CHECK(owner.velocity_match_target_ship_slot == -1);

  // Target heavier (100 * 0.75 = 75 > 10): the source self-locks and is pulled
  // toward the target.
  state.scenario.ships[0].mass_tons = 10;
  state.scenario.ships[1].mass_tons = 100;
  target.velocity_match_target_ship_slot = -1;
  target.velocity_match_start_tick_60hz = 0;
  owner.velocity_match_target_ship_slot = -1;
  owner.vel_x = 0.0F;
  owner.vel_y = 0.0F;
  state.beam_hit_queue[0] = BeamHit{};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, 1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(owner.velocity_match_target_ship_slot == 0);
  CHECK(owner.velocity_match_start_tick_60hz == 1234);
  CHECK(target.velocity_match_target_ship_slot == -1);
  CHECK((owner.vel_x != 0.0F || owner.vel_y != 0.0F));

  // Capability Flags 0x400 target: the impulse is suppressed and no lock arms.
  state.scenario.ships[1].capability_flags = 0x0400U;
  owner.velocity_match_target_ship_slot = -1;
  owner.vel_x = 0.0F;
  owner.vel_y = 0.0F;
  state.beam_hit_queue[0] = BeamHit{};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, 1, 0, -1, 0));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(owner.velocity_match_target_ship_slot == -1);
  CHECK(target.velocity_match_target_ship_slot == -1);
  CHECK(owner.vel_x == 0.0F);
  CHECK(owner.vel_y == 0.0F);
}

// Ghidra Shot_HandleShot shot_fade_rate quads (WeaponDef +0xb8): ordinary vs
// flags_tertiary 0x2 additive, the additive-negative no-advance quirk, the
// positive RGB min-4 clamp, the no-fade full-0x20 path, and the pre-advance
// latched alpha.
TEST_CASE("shot fade quads preserve additive quirks and pre-advance alpha",
          "[weapon][shot]") {
  auto run =
      [](std::int16_t fade_rate, std::uint16_t flags_tertiary, float elapsed) {
        GameState state;
        state.scenario.weapons.resize(1);
        Weapon &w = state.scenario.weapons[0];
        w.weapon_mode_code = 4;
        w.lifetime_ticks = 30;
        w.projectile_speed = 100;
        w.shot_fade_rate = fade_rate;
        w.flags_tertiary = flags_tertiary;
        ActiveShot s;
        s.weapon_id = 0;
        s.owner_ship_slot = -1;
        s.system_id = 0;
        s.life_ticks_remaining = 30.0F;
        state.active_shots.push_back(s);
        NovaWeapon_TickShots(state, elapsed);
        return state.active_shots[0];
      };

  // Ordinary negative: a2 = 32 - 0, alpha 0, progress advances by 4*0.7.
  const ActiveShot ordinary_neg = run(-4, 0, 0.7F);
  CHECK(ordinary_neg.visibility_attenuation == Catch::Approx(32.0F));
  CHECK(ordinary_neg.fade_alpha == Catch::Approx(0.0F));
  CHECK_FALSE(ordinary_neg.fade_additive);
  CHECK(ordinary_neg.visibility_or_falloff == Catch::Approx(2.8F));

  // Additive negative: a2 = 0x20, alpha = progress = 0, and the original does
  // NOT advance visibility in this branch.
  const ActiveShot additive_neg = run(-4, 0x0002U, 0.7F);
  CHECK(additive_neg.visibility_attenuation == Catch::Approx(32.0F));
  CHECK(additive_neg.fade_alpha == Catch::Approx(0.0F));
  CHECK(additive_neg.visibility_or_falloff == Catch::Approx(0.0F));

  // Ordinary positive: a2 = min(0, 0x1f) = 0, alpha 1, progress advances.
  const ActiveShot ordinary_pos = run(4, 0, 0.7F);
  CHECK(ordinary_pos.visibility_attenuation == Catch::Approx(0.0F));
  CHECK(ordinary_pos.fade_alpha == Catch::Approx(1.0F));
  CHECK(ordinary_pos.visibility_or_falloff == Catch::Approx(2.8F));

  // Additive positive: a2 = 0x20, alpha = max(4, 32 - 0)/32 = 1.
  const ActiveShot additive_pos = run(4, 0x0002U, 0.7F);
  CHECK(additive_pos.visibility_attenuation == Catch::Approx(32.0F));
  CHECK(additive_pos.fade_alpha == Catch::Approx(1.0F));
  CHECK(additive_pos.visibility_or_falloff == Catch::Approx(2.8F));

  // Additive with no fade rate on a >=16-bit surface: all corners 0x20.
  const ActiveShot additive_static = run(0, 0x0002U, 0.7F);
  CHECK(additive_static.visibility_attenuation == Catch::Approx(32.0F));
  CHECK(additive_static.fade_alpha == Catch::Approx(1.0F));
}

// Ghidra Shot_HandleShot animated branch: the displayed frame is latched
// BEFORE the animation increment, so frame 0 shows on the first advancing
// call (the increment only shows next call).
TEST_CASE("animated shot displays the pre-increment frame", "[weapon][shot]") {
  GameState state;
  state.scenario.weapons.resize(1);
  Weapon &w = state.scenario.weapons[0];
  w.weapon_mode_code = 4;
  w.lifetime_ticks = 30;
  w.projectile_speed = 100;
  w.flags = 0x0001U; // animated sprite set
  w.beam_width_or_animation_frame_delay = 1;
  state.player.current_system_id = 0;
  ActiveShot s;
  s.weapon_id = 0;
  s.owner_ship_slot = -1;
  s.system_id = 0;
  s.life_ticks_remaining = 30.0F;
  state.active_shots.push_back(s);
  NovaWeapon_TickShots(state, 1.0F);
  CHECK(state.active_shots[0].frame_cycle_index == 1);
  CHECK(state.active_shots[0].display_frame == 0);
  NovaWeapon_TickShots(state, 1.0F);
  CHECK(state.active_shots[0].display_frame == 1);
}

// Ghidra 0x00435830 Shot_HandleShot smoke-puff arm + 0x0042c660
// Shot_UpdateWeaponSmokePuffs: a Flags1 0x200/0x400 weapon queues a pooled
// smoke sprite selected from SmokeSet, and the pool advances/deactivates on
// the 0.25 * tick cadence. Also pins the shot_fade_rate visibility accumulator.
TEST_CASE("shot smoke puffs spawn and fade on the effect cadence",
          "[weapon][shot]") {
  GameState state;
  state.scenario.weapons.resize(1);
  Weapon &w = state.scenario.weapons[0];
  w.weapon_mode_code = 4;
  w.lifetime_ticks = 30;
  w.projectile_speed = 100;
  w.flags = 0x0400U; // small smoke variant (0), no 0x800 ping-pong
  w.smoke_set = 1;   // only SmokeSet 0/1 have a loaded cicn set
  w.shot_fade_rate = -4;
  state.player.current_system_id = 0;

  ActiveShot shot;
  shot.weapon_id = 0;
  shot.owner_ship_slot = -1;
  shot.system_id = 0;
  shot.life_ticks_remaining = 30.0F;
  shot.pos_x = 10.0F;
  shot.pos_y = 20.0F;
  state.active_shots.push_back(shot);

  // 0.7 normalized ticks is one original 21 ms flight call.
  NovaWeapon_TickShots(state, 0.7F);
  int active = 0;
  for (const WeaponSmokePuff &puff : state.weapon_smoke_puffs) {
    if (puff.life >= 0.0F) {
      ++active;
      CHECK(puff.effect_slot == 1);
      CHECK(puff.variant == 0);
      CHECK(puff.pos_x == Catch::Approx(10.0F));
    }
  }
  CHECK(active == 1);
  // Negative shot_fade_rate advances visibility by |rate| * ticks.
  CHECK(state.active_shots[0].visibility_or_falloff == Catch::Approx(2.8F));

  // Life advances by 0.25 * 0.7 = 0.175 per call; the 8-frame variant dies
  // once the truncated frame reaches 8 (~46 calls after it spawns). The shot
  // spawns puffs until it expires at ~43 calls, so tick well past that.
  for (int call = 0; call < 120; ++call) {
    NovaWeapon_TickShots(state, 0.7F);
  }
  for (const WeaponSmokePuff &puff : state.weapon_smoke_puffs) {
    CHECK(puff.life < 0.0F);
  }
}

// The original loader (0x004ae7fe) builds sprite sets only for SmokeSet 0 and
// 1, and Shot_SpawnWeaponSmokePuff's slot scan requires a non-null set. A
// higher SmokeSet therefore spawns nothing at all and must not consume a pool
// slot (which would starve a later valid puff).
TEST_CASE("shot smoke puff with a null SmokeSet consumes no pool slot",
          "[weapon][shot]") {
  GameState state;
  state.scenario.weapons.resize(1);
  Weapon &w = state.scenario.weapons[0];
  w.weapon_mode_code = 4;
  w.lifetime_ticks = 30;
  w.projectile_speed = 100;
  w.flags = 0x0400U;
  w.smoke_set = 2; // no loaded set
  state.player.current_system_id = 0;

  ActiveShot shot;
  shot.weapon_id = 0;
  shot.owner_ship_slot = -1;
  shot.system_id = 0;
  shot.life_ticks_remaining = 30.0F;
  state.active_shots.push_back(shot);

  NovaWeapon_TickShots(state, 0.7F);

  for (const WeaponSmokePuff &puff : state.weapon_smoke_puffs) {
    CHECK(puff.life < 0.0F);
  }
}

// Regression: 0x0042f270's first contact clause is candidate_slot !=
// owner_slot, so a fixed beam can never intercept its own firing ship. The
// port's BeamCandidateEligible omitted that clause. The geometry test above
// masked the bug because its owner faced heading 0, and BearingDeg(point,
// same point) yields 180 deg (atan2(+0,-0)), which fell outside the forward
// cone. Facing the owner 180 deg puts that same self-bearing on-axis, so only
// the owner-slot check keeps the active owner from hitting itself.
TEST_CASE("mode-zero beams never intercept their own owner", "[weapon][beam]") {
  GameState state;
  state.scenario.weapons.resize(1);
  Weapon &beam = state.scenario.weapons[0];
  beam.weapon_mode_code = 0;
  beam.beam_length_px = 100;
  beam.lifetime_ticks = 10;
  beam.beam_falloff = 0x10;
  state.scenario.ships.resize(1);
  state.player.current_system_id = 0;

  // The owner has a valid ship class, so without the owner-slot exclusion it
  // is a fully eligible self-candidate at distance 0.
  Ship &owner = state.ShipAt(0);
  owner.is_active = true;
  owner.ship_instance_id = 0;
  owner.ship_class_id = 0;
  owner.current_system_id = 0;
  owner.armor_points = 100.0F;
  owner.pos_x = 100.0F;
  owner.pos_y = 200.0F;
  owner.heading = 3.14159265358979323846F; // 180 deg, beam points toward +y

  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, 0, -1, 180));
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  const BeamHit &queued = state.beam_hit_queue[0];
  CHECK_FALSE(queued.impact_resolved);
  CHECK(queued.target_x == Catch::Approx(100.0F));
  CHECK(queued.target_y == Catch::Approx(300.0F));
  CHECK(owner.armor_points == Catch::Approx(100.0F));
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

TEST_CASE("point defense only evaluates the first ready bank",
          "[weapon][point-defense]") {
  // Original behavior (Weapon_SelectTurretTargetWithinArc 0x0043a310): the
  // selector picks the first ready mode-9/10 bank (lowest weapon id) and only
  // then searches for a target using THAT weapon's reach/blind spot. If the
  // first ready bank cannot see anything it returns before the remaining PD
  // banks are ever considered, so a short-range PD weapon gates a longer-range
  // one. The port reproduces this first-ready-then-target order.
  GameState state;
  state.scenario.weapons.resize(3);
  Weapon &short_pd = state.scenario.weapons[0];
  short_pd.weapon_mode_code = 10;
  short_pd.beam_length_px = 10;
  short_pd.ammo_type = -1;
  short_pd.mass_damage = 2;
  short_pd.energy_damage = 3;
  short_pd.lifetime_ticks = 2;
  short_pd.reload_ticks = 10;
  Weapon &long_pd = state.scenario.weapons[1];
  long_pd.weapon_mode_code = 10;
  long_pd.beam_length_px = 200;
  long_pd.ammo_type = -1;
  long_pd.mass_damage = 2;
  long_pd.energy_damage = 3;
  long_pd.lifetime_ticks = 2;
  long_pd.reload_ticks = 10;
  state.scenario.weapons[2].weapon_mode_code = 1;

  state.scenario.ships.resize(1);
  Ship &defender = state.player;
  defender.is_active = true;
  defender.ship_instance_id = 0;
  defender.ship_class_id = 0;
  defender.armor_points = 100.0F;
  state.weapon_count_by_class[0] = 1;
  state.weapon_count_by_class[100] = 1; // bank 1, kBankStride = 100

  ActiveShot incoming;
  incoming.weapon_id = 2;
  incoming.target_ship_slot = 0;
  incoming.life_ticks_remaining = 20.0F;
  incoming.pos_x = 0.0F;
  incoming.pos_y = -100.0F;
  incoming.point_defense_durability = 4;
  state.active_shots.push_back(incoming);

  // Bank 0 is ready first but out of reach: nothing fires, and bank 1 -- which
  // could reach the shot -- is never tried.
  NovaWeapon_SelectTurretTargetWithinArc(state, defender);
  CHECK(state.weapon_bank_cooldown[0] == 0.0F);
  CHECK(state.weapon_bank_cooldown[1] == 0.0F);
  CHECK(state.beam_hit_queue[0].target_shot_slot == -1);

  // Put the short-range bank on cooldown; now the long-range bank is the first
  // ready one and fires.
  state.weapon_bank_cooldown[0] = 5.0F;
  NovaWeapon_SelectTurretTargetWithinArc(state, defender);
  CHECK(state.beam_hit_queue[0].forced_targeting == 1);
  CHECK(state.beam_hit_queue[0].target_shot_slot == 0);
  CHECK(state.weapon_bank_cooldown[1] == Catch::Approx(10.0F));
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

  state.scenario.ships[0].flags3 = 0x0040;
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

// Thunderhead / Pirate Thunderhead mount the Thunderhead Lance (weapon 0xa6,
// Guidance 0, ExitType 3). sh\x8an group 3 holds the side-mounted exits:
// Thunderhead lateral +7/-7/+7/-7, Pirate lateral +9/+9/-9/-9, both with
// forward +9. Shot_QueueBeamHit (0x00427a90) stores the chosen quadrant and
// advances the ship's per-group rotation; Shot_UpdateBeamHitQueue (0x0042f270)
// re-derives the offset source every frame from the live owner. This test uses
// the shipped definitions so the exit family, scales and rotation are pinned.
TEST_CASE("Thunderhead Lance fires from alternating side exits",
          "[weapon][beam][data]") {
  if (!ArchivesAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  GameState state;
  REQUIRE(state.scenario.LoadFromArchives());

  const Weapon *lance = state.scenario.Weapon(0xa6);
  REQUIRE(lance != nullptr);
  CHECK(lance->weapon_mode_code == 0);
  CHECK(lance->turret_group_id == 3);
  CHECK(lance->beam_length_px == 100);
  const std::int16_t lance_bank = static_cast<std::int16_t>(0xa6 - 0x80);

  const ShipClass *thunder = state.scenario.Ship(0x9d);
  REQUIRE(thunder != nullptr);
  REQUIRE(thunder->muzzle_ready);
  CHECK(thunder->frames_per_rotation == 36);
  CHECK(thunder->muzzle_forward[3][0] == 9);
  CHECK(thunder->muzzle_lateral[3][0] == 7);
  CHECK(thunder->muzzle_lateral[3][1] == -7);
  CHECK(thunder->muzzle_lateral[3][2] == 7);
  CHECK(thunder->muzzle_lateral[3][3] == -7);
  CHECK(thunder->muzzle_drop[3][0] == 0);
  CHECK(thunder->muzzle_scale_near_x == Catch::Approx(1.30F));
  CHECK(thunder->muzzle_scale_near_y == Catch::Approx(1.00F));
  CHECK(thunder->muzzle_scale_far_x == Catch::Approx(1.30F));
  CHECK(thunder->muzzle_scale_far_y == Catch::Approx(1.30F));

  const ShipClass *pirate = state.scenario.Ship(0x113);
  REQUIRE(pirate != nullptr);
  REQUIRE(pirate->muzzle_ready);
  CHECK(pirate->muzzle_forward[3][0] == 9);
  CHECK(pirate->muzzle_lateral[3][0] == 9);
  CHECK(pirate->muzzle_lateral[3][1] == 9);
  CHECK(pirate->muzzle_lateral[3][2] == -9);
  CHECK(pirate->muzzle_lateral[3][3] == -9);
  CHECK(pirate->muzzle_scale_near_x == Catch::Approx(1.00F));
  CHECK(pirate->muzzle_scale_near_y == Catch::Approx(0.70F));

  Ship &owner = state.ShipAt(0);
  owner.is_active = true;
  owner.ship_instance_id = 0;
  owner.current_system_id = state.player.current_system_id;
  owner.armor_points = 100.0F;
  owner.pos_x = 1000.0F;
  owner.pos_y = 2000.0F;
  owner.heading = 0.0F; // rotation frame 0 -> turret bearing 0

  // First quadrant is a seeded RandomBelow(4) roll; the following three are a
  // deterministic +1 mod 4 cycle of the ship's per-group state.
  owner.ship_class_id = 0x9d - 0x80;
  owner.muzzle_quadrant = {-1, -1, -1, -1};
  state.beam_hit_queue.fill(BeamHit{});
  state.rng.seed(0x5eed);
  std::mt19937 probe(0x5eed);
  const int expected_first =
      std::uniform_int_distribution<std::int32_t>{0, 3}(probe);
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, lance_bank, -1, 0));
  const BeamHit &first = state.beam_hit_queue[0];
  CHECK(first.turret_group_id == 3);
  CHECK(first.turret_quadrant == expected_first);
  const float thunder_lateral[4] = {7.0F, -7.0F, 7.0F, -7.0F};
  CHECK(first.source_x ==
        Catch::Approx(1000.0F + thunder_lateral[expected_first] * 1.30F));
  CHECK(first.source_y == Catch::Approx(2000.0F - 9.0F));
  // The queued endpoint is laid downrange from the muzzle, not the centre.
  CHECK(first.target_x == Catch::Approx(first.source_x));
  CHECK(first.target_y == Catch::Approx(first.source_y - 100.0F));
  CHECK(first.target_x != Catch::Approx(1000.0F));
  for (int i = 1; i < 4; ++i) {
    REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, lance_bank, -1, 0));
    const BeamHit &beam = state.beam_hit_queue[static_cast<std::size_t>(i)];
    CHECK(beam.turret_quadrant == ((expected_first + i) & 3));
    CHECK(
        beam.source_x ==
        Catch::Approx(1000.0F + thunder_lateral[beam.turret_quadrant] * 1.30F));
  }

  // Pirate Thunderhead: +9/+9/-9/-9 lateral, near scales 1.00/0.70.
  state.beam_hit_queue.fill(BeamHit{});
  owner.ship_class_id = 0x113 - 0x80;
  owner.muzzle_quadrant = {0, 0, 0, 0}; // group 3 starts at quadrant 0
  const float pirate_lateral[4] = {9.0F, 9.0F, -9.0F, -9.0F};
  for (int i = 0; i < 4; ++i) {
    REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, lance_bank, -1, 0));
    const BeamHit &beam = state.beam_hit_queue[static_cast<std::size_t>(i)];
    CHECK(beam.turret_quadrant == i);
    CHECK(beam.source_x == Catch::Approx(1000.0F + pirate_lateral[i] * 1.00F));
    CHECK(beam.source_y == Catch::Approx(2000.0F - 9.0F * 0.70F));
  }

  // A full beam queue must fail without consuming the ship's rotation state or
  // an RNG roll.
  state.beam_hit_queue.fill(BeamHit{});
  for (BeamHit &slot : state.beam_hit_queue) {
    slot.lifetime_ticks = 5;
  }
  owner.ship_class_id = 0x9d - 0x80;
  owner.muzzle_quadrant = {0, 0, 0, 2}; // group 3 = 2
  state.rng.seed(0x1111);
  std::mt19937 full_probe(0x1111);
  const auto expected_failed_draw =
      std::uniform_int_distribution<std::int32_t>{0, 3}(full_probe);
  CHECK_FALSE(NovaWeapon_QueueBeamHit(state, 0, -1, lance_bank, -1, 0));
  CHECK(owner.muzzle_quadrant[3] == 2);
  const auto first_after_failure =
      std::uniform_int_distribution<std::int32_t>{0, 3}(state.rng);
  CHECK(first_after_failure == expected_failed_draw);

  // A queued beam keeps its chosen quadrant after the ship's per-group state
  // advances independently; the live tick still uses the stored quadrant.
  state.beam_hit_queue.fill(BeamHit{});
  owner.ship_class_id = 0x9d - 0x80;
  owner.pos_x = 1000.0F;
  owner.pos_y = 2000.0F;
  owner.heading = 0.0F;
  owner.muzzle_quadrant = {0, 0, 0, 0};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, lance_bank, -1, 0));
  BeamHit &persist = state.beam_hit_queue[0];
  REQUIRE(persist.turret_quadrant == 0);
  CHECK(owner.muzzle_quadrant[3] == 1); // queue advanced the ship state
  owner.muzzle_quadrant[3] = 2;         // owner cycles on independently
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(persist.turret_quadrant == 0);
  CHECK(persist.source_x == Catch::Approx(1000.0F + 7.0F * 1.30F));

  // Moving + rotating owner: the live tick re-derives the same exit offset at
  // heading 90 deg (frame 9), which selects the FAR scales.
  state.beam_hit_queue.fill(BeamHit{});
  owner.ship_class_id = 0x9d - 0x80;
  owner.muzzle_quadrant = {0, 0, 0, 0};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, lance_bank, -1, 0));
  BeamHit &moving = state.beam_hit_queue[0];
  REQUIRE(moving.turret_quadrant == 0);
  owner.pos_x = 1500.0F;
  owner.pos_y = 2500.0F;
  owner.heading = 1.5707963267948966F; // pi/2
  NovaWeapon_TickBeamHitQueue(state, 1.0F);
  CHECK(moving.source_x == Catch::Approx(1500.0F + 9.0F * 1.30F));
  CHECK(moving.source_y == Catch::Approx(2500.0F + 7.0F * 1.30F));
  // Mode-0 endpoint follows the live heading from the offset source.
  CHECK(moving.target_x == Catch::Approx(moving.source_x + 100.0F));
  CHECK(moving.target_y == Catch::Approx(moving.source_y));

  // Guards for a weapon with no turret group: no quadrant, owner centre,
  // matching the visible result of the original's out-of-range access.
  state.beam_hit_queue.fill(BeamHit{});
  state.scenario.weapons.resize(1);
  state.scenario.weapons[0].weapon_mode_code = 0;
  state.scenario.weapons[0].beam_length_px = 100;
  state.scenario.weapons[0].lifetime_ticks = 10;
  state.scenario.weapons[0].turret_group_id = -1;
  owner.ship_class_id = 0x9d - 0x80;
  owner.heading = 0.0F;
  owner.pos_x = 1000.0F;
  owner.pos_y = 2000.0F;
  owner.muzzle_quadrant = {-1, -1, -1, -1};
  REQUIRE(NovaWeapon_QueueBeamHit(state, 0, -1, 0, -1, 0));
  const BeamHit &centre = state.beam_hit_queue[0];
  CHECK(centre.turret_quadrant == -1);
  CHECK(centre.source_x == Catch::Approx(1000.0F));
  CHECK(centre.source_y == Catch::Approx(2000.0F));
}

// Ship_HandleShip 0x00433050 per-bank cooldown tail.
TEST_CASE("NPC weapon bank cooldowns decay, reload targetless bays, and pin "
          "ionized disruptors",
          "[weapon][npc]") {
  GameState state;
  state.scenario.weapons.resize(2);
  Weapon &reload_bay = state.scenario.weapons[0];
  reload_bay.weapon_mode_code = 99;
  reload_bay.reload_ticks = 30.0F;
  Weapon &disruptor = state.scenario.weapons[1];
  disruptor.flags_quaternary = 0x0020;

  Ship &ship = state.ShipAt(1);
  ship.ship_class_id = 0;
  ship.primary_target_ship_slot = -1;
  ship.npc_weapon_count_by_class[0] = 1;
  ship.npc_weapon_count_by_class[1] = 1;
  ship.npc_weapon_bank_cooldown[0] = 0.0F;
  ship.npc_weapon_bank_cooldown[1] = 5.0F;

  NovaWeapon_TickNpcWeaponBanks(state, ship, 1.0F);
  // A targetless mode-99 bay reloads to Reload; the disruptor decays by one.
  CHECK(ship.npc_weapon_bank_cooldown[0] == Catch::Approx(30.0F));
  CHECK(ship.npc_weapon_bank_cooldown[1] == Catch::Approx(4.0F));

  // With a primary target the mode-99 reload arm is skipped.
  ship.primary_target_ship_slot = 2;
  ship.npc_weapon_bank_cooldown[0] = 0.0F;
  ship.npc_weapon_bank_cooldown[1] = 5.0F;
  NovaWeapon_TickNpcWeaponBanks(state, ship, 1.0F);
  CHECK(ship.npc_weapon_bank_cooldown[0] == Catch::Approx(0.0F));

  // Ionized pin: full ionization charge + flags_quaternary 0x20 -> 1.0.
  state.scenario.ships.resize(1);
  state.scenario.ships[0].ionization_capacity = 100;
  ship.ionization_points = 100.0F;
  ship.npc_weapon_bank_cooldown[1] = 7.0F;
  NovaWeapon_TickNpcWeaponBanks(state, ship, 1.0F);
  CHECK(ship.npc_weapon_bank_cooldown[1] == Catch::Approx(1.0F));

  // Ammo gate: a bank with no ammo does not decay.
  ship.npc_weapon_count_by_class[0] = 0;
  ship.npc_weapon_bank_cooldown[0] = 5.0F;
  NovaWeapon_TickNpcWeaponBanks(state, ship, 1.0F);
  CHECK(ship.npc_weapon_bank_cooldown[0] == Catch::Approx(5.0F));
}

} // namespace game
