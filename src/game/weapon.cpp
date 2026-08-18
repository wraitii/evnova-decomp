#include "weapon.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "collision.hpp"
#include "game_state.hpp"
#include "outfit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

namespace game {
namespace {

// The clean-room weapon banks are kept in the GameState strided arrays with the
// original's 100-element stride (see game_state.hpp). These helpers read/write
// the ammo / secondary counters for one zero-based weapon bank.
constexpr std::size_t kBankStride = 100;

std::int16_t &BankAmmo(GameState &state, std::int16_t bank) {
  return state.weapon_bank_ammo[static_cast<std::size_t>(bank) * kBankStride];
}

const std::int16_t &BankAmmo(const GameState &state, std::int16_t bank) {
  return state.weapon_bank_ammo[static_cast<std::size_t>(bank) * kBankStride];
}

std::int16_t &BankSecondary(GameState &state, std::int16_t bank) {
  return state
      .weapon_bank_secondary[static_cast<std::size_t>(bank) * kBankStride];
}

const std::int16_t &BankSecondary(const GameState &state, std::int16_t bank) {
  return state
      .weapon_bank_secondary[static_cast<std::size_t>(bank) * kBankStride];
}

// The weapon loaded in a bank, or nullptr when the bank holds nothing valid.
const Weapon *WeaponAt(const GameState &state, std::int16_t bank) {
  // Banks are indexed by zero-based weapon id; the scenario Weapon() lookup
  // uses the 0x80.. residue. Bank slot b == weapon resource id -- but our
  // clean Weapon table is indexed by (weapon id - 0x80). Resolve via the
  // resource-id offset: bank slot `b` corresponds to weapon resource `b+0x80`.
  return state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
}

} // namespace

void NovaWeapon_SeedBanksFromShipStock(GameState &state,
                                       std::int16_t ship_class_id) {
  // Seeds the player's 0x100 weapon-bank ammo/secondary counters from a ship
  // class's mounted stock weapons (Ghidra default_weapon_ammo / default_weapon_
  // secondary). For each stock weapon triple {weapon_id, count, ammo_load} the
  // mounted-count goes into weapon_bank_ammo[weapon_id-0x80] and any carried
  // rounds (ammo_load, when > 0) into the matching secondary/ammo counter. The
  // original seeds the new ship's banks this way in Menu_RunNewGameFlow and
  // Outfit_SwapPlayerShipWithEscort, then calls
  // Weapon_ReconcileOutfitPoolWithWeaponBanks to register the mounted stock
  // guns as owned outfits.
  const ShipClass *ship =
      state.scenario.Ship(static_cast<std::int16_t>(ship_class_id + 0x80));
  if (ship == nullptr) {
    return;
  }
  for (const ShipDefaultWeaponBank &stock : ship->stock_weapons) {
    if (stock.weapon_id < 0x80 || stock.weapon_id > 0x17f) {
      continue; // unmounted bank (weapon_id -1) or out-of-range
    }
    const std::size_t bank = static_cast<std::size_t>(stock.weapon_id - 0x80);
    state.weapon_bank_ammo[bank * kBankStride] =
        static_cast<std::int16_t>(stock.count > 0 ? stock.count : 0);
    if (stock.ammo_load > 0) {
      state.weapon_bank_secondary[bank * kBankStride] =
          static_cast<std::int16_t>(stock.ammo_load);
    }
  }
}

void NovaWeapon_RebuildBanksFromOwnedOutfits(GameState &state) {
  // Zero-sweep all 0x100 banks (the original clears both counters).
  for (std::size_t b = 0; b < 0x100; ++b) {
    state.weapon_bank_ammo[b * kBankStride] = 0;
    state.weapon_bank_secondary[b * kBankStride] = 0;
  }
  // Accumulate from owned outfits. outfit_owned_count is indexed by outfit id
  // - 0x80; scenario.outfits is indexed the same way, so index i maps to both.
  const auto &outfits = state.scenario.outfits;
  for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size(); ++i) {
    const std::int16_t owned = state.inventory.outfit_owned_count[i];
    if (owned <= 0 || i >= outfits.size()) {
      continue;
    }
    const Outfit &o = outfits[i];
    // ModType 1 (kWeapon): mod_val is the zero-based weapon bank slot
    // (resource id minus 0x80) the owned count loads into; owned count > 0
    // gates firing.
    if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kWeapon)) {
      if (o.mod_val >= 0 && o.mod_val < 0x100) {
        BankAmmo(state, o.mod_val) =
            static_cast<std::int16_t>(BankAmmo(state, o.mod_val) + owned);
      }
    }
    // ModType 3 (kAmmo): mod_val is the zero-based weapon bank slot whose
    // carried-round (secondary) counter this ammo outfit backs.
    if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kAmmo)) {
      if (o.mod_val >= 0 && o.mod_val < 0x100) {
        BankSecondary(state, o.mod_val) =
            static_cast<std::int16_t>(BankSecondary(state, o.mod_val) + owned);
      }
    }
  }
}

void NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(GameState &state) {
  // Ghidra Weapon_ReconcileOutfitPoolWithWeaponBanks (0x00462ec0) reconciles
  // the outfit-pool counts with the live weapon-bank counters in both
  // directions:
  //   1. snapshot each bank's ammo/secondary counter;
  //   2. for every owned outfit, subtract the count it already explains from
  //      the matching snapshot (weapon outfits from ammo, ammo outfits from
  //      secondary), clamping the owned count down if more is owned than the
  //      bank carries;
  //   3. clamp any negative owned count to zero;
  //   4. convert leftover positive bank balance back into owned outfits: for
  //      each bank with ammo > 0 still unaccounted, add that amount to the
  //      first owned-eligible weapon outfit (ModType 1) whose mod_val is the
  //      bank slot; likewise leftover secondary -> an ammo outfit (ModType 3).
  // Menu_RunNewGameFlow calls this right after seeding weapon banks from the
  // starting ship class's stock weapons, so a mounted weapon that has no
  // DefaultItem entry (e.g. the Shuttle's Light Blaster) is registered as an
  // owned outfit and becomes sellable in the Outfitter.
  std::array<std::int16_t, 0x100> bank_ammo{};
  std::array<std::int16_t, 0x100> bank_secondary{};
  for (std::int16_t b = 0; b < 0x100; ++b) {
    bank_ammo[b] = BankAmmo(state, b);
    bank_secondary[b] = BankSecondary(state, b);
  }

  const auto &outfits = state.scenario.outfits;
  for (std::size_t i = 0;
       i < state.inventory.outfit_owned_count.size() && i < outfits.size();
       ++i) {
    const std::int16_t owned = state.inventory.outfit_owned_count[i];
    if (owned <= 0) {
      continue;
    }
    const Outfit &o = outfits[i];
    const auto consume = [](std::int16_t &balance, std::int16_t &owned_count) {
      if (owned_count < balance) {
        balance = static_cast<std::int16_t>(balance - owned_count);
      } else {
        owned_count = balance;
        balance = 0;
      }
    };
    if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kWeapon) &&
        o.mod_val >= 0 && o.mod_val < 0x100) {
      consume(bank_ammo[o.mod_val], state.inventory.outfit_owned_count[i]);
    } else if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kAmmo) &&
               o.mod_val >= 0 && o.mod_val < 0x100) {
      consume(bank_secondary[o.mod_val], state.inventory.outfit_owned_count[i]);
    }
  }
  for (std::int16_t &count : state.inventory.outfit_owned_count) {
    if (count < 0) {
      count = 0;
    }
  }

  // Leftover positive bank ammo that no owned outfit explains becomes an owned
  // weapon outfit; leftover secondary becomes an owned ammo outfit.
  bool materialized = false;
  for (std::int16_t b = 0; b < 0x100; ++b) {
    if (bank_ammo[b] > 0) {
      for (std::size_t i = 0;
           i < state.inventory.outfit_owned_count.size() && i < outfits.size();
           ++i) {
        const Outfit &o = outfits[i];
        if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kWeapon) &&
            o.mod_val == b) {
          state.inventory.outfit_owned_count[i] = static_cast<std::int16_t>(
              state.inventory.outfit_owned_count[i] + bank_ammo[b]);
          bank_ammo[b] = 0;
          materialized = true;
          break;
        }
      }
    }
    if (bank_secondary[b] > 0) {
      for (std::size_t i = 0;
           i < state.inventory.outfit_owned_count.size() && i < outfits.size();
           ++i) {
        const Outfit &o = outfits[i];
        if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kAmmo) &&
            o.mod_val == b) {
          state.inventory.outfit_owned_count[i] = static_cast<std::int16_t>(
              state.inventory.outfit_owned_count[i] + bank_secondary[b]);
          bank_secondary[b] = 0;
          materialized = true;
          break;
        }
      }
    }
  }

  if (materialized) {
    OutfitMarkStatsDirty(state);
  }
}

