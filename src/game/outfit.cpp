#include "outfit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "targeting.hpp"

namespace game {
namespace {

// One outfit's effective (ModType, ModVal) effect pair. The original stores
// up to four of these per OutfitDef (ModType1/Val1 .. ModType4/Val4); our
// clean-room Outfit carries ModType1/Val1 in mod_type/mod_val and ModType2-4
// in alt_mod_types/alt_mod_vals.
struct Effect {
  std::int16_t type = 0;
  std::int16_t val = 0;
};

// Iterates the up-to-4 (ModType, ModVal) effect pairs for one outfit.
std::array<Effect, 4> OutfitEffects(const Outfit &o) {
  return {Effect{o.mod_type, o.mod_val},
          Effect{o.alt_mod_types[0], o.alt_mod_vals[0]},
          Effect{o.alt_mod_types[1], o.alt_mod_vals[1]},
          Effect{o.alt_mod_types[2], o.alt_mod_vals[2]}};
}

// Scale applied to the summed shield-recharge bonuses: shield_recharge bonus
// (opcode 5) = kShieldRechargeScale / modval, in shield points per frame.
// Ghidra _DAT_00575778. TODO(decomp): verify magnitude against a capture.
constexpr float kShieldRechargeScale = 50.0F;

// Symmetric armor-repair bonus scale (opcode 29). Provisional, same caveat.
constexpr float kArmorRechargeScale = 50.0F;

// The inventory mutation / cache helpers update this through GameState.
// Outfit_ComputePlayerEffectiveStats runs a full 0x200-outfit scan; the
// low-level Ship_ComputeShip* helpers in the original instead cache the player
// result in _DAT_00735688/90/98 and invalidate on ownership changes. This
// module exposes OutfitMarkStatsDirty and the spaceflight loop caches the
// computed snapshot in GameState.cached_stats (stat_cache_valid).
constexpr float kFuelCapacityClamp = 32000.0F; // opcode 12 clamp [0,32000]

constexpr std::int16_t kCloakingDeviceModType = 0x11;
constexpr std::uint16_t kAreaCloakModValFlag = 0x1000;

[[nodiscard]] bool OutfitHasCloakingDevice(const Outfit &outfit,
                                           bool area_only) {
  const auto matches = [area_only](std::int16_t mod_type,
                                   std::int16_t mod_val) {
    return mod_type == kCloakingDeviceModType &&
           (!area_only || (static_cast<std::uint16_t>(mod_val) &
                           kAreaCloakModValFlag) != 0U);
  };
  if (matches(outfit.mod_type, outfit.mod_val)) {
    return true;
  }
  for (std::size_t i = 0; i < outfit.alt_mod_types.size(); ++i) {
    if (matches(outfit.alt_mod_types[i], outfit.alt_mod_vals[i])) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool ShipClassHasCloakingDevice(const GameState &state,
                                              const Ship &ship,
                                              bool area_only) {
  const ShipClass *ship_class =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (ship_class == nullptr) {
    return false;
  }
  for (std::size_t i = 0; i < ship_class->default_outfit_ids.size(); ++i) {
    if (ship_class->default_outfit_counts[i] <= 0) {
      continue;
    }
    const Outfit *outfit =
        state.scenario.Outfit(ship_class->default_outfit_ids[i]);
    if (outfit != nullptr && OutfitHasCloakingDevice(*outfit, area_only)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool PlayerHasCloakingDevice(const GameState &state,
                                           bool area_only) {
  for (std::size_t index = 0; index < state.inventory.outfit_owned_count.size();
       ++index) {
    if (state.inventory.outfit_owned_count[index] <= 0) {
      continue;
    }
    const Outfit *outfit =
        state.scenario.Outfit(static_cast<std::int16_t>(index + 0x80));
    if (outfit != nullptr && OutfitHasCloakingDevice(*outfit, area_only)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] const Outfit *FindCloakingDevice(const GameState &state,
                                               const Ship &ship) {
  const auto find_in = [&](std::int16_t outfit_id) -> const Outfit * {
    const Outfit *outfit = state.scenario.Outfit(outfit_id);
    if (outfit != nullptr && OutfitHasCloakingDevice(*outfit, false)) {
      return outfit;
    }
    return nullptr;
  };

  if (ship.ship_instance_id == 0) {
    for (std::size_t index = 0;
         index < state.inventory.outfit_owned_count.size(); ++index) {
      if (state.inventory.outfit_owned_count[index] <= 0) {
        continue;
      }
      if (const Outfit *outfit =
              find_in(static_cast<std::int16_t>(index + 0x80))) {
        return outfit;
      }
    }
    return nullptr;
  }

  const ShipClass *ship_class =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (ship_class == nullptr) {
    return nullptr;
  }
  for (std::size_t index = 0; index < ship_class->default_outfit_ids.size();
       ++index) {
    if (ship_class->default_outfit_counts[index] <= 0) {
      continue;
    }
    if (const Outfit *outfit =
            find_in(ship_class->default_outfit_ids[index])) {
      return outfit;
    }
  }
  return nullptr;
}

void MarkStatsDirty(GameState &state) { state.stat_cache_valid = false; }

} // namespace

void OutfitMarkStatsDirty(GameState &state) { MarkStatsDirty(state); }

// Ghidra 0x00464b50 Outfit_HasCloakingDevice.
bool NovaOutfit_HasCloakingDevice(const GameState &state, const Ship &ship) {
  if (ship.ship_instance_id == 0) {
    return PlayerHasCloakingDevice(state, false);
  }
  // Ghidra's helper includes this NPC-only escort/control-mode exception
  // before scanning the ship class's default loadout.
  if (ship.ai_target_ship_slot >= 0 && ship.ai_control_mode == 0xc &&
      state.SlotInRange(static_cast<std::size_t>(ship.ai_target_ship_slot))) {
    const Ship &target =
        state.ShipAt(static_cast<std::size_t>(ship.ai_target_ship_slot));
    if (NovaTargeting_ShipAtCloakVisibilityThreshold(target) &&
        NovaOutfit_HasAreaCloakingDevice(state, target)) {
      return true;
    }
  }
  return ShipClassHasCloakingDevice(state, ship, false);
}

// Ghidra 0x00464c80 Outfit_HasAreaCloakingDevice.
bool NovaOutfit_HasAreaCloakingDevice(const GameState &state,
                                      const Ship &ship) {
  if (ship.ship_instance_id == 0) {
    return PlayerHasCloakingDevice(state, true);
  }
  return ShipClassHasCloakingDevice(state, ship, true);
}

std::int16_t NovaOutfit_GetCloakFuelDrainFlags(
    const GameState &state, const Ship &ship) {
  const Outfit *outfit = FindCloakingDevice(state, ship);
  return outfit == nullptr
             ? 0
             : static_cast<std::int16_t>((static_cast<std::uint16_t>(
                                               outfit->mod_val) >> 4) &
                                          0x0fU);
}

std::int16_t NovaOutfit_GetCloakShieldDrainFlags(const GameState &state,
                                                const Ship &ship) {
  const Outfit *outfit = FindCloakingDevice(state, ship);
  return outfit == nullptr
             ? 0
             : static_cast<std::int16_t>((static_cast<std::uint16_t>(
                                               outfit->mod_val) >> 12) &
                                          0x0fU);
}

bool NovaOutfit_HasCloakShieldDropOnActivation(const GameState &state,
                                               const Ship &ship) {
  const Outfit *outfit = FindCloakingDevice(state, ship);
  return outfit != nullptr &&
         (static_cast<std::uint16_t>(outfit->mod_val) & 0x0004U) != 0U;
}

// ---------------------------------------------------------------------------
// Effective-stats aggregation
// ---------------------------------------------------------------------------
// The original's Ship_ComputeShip* helpers (see header for addresses) compute
// a player ship's effective maximums from ship-class base + outfit opcode
// bonuses, weighting each bonus by the owned count of the outfit carrying it.
// The player paths cache their result in a sentinel-dirty global; we instead
// expose a pure function and let the loop cache it in GameState.

[[nodiscard]] bool Outfit_HasOwnedEffect(const GameState &state,
                                         OutfitEffect effect) {
  for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size();
       ++id) {
    if (state.inventory.outfit_owned_count[id] <= 0) {
      continue;
    }
    // id is a zero-based after-0x80 index; the scenario table stores its entry
    // at that index too (Ship/Outfit() use the +0x80 convention). Because the
    // table is indexed [0..], the entry for zero-based `id` is directly
    // state.scenario.outfits[id].
    if (id >= state.scenario.outfits.size()) {
      continue;
    }
    const Outfit &o = state.scenario.outfits[id];
    for (const Effect &e : OutfitEffects(o)) {
      if (e.type == static_cast<std::int16_t>(effect)) {
        return true;
      }
    }
  }
  return false;
}

float Outfit_GetPlayerAfterburnerFuelBurnRate(const GameState &state) {
  // _DAT_00575858 is 1/30. The original stops at the first opcode-15 slot in
  // an outfit but continues through the table, so a later owned afterburner
  // replaces an earlier value; ownership count does not multiply the rate.
  float rate = 0.0F;
  for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size();
       ++id) {
    if (state.inventory.outfit_owned_count[id] <= 0 ||
        id >= state.scenario.outfits.size()) {
      continue;
    }
    for (const Effect &e : OutfitEffects(state.scenario.outfits[id])) {
      if (e.type == static_cast<std::int16_t>(OutfitEffect::kAfterburner)) {
        rate = static_cast<float>(e.val) / 30.0F;
        break;
      }
    }
  }
  return rate;
}

PlayerEffectiveStats
Outfit_ComputePlayerEffectiveStats(const GameState &state) {
  const int16_t ship_class_id = state.player.ship_class_id;
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship_class_id + 0x80));
  // Fall back to the starter-class values when the class table is unavailable,
  // matching the movement fallback in NovaPlayer_UpdateFromInput.
  ShipClass fallback;
  if (!cls) {
    fallback.base_shield = 0;
    fallback.base_armor = 0;
    fallback.base_fuel = 0;
    fallback.cargo_holds = 0;
    fallback.accel = 500.0F;
    fallback.speed = 400.0F;
    fallback.turn_rate = 40.0F;
    cls = &fallback;
  }

  PlayerEffectiveStats s;
  // These movement fields remain in their raw resource units until the
  // movement integrator applies the corresponding conversion.
  s.max_shield_points = static_cast<float>(cls->base_shield);
  s.max_armor_points = static_cast<float>(cls->base_armor);
  s.fuel_capacity = static_cast<float>(cls->base_fuel);
  s.cargo_capacity = static_cast<float>(cls->cargo_holds);
  s.thrust_raw = cls->accel;
  s.speed_raw = cls->speed;
  s.turn_raw = cls->turn_rate;
  // Base recharge rates (the loader scaled ShieldRech/ArmorRech). Opcode 5
  // (kShieldRecharge) and opcode 29 (kArmorRecharge) add to these.
  s.shield_recharge = cls->shield_recharge;
  s.armor_recharge = cls->armor_recharge;

  // Outfit opcode bonuses, weighted by owned count. Mirrors the exact
  // aggregate shapes: 4 effect slots per outfit, only outfits with an owned
  // count > 0 contribute.
  for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size();
       ++id) {
    const std::int16_t owned = state.inventory.outfit_owned_count[id];
    if (owned <= 0 || id >= state.scenario.outfits.size()) {
      continue;
    }
    const Outfit &o = state.scenario.outfits[id];
    for (const Effect &e : OutfitEffects(o)) {
      const float weighted =
          static_cast<float>(owned) * static_cast<float>(e.val);
      switch (static_cast<OutfitEffect>(e.type)) {
      case OutfitEffect::kShield: // opcode 4
        s.max_shield_points += weighted;
        break;
      case OutfitEffect::kArmor: // opcode 6
        s.max_armor_points += weighted;
        break;
      case OutfitEffect::kCargoSpace: // opcode 2
        s.cargo_capacity += weighted;
        break;
      case OutfitEffect::kAccelerator: // opcode 7
        s.thrust_raw += weighted;
        break;
      case OutfitEffect::kSpeed: // opcode 8
        s.speed_raw += weighted;
        break;
      case OutfitEffect::kTurn: // opcode 9
        s.turn_raw += weighted;
        break;
      case OutfitEffect::kShieldRecharge: // opcode 5
        if (e.val != 0) {
          s.shield_recharge +=
              static_cast<float>(owned) *
              (kShieldRechargeScale / static_cast<float>(e.val));
        }
        break;
      case OutfitEffect::kArmorRecharge: // opcode 29
        if (e.val != 0) {
          s.armor_recharge += static_cast<float>(owned) *
                              (kArmorRechargeScale / static_cast<float>(e.val));
        }
        break;
      case OutfitEffect::kFuelCapacity: // opcode 12
        s.fuel_capacity += weighted;
        break;
      case OutfitEffect::kModifyMaxGuns: // opcode 45
        s.max_guns += e.val;
        break;
      case OutfitEffect::kModifyMaxTurrets: // opcode 46
        s.max_turrets += e.val;
        break;
      default:
        break;
      }
    }
  }

  // Clamps the exact way the loader clamps: fuel_capacity to [0,32000].
  s.fuel_capacity = std::clamp(s.fuel_capacity, 0.0F, kFuelCapacityClamp);
  s.max_shield_points = std::max(0.0F, s.max_shield_points);
  s.max_armor_points = std::max(0.0F, s.max_armor_points);
  // Base gun/turret slot counts (class max) + opcode mods.
  s.max_guns += cls->max_gun;
  s.max_turrets += cls->max_turret;
  return s;
}

float NovaOutfit_ComputeIonizationDecayRate(const GameState &state,
                                            const Ship &ship) {
  const ShipClass *cls = state.scenario.Ship(
      static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  float rate = cls != nullptr ? cls->ionization_decay_rate : 0.0F;
  if (ship.ship_instance_id != 0) {
    return rate;
  }

  constexpr float kModValueScale = 0.01F; // Ghidra DAT_00575738
  for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size();
       ++id) {
    const std::int16_t owned = state.inventory.outfit_owned_count[id];
    if (owned <= 0 || id >= state.scenario.outfits.size()) {
      continue;
    }
    for (const Effect &effect : OutfitEffects(state.scenario.outfits[id])) {
      if (effect.type ==
          static_cast<std::int16_t>(OutfitEffect::kIonDissipator)) {
        rate += static_cast<float>(owned) * static_cast<float>(effect.val) *
                kModValueScale;
      }
    }
  }
  return rate;
}

// ---------------------------------------------------------------------------
// Ownership limiting
// ---------------------------------------------------------------------------
// Mirrors Outfit_ClampOutfitOwnedCountToCurrentLimits (0x004656a0): resolve
// the effective owned count and max-allowed for one outfit, honoring (in
// order) ammo-backed weapon limits, ModType-27 (kIncreaseMax) maximum
// multipliers, and the gun/turret slot caps. `outfit_resource_id` is the
// zero-based after-0x80 outfit id.

OutfitOwnership
Outfit_ClampOwnedCountToLimits(const GameState &state,
                               std::int16_t outfit_resource_id) {
  OutfitOwnership out;
  if (outfit_resource_id < 0 || outfit_resource_id >= 0x200) {
    return out; // out of range -> both 0
  }
  const std::size_t idx = static_cast<std::size_t>(outfit_resource_id);
  if (idx >= state.scenario.outfits.size()) {
    return out;
  }
  const Outfit &o = state.scenario.outfits[idx];
  const std::int16_t owned = state.inventory.outfit_owned_count[idx];

  // Base maximum = the outfit's Max field.
  out.max_allowed = o.max_count;

  std::int16_t effective = owned;

  // (1) Ammo-backed weapons: an outfit whose ModType1 == 3 (ammo) supplies a
  // weapon bank; the ammo outfit's holdings are capped by the bank ammo and
  // any ammo capacity the weapon definitions impose.
  //
  // TODO(decomp): the original caps this via the weapon bank / ammo system
  // (Weapon_CanFireWeaponBank gating on weapon_bank_secondary counters), not
  // a per-weapon "max ammo" payload field. Earlier clean-room code fabricated
  // a max_ammo from payload +0x5a, which is actually burst_cycle_ticks (see
  // the weapon-decode audit). Until the real ammo system is reconstructed we
  // leave the capacity cap out; the plain owned-count handling below still
  // bounds it correctly for non-ammo outfits.
  if (o.mod_type == static_cast<std::int16_t>(OutfitEffect::kAmmo)) {
    // Held ammo is not yet a separate concept in this build; skip the
    // capacity cap (TODO(decomp) above).
    out.effective_owned = static_cast<std::int16_t>(std::max<int>(0, owned));
    return out;
  }

  // (2) ModType-27 (kIncreaseMax) maximum multipliers: every owned outfit
  // pointing at this one (mod type 27, mod val == this outfit's id) multiplies
  // the base maximum by the owned count of the multiplier.
  std::int32_t multiplier = 0;
  for (std::size_t mid = 0; mid < state.inventory.outfit_owned_count.size();
       ++mid) {
    const std::int16_t mowned = state.inventory.outfit_owned_count[mid];
    if (mowned <= 0 || mid >= state.scenario.outfits.size()) {
      continue;
    }
    const Outfit &mo = state.scenario.outfits[mid];
    for (const Effect &e : OutfitEffects(mo)) {
      // The mod value names the target outfit's RESOURCE id (0x80..), matching
      // how the Bible/Max references the target.
      if (e.type == static_cast<std::int16_t>(OutfitEffect::kIncreaseMax) &&
          e.val == static_cast<std::int16_t>(outfit_resource_id + 0x80)) {
        multiplier += mowned;
        break;
      }
    }
  }
  multiplier = std::max<std::int32_t>(1, multiplier);
  const std::int16_t lifted = static_cast<std::int16_t>(
      std::clamp<std::int32_t>(multiplier * out.max_allowed, 0, 32767));
  if (multiplier > 1 || lifted < out.max_allowed) {
    out.max_allowed = lifted;
  }
  effective = std::min<int>(effective, lifted);

  // (3) Gun / turret slot caps (outfit Flags bits 0x0001 / 0x0002): the player
  // can only mount as many guns/turrets as the ship class allows, lifted by
  // opcode 45 (guns) / 46 (turrets) mods. The cap is the sum of the two
  // relevant variables: guns use class max_gun = max_guns minus the 45 lifts
  // we already folded into PlayerEffectiveStats; to avoid double counting we
  // recompute the base + lifts here from the class and owned outfits directly,
  // exactly mirroring the decomp's sVar7 (= max_gun + opcode45) and sVar8
  // (= owned guns) tracking.
  const std::int16_t ship_class_id = state.player.ship_class_id;
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship_class_id + 0x80));

