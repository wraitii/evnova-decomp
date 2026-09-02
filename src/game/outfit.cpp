#include "outfit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "targeting.hpp"
#include "travel.hpp"

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

// Ship_ComputeShipShieldRegenRate multiplies each ModType-5 ModVal by
// _DAT_00575768 (0.001): recharge resource units are points * 1000/frame.
constexpr float kShieldRechargeScale = 0.001F;

// Symmetric armor-repair bonus scale (opcode 29).
constexpr float kArmorRechargeScale = 0.001F;

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
           (!area_only ||
            (static_cast<std::uint16_t>(mod_val) & kAreaCloakModValFlag) != 0U);
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
         index < state.inventory.outfit_owned_count.size();
         ++index) {
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
    if (const Outfit *outfit = find_in(ship_class->default_outfit_ids[index])) {
      return outfit;
    }
  }
  return nullptr;
}

void MarkStatsDirty(GameState &state) { state.stat_cache_valid = false; }

} // namespace

void OutfitMarkStatsDirty(GameState &state) {
  MarkStatsDirty(state);
  // Ownership changes alter the outfit-derived jamming bonuses, so the
  // player's lazily cached Ship.jamming_score channels (Ghidra ShipState
  // +0xC926) must recompute. The original only reseeds the cache when a ship
  // slot is allocated; the port also resets it here and at system transitions
  // (NovaWeapon_ClearTransientCombatState) so purchases apply immediately.
  state.player.jamming_score.fill(-1);
}

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

// Ghidra 0x00464db0 Outfit_GetCloakFuelDrainFlags.
std::int16_t NovaOutfit_GetCloakFuelDrainFlags(const GameState &state,
                                               const Ship &ship) {
  const Outfit *outfit = FindCloakingDevice(state, ship);
  return outfit == nullptr
             ? 0
             : static_cast<std::int16_t>(
                   (static_cast<std::uint16_t>(outfit->mod_val) >> 4) & 0x0fU);
}

// Ghidra 0x00465090 Outfit_GetCloakShieldDrainFlags.
std::int16_t NovaOutfit_GetCloakShieldDrainFlags(const GameState &state,
                                                 const Ship &ship) {
  const Outfit *outfit = FindCloakingDevice(state, ship);
  return outfit == nullptr
             ? 0
             : static_cast<std::int16_t>(
                   (static_cast<std::uint16_t>(outfit->mod_val) >> 12) & 0x0fU);
}

// Ghidra 0x00464e30 Outfit_HasCloakShieldDropOnActivation.
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