bool NovaWeapon_CanFireBank(const GameState &state, std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return false;
  }
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (!w) {
    return false;
  }
  // Carrier-bay weapons (mode 99) need a loaded ship in the secondary counter.
  if (w->weapon_mode_code == 99) {
    return BankSecondary(state, weapon_bank) >= 1;
  }
  // Ammo/energy requirement. ammo_type (Ghidra ammo_or_energy_cost_code) <
  // -999 means the weapon draws energy from fuel (not implemented; the Light
  // Blaster and most gun weapons are free). In [0,255] it needs carried
  // ammunition; otherwise (-1 = unlimited) no ammo is required.
  const int cost = w->ammo_type;
  if (cost >= 0 && cost <= 0xff) {
    if (BankSecondary(state, static_cast<std::int16_t>(cost)) < 1) {
      return false;
    }
  }
  return true;
}

int NovaWeapon_SpawnProjectile(GameState &state,
                               std::int16_t owner_ship_slot,
                               std::int16_t target_ship_slot,
                               std::int16_t weapon_id,
                               bool spawn_without_owner,
                               bool apply_random_spread) {
  if (weapon_id < 0 || weapon_id >= 0x100) {
    return -1;
  }
  const Weapon *w = WeaponAt(state, weapon_id);
  if (w == nullptr) {
    return -1;
  }

  const bool owner_in_range =
      owner_ship_slot >= 0 &&
      owner_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips);
  if (!spawn_without_owner && !owner_in_range) {
    return -1;
  }

  ActiveShot shot;
  shot.weapon_id = weapon_id;
  shot.owner_ship_slot = spawn_without_owner ? -1 : owner_ship_slot;
  shot.target_ship_slot = target_ship_slot;
  shot.system_id = owner_in_range
                       ? state.ShipAt(static_cast<std::size_t>(owner_ship_slot))
                             .current_system_id
                       : state.player.current_system_id;

  float heading = 0.0F;
  if (owner_in_range) {
    const Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    shot.pos_x = owner.pos_x;
    shot.pos_y = owner.pos_y;
    heading = owner.heading;

    // NPC Shot_SpawnShotFromWeapon aims at its selected target. The player
    // path retains current-heading behavior until its aim branches are ported.
    if (owner_ship_slot != 0 && target_ship_slot >= 0 &&
        target_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips)) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(target_ship_slot));
      if (target.is_active &&
          target.current_system_id == owner.current_system_id) {
        heading = std::atan2(target.pos_x - owner.pos_x,
                             -(target.pos_y - owner.pos_y));
      }
    }

    // The player sh\x8an descriptor supplies the four-barrel muzzle geometry.
    // NPC muzzle descriptors are not represented yet, so their projectiles
    // fall back to the hull origin until that data is decoded.
    const int turret_group = static_cast<int>(w->turret_group_id);
    if (owner_ship_slot == 0 && owner.muzzle_ready && turret_group >= 0 &&
        turret_group < 4) {
      auto &quadrant = state.player.muzzle_quadrant[turret_group];
      if (quadrant < 0 || quadrant > 3) {
        quadrant = static_cast<std::int8_t>(
            std::uniform_int_distribution<int>{0, 3}(state.rng));
      }
      const int q = quadrant;
      const float forward =
          static_cast<float>(owner.muzzle_forward[turret_group][q]);
      const float lateral =
          static_cast<float>(owner.muzzle_lateral[turret_group][q]);
      const float drop = static_cast<float>(owner.muzzle_drop[turret_group][q]);
      const float sin_heading = std::sin(heading);
      const float cos_heading = std::cos(heading);
      shot.pos_x += (sin_heading * forward + cos_heading * lateral) *
                    owner.muzzle_scale_x;
      shot.pos_y += (-cos_heading * forward + sin_heading * lateral) *
                        owner.muzzle_scale_y -
                    drop;
      quadrant = static_cast<std::int8_t>((q + 1) & 3);
    }
  }

  // The original jitters the firing bearing by the weapon's spread field.
  // Full turret/lead rules remain deferred, but the signed angular spread is
  // preserved for the basic straight-flight path.
  if (apply_random_spread && w->inaccuracy > 0 && w->weapon_mode_code != 5) {
    const int spread = std::uniform_int_distribution<int>{
        -static_cast<int>(w->inaccuracy),
        static_cast<int>(w->inaccuracy)}(state.rng);
    heading += static_cast<float>(spread) * (3.14159265358979323846F / 180.0F);
  }

  const float speed = static_cast<float>(w->projectile_speed) / 100.0F;
  if (owner_in_range) {
    const Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    shot.vel_x = std::sin(heading) * speed + owner.vel_x;
    shot.vel_y = -std::cos(heading) * speed + owner.vel_y;
  } else {
    shot.vel_x = std::sin(heading) * speed;
    shot.vel_y = -std::cos(heading) * speed;
  }

  shot.life_ticks_remaining =
      std::max(1.0F, static_cast<float>(w->lifetime_ticks));
  shot.life_frames = static_cast<int>(std::ceil(shot.life_ticks_remaining));
  shot.collision_radius_px = 2.0F;
  // Weapon_GetShotImpactVariant (0x0046c2f0): Flags2 bit 0x1000 makes a
  // weapon disable but not destroy. Shot_ResolveShipHitFromWeapon preserves
  // one armor point for that variant.
  shot.impact_variant =
      (w->flags_secondary & 0x1000U) != 0U ? static_cast<std::int8_t>(1) : 0;
  state.active_shots.push_back(shot);
  return static_cast<int>(state.active_shots.size() - 1);
}

