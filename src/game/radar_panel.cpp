#include "radar_panel.hpp"

#include "government.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "ship_ai.hpp"
#include "targeting.hpp"

#include <algorithm>
#include <random>

namespace game {
namespace {

// The IFF palette is a set of fixed RGBColor triples seeded at session start
// by Settings_InitTimingPresets (0x004ad7c0). Components are 16-bit; the SDL
// port truncates to 8 bits (the original's RGB555 rasterization keeps only
// the high 5 bits of each component anyway).
[[nodiscard]] constexpr std::uint8_t Comp8(std::uint16_t component) {
  return static_cast<std::uint8_t>(component >> 8);
}

[[nodiscard]] constexpr SDL_Color
Rgb(std::uint16_t r, std::uint16_t g, std::uint16_t b) {
  return SDL_Color{Comp8(r), Comp8(g), Comp8(b), SDL_ALPHA_OPAQUE};
}

// RGBColor triples at DAT_00733b32 / DAT_00733b50 / DAT_00733b56 / DAT_00733b5c
// (Settings_InitTimingPresets 0x004ad7c0).
constexpr SDL_Color kIffShipAttackingWithPlayer = Rgb(0x0000, 0xffff, 0x0000);
constexpr SDL_Color kIffShipNeutral = Rgb(0x0000, 0x0000, 0xffff);
constexpr SDL_Color kIffShipFireRestricted = Rgb(0x4000, 0x4000, 0x4000);
// The primary-target blink grey is shared with the HUD renderer (declared in
// radar_panel.hpp as kRadarTargetBlinkColor).
constexpr SDL_Color kIffStellarInactive = Rgb(0x8000, 0x8000, 0x8000);
constexpr SDL_Color kIffStellarHazard = Rgb(0x0000, 0xffff, 0x0000);
constexpr SDL_Color kIffStellarFriendly = Rgb(0xffff, 0xffff, 0x0000);
constexpr SDL_Color kIffStellarNeutral = Rgb(0xffff, 0x6666, 0x0000);
constexpr SDL_Color kIffStellarHostile = Rgb(0xffff, 0x0000, 0x0000);
constexpr SDL_Color kIffStellarWormhole = Rgb(0x0000, 22000, 22000);

// Ship state colours (Ship_GetShipRadarColor 0x00465f00): the player slot and
// the distress / hostility picks.
constexpr SDL_Color kIffPlayerColor = Rgb(0x0000, 0xffff, 0xffff);

// The shared reputation-threshold colour ladder used for both ordinary and
// hypergate stellar bodies in Stellar_GetStellarRadarColor: below the
// stellar's MinStatus the blip is red (hostile reputation) or orange, at or
// above it yellow (0x7fff MinStatus always takes the denied ladder; -0x7fff
// never does).
[[nodiscard]] SDL_Color StellarReputationColor(const GameState &state,
                                               const Stellar &st) {
  const std::int16_t sys_rep =
      st.system_id >= 0 && st.system_id < static_cast<std::int16_t>(
                                              state.system_reputation.size())
          ? state.system_reputation[static_cast<std::size_t>(st.system_id)]
          : 0;
  const bool below_threshold =
      st.min_status == 0x7fff ||
      (sys_rep < st.min_status && st.min_status > -0x7fff);
  if (below_threshold) {
    return sys_rep < 0 ? kIffStellarHostile : kIffStellarNeutral;
  }
  return kIffStellarFriendly;
}

} // namespace

SDL_Color Ship_RadarDisplayColor(const GameState &state, const Ship &ship) {
  if (ship.ship_instance_id == 0) {
    return kIffPlayerColor;
  }
  if (NovaAiShip_IsDisabled(state, ship)) {
    return kIffShipFireRestricted;
  }
  if (NovaTargeting_IsShipEligibleForDistressCall(state, ship)) {
    return kIffStellarHostile;
  }
  const std::int16_t squad_leader = ship.squad_leader_ship_slot;
  if (squad_leader == 0 && ship.defense_fleet_home_stellar_id == -1) {
    return kIffShipAttackingWithPlayer;
  }
  if (squad_leader == -1) {
    return kIffShipNeutral;
  }
  if (state.SlotInRange(static_cast<std::size_t>(squad_leader)) &&
      state.ShipAt(static_cast<std::size_t>(squad_leader))
              .squad_leader_ship_slot == 0) {
    return kIffShipAttackingWithPlayer;
  }
  return kIffShipNeutral;
}

SDL_Color Stellar_RadarDisplayColor(const GameState &state, const Stellar &st) {
  // Inactive bodies and uninhabited bodies (Flags bit 0x20) stay grey.
  if (!NovaTargeting_StellarTargetsSpriteSetActive(st) ||
      (st.flags & 0x20U) != 0U) {
    return kIffStellarInactive;
  }
  // Hypergates (availability 0x1000): yellow when the owning government's
  // policy flag 1 admits the player, else the reputation ladder.
  if ((st.availability_flags & 0x1000U) != 0U) {
    if (st.government_id != -1 &&
        NovaGovernment_GetPolicyFlag(state.scenario, st.government_id, 1)) {
      return kIffStellarFriendly;
    }
    return StellarReputationColor(state, st);
  }
  // Wormholes (availability 0x2000) draw dark cyan.
  if ((st.availability_flags & 0x2000U) != 0U) {
    return kIffStellarWormhole;
  }
  // Hazard markers (availability 0x20 hazard/derelict bit, cached into
  // StellarDef +0x46) draw green.
  if (st.hazard_marker) {
    return kIffStellarHazard;
  }
  if (st.government_id != -1 &&
      NovaGovernment_GetPolicyFlag(state.scenario, st.government_id, 1)) {
    return kIffStellarFriendly;
  }
  return StellarReputationColor(state, st);
}

bool Outfit_HasCloakRadarVisibility(const GameState &state, const Ship &ship) {
  constexpr std::uint16_t kRadarVisibleFlag = 0x0002;
  const auto visible = [](const Outfit &outfit,
                          std::uint16_t radar_visible_flag) {
    return outfit.mod_type ==
               static_cast<std::int16_t>(OutfitEffect::kCloaking) &&
           (static_cast<std::uint16_t>(outfit.mod_val) & radar_visible_flag) !=
               0U;
  };
  if (ship.ship_instance_id == 0) {
    for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size();
         ++id) {
      if (state.inventory.outfit_owned_count[id] <= 0 ||
          id >= state.scenario.outfits.size()) {
        continue;
      }
      if (visible(state.scenario.outfits[id], kRadarVisibleFlag)) {
        return true;
      }
    }
    return false;
  }
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
    if (outfit != nullptr && visible(*outfit, kRadarVisibleFlag)) {
      return true;
    }
  }
  return false;
}

