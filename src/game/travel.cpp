#include "travel.hpp"

#include "../log.hpp"
#include "hud_overlay.hpp"
#include "outfit.hpp"

#include <algorithm>
#include <cmath>

namespace game {
namespace {

// Jump-sequence length in 1/60s ticks. The original hyperspace flight is
// driven by Stellar_GetJumpSequenceDurationMs / ai_station_hold_timer
// (TODO(decomp) exact duration); ~1.5s is a reasonable countdown stand-in.
constexpr int kJumpSequenceTicks = 90;

// The jump-engagement proximity: squared distance (px^2) required for a
// restricted travel stellar (StellarDef.availability_flags & 0x3000). Mirrors
// _DAT_00575750 in Stellar_FindNearestAvailableTravelStellar.
constexpr float kRestrictedTravelRangeSq = 1000.0F * 1000.0F;

// The initial "nearest so far" squared-distance sentinel in the search
// (larger than any real distance). Mirrors _DAT_00575748.
constexpr float kMaxDistanceSq = 1e12F;

// The player's current system as a resource id (zero-based current_system_id
// + 0x80), following the HUD lookup convention.
std::int16_t CurrentSystemResource(const GameState &state) {
  return static_cast<std::int16_t>(state.player.current_system_id + 0x80);
}

// Completes an engaged jump: the ship lands in the destination system,
// mirrors the jump-complete block in Ship_HandlePlayerShipCore (fuel burn,
// system change, reposition, shield/armor refill) without the fleet warp-sync,
// mission or rendering side effects.
void CompleteJump(GameState &state) {
  TravelState &t = state.travel;

  // Burn the jump's fuel.
  state.player.fuel_points =
      std::max(0.0F, state.player.fuel_points - kJumpFuelCost);

  // Change system and clear the engaged destination.
  state.player.current_system_id = t.destination_system_id;
  state.player.pos_x = 0.0F;
  state.player.pos_y = 60.0F; // spawn above the new system's origin
  state.player.vel_x = 0.0F;
  state.player.vel_y = 0.0F;
  state.player.speed = 0.0F;

  // Mark the destination system (and its linked neighbours) explored/visible
  // so the starmap shows progression. This mirrors the discovery flood
  // (System_FloodDiscoverAdjacentSystems) that runs on system entry in the
  // original: reaching a system reveals it and its immediate neighbourhood.
  NovaTravel_MarkSystemDiscovered(state, t.destination_system_id);

  // Refill shields/armor from the effective (outfit-derived) maximums.
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  state.player.shield_points = eff.max_shield_points;
  state.player.armor_points = eff.max_armor_points;
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  NovaLog::Info("hyperspace jump landed: system id {} (resource {}) after "
                "burning {} fuel; shields/armor refilled",
                state.player.current_system_id,
                static_cast<int>(CurrentSystemResource(state)),
                static_cast<int>(kJumpFuelCost));

  // Clear the travel engagement; the spaceflight loop re-spawns the starfield
  // when it observes just_completed. The plotted starmap destination is also
  // consumed by the jump (a fresh plot is needed for the next jump).
  t.starmap_destination_system_id = -1;
  t.travel_slot = -1;
  t.engaged_stellar_id = -1;
  t.destination_system_id = -1;
}

// ---------------------------------------------------------------------------
// Starmap plot: resolve a galaxy-map destination to a travel slot.
// ---------------------------------------------------------------------------
// Maps a destination zero-based system id to the slot (0..15) in the current
// system whose linked destination matches (System.links[slot] alongside
// System.nav_defs[slot]), or -1 when there is no direct link.
int FindLinkedTravelSlot(const GameState &state,
                         std::int16_t destination_zero_based) {
  const System *sys = state.scenario.System(CurrentSystemResource(state));
  if (!sys) {
    return -1;
  }
  const std::int16_t dest_resource =
      static_cast<std::int16_t>(destination_zero_based + 0x80);
  for (std::size_t slot = 0; slot < sys->links.size(); ++slot) {
    if (sys->links[slot] == dest_resource) {
      return static_cast<int>(slot);
    }
  }
  return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// Plots a galaxy-map destination as the next jump target.
// ---------------------------------------------------------------------------
bool NovaTravel_PlotStarmapDestination(GameState &state,
                                       std::int16_t destination_zero_based) {
  TravelState &t = state.travel;
  t.starmap_destination_system_id = destination_zero_based;
  if (destination_zero_based < 0 ||
      destination_zero_based == state.player.current_system_id) {
    return false;
  }
  const int slot = FindLinkedTravelSlot(state, destination_zero_based);
  if (slot < 0) {
    // No direct single jump reaches it. Keep the plot recorded so the HUD can
    // show the intention, but arm nothing -- 'j' falls back to the nearest
    // travel point.
    return false;
  }
  const System *sys = state.scenario.System(CurrentSystemResource(state));
  if (!sys) {
    return false;
  }
  // The destination is resolved purely from the hyperlink (System.links[slot]);
  // a NavDef departure-point stellar at the same slot is NOT required -- the
  // scenario data does not pair them 1:1 (e.g. Kania links to Tichel at slot 3
  // with no travel stellar there, yet 'j' still jumps Kania->Tichel). If a
  // departure-point stellar does exist, surface it as the selected target so
  // the travel reticle/HUD point at it.
  t.travel_slot = static_cast<std::int16_t>(slot);
  t.destination_system_id = destination_zero_based;
  const std::int16_t stellar_id = sys->nav_defs[static_cast<std::size_t>(slot)];
  if (stellar_id >= 0x80) {
    t.engaged_stellar_id = stellar_id;
    t.selected_stellar_id = stellar_id;
  }
  t.selected_stellar_is_manual = true;
  NovaLog::Info("plotted starmap jump to system {} (hyperlink slot {})",
                destination_zero_based,
                slot);
  return true;
}

// ---------------------------------------------------------------------------
// Mirrors Stellar_FindNearestAvailableTravelStellar (0x00462db0).
// ---------------------------------------------------------------------------
int NovaTravel_FindNearestTravelPoint(const GameState &state) {
  const System *sys = state.scenario.System(CurrentSystemResource(state));
  if (!sys) {
    return -1;
  }
  int best_slot = -1;
  float best_dist_sq = kMaxDistanceSq;
  for (std::size_t slot = 0; slot < sys->nav_defs.size(); ++slot) {
    const std::int16_t stellar_id = sys->nav_defs[slot];
    if (stellar_id < 0x80) {
      continue; // no travel point in this slot
    }
    const Stellar *st = state.scenario.Stellar(stellar_id);
    if (!st || !st->is_available || (st->flags & 1U) == 0U) {
      continue;
    }
    // Distance from the ship to the travel point (world coords; +y is down).
    const float dx = state.player.pos_x - static_cast<float>(st->pos_x);
    const float dy = state.player.pos_y - static_cast<float>(st->pos_y);
    const float dist_sq = dx * dx + dy * dy;
    // Restricted travel stellar: only usable within the no-jump radius.
    const bool restricted = (st->availability_flags & 0x3000) != 0;
    if (restricted && dist_sq >= kRestrictedTravelRangeSq) {
      continue;
    }
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best_slot = static_cast<int>(slot);
    }
  }
  return best_slot;
}

// ---------------------------------------------------------------------------
// Mirrors Stellar_CanShipInitiateJumpSequence (0x00415b80).
// ---------------------------------------------------------------------------
bool NovaTravel_CanStartJump(const GameState &state) {
  // Player-side convenience wrapper around the faithful per-ship gate.
  return NovaTravel_CanShipInitiateJumpSequence(state, state.player);
}

// Mirrors Stellar_CanShipInitiateJumpSequence (0x00415b80) for an arbitrary
// (NPC) ship. Gates on the ship's OWN class fuel capacity being at least one
// jump (kJumpFuelCost) -- NOT the player's, which is what the old NPC AI call
// sites wrongly used. The original further blocks while the ship is locked to
// another ship's velocity match (ShipState.velocity_match_target_ship_slot !=
// -1 and != own id) and while a mission ship lacks fuel; those need the
// velocity-match / mission systems and are deferred (TODO(decomp)).
//
// It additionally gates on the CURRENT fuel amount: Stellar_HandlePlayerShip-
// Core' jump block refuses to (re)enter the hyperspace sequence while
// `ship->fuel_points < FLOAT_kJumpFuelCost` (0x005755a4 == kJumpFuelCost ==
// 100), showing the "not enough fuel to take off" denial overlay. A ship with
// no fuel in the tank cannot engage a jump even though its class can hold a
// jump's worth of fuel.
bool NovaTravel_CanShipInitiateJumpSequence(const GameState &state,
                                            const Ship &ship) {
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const float class_fuel_capacity =
      cls ? static_cast<float>(cls->base_fuel) : 0.0F;
  return class_fuel_capacity >= kJumpFuelCost &&
         ship.fuel_points >= kJumpFuelCost;
}

// ---------------------------------------------------------------------------
// Discovery flood.
// ---------------------------------------------------------------------------
void NovaTravel_MarkSystemDiscovered(GameState &state,
                                     std::int16_t zero_based_system_id) {
  const std::size_t count = state.scenario.systems.size();
  const auto mark = [&](std::int16_t zero_based) {
    if (zero_based < 0 || static_cast<std::size_t>(zero_based) >= count) {
      return;
    }
    const std::size_t idx = static_cast<std::size_t>(zero_based);
    auto &sys = state.scenario.systems[idx];
    // Keep the scenario's per-system visibility flag in sync so target/fog
    // helpers that consult is_visible stay correct (targeting.cpp forces the
    // current system visible; we extend that to neighbours reached by jump).
    sys.is_visible = true;
    sys.has_explored_flag = true;
    if (idx < state.control.explored_systems.size()) {
      state.control.explored_systems.set(idx);
    }
  };
  mark(zero_based_system_id);
  // Reveal the immediate neighbourhood too (links are stored as system
  // *resource* ids in System.links).
  const auto *sys = state.scenario.System(
      static_cast<std::int16_t>(zero_based_system_id + 0x80));
  if (!sys) {
    return;
  }
  for (const std::int16_t link : sys->links) {
    if (link >= 0x80) {
      mark(static_cast<std::int16_t>(link - 0x80));
    }
  }
}

// ---------------------------------------------------------------------------
// Cross-system jump state machine.
// ---------------------------------------------------------------------------
void NovaTravel_Tick(GameState &state, bool travel_input, float frame_time_ms) {
  TravelState &t = state.travel;
  t.just_completed = false;

  if (t.engaging) {
    // (b) Engaged: advance the countdown, then complete the jump.
    // Advance by ~1 tick per real frame (the original accumulates frame time
    // into ai_station_hold_timer). Scaling by the real frame time keeps the
    // on-screen pace independent of host frame rate.
    const float ticks_elapsed = frame_time_ms / (1000.0F / 60.0F);
    t.jump_countdown_ticks -= static_cast<int>(std::max(1.0F, ticks_elapsed));
    if (t.jump_countdown_ticks <= 0) {
      CompleteJump(state);
      t.engaging = false;
      t.jump_countdown_ticks = 0;
      t.just_completed = true;
      NovaLog::Debug("hyperspace jump sequence finished");
    }
    return;
  }

  if (t.jump_countdown_ticks > 0) {
    return; // transient; should not persist with engaging false
  }

  // (a) Idle: wait for the travel key. When a starmap plot armed a specific
  // travel slot, jump toward that plotted destination; otherwise fall back to
  // the nearest available travel point (the original's
  // Stellar_FindNearestAvailableTravelStellar behaviour via 'j').
  if (!travel_input) {
    return;
  }
  if (!NovaTravel_CanStartJump(state)) {
    // A 'j' press with an empty tank is refused with a HUD-overlay denial,
    // mirroring the original's jump block showing the "not enough fuel"
    // message rather than silently ignoring the command.
    NovaHud_ShowOverlayMessage(state,
                               "Not enough fuel to make a hyperspace jump.",
                               0xe0,
                               0xe0,
                               0xe0,
                               600U);
    return;
  }
  const int slot = (t.travel_slot >= 0 && t.starmap_destination_system_id >= 0)
                       ? static_cast<int>(t.travel_slot)
                       : NovaTravel_FindNearestTravelPoint(state);
  if (slot < 0) {
    return; // no travel point in range/available
  }
  const System *sys = state.scenario.System(CurrentSystemResource(state));
  if (!sys) {
    return;
  }
  const std::int16_t dest_resource = sys->links[static_cast<std::size_t>(slot)];
  if (dest_resource < 0x80 || dest_resource == CurrentSystemResource(state)) {
    // No valid outward link, or it points back at the current system.
    return;
  }
  const std::int16_t dest_zero_based =
      static_cast<std::int16_t>(dest_resource - 0x80);
  const std::int16_t stellar_id = sys->nav_defs[static_cast<std::size_t>(slot)];

  t.travel_slot = static_cast<std::int16_t>(slot);
  t.engaged_stellar_id = stellar_id;
  t.destination_system_id = dest_zero_based;
  t.starmap_destination_system_id = dest_zero_based;
  t.jump_countdown_ticks = kJumpSequenceTicks;
  t.engaging = true;
  NovaLog::Debug(
      "hyperspace jump engaged: from stellar {} (slot {}) to system {}",
      stellar_id,
      slot,
      dest_zero_based);
}

} // namespace game