void NovaWeapon_FirePlayerWeaponBank(GameState &state,
                                     std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return;
  }
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (!w || state.weapon_bank_cooldown[weapon_bank] > 0.0F) {
    return; // unmounted bank, or still cooling down this frame
  }
  if (!NovaWeapon_CanFireBank(state, weapon_bank)) {
    return;
  }

  // Spawn the shot for the unguided-projectile weapon modes (guidance -1/1/5/6
  // in Ghidra Weapon_FirePlayerWeaponBank -> Shot_SpawnShotFromWeapon). Beams
  // (mode 0), turrets (3/4), guided (7/8) and launchers (99) are not yet
  // reconstructed; this build only reproduces the direct-fire projectile.
  if ((w->weapon_mode_code != -1) && (w->weapon_mode_code != 1) &&
      !((w->weapon_mode_code - 5U) < 2U)) {
    return;
  }

  // Weapon_FirePlayerWeaponBank passes the selected primary target through to
  // Shot_SpawnShotFromWeapon, including for the straight projectile modes.
  // Keeping that context on the shared record is important for mode-1
  // target-only collision eligibility.
  const int shot_slot =
      NovaWeapon_SpawnProjectile(state,
                                 0,
                                 state.player.primary_target_ship_slot,
                                 weapon_bank,
                                 /*spawn_without_owner=*/false,
                                 /*apply_random_spread=*/false);
  if (shot_slot < 0) {
    return;
  }
  // A round actually spawned: mirror Weapon_FirePlayerWeaponBank's
  // `volley_fired > 0` gate and queue this weapon's fire sound (slot, not
  // resource id) for the spaceflight loop to play through the cached sound.
  // The player is both source and listener, so the spatial attenuation in
  // NovaAudio_PlaySpatialByDistance evaluates to full volume (the original
  // passes &g_ship_states->pos_x for both).
  if (w->fire_sound >= 0) {
    state.pending_fire_sounds.push_back({w->fire_sound,
                                         state.player.pos_x,
                                         state.player.pos_y,
                                         (w->flags & 0x0010U) != 0U});
  }

  // Set the bank cooldown to the fire interval. The original divides the
  // weapon's fire cadence by the number of weapons mounted in this bank
  // (Weapon_FirePlayerWeaponBank sets `weapon_bank_cooldown = sVar18 *
  // speed_scalar / weapon_bank_ammo`, with sVar18 = shots actually fired, 1 for
  // the light blaster's non-burst path). weapon_bank_ammo holds the mount
  // count, so buying/installing a second identical weapon doubles the fire
  // rate rather than being a no-op. The bank cannot fire again until this
  // elapses (NovaWeapon_TickShots counts it down); reload_ticks is the
  // reference-cadence speed_scalar in frames.
  const int mount_count =
      std::max(1, static_cast<int>(BankAmmo(state, weapon_bank)));
  state.weapon_bank_cooldown[weapon_bank] =
      static_cast<float>(std::max(1, static_cast<int>(w->reload_ticks))) /
      static_cast<float>(mount_count);
}