bool Player_HasCloakScannerReveal(const GameState &state,
                                  std::uint16_t reveal_flag) {
  for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size();
       ++id) {
    if (state.inventory.outfit_owned_count[id] <= 0 ||
        id >= state.scenario.outfits.size()) {
      continue;
    }
    const Outfit &o = state.scenario.outfits[id];
    const auto matches = [&](std::int16_t mod_type, std::int16_t mod_val) {
      return mod_type ==
                 static_cast<std::int16_t>(OutfitEffect::kCloakScanner) &&
             (static_cast<std::uint16_t>(mod_val) & reveal_flag) != 0U;
    };
    if (matches(o.mod_type, o.mod_val)) {
      return true;
    }
    for (std::size_t i = 0; i < o.alt_mod_types.size(); ++i) {
      if (matches(o.alt_mod_types[i], o.alt_mod_vals[i])) {
        return true;
      }
    }
  }
  return false;
}

bool Player_HasCloakScannerRadarReveal(const GameState &state) {
  return Player_HasCloakScannerReveal(state, 0x0001);
}

int Ship_ComputeScannerStrength(const GameState &state) {
  constexpr std::int16_t kInterferenceModType = 0x18; // ModType 24
  int strength = 0;
  for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size();
       ++id) {
    const std::int16_t owned = state.inventory.outfit_owned_count[id];
    if (owned <= 0 || id >= state.scenario.outfits.size()) {
      continue;
    }
    const Outfit &o = state.scenario.outfits[id];
    const auto accumulate = [&](std::int16_t mod_type, std::int16_t mod_val) {
      if (mod_type == kInterferenceModType) {
        strength += owned * mod_val;
      }
    };
    accumulate(o.mod_type, o.mod_val);
    for (std::size_t i = 0; i < o.alt_mod_types.size(); ++i) {
      accumulate(o.alt_mod_types[i], o.alt_mod_vals[i]);
    }
  }
  return std::clamp(strength, -100, 100);
}

void Frame_RollProximityScanDetection(GameState &state) {
  const int odds = std::clamp(
      [&] {
        const auto *sys = state.scenario.System(
            static_cast<std::int16_t>(state.player.current_system_id + 0x80));
        return (sys ? sys->interference : 0) -
               Ship_ComputeScannerStrength(state);
      }(),
      0,
      100);
  std::uniform_int_distribution<int> roll(0, 99);
  state.proximity_scan_detected = roll(state.rng) + 1 <= odds;
}

// Ghidra 0x004654b0 Outfit_HasPlayerOwnedOutfitType0x0E_Cached.
bool Outfit_PlayerHasIffOutfit(const GameState &state) {
  return Outfit_HasOwnedEffect(state, OutfitEffect::kIff);
}

// Ghidra 0x00465410 Outfit_HasPlayerOwnedOutfitType0x0D_Cached.
bool Outfit_PlayerHasDensityScanner(const GameState &state) {
  return Outfit_HasOwnedEffect(state, OutfitEffect::kDensityScanner);
}

} // namespace game
