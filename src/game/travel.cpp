#include "travel.hpp"

#include "../log.hpp"
#include "../util/format.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "landed_store.hpp"
#include "mission.hpp"
#include "mission_script.hpp"
#include "mission_trace.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"
#include "spaceflight.hpp"
#include "targeting.hpp"
#include "weapon.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <random>
#include <utility>

namespace game {
namespace {

std::int16_t StellarSystem(const GameState &state, std::int16_t stellar_id) {
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr) {
    return -1;
  }
  if (stellar->system_id >= 0 && static_cast<std::size_t>(stellar->system_id) <
                                     state.scenario.systems.size()) {
    return stellar->system_id;
  }
  return NovaTargeting_FindSystemContainingStellar(state.scenario, stellar_id);
}

std::string RestrictedArrivalMessage(const GameState &state,
                                     const System &system,
                                     RestrictedTravelKind kind) {
  const std::uint16_t lead =
      kind == RestrictedTravelKind::kHypergate ? 0x2e : 0x2f;
  std::string message =
      NovaHud_LoadStringEntry(0x7d2, lead)
          .value_or(kind == RestrictedTravelKind::kHypergate ? "Entering"
                                                             : "Emerging in");
  message += " ";
  message += system.name;
  message += " ";
  message += NovaHud_LoadStringEntry(0x7d2, 0x30).value_or("system on");
  message += " ";
  message += NovaText_FormatDateString(
      state.date, false, state.date_prefix, state.date_suffix);
  message += ".";
  const bool has_nav = std::any_of(system.nav_defs.begin(),
                                   system.nav_defs.end(),
                                   [](std::int16_t id) { return id >= 0x80; });
  if (!has_nav) {
    message += " ";
    message += NovaHud_LoadStringEntry(0x7d2, 0x31)
                   .value_or("No stellar objects present.");
  }
  return message;
}

// Turn-around velocity damp per 30 Hz tick while the brake phase runs.
// The original is g_jump_turnaround_velocity_damp 0x5755f0 = 0.99203847;
// deliberate feel tuning (divergence): 0.985 brakes decisively from the
// moment the jump engages instead of coasting through the first second.
constexpr float kTurnDamp = 0.985F;

// Deliberate feel tuning (divergence): the original's brake retro-thrust is
// the bare effective thrust once the hull faces the departure bearing; the
// port doubles it so the initial slowdown reads harder.
constexpr float kBrakeThrustScale = 2.0F;

// Slow-phase velocity damp per 30 Hz tick during the stationary hold
// (g_hyperspace_slow_phase_velocity_damp 0x5755f8, double 0.98006866),
// gentler than the turn-around's 0.99204.
constexpr float kSlowPhaseVelDamp = 0.98006866F;

// The pre-fire turnaround turns at the class turn rate (Ship_ComputeShipMax
// TurnRateDeg 0x00463e70, floor 1.0 deg/tick) -- no speed-up. The "+1" addend
// and the 20-deg figure are only the FACING WINDOW (the alignment within which
// the ship punches thrust back along the departure bearing), from
// g_jump_turnaround_turn_rate_addend 0x57555c = 1.0 and
// g_jump_turnaround_min_turn_rate_deg 0x5755e8 = 20.0.
constexpr float kTurnRateAddend = 1.0F;
constexpr float kTurnAroundAlignDeg = 20.0F;

// Tunnel ramp schedule (Ship_HandlePlayerShipCore tunnel block, ~0x004505e0).
// VERIFIED UNITS (the historical _ms names are lies -- see the Ghidra plate
// comments): the elapsed clock and the duration share the 1/60 s tick unit
// (NovaTime_GetTickCount60Hz counts 1/60 s; the duration is the Warp up cue's
// own length in the same unit, loaded from snd 128/129 at preload, loader
// 0x004b0a07; see NovaTravel_JumpSequenceDuration60Hz). progress =
// elapsed_60hz * jump_duration_multiplier
//   / (duration_60hz * 0.01)      (g_hyperspace_jump_duration_scale 0x575560)
//   - escape_pod_offset / multiplier (0x575568 = 35.0)
// For a mult=1 stock ship: onset (progress > 0) at tick 127 = 2.12 s into the
// hold, the min(progress, 50) px/tick cap at tick 309 = 5.16 s, and the fire
// lands when the cue (whose duration is 1/multiplier of its base length)
// finishes at tick 364 = 6.07 s for multiplier 1.0 -- a couple of seconds of
// stationary alignment, then the
// acceleration with the glow overdriving, then the boom.
constexpr float kHyperspaceTickHz = 60.0F;
constexpr float kJumpDurationScale = 0.01F; // 0x575560
constexpr float kJumpProgressOffset =
    35.0F; // g_hyperspace_jump_progress_offset 0x575568
// The progress-onset threshold the jump progress tests against (g_hyperspace-
// progress_onset_threshold 0x575540, 0.0): the tunnel ramp starts and the
// disabled-jump collapse gains its exit velocity once progress crosses it.
constexpr float kJumpProgressOnsetThreshold = 0.0F;
constexpr float kTunnelSpeedCap = 50.0F; // 0x5755d8 threshold and literal cap
// ShipClassDef.jump_duration_multiplier (loader:
// NovaData_LoadScenarioResourceTables 0x004bd3c0, shp pass) is applied per ship
// class. It scales the tunnel ramp clock and the 'Warp up' cue duration through
// 0x0046ab00; SDL receives the multiplier as playback speed so the ramp
// schedule and cue-gated fire stay aligned.

// The stationary hold must run at least this many 30 Hz ticks before the fire
// (g_hyperspace_engage_hold_30hz 0x5755a8, float 30.0 -- 30 Hz ticks because
// the hold timer accumulates g_avg_frame_tick_scale = elapsed_ms * 0.03;
// 30 ticks = 1.0 s). The fire additionally waits for the 'Warp up' cue to
// finish (the caller reports the live voice count; see NovaTravel_Tick).
constexpr float kEngageHoldTicks = 30.0F;

// In-tunnel alignment window (deg): the hull must face the jump bearing within
// max(class turn rate, 30.0) for the tunnel ramp to apply (the tunnel block of
// Ship_HandlePlayerShipCore; the 30.0 floor is the literal 0x41f00000 used when
// the class turn rate does not exceed g_hyperspace_engage_hold_30hz).
constexpr float kTunnelAlignDeg = 30.0F;

// The stopped test for the turn-around handoff: the original's jump dispatch
// treats |round(vel_x)| < 2 && |round(vel_y)| < 2 (px/tick, ShipState +0x20)
// as "come to a stop", at which point the hold begins.
constexpr float kStoppedRoundedVel = 2.0F;

// Arrival position hurl (px) along the departure map bearing + 180 from the
// in-system origin (0,0) (g_hyperspace_engage_velocity_hurl 0x57600, float
// 1350.0; the original zeroes the ship at 0x0044f4cd before the polar add):
// the ship materializes on the near side of the new system and streaks through
// the center at max speed under normal flight.
constexpr float kArrivalHurlPx = 1350.0F;

// Engine-glow caps (the original's ShipState +0xc8d4 glow counter): 24 is the
// normal-thrust ramp target (2 per tick * 12 in the tune), and the quick-ramp
// step during the turnaround "overdrives" it by +3/tick to the same 24 cap.
constexpr std::int16_t kGlowMax = 24;
constexpr std::int16_t kGlowFastStep = 3;

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

// Ghidra 0x00465d90 Ship_FormatLocalizedCountWord. Counts 1..10 load STR#
// 0x89 "Date/Numbers" entries 0x1d..0x26 ("one".."ten"); anything else
// renders as decimal digits (PascalString_FromUInt). translate_first runs the
// first byte through the MetroWerks C-locale toupper (MWRuntime_ToUpper
// 0x004d6260), so "one" displays as "One". Used by the arrival
// "fighter(s) abandoned" tally (0x0044fc92, translate_first = 1).
std::string FormatArrivalCountWord(int count, bool translate_first) {
  std::string word;
  if (count < 1 || 10 < count) {
    word = std::to_string(count);
  } else if (const auto text = NovaHud_LoadStringEntry(
                 0x89, static_cast<std::uint16_t>(count + 0x1c))) {
    word = *text;
  } else {
    word = std::to_string(count);
  }
  if (translate_first) {
    word = evnova::util::UpperFirstAscii(std::move(word));
  }
  return word;
}

// Ghidra flight-tail block of Ship_HandlePlayerShipCore (~0x00450a2c): with a
// plotted jump armed (travel_transfer_mode 3 + secondary target), a per-tick
// range probe from the system center over NON-restricted nav stellars drives
// a rising-edge cue. On the 0 -> 1 edge of the in-range latch (DAT_007cab34)
// while not engaged and with fuel for one jump: transition-table sound [4]
// queues (pending_ui_sounds), the travel status panel is marked dirty (no
// port equivalent -- the HUD repaints per frame), and the current overlay
// message (e.g. the 'not yet far enough away' denial) is cleared by a 1-tick
// empty overwrite. The latch updates whenever the jump is armed -- including
// while engaged, where the cue itself is suppressed (hold timer > 0). The
// original never resets the latch on arrival; it only re-arms/clears while a
// jump is armed, so the port mirrors the stale-latch behavior.
void TickJumpRangeCue(GameState &state) {
  // @port 0x0044D0C3 80% gameplay,synthetic
  // Ghidra 0x0044d0c3 PlayerTick_PositionAndJumpRangeCue (synthetic region of
  // 0x0044aa70): position integration is owned by the movement path; this is
  // the jump-range rising-edge cue. Exact parent ordering and the overlay-
  // clearing comparisons remain partially reconstructed.
  TravelState &t = state.travel;
  if (state.player.travel_transfer_mode != 3 || t.travel_slot < 0) {
    return;
  }
  const bool in_jump_range = NovaTravel_PlayerInJumpRange(state);
  if (!in_jump_range) {
    t.jump_range_cue_latch = false;
    return;
  }
  if (!t.jump_range_cue_latch && !t.engaging &&
      state.player.fuel_points >= kJumpFuelCost) {
    state.pending_ui_sounds.push_back({4, 1});
    if (state.hud_overlay.active) {
      NovaHud_ShowOverlayMessage(state, "", 0xe0U, 0xe0U, 0xe0U, 1);
    }
  }
  t.jump_range_cue_latch = true;
}

// @port 0x0044F3D0 75% gameplay,synthetic
// Ghidra 0x0044f3d0 PlayerTick_HyperspaceSequenceAnchor (synthetic region of
// 0x0044aa70): the fire/arrival instant. Remaining TODO(decomp): escort
// warp-sync/loss count, multi-jump depth (0x0046cdd0), the event-message
// arrival variant, and the disabled exit.
// Completes an engaged jump: the fire/arrival moment. Mirrors the fire +
// arrival block at PlayerTick_HyperspaceSequenceAnchor (0x0044f3d0) and
// PlayerTick_SystemTransitionAndArrival (0x0044f660) in
// Ship_HandlePlayerShipCore: the 'Warp out' boom, the position hurl 1350 px
// from the in-system origin (0,0) along the map bearing + 180 with velocity
// reset to max speed along the current heading, the fuel burn, the system
// change, and the discovery flood. No shield/armor refill happens (the original
// restores nothing on hyperspace arrival). Control returns to normal flight
// immediately; the ship coasts through the new system.
void FireJump(GameState &state) {
  TravelState &t = state.travel;
  PlayerShip &player = state.player;

  // Departure raises the no-asteroids latch (Ghidra DAT_00596d2c = 1 in the
  // jump/landing transitions), so the departing system's drift records are
  // hidden and cleared while the hyperspace tunnel plays. The arrival path
  // calls NovaAsteroid_InitSystem, which clears the latch and rebuilds the
  // new system's field.
  state.no_asteroids_latch = true;

  // Full-screen white flash (the original's centered effect 0x32, the 'boom'
  // white frame) and the 'Warp out' sound (snd 130), latched for the
  // spaceflight loop (which owns SdlAudio) -- the flash and the boom land on
  // the same frame. The Mac arrival also runs _FadeWhiteOut (a 1.5 s
  // CoreGraphics display fade) from the hold-end top block; the loop honours
  // it through screen_flash_mode == kFadeOut.
  state.screen_flash_intensity = 1.0F;
  state.screen_flash_mode = GameState::ScreenFlashMode::kFadeOut;
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

  // Refill nothing: the original's arrival block does NOT restore
  // shields/armor on hyperspace arrival (divergence removed).

  // Book the arrival as visited at level 1 and rebuild the map reveal (the
  // original's arrival block: discovery_state >= 1 on slot + current, then
  // System_RebuildSystemVisibilityMap(cur, 0, 1) -- only the arrival system is
  // flooded; the one-hop window comes from the discovered_this_rebuild latch).
  NovaSystem_OnSystemEntered(state, t.destination_system_id, 1);

  // Hyperspace arrival advances the calendar once per jump day. Ghidra
  // PlayerTick_SystemTransitionAndArrival: the player's travel days seed the
  // max (0x0044f8b2), the abandoned-fighter walk (0x0044f8d6, below) extends
  // it over every still-active attached ship, then Mission_TickDailyWorld-
  // Update runs that many times (0x0044f95f).
  int travel_days = NovaStellar_ComputeHyperspaceTravelDays(state, player);
  // Abandoned-fighter walk (0x0044f8d6): over every active ship attached to
  // the player (squad_leader_ship_slot == 0) that is not disabled -- deployed
  // carrier fighters (ai_behavior_code 5, seeded by Weapon_SpawnShipFrom-
  // CarrierBayWeapon 0x0041e640) that cannot jump are deactivated and
  // tallied for the arrival overlay's "fighter(s) abandoned" appendix;
  // every still-active attached ship (fighters and behavior-6 escorts
  // alike) contributes its travel days to the max.
  int abandoned_fighters = 0;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &attached = state.ShipAt(slot);
    if (!attached.is_active || attached.squad_leader_ship_slot != 0 ||
        NovaAiShip_IsDisabled(state, attached)) {
      continue;
    }
    if (attached.ai_behavior_code == 5 &&
        !NovaTravel_CanShipInitiateJumpSequence(state, attached)) {
      attached.is_active = false;
      ++abandoned_fighters;
      continue;
    }
    travel_days = std::max(
        travel_days, NovaStellar_ComputeHyperspaceTravelDays(state, attached));
  }
  // Attached ships that can jump are counted here but not transferred at
  // jump time -- like the original, they keep the departure system id until
  // the escort-adoption slice of System_RebuildInitialNpcAndMission-
  // Population (0x0041af90) runs at arrival (NovaSystem_RestorePlayerEscorts,
  // called from the spaceflight loop's just_completed block).
  // Mission-fleet attached ships are handled by
  // NovaSystem_RestoreMissionFleets.
  for (int day = 0; day < travel_days; ++day) {
    Mission_TickDailyWorldUpdate(state);
  }
  // The original charges at 0x0044fef8, after rebuilding the arrival fleet.
  t.pending_payroll_periods = static_cast<std::int16_t>(travel_days);