void NovaWeapon_FirePlayerPrimary(GameState &state) {
  // Ghidra Ship_HandlePlayerShipControl's primary-fire loop fires every bank
  // that has ammo (weapon_bank_ammo[b] > 0) and is NOT a secondary weapon
  // (Weapon.flags bit 0x2 unset). The player's main Light Blaster bank is such
  // a bank.
  for (std::int16_t b = 0; b < 0x100; ++b) {
    if (BankAmmo(state, b) <= 0) {
      continue;
    }
    const Weapon *w = WeaponAt(state, b);
    if (!w || (w->flags & 0x0002U) != 0) {
      continue; // unmounted or a secondary weapon
    }
    NovaWeapon_FirePlayerWeaponBank(state, b);
  }
}

void NovaWeapon_TickNpcWeaponBanks(Ship &ship, float elapsed_ticks) {
  const float ticks = std::max(0.0F, elapsed_ticks);
  for (float &cooldown : ship.npc_weapon_bank_cooldown) {
    cooldown = std::max(0.0F, cooldown - ticks);
  }
}

bool NovaWeapon_QueueBeamHit(GameState &state,
                             std::int16_t owner_ship_slot,
                             std::int16_t target_ship_slot,
                             std::int16_t weapon_id,
                             std::int16_t forced_targeting) {
  if (owner_ship_slot < 0 ||
      owner_ship_slot >= static_cast<std::int16_t>(GameState::kMaxShips) ||
      weapon_id < 0 || weapon_id >= 0x100) {
    return false;
  }
  const Weapon *weapon = WeaponAt(state, weapon_id);
  if (weapon == nullptr) {
    return false;
  }
  for (BeamHit &beam : state.beam_hit_queue) {
    if (beam.lifetime_ticks >= -1) {
      continue;
    }
    const Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
    beam.source_x = owner.pos_x;
    beam.source_y = owner.pos_y;
    beam.target_x = owner.pos_x;
    beam.target_y = owner.pos_y;
    if (target_ship_slot >= 0 &&
        target_ship_slot < static_cast<std::int16_t>(GameState::kMaxShips)) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(target_ship_slot));
      beam.target_x = target.pos_x;
      beam.target_y = target.pos_y;
    }
    beam.lifetime_ticks = std::max<std::int16_t>(1, weapon->lifetime_ticks);
    beam.animation_counter = 0;
    beam.weapon_id = weapon_id;
    beam.owner_ship_slot = owner_ship_slot;
    beam.target_ship_slot = target_ship_slot;
    beam.forced_targeting = forced_targeting;
    beam.turret_quadrant = -1;
    beam.turret_group_id = weapon->turret_group_id;
    beam.impact_variant = (weapon->flags_secondary & 0x1000U) != 0U ? 1 : 0;
    beam.impact_resolved = false;
    return true;
  }
  return false;
}

void NovaWeapon_TickBeamHitQueue(GameState &state, float elapsed_ticks) {
  const float ticks = std::max(0.0F, elapsed_ticks);
  for (BeamHit &beam : state.beam_hit_queue) {
    if (beam.lifetime_ticks < -1) {
      continue;
    }
    if (beam.target_ship_slot >= 0 &&
        beam.target_ship_slot <
            static_cast<std::int16_t>(GameState::kMaxShips)) {
      const Ship &target =
          state.ShipAt(static_cast<std::size_t>(beam.target_ship_slot));
      beam.target_x = target.pos_x;
      beam.target_y = target.pos_y;
      if (!beam.impact_resolved && target.is_active) {
        NovaWeapon_ResolveDirectWeaponHit(state,
                                          beam.owner_ship_slot,
                                          beam.target_ship_slot,
                                          beam.weapon_id,
                                          beam.impact_variant);
        beam.impact_resolved = true;
      }
    }
    beam.animation_counter = static_cast<std::int16_t>(
        beam.animation_counter + static_cast<std::int16_t>(ticks));
    beam.lifetime_ticks = static_cast<std::int16_t>(
        beam.lifetime_ticks - static_cast<std::int16_t>(ticks));
    if (beam.lifetime_ticks < 0) {
      beam = BeamHit{};
    }
  }
}

