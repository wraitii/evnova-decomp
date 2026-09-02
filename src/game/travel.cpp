#include "travel.hpp"

#include "../log.hpp"
#include "hud_overlay.hpp"
#include "outfit.hpp"
#include "weapon.hpp"

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

// The pre-fire turnaround turns at the class turn rate (Ship_ComputeShipMax
// TurnRateDeg 0x00463e70, floor 1.0 deg/tick) -- no speed-up. The "+1" addend
// and the 20-deg figure are only the FACING WINDOW (the alignment within which
// the ship punches thrust back along the departure bearing), from
// g_jump_turnaround_turn_rate_addend 0x57555c = 1.0 and
// g_jump_turnaround_min_turn_rate_deg 0x5755e8 = 20.0.
constexpr float kTurnRateAddend = 1.0F;
constexpr float kTurnAroundAlignDeg = 20.0F;

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

  // Ghidra ShipState +0x94: the player-tick jump branch (0x0044aa70) records
  // the pre-jump system before switching; mission-fleet respawn arrivals
  // orient their bearing on it (System_TickNpcSpawnMaintenance 0x0041d6e0).
  player.jump_destination_system_id = player.current_system_id;

  // Change system and clear the engaged destination.
  player.current_system_id = t.destination_system_id;
  NovaWeapon_ClearTransientCombatState(state);

  // Refill shields/armor from the effective (outfit-derived) maximums. This
  // also refreshes state.cached_stats, which PlayerMaxSpeed below relies on.
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  player.shield_points = eff.max_shield_points;
  player.armor_points = eff.max_armor_points;
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  // Book the arrival as visited at level 1 and rebuild the map reveal (the
  // original's arrival block: discovery_state >= 1 on slot + current, then
  // System_RebuildSystemVisibilityMap(cur, 0, 1) -- only the arrival system is
  // flooded; the one-hop window comes from the discovered_this_rebuild latch).
  NovaSystem_OnSystemEntered(state, t.destination_system_id, 1);

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
  // Arrival route maintenance (Ship_HandlePlayerShipCore's arrival tick):
  // the map pan re-centres on the new system (0x0044f8a6), a plotted route
  // hop that matched this system is consumed
  // (System_NormalizePlannedRouteToCurrentSystem 0x004a7fc0) and the travel
  // slot re-arms from the next hop
  // (NovaUi_SyncTravelSelectionFromStarmapRoute 0x004a8080).
  state.starmap_pan_x = cx;
  state.starmap_pan_y = cy;
  NovaStarmap_NormalizeRouteToCurrentSystem(state);
  NovaStarmap_SyncTravelSelectionFromRoute(state);
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
  for (std::size_t slot = 0; slot < sys->links.size(); ++slot) {
    const std::int16_t link = sys->links[slot];
    if (link < 0x80) {
      continue;
    }
    // Ghidra 0x004a5840 compares the clicked system against the LINK resolved
    // through the visibility chain (System_ResolveVisibleSystemForTravel
    // 0x0046b920), not the raw link id — a visible twin arms the slot whose
    // raw link points at a currently hidden root.
    if (NovaSystem_ResolveVisibleForTravel(
            state, static_cast<std::int16_t>(link - 0x80)) ==
        destination_zero_based) {
      return static_cast<int>(slot);
    }
  }
  return -1;
}

} // namespace

// Ghidra 0x004a8080 NovaUi_SyncTravelSelectionFromStarmapRoute.
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
  // Plotted-jump mode latch (0x004a491e): the accent line and the jump HUD
  // read travel_transfer_mode == 3.
  state.player.travel_transfer_mode = 3;
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
// Ghidra 0x00462db0 Stellar_FindNearestAvailableTravelStellar.
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
// Ghidra 0x00415b80 Stellar_CanShipInitiateJumpSequence.
// ---------------------------------------------------------------------------
bool NovaTravel_CanStartJump(const GameState &state) {
  // Player-side convenience wrapper around the faithful per-ship gate.
  return NovaTravel_CanShipInitiateJumpSequence(state, state.player);
}