  // Arrive 1350 px from the in-system origin (0,0) on the near side along
  // the jump heading's reverse: the original zeroes the ship position
  // (0x0044f4cd) and then adds a polar velocity at
  // Math_BearingFromPointToPoint(cur,dest) + 180 (0x0044f5ae), magnitude
  // g_hyperspace_engage_velocity_hurl 1350, and resets velocity to max speed
  // along the ship's heading (which the hold aligned onto the map travel
  // bearing) -- "at full speed into the new system", streaking past the
  // center in normal flight.
  const auto *dest_sys = state.scenario.System(
      static_cast<std::int16_t>(t.destination_system_id + 0x80));
  const float arrival_speed = PlayerMaxSpeed(state);
  player.pos_x = -std::sin(t.jump_heading_rad) * kArrivalHurlPx;
  player.pos_y = std::cos(t.jump_heading_rad) * kArrivalHurlPx;
  player.vel_x = std::sin(player.heading) * arrival_speed;
  player.vel_y = -std::cos(player.heading) * arrival_speed;
  player.speed = arrival_speed;

  // Arrival overlay (0x0044f954 tail): when the destination system defines an
  // event message (SystemDef field_0x92 != -1) the original shows it instead
  // (System_ShowSystemEventMessage); otherwise a random "Entering the /
  // Jumping into the / Arriving in the" lead (STR# 0x7d2 0x2b..0x2d), the
  // system display name, " system on " (0x30), the formatted arrival date
  // (Stellar_FormatElapsedTravelTime -> NovaText_FormatDateString full month
  // names), and the no-nav appendix (0x31).
  if (dest_sys != nullptr) {
    if (dest_sys->message_id != -1) {
      System_ShowSystemEventMessage(state, dest_sys->message_id);
    } else {
      const std::uint16_t lead =
          0x2b + static_cast<std::uint16_t>(
                     std::uniform_int_distribution<int>{0, 2}(state.rng));
      std::string msg;
      if (const auto text = NovaHud_LoadStringEntry(0x7d2, lead)) {
        msg = *text;
      }
      msg += " ";
      msg += dest_sys->name;
      msg += " ";
      if (const auto text = NovaHud_LoadStringEntry(0x7d2, 0x30)) {
        msg += *text;
      }
      msg += " ";
      msg += NovaText_FormatDateString(
          state.date, false, state.date_prefix, state.date_suffix);
      msg += ".";
      // 0x31 "No stellar objects present." when the system defines no navs.
      bool has_navs = false;
      for (const std::int16_t nav : dest_sys->nav_defs) {
        if (nav >= 0x80) {
          has_navs = true;
          break;
        }
      }
      if (!has_navs) {
        if (const auto text = NovaHud_LoadStringEntry(0x7d2, 0x31)) {
          msg += " ";
          msg += *text;
        }
      }
      // Abandoned-fighter tally appendix (0x0044fc62): "  (" + count word +
      // " " + "fighter abandoned" (count == 1, 0xa4) / "fighters abandoned"
      // (0xa5) + ")". Note the original's two leading spaces.
      if (abandoned_fighters > 0) {
        msg += "  (";
        msg += FormatArrivalCountWord(abandoned_fighters,
                                      /*translate_first=*/true);
        msg += " ";
        const std::uint16_t word_id = abandoned_fighters == 1 ? 0xa4 : 0xa5;
        if (const auto text = NovaHud_LoadStringEntry(0x7d2, word_id)) {
          msg += *text;
        }
        msg += ")";
      }
      NovaHud_ShowOverlayMessage(state, msg, static_cast<std::uint64_t>(0xf0U));
    }
  }

  NovaLog::Info("hyperspace jump fired: system id {} (resource {}) after "
                "burning {} fuel",
                state.player.current_system_id,
                static_cast<int>(CurrentSystemResource(state)),
                static_cast<int>(kJumpFuelCost));

