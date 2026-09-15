#include "targeting.hpp"

#include "../log.hpp"
#include "government.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"
#include <array>
#include <cmath>

namespace game {

void NovaTargeting_ClearDestroyedShipReferences(GameState &state,
                                                std::int16_t destroyed_slot) {
  if (destroyed_slot < 0 ||
      !state.SlotInRange(static_cast<std::size_t>(destroyed_slot))) {
    return;
  }
  if (state.player.primary_target_ship_slot == destroyed_slot) {
    state.player.primary_target_ship_slot = -1;
  }
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.ship_instance_id == destroyed_slot) {
      continue;
    }
    if (ship.primary_target_ship_slot == destroyed_slot) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      ship.primary_target_ship_slot = -1;
      ship.ai_secondary_target_slot = -1;
      ship.ai_hostility_accumulator = 0;
    } else if (ship.ai_secondary_target_slot == destroyed_slot) {
      ship.ai_state_code = 0;
      ship.ai_control_mode = 0;
      ship.ai_secondary_target_slot = -1;
      ship.ai_hostility_accumulator = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// Player scanner capabilities (modType 0x1e cloak-scanner outfit family).
// ---------------------------------------------------------------------------
// Ghidra 0x0046c930 Outfit_HasScannerTargetUntargetableCapability and
// 0x0046ca60 Outfit_HasCloakScannerTargetCloakedCapability (for the player):
// any owned outfit whose primary or alternate mod type is 0x1e (kCloakScanner)
// and whose mod value has bit 0x04 grants targeting of "untargetable" ships
// (class flags_secondary bit 2), bit 0x08 grants targeting through the
// cloak-visibility gate. The original memoizes the result in the tri-state
// caches g_player_has_cloak_scanner_target_untargetable_cached (DAT_007356be)
// / _cloaked_cached (DAT_007356c0); this build rescans the 0x200-entry owned
// table on demand because the calls happen once per targeting command press,
// not per frame.
struct PlayerScannerCapabilities {
  bool can_target_untargetable = false; // modVal & 0x04
  bool can_target_cloaked = false;      // modVal & 0x08
};

[[nodiscard]] PlayerScannerCapabilities
ScannerCapabilities(const GameState &state) {
  PlayerScannerCapabilities caps;
  const auto &owned = state.inventory.outfit_owned_count;
  const auto check_mods = [&](const Outfit &outfit) {
    const auto probe = [&](std::int16_t mod_type, std::int16_t mod_val) {
      if (mod_type == static_cast<std::int16_t>(OutfitEffect::kCloakScanner)) {
        caps.can_target_untargetable =
            caps.can_target_untargetable || (mod_val & 0x04) != 0;
        caps.can_target_cloaked =
            caps.can_target_cloaked || (mod_val & 0x08) != 0;
      }
    };
    probe(outfit.mod_type, outfit.mod_val);
    for (std::size_t i = 0; i < outfit.alt_mod_types.size(); ++i) {
      probe(outfit.alt_mod_types[i], outfit.alt_mod_vals[i]);
    }
  };
  for (std::size_t id = 0; id < owned.size(); ++id) {
    if (owned[id] <= 0) {
      continue;
    }
    const auto *outfit =
        state.scenario.Outfit(static_cast<std::int16_t>(id + 0x80));
    if (outfit) {
      check_mods(*outfit);
    }
  }
  return caps;
}

// ---------------------------------------------------------------------------
// Ship_IsShipCloakVisibilityThresholdActive (0x0046c7a0).
// ---------------------------------------------------------------------------
bool NovaTargeting_ShipAtCloakVisibilityThreshold(const Ship &ship) {
  constexpr float kEnteringCloakThreshold =
      24.0F; // g_cloak_visibility_enter_threshold
  constexpr float kClearingCloakThreshold =
      8.0F; // g_cloak_visibility_clear_threshold
  constexpr float kBaselineThreshold =
      16.0F; // g_cloak_visibility_baseline_threshold
  const float progress = ship.cloak_fade_progress;
  if (progress > kEnteringCloakThreshold && ship.cloak_transition_latch >= 0) {
    return true;
  }
  if (progress > kClearingCloakThreshold && ship.cloak_transition_latch < 0) {
    return true;
  }
  return progress > kBaselineThreshold;
}

// ---------------------------------------------------------------------------
// Ship_IsShipEligibleForDistressCall (0x0040f6d0).
// ---------------------------------------------------------------------------
bool NovaTargeting_IsShipEligibleForDistressCall(const GameState &state,
                                                 const Ship &ship) {
  if (!ship.is_active) {
    return false;
  }
  // Not coasting through a reversal (the original requires ai_maneuver_timer_ms
  // <= 0.0; FLOAT_00575000 = 0.0).
  if (ship.ai_maneuver_timer_ms > 0.0F) {
    return false;
  }
  if (NovaAiShip_IsDisabled(state, ship)) {
    return false;
  }
  const std::int16_t target = ship.primary_target_ship_slot;
  if (target < 0) {
    return false;
  }
  // Retreat/disengage state set the original excludes.
  const std::int16_t st = ship.ai_state_code;
  if (st == 7 || st == 9 || st == 0xf || st == 10 || st == 0xb || st == 5 ||
      st == 0xc || st == 0x12) {
    return false;
  }
  // Primary target is the player, or a ship that itself targets the player.
  if (target == 0) {
    return true;
  }
  if (target < static_cast<std::int16_t>(GameState::kMaxShips) &&
      state.ShipAt(static_cast<std::size_t>(target)).squad_leader_ship_slot ==
          0) {
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Ship_IsShipAcquirableAsTarget (0x0040faa0).
// ---------------------------------------------------------------------------
bool NovaTargeting_IsShipAcquirableAsTarget(const GameState &state,
                                            const Ship &candidate,
                                            const Ship &acquirer) {
  if (!acquirer.is_active || NovaAiShip_IsDisabled(state, acquirer)) {
    return false;
  }
  const std::int16_t candidate_id = candidate.ship_instance_id;
  if (acquirer.squad_leader_ship_slot == candidate_id) {
    return false; // already locked onto the candidate
  }
  if (acquirer.ship_instance_id == 0) {
    // Player branch: government policy flag 0 (the aggro gate) or the
    // candidate is eligible for a distress call.
    if (NovaGovernment_GetPolicyFlag(
            state.scenario, candidate.faction_or_government_id, 0)) {
      return true;
    }
    return NovaTargeting_IsShipEligibleForDistressCall(state, candidate);
  }
  // NPC branch: the candidate is the acquirer's primary target (and the
  // acquirer is in an attacking state), or another active ship targets the
  // candidate while the acquirer's primary target is that ship.
  const std::int16_t st = acquirer.ai_state_code;
  const bool state_ok = st != 7 && st != 9 && st != 0xf && st != 10 &&
                        st != 0xb && st != 5 && st != 0xc && st != 0x12;
  if (acquirer.primary_target_ship_slot == candidate_id && state_ok) {
    return true;
  }
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &other = state.ShipAt(slot);
    if (!other.is_active || other.squad_leader_ship_slot != candidate_id) {
      continue;
    }
    if (acquirer.primary_target_ship_slot != static_cast<std::int16_t>(slot)) {
      continue;
    }
    if (slot == static_cast<std::size_t>(acquirer.ship_instance_id) ||
        !state_ok) {
      continue;
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Player target cycling (0x00461bd0 / 0x00461f60).
// ---------------------------------------------------------------------------
// Per-ship "combat relevance" flag (the acStack_90 markability table built in
// the first loop of Ship_FindNextPlayerCycleTarget). A ship is relevant when
// it targets the player directly (squad_leader_ship_slot == 0) or targets a
// ship that itself targets the player, provided it is not a mission-fleet
// escort. The mission-fleet-escort arm (acStack_50: fleet def active byte != 0
// and escort flag short == 1) is not modelled because mission-fleet defs are
// not reconstructed yet (TODO(decomp)); it is always false here.
[[nodiscard]] bool ShipIsCycleRelevant(const GameState &state,
                                       const Ship &ship) {
  if (ship.squad_leader_ship_slot == 0) {
    return true; // directly targeting the player
  }
  const std::int16_t target = ship.squad_leader_ship_slot;
  if (target == -1 || ship.defense_fleet_home_stellar_id != -1 ||
      !state.SlotInRange(static_cast<std::size_t>(target))) {
    return false;
  }
  return state.ShipAt(static_cast<std::size_t>(target))
             .squad_leader_ship_slot == 0;
}

// Shared candidate test for the cycle search loops. Mirrors the filter chain
// of Ship_FindNextPlayerCycleTarget: active, not destroyed, cloak-visibility
// gate (cloak scanner or combat-relevant while the include-combat modifier is
// held), same system, not AI state 0x15, untargetable class gate (scanner
// outfit), and the relevance-vs-modifier equality (`relevant == include_combat`
// -- no modifier cycles non-relevant ships, the modifier cycles relevant
// ones).
[[nodiscard]] bool
ShipIsCycleEligible(const GameState &state,
                    std::int16_t slot,
                    std::int16_t system_id,
                    bool include_combat,
                    const PlayerScannerCapabilities &scanner,
                    const std::array<bool, GameState::kMaxShips> &relevant) {
  const Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
  if (!ship.is_active || NovaAiShip_IsDestroyed(ship)) {
    return false;
  }
  const bool is_relevant = relevant[static_cast<std::size_t>(slot)];
  if (NovaTargeting_ShipAtCloakVisibilityThreshold(ship) &&
      !scanner.can_target_cloaked && !(is_relevant && include_combat)) {
    return false;
  }
  if (ship.current_system_id != system_id || ship.ai_state_code == 0x15) {
    return false;
  }
  // flags_secondary bit 2 = "can't be targeted"; the untargetable-scanner
  // outfit lifts it. A missing class entry falls through as targetable (the
  // original deactivates out-of-range class ships before targeting runs).
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls != nullptr && (cls->flags_secondary & 4U) != 0 &&
      !scanner.can_target_untargetable) {
    return false;
  }
  return is_relevant == include_combat;
}

std::int16_t NovaTargeting_FindNextPlayerCycleTarget(const GameState &state,
                                                     std::int16_t current_slot,
                                                     std::int16_t system_id,
                                                     bool include_combat) {
  const PlayerScannerCapabilities scanner = ScannerCapabilities(state);
  std::array<bool, GameState::kMaxShips> relevant{};
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    relevant[slot] = ShipIsCycleRelevant(state, state.ShipAt(slot));
  }
  const auto eligible = [&](std::int16_t slot) {
    return ShipIsCycleEligible(
        state, slot, system_id, include_combat, scanner, relevant);
  };
  if (current_slot == -1) {
    for (std::int16_t slot = 1;
         slot < static_cast<std::int16_t>(GameState::kMaxShips);
         ++slot) {
      if (eligible(slot)) {
        return slot;
      }
    }
  } else {
    for (std::int16_t slot = static_cast<std::int16_t>(current_slot + 1);
         slot < static_cast<std::int16_t>(GameState::kMaxShips);
         ++slot) {
      if (eligible(slot)) {
        return slot;
      }
    }
  }
  return current_slot;
}

std::int16_t
NovaTargeting_FindPreviousPlayerCycleTarget(const GameState &state,
                                            std::int16_t current_slot,
                                            std::int16_t system_id,
                                            bool include_combat) {
  const PlayerScannerCapabilities scanner = ScannerCapabilities(state);
  std::array<bool, GameState::kMaxShips> relevant{};
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    relevant[slot] = ShipIsCycleRelevant(state, state.ShipAt(slot));
  }
  const auto eligible = [&](std::int16_t slot) {
    return ShipIsCycleEligible(
        state, slot, system_id, include_combat, scanner, relevant);
  };
  if (current_slot == -1) {
    for (std::int16_t slot =
             static_cast<std::int16_t>(GameState::kMaxShips - 1);
         slot >= 1;
         --slot) {
      if (eligible(slot)) {
        return slot;
      }
    }
  } else {
    for (std::int16_t slot = static_cast<std::int16_t>(current_slot - 1);
         slot >= 1;
         --slot) {
      if (eligible(slot)) {
        return slot;
      }
    }
  }
  return current_slot;
}

// ---------------------------------------------------------------------------
// Ship_SelectNearestEngagedTarget (0x00462850) / _HostileCombatTarget
// (0x00462bd0).
// ---------------------------------------------------------------------------
// Shared "nearest target" candidate core for the two player scans: active,
// not destroyed, not disabled (hostile scan only), visible through the
// cloak gate (or cloak scanner), in the player's system, not in AI state 0x15
// (engaged scan only), not class-untargetable (or scanner), and NOT already
// locked onto the player (squad_leader_ship_slot != 0).
[[nodiscard]] bool
ShipIsNearestScanEligible(const GameState &state,
                          const Ship &ship,
                          const PlayerScannerCapabilities &scanner,
                          bool require_not_fire_restricted,
                          bool exclude_state_15) {
  if (!ship.is_active || NovaAiShip_IsDestroyed(ship) ||
      ship.squad_leader_ship_slot == 0 ||
      ship.current_system_id != state.player.current_system_id) {
    return false;
  }
  if (require_not_fire_restricted && NovaAiShip_IsDisabled(state, ship)) {
    return false;
  }
  if (NovaTargeting_ShipAtCloakVisibilityThreshold(ship) &&
      !scanner.can_target_cloaked) {
    return false;
  }
  if (exclude_state_15 && ship.ai_state_code == 0x15) {
    return false;
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls != nullptr && (cls->flags_secondary & 4U) != 0 &&
      !scanner.can_target_untargetable) {
    return false;
  }
  return true;
}

std::int16_t NovaTargeting_SelectNearestEngagedTarget(const GameState &state) {
  const PlayerScannerCapabilities scanner = ScannerCapabilities(state);
  std::int16_t best = -1;
  float best_dist_sq = -1.0F;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (!ShipIsNearestScanEligible(state,
                                   ship,
                                   scanner,
                                   /*require_not_fire_restricted=*/false,
                                   /*exclude_state_15=*/true)) {
      continue;
    }
    const float dx = state.player.pos_x - ship.pos_x;
    const float dy = state.player.pos_y - ship.pos_y;
    const float dist_sq = dx * dx + dy * dy;
    if (best == -1 || dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = static_cast<std::int16_t>(slot);
    }
  }
  return best;
}

std::int16_t
NovaTargeting_SelectNearestHostileCombatTarget(const GameState &state) {
  const PlayerScannerCapabilities scanner = ScannerCapabilities(state);
  std::int16_t best = -1;
  float best_dist_sq = -1.0F;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (!ShipIsNearestScanEligible(state,
                                   ship,
                                   scanner,
                                   /*require_not_fire_restricted=*/true,
                                   /*exclude_state_15=*/false)) {
      continue;
    }
    // Hostile = distress-eligible, or locked on its primary target in AI
    // state 4 while that primary is an active ship targeting the player
    // (Ship_IsShipLockedOnAttackerInAiState0x04 0x004102b0).
    bool hostile = NovaTargeting_IsShipEligibleForDistressCall(state, ship);
    if (!hostile) {
      const std::int16_t t = ship.primary_target_ship_slot;
      if (t >= 0 && t < static_cast<std::int16_t>(GameState::kMaxShips)) {
        const Ship &target = state.ShipAt(static_cast<std::size_t>(t));
        if (target.is_active && target.squad_leader_ship_slot == 0 &&
            ship.ai_state_code == 4) {
          hostile = true;
        }
      }
    }
    if (!hostile) {
      continue;
    }
    const float dx = state.player.pos_x - ship.pos_x;
    const float dy = state.player.pos_y - ship.pos_y;
    const float dist_sq = dx * dx + dy * dy;
    if (best == -1 || dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = static_cast<std::int16_t>(slot);
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// Ghidra 0x0046E3C0 Stellar_IsStellarActive.
// ---------------------------------------------------------------------------
// The original reads the StellarDef's strength capacity (+0x40), its live
// strength (+0x3c, negative = destroyed) and its engagement access counter
// (+0x47c). Our Stellar carries the same state as strength_capacity (the
// +0x40 Bible Strength capacity) and strength (the live +0x3c value decremented
// by planet-type weapons); see scenario_data.
bool NovaTargeting_IsStellarActive(const Stellar &st) {
  return st.strength_capacity > 0 && (st.strength < 0 || st.engage_access > 0);
}

// Ghidra Stellar_UpdateStellarSprites (0x0042cd10) zone selection.
std::int16_t NovaTargeting_StellarSpriteLinkId(const Stellar &st) {
  if (NovaTargeting_IsStellarActive(st) && st.link_b_id >= 0) {
    return st.link_b_id;
  }
  return st.link_a_id;
}

// ---------------------------------------------------------------------------
// Ghidra 0x0046E3F0 Stellar_StellarTargetsSpriteSetActive.
// ---------------------------------------------------------------------------
// travel_flags bit 1 is the "has control bit" marker; bit 0x80 is the engaged
// flag. The stellar is target-active when the control bit is set and sprite
// activity agrees with the engaged flag (both on, or both off).
bool NovaTargeting_StellarTargetsSpriteSetActive(const Stellar &st) {
  if ((st.flags & 1U) == 0U) {
    return false;
  }
  const bool active = NovaTargeting_IsStellarActive(st);
  const bool engaged = (st.flags & 0x80U) != 0U;
  return active == engaged;
}

// ---------------------------------------------------------------------------
// Ghidra 0x0046E440 Stellar_IsStellarUsableForTravel.
// ---------------------------------------------------------------------------
bool NovaTargeting_IsStellarUsableForTravel(const Stellar &st) {
  return NovaTargeting_StellarTargetsSpriteSetActive(st) &&
         (st.availability_flags & 0x3000U) == 0U;
}

// ---------------------------------------------------------------------------
// Ghidra 0x00465610 Stellar_ComputeTravelRangeSq.
// ---------------------------------------------------------------------------
// Base no-jump radius 1000; each owned outfit of ModType 23 (hyperspace
// distance modifier) adds `mod_val * owned_count` to the radius; the result is
// clamped >= 0 and squared. The original gates the whole computation on the
// player's primary ship instance (ShipState +0x86 == 0); the reimplementation
// is always called for the player ship, so that gate is satisfied.
float NovaTargeting_ComputeTravelRangeSq(const GameState &state) {
  constexpr int kBaseRadius = 1000;
  constexpr std::int16_t kHyperspaceDistanceMod = 0x17; // ModType 23
  int radius = kBaseRadius;

  const auto &inv = state.inventory.outfit_owned_count;
  for (std::size_t id = 0; id < inv.size(); ++id) {
    const std::int16_t owned = inv[id];
    if (owned <= 0) {
      continue;
    }
    // The outfit whose owned count sits at zero-based index `id` has resource
    // id id + 0x80 (the 0x200 g_outfit_owned_count array is that offset space).
    const auto *outfit =
        state.scenario.Outfit(static_cast<std::int16_t>(id + 0x80));
    if (!outfit) {
      continue;
    }
    // Check the primary + 3 alternate mod slots (loop of 4 in the original).
    if (outfit->mod_type == kHyperspaceDistanceMod) {
      radius += outfit->mod_val * owned;
    }
    for (std::size_t alt = 0; alt < outfit->alt_mod_types.size(); ++alt) {
      if (outfit->alt_mod_types[alt] == kHyperspaceDistanceMod) {
        radius += outfit->alt_mod_vals[alt] * owned;
      }
    }
  }

  if (radius < 0) {
    radius = 0;
  }
  return static_cast<float>(radius) * static_cast<float>(radius);
}

// ---------------------------------------------------------------------------
// Ghidra 0x0040CD80 Stellar_IsStellarAdjacentToCurrentSystem.
// ---------------------------------------------------------------------------
// Counts the system's populated nav entries; if there are any, true only when
// `stellar_id` appears among them. Preserves the original's degenerate "no nav
// entries => adjacent" behaviour.
bool NovaTargeting_IsStellarAdjacentToSystem(const System &sys,
                                             std::int16_t stellar_id) {
  std::size_t populated = 0;
  for (const auto nav : sys.nav_defs) {
    if (nav != -1) {
      ++populated;
    }
  }
  if (populated == 0) {
    return true;
  }
  for (const auto nav : sys.nav_defs) {
    if (nav == stellar_id && stellar_id != -1) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Ghidra 0x0046E790 System_FindSystemContainingStellar.
// ---------------------------------------------------------------------------
// Returns the zero-based index of the first system (visible systems preferred,
// then all systems) whose nav list contains `stellar_id`, else -1. `stellar_id`
// is a resource id (>= 0x80) rather than a zero-based id because that is how
// the nav lists store them.
std::int16_t
NovaTargeting_FindSystemContainingStellar(const ScenarioData &scenario,
                                          std::int16_t stellar_id) {
  const auto scan = [&](bool require_visible) -> std::int16_t {
    for (std::size_t idx = 0; idx < scenario.systems.size(); ++idx) {
      const System &sys = scenario.systems[idx];
      if (require_visible && !sys.is_visible) {
        continue;
      }
      for (const auto nav : sys.nav_defs) {
        if (nav == stellar_id && nav != -1) {
          return static_cast<std::int16_t>(idx);
        }
      }
    }
    return -1;
  };

  const std::int16_t visible = scan(/*require_visible=*/true);
  if (visible >= 0) {
    return visible;
  }
  return scan(/*require_visible=*/false);
}

// ---------------------------------------------------------------------------
// Ghidra 0x00432470 System_UpdateSystemAndStellarDisplayState (scope 3).
// ---------------------------------------------------------------------------
// For the player's current system, re-derive each stellar's owning system_id
// and is_available / hazard_marker. A stellar whose system is unset or invalid
// is re-homed to the current system when it appears in the current nav list.
// The sprite-set resource bookkeeping (link_a/link_b allocation) is left to the
// renderer/view.
void NovaTargeting_UpdateStellarAvailability(GameState &state) {
  const std::int16_t current_sys =
      static_cast<std::int16_t>(state.player.current_system_id);
  const auto *cur =
      state.scenario.System(static_cast<std::int16_t>(current_sys + 0x80));
  if (!cur) {
    return;
  }
  // Home every loaded system's nav stellars first (the membership claim of
  // NovaResources_EvaluateAvailability 0x00448090, first system wins; the
  // original's is_visible/has_explored_flag gates are load-time-true there,
  // so every loaded syst claims its stellars regardless of visitation). This
  // must not be gated on the discovery fog: the starmap ring colours grade
  // far systems' nav stellars through is_available.
  for (std::size_t sys_idx = 0; sys_idx < state.scenario.systems.size();
       ++sys_idx) {
    const System &system = state.scenario.systems[sys_idx];
    for (const std::int16_t nav : system.nav_defs) {
      if (nav < 0x80) {
        continue;
      }
      const std::size_t stellar_idx = static_cast<std::size_t>(nav - 0x80);
      if (stellar_idx >= state.scenario.stellars.size()) {
        continue;
      }
      Stellar &st = state.scenario.stellars[stellar_idx];
      if (st.system_id < 0 ||
          st.system_id >=
              static_cast<std::int16_t>(state.scenario.systems.size())) {
        st.system_id = static_cast<std::int16_t>(sys_idx);
      }
    }
  }
  // Mark available every stellar owned by a loaded, NCB-visible system
  // (0x00432470 scope 3; the owning system's is_visible is a loaded/NCB flag,
  // not the discovery fog).
  for (std::size_t idx = 0; idx < state.scenario.stellars.size(); ++idx) {
    Stellar &st = state.scenario.stellars[idx];
    st.is_available = false;
    st.hazard_marker = false;

    // A stellar that names an out-of-range system may be re-homed when it sits
    // in the current system's nav list (the original fixes missing system ids
    // to the current system by scanning the current nav list).
    if (st.system_id < 0 || st.system_id >= 0x800) {
      if (NovaTargeting_IsStellarAdjacentToSystem(*cur, idx + 0x80)) {
        st.system_id = current_sys;
      }
    }

    if ((st.system_id >= 0 && st.system_id < 0x800) &&
        state.scenario.systems.size() >
            static_cast<std::size_t>(st.system_id) &&
        state.scenario.systems[static_cast<std::size_t>(st.system_id)]
            .is_visible) {
      st.is_available = true;
      if ((st.availability_flags & 0x20U) != 0U) {
        st.hazard_marker = true;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Per-frame player target validation (0x0044aa70 prologue).
// ---------------------------------------------------------------------------
// Ship_HandlePlayerShipCore's opening block: the player's primary ship target
// is a persistent selection, not a per-frame scan, and it survives the
// target's own movement (including a jump into another system) -- it only
// drops when the target is inactive, destroyed, entering hyperspace (AI state
// 0x15), or cloaked past the visibility threshold while it pursues its own AI
// target (that last gate lifted by the cloak-scanner outfit). The stellar
// target has no per-frame validation at all; it is cleared on system arrival
// (same prologue function's transition block) and by the clear-target command.
// Ghidra Ship_HandlePlayerShipCore 0x0044AA70 synthetic CFG:
// player-target validation 0x0044ADA1 -> 0x0044AEFB.
void NovaTargeting_ValidatePlayerTarget(GameState &state) {
  const std::int16_t target_slot = state.player.primary_target_ship_slot;
  if (target_slot < 1 ||
      target_slot >= static_cast<std::int16_t>(GameState::kMaxShips)) {
    return;
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  bool invalid = !target.is_active || NovaAiShip_IsDestroyed(target) ||
                 target.ai_state_code == 0x15;
  if (!invalid && NovaTargeting_ShipAtCloakVisibilityThreshold(target) &&
      target.squad_leader_ship_slot != 0) {
    // The cloak drop only applies while the target is busy with its own AI
    // target (not when it hunts the player); the cloak-scanner outfit lifts it.
    invalid = !ScannerCapabilities(state).can_target_cloaked;
  }
  if (invalid) {
    state.player.primary_target_ship_slot = -1;
    state.ship_reticle_pulse = 0.0F;
  }
}

// ---------------------------------------------------------------------------
// Ghidra 0x00462db0 Stellar_FindNearestAvailableTravelStellar.
// ---------------------------------------------------------------------------
std::int16_t
NovaTargeting_FindNearestAvailableTravelStellar(const GameState &state) {
  const auto *cur = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (!cur) {
    return -1;
  }
  std::int16_t best = -1;
  float best_dist_sq = 1e12F;
  for (const auto nav : cur->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const auto *st = state.scenario.Stellar(nav);
    if (!st || !st->is_available ||
        st->system_id != state.player.current_system_id ||
        (st->flags & 1U) == 0U) {
      continue;
    }
    const float dx = state.player.pos_x - static_cast<float>(st->pos_x);
    const float dy = state.player.pos_y - static_cast<float>(st->pos_y);
    const float dist_sq = dx * dx + dy * dy;
    // Restricted 0x3000 lanes need the no-jump-radius proximity test.
    if ((st->availability_flags & 0x3000U) != 0U &&
        dist_sq > NovaTargeting_ComputeTravelRangeSq(state)) {
      continue;
    }
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = nav;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// Stellar-target cycling. The original has no per-stellar cycle command (its
// cycle-travel-target command rotates adjacent *systems*); this clean-room
// Tab binding follows the ship-cycle semantics: the search does not wrap, and
// advancing past either end clears the selection back to "none".
// ---------------------------------------------------------------------------
bool NovaTargeting_CyclePlayerStellarTarget(GameState &state, bool forward) {
  const auto *cur = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (!cur) {
    return false;
  }
  std::array<std::int16_t, 16> candidates{};
  std::size_t count = 0;
  for (const auto sid : cur->nav_defs) {
    const auto *st = state.scenario.Stellar(sid);
    if (sid >= 0x80 && st && st->is_available &&
        st->system_id == state.player.current_system_id &&
        (st->flags & 1U) != 0U) {
      if ((st->availability_flags & 0x3000U) != 0U) {
        const float dx = state.player.pos_x - static_cast<float>(st->pos_x);
        const float dy = state.player.pos_y - static_cast<float>(st->pos_y);
        if (dx * dx + dy * dy > NovaTargeting_ComputeTravelRangeSq(state)) {
          continue;
        }
      }
      candidates[count++] = sid;
    }
  }
  if (count == 0) {
    state.travel.selected_stellar_id = -1;
    state.travel.selected_stellar_is_manual = false;
    return false;
  }
  // No wrap: advancing past the last candidate (or backwards past the first)
  // clears the selection to "none", like the ship cycle's caller clearing on
  // a no-op. Cycling from "none" picks the first (forward) or last
  // (backwards) candidate.
  const std::size_t none = count;
  std::size_t current = none;
  for (std::size_t i = 0; i < count; ++i) {
    if (candidates[i] == state.travel.selected_stellar_id) {
      current = i;
      break;
    }
  }
  std::size_t next;
  if (current == none) {
    // Selection is "none" (or a stale id no longer eligible): start fresh.
    next = forward ? 0 : count - 1;
  } else if (forward) {
    if (current + 1 >= count) {
      state.travel.selected_stellar_id = -1;
      state.travel.selected_stellar_is_manual = false;
      return false;
    }
    next = current + 1;
  } else {
    if (current == 0) {
      state.travel.selected_stellar_id = -1;
      state.travel.selected_stellar_is_manual = false;
      return false;
    }
    next = current - 1;
  }
  state.travel.selected_stellar_id = candidates[next];
  state.travel.selected_stellar_is_manual = true;
  // Stellar navigation and its reticle use the player's mode-2 channel.
  // Cross-system arrival resets the channel to -1, so a first post-jump Tab
  // selection must explicitly re-arm it.
  state.player.travel_transfer_mode = 2;
  return true;
}

bool NovaTargeting_CanOpenTravelDestinationInteraction(const GameState &state) {
  const std::int16_t sid = state.travel.selected_stellar_id;
  if (sid < 0x80) {
    return false;
  }
  const auto *st = state.scenario.Stellar(sid);
  if (!st || !st->is_available ||
      st->system_id != state.player.current_system_id ||
      (st->availability_flags & 0x3000U) != 0U || (st->flags & 0x20U) != 0U ||
      !NovaTargeting_StellarTargetsSpriteSetActive(*st)) {
    return false;
  }
  return true;
}

} // namespace game