// Ghidra 0x00415b80 Stellar_CanShipInitiateJumpSequence, for an arbitrary
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

// Ghidra 0x0046b9b0 System_ResolveSystemDiscoverySlot.
// ---------------------------------------------------------------------------
// Galaxy discovery (fog of war). See travel.hpp for the model: the original's
// per-system fog record is SystemDef.discovery_state (+0x90), the starmap's
// "one jump ahead" window is the transient discovered_this_rebuild latch.
// ---------------------------------------------------------------------------
std::int16_t NovaSystem_ResolveDiscoverySlot(const GameState &state,
                                             std::int16_t system_id) {
  if (system_id < 0 ||
      static_cast<std::size_t>(system_id) >= state.scenario.systems.size()) {
    return -1;
  }
  const std::int16_t root =
      state.scenario.systems[static_cast<std::size_t>(system_id)]
          .visibility_root_system_id;
  if (root != -1) {
    return root;
  }
  return system_id;
}

// Ghidra 0x0046b920 System_ResolveVisibleSystemForTravel. Follows the
// visibility-root/parent chain built by the loader's twin-grouping pass
// (0x004bd3c0 / 0x004beb4f) to the first twin whose Visibility NCB currently
// holds; a group with no visible member resolves to -1.
std::int16_t NovaSystem_ResolveVisibleForTravel(const GameState &state,
                                                std::int16_t system_id) {
  if (system_id < 0 ||
      static_cast<std::size_t>(system_id) >= state.scenario.systems.size()) {
    return -1;
  }
  const auto &sys = state.scenario.systems[static_cast<std::size_t>(system_id)];
  if (sys.visibility_root_system_id != -1) {
    std::int16_t candidate = sys.visibility_root_system_id;
    while (candidate >= 0 && static_cast<std::size_t>(candidate) <
                                 state.scenario.systems.size()) {
      if (state.scenario.systems[static_cast<std::size_t>(candidate)]
              .is_visible) {
        return candidate;
      }
      candidate = state.scenario.systems[static_cast<std::size_t>(candidate)]
                      .visible_parent_system_id;
    }
    return -1;
  }
  return sys.is_visible ? system_id : -1;
}

// Ghidra 0x00468af0 System_HasUsableTravelDestination. Scans the departure
// stellar list (nav_stellar_ids) and accepts a spob that is a normal,
// reachable destination: travel_flags bit 0x20 clear and availability_flags
// & 0x3000 clear. The original scans the first four slots only.
bool NovaSystem_HasUsableTravelDestination(const GameState &state,
                                           std::int16_t system_id) {
  if (system_id < 0 ||
      static_cast<std::size_t>(system_id) >= state.scenario.systems.size()) {
    return false;
  }
  const auto &navs =
      state.scenario.systems[static_cast<std::size_t>(system_id)].nav_defs;
  for (std::size_t i = 0; i < 4 && i < navs.size(); ++i) {
    const auto *stellar = state.scenario.Stellar(navs[i]);
    if (stellar == nullptr) {
      continue;
    }
    if ((stellar->flags & 0x20U) == 0U &&
        (stellar->availability_flags & 0x3000U) == 0U) {
      return true;
    }
  }
  return false;
}

void NovaSystem_MarkSystemVisited(GameState &state,
                                  std::int16_t zero_based_system_id,
                                  std::int16_t level) {
  if (zero_based_system_id < 0 ||
      static_cast<std::size_t>(zero_based_system_id) >=
          state.scenario.systems.size()) {
    return;
  }
  const std::size_t idx = static_cast<std::size_t>(zero_based_system_id);
  auto &sys = state.scenario.systems[idx];
  if (sys.discovery_state < level) {
    sys.discovery_state = level;
  }
  // Mirror into the persistent explored bitset (the NCB `has_explored` test
  // and the pilot-save discovery block read it). SystemDef.is_visible /
  // has_explored_flag are load-time flags set by the scenario decoder and are
  // NOT fog (see scenario_data.hpp); the fog record is discovery_state.
  if (idx < state.control.explored_systems.size()) {
    state.control.explored_systems.set(idx);
  }
}