  // The plotted starmap destination and travel engagement are consumed by the
  // fire; the spaceflight loop re-spawns the starfield/asteroids for the new
  // system when it observes just_completed.
  t.travel_slot = -1;
  // @port 0x0044F660 50% gameplay,ui,synthetic
  // Ghidra 0x0044f660 PlayerTick_SystemTransitionAndArrival (synthetic region
  // of 0x0044aa70): destination transfer, arrival state, discovery, the arrival
  // overlay and route maintenance. Remaining TODO(decomp): the arrival reset
  // pass and route wipe-on-mismatch details.
  // Arrival clears the travel/landing stellar selection (g_travel_selected_
  // stellar_id = 0xffff at 0x0044f7fa) and the approach timer
  // (g_travel_engage_timer = 0xffff at 0x0044f803).
  t.selected_stellar_id = -1;
  t.engage_timer = -1;
  // Arrival re-latches the flight-hint state to 0x7fff (PlayerTick_System-
  // TransitionAndArrival 0x0044f83f), keeping the launch departure message
  // armed for the system's landings.
  t.travel_hint_state = 0x7fff;
  t.engaged_stellar_id = -1;
  t.starmap_destination_system_id = -1;
  t.destination_system_id = -1;
  t.hyperspace_mode = false;
  t.just_completed = true;
  // Arrival clears the plotted-jump latch (travel_transfer_mode = -1 at
  // 0x0044f660) so the nav panel drops back to the idle state.
  state.player.travel_transfer_mode = -1;
  // Mission offering reroll (0x0044f86f: 1000 x rand(100)+1 over the mission
  // definition table) -- the same pass Mission_RerollOfferingRolls
  // reconstructs for the landing arrival (the original fills all 1000 slots
  // unconditionally; the helper stops at the definition count, which is all
  // the consumers read).
  Mission_RerollOfferingRolls(state);
  // Arrival command grace: the Stellar_HandleStellarEntryAndExit rebuild
  // epilogue (0x004586a6) latches the spaceflight frame counter to -15, so
  // the special-interaction / mission-computer commands ignore input for 15
  // frames after arriving (held keys from the map do not trigger windows).
  state.arrival_command_grace_frames = 15;
  // Arrival closes the Escort Commands overlay (0x0044fe0b writes 0 to
  // g_target_category_panel_timer).
  state.escort.panel_timer = 0;
  // TODO(decomp(0x0044f803)) skipped: the remaining arrival resets target
  // globals the port does not model -- g_last_system_for_ambient_rolls
  // (0xffff), the interaction bribe latch (-1), DAT_00596d30/31 (0),
  // g_travel_countdown (0), the HUD dirty flags (immediate-mode rendering makes
  // them moot) and the starmap-window hide (Sprite_SetVisible 0x0044f857; the
  // map is modal in the port and cannot be open during flight). The interaction
  // action index draw and the ambient-traffic escalation re-arm are ported in
  // PlayerTick_JumpArrivalBlock.
  // Arrival route maintenance (Ship_HandlePlayerShipCore's arrival tick):
  // the map pan re-centres on the new system (0x0044f8a6, reading
  // g_system_defs_ptr[current_system_id].pos_x/y), a plotted route hop that
  // matched this system is consumed
  // (System_NormalizePlannedRouteToCurrentSystem 0x004a7fc0) and the travel
  // slot re-arms from the next hop
  // (NovaUi_SyncTravelSelectionFromStarmapRoute 0x004a8080).
  if (const auto *cur_sys = state.scenario.System(
          static_cast<std::int16_t>(state.player.current_system_id + 0x80))) {
    state.starmap_pan_x = static_cast<float>(cur_sys->pos_x);
    state.starmap_pan_y = static_cast<float>(cur_sys->pos_y);
  } else {
    state.starmap_pan_x = 0.0F;
    state.starmap_pan_y = 0.0F;
  }
  // Route wipe on mismatch (0x0044f9ec): when the next plotted hop is not
  // the system just entered, the whole 16-hop route is discarded wholesale;
  // when it matches, the hop is consumed by the normalize below. The
  // multi-jump continuation path (not ported) skips this wipe.
  if (t.starmap_route[1] != -1 &&
      NovaSystem_ResolveVisibleForTravel(state, t.starmap_route[1]) !=
          NovaSystem_ResolveVisibleForTravel(state,
                                             state.player.current_system_id)) {
    t.starmap_route.fill(-1);
  }
  NovaStarmap_NormalizeRouteToCurrentSystem(state);
  NovaStarmap_SyncTravelSelectionFromRoute(state);
  // The jump completes at the fire/arrival instant: the ship is now in the
  // NEW system at max speed and control returns to normal flight (the world
  // scrolls past as the ship coasts through the new system). There is no
  // separate in-tunnel phase after this.
  t.engaging = false;
  t.jump_phase = TravelState::JumpPhase::kIdle;
  t.tunnel_elapsed_60hz = 0.0F;
  t.hold_audio_latch = false;
  t.warp_up_started = false;
  t.jump_heading_rad = 0.0F;
  // The jump hold is over. The arrival handler later windows this to -999 for
  // the escort scatter before zeroing it; reset here so direct callers (tests)
  // do not leave the player station-held.
  player.ai_station_hold_timer = -1.0F;
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

// @port 0x0046efb0 100%
// Ghidra 0x0046efb0 Stellar_GetJumpSequenceDuration60Hz. The slots are filled
// by the preload (NovaAudio_PreloadGameplayData 0x004b0740) and keep the 350
// fallback for a missing/broken cue; see GameState::jump_duration_*.
[[nodiscard]] float
NovaTravel_JumpSequenceDuration60Hz(const GameState &state) {
  return static_cast<float>(state.x2_mode_active
                                ? state.jump_duration_noengine_60hz
                                : state.jump_duration_engine_60hz);
}

// ShipClassDef.jump_duration_multiplier for the player's current hull. The
// loader floors it at 0.5; 1.0 is the fallback for a missing/hostile class.
[[nodiscard]] float
NovaTravel_PlayerJumpDurationMultiplier(const GameState &state) {
  const std::int16_t class_id = state.player.ship_class_id;
  if (class_id >= 0 &&
      static_cast<std::size_t>(class_id) < state.scenario.ships.size()) {
    return state.scenario.ships[static_cast<std::size_t>(class_id)]
        .jump_duration_multiplier;
  }
  return 1.0F;
}

// @port 0x00456CA0 75% rng,rendering
// Ghidra 0x00456ca0 Stellar_EnterWormhole.
std::int16_t
NovaTravel_SelectWormholeDestination(GameState &state,
                                     std::int16_t source_stellar_id) {
  const Stellar *source = state.scenario.Stellar(source_stellar_id);
  if (source == nullptr || (source->availability_flags & 0x2000U) == 0U) {
    return -1;
  }
  std::vector<std::int16_t> candidates;
  for (const std::int16_t link : source->hyperlinks) {
    const std::int16_t system = StellarSystem(state, link);
    if (system >= 0 && NovaSystem_ResolveVisibleForTravel(state, system) >= 0) {
      candidates.push_back(link);
    }
  }
  if (candidates.empty()) {
    const bool source_has_links =
        std::any_of(source->hyperlinks.begin(),
                    source->hyperlinks.end(),
                    [](std::int16_t link) { return link >= 0x80; });
    if (source_has_links) {
      return -1;
    }
    for (std::size_t i = 0; i < state.scenario.stellars.size(); ++i) {
      const std::int16_t id = static_cast<std::int16_t>(i + 0x80);
      const Stellar &candidate = state.scenario.stellars[i];
      if (id == source_stellar_id || !candidate.is_available ||
          (candidate.availability_flags & 0x2000U) == 0U ||
          std::any_of(candidate.hyperlinks.begin(),
                      candidate.hyperlinks.end(),
                      [](std::int16_t link) { return link >= 0x80; })) {
        continue;
      }
      const std::int16_t system = StellarSystem(state, id);
      if (system >= 0 && system != state.player.current_system_id &&
          NovaSystem_ResolveVisibleForTravel(state, system) >= 0) {
        candidates.push_back(id);
      }
    }
  }
  if (candidates.empty()) {
    return -1;
  }
  return candidates[std::uniform_int_distribution<std::size_t>{
      0, candidates.size() - 1}(state.rng)];
}

// @port 0x00456480 65% gameplay,rendering
// Ghidra 0x00456480 Stellar_EnterHypergate; destination validation after
// NovaUi_RunStarmapWindow returns in its linked-destination mode.
std::int16_t
NovaTravel_ResolveHypergateDestination(const GameState &state,
                                       std::int16_t source_stellar_id,
                                       std::int16_t selected_system_id) {
  const Stellar *source = state.scenario.Stellar(source_stellar_id);
  if (source == nullptr || (source->availability_flags & 0x1000U) == 0U ||
      selected_system_id < 0) {
    return -1;
  }
  for (const std::int16_t link : source->hyperlinks) {
    const std::int16_t system = StellarSystem(state, link);
    if (system >= 0 && NovaSystem_ResolveVisibleForTravel(state, system) ==
                           selected_system_id) {
      return link;
    }
  }
  return -1;
}

// Ghidra 0x00456480 Stellar_EnterHypergate and 0x00456ca0
// Stellar_EnterWormhole share this system-entry body inline.
bool NovaTravel_CompleteRestrictedTravel(GameState &state,
                                         std::int16_t destination_stellar_id,
                                         RestrictedTravelKind kind) {
  const Stellar *destination = state.scenario.Stellar(destination_stellar_id);
  const std::int16_t stored_system =
      StellarSystem(state, destination_stellar_id);
  const std::int16_t destination_system =
      NovaSystem_ResolveVisibleForTravel(state, stored_system);
  if (destination == nullptr || destination_system < 0) {
    return false;
  }
  PlayerShip &player = state.player;
  player.jump_destination_system_id = player.current_system_id;
  player.current_system_id = destination_system;
  NovaWeapon_ClearTransientCombatState(state);
  NovaSystem_OnSystemEntered(state, destination_system, 1);
  state.starmap_pan_x =
      static_cast<float>(state.scenario.systems[destination_system].pos_x);
  state.starmap_pan_y =
      static_cast<float>(state.scenario.systems[destination_system].pos_y);
  player.pos_x = static_cast<float>(destination->pos_x);
  player.pos_y = static_cast<float>(destination->pos_y);
  const std::int16_t heading_deg =
      destination->emergence_angle_deg.has_value() &&
              *destination->emergence_angle_deg >= 0 &&
              *destination->emergence_angle_deg <= 359
          ? *destination->emergence_angle_deg
          : static_cast<std::int16_t>(
                std::uniform_int_distribution<int>{0, 359}(state.rng));
  player.heading =
      static_cast<float>(heading_deg) * (std::numbers::pi_v<float> / 180.0F);
  player.speed = PlayerMaxSpeed(state) * 0.5F;
  player.vel_x = std::sin(player.heading) * player.speed;
  player.vel_y = -std::cos(player.heading) * player.speed;
  NovaHud_ShowOverlayMessage(
      state,
      RestrictedArrivalMessage(
          state, state.scenario.systems[destination_system], kind),
      static_cast<std::uint64_t>(
          kind == RestrictedTravelKind::kHypergate ? 0x168U : 0xf0U));
  state.warp_out_sound_pending = true;
  state.no_asteroids_latch = true;
  // White PaintRect at the destination frame. The Mac hypergate then runs the
  // gated _FadeWhiteOut (1.5 s display fade, 0x63c40); the wormhole instead
  // resets the starfield, so it keeps the one-frame flash with no fade.
  state.screen_flash_intensity = 1.0F;
  state.screen_flash_mode = kind == RestrictedTravelKind::kHypergate
                                ? GameState::ScreenFlashMode::kFadeOut
                                : GameState::ScreenFlashMode::kInstant;
  state.travel.starmap_route.fill(-1);
  state.travel.starmap_route[0] = destination_system;
  state.travel.travel_slot = -1;
  state.travel.starmap_destination_system_id = -1;
  state.travel.destination_system_id = -1;
  state.travel.selected_stellar_id = -1;
  state.travel.engaged_stellar_id = -1;
  state.travel.engage_timer = -1;
  state.travel.hyperspace_mode = false;
  state.travel.engaging = false;
  state.travel.just_completed = true;
  state.travel.pending_payroll_periods = 0;
  player.travel_transfer_mode = -1;
  player.ai_secondary_target_slot = -1;
  player.primary_target_ship_slot = -1;
  state.arrival_command_grace_frames = 15;
  return true;
}

// Ghidra 0x0044aa70 Ship_HandlePlayerShipCore, payroll tail at 0x0044fef8.
void NovaTravel_ProcessArrivalPayroll(
    GameState &state,
    const std::function<void(const std::string &)> &show_text) {
  const auto periods = std::exchange(state.travel.pending_payroll_periods, 0);
  if (periods > 0) {
    Player_ProcessEscortPayroll(state, periods, show_text);
  }
}

bool NovaTravel_PlayerInJumpRange(const GameState &state) {
  const System *sys = state.scenario.System(CurrentSystemResource(state));
  if (sys == nullptr) {
    return false;
  }
  const float range_sq = NovaTargeting_ComputeTravelRangeSq(state);
  for (const std::int16_t nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue; // no travel point in this slot
    }
    const Stellar *st = state.scenario.Stellar(nav);
    if (st == nullptr || (st->availability_flags & 0x3000U) != 0U) {
      continue; // restricted travel stellars do not gate the probe
    }
    const float dist_sq = state.player.pos_x * state.player.pos_x +
                          state.player.pos_y * state.player.pos_y;
    if (dist_sq <= range_sq) {
      return false;
    }
  }
  return true;
}

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
  // with no travel stellar there, yet 'j' still jumps Kania->Tichel). Latching
  // plotted-jump mode 3 clears the stellar selection (the original's
  // g_travel_selected_stellar_id = -1 in PlayerTick_HyperspaceCommand): the
  // travel reticle hides and the nav panel switches to the Hyperspace
  // destination display.
  t.travel_slot = static_cast<std::int16_t>(slot);
  t.destination_system_id = destination_zero_based;
  // Plotted-jump mode latch (0x004a491e): the accent line and the jump HUD
  // read travel_transfer_mode == 3.
  state.player.travel_transfer_mode = 3;
  const std::int16_t stellar_id = sys->nav_defs[static_cast<std::size_t>(slot)];
  t.engaged_stellar_id = stellar_id >= 0x80 ? stellar_id : -1;
  t.selected_stellar_id = -1;
  t.selected_stellar_is_manual = false;
  NovaLog::Info("plotted starmap jump to system {} (hyperlink slot {})",
                destination_zero_based,
                slot);
  return true;
}