// Ghidra 0x0046e060 Ship_GetShipFuelBurnRate.
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
  // The class-base + opcode-bonus aggregates below fold the separate original
  // helpers into one pass: Ship_ComputeShipMaxShieldPoints [0x00463550],
  // Ship_ComputeShipMaxArmor [0x004637a0], Ship_ComputeShipFuelCapacity
  // [0x00463a20], Ship_ComputeShipShieldRechargeRate [0x00463b30] and
  // Ship_ComputeShipEffectiveThrust [0x004640a0] (NPC variants ported in
  // NovaShip_ComputeEffectiveStats).
  s.max_shield_points = static_cast<float>(cls->base_shield);
  s.max_armor_points = static_cast<float>(cls->base_armor);
  s.fuel_capacity = static_cast<float>(cls->base_fuel);
  s.cargo_capacity = static_cast<float>(cls->cargo_holds);
  s.thrust_raw = cls->accel;
  s.speed_raw = cls->speed;
  s.turn_raw = cls->turn_rate;
  // Base recharge rates (the loader scales ShieldRech/ArmorRech). Opcode 5
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
              static_cast<float>(owned * e.val) * kShieldRechargeScale;
        }
        break;
      case OutfitEffect::kArmorRecharge: // opcode 29
        if (e.val != 0) {
          s.armor_recharge +=
              static_cast<float>(owned * e.val) * kArmorRechargeScale;
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

// Ghidra 0x0046c080 Ship_ComputeIonizationDecayRate.
float NovaOutfit_ComputeIonizationDecayRate(const GameState &state,
                                            const Ship &ship) {
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
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
// Ghidra 0x004656a0 Outfit_ClampOutfitOwnedCountToCurrentLimits: resolve
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
// On-acquire side effects
// ---------------------------------------------------------------------------
// Ghidra 0x00427770 Outfit_GrantOutfitToPlayer. See outfit.hpp for the effect
// inventory. Returns true when the outfit was a consumed one-shot effect item
// (map reveal / paint / clean-record) rather than a stackable owned outfit.
bool NovaOutfit_GrantOutfitToPlayer(GameState &state,
                                    std::int16_t outfit_zero_based_id) {
  if (outfit_zero_based_id < 0 || outfit_zero_based_id >= 0x200) {
    return false;
  }
  const Outfit *outfit = state.scenario.Outfit(
      static_cast<std::int16_t>(outfit_zero_based_id + 0x80));
  if (outfit == nullptr) {
    return false;
  }

  // Ordered dispatch, mirroring the original (0x00427770): the FIRST
  // ModType-16 slot reveals, the first ModType-43 slot paints, and the
  // ModType-21 record clears only run when neither of those consumed the
  // outfit (the original checks its map latch + paint flag before the record
  // pass). Ownership increments only for plain outfits.
  bool has_map = false;
  std::int16_t map_val = 0;
  bool has_paint = false;
  for (const Effect &e : OutfitEffects(*outfit)) {
    if (e.type == static_cast<std::int16_t>(OutfitEffect::kMap) && !has_map) {
      has_map = true;
      map_val = e.val;
    } else if (e.type == static_cast<std::int16_t>(OutfitEffect::kPaint)) {
      has_paint = true;
    }
  }

  bool consumed = false;
  if (has_map) {
    // Galaxy-map reveal. The discovery threshold is always 2: revealed
    // systems read as visited, indistinguishable from a landed arrival. The
    // reveal floods map_val links deep from the current system (stock maps:
    // ModVal 1-3, Dr Ralph's Exploration Map 10).
    if (map_val >= 1) {
      // Reveal everything within ModVal links of the current system.
      NovaSystem_RebuildDiscoveryState(
          state, state.player.current_system_id, map_val, 2);
    } else if (map_val == -1) {
      // Reveal all neutral systems with a usable destination. The original's
      // is_visible gate is a loader-set load-time flag, so every loaded
      // system passes - the clean-room mirrors that by not gating at all.
      for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
        const auto id = static_cast<std::int16_t>(i);
        const System &sys = state.scenario.systems[i];
        if (sys.government_id == -1 &&
            NovaSystem_HasUsableTravelDestination(state, id)) {
          NovaSystem_RebuildDiscoveryState(state, id, 0, 2);
        }
      }
    } else if (map_val <= -1000) {
      // Reveal every system whose government carries the target class id in
      // any of its Class 1-4 fields (the original's is_visible gate passes
      // for every loaded system, as above).
      const std::int16_t target_class =
          static_cast<std::int16_t>(-(map_val + 1000));
      for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
        const System &sys = state.scenario.systems[i];
        if (sys.government_id < 0) {
          continue;
        }
        const Government *gov = state.scenario.Government(
            static_cast<std::int16_t>(sys.government_id + 0x80));
        if (gov == nullptr) {
          continue;
        }
        const bool matches = std::any_of(
            gov->classes.begin(),
            gov->classes.end(),
            [target_class](std::int16_t c) { return c == target_class; });
        if (matches) {
          NovaSystem_RebuildDiscoveryState(
              state, static_cast<std::int16_t>(i), 0, 2);
        }
      }
    }
    // The map latch (DAT_007d4c08) is set whenever a map slot exists, any
    // value: it blocks further map purchases until the outfitter reopens.
    state.control.map_grant_latch = true;
    consumed = true;
  }

  if (!consumed && !has_paint) {
    // Clean-record pass: every ModType-21 slot clears negative system
    // reputation — ModVal -1 targets every system (the original's is_visible
    // gate is load-time true there), otherwise the government id in ModVal.
    // Legal-record display is not modelled yet; the reputation table is the
    // persisted effect.
    bool cleared = false;
    for (const Effect &e : OutfitEffects(*outfit)) {
      if (e.type != static_cast<std::int16_t>(OutfitEffect::kCleanRecord)) {
        continue;
      }
      for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
        const System &sys = state.scenario.systems[i];
        const bool target = e.val == -1 || sys.government_id == e.val;
        if (target && i < state.system_reputation.size() &&
            state.system_reputation[i] < 0) {
          state.system_reputation[i] = 0;
        }
      }
      cleared = true;
    }
    if (cleared) {
      state.control.record_grant_latch = true; // DAT_007d4c09
      consumed = true;
    }
  }

  if (has_paint) {
    // Ghidra decodes ModVal as 15-bit RGB into 5-bit channels
    // (DAT_00733b4a/b/c/e). Ship paint rendering is not modelled yet.
    // TODO(decomp): store + consume the tint in ship_visual once paint is
    // reconstructed; for now the outfit is still treated as consumed
    // (non-stackable), matching the original's ownership behavior.
    consumed = true;
  }

  if (consumed) {
    return true;
  }
  // Plain outfit: just add it to the owned inventory.
  (void)Outfit_AddInstalledOutfit(state, outfit_zero_based_id, 1);
  return false;
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