void NovaWeapon_FireNpcWeaponBank(GameState &state, Ship &ship) {
  const std::int16_t bank = ship.active_weapon_bank_slot;
  if (ship.ship_instance_id == 0 || bank < 0 || bank >= 0x100 ||
      ship.ai_fire_trigger_latch == 0) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(bank);
  const Weapon *weapon =
      state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
  if (weapon == nullptr || ship.npc_weapon_bank_ammo[index] <= 0 ||
      ship.npc_weapon_bank_cooldown[index] > 0.0F) {
    return;
  }
  const std::int16_t secondary = ship.npc_weapon_bank_secondary[index];
  // Stock energy weapons use -1 as the unlimited-secondary sentinel; zero is
  // the only empty value here.
  if (secondary == 0 || weapon->weapon_mode_code == 99) {
    return;
  }
  const std::int16_t mode = weapon->weapon_mode_code;
  if (mode == 0) {
    if (!NovaWeapon_QueueBeamHit(state,
                                 ship.ship_instance_id,
                                 ship.primary_target_ship_slot,
                                 bank)) {
      return;
    }
  } else if (mode != -1 && mode != 1 && mode != 4 && mode != 6 && mode != 7 &&
             mode != 8) {
    return;
  } else {
    const int shot_slot =
        NovaWeapon_SpawnProjectile(state,
                                   ship.ship_instance_id,
                                   ship.primary_target_ship_slot,
                                   bank,
                                   false,
                                   true);
    if (shot_slot < 0) {
      return;
    }
  }
  if (secondary > 0 && ship.mission_ship_slot != 0x3ff &&
      (weapon->flags_tertiary & 0x0001U) == 0U) {
    ship.npc_weapon_bank_secondary[index] =
        static_cast<std::int16_t>(secondary - 1);
  }
  // A volley fired (sVar9 >= 1 in Weapon_FireShipWeapons): queue the fire
  // sound, sourced at this ship, for the spaceflight loop. The original plays
  // it via NovaAudio_PlaySpatialByDistance with the player ship as listener,
  // so NPC fire fades with distance; flags_primary bit 0x10 marks sounds that
  // must not stack (NovaAudio_CountActiveByHandle gate).
  if (weapon->fire_sound >= 0) {
    state.pending_fire_sounds.push_back({weapon->fire_sound,
                                         ship.pos_x,
                                         ship.pos_y,
                                         (weapon->flags & 0x0010U) != 0U});
  }
  const int mount_count =
      std::max(1, static_cast<int>(ship.npc_weapon_bank_ammo[index]));
  ship.npc_weapon_bank_cooldown[index] =
      static_cast<float>(std::max(1, static_cast<int>(weapon->reload_ticks))) /
      static_cast<float>(mount_count);
  ship.ai_fire_trigger_latch = 0;
  ship.active_weapon_bank_slot = -1;
}

// Ghidra Shot_HandleShot (0x00435830) time-animated shot-frame branch: for a
// weapon with flags_primary bit 0 set, each frame accumulates the real frame
// time into ShotState.anim_elapsed and, when it crosses the weapon's
// shot_anim_frame_dwell (Ghidra WeaponDef.homing_strength_or_turn_rate; a
// dwell < 1 advances every frame), steps ShotState.frame_cycle_index, wrapping
// at the shot sprite-set's frame count (0, or frame_count-1 when the weapon's
// flags_secondary bit 1 is set). The Light Blaster and the other unguided
// projectiles take the static/heading branch instead, so this only drives
// genuinely time-animated weapon shots.
void NovaWeapon_StepShotAnimation(GameState &state,
                                  ActiveShot &shot,
                                  float frame_time_ms) {
  const Weapon *w = WeaponAt(state, shot.weapon_id);
  if (!w) {
    return;
  }
  // flags_primary bit 0 clear -> static/heading shot-frame path (no animation).
  if ((w->flags & 0x0001U) == 0) {
    return;
  }
  const std::int16_t dwell = w->shot_anim_frame_dwell;
  shot.anim_elapsed += frame_time_ms;
  if (dwell < 1 || shot.anim_elapsed >= static_cast<float>(dwell)) {
    shot.frame_cycle_index += 1;
    shot.anim_elapsed = 0.0F;
  }
  // The caller (DrawShots) clamps the displayed frame to the sprite set's frame
  // count, mirroring Shot_HandleShot's wrap. The reverse-wrap (flags_secondary
  // bit 1 -> frame_count - 1) needs the frame count, which lives on the SDL
  // side; DrawShots owns that clamp once the set is resolved.
}

