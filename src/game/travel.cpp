#include "travel.hpp"

#include "../log.hpp"
#include "hud_overlay.hpp"
#include "outfit.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace game {
namespace {

// Slow-turn/brake hold duration (ms) before the tunnel fires. The original
// ramps ai_station_hold_timer by g_avg_frame_time_ms and damps velocity by
// g_hyperspace_slow_phase_velocity_damp (0.98) per frame while the ship turns
// onto the jump vector, firing the tunnel once it crosses
// g_hyperspace_engage_hold_ms (30 ms). The raw frame-time ramp makes that hold
// practically instantaneous; we lengthen it so the turn-and-slow reads on
// screen. (TODO(decomp): the exact cadence of the hold vs avg-frame-time.)
constexpr float kSlowTurnHoldMs = 700.0F;

// In-tunnel coast duration (ms), driven by a wall-clock stopwatch
// (flight_start_ms mirroring ai_mode_start_time_ms + NovaTime_GetTicksMs). The
// original's Stellar_GetJumpSequenceDurationMs returns ~350 ms and scales the
// flight progress by ShipClassDef.jump_duration_multiplier; we keep a fixed
// gate long enough for the streaking star-tunnel visual to read.
// (TODO(decomp): apply jump_duration_multiplier to tune per class.)
constexpr float kJumpTunnelMs = 900.0F;

// Player max speed scale: px/tick = eff.speed_raw / kMaxSpeedScale (the
// movement integrator's max_speed_px_per_tick). Used for the arrival-at-speed
// (enter the new system at top speed aimed at center).
constexpr float kMaxSpeedScale = 100.0F;

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

// Returns the player's top speed in px/tick, matching the movement
// integrator's max_speed_px_per_tick = eff.speed_raw / kMaxSpeedScale. Falls
// back to 0 when the effective stat cache is unavailable.
float PlayerMaxSpeed(const GameState &state) {
  return state.cached_stats.speed_raw / kMaxSpeedScale;
}

// Completes an engaged jump: the ship lands in the destination system,
// mirrors the jump-complete block in Ship_HandlePlayerShipCore (fuel burn,
// system change, reposition just as a radial offset, shield/armor refill)
// without the fleet warp-sync, mission or rendering side effects. The ship
// arrives a short radial offset from the new system's origin and is set moving
// at its top speed along the jump heading (aimed back at the system center) --
// the original spends the tunnel coasting toward the destination and the
// arrival handoff to normal flight begins at speed.
void CompleteJump(GameState &state) {
  TravelState &t = state.travel;

  // Burn the jump's fuel.
  state.player.fuel_points =
      std::max(0.0F, state.player.fuel_points - kJumpFuelCost);

  // Change system and clear the engaged destination.
  state.player.current_system_id = t.destination_system_id;

  // Refill shields/armor from the effective (outfit-derived) maximums. This
  // also refreshes state.cached_stats, which PlayerMaxSpeed below relies on.
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  state.player.shield_points = eff.max_shield_points;
  state.player.armor_points = eff.max_armor_points;
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  // Arrive just outside the destination system's origin and move at top speed
  // along the jump heading, matching the original's arrival at full speed into
  // the new system. The tunnel coasted the ship along jump_heading_rad toward
  // the destination, so the ship comes out a radial offset out from the origin
  // (a little far away) still at max speed, aimed back at the center. Use the
  // destination system's own origin (the arrival point) so this works even
  // when the destination's geometry differs from the departure system.
  const float arrival_speed = std::max(PlayerMaxSpeed(state), 1.0F);
  constexpr float kArrivalOffset = 120.0F; // px behind the origin
  state.player.pos_x =
      -std::sin(t.jump_heading_rad) * kArrivalOffset;
  state.player.pos_y = std::cos(t.jump_heading_rad) * kArrivalOffset;
  state.player.heading = t.jump_heading_rad;
  state.player.vel_x = std::sin(t.jump_heading_rad) * arrival_speed;
  state.player.vel_y = -std::cos(t.jump_heading_rad) * arrival_speed;
  state.player.speed = arrival_speed;

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
  t.hyperspace_mode = false;
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
// Destination-system cycling (the Backslash / command-0x60 channel).
// ---------------------------------------------------------------------------
// Mirrors the command-0x60 block in Ship_HandlePlayerShip
// (g_playerCycleTravelTargetCommandLatch): builds the set of travelable
// destination slots for the current system, then nudges the travel-slot
// selector one slot forward/backward through them (wrapping). A slot is a
// candidate when its linked destination (System.links[slot] >= 0x80) resolves
// to a real, non-self system. On a hit it sets the travel slot + destination
// on state.travel so 'j' jumps there and the HUD travel panel shows the name.
std::int16_t NovaTravel_CycleDestinationSystem(GameState &state, bool forward) {
  TravelState &t = state.travel;
  const System *sys = state.scenario.System(CurrentSystemResource(state));
  if (!sys) {
    return -1;
  }
  const std::int16_t current_res = CurrentSystemResource(state);
  const std::size_t n = state.scenario.systems.size();

  // Collect candidate slots: every link slot whose destination is a real,
  // non-self system.
  std::array<int, 16> slots{};
  std::size_t count = 0;
  for (std::size_t slot = 0; slot < sys->links.size(); ++slot) {
    const std::int16_t link = sys->links[slot];
    if (link < 0x80 || link == current_res) {
      continue; // no link, or it loops back to the current system
    }
    const std::int16_t dest = static_cast<std::int16_t>(link - 0x80);
    if (dest < 0 || static_cast<std::size_t>(dest) >= n) {
      continue; // dangling link to an out-of-range system
    }
    slots[count++] = static_cast<int>(slot);
  }
  if (count == 0) {
    t.travel_slot = -1;
    t.destination_system_id = -1;
    t.starmap_destination_system_id = -1;
    return -1; // current system has no travelable links
  }

  // Locate the current travel-slot selection within the candidate list.
  std::size_t index = count;
  for (std::size_t i = 0; i < count; ++i) {
    if (slots[i] == t.travel_slot) {
      index = i;
      break;
    }
  }
  const std::size_t next =
      index == count
          ? (forward ? 0 : count - 1)
          : (forward ? (index + 1) % count : (index + count - 1) % count);
  const int slot = slots[next];
  const std::int16_t dest = static_cast<std::int16_t>(
      sys->links[static_cast<std::size_t>(slot)] - 0x80);

  t.travel_slot = static_cast<std::int16_t>(slot);
  t.starmap_destination_system_id = dest;
  t.destination_system_id = dest;
  const std::int16_t stellar_id = sys->nav_defs[static_cast<std::size_t>(slot)];
  if (stellar_id >= 0x80) {
    t.engaged_stellar_id = stellar_id;
  }
  NovaLog::Info(
      "cycled destination system to {} (hyperlink slot {})", dest, slot);
  return dest;
}

// ---------------------------------------------------------------------------
// Cross-system jump state machine.
// ---------------------------------------------------------------------------
void NovaTravel_Tick(GameState &state, bool travel_input, float frame_time_ms) {
  TravelState &t = state.travel;
  t.just_completed = false;

  if (t.engaging) {
    // (b) Engaged: drive the visible jump phases, then complete.
    switch (t.jump_phase) {
      case TravelState::JumpPhase::kSlowTurn: {
        // Turn the hull onto the jump heading and brake to a stop, mirroring
        // the original's pre-fire hold: ai_station_hold_timer ramps by frame
        // time while both velocity axes are damped by
        // g_hyperspace_slow_phase_velocity_damp (0.98) each frame. Here we
        // turn toward the jump heading at the class turn rate (deg/tick) and
        // apply the same damper; once the hold elapses we fire the tunnel.
        Ship &player = state.player;
        const float max_turn = std::round(state.cached_stats.turn_raw * 0.1F);
        const float turn_rad =
            max_turn * (3.14159265358979323846F / 180.0F) *
            (frame_time_ms / 1000.0F / 30.0F);
        float delta = std::remainder(t.jump_heading_rad - player.heading,
                                     6.28318530717958646F);
        delta = std::clamp(delta, -turn_rad, turn_rad);
        player.heading =
            std::fmod(player.heading + delta + 6.28318530717958646F,
                      6.28318530717958646F);
        // Brake: damp both velocity axes (original g_hyperspace_slow_phase-
        // velocity_damp 0.98).
        constexpr float kBrakeDamp = 0.98F;
        player.vel_x *= kBrakeDamp;
        player.vel_y *= kBrakeDamp;
        player.speed = std::hypot(player.vel_x, player.vel_y);

        t.slow_turn_elapsed_ms += frame_time_ms;
        if (t.slow_turn_elapsed_ms >= kSlowTurnHoldMs) {
          // Fire: snap position and set the ship coasting at max speed along
          // the jump heading (the tunnel flight). Mirrors the original's fire
          // block (max speed via Ship_ComputeShipEffectiveMaxSpeed). The
          // momentary 180-deg hurl is skipped since the same frame zeroes it.
          t.jump_phase = TravelState::JumpPhase::kFlying;
          t.flying_elapsed_ms = 0.0F;
          Ship &p = state.player;
          p.engine_glow_level = 32; // glow slam to max (afterburner cap)
          p.engine_glow_intensity = 1.0F;
          const float max_speed = std::max(PlayerMaxSpeed(state), 1.0F);
          p.vel_x = std::sin(t.jump_heading_rad) * max_speed;
          p.vel_y = -std::cos(t.jump_heading_rad) * max_speed;
          p.speed = max_speed;
          NovaLog::Debug(
              "hyperspace tunnel fired: coasting at max speed along heading");
        }
        break;
      }
      case TravelState::JumpPhase::kFlying: {
        // In-tunnel coast: hold the ship at max speed along the heading while
        // the starfield streams (the tunnel visual), then complete when the
        // wall-clock stopwatch elapses the tunnel duration.
        Ship &player = state.player;
        const float max_speed = std::max(PlayerMaxSpeed(state), 1.0F);
        player.vel_x = std::sin(t.jump_heading_rad) * max_speed;
        player.vel_y = -std::cos(t.jump_heading_rad) * max_speed;
        player.speed = max_speed;
        player.engine_glow_level = 32;
        player.engine_glow_intensity = 1.0F;
        t.flying_elapsed_ms += frame_time_ms;
        if (t.flying_elapsed_ms >= kJumpTunnelMs) {
          CompleteJump(state);
          t.engaging = false;
          t.jump_phase = TravelState::JumpPhase::kIdle;
          t.slow_turn_elapsed_ms = 0.0F;
          t.flying_elapsed_ms = 0.0F;
          t.jump_heading_rad = 0.0F;
          t.just_completed = true;
          NovaLog::Debug("hyperspace jump sequence finished");
        }
        break;
      }
      case TravelState::JumpPhase::kIdle:
      default:
        break;
    }
    return;
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

  // Compute the jump heading: the bearing from the ship out to the
  // destination system (constants folded with the polar convention used by
  // Math_BearingFromPointToPoint / Math_AddPolarVelocity: heading 0 = up,
  // clockwise; vel_x += sin(h)*s ; vel_y -= cos(h)*s). This is the direction
  // the ship faces during the slow-turn and coasts through the tunnel.
  const System *dest_sys =
      state.scenario.System(static_cast<std::int16_t>(dest_zero_based + 0x80));
  if (dest_sys != nullptr) {
    // Destination system center in world coords (System.pos_x/pos_y).
    const float dx = static_cast<float>(dest_sys->pos_x) - state.player.pos_x;
    const float dy = static_cast<float>(dest_sys->pos_y) - state.player.pos_y;
    t.jump_heading_rad = std::atan2(dx, -dy); // polar heading to destination
  } else {
    // Fallback: keep the current heading.
    t.jump_heading_rad = state.player.heading;
  }

  t.engaging = true;
  t.jump_phase = TravelState::JumpPhase::kSlowTurn;
  t.slow_turn_elapsed_ms = 0.0F;
  t.flying_elapsed_ms = 0.0F;
  NovaLog::Debug(
      "hyperspace jump engaged: from stellar {} (slot {}) to system {}",
      stellar_id,
      slot,
      dest_zero_based);
}

} // namespace game