// @port 0x00462DB0 90% gameplay,divergence
// DIVERGENCE(original): per-stellar is_available is treated as present, and
// the port no longer auto-seeds this on a per-frame cadence.
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

// @port 0x00415b80 100%
// Ghidra 0x00415b80 Stellar_CanShipInitiateJumpSequence, for an arbitrary
// (NPC) ship. Gates on the ship's OWN class fuel capacity being at least one
// jump (kJumpFuelCost), NOT the player's. Blocks while the ship is locked to
// another ship's velocity match (velocity_match_target_ship_slot != -1 and !=
// own instance id), and, for a personality whose Flags2 0x0001 means
// "starts with zero fuel", while the current tank is below one jump.
// Ordinary ships are NOT gated on current fuel; the current-fuel term that
// shows the player's "not enough fuel to take off" denial lives only in
// Stellar_HandlePlayerShipCore's jump block, not in this shared gate. The NPC
// transfer path (NovaAi_CompleteNpcJump) charges no fuel, so a freshly spawned
// fleet lead with fuel_points 0 can still jump.
bool NovaTravel_CanShipInitiateJumpSequence(const GameState &state,
                                            const Ship &ship) {
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const float class_fuel_capacity =
      cls ? static_cast<float>(cls->base_fuel) : 0.0F;
  if (class_fuel_capacity < kJumpFuelCost) {
    return false;
  }
  if (ship.velocity_match_target_ship_slot != -1 &&
      ship.velocity_match_target_ship_slot != ship.ship_instance_id) {
    return false;
  }
  if (ship.pers_def_slot != -1 && static_cast<std::size_t>(ship.pers_def_slot) <
                                      state.scenario.pers_defs.size()) {
    const PersDef &pers =
        state.scenario.pers_defs[static_cast<std::size_t>(ship.pers_def_slot)];
    if ((pers.flags_secondary & 0x0001) != 0 &&
        ship.fuel_points < kJumpFuelCost) {
      return false;
    }
  }
  return true;
}

// @port 0x00447F00 100%
// Ghidra 0x00447f00 System_IsSystemVisible.
bool NovaSystem_IsSystemVisible(const GameState &state,
                                std::int16_t system_id) {
  if (system_id < 0 ||
      static_cast<std::size_t>(system_id) >= state.scenario.systems.size()) {
    return false;
  }
  return state.scenario.systems[static_cast<std::size_t>(system_id)].is_visible;
}

// @port 0x0046B9B0 100%
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

// @port 0x00468af0 100%
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

// @port 0x0046C250 100%
// Ghidra 0x0046c250 System_GetEffectiveMurkPercent. See the header for the
// decoded formula. The original's loop walks all 0x200 outfit slots, tests the
// signed owned count > 0 and then each of the four (ModType, ModVal) pairs for
// ModType 0x1c, accumulating owned_count * ModVal; the total is clamped to
// [0, 100].
std::int16_t NovaSystem_GetEffectiveMurkPercent(const GameState &state) {
  int murk = 0;
  if (state.player.current_system_id >= 0 &&
      static_cast<std::size_t>(state.player.current_system_id) <
          state.scenario.systems.size()) {
    murk = std::max<int>(
        0,
        state.scenario
            .systems[static_cast<std::size_t>(state.player.current_system_id)]
            .murk);
  }
  const auto &owned = state.inventory.outfit_owned_count;
  for (std::size_t i = 0; i < owned.size() && i < state.scenario.outfits.size();
       ++i) {
    const std::int16_t count = owned[i];
    if (count <= 0) {
      continue;
    }
    const Outfit &outfit = state.scenario.outfits[i];
    const std::int16_t mod_types[4] = {outfit.mod_type,
                                       outfit.alt_mod_types[0],
                                       outfit.alt_mod_types[1],
                                       outfit.alt_mod_types[2]};
    const std::int16_t mod_vals[4] = {outfit.mod_val,
                                      outfit.alt_mod_vals[0],
                                      outfit.alt_mod_vals[1],
                                      outfit.alt_mod_vals[2]};
    for (int slot = 0; slot < 4; ++slot) {
      // OutfitEffect::kMurkMod == 0x1c (28): +/- current system murkiness.
      if (mod_types[slot] == 0x1c) {
        murk += static_cast<int>(count) * mod_vals[slot];
      }
    }
  }
  return static_cast<std::int16_t>(std::clamp(murk, 0, 100));
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
}

// Ghidra 0x00448be0 NovaExpression_EvaluateToken, 'E' token. The original tests
// `0 < g_system_defs_ptr[resource_id - 0x80].discovery_state`; is_visible /
// has_explored_flag are loader-set availability flags, not fog (see
// scenario_data.hpp).
bool NovaSystem_HasExploredToken(const GameState &state,
                                 std::int16_t resource_id) {
  if (resource_id < 0x80 || resource_id >= 0x880) {
    return false;
  }
  const std::size_t idx = static_cast<std::size_t>(resource_id - 0x80);
  if (idx >= state.scenario.systems.size()) {
    return false;
  }
  return state.scenario.systems[idx].discovery_state > 0;
}

// @port 0x00467ab0 95% gameplay
// Ghidra 0x00467ab0 System_FloodDiscoverAdjacentSystems. Depth-gated
// recursion through NovaSystem_ResolveVisibleForTravel (0x0046b920).
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