void NovaWeapon_TickShots(GameState &state,
                          float frame_time_ms,
                          float elapsed_ticks) {
  // Advance shots by velocity and lifetime; drop expired rounds. Also step the
  // time-animated shot-frame cycle (Shot_HandleShot animated branch) for
  // weapons that use it; static/heading shot sets are untouched.
  auto &shots = state.active_shots;
  const float tick_scale = std::max(0.0F, elapsed_ticks);
  for (auto &shot : shots) {
    if (shot.consumed) {
      continue;
    }
    shot.pos_x += shot.vel_x * tick_scale;
    shot.pos_y += shot.vel_y * tick_scale;
    if (shot.life_ticks_remaining <= 0.0F) {
      shot.life_ticks_remaining = static_cast<float>(shot.life_frames);
    }
    shot.life_ticks_remaining -= tick_scale;
    shot.life_frames =
        static_cast<int>(std::ceil(std::max(0.0F, shot.life_ticks_remaining)));
    NovaWeapon_StepShotAnimation(state, shot, frame_time_ms);
  }
  shots.erase(std::remove_if(shots.begin(),
                             shots.end(),
                             [](const ActiveShot &s) {
                               return s.consumed ||
                                      s.life_ticks_remaining <= 0.0F;
                             }),
              shots.end());
  // Count every weapon-bank cooldown down toward zero.
  for (float &cd : state.weapon_bank_cooldown) {
    if (cd > 0.0F) {
      cd = std::max(0.0F, cd - tick_scale);
    }
  }
}

std::string NovaWeapon_BankDisplayName(const GameState &state,
                                       std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return "?";
  }
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (!w || BankAmmo(state, weapon_bank) <= 0) {
    return "?";
  }
  return w->name.empty() ? "?" : w->name;
}

std::int16_t NovaWeapon_BankAmmoCount(const GameState &state,
                                      std::int16_t weapon_bank) {
  if (weapon_bank < 0 || weapon_bank >= 0x100) {
    return -1;
  }
  const Weapon *w = WeaponAt(state, weapon_bank);
  if (!w) {
    return -1;
  }
  // Energy/unlimited weapons show no count (NovaUi_DrawActiveWeaponAmmoPanel's
  // early cases).
  if (w->ammo_type == -1 || (w->flags_secondary & 0x40U) != 0U) {
    return -1;
  }
  // A special (mode 99) or out-of-range ammo_type weapon reads its own bank's
  // secondary counter; a normal weapon reads the ammo counter of the weapon
  // bank whose id equals its ammo_type.
  const std::int16_t source_bank =
      (w->ammo_type < 0 || w->ammo_type > 0xff || w->weapon_mode_code == 99)
          ? weapon_bank
          : w->ammo_type;
  return std::min<std::int16_t>(BankSecondary(state, source_bank), 9999);
}

void NovaWeapon_PreloadFireSound(GameState &state,
                                 std::int16_t fire_sound_slot) {
  if (fire_sound_slot < 0 || fire_sound_slot >= 36) {
    return; // no fire sound for this slot
  }
  if (state.weapon_fire_sounds[fire_sound_slot].has_value()) {
    return; // already cached
  }
  // Slot -> snd resource id 200 + slot (see NovaWeapon_FireSoundResourceId).
  const auto resource = NovaResource_LoadSndData(static_cast<std::uint16_t>(
      NovaWeapon_FireSoundResourceId(fire_sound_slot)));
  if (!resource) {
    return; // resource missing; weapon fires silently
  }
  if (auto decoded = NovaSound_Decode(*resource)) {
    state.weapon_fire_sounds[fire_sound_slot] = std::move(*decoded);
    const auto resource_id = NovaWeapon_FireSoundResourceId(fire_sound_slot);
    if (resource_id >= GameState::kGameplaySoundFirstId &&
        static_cast<std::size_t>(resource_id) <
            static_cast<std::size_t>(GameState::kGameplaySoundFirstId) +
                GameState::kGameplaySoundCount) {
      state.gameplay_sounds[resource_id - GameState::kGameplaySoundFirstId] =
          *state.weapon_fire_sounds[fire_sound_slot];
    }
    NovaLog::Info("cached weapon fire sound slot {} (snd id {})",
                  fire_sound_slot,
                  NovaWeapon_FireSoundResourceId(fire_sound_slot));
  } else {
    NovaLog::Warn("weapon fire sound slot {} (snd id {}) failed to decode",
                  fire_sound_slot,
                  NovaWeapon_FireSoundResourceId(fire_sound_slot));
  }
}