  std::int16_t gun_cap = 0;       // sVar7: class max_gun + opcode 45 lifts
  std::int16_t turret_cap = 0;    // sVar8: class max_tur + opcode 46 lifts
  std::int16_t guns_owned = 0;    // total owned guns (flags & 1)
  std::int16_t turrets_owned = 0; // total owned turrets (flags & 2)
  if (cls) {
    gun_cap = cls->max_gun;
    turret_cap = cls->max_turret;
  }
  for (std::size_t gid = 0; gid < state.inventory.outfit_owned_count.size();
       ++gid) {
    const std::int16_t gowned = state.inventory.outfit_owned_count[gid];
    if (gowned <= 0 || gid >= state.scenario.outfits.size()) {
      continue;
    }
    const Outfit &go = state.scenario.outfits[gid];
    if (go.flags & 0x0001) {
      guns_owned += gowned;
    }
    if (go.flags & 0x0002) {
      turrets_owned += gowned;
    }
    for (const Effect &e : OutfitEffects(go)) {
      if (e.type == static_cast<std::int16_t>(OutfitEffect::kModifyMaxGuns)) {
        gun_cap += e.val;
      } else if (e.type ==
                 static_cast<std::int16_t>(OutfitEffect::kModifyMaxTurrets)) {
        turret_cap += e.val;
      }
    }
  }