// @port 0x00467970 90% gameplay
// Ghidra 0x00467970 System_RebuildSystemVisibilityMap. Clears the flood mask,
// floods the origin at (max_depth, threshold), then rebuilds the one-jump-ahead
// discovered_this_rebuild latch. The port shares the latch pass with
// NovaSystem_RebuildDiscoveredLatch (0x00432470 scope B).
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

  NovaSystem_RebuildDiscoveredLatch(state);
}

void NovaSystem_RebuildDiscoveredLatch(GameState &state) {
  // Twin propagation (System_UpdateSystemAndStellarDisplayState 0x00432470
  // scope B first loop): a system inherits the max discovery_state of its
  // discovery-slot twin.
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    auto &sys = state.scenario.systems[i];
    if (!sys.has_explored_flag) {
      continue;
    }
    const std::int16_t slot =
        NovaSystem_ResolveDiscoverySlot(state, static_cast<std::int16_t>(i));
    if (slot == static_cast<std::int16_t>(i) || slot < 0 ||
        static_cast<std::size_t>(slot) >= state.scenario.systems.size()) {
      continue;
    }
    const auto &twin = state.scenario.systems[static_cast<std::size_t>(slot)];
    if (sys.discovery_state < twin.discovery_state) {
      sys.discovery_state = twin.discovery_state;
    }
  }

  // Latch pass (0x00432470 scope B tail, also the 0x00467970 rebuild tail):
  // every visible visited system latches itself, and so does every travel-
  // resolvable link neighbour — that latch is the starmap's one-jump-ahead
  // window. Invisible twin clones never latch and never show on the map.
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

// @port 0x00467bd0 80% gameplay
// Ghidra 0x00467bd0 Frame_TriggerSystemRegionEvents.
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
    return Outfit_PlayerHasOutfitForControlExpression(state, id);
  };
  expression.has_explored = [&state](std::int16_t id) {
    return NovaSystem_HasExploredToken(state, id);
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
    // (0x00467cc1); the full opcode grammar applies, not just control bits.
    Mission_ExecuteReactionScript(state,
                                  neb.on_explore_expression,
                                  MissionScriptContext{"nebula OnExplore"});
  }
}

void NovaSystem_OnSystemEntered(GameState &state,
                                std::int16_t zero_based_system_id,
                                std::int16_t level) {
  // Arrival pre-latch: the entered system and its discovery slot are booked as
  // visited even before the flood (0x0044aa70 writes level 1 for hyperspace
  // arrivals, 0x00455e10 Stellar_RunDockAndLaunchSequence writes level 2).
  NovaSystem_MarkSystemVisited(state, zero_based_system_id, level);
  NovaSystem_MarkSystemVisited(
      state,
      NovaSystem_ResolveDiscoverySlot(state, zero_based_system_id),
      level);
  NovaSystem_RebuildDiscoveryState(state, zero_based_system_id, 0, level);
}

// ---------------------------------------------------------------------------
// Destination-system cycling (key binding 13; Ghidra 0x0044b8b9..0x0044def6 in
// PlayerTick_TravelSelectionCommands, g_playerCycleTravelTargetCommandLatch).
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
  // Cycle latches plotted-jump mode 3 like the starmap arm (0x0044dea7);
  // mode 3 clears the stellar selection (reticle hides -- see
  // PlayerTick_HyperspaceCommand).
  state.player.travel_transfer_mode = 3;
  const std::int16_t stellar_id = sys->nav_defs[static_cast<std::size_t>(slot)];
  t.engaged_stellar_id = stellar_id >= 0x80 ? stellar_id : -1;
  t.selected_stellar_id = -1;
  t.selected_stellar_is_manual = false;
  NovaLog::Info(
      "cycled destination system to {} (hyperlink slot {})", dest, slot);
  return dest;
}

// Advances the hyperspace flash by one render frame. kFadeIn and kFadeOut are
// the Mac 1.5 s _FadeWhiteIn/_FadeWhiteOut display fades. kBuildup is the
// pre-trigger hold state; the Mac scalar only requests the fade once when it
// becomes positive. kInstant is the original ~60 ms one-frame fallback for
// the wormhole and the disabled-jump collapse.
void NovaTravel_AdvanceScreenFlash(GameState &state, float frame_time_ms) {
  switch (state.screen_flash_mode) {
  case GameState::ScreenFlashMode::kFadeIn:
    state.screen_flash_intensity =
        std::min(1.0F, state.screen_flash_intensity + frame_time_ms / 1500.0F);
    break;
  case GameState::ScreenFlashMode::kFadeOut:
    state.screen_flash_intensity =
        std::max(0.0F, state.screen_flash_intensity - frame_time_ms / 1500.0F);
    break;
  case GameState::ScreenFlashMode::kInstant:
    state.screen_flash_intensity =
        std::max(0.0F, state.screen_flash_intensity - frame_time_ms / 60.0F);
    break;
  case GameState::ScreenFlashMode::kNone:
  case GameState::ScreenFlashMode::kBuildup:
    break;
  }
  if (state.screen_flash_intensity <= 0.0F &&
      state.screen_flash_mode != GameState::ScreenFlashMode::kBuildup &&
      state.screen_flash_mode != GameState::ScreenFlashMode::kFadeIn) {
    state.screen_flash_mode = GameState::ScreenFlashMode::kNone;
  }
}