void NovaWeapon_PreloadOwnedFireSounds(GameState &state) {
  // Scan the rebuilt primary weapon banks and preload each distinct owned
  // weapon's fire sound so a bank never misses its first shot (the original
  // preloads the whole g_gameplay_sound_handle_table at startup; here we only
  // load what the player's banks use).
  for (std::int16_t b = 0; b < 0x100; ++b) {
    const std::int16_t ammo = BankAmmo(state, b);
    if (ammo <= 0) {
      continue;
    }
    const Weapon *w = WeaponAt(state, b);
    if (!w || (w->flags & 0x0002U) != 0) {
      continue; // unmounted or a secondary weapon
    }
    NovaWeapon_PreloadFireSound(state, w->fire_sound);
  }
}

void NovaWeapon_PreloadGameplaySounds(GameState &state) {
  std::size_t loaded = 0;
  for (std::size_t offset = 0; offset < GameState::kGameplaySoundCount;
       ++offset) {
    if (state.gameplay_sounds[offset].has_value()) {
      ++loaded;
      continue;
    }
    const auto resource_id =
        static_cast<std::uint16_t>(GameState::kGameplaySoundFirstId + offset);
    const auto resource = NovaResource_LoadSndData(resource_id);
    if (!resource) {
      continue;
    }
    if (auto decoded = NovaSound_Decode(*resource)) {
      state.gameplay_sounds[offset] = std::move(*decoded);
      ++loaded;
    } else {
      NovaLog::Warn("gameplay snd resource {} failed to decode", resource_id);
    }
  }
  NovaLog::Info("preloaded {} gameplay sound resources (ids {}..{})",
                loaded,
                GameState::kGameplaySoundFirstId,
                GameState::kGameplaySoundFirstId +
                    GameState::kGameplaySoundCount - 1);
}

// Ghidra NovaAudio_PlaySpatialByDistance (0x004692e0): the original computes
// left/right channel gains from the rounded source/listener displacement,
// clamps each channel to the [extent/8, extent] band, then averages the
// channels (NovaEffects_QueueCenteredResource -> Audio_AllocateVoiceSlot feeds
// a single mono gain to the mixer). The extent is the sound-volume scaled
// global (0..0x100, Frame_UpdateEffectIntensityGlobal), so the *distance*
// behavior factors out into a pure 0..1 attenuation; the caller's master
// volume carries the preference. Within 200 px the sound plays at full
// volume; beyond that the loud channel falls as 722500/d^2 (full at 850 px)
// and the quiet channel as 40000/d^2, each floored at 1/8 of the extent. The
// integer channel truncation, clamp, and (L+R+1)>>1 average are preserved so
// the gain matches the original at 1/256 resolution.
float NovaWeapon_ComputeSpatialFireGain(float listener_x,
                                        float listener_y,
                                        float src_x,
                                        float src_y) {
  const int dx = std::lround(src_x - listener_x);
  const int dy = std::lround(src_y - listener_y);
  const std::int64_t dist_sq =
      static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy;
  if (dist_sq <= 40000) {
    return 1.0F; // 200 px or closer (including src == listener): full volume
  }
  // Channel gain as the original computes it with the extent at full scale
  // (E = 0x100): (E * numerator) / dist_sq, truncated, clamped to [E/8, E].
  auto channel = [dist_sq](std::int64_t numerator) -> std::int64_t {
    const std::int64_t gain = (numerator * 256) / dist_sq;
    return std::clamp<std::int64_t>(gain, 32, 256);
  };
  const std::int64_t loud = channel(722500);
  const std::int64_t quiet = channel(40000);
  std::int64_t left = loud;
  std::int64_t right = loud;
  if (dx < 200) {
    if (dx < -200) {
      right = quiet; // source clearly to the left
    }
    // -200 <= dx < 200: horizontally centered, both channels loud
  } else {
    right = loud;
    left = quiet; // source clearly to the right
  }
  const std::int64_t average = (left + right + 1) >> 1;
  return static_cast<float>(average) / 256.0F;
}

} // namespace game
