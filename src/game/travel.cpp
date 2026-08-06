#include "travel.hpp"

#include "../log.hpp"
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
  // when it observes just_completed.
  t.travel_slot = -1;
  t.engaged_stellar_id = -1;
  t.destination_system_id = -1;
}

} // namespace

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
    if (!st) {
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
  const ShipClass *cls = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  // The original gates on the ship *class* fuel capacity being at least one
  // jump (a class that physically cannot carry a jump's fuel can't jump).
  const float class_fuel_capacity =
      cls ? static_cast<float>(cls->base_fuel) : 0.0F;
  if (class_fuel_capacity < kJumpFuelCost) {
    return false;
  }
  // And enough fuel on hand to pay for the jump (the completion decrements
  // kJumpFuelCost out of fuel_points).
  if (state.player.fuel_points < kJumpFuelCost) {
    return false;
  }
  // TODO(decomp): the original also blocks while velocity-matched to another
  // ship and under certain mission-ship flags; no NPC fleet / missions yet.
  return true;
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

  // (a) Idle: wait for the travel key near an available travel point.
  if (!travel_input || !NovaTravel_CanStartJump(state)) {
    return;
  }
  const int slot = NovaTravel_FindNearestTravelPoint(state);
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
  t.jump_countdown_ticks = kJumpSequenceTicks;
  t.engaging = true;
  NovaLog::Debug(
      "hyperspace jump engaged: from stellar {} (slot {}) to system {}",
      stellar_id,
      slot,
      dest_zero_based);
}

} // namespace game