// ---------------------------------------------------------------------------
// Cross-system jump state machine.
// ---------------------------------------------------------------------------
// Ghidra Ship_HandlePlayerShipCore 0x0044AA70, composed from disjoint internal
// CFGs: engage 0x0044C18A; status-overlay clear 0x0044C310 -> 0x0044C31D;
// escort jump warning 0x0044C31D -> 0x0044C4DA; completion audio
// 0x0044C75C -> 0x0044C84D; tunnel 0x0044CCAF -> 0x0044CFFE; fire/arrival
// 0x0044F3D0 and 0x0044F660; turnaround 0x0044FFF0; and flight-tail range cue
// 0x0044D0C3 -> 0x00450717. These remain one coupled travel state machine.
void NovaTravel_Tick(GameState &state,
                     bool travel_input,
                     float frame_time_ms,
                     bool warp_up_sound_active) {
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

    // Jump-sequence clock (60 Hz ticks since the mode stamp): stamped at the
    // engage and re-stamped when the stationary hold begins (ai_mode_start_
    // time_ms, 0x0044c4e9), read by both the tunnel ramp and the collapse.
    t.tunnel_elapsed_60hz += frame_time_ms * (kHyperspaceTickHz / 1000.0F);

    // @port 0x0044B037 85% rendering,synthetic
    // Ghidra 0x0044b037 PlayerTick_HyperspaceExitGate (synthetic region of
    // 0x0044aa70): becoming disabled between engage and fire aborts the jump.
    // The gate is ported; TODO(decomp): whether the Mac abort path reaches the
    // hold-end _FadeWhiteOut.
    // Disabled-jump collapse (Ship_HandlePlayerShipCore 0x0044b037 gate).
    // Becoming disabled any time between the engage and the fire aborts the
    // jump on that frame: hold timer = -1, the 'Warp up' cue is cancelled,
    // the centered 'boom' effect 0x32 queues (white flash + Warp out sound),
    // and STR# 0x7d2 0x23 overlays. NO system change -- the ship stays in
    // the current system. The exit velocity clause only applies once the
    // tunnel ramp had begun (progress past the onset threshold): velocity is
    // zeroed then set to min(progress, max speed) along the heading; earlier
    // in the sequence the velocity is left as-is (the turnaround damping has
    // it near zero). The hold accumulator freezing (0x0044c940) is subsumed
    // by the abort.
    if (NovaAiShip_IsDisabled(state, player)) {
      const float jump_multiplier =
          NovaTravel_PlayerJumpDurationMultiplier(state);
      const float progress = t.tunnel_elapsed_60hz * jump_multiplier /
                                 (NovaTravel_JumpSequenceDuration60Hz(state) *
                                  kJumpDurationScale) -
                             kJumpProgressOffset / jump_multiplier;
      // @port 0x0044B120 100% synthetic
      // Ghidra 0x0044b120 PlayerTick_HyperspaceExitVelocity (synthetic region
      // of 0x0044aa70): once the tunnel ramp has begun, the collapse zeroes
      // velocity and sets it to min(progress, max speed) along the heading.
      if (progress > kJumpProgressOnsetThreshold) {
        const float speed = std::min(progress, PlayerMaxSpeed(state));
        player.vel_x = std::sin(player.heading) * speed;
        player.vel_y = -std::cos(player.heading) * speed;
        player.speed = speed;
      }
      // The centered 'boom' effect 0x32 (white flash + Warp out sound). The
      // Windows build does not fade here; the Mac top block only runs
      // _FadeWhiteOut once ai_station_hold_timer <= 0, so this abort keeps the
      // legacy one-frame flash (TODO(decomp): confirm the Mac abort path).
      state.screen_flash_intensity = 1.0F;
      state.screen_flash_mode = GameState::ScreenFlashMode::kInstant;
      state.warp_out_sound_pending = true;
      state.warp_up_cancel_pending = true;
      t.jump_phase = TravelState::JumpPhase::kIdle;
      t.engaging = false;
      t.tunnel_elapsed_60hz = 0.0F;
      t.hold_audio_latch = false;
      t.warp_up_started = false;
      player.ai_station_hold_timer = -1.0F;
      // The plotted destination stays armed (the original keeps travel_
      // transfer_mode 3 and the secondary target); the player can re-engage
      // once repaired.
      const auto text = NovaHud_LoadStringEntry(0x7d2, 0x23);
      NovaHud_ShowOverlayMessage(
          state, text.value_or(""), 0xfa, 0x00, 0x0c, 0xf0U);
      NovaLog::Info("hyperspace field collapsed: jump disabled mid-sequence");
      // Fall through (no return): the original's flight tail still runs in
      // the collapse frame, so the jump-range cue can fire on the same tick.
    }

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
    // @port 0x0044F127 80% gameplay,rendering,synthetic
    // Ghidra 0x0044f127 PlayerTick_JumpTurnaroundContinuation (synthetic region
    // of 0x0044aa70): ordinary hyperspace-engage turnaround/braking. Partial
    // coverage: the downstream 0x0044cffe manual tails (speed-cap clamp, 0.985
    // fire-restricted decay, glow ramp) are unported here and the brake frame
    // does not stamp ai_station_hold_timer = 1.0.
    case TravelState::JumpPhase::kBrake: {
      // Pre-fire turn-around, mirroring the jump dispatch in
      // Ship_HandlePlayerShipCore (0x0044c195 engage / 0x0044fff0 continuation
      // anchor): while |round(vel)| >= 2 on either axis the ship faces the
      // REVERSE of its velocity (bearing of vel*100 from the origin), brakes
      // by g_jump_turnaround_velocity_damp (0x99204) per 30 Hz tick, and once
      // within the facing window (max(class turn + 1, 20) deg -- a gate, not a
      // turn speed) applies effective thrust back along it to stop the ship.
      // Ends at the original's |trunc(vel_x)| < 2 && |trunc(vel_y)| < 2 stop
      // test -- the hold begins and the 'Warp up' cue pre-stages there
      // (0x0044c4e9).
      // Ship_CheckSpecialLoadoutCapability (0x0044c4db, Ghidra 0x0046d080):
      // a hull with class Flags2 0x0020 or the ModType-37 fast-jumping outfit
      // skips the per-axis stop requirement and enters the hold directly.
      // Ordinary hulls still compare |trunc(vel)| < 2 on both axes.
      const bool fast_jump = NovaOutfit_HasFastJumpCapability(state, player);
      const bool stopped =
          fast_jump ||
          (std::abs(std::trunc(player.vel_x)) < kStoppedRoundedVel &&
           std::abs(std::trunc(player.vel_y)) < kStoppedRoundedVel);
      if (!stopped) {
        // Inertialess jump brake (Ghidra 0x0044f0e3): decay scalar speed
        // (+0x48) by Ship_ComputeShipEffectiveThrust * tick scale, floored at
        // zero, then shared 0x0044cffe/0x0043b020 steering toward
        // heading*speed.
        // TODO(decomp): scalar cap, fire-restricted 0.985 decay, glow ramp
        // unported (fade_glow placeholder).
        if (NovaPlayer_IsInertialess(state)) {
          const float thrust = PlayerThrust(state);
          player.speed = std::max(0.0F, player.speed - thrust * ticks);
          NovaShip_SteerVelocityTowardShipHeading(player, thrust, ticks);
          fade_glow();
          player.pos_x += player.vel_x * ticks;
          player.pos_y += player.vel_y * ticks;
        } else {
          // Still moving: turn around to face the reverse of the velocity.
          const float vel_heading = std::atan2(player.vel_x, -player.vel_y);
          float desired =
              std::fmod(vel_heading + 3.14159265358979323846F, kTwoPi);
          if (desired < 0.0F) {
            desired += kTwoPi;
          }
          turn_toward(desired, class_turn_deg);

          // Once facing the reverse heading within the facing window, punch
          // back along it (Ship_ComputeShipEffectiveThrust toward the heading)
          // and ramp the engine glow; otherwise the glow fades.
          const float delta_deg =
              std::abs(std::remainder(desired - player.heading, kTwoPi)) *
              (180.0F / 3.14159265358979323846F);
          if (delta_deg < turn_align_window) {
            const float thrust = PlayerThrust(state) * kBrakeThrustScale;
            player.vel_x += std::sin(player.heading) * thrust * ticks;
            player.vel_y += -std::cos(player.heading) * thrust * ticks;
            ramp_glow(kGlowFastStep);
          } else {
            fade_glow();
          }
          // Brake (g_jump_turnaround_velocity_damp, applied per 30 Hz tick) and
          // keep the ship coasting through the deceleration.
          const float damp = std::pow(kTurnDamp, ticks);
          player.vel_x *= damp;
          player.vel_y *= damp;
          player.speed = std::hypot(player.vel_x, player.vel_y);
          player.pos_x += player.vel_x * ticks;
          player.pos_y += player.vel_y * ticks;
        }
      } else {
        // Stopped: begin the stationary hold. The original seeds the station-
        // hold timer to 2.0 (0x0044c548), stamps the 60 Hz jump clock
        // (ai_mode_start_time_ms at 0x0044c54f) and pre-stages the 'Warp up'
        // cue. The timer doubles as the hold clock and the flag the escort
        // jump sync (Ship_SyncJumpStateToSquad) and the player-led jump
        // spin-up (Ship_HandleShip 0x00433050) read, so it MUST stay positive
        // while the hold runs.
        t.jump_phase = TravelState::JumpPhase::kHold;
        player.ai_station_hold_timer = 2.0F;
        player.ai_mode_start_time_ms = state.tick_60hz;
        t.hold_audio_latch = false;
        // The hold-begin block re-stamps the tunnel clock; the schedule runs
        // from here.
        t.tunnel_elapsed_60hz = 0.0F;
        state.screen_flash_intensity = 0.0F;
        state.screen_flash_mode = GameState::ScreenFlashMode::kBuildup;
        state.screen_flash_fade_in_started = false;
        // The hold-begin block also latches the flight-hint state to 0x7fff
        // (Ship_HandlePlayerShipCore 0x0044c561), arming the launch departure
        // message for every landing until a pre-jump landing consumes it.
        t.travel_hint_state = 0x7fff;
        if (!t.warp_up_started) {
          t.warp_up_started = true;
          state.warp_up_sound_pending = true;
        }
      }
      break;
    }
    case TravelState::JumpPhase::kHold: {
      // Ghidra 0x0044c705: every tick of the engage hold opens with the squad
      // sync (Ship_SyncJumpStateToSquad) -- escorts with no stellar attachment
      // copy the leader's hold clock, drop their target (-2 sentinel) and
      // enter AI state 0x0B, holding formation until the jump fires. They
      // transfer systems at arrival via escort adoption, not here.
      NovaAi_SyncJumpStateToSquad(state, player, state.tick_60hz);
      // @port 0x00467e60 100% divergence
      // DIVERGENCE(original): the Windows build calls the bare-RET
      // NoSys_NoOp_00467e60 and its fade build-up is dead; the port follows
      // the Mac _FadeWhiteIn display behaviour unconditionally.
      // Ghidra 0x00467e60 NoSys_NoOp_00467e60.
      // Mac progressive white fade-in (the top block of Ship_HandlePlayer-
      // ShipCore 0x0044aa70; Mac _HandlePlayer ~0x683bf): the tunnel scalar
      // FLOAT_007354a0 is clamped [0,100], and a positive value requests one
      // asynchronous 1.5 s _FadeWhiteIn. It is a trigger, not continuously
      // sampled opacity. The Windows build computes progress * 0.3 - 15
      // (0x00450601) and calls the stubbed NoSys_NoOp_00467e60, so its build-up
      // is dead; the port follows the Mac display behaviour unconditionally.
      const float jump_multiplier =
          NovaTravel_PlayerJumpDurationMultiplier(state);
      const float progress = t.tunnel_elapsed_60hz * jump_multiplier /
                                 (NovaTravel_JumpSequenceDuration60Hz(state) *
                                  kJumpDurationScale) -
                             kJumpProgressOffset / jump_multiplier;
      const float fade_trigger = (progress - 55.0F) * 5.0F;
      if (!state.screen_flash_fade_in_started && fade_trigger > 0.0F) {
        state.screen_flash_fade_in_started = true;
        state.screen_flash_intensity = 0.0F;
        state.screen_flash_mode = GameState::ScreenFlashMode::kFadeIn;
      }
      // Stationary alignment hold (the hold branch of the disabled
      // window, decompile around 0x0044f3d0): velocity damps by
      // g_hyperspace_slow_phase_velocity_damp (0.98007) per tick, the hull
      // turns onto the map-space bearing toward the destination system (the
      // per-frame ai_desired_heading_deg = Bearing(cur,dest) re-aim at
      // LAB_0044edfd) at the class turn rate, and the engine glow fades. The
      // fire lands once the hold passes g_hyperspace_engage_hold_30hz (30
      // ticks) AND the 'Warp up' cue is no longer active
      // (NovaAudio_CountActiveByHandle latch, g_playerHyperspaceAudioLatch).
      // Ship_CheckSpecialLoadoutCapability (0x0044c72e, Ghidra 0x0046d080):
      // fast-jump hulls keep their momentum through the hold -- the slow-phase
      // velocity damp only runs for ordinary hulls.
      const bool fast_jump = NovaOutfit_HasFastJumpCapability(state, player);
      const bool inertialess = NovaPlayer_IsInertialess(state);
      if (!fast_jump) {
        const float damp = std::pow(kSlowPhaseVelDamp, ticks);
        player.vel_x *= damp;
        player.vel_y *= damp;
        // Ordinary hulls derive the scalar speed from the damped vector;
        // inertialess hulls keep the maintained +0x48 scalar authoritative
        // (the original's 0x0044f414 damp writes vel_x/vel_y only).
        if (!inertialess) {
          player.speed = std::hypot(player.vel_x, player.vel_y);
        }
      }
      fade_glow();
      // Align onto the jump heading at the class turn rate. The original
      // stores the integer map bearing in the player's ai_desired_heading_deg
      // (0x0044eeb3) and turns through the shared auto-turn arm; the escorts'
      // mode-0xD spin-up mirrors that field, so it must be live while the hold
      // runs (a stale/zero value sends them to the leader-desired fallback and
      // they point straight up).
      int jump_heading_deg = static_cast<int>(
          std::lround(t.jump_heading_rad * (180.0F / 3.14159265358979323846F)));
      jump_heading_deg = ((jump_heading_deg % 360) + 360) % 360;
      player.ai_desired_heading_deg =
          static_cast<std::int16_t>(jump_heading_deg);
      turn_toward(
          t.jump_heading_rad,
          std::max(std::round(state.cached_stats.turn_raw * 0.1F), 1.0F));
      // Shared manual-flight inertialess tail (Ghidra 0x0044cffe ->
      // 0x0044d05b, PlayerTick_InertialessSteering): the engaged hold still
      // reaches it, so an inertialess hull -- including fast-jump+inertialess
      // -- steers its velocity toward heading*speed while the hold turns onto
      // the jump bearing. It runs after the slow damp and after the auto-turn,
      // and before position integration, matching the original frame order.
      // TODO(decomp): the shared tail's g_player_speed_cap_x scalar clamp,
      // fire-restricted 0.985 decay and speed-proportional glow ramp remain
      // unported in this suspended-movement path.
      if (inertialess) {
        NovaShip_SteerVelocityTowardShipHeading(
            player, PlayerThrust(state), ticks);
      }
      // Keep integrating the residual drift; the fire overwrites it.
      player.pos_x += player.vel_x * ticks;
      player.pos_y += player.vel_y * ticks;

      player.ai_station_hold_timer += ticks;
      // @port 0x0044D371 45% gameplay,synthetic
      // Ghidra 0x0044d371 PlayerTick_HyperspaceProgressBranch (synthetic region
      // of 0x0044aa70): hold/fire audio cadence. Escape-pod/disabled cases
      // remain TODO(decomp).
      if (player.ai_station_hold_timer > kEngageHoldTicks &&
          !warp_up_sound_active) {
        t.hold_audio_latch = true;
      }
      if (player.ai_station_hold_timer >= kEngageHoldTicks &&
          t.hold_audio_latch) {
        // Boom/arrival: full-screen flash + 'Warp out' boom + the 1350 px
        // hurl + system change (see FireJump). Control returns to normal
        // flight immediately.
        FireJump(state);
      }
      if (t.jump_phase != TravelState::JumpPhase::kHold) {
        // The fire landed this frame; the original's arrival block resets the
        // hold timer before its tunnel block runs, so the tunnel motion is
        // skipped on the fire frame.
        break;
      }
      // @port 0x0044CCAF 90% rendering,synthetic
      // Ghidra 0x0044ccaf PlayerTick_HyperspaceTunnelAcceleration (synthetic
      // region of 0x0044aa70). TODO(decomp): the starfield streak pass and its
      // scalar/cadence.
      // In-tunnel acceleration (the tunnel block of Ship_HandlePlayerShipCore,
      // after the hold/fire branch): once the stopped hull faces the jump
      // bearing within max(class turn, 30 deg), the position advances along
      // the heading by min(progress, 50) px/tick -- a direct position step,
      // NOT thrust into vel_x/vel_y (which stay damped near zero) -- and the
      // engine glow overdrives +4/tick up to 32, past the normal-thrust cap of
      // 24. With the verified 60 Hz tick units, multiplier=1 has ~2.1 s of
      // stationary alignment while the cue rises, the ramp to the 50 px/tick
      // cap by ~5.2 s, and the boom when the cue finishes (6.08 s /
      // multiplier).
      // TODO(decomp): the original also has a starfield streak render pass
      // during the tunnel; its source scalar and exact cadence remain
      // unresolved here.
      const float heading_deg =
          player.heading * (180.0F / 3.14159265358979323846F);
      const float jump_deg =
          t.jump_heading_rad * (180.0F / 3.14159265358979323846F);
      const float align_delta_deg =
          std::abs(std::remainder(jump_deg - heading_deg, 360.0F));
      if (align_delta_deg <= std::max(class_turn_deg, kTunnelAlignDeg)) {
        if (progress > 0.0F) {
          const float step = std::min(progress, kTunnelSpeedCap) * ticks;
          player.pos_x += std::sin(player.heading) * step;
          player.pos_y += -std::cos(player.heading) * step;
          player.engine_glow_level =
              std::min<std::int16_t>(0x20, player.engine_glow_level + 4);
          refresh_glow();
        }
      }
      break;
    }
    case TravelState::JumpPhase::kIdle:
    default:
      break;
    }
    // The original's flight tail runs after the phase machine even while
    // engaged: the range latch keeps updating, but the cue is suppressed by
    // the hold timer (t.engaging here).
    TickJumpRangeCue(state);
    return;
  }

  // @port 0x0044C18A 60% gameplay,synthetic
  // Ghidra 0x0044c18a PlayerTick_HyperspaceCommand (synthetic region of
  // 0x0044aa70): engage gates. Remaining TODO(decomp): escort fleet
  // constraints, the velocity-match gate, and port-equivalence timing of the
  // mode-3 re-aim.
  // (a) Idle: wait for the travel key. The original's jump dispatch
  // (Ship_HandlePlayerShipCore 0x0044c195, binding 14) requires a plotted
  // destination (travel_transfer_mode == 3 with ai_secondary_target_slot !=
  // -1): a bare 'j' with nothing plotted only raises the "select a
  // destination" reminder (0x0044c628, STR# 0x7d2 0x1c) -- there is no
  // nearest-travel-point fallback on the player jump.
  if (!travel_input) {
    TickJumpRangeCue(state);
    return;
  }
  // Disabled ships cannot engage (0x0044c1a3): the jump dispatch bails on
  // Ship_IsShipDisabled before any denial feedback -- a silent refusal.
  if (NovaAiShip_IsDisabled(state, state.player)) {
    return;
  }
  const System *sys = state.scenario.System(CurrentSystemResource(state));
  if (!sys) {
    return;
  }
  const auto overlay_denial = [&](std::uint16_t entry) {
    const auto text = NovaHud_LoadStringEntry(0x7d2, entry);
    NovaHud_ShowOverlayMessage(
        state, text.value_or(""), 0xfa, 0x00, 0x0c, 0xf0U);
  };
  if (t.starmap_destination_system_id < 0 || t.travel_slot < 0) {
    // g_pending_overlay_message's DAT_0072edcc (STR# 0x7d2 entry 0x1d, loaded
    // from the 0x16..0x1f block) shown at 0x0044c1d0 for the fresh-pilot
    // "select a destination" hint. The previous 0x1c loaded the tutorial
    // fragment "' to begin your jump."
    overlay_denial(0x1d); // "You have to select a destination before you can
                          // start a hyperspace jump."
    return;
  }
  if (!NovaTravel_CanStartJump(state)) {
    // Fuel below one jump: STR# 0x7d2 entry 0xa "Insufficient energy for
    // hyperspace jump." (the cached string DAT_0072dccc shown at 0x0044c6b8;
    // the 3..0xa loader block maps DAT_0072dccc to entry 0xa).
    overlay_denial(0xa);
    return;
  }
  const std::size_t slot = static_cast<std::size_t>(t.travel_slot);
  const std::int16_t dest_resource = sys->links[slot];
  if (dest_resource < 0x80 ||
      NovaSystem_ResolveVisibleForTravel(
          state, static_cast<std::int16_t>(dest_resource - 0x80)) !=
          t.starmap_destination_system_id) {
    // The armed slot no longer resolves to the plotted destination (stale
    // arm after a system change): treat as nothing plotted.
    overlay_denial(0x1d);
    return;
  }
  // No-jump radius around the SYSTEM CENTER (0x0044c220 loop +
  // Stellar_ComputeTravelRangeSq 0x00465610): while a non-restricted nav
  // stellar exists and the ship sits inside the travel range measured from
  // the origin, the engage is refused with STR# 0x7d2 0x2a. The fast-jump
  // capability (class Flags2 0x0020 or ModType 37; see
  // NovaOutfit_HasFastJumpCapability 0x0046d080) does NOT bypass this
  // range gate -- only the brake phase's per-axis stop gate.
  bool has_usable_nav = false;
  for (const std::int16_t nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const Stellar *nav_stellar = state.scenario.Stellar(nav);
    if (nav_stellar != nullptr &&
        (nav_stellar->availability_flags & 0x3000U) == 0U) {
      has_usable_nav = true;
      break;
    }
  }
  const float dist_sq = state.player.pos_x * state.player.pos_x +
                        state.player.pos_y * state.player.pos_y;
  if (has_usable_nav && dist_sq <= NovaTargeting_ComputeTravelRangeSq(state)) {
    overlay_denial(0x2a); // "Can't initiate hyperspace jump - not yet far
                          // enough away from system center."
    return;
  }

  const std::int16_t dest_zero_based =
      static_cast<std::int16_t>(dest_resource - 0x80);
  const std::int16_t stellar_id = sys->nav_defs[slot];

  t.engaged_stellar_id = stellar_id >= 0x80 ? stellar_id : -1;
  t.destination_system_id = dest_zero_based;
  t.starmap_destination_system_id = dest_zero_based;

  // A plotted (mode-3) jump clears the stellar selection every frame in the
  // original (g_travel_selected_stellar_id = -1 at PlayerTick_Hyperspace-
  // Command): the target reticle hides and the nav panel switches to the
  // Hyperspace destination display.
  t.selected_stellar_id = -1;
  t.selected_stellar_is_manual = false;

  // The original turns toward the map-space bearing from the current system
  // to its linked destination; in-system player coordinates are unrelated.
  const System *dest_sys =
      state.scenario.System(static_cast<std::int16_t>(dest_zero_based + 0x80));
  if (dest_sys != nullptr) {
    const float dx = static_cast<float>(dest_sys->pos_x - sys->pos_x);
    const float dy = static_cast<float>(dest_sys->pos_y - sys->pos_y);
    t.jump_heading_rad = std::atan2(dx, -dy);
  } else {
    // Fallback: keep the current heading.
    t.jump_heading_rad = state.player.heading;
  }

  t.engaging = true;
  t.jump_phase = TravelState::JumpPhase::kBrake;
  t.warp_up_started = false;
  t.tunnel_elapsed_60hz = 0.0F;
  t.hold_audio_latch = false;
  NovaLog::Debug(
      "hyperspace jump engaged: from stellar {} (slot {}) to system {}",
      t.engaged_stellar_id,
      slot,
      dest_zero_based);
  // Flight tail (engaged now: the cue is suppressed on the engage frame,
  // matching the original's tail-after-commands order).
  TickJumpRangeCue(state);
}

