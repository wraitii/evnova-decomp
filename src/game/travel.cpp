#include "travel.hpp"

#include "../log.hpp"
#include "hud_overlay.hpp"
#include "outfit.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace game {
namespace {

constexpr float kWarmupMs = 2000.0F;
// TODO(decomp): derive this from ShipClassDef.jump_duration_multiplier.
constexpr float kZoomMs = 2200.0F;

// The stopped threshold for the turn-around: the original's Ship_HandlePlayer-
// Ship pre-fire block treats |round(vel_x)| < 2 && |round(vel_y)| < 2 as
// "come to a stop", at which point it starts the warp-up hold. We use a
// tighter absolute stop so the flip-and-boost cut leaves no drift.
constexpr float kStoppedVel = 0.5F;

// Turn-around damp: while turning, the ship brakes by this factor per frame
// (original g_jump_turnaround_velocity_damp 0x5755f0, a double 0.992).
constexpr float kTurnDamp = 0.992F;

// Add one degree to the normal effective turn rate while braking.
constexpr float kTurnRateAddend = 1.0F;

// Slow-phase velocity damp during the stationary hold
// (g_hyperspace_slow_phase_velocity_damp 0x5755f8 = 0.98), gentler than the
// turn-around's 0.992.
constexpr float kSlowPhaseVelDamp = 0.98F;

// Engine-glow caps (the original's ShipState +0xc8d4 glow counter): 24 is the
// normal-thrust ramp target (2 per tick * 12 in the tune), and the quick-ramp
// step during the turnaround “overdrives” it by +3/frame to the same 24 cap.
// The glow is turned during the brake (~up to 24), faded while coasting to a
// stop, then re-ramped +3/frame to 24 during the zoom thrust.
constexpr std::int16_t kGlowMax = 24;
constexpr std::int16_t kGlowFastStep = 3;

// Arrival spawn offset (px) from the destination system center, on the far
// side along the jump heading so the ship streaks past the center at speed.
// (Divergence: the original snaps to the system origin and applies a
// 1350-unit position hurl away from the destination bearing; we keep a
// moderate offset so the destination's stellar bodies stay in view.)
constexpr float kArrivalOffset = 160.0F;

// Player max speed scale: px/tick = eff.speed_raw / kMaxSpeedScale (the
// movement integrator's max_speed_px_per_tick). Used for the arrival-at-speed
// (enter the new system at top speed aimed at center).
constexpr float kMaxSpeedScale = 100.0F;

// Player thrust in px/tick^2 (Ship_ComputeShipEffectiveThrust 0x004640a0 via
// the movement integrator's loader scale: raw accel / 10000 * 2.0). Used by
// the turn-around's flip-and-boost (the original applies effective thrust
// toward the heading once facing).
float PlayerThrust(const GameState &state) {
  return state.cached_stats.thrust_raw / 10000.0F * 2.0F;
}

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

// Completes an engaged jump: the fire/arrival moment. Mirrors the fire +
// arrival block in Stellar_HandlePlayerHyperspaceSequence (0x0044f3d0): the
// full-screen flash and the 'Warp out' boom, the fuel burn, the system change
// with the ship repositioned at a radial offset from the destination center
// and moving at its top speed, the shield/armor refill, and the discovery
// flood. The jump completes here (at the end of the zoom), arriving already in
// the NEW system at max speed; the ship then coasts through it in normal
// flight, which is why the system change is done here rather than after a
// separate tunnel phase.
void FireJump(GameState &state) {
  TravelState &t = state.travel;
  PlayerShip &player = state.player;

  // Full-screen white flash (the original's centered effect 0x32, the 'boom'
  // white frame) and the 'Warp out' sound (snd 130), latched for the
  // spaceflight loop (which owns SdlAudio) -- the flash and the boom land on
  // the same frame.
  state.screen_flash_intensity = 1.0F;
  state.warp_out_sound_pending = true;

  // Burn the jump's fuel.
  player.fuel_points = std::max(0.0F, player.fuel_points - kJumpFuelCost);

  // Change system and clear the engaged destination.
  player.current_system_id = t.destination_system_id;

  // Refill shields/armor from the effective (outfit-derived) maximums. This
  // also refreshes state.cached_stats, which PlayerMaxSpeed below relies on.
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  player.shield_points = eff.max_shield_points;
  player.armor_points = eff.max_armor_points;
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  // Reveal the destination system and its immediate neighbourhood (the
  // original's System_FloodDiscoverAdjacentSystems on arrival).
  NovaTravel_MarkSystemDiscovered(state, t.destination_system_id);

  // Arrive just outside the destination system's center on the far side along
  // the jump heading, and move at top speed along the ship's heading (which
  // the brake/hold aligned onto the jump vector and the zoom thrust along) --
  // the original's arrival "at full speed into the new system", streaking past
  // the center in normal flight.
  const auto *dest_sys = state.scenario.System(
      static_cast<std::int16_t>(t.destination_system_id + 0x80));
  const float cx = dest_sys ? static_cast<float>(dest_sys->pos_x) : 0.0F;
  const float cy = dest_sys ? static_cast<float>(dest_sys->pos_y) : 0.0F;
  const float arrival_speed = std::max(PlayerMaxSpeed(state), 1.0F);
  player.pos_x = cx - std::sin(t.jump_heading_rad) * kArrivalOffset;
  player.pos_y = cy + std::cos(t.jump_heading_rad) * kArrivalOffset;
  player.vel_x = std::sin(player.heading) * arrival_speed;
  player.vel_y = -std::cos(player.heading) * arrival_speed;
  player.speed = arrival_speed;

  NovaLog::Info("hyperspace jump fired: system id {} (resource {}) after "
                "burning {} fuel; shields/armor refilled",
                state.player.current_system_id,
                static_cast<int>(CurrentSystemResource(state)),
                static_cast<int>(kJumpFuelCost));

  // The plotted starmap destination and travel engagement are consumed by the
  // fire; the spaceflight loop re-spawns the starfield/asteroids for the new
  // system when it observes just_completed.
  t.travel_slot = -1;
  t.engaged_stellar_id = -1;
  t.starmap_destination_system_id = -1;
  t.destination_system_id = -1;
  t.hyperspace_mode = false;
  t.just_completed = true;
  // The jump completes at the fire/arrival instant: the ship is now in the
  // NEW system at max speed and control returns to normal flight (the world
  // scrolls past as the ship coasts through the new system). There is no
  // separate in-tunnel phase after this.
  t.engaging = false;
  t.jump_phase = TravelState::JumpPhase::kIdle;
  t.hold_elapsed_ms = 0.0F;
  t.zoom_elapsed_ms = 0.0F;
  t.warp_up_started = false;
  t.jump_heading_rad = 0.0F;
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
    // (b) Engaged: drive the visible jump phases, then complete at the
    // fire/arrival instant.
    constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
    constexpr float kTwoPi = 6.28318530717958646F;
    // The movement model's tick cadence (30 Hz reference; see
    // NovaPlayer_IntegrateMovement).
    const float ticks = frame_time_ms / (1000.0F / 30.0F);
    Ship &player = state.player;

    // Update the engine-glow intensity from the level counter (0..24).
    const auto refresh_glow = [&]() {
      player.engine_glow_intensity = std::clamp(
          static_cast<float>(player.engine_glow_level) / 24.0F, 0.0F, 1.0F);
    };
    // Ramp the glow counter up by `step`, capped at kGlowMax.
    const auto ramp_glow = [&](std::int16_t step) {
      player.engine_glow_level =
          std::min<std::int16_t>(kGlowMax, player.engine_glow_level + step);
      refresh_glow();
    };
    // Fade the glow counter down by 1, floored at 0.
    const auto fade_glow = [&]() {
      if (player.engine_glow_level > 0) {
        player.engine_glow_level =
            static_cast<std::int16_t>(player.engine_glow_level - 1);
        refresh_glow();
      }
    };
    const float max_turn_around = std::max(
        std::round(state.cached_stats.turn_raw * 0.1F) + kTurnRateAddend, 1.0F);
    // Turn the heading by at most the class turn rate (rad) this frame.
    const auto turn_toward = [&](float desired, float max_turn_deg) {
      const float turn_rad = max_turn_deg * kDegToRad * ticks;
      float delta = std::remainder(desired - player.heading, kTwoPi);
      delta = std::clamp(delta, -turn_rad, turn_rad);
      player.heading = std::fmod(player.heading + delta + kTwoPi, kTwoPi);
    };

    switch (t.jump_phase) {
    case TravelState::JumpPhase::kBrake: {
      // Pre-fire turn-around, mirroring the travel_transfer_mode == 3 block in
      // Ship_HandlePlayerShip (0x0044b120): while the ship still has velocity
      // it turns toward the REVERSE of its velocity (bearing from vel*100 back
      // to origin, i.e. flying out from the system) at the effective class
      // turn rate plus one degree, brakes by
      // g_jump_turnaround_velocity_damp (0.992) per frame, and once facing the
      // heading within one tick applies effective thrust back along it (the
      // flip-and-boost that visibly stops the ship) while ramping the engine
      // glow by +3/frame up to 24 (ShipState +0xc8d4, the normal-thrust cap).
      // Ends when the ship has come to a stop (|vel| < kStoppedVel) -- the
      // original's LAB_0044edfd handoff to the warp-up hold.
      if (std::abs(player.vel_x) >= kStoppedVel ||
          std::abs(player.vel_y) >= kStoppedVel) {
        // Still moving: turn around to face the reverse of the velocity.
        const float vel_heading = std::atan2(player.vel_x, -player.vel_y);
        float desired =
            std::fmod(vel_heading + 3.14159265358979323846F, kTwoPi);
        if (desired < 0.0F) {
          desired += kTwoPi;
        }
        turn_toward(desired, max_turn_around);

        // Once facing the reverse heading within one tick, punch back along it
        // (Ship_ComputeShipEffectiveThrust toward the heading) and ramp the
        // engine glow; otherwise the glow fades.
        const float delta_deg =
            std::abs(std::remainder(desired - player.heading, kTwoPi)) *
            (180.0F / 3.14159265358979323846F);
        if (delta_deg < max_turn_around) {
          const float thrust = PlayerThrust(state);
          player.vel_x += std::sin(player.heading) * thrust * ticks;
          player.vel_y += -std::cos(player.heading) * thrust * ticks;
          ramp_glow(kGlowFastStep);
        } else {
          fade_glow();
        }
        // Brake (original g_jump_turnaround_velocity_damp = 0.992) and keep
        // the ship coasting through the deceleration.
        player.vel_x *= kTurnDamp;
        player.vel_y *= kTurnDamp;
        player.speed = std::hypot(player.vel_x, player.vel_y);
        player.pos_x += player.vel_x * ticks;
        player.pos_y += player.vel_y * ticks;
      } else {
        // First finish the map-vector turn; Warp up starts with the launch.
        t.jump_phase = TravelState::JumpPhase::kHold;
        t.hold_elapsed_ms = 0.0F;
      }
      break;
    }
    case TravelState::JumpPhase::kHold: {
      // Stationary alignment hold: the ship sits at the jump point, damping
      // the remaining drift by the slow-phase damp
      // (g_hyperspace_slow_phase_velocity_damp 0.98) and turning onto the jump
      // heading at class rate (the original's slow-turn re-aim
      // Ship_TurnShipTowardHeading in Stellar_HandlePlayerHyperspaceSequence --
      // the frame skipped alignment with the ship's actual jump vector). The
      // engine glow fades. After a short charging beat once aligned with the
      // jump vector, the zoom begins.
      player.vel_x *= kSlowPhaseVelDamp;
      player.vel_y *= kSlowPhaseVelDamp;
      player.speed = std::hypot(player.vel_x, player.vel_y);
      fade_glow();
      // Align onto the jump heading at the class turn rate (unlimited by the
      // turn-around's 20-deg floor -- the ship is already nearly pointed).
      turn_toward(
          t.jump_heading_rad,
          std::max(std::round(state.cached_stats.turn_raw * 0.1F), 1.0F));
      const float alignment_error =
          std::abs(std::remainder(t.jump_heading_rad - player.heading, kTwoPi));
      if (alignment_error < 0.001F) {
        t.jump_phase = TravelState::JumpPhase::kWarmup;
        t.hold_elapsed_ms = 0.0F;
        t.warp_up_started = true;
        state.warp_up_sound_pending = true;
      }
      break;
    }
    case TravelState::JumpPhase::kWarmup:
      player.vel_x *= kSlowPhaseVelDamp;
      player.vel_y *= kSlowPhaseVelDamp;
      player.speed = std::hypot(player.vel_x, player.vel_y);
      fade_glow();
      t.hold_elapsed_ms += frame_time_ms;
      if (t.hold_elapsed_ms >= kWarmupMs) {
        t.jump_phase = TravelState::JumpPhase::kZoom;
        t.zoom_elapsed_ms = 0.0F;
      }
      break;
    case TravelState::JumpPhase::kZoom: {
      // Acceleration-zoom: the ship thrusts to max speed along the jump
      // heading as the origin system parallaxes away (the ambient-star tunnel
      // streaks via SpaceflightView::UpdateAmbientStarsTunnel, driven by the
      // world movement delta). The engine glow ramps +3/frame toward 24
      // (powered warp). Mirrors the original's in-tunnel flight whose length
      // is Stellar_GetJumpSequenceDurationMs (350) / jump_duration_multiplier;
      // here a fixed kZoomMs fallback (TODO(decomp): decode the class
      // multiplier and use 350/m).
      player.vel_x *= kSlowPhaseVelDamp;
      player.vel_y *= kSlowPhaseVelDamp;
      player.speed = std::hypot(player.vel_x, player.vel_y);
      // Accelerate along the jump heading to max speed (polar clamp).
      const float max_speed = std::max(PlayerMaxSpeed(state), 1.0F);
      const float thrust = PlayerThrust(state);
      player.vel_x += std::sin(t.jump_heading_rad) * thrust * ticks;
      player.vel_y += -std::cos(t.jump_heading_rad) * thrust * ticks;
      const float speed = std::hypot(player.vel_x, player.vel_y);
      if (speed > max_speed) {
        const float scale = max_speed / speed;
        player.vel_x *= scale;
        player.vel_y *= scale;
      }
      player.heading = t.jump_heading_rad;
      player.speed = std::hypot(player.vel_x, player.vel_y);
      // Advance world position so the origin system scrolls away.
      player.pos_x += player.vel_x * ticks;
      player.pos_y += player.vel_y * ticks;
      ramp_glow(kGlowFastStep);

      t.zoom_elapsed_ms += frame_time_ms;
      if (t.zoom_elapsed_ms >= kZoomMs) {
        // Boom/arrival: full-screen flash + 'Warp out' boom + system change
        // (see FireJump). The ship arrives in the NEW system at max speed and
        // control returns to normal flight.
        FireJump(state);
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

  // The original turns toward the map-space bearing from the current system
  // to its linked destination; in-system player coordinates are unrelated.
  const System *source_sys =
      state.scenario.System(CurrentSystemResource(state));
  const System *dest_sys =
      state.scenario.System(static_cast<std::int16_t>(dest_zero_based + 0x80));
  if (source_sys != nullptr && dest_sys != nullptr) {
    const float dx = static_cast<float>(dest_sys->pos_x - source_sys->pos_x);
    const float dy = static_cast<float>(dest_sys->pos_y - source_sys->pos_y);
    t.jump_heading_rad = std::atan2(dx, -dy);
  } else {
    // Fallback: keep the current heading.
    t.jump_heading_rad = state.player.heading;
  }

  t.engaging = true;
  t.jump_phase = TravelState::JumpPhase::kBrake;
  t.warp_up_started = false;
  t.hold_elapsed_ms = 0.0F;
  t.zoom_elapsed_ms = 0.0F;
  NovaLog::Debug(
      "hyperspace jump engaged: from stellar {} (slot {}) to system {}",
      stellar_id,
      slot,
      dest_zero_based);
}

} // namespace game