  // Apply the slot cap for whichever kind this outfit is.
  if (o.flags & 0x0001) { // gun
    if (gun_cap < 1) {
      out.effective_owned = 0;
      return out;
    }
    out.max_allowed = std::min(out.max_allowed, gun_cap);
    if (gun_cap <= guns_owned) {
      out.effective_owned =
          static_cast<std::int16_t>(std::max<int>(0, gun_cap));
      return out; // all gun slots full
    }
  }
  if (o.flags & 0x0002) { // turret
    if (turret_cap < 1) {
      out.effective_owned = 0;
      return out;
    }
    out.max_allowed = std::min(out.max_allowed, turret_cap);
    if (turret_cap <= turrets_owned) {
      out.effective_owned =
          static_cast<std::int16_t>(std::max<int>(0, turret_cap));
      return out; // all turret slots full
    }
  }

  // Otherwise effective owned is min(owned, max_allowed) for non-limited items
  // (the decomp's final branch preserves the owned count, which is already the
  // min here because the limits above clamp it).
  out.effective_owned = static_cast<std::int16_t>(
      std::min<int>(effective, std::max<int>(0, out.max_allowed)));
  return out;
}

// ---------------------------------------------------------------------------
// Inventory mutation
// ---------------------------------------------------------------------------
std::int16_t Outfit_AddInstalledOutfit(GameState &state,
                                       std::int16_t outfit_resource_id,
                                       std::int16_t count) {
  if (count <= 0 || outfit_resource_id < 0 || outfit_resource_id >= 0x200) {
    return 0;
  }
  const std::size_t idx = static_cast<std::size_t>(outfit_resource_id);
  if (idx >= state.inventory.outfit_owned_count.size()) {
    return 0;
  }
  const OutfitOwnership lim =
      Outfit_ClampOwnedCountToLimits(state, outfit_resource_id);
  // We can add up to (max - current) more, but only if the cap isn't already
  // reducing it. max_allowed is the independent ceiling.
  const std::int16_t room = static_cast<std::int16_t>(
      std::max(0,
               static_cast<int>(lim.max_allowed) -
                   state.inventory.outfit_owned_count[idx]));
  const std::int16_t applied = std::min(count, room);
  if (applied > 0) {
    state.inventory.outfit_owned_count[idx] = static_cast<std::int16_t>(
        state.inventory.outfit_owned_count[idx] + applied);
    OutfitMarkStatsDirty(state);
  }
  return applied;
}

std::int16_t Outfit_RemoveOutfit(GameState &state,
                                 std::int16_t outfit_resource_id,
                                 std::int16_t count) {
  if (count <= 0 || outfit_resource_id < 0 || outfit_resource_id >= 0x200) {
    return 0;
  }
  const std::size_t idx = static_cast<std::size_t>(outfit_resource_id);
  if (idx >= state.inventory.outfit_owned_count.size()) {
    return 0;
  }
  std::int16_t &owned = state.inventory.outfit_owned_count[idx];
  const std::int16_t removed =
      static_cast<std::int16_t>(std::min<int>(owned, count));
  owned = static_cast<std::int16_t>(owned - removed);
  if (removed > 0) {
    OutfitMarkStatsDirty(state);
  }
  return removed;
}

// ---------------------------------------------------------------------------
// Cargo bookkeeping
// ---------------------------------------------------------------------------
// The player ship's total cargo + junk. Mirrors
// Outfit_ComputePlayerCargoAndJunkTotal (0x0046a5d0): the 6 cargo bins plus
// junk quantities (no mission-cargo in this build).
std::int16_t Outfit_ComputePlayerCargoAndJunkTotal(const GameState &state) {
  std::int32_t total = 0;
  for (const std::int16_t bin : state.inventory.cargo_bins) {
    total += bin;
  }
  for (const std::int16_t junk : state.inventory.junk_counts) {
    if (junk > 0) {
      total += junk;
    }
  }
  return static_cast<std::int16_t>(total);
}

// Player fleet cargo capacity = the player's own outfit-derived capacity (no
// NPC escorts yet). Mirrors Outfit_ComputeFleetCargoCapacity (0x00469760).
std::int16_t Outfit_ComputePlayerFleetCargoCapacity(const GameState &state) {
  return static_cast<std::int16_t>(
      Outfit_ComputePlayerEffectiveStats(state).cargo_capacity);
}

std::int16_t Outfit_ComputeRemainingCargoSpace(const GameState &state) {
  const std::int32_t capacity = Outfit_ComputePlayerFleetCargoCapacity(state);
  const std::int32_t used = Outfit_ComputePlayerCargoAndJunkTotal(state);
  return static_cast<std::int16_t>(std::max<std::int32_t>(0, capacity - used));
}

} // namespace game