// ---------------------------------------------------------------------------
// Plotted starmap route (state.travel.starmap_route; Ghidra DAT_00735404).
// ---------------------------------------------------------------------------

// @port 0x004A7E80 90% gameplay
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

// @port 0x004A7FC0 90% gameplay
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

// @port 0x004A8080 90% gameplay
// Ghidra 0x004a8080 NovaUi_SyncTravelSelectionFromStarmapRoute: arms the
// travel slot and starmap destination from the first plotted hop.
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
      // Route re-arm (0x004a8080) also latches plotted-jump mode 3 and
      // clears the stellar selection.
      state.player.travel_transfer_mode = 3;
      t.selected_stellar_id = -1;
      t.selected_stellar_is_manual = false;
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
  const auto &tail_sys = state.scenario.systems[static_cast<std::size_t>(tail)];
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
    const auto &last_sys =
        state.scenario.systems[static_cast<std::size_t>(route[count - 1])];
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

bool NovaTravel_PlayerMeetsStellarAccess(const GameState &state,
                                         std::int16_t stellar_id) {
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr ||
      stellar->system_id != state.player.current_system_id) {
    return false;
  }
  bool eligible = false;
  if (stellar->government_id == -1) {
    eligible = true;
  } else {
    const std::int16_t sys_rep =
        state.player.current_system_id >= 0 &&
                state.player.current_system_id <
                    static_cast<std::int16_t>(state.system_reputation.size())
            ? state.system_reputation[static_cast<std::size_t>(
                  state.player.current_system_id)]
            : 0;
    const std::int16_t threshold = stellar->min_status;
    if (((threshold <= sys_rep) || (threshold == -0x7fff)) &&
        threshold != 0x7fff) {
      eligible = true;
    }
  }
  if (stellar->dominated) {
    eligible = true;
  }
  if (state.travel.engage_timer > 0x2ed) {
    eligible = true;
  }
  if (stellar->government_id != -1) {
    const auto govt_index = static_cast<std::size_t>(stellar->government_id);
    if (govt_index >= state.scenario.governments.size()) {
      eligible = false;
    } else {
      const Government &government = state.scenario.governments[govt_index];
      if (!NovaOutfit_EvaluateRequireMask(
              state, government.require_lo, government.require_hi)) {
        eligible = false;
      }
    }
  }
  if (!eligible) {
    for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions; ++slot) {
      if (!state.active_mission_runtime_flags[slot].is_active) {
        continue;
      }
      // Ghidra compares the travel slot against the active mission's two
      // stellar fields (misn -0x6d / -0x69, the Visit/Return ids).
      const ActiveMission &mission = state.active_missions[slot];
      if (mission.travel_stellar_id == stellar_id ||
          mission.return_stellar_id == stellar_id) {
        eligible = true;
        break;
      }
    }
  }
  if (!eligible && stellar->government_id != -1 &&
      NovaGovernment_GetPolicyFlag(state.scenario, stellar->government_id, 1)) {
    eligible = true;
  }
  return eligible;
}