void NovaSystem_FloodDiscoverAdjacentSystems(
    GameState &state,
    std::int16_t zero_based_system_id,
    std::int16_t depth,
    std::int16_t max_depth,
    std::int16_t threshold,
    std::vector<std::uint8_t> &visited) {
  if (depth > max_depth) {
    return;
  }
  if (zero_based_system_id < 0 ||
      static_cast<std::size_t>(zero_based_system_id) >=
          state.scenario.systems.size()) {
    return;
  }
  const std::size_t idx = static_cast<std::size_t>(zero_based_system_id);
  if (idx >= visited.size() || visited[idx] != 0) {
    return;
  }
  visited[idx] = 1;
  // The original fires the system's rectangular nebula/region triggers per
  // newly reached system (Frame_TriggerSystemEvents 0x00467bd0), latching
  // explored nebulae and running their OnExplore set expressions.
  NovaSystem_TriggerNebulaRegionEvents(state, zero_based_system_id);
  NovaSystem_MarkSystemVisited(state, zero_based_system_id, threshold);

  const std::int16_t slot =
      NovaSystem_ResolveDiscoverySlot(state, zero_based_system_id);
  if (slot != zero_based_system_id && slot >= 0 &&
      static_cast<std::size_t>(slot) < state.scenario.systems.size()) {
    auto &slot_sys = state.scenario.systems[static_cast<std::size_t>(slot)];
    if (slot_sys.discovery_state < threshold) {
      slot_sys.discovery_state = threshold;
    }
  }

  // Links are stored as system resource ids in System.links (already
  // normalized to visibility roots by the scenario loader, 0x004bd3c0). The
  // original recurses through System_ResolveVisibleSystemForTravel
  // (0x0046b920), which returns -1 for systems whose Visibility NCB currently
  // fails -- an invisible twin group blocks the flood entirely.
  const auto &sys = state.scenario.systems[idx];
  for (const std::int16_t link : sys.links) {
    if (link < 0x80) {
      continue;
    }
    const std::int16_t target = NovaSystem_ResolveVisibleForTravel(
        state, static_cast<std::int16_t>(link - 0x80));
    if (target < 0) {
      continue;
    }
    NovaSystem_FloodDiscoverAdjacentSystems(
        state,
        target,
        static_cast<std::int16_t>(depth + 1),
        max_depth,
        threshold,
        visited);
  }
}

void NovaSystem_RebuildDiscoveryState(GameState &state,
                                      std::int16_t origin_zero_based,
                                      std::int16_t max_depth,
                                      std::int16_t threshold) {
  if (state.scenario.systems.empty()) {
    return;
  }
  std::vector<std::uint8_t> visited(state.scenario.systems.size(), 0);
  NovaSystem_FloodDiscoverAdjacentSystems(
      state, origin_zero_based, 0, max_depth, threshold, visited);

  // Post-pass: rebuild the transient discovered_this_rebuild latch — every
  // VISIBLE visited system gets it, plus every travel-resolvable link
  // neighbour of one (System_RebuildSystemVisibilityMap 0x00467970 gates the
  // source on is_visible && has_explored_flag && discovery_state > 0 and
  // resolves each link through System_ResolveVisibleSystemForTravel
  // 0x0046b920, so invisible twin clones never latch and never show on the
  // map).
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    state.scenario.systems[i].discovered_this_rebuild = false;
  }
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    auto &sys = state.scenario.systems[i];
    if (sys.discovery_state <= 0 || !sys.is_visible || !sys.has_explored_flag) {
      continue;
    }
    sys.discovered_this_rebuild = true;
    for (const std::int16_t link : sys.links) {
      if (link < 0x80) {
        continue;
      }
      const std::int16_t target = NovaSystem_ResolveVisibleForTravel(
          state, static_cast<std::int16_t>(link - 0x80));
      if (target >= 0) {
        state.scenario.systems[static_cast<std::size_t>(target)]
            .discovered_this_rebuild = true;
      }
    }
  }
}

