#include "targeting.hpp"

#include "../log.hpp"
#include "outfit.hpp"

namespace game {

// ---------------------------------------------------------------------------
// Mirrors Stellar_IsStellarActive (0x0046E3C0).
// ---------------------------------------------------------------------------
// The original reads the StellarDef's ambient-sprite population (+0x40), its
// sprite handle (+0x3c, negative = active/reserved) and its engagement access
// counter (+0x47c). Our Stellar carries the same state decoupled from SDL as
// sprite_population / sprite_handle_active / engage_access (see scenario_data).
bool NovaTargeting_IsStellarActive(const Stellar &st) {
  return st.sprite_population > 0 &&
         (st.sprite_handle_active || st.engage_access > 0);
}

// ---------------------------------------------------------------------------
// Mirrors Stellar_StellarTargetsSpriteSetActive (0x0046E3F0).
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
// Mirrors Stellar_IsStellarUsableForTravel (0x0046E440).
// ---------------------------------------------------------------------------
bool NovaTargeting_IsStellarUsableForTravel(const Stellar &st) {
  return NovaTargeting_StellarTargetsSpriteSetActive(st) &&
         (st.availability_flags & 0x3000U) == 0U;
}

// ---------------------------------------------------------------------------
// Mirrors Stellar_ComputeTravelRangeSq (0x00465610).
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
// Mirrors Stellar_IsStellarAdjacentToCurrentSystem (0x0040CD80).
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
// Mirrors System_FindSystemContainingStellar (0x0046E790).
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
// Mirrors scope (3) of System_UpdateSystemAndStellarDisplayState (0x00432470).
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
  // Home to / mark available every stellar currently owned by a visible system.
  for (std::size_t idx = 0; idx < state.scenario.stellars.size(); ++idx) {
    Stellar &st = state.scenario.stellars[idx];
    const std::int16_t resource = static_cast<std::int16_t>(idx + 0x80);
    st.is_available = false;
    st.hazard_marker = false;

    // A stellar that names an out-of-range system may be re-homed when it sits
    // in the current system's nav list (the original fixes missing system ids
    // to the current system by scanning the current nav list).
    if (st.system_id < 0 || st.system_id >= 0x800) {
      if (NovaTargeting_IsStellarAdjacentToSystem(*cur, resource)) {
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
// Landing / travel-destination selection (clean-room; built from the
// primitives).
// ---------------------------------------------------------------------------
bool NovaTargeting_IsLandableStellar(const Stellar &st) {
  // travel_flags bit 0x2 is the "landable" (non-jump) slot.
  return NovaTargeting_IsStellarUsableForTravel(st) && (st.flags & 0x2U) != 0U;
}

std::int16_t NovaTargeting_FindNearestLandableStellar(const GameState &state) {
  const auto *cur = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (!cur) {
    return -1;
  }
  const float range_sq = NovaTargeting_ComputeTravelRangeSq(state);
  // The player's world position; the stellar must be within the no-jump radius.
  const float px = state.player.pos_x;
  const float py = state.player.pos_y;

  std::int16_t best = -1;
  float best_dist_sq = 1e12F; // kMaxDistanceSq sentinel, larger than any real
  for (const auto nav : cur->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const auto *st = state.scenario.Stellar(nav);
    if (!st || !NovaTargeting_IsLandableStellar(*st)) {
      continue;
    }
    const float dx = px - static_cast<float>(st->pos_x);
    const float dy = py - static_cast<float>(st->pos_y);
    const float dist_sq = dx * dx + dy * dy;
    if (dist_sq > range_sq) {
      continue; // beyond the no-jump/engagement radius
    }
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = nav;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// Per-frame player targeting + landing interaction (clean-room, mocked).
// ---------------------------------------------------------------------------
void NovaTargeting_UpdatePlayerTarget(GameState &state) {
  const auto *cur = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  state.travel.selected_stellar_id = -1;
  if (!cur) {
    return;
  }
  const float range_sq = NovaTargeting_ComputeTravelRangeSq(state);
  const float px = state.player.pos_x;
  const float py = state.player.pos_y;

  std::int16_t best = -1;
  float best_dist_sq = 1e12F;
  for (const auto nav : cur->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const auto *st = state.scenario.Stellar(nav);
    if (!st || !NovaTargeting_IsStellarUsableForTravel(*st)) {
      continue;
    }
    const float dx = px - static_cast<float>(st->pos_x);
    const float dy = py - static_cast<float>(st->pos_y);
    const float dist_sq = dx * dx + dy * dy;
    if (dist_sq > range_sq) {
      continue;
    }
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = nav;
    }
  }
  state.travel.selected_stellar_id = best;
}

bool NovaTargeting_IsLandingAvailable(const GameState &state) {
  const std::int16_t sid = state.travel.selected_stellar_id;
  if (sid < 0x80) {
    return false;
  }
  const auto *st = state.scenario.Stellar(sid);
  if (!st || !NovaTargeting_IsLandableStellar(*st)) {
    return false;
  }
  const float range_sq = NovaTargeting_ComputeTravelRangeSq(state);
  const float dx = state.player.pos_x - static_cast<float>(st->pos_x);
  const float dy = state.player.pos_y - static_cast<float>(st->pos_y);
  return (dx * dx + dy * dy) <= range_sq;
}

bool NovaLanding_TryLand(GameState &state) {
  state.travel.landed_this_frame = false;
  const std::int16_t sid = state.travel.selected_stellar_id;
  if (!NovaTargeting_IsLandingAvailable(state)) {
    return false;
  }
  const auto *st = state.scenario.Stellar(sid);
  if (!st) {
    return false;
  }

  // Landing transition state changes (mirrors Stellar_LandOnSpob 0x00456480):
  // reposition to the stellar, zero velocity/speed, refill shields/armor, and
  // deduct the stellar's service cost (clamped to 0 credits).
  state.player.pos_x = static_cast<float>(st->pos_x);
  state.player.pos_y = static_cast<float>(st->pos_y);
  state.player.vel_x = 0.0F;
  state.player.vel_y = 0.0F;
  state.player.speed = 0.0F;
  const auto eff = Outfit_ComputePlayerEffectiveStats(state);
  state.player.shield_points = eff.max_shield_points;
  state.player.armor_points = eff.max_armor_points;
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  // Service cost (StellarDef service_cost; a positive fee for hangar/slip when
  // the stellar is not a hazard). The player pays once per landing.
  if (st->service_cost > 0 && !st->hazard_marker) {
    state.player.credits -= st->service_cost;
    if (state.player.credits < 0) {
      state.player.credits = 0;
    }
  }

  // The dock/world context that follows a real landing (shops, missions,
  // systems map, re-launch) is not reconstructed; this is a dock-and-return
  // stub that logs the visit and leaves the player in free flight.
  NovaLog::Info("LANDED [mocked] at stellar {} ({}); shields/armor refilled; "
                "credits {}",
                sid,
                st->name,
                state.player.credits);
  state.travel.landed_this_frame = true;
  return true;
}

} // namespace game