// @port 0x00459950 90% gameplay,ui
// Ghidra 0x00459950 NovaUi_UpdateTravelEngagementProgress.
void NovaTravel_UpdateEngagementProgress(GameState &state) {
  constexpr std::int16_t kArmedTimer = 0x2ee;
  constexpr std::int16_t kRequestAxisRange = 0xfa; // 250
  const std::int16_t selected = state.travel.selected_stellar_id;
  const Stellar *stellar = state.scenario.Stellar(selected);
  // Left the current system (or the selection was cleared): wipe the approach.
  if (stellar == nullptr ||
      stellar->system_id != state.player.current_system_id) {
    state.travel.selected_stellar_id = -1;
    state.travel.engage_timer = -1;
    return;
  }
  if (state.travel.engage_timer < 0) {
    // The original arms the timer from the first land command for a newly
    // selected stellar (0x0045937c/0x004593d1 set it to 0 or 0x2ed); the port
    // initialises it on the first tick the selection is seen so the approach
    // can arm without an initially rejected press.
    state.travel.engage_timer = 0;
  }

  const bool eligible = NovaTravel_PlayerMeetsStellarAccess(state, selected);

  if ((stellar->flags & 0x20U) == 0U && eligible) {
    if (state.travel.engage_timer > 0x2ec) {
      ++state.travel.engage_timer;
    }
    const float dx = state.player.pos_x - static_cast<float>(stellar->pos_x);
    const float dy = state.player.pos_y - static_cast<float>(stellar->pos_y);
    if (std::abs(dx) < kRequestAxisRange && std::abs(dy) < kRequestAxisRange &&
        state.travel.engage_timer < kArmedTimer) {
      state.travel.engage_timer = kArmedTimer;
    }
    if (state.travel.engage_timer == kArmedTimer) {
      // Ghidra 0x00459950 queues transition-table cue 1 at the same edge as
      // the clearance message. Incrementing the timer above prevents repeats.
      state.pending_ui_sounds.push_back({1, 1});
      // "Cleared to dock/land" overlay (STR# 0x7d2), composing the randomly
      // rolled lead/connector/tail variants the original builds.
      const bool is_station = (stellar->flags & 0x10U) != 0U;
      // The original interpolates g_player_ship_name (the hull's registration
      // name), NOT the system name; e.g. "<ship>, you're cleared to land."
      const std::string &ship_name = state.player.ship_name;
      auto text = [](std::uint16_t entry, const char *fallback) {
        return NovaHud_LoadStringEntry(0x7d2, entry).value_or(fallback);
      };
      std::string message;
      const int lead = std::uniform_int_distribution<int>{0, 2}(state.rng);
      if (is_station) {
        if (lead == 0) {
          message = text(0x5e, "Cleared to dock");
          message += ", " + ship_name + ". ";
        } else if (lead == 1) {
          message =
              ship_name + ", " + text(0x5f, "you're cleared to dock.") + " ";
        } else {
          message = text(0x60, "You are cleared to dock.") + " ";
        }
      } else {
        if (lead == 0) {
          message = text(0x61, "Cleared to land");
          message += ", " + ship_name + ". ";
        } else if (lead == 1) {
          message =
              ship_name + ", " + text(0x62, "you're cleared to land.") + " ";
        } else {
          message = text(0x63, "You are cleared to land.") + " ";
        }
      }
      if (std::uniform_int_distribution<int>{0, 1}(state.rng) == 0) {
        message += text(0x64, "Commence final approach.");
      } else {
        message += text(0x65, "Welcome to") + " " + stellar->name + ". ";
      }
      if (stellar->service_cost > 0 && !stellar->dominated) {
        message += "  ";
        message += text(is_station ? 0x67 : 0x68,
                        is_station ? "[Docking fee is" : "[Landing fee is");
        message += " " + std::to_string(stellar->service_cost) + " credits";
        message += text(0x69, ".]");
      }
      NovaHud_ShowOverlayMessage(state, message, 0xe0, 0xe0, 0xe0, 0xfaU);
    }
  } else {
    if (!eligible) {
      return;
    }
    // travel_flags 0x20 (cannot-land) but otherwise eligible: the original
    // still arms the timer and (when the land-command latch is set) shows STR
    // 0x35. The port's docking gate rejects 0x20 targets, so only the timer
    // arming/expiry is reproduced here; the overlay is TODO(decomp).
    if (state.travel.engage_timer > 0x2ec) {
      ++state.travel.engage_timer;
    }
    const float dx = state.player.pos_x - static_cast<float>(stellar->pos_x);
    const float dy = state.player.pos_y - static_cast<float>(stellar->pos_y);
    if (std::abs(dx) < kRequestAxisRange && std::abs(dy) < kRequestAxisRange &&
        state.travel.engage_timer < kArmedTimer) {
      state.travel.engage_timer = kArmedTimer;
    }
  }
  if (state.travel.engage_timer > 0x7ff) {
    state.travel.engage_timer = -1;
    state.travel.selected_stellar_id = -1;
  }
}

bool NovaTravel_PlayerPastJumpOnset(const GameState &state) {
  // Same ramp schedule as the in-tunnel movement block (travel.cpp ~line
  // 1229). tunnel_elapsed_60hz is the port's authoritative player jump clock
  // (1/60 s ticks since the hold began), standing in for the original's
  // NovaTime_GetTickCount60Hz() - ai_mode_start_time_ms.
  const float jump_multiplier = NovaTravel_PlayerJumpDurationMultiplier(state);
  const float progress =
      state.travel.tunnel_elapsed_60hz * jump_multiplier /
          (NovaTravel_JumpSequenceDuration60Hz(state) * kJumpDurationScale) -
      kJumpProgressOffset / jump_multiplier;
  return progress > kJumpProgressOnsetThreshold;
}

} // namespace game