void NovaSystem_TriggerNebulaRegionEvents(GameState &state,
                                          std::int16_t zero_based_system_id) {
  if (state.scenario.nebulae.empty() || zero_based_system_id < 0 ||
      static_cast<std::size_t>(zero_based_system_id) >=
          state.scenario.systems.size()) {
    return;
  }
  const System &sys =
      state.scenario.systems[static_cast<std::size_t>(zero_based_system_id)];
  ControlExpressionState expression;
  expression.get_control_bit = [&state](std::uint32_t bit) {
    return state.control.ControlBit(bit);
  };
  expression.is_registered = [&state](std::uint32_t) {
    return state.control.registered;
  };
  expression.is_male = [&state] { return state.control.male; };
  expression.owns_outfit = [&state](std::int16_t id) {
    return id >= 0 &&
           id < static_cast<std::int16_t>(
                    state.inventory.outfit_owned_count.size()) &&
           state.inventory.outfit_owned_count[static_cast<std::size_t>(id)] > 0;
  };
  expression.has_explored = [&state](std::int16_t id) {
    return id >= 0 && id < 0x800 &&
           state.control.explored_systems.test(static_cast<std::size_t>(id));
  };
  for (Nebula &neb : state.scenario.nebulae) {
    // No rect (absent resource): skip, like the original's zero-filled slot.
    if (neb.width == 0 || neb.height == 0) {
      continue;
    }
    // The original caches the ActiveOn evaluation (+0x8 byte) in
    // NovaResources_EvaluateAvailability; re-evaluating per check is
    // equivalent (controls only change through set expressions).
    neb.active_on =
        NovaControlExpression_Evaluate(neb.active_on_expression, expression);
    if (!neb.active_on || neb.explored) {
      continue;
    }
    // Rect inset by 8 around the nebula rect, then a point test on the
    // system's world position (0x00467c57).
    const int x0 = neb.x + 8;
    const int y0 = neb.y + 8;
    const int x1 = neb.x + neb.width - 8;
    const int y1 = neb.y + neb.height - 8;
    if (sys.pos_x < x0 || sys.pos_x > x1 || sys.pos_y < y0 || sys.pos_y > y1) {
      continue;
    }
    neb.explored = true;
    NovaLog::Info("nebula region reached at system {} (resource {}); running "
                  "OnExplore expression",
                  zero_based_system_id,
                  zero_based_system_id + 0x80);
    // Ghidra Mission_ExecuteReactionScript on the OnExplore set string
    // (0x00467cc1); the clean-room executor handles the control-bit set
    // grammar (Bxxx / !Bxxx / Bxxx=0).
    NovaControlExpression_ExecuteSet(
        neb.on_explore_expression,
        ControlExpressionMutation{[&state](std::uint32_t bit, bool value) {
          state.control.SetControlBit(bit, value);
        }});
  }
}

void NovaSystem_OnSystemEntered(GameState &state,
                                std::int16_t zero_based_system_id,
                                std::int16_t level) {
  // Arrival pre-latch: the entered system and its discovery slot are booked as
  // visited even before the flood (0x0044aa70 writes level 1 for hyperspace
  // arrivals, 0x00455e10 Stellar_TravelToSystem writes level 2).
  NovaSystem_MarkSystemVisited(state, zero_based_system_id, level);
  NovaSystem_MarkSystemVisited(
      state,
      NovaSystem_ResolveDiscoverySlot(state, zero_based_system_id),
      level);
  NovaSystem_RebuildDiscoveryState(state, zero_based_system_id, 0, level);
}