// Ghidra 0x00469760 Outfit_ComputeFleetCargoCapacity. Player fleet cargo
// capacity = the player's own outfit-derived capacity (no NPC escorts yet).
std::int16_t Outfit_ComputePlayerFleetCargoCapacity(const GameState &state) {
  return static_cast<std::int16_t>(
      Outfit_ComputePlayerEffectiveStats(state).cargo_capacity);
}

// Ghidra 0x0046a730 Ship_ComputeShipTotalMass.
std::int32_t Outfit_ComputePlayerTotalMass(const GameState &state) {
  if (state.player.ship_class_id < 0) {
    return 0;
  }
  const auto *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (ship_class == nullptr) {
    return 0;
  }
  std::int32_t total_mass = ship_class->mass_tons;
  for (std::size_t outfit_id = 0;
       outfit_id < state.inventory.outfit_owned_count.size();
       ++outfit_id) {
    const auto owned = state.inventory.outfit_owned_count[outfit_id];
    if (owned <= 0 || outfit_id >= state.scenario.outfits.size()) {
      continue;
    }
    total_mass +=
        static_cast<std::int32_t>(owned) *
        state.scenario.outfits[outfit_id].PurchaseMass(ship_class->mass_tons);
  }
  return total_mass;
}

// Ghidra 0x0046a7c0 Outfit_ComputeRemainingCargoSpace.
std::int16_t Outfit_ComputeRemainingCargoSpace(const GameState &state) {
  const std::int32_t capacity = Outfit_ComputePlayerFleetCargoCapacity(state);
  const std::int32_t used = Outfit_ComputePlayerCargoAndJunkTotal(state);
  return static_cast<std::int16_t>(std::max<std::int32_t>(0, capacity - used));
}

// Ghidra 0x0046cb90 Outfit_HasMiningScoopOutfit. ModType 0x1F in any of the
// four mod slots of an owned (player) or class-default (NPC) outfit.
bool NovaOutfit_HasMiningScoopOutfit(const GameState &state, const Ship &ship) {
  auto outfit_has_scoop = [](const Outfit &outfit) {
    return outfit.mod_type == 0x1f ||
           std::any_of(outfit.alt_mod_types.begin(),
                       outfit.alt_mod_types.end(),
                       [](std::int16_t type) { return type == 0x1f; });
  };
  if (ship.ship_instance_id == 0) {
    for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size();
         ++id) {
      if (state.inventory.outfit_owned_count[id] <= 0) {
        continue;
      }
      const Outfit *outfit =
          state.scenario.Outfit(static_cast<std::int16_t>(id + 0x80));
      if (outfit != nullptr && outfit_has_scoop(*outfit)) {
        return true;
      }
    }
    return false;
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls == nullptr) {
    return false;
  }
  for (std::size_t slot = 0; slot < cls->default_outfit_ids.size(); ++slot) {
    if (cls->default_outfit_counts[slot] <= 0) {
      continue;
    }
    const Outfit *outfit = state.scenario.Outfit(cls->default_outfit_ids[slot]);
    if (outfit != nullptr && outfit_has_scoop(*outfit)) {
      return true;
    }
  }
  return false;
}

void NovaOutfit_AccumulatePlayerContributeMask(const GameState &state,
                                               std::uint32_t &contribute_lo,
                                               std::uint32_t &contribute_hi) {
  contribute_lo = 0;
  contribute_hi = 0;
  // Ship-class baseline contribute (ShipClassDef field_0xa30/0xa34).
  const std::int16_t ship_class_id = state.player.ship_class_id;
  const ShipClass *cls =
      ship_class_id >= 0
          ? state.scenario.Ship(static_cast<std::int16_t>(ship_class_id + 0x80))
          : nullptr;
  if (cls != nullptr) {
    contribute_lo |= cls->contribute_lo;
    contribute_hi |= cls->contribute_hi;
  }
  // Owned outfits contribute while at least one unit is held.
  for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size() &&
                          i < state.scenario.outfits.size();
       ++i) {
    if (state.inventory.outfit_owned_count[i] > 0) {
      contribute_lo |= state.scenario.outfits[i].contribute_lo;
      contribute_hi |= state.scenario.outfits[i].contribute_hi;
    }
  }
}

bool NovaOutfit_EvaluateRequireMask(const GameState &state,
                                    std::uint32_t require_lo,
                                    std::uint32_t require_hi) {
  std::uint32_t contribute_lo = 0;
  std::uint32_t contribute_hi = 0;
  NovaOutfit_AccumulatePlayerContributeMask(
      state, contribute_lo, contribute_hi);
  return (require_lo & contribute_lo) == require_lo &&
         (require_hi & contribute_hi) == require_hi;
}

} // namespace game