// ---------------------------------------------------------------------------
// Destination-system cycling (key binding 13; Ghidra 0x0044b8b9..0x0044def6 in
// PlayerTick_TargetAndTravelCommands, g_playerCycleTravelTargetCommandLatch).
// ---------------------------------------------------------------------------
// Mirrors the original: a candidate slot is one whose linked destination
// resolves to a visible system through the visibility chain
// (System_ResolveVisibleSystemForTravel 0x0046b920, 0x0044de58) and is not a
// self-link; pressing the key arms travel mode 3 and advances the slot
// selector forward/backward through the candidates (wrapping 0..15). The
// destination is the RESOLVED twin of the raw link, so 'j' jumps to whichever
// group member currently passes its Visibility NCB.
// Divergences: the original gates the latch on a fuel check ([ship+0x50] vs
// DAT_00575538) and plays a centered UI sound (NovaAudio_QueueCenteredSound
// 0x0044de80); the clean-room does neither.
std::int16_t NovaTravel_CycleDestinationSystem(GameState &state, bool forward) {
  TravelState &t = state.travel;
  const System *sys = state.scenario.System(CurrentSystemResource(state));
  if (!sys) {
    return -1;
  }
  const std::int16_t current_res = CurrentSystemResource(state);
  const std::size_t n = state.scenario.systems.size();

  // Collect candidate slots: every link slot whose destination resolves to a
  // visible system (twin-resolved) and does not loop back to the current one.
  std::array<int, 16> slots{};
  std::size_t count = 0;
  for (std::size_t slot = 0; slot < sys->links.size(); ++slot) {
    const std::int16_t link = sys->links[slot];
    if (link < 0x80 || link == current_res) {
      continue; // no link, or it loops back to the current system
    }
    const std::int16_t dest = NovaSystem_ResolveVisibleForTravel(
        state, static_cast<std::int16_t>(link - 0x80));
    if (dest < 0 || static_cast<std::size_t>(dest) >= n) {
      continue; // link leads into an invisible twin group
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
  const std::int16_t dest = NovaSystem_ResolveVisibleForTravel(
      state,
      static_cast<std::int16_t>(sys->links[static_cast<std::size_t>(slot)] -
                                0x80));

  t.travel_slot = static_cast<std::int16_t>(slot);
  t.starmap_destination_system_id = dest;
  t.destination_system_id = dest;
  // Cycle latches plotted-jump mode 3 like the starmap arm (0x0044dea7).
  state.player.travel_transfer_mode = 3;
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
    // The turn is done at the class turn rate (no min-20 boost); the min-20
    // only sizes the facing window for the thrust-back below.
    const float class_turn_deg =
        std::max(std::round(state.cached_stats.turn_raw * 0.1F), 1.0F);
    const float turn_align_window =
        std::max(class_turn_deg + kTurnRateAddend, kTurnAroundAlignDeg);
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
      // to origin), brakes by g_jump_turnaround_velocity_damp (0.992) per
      // frame, and once within the facing window
      // (max(class turn + 1, 20) deg -- a gate, not a turn speed) applies
      // effective thrust back along it to visibly stop the ship. Ends when the
      // ship has come to a stop (|vel| < kStoppedVel) -- the original's
      // LAB_0044edfd handoff to the warp-up hold.
      if (std::abs(player.vel_x) >= kStoppedVel ||
          std::abs(player.vel_y) >= kStoppedVel) {
        // Still moving: turn around to face the reverse of the velocity.
        const float vel_heading = std::atan2(player.vel_x, -player.vel_y);
        float desired =
            std::fmod(vel_heading + 3.14159265358979323846F, kTwoPi);
        if (desired < 0.0F) {
          desired += kTwoPi;
        }
        turn_toward(desired, class_turn_deg);

        // Once facing the reverse heading within the facing window, punch back
        // along it (Ship_ComputeShipEffectiveThrust toward the heading) and
        // ramp the engine glow; otherwise the glow fades.
        const float delta_deg =
            std::abs(std::remainder(desired - player.heading, kTwoPi)) *
            (180.0F / 3.14159265358979323846F);
        if (delta_deg < turn_align_window) {
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
      // Align onto the jump heading at the class turn rate.
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
                               0x168U);
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

// ---------------------------------------------------------------------------
// Plotted starmap route (state.travel.starmap_route; Ghidra DAT_00735404).
// ---------------------------------------------------------------------------

void NovaStarmap_NormalizeRoutePlan(GameState &state) {
  auto &route = state.travel.starmap_route;
  // Ghidra 0x004a7e80: drop a leading empty slot by shifting left, and clear
  // the whole route when the first hop is missing.
  if (route[0] == -1) {
    for (std::size_t i = 0; i + 1 < route.size(); ++i) {
      route[i] = route[i + 1];
    }
    route.back() = -1;
  }
  if (route[1] == -1) {
    route.fill(-1);
  }
}

void NovaStarmap_NormalizeRouteToCurrentSystem(GameState &state) {
  auto &route = state.travel.starmap_route;
  const std::int16_t first_hop =
      NovaSystem_ResolveVisibleForTravel(state, route[1]);
  const std::int16_t current =
      NovaSystem_ResolveVisibleForTravel(state, state.player.current_system_id);
  if (first_hop != -1 && first_hop == current) {
    // The ship just arrived at the plotted first hop: consume it.
    route[0] = -1;
    NovaStarmap_NormalizeRoutePlan(state);
  } else if (first_hop == -1) {
    route.fill(-1);
  }
}

void NovaStarmap_SyncTravelSelectionFromRoute(GameState &state) {
  auto &t = state.travel;
  if (t.starmap_route[1] == -1) {
    return;
  }
  const std::int16_t first_hop =
      NovaSystem_ResolveVisibleForTravel(state, t.starmap_route[1]);
  const System *sys = state.scenario.System(CurrentSystemResource(state));
  if (!sys) {
    return;
  }
  for (std::size_t slot = 0; slot < sys->links.size(); ++slot) {
    const std::int16_t link = sys->links[slot];
    if (link < 0x80) {
      continue;
    }
    if (NovaSystem_ResolveVisibleForTravel(
            state, static_cast<std::int16_t>(link - 0x80)) == first_hop) {
      t.travel_slot = static_cast<std::int16_t>(slot);
      t.starmap_destination_system_id = first_hop;
      t.selected_stellar_is_manual = true;
      // Route re-arm (0x004a8080) also latches plotted-jump mode 3.
      state.player.travel_transfer_mode = 3;
      NovaLog::Info(
          "starmap route re-armed: next hop system {} on link slot {}",
          first_hop,
          slot);
      return;
    }
  }
}

bool NovaStarmap_RouteHasHops(const GameState &state) {
  return state.travel.starmap_route[1] != -1;
}

// Ghidra 0x004a47cc (NovaUi_StarmapWindowInnerLoop, shift-click branch): edits
// the plotted route at the twin-resolved clicked system `hit`:
//   - hit on the current system's discovery slot: reset the route to empty
//   - hit slot-matches an already-plotted hop: clear that hop and everything
//     plotted after it, then FALL THROUGH (the original does not return
//     here): the tail/append logic below re-appends the hit when the gate
//     still passes, so the route ends AT the clicked hop rather than before
//     it
//   - hit shares the tail's map position (a twin of it): pop the tail,
//     suppressing the append
//   - else append when the tail is visited or the hit is latched
//     (discovered_this_rebuild) and the hit is a travel-resolvable adjacency
//     of the tail (System_ResolveVisibleSystemForTravel 0x0046b920 on both
//     ends)
// Returns true when the click appended a hop: the original moves the map
// selection (g_starmap_selected_system_id) only in that case.
bool NovaStarmap_EditRouteAtHop(GameState &state, std::int16_t hit) {
  auto &route = state.travel.starmap_route;
  if (hit < 0 ||
      static_cast<std::size_t>(hit) >= state.scenario.systems.size()) {
    return false;
  }
  // The original runs the route normalizer first (0x004a7e80, call at
  // 0x004a47d9).
  NovaStarmap_NormalizeRoutePlan(state);

  const std::int16_t current = state.player.current_system_id;
  const std::int16_t hit_slot = NovaSystem_ResolveDiscoverySlot(state, hit);
  if (hit_slot == NovaSystem_ResolveDiscoverySlot(state, current)) {
    // Shift-clicking the current system's slot resets the route to empty.
    // The original then falls into the tail/append logic, but that is a
    // no-op here: route[0] is -1 so the twin-pop guard fails, and no link
    // of the current system resolves back into its own group (self-links
    // are dropped at load), so the append gate can never fire.
    route.fill(-1);
    return false;
  }

  // Truncate: the first plotted hop whose discovery slot matches the hit is
  // cleared along with everything plotted after it (the scan stops at the
  // first empty slot). Control falls through to the tail/append logic.
  for (std::size_t i = 1; i < route.size(); ++i) {
    if (route[i] == -1) {
      break;
    }
    if (NovaSystem_ResolveDiscoverySlot(state, route[i]) == hit_slot) {
      for (std::size_t k = i; k < route.size(); ++k) {
        route[k] = -1;
      }
      break;
    }
  }

  // Tail recompute: count = index of the first empty slot from 1 (-1 when
  // the route is completely full; the original normalizes that to one hop
  // from the current system, 0x004a4b4a).
  int count = -1;
  for (std::size_t i = 1; i < route.size(); ++i) {
    if (route[i] == -1) {
      count = static_cast<int>(i);
      break;
    }
  }
  if (count < 0) {
    if (route[0] == -1) {
      route[0] = current;
    }
    count = 1;
  }

  const auto &hit_sys = state.scenario.systems[static_cast<std::size_t>(hit)];
  // Append gate + adjacency, computed against the pre-pop tail; the twin-pop
  // below overrides the result.
  bool append = false;
  std::int16_t tail = current;
  if (count >= 2) {
    tail = route[count - 1];
    const std::int16_t tail_resolved =
        NovaSystem_ResolveVisibleForTravel(state, tail);
    if (tail_resolved != tail && tail_resolved != -1) {
      tail = tail_resolved;
    }
  }
  const auto &tail_sys =
      state.scenario.systems[static_cast<std::size_t>(tail)];
  if (tail_sys.discovery_state > 0 || hit_sys.discovered_this_rebuild) {
    for (const std::int16_t link : tail_sys.links) {
      if (link < 0x80) {
        continue;
      }
      if (NovaSystem_ResolveVisibleForTravel(
              state, static_cast<std::int16_t>(link - 0x80)) == hit) {
        append = true;
        break;
      }
    }
  }

  // A hit sharing the last plotted hop's map position (its visibility twin)
  // pops that hop instead of appending.
  if (count > 0 && route[count - 1] >= 0 &&
      static_cast<std::size_t>(route[count - 1]) <
          state.scenario.systems.size()) {
    const auto &last_sys = state.scenario.systems[static_cast<std::size_t>(
        route[count - 1])];
    if (hit_sys.pos_x == last_sys.pos_x && hit_sys.pos_y == last_sys.pos_y) {
      route[count - 1] = -1;
      append = false;
    }
  }

  if (append) {
    if (route[0] == -1) {
      route[0] = current;
    }
    route[static_cast<std::size_t>(count)] = hit;
  }
  return append;
}

void NovaStarmap_ClearRoute(GameState &state) {
  auto &route = state.travel.starmap_route;
  route.fill(-1);
  route[0] = state.player.current_system_id;
  // The original also disarms the plotted jump and resets the selection to
  // the current system (0x004a3aa0 action-8 branch).
  state.travel.travel_slot = -1;
  state.travel.starmap_destination_system_id = -1;
}

} // namespace game
