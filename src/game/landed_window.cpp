#include "landed_window.hpp"
#include "mission.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "asteroid.hpp"
#include "docked_dialog.hpp"
#include "escort_formation.hpp"
#include "hud_renderer.hpp"
#include "landed_store.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "pilot_file.hpp"
#include "scenario_data.hpp"
#include "selection_text_dialog.hpp"
#include "services_buttons.hpp"
#include "ship_spawn.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#include "spaceflight.hpp"

namespace game {

// ---------------------------------------------------------------------------
// Stellar_HandleStellarEntryAndExit (0x00457580) normal-arrival gate.
// ---------------------------------------------------------------------------
// Stellar_MaxLandingDistance: the per-axis landing approach envelope for a
// target stellar. NOT a distinct binary function -- the arrival gate inlines
// this computation at 0x00458786..0x004587a9, using
// System_GetCurrentSystemLinkSpriteWidth (0x00462410) and the 1.75 double
// k_stellar_arrival_envelope_scale_f64 (0x005756a0).
//
// Exact return value: a half-extent in pixels. The landing is in range only
// when BOTH |player.pos_x - stellar.pos_x| AND |player.pos_y - stellar.pos_y|
// are strictly less than this value (a square envelope, not a radius):
//   - target_sprite_full_width <= 0 (no prepared ambient sprite): 75 (0x4b).
//   - otherwise: round(target_sprite_full_width * 1.75), where the input is
//     the link_a spin set's current-frame full width. If a sprite is prepared
//     but the link_a set is unavailable,
//     System_GetCurrentSystemLinkSpriteWidth itself returns 150 (0x96), giving
//     round(150 * 1.75) = 262.
// nearbyint mirrors the original x87 FIST (round half to even).
float Stellar_MaxLandingDistance(std::int16_t target_sprite_full_width) {
  constexpr float kNoSpriteAxisRange = 0x4b; // 75
  constexpr double kSpriteRangeScale =
      1.75; // k_stellar_arrival_envelope_scale_f64
  if (target_sprite_full_width <= 0) {
    return kNoSpriteAxisRange;
  }
  return static_cast<float>(std::nearbyint(
      static_cast<double>(target_sprite_full_width) * kSpriteRangeScale));
}

// Ghidra 0x004250f0 Player_RefuelShipWithCredits. Arrival auto-refuel for the
// auto-refueller outfit (ModType 19). Runs once per landing at a landable
// stellar (travel_flags 0x20 clear) from the Stellar_RunDockAndLaunchSequence
// arrival subset, before the Spaceport interaction loop. When the player owns
// any outfit with ModType 19, rounds fuel to the nearest unit and tops up to
// the effective capacity at exactly 1 credit per unit, clamped to the available
// credits. The EVN Bible marks ModType 19 "ignored", but the engine consumes
// it here.
void Player_RefuelShipWithCredits(GameState &state) {
  const std::int16_t stellar_id = state.travel.selected_stellar_id;
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    if (state.inventory.outfit_owned_count[idx] <= 0) {
      continue;
    }
    const Outfit &def = state.scenario.outfits[idx];
    const bool is_auto_refueller =
        def.mod_type == static_cast<std::int16_t>(OutfitEffect::kAutoRefuel) ||
        std::any_of(def.alt_mod_types.begin(),
                    def.alt_mod_types.end(),
                    [](std::int16_t mod) {
                      return mod == static_cast<std::int16_t>(
                                        OutfitEffect::kAutoRefuel);
                    });
    if (!is_auto_refueller) {
      continue;
    }
    const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
    state.cached_stats = eff;
    state.stat_cache_valid = true;
    // The original rounds fuel to the nearest unit (ROUND) before topping
    // up (0x00425150).
    const std::int16_t fuel_now =
        static_cast<std::int16_t>(std::lrintf(state.player.fuel_points));
    if (fuel_now >= static_cast<std::int16_t>(eff.fuel_capacity)) {
      break;
    }
    state.player.fuel_points = static_cast<float>(fuel_now);
    const float missing = eff.fuel_capacity - state.player.fuel_points;
    const std::int32_t spend = static_cast<std::int32_t>(std::min<std::int64_t>(
        state.player.credits, static_cast<std::int64_t>(missing)));
    state.player.credits -= spend;
    state.player.fuel_points += static_cast<float>(spend);
    NovaLog::Info("auto-refueller: bought {:.0f} fuel for {} credits at "
                  "stellar {}",
                  missing,
                  spend,
                  stellar_id);
    break;
  }
}

// ---------------------------------------------------------------------------
// Ghidra 0x00455e10 Stellar_RunDockAndLaunchSequence: arrival half.
// The launch half is Stellar_Launch. The fee gate/deduction below is
// Stellar_HandleStellarEntryAndExit (0x00457580) behavior folded in here.
// ---------------------------------------------------------------------------
bool Stellar_Dock(GameState &state,
                  LandedContext &ctx,
                  std::int16_t target_sprite_full_width) {
  ctx.landed = false;
  ctx.denial = LandedDenial::kNone;
  const std::int16_t stellar_id = state.travel.selected_stellar_id;
  const auto *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr || !stellar->is_available ||
      stellar->system_id != state.player.current_system_id ||
      (stellar->availability_flags & 0x3000U) != 0U ||
      (stellar->flags & 0x20U) != 0U ||
      !NovaTargeting_StellarTargetsSpriteSetActive(*stellar)) {
    ctx.denial = LandedDenial::kUnavailable;
    return false;
  }
  // Stellar_HandleStellarEntryAndExit 0x004585ca..0x0045870d resolves the
  // reputation / government ScanMask / mission override / government-policy
  // gate before any approach-distance feedback. Without this explicit check,
  // NovaTravel_UpdateEngagementProgress merely leaves the timer unarmed and a
  // denied body is misleadingly reported as "too far".
  if (!NovaTravel_PlayerMeetsStellarAccess(state, stellar_id)) {
    ctx.denial = LandedDenial::kUnauthorized;
    return false;
  }
  // Stellar_HandleStellarEntryAndExit normal-arrival gate. The original runs a
  // single failure branch (0x00458de0) and picks the feedback from whether the
  // ship was inside the envelope with the approach armed:
  //  - engage timer < 0x2ee (NovaUi_UpdateTravelEngagementProgress has not yet
  //    armed the request) or out of the sprite-derived envelope -> too far;
  //  - otherwise, still moving (|vel| > 0.75 on either axis) or with an
  //    unexpired maneuver timer -> too fast.
  constexpr float kApproachVelocityLimit = 0.75F; // g_lit_0p75
  const float arrival_axis_range =
      Stellar_MaxLandingDistance(target_sprite_full_width);
  const bool within_envelope =
      std::abs(state.player.pos_x - static_cast<float>(stellar->pos_x)) <
          arrival_axis_range &&
      std::abs(state.player.pos_y - static_cast<float>(stellar->pos_y)) <
          arrival_axis_range;
  if (!within_envelope || state.travel.engage_timer < 0x2ee) {
    ctx.denial = LandedDenial::kTooFar;
    return false;
  }
  if (std::abs(state.player.vel_x) > kApproachVelocityLimit ||
      std::abs(state.player.vel_y) > kApproachVelocityLimit ||
      state.player.ai_maneuver_timer_ms > 0.0F) {
    ctx.denial = LandedDenial::kTooFast;
    return false;
  }

  // Stellar_HandleStellarEntryAndExit checks affordability before it begins the
  // arrival transition, then deducts the full fee (unless the stellar is in
  // its hostile/hazard state). Do the same before touching player state.
  const bool fee_waived = stellar->dominated;
  if (stellar->service_cost > 0 && !fee_waived &&
      state.player.credits < stellar->service_cost) {
    NovaLog::Info("landing denied at stellar {}: service cost {} exceeds "
                  "available credits {}",
                  stellar_id,
                  stellar->service_cost,
                  state.player.credits);
    ctx.denial = LandedDenial::kTooExpensive;
    return false;
  }

  if (stellar->service_cost > 0 && !fee_waived) {
    state.player.credits -= stellar->service_cost;
  }
  // Stellar_RunDockAndLaunchSequence (0x00455e37) runs
  // Weapon_ReconcileOutfitPoolWith- WeaponBanks at the start of the travel
  // transition, so any stock weapon bank acquired since the last reconcile
  // (e.g. a ship bought at the shipyard with mounted stock guns) becomes a
  // sellable owned outfit. Landed Offfitter session buys/sells use this same
  // reconcile at modal entry.
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
  // Stellar_RunDockAndLaunchSequence (0x00455e57): landing at a landable
  // stellar runs the auto-refueller arrival pass before the Spaceport
  // interaction loop. The else-arm (travel_flags 0x20 set -> centered
  // transition sound [1]) is unreachable through this dock gate.
  Player_RefuelShipWithCredits(state);
  // Stellar_RunDockAndLaunchSequence (0x00455e37) runs
  // Weapon_ReconcileOutfitPoolWith- WeaponBanks at the start of the travel
  // transition, so any stock weapon bank acquired since the last reconcile
  // (e.g. a ship bought at the shipyard with mounted stock guns) becomes a
  // sellable owned outfit. Landed Offfitter session buys/sells use this same
  // reconcile at modal entry.
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);

  // NOTE: the ship's meters (shield/armor), position/velocity and the
  // calendar are deliberately NOT touched here. The original restores all of
  // that in the launch tail, after the interaction loop returns -- see
  // Stellar_Launch.

  // Offering rolls redraw on landing too (Stellar_HandleStellarEntryAndExit
  // 0x00458802 arm).
  Mission_RerollOfferingRolls(state);

  ctx.stellar_id = stellar_id;
  ctx.landed = true;
  // Stellar_RunDockAndLaunchSequence entry (0x00455e19/0x00455e20): bracket the
  // docked visit, clear the one-shot reposition latch, and record the landing
  // stellar. The original writes its 0-based g_stellar_defs index here; the
  // port keeps the 0x80-based resource id so the player path matches the AI
  // (see ShipState::ai_secondary_target_slot). M may repoint this field at the
  // destination system's first nav and the launch tail reads it back.
  state.system_transition_active = true;
  state.skip_player_reposition_once = false;
  state.player.ai_secondary_target_slot = stellar_id;
  // The landing transition raises the no-asteroids latch
  // (Stellar_HandleStellarEntryAndExit 0x00457580 sets DAT_00596d2c = 1), so
  // the docked view hides the drifting field; the launch tail re-initialises
  // it.
  state.no_asteroids_latch = true;
  // 0x00458186: the same landing entry raises the four "clear transient
  // sprites" latches, so the departing system's shots, beam records, impact
  // effects, fading destruction fragments and freeflight objects (mined
  // resource-boxes, jettisoned pods) are wiped on the next transition frame.
  // The port clears them synchronously here; the docked modal blocks the
  // spaceflight tick, so nothing redraws them before the launch tail.
  NovaWeapon_ClearTransientCombatState(state);
  ctx.selection = LandedService::kLaunch;
  state.travel.landed_this_frame = true;
  NovaLog::Info("landed at stellar {} ({}); {} credits remain",
                stellar_id,
                stellar->name,
                state.player.credits);
  return true;
}

// ---------------------------------------------------------------------------
// Stellar_RunDockAndLaunchSequence (0x00455e10): launch tail,
// 0x00455f99..0x00456268. Runs when the destination-interaction loop returns,
// i.e. on leaving the dock. The caller then shows the departure overlay and
// resyncs its frame clock (the original zeroes g_avg_frame_tick_scale at
// 0x00456174).
// ---------------------------------------------------------------------------
void Stellar_Launch(GameState &state) {
  // 0x00455f99/0x0045600b: velocity and speed kill. The original zeroes the
  // velocity once more after the reposition; one pass is equivalent.
  state.player.vel_x = 0.0F;
  state.player.vel_y = 0.0F;
  state.player.speed = 0.0F;
  // 0x00455fa6: snap to the queued travel stellar (ai_secondary_target_slot, a
  // 0x80-based resource id in the port, which a docked M may have repointed at
  // the destination system's first nav) unless a docked N latched
  // g_skip_player_reposition_once (0x00449bda). The no-stellar fallback is the
  // in-system origin (0,0), not System::pos_x.
  if (state.skip_player_reposition_once) {
    state.skip_player_reposition_once = false;
  } else if (state.player.ai_secondary_target_slot < 0) {
    state.player.pos_x = 0.0F;
    state.player.pos_y = 0.0F;
  } else if (const auto *stellar = state.scenario.Stellar(
                 state.player.ai_secondary_target_slot)) {
    state.player.pos_x = static_cast<float>(stellar->pos_x);
    state.player.pos_y = static_cast<float>(stellar->pos_y);
  }
  // 0x00456020/0x0045602a: shield and armor refill to the effective maxima.
  const PlayerEffectiveStats effective =
      Outfit_ComputePlayerEffectiveStats(state);
  state.player.shield_points = effective.max_shield_points;
  state.player.armor_points = effective.max_armor_points;
  state.cached_stats = effective;
  state.stat_cache_valid = true;
  // 0x00456033: the single daily world tick runs at LAUNCH, after the
  // interaction loop -- the Spaceport's mission gate therefore saw the
  // pre-landing date when failing overdue deadlines.
  Mission_TickDailyWorldUpdate(state);
  // 0x00456038: the persisted stat-modifier random walk
  // (Frame_JitterPlayerStatModifiers 0x00431480). The second-pair reroll
  // (Frame_RerollPlayerStatModifiers 0x00431500) is NOT part of the launch
  // tail -- its only call site is the in-flight jump arrival at 0x0044fa15.
  NovaFrame_JitterPlayerStatModifiers(state);
  // 0x00456060..0x0045609b: discovery booking at level 2 (slot + current
  // system), visibility rebuild and region events. The original runs this
  // BEFORE the mission rearm loop, so that loop observes the refreshed
  // discovery state.
  NovaSystem_OnSystemEntered(state, state.player.current_system_id, 2);
  // 0x004560a0: the launch inlines only the per-mission tail of
  // Mission_RefreshActiveMissionSpawnState (0x00448910) -- it does NOT run
  // that function's ShipStart-1 delayed-arrival head.
  Mission_RearmActiveMissionTimers(state);
  // 0x00456103: launch autosave, with ship->ai_secondary_target_slot as the
  // restore point (block1 +0x00). The pilot file stores the original's 0-based
  // g_stellar_defs index, so rebase the port's 0x80-based field at this
  // boundary; a docked M/N may have overwritten it (destination nav / -1).
  // Tests and incomplete bootstrap states have no pilot name; the original
  // entry point is likewise only reachable for an active pilot.
  if (!state.pilot.first_name.empty()) {
    if (const auto directory = PilotFileSaveDirectory()) {
      if (!PilotFileSaveGame(*directory,
                             state,
                             PilotFileStellarIndexFromResourceId(
                                 state.player.ai_secondary_target_slot))) {
        NovaLog::Error("launch: could not autosave pilot '{}'",
                       state.pilot.first_name);
      }
    }
  }
  // 0x00456109: random launch heading, rand(0x168) = 0..359 degrees (the
  // port stores radians).
  {
    constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
    std::uniform_int_distribution<int> heading_roll{0, 0x168 - 1};
    state.player.heading =
        static_cast<float>(heading_roll(state.rng)) * kDegToRad;
  }
  // 0x0045612d: the docked visit is over; M/N no longer take the transition
  // arm.
  state.system_transition_active = false;
  // 0x00456128: swallow every player command edge latch so the Escape that
  // left the dock (still held when flight resumes) must be released and
  // re-pressed before it returns to the menu. NovaUi_MarkTravelAndStatusPanels-
  // Dirty also arms the escort/cloak/zoom latches, matching the original.
  NovaUi_MarkTravelAndStatusPanelsDirty(state);
  // 0x00456158: travel-selection and engage-timer reset.
  state.travel.selected_stellar_id = -1;
  state.travel.selected_stellar_is_manual = false;
  state.travel.engage_timer = -1; // 0x00456161
  // 0x00456195..0x00456250: wipe the transient shot pool (every ShotState
  // fuse reset to -2.0).
  NovaWeapon_ClearTransientCombatState(state);
  // 0x00456256: DAT_00597974 = tick60 - 60 re-arms the cursor-anchored
  // travel-selection sprite. TODO(decomp(0x00439280)) skipped: that sprite
  // channel is not reconstructed.
  // The launch clears the no-asteroids latch set on landing (the original does
  // this through the spaceflight loop's transition reconciliation plus the
  // every-tick Asteroid_Spawn('\x01') ring, which is not yet ported) and
  // rebuilds the current system's drifting field around the repositioned
  // player.
  NovaAsteroid_InitSystem(state);

  // Stellar_HandleStellarEntryAndExit 0x00458304: the post-launch cleanup
  // drops the player's primary ship target just before the vacant-ship sweep,
  // so a recycled NPC slot cannot leave a stale reticle on an unrelated fresh
  // ship after the population rebuild below.
  state.player.primary_target_ship_slot = -1;
  state.ship_reticle_pulse = 0.0F;

  // Stellar_HandleStellarEntryAndExit 0x00458a47..0x00458bd2 performs the
  // vacant-ship sweep and System_RebuildInitialNpcAndMissionPopulation only
  // after Stellar_RunDockAndLaunchSequence returns. Missions accepted in the
  // Spaceport loop must therefore participate in this rebuild.
  NovaShip_DeactivateVacantShipsAndTally(state, /*keep_player_engaged=*/false);
  // Ghidra 0x0041af90: the normal launch caller passes flag=1. Adopt attached
  // escorts before mission fleets and ambient ships; this also refills their
  // hull meters/weapon stock and snaps the player formation.
  NovaSystem_RestorePlayerEscorts(
      state,
      /*refill=*/true,
      static_cast<std::uint32_t>(state.gameplay_now_ms));
  NovaSystem_RestoreMissionFleets(
      state,
      state.player.current_system_id,
      /*copy_player_heading=*/true,
      static_cast<std::uint32_t>(state.gameplay_now_ms));
  NovaSystem_PopulateInitialNpcShips(state, state.player.current_system_id);
}

// ---------------------------------------------------------------------------
// Fuel service.
// ---------------------------------------------------------------------------
// Refuels the player ship toward its effective fuel capacity. `price_per_unit`
// is the price in credits for one fuel point (the raw fuel_points scale where
// a typical ship holds a few hundred units); the player pays per unit topped
// up, clamped so credits never go negative (they may only buy as much as they
// can afford). Returns the credits actually spent.
std::int32_t NovaLanded_Refuel(GameState &state, std::int32_t price_per_unit) {
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  const float missing =
      std::max(0.0F, eff.fuel_capacity - state.player.fuel_points);
  if (missing <= 0.0F) {
    return 0;
  }
  // The original's stellar marker makes refuelling free, not unavailable.
  if (price_per_unit <= 0) {
    state.player.fuel_points = eff.fuel_capacity;
    NovaLog::Info("refuel: granted {:.1f} fuel by free stellar service",
                  missing);
    return 0;
  }
  // Full cost for the whole top-up; the player may only buy as far as credits
  // reach, so cap the spend at the wallet.
  const std::int64_t full_cost =
      std::int64_t{price_per_unit} * static_cast<std::int64_t>(missing);
  const std::int64_t spend =
      std::min<std::int64_t>(state.player.credits, full_cost);
  state.player.credits -= static_cast<std::int32_t>(spend);
  const float gained =
      static_cast<float>(spend) / static_cast<float>(price_per_unit);
  state.player.fuel_points =
      std::min(eff.fuel_capacity, state.player.fuel_points + gained);
  NovaLog::Info("refuel: bought {:.1f} fuel for {} credits ({} remaining)",
                gained,
                spend,
                state.player.credits);
  return static_cast<std::int32_t>(spend);
}

// ---------------------------------------------------------------------------
// Armor/shield repair service.
// ---------------------------------------------------------------------------
std::int32_t NovaLanded_Repair(GameState &state,
                               std::int32_t price_per_armor_point) {
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  // The original repairs the whole hull; shields are already full from the
  // arrival transition, so only the armor gap is billed.
  const float armor_gap =
      std::max(0.0F, eff.max_armor_points - state.player.armor_points);
  if (armor_gap <= 0.0F || price_per_armor_point <= 0) {
    return 0;
  }
  const std::int64_t full_cost = std::int64_t{price_per_armor_point} *
                                 static_cast<std::int64_t>(armor_gap);
  const std::int32_t spend = static_cast<std::int32_t>(
      std::min<std::int64_t>(state.player.credits, full_cost));
  state.player.credits -= spend;
  const float repaired =
      static_cast<float>(spend) / static_cast<float>(price_per_armor_point);
  state.player.armor_points =
      std::min(eff.max_armor_points, state.player.armor_points + repaired);
  NovaLog::Info("repair: fixed {:.1f} armor for {} credits ({} remaining)",
                repaired,
                spend,
                state.player.credits);
  return spend;
}

// ---------------------------------------------------------------------------
// Window / service-button geometry.
//
// The original docked screen is the Spaceport window (DLOG 0x3e8, DITL 0x3e8),
// a near-full-screen 640x480 panel whose backdrop is destination-art PICT
// 0x2134 (Ghidra FUN_0048e970 sets g_travel_overlay_sprite_handle =
// Resource_LoadPictAsImage(0x2134), drawn across the whole window rect). The
// 263x185 PICT 0x2137 is a *sub*-window decoration (the travel-services
// modal DLOG 0x3f5), not the docked backdrop. Service controls live in two
// ~145px-wide
// columns down the left and right edges (DITL 0x3e8 entries 3,6,7,8,9,10,11,
// 12), mirroring the real docked buttons.
//
// We reproduce that docked panel here: the backdrop fills the full 640x480
// screen and the window content (panels, title band, and the two-column x
// 4-row service grid at the lower-left and lower-right) is laid out from the
// real Spaceport DLOG/DITL 0x3e8 via NovaDialogWindow_Layout, so the draw pass
// and the mouse hit-test share the same data-driven rects. The hardcoded
// two-column grid remains only as a fallback when the dialog archive is
// absent.

namespace {

// The original Spaceport DLOG is 618x517. Keep that native size and centre it
// in the unrestricted window coordinate space; a 640x480 clipped viewport
// cannot contain the dialog vertically.
using PanelRect = SDL_FRect;

[[nodiscard]] PanelRect DockedPanel(const SdlPlatform &platform) {
  (void)platform;
  constexpr float kDockedWidth = 618.0F;
  constexpr float kDockedHeight = 517.0F;
  return {0.0F, 0.0F, kDockedWidth, kDockedHeight};
}

// Fallback button columns (DITL-left-column values) used when the dialog
// archive is absent, so the dock still renders.
struct ServiceColumns {
  float left_x = 3.0F;
  float right_x = 471.0F;
  float width = 145.0F;
  float height = 25.0F;
  float row_pitch = 41.0F;
  float row0_y = 333.0F;
};

} // namespace

// Public adapter (#3): lays the Spaceport dialog (DLOG+ DITL 0x3e8) onto the
// 640x480 logical panel, centering the dialog window on it and mapping every
// DITL item to panel space (screen = window_origin + dialog_rect). This
// lays the panel/title/status/button geometry out from coordinates read
// straight from Nova.rez (no hardcoded layout). Type definitions live here.
// When the dialog resources are unavailable it falls back to a minimal
// layout with just the buttons from the hardcoded two-column grid.
// Ghidra 0x008730a1 Dialog_CreateFromDlog (partial port: window bounds +
// linked DITL via NovaResource_LoadDialogDefinition, playfield centering).
bool NovaDialogWindow_Layout(const SDL_FRect &panel, DockedLayout &out) {
  constexpr ServiceColumns kFallback;
  constexpr std::size_t kRows = 4;
  const std::size_t button_count = std::min<std::size_t>(
      static_cast<std::size_t>(LandedService::kCount), kRows * 2U);

  const auto def = NovaResource_LoadDialogDefinition(0x3e8);
  const auto items = NovaResource_LoadDialogItems(0x3e8);
  if (def && items) {
    // Window size = DLOG bounds right-bottom minus left-top (618x517 for the
    // Spaceport window); centered like Dialog_CreateFromDlog centers it, with
    // the centered offset truncated toward zero to match the game's integer
    // (display - windowSize)/2 arithmetic.
    const float win_w = static_cast<float>(def->right - def->left);
    const float win_h = static_cast<float>(def->bottom - def->top);
    const auto center_trunc = [](float span) {
      return std::trunc(span / 2.0F);
    };
    const float win_x = panel.x + center_trunc(panel.w - win_w);
    const float win_y = panel.y + center_trunc(panel.h - win_h);
    out.window = {win_x, win_y, win_w, win_h};
    out.from_ditl = true;

    for (const auto &item : *items) {
      DockedItem d;
      d.rect.x = win_x + static_cast<float>(item.left);
      d.rect.y = win_y + static_cast<float>(item.top);
      d.rect.w = static_cast<float>(item.right - item.left);
      d.rect.h = static_cast<float>(item.bottom - item.top);
      const int w = item.right - item.left;
      const int h = item.bottom - item.top;
      // These controls are addressed explicitly by the original renderer
      // (UiPanel_GetEntryInfo items 3, 5, and 6); use the DITL ordinals when
      // available instead of inferring their role from size. Item 3 holds
      // the stellar name, item 5 the planet image, and item 6 the landing
      // description.
      if (item.index == 2) {
        d.kind = DockedItemKind::kTitleBand;
      } else if (item.index == 4) {
        d.kind = DockedItemKind::kOuterPanel;
      } else if (item.index == 5) {
        d.kind = DockedItemKind::kInnerPanel;
      } else if (w == 145 && h == 25) {
        d.kind = DockedItemKind::kButton;
      } else if (w > 500 && h > 200) {
        d.kind = DockedItemKind::kOuterPanel;
      } else if (w > 250 && h > 150) {
        d.kind = DockedItemKind::kInnerPanel;
      } else if (w > 250 && h < 30) {
        d.kind = DockedItemKind::kTitleBand;
      } else {
        d.kind = DockedItemKind::kOrnament;
      }
      d.ditl_index = item.index;
      out.items.push_back(d);
    }
    return true;
  }
  NovaLog::Todo("Spaceport DLOG/DITL 0x3e8 not usable; using hardcoded dock "
                "grid layout");
  // Minimal fallback: the eight buttons in the reference two-column grid.
  out.from_ditl = false;
  out.window = {panel.x, panel.y, 618.0F, 517.0F};
  // The fallback has no raw DITL parser output, but retain the original
  // control ordinals so service dispatch still follows the same map.
  constexpr std::array<std::size_t, 7> kFallbackDitlItems{
      11, 3, 6, 7, 8, 9, 10};
  for (std::size_t i = 0; i < button_count; ++i) {
    const std::size_t side = i >= kRows ? 1 : 0;
    const std::size_t row = i % kRows;
    DockedItem d;
    d.kind = DockedItemKind::kButton;
    d.rect.x = panel.x + (side == 0 ? kFallback.left_x : kFallback.right_x);
    d.rect.y = panel.y + kFallback.row0_y +
               static_cast<float>(row) * kFallback.row_pitch;
    d.rect.w = kFallback.width;
    d.rect.h = kFallback.height;
    d.ditl_index = i < kFallbackDitlItems.size() ? kFallbackDitlItems[i] : 0;
    out.items.push_back(d);
  }
  return false;
}

std::optional<LandedService>
NovaDialog_DockedServiceForDitlItem(std::size_t ditl_index) {
  switch (ditl_index) {
  // UiPanel_GetEntryInfo uses one-based item numbers; NovaDialogItem::index
  // is deliberately zero-based. Ghidra's {12,4,7,8,9,10,11} therefore maps
  // to the resource ordinals below.
  case 11:
    return LandedService::kLaunch;
  case 3:
    return LandedService::kRefuel;
  case 6:
    return LandedService::kBuySellCargo;
  case 7:
    return LandedService::kOutfit;
  case 8:
    return LandedService::kShipyard;
  case 9:
    return LandedService::kMissionBbs;
  case 10:
    return LandedService::kBar;
  default:
    return std::nullopt;
  }
}

// Greedy word-wrap for the docked landing-description panel (see header).
// Grows a candidate line word-by-word and, when the measured width would
// exceed `max_width`, closes the current line and starts the next with only
// the new word (never re-append the already-drained words, which would show a
// "cumulative" repeat of the text on every line).
std::vector<std::string>
WrapDescriptionLines(std::string_view text,
                     int max_width,
                     const std::function<int(std::string_view)> &measure) {
  std::vector<std::string> lines;
  if (text.empty() || max_width <= 0) {
    return lines;
  }
  std::string line;
  std::size_t i = 0;
  while (i < text.size()) {
    // Skip inter-word whitespace; a newline (desc resources use the Mac '\r')
    // forces an explicit line break. Each return terminates the current line,
    // so a run of two (the "...\r\rRequires: ..." desc form) yields a blank
    // line -- matching DrawTextW's DT_WORDBREAK behavior. The empty-line push
    // is skipped for the trailing return.
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' ||
                               text[i] == '\r' || text[i] == '\t')) {
      if (text[i] == '\n' || text[i] == '\r') {
        if (!lines.empty() || !line.empty()) {
          lines.push_back(line);
        }
        line.clear();
      }
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    std::size_t word_end = i;
    while (word_end < text.size() && text[word_end] != ' ' &&
           text[word_end] != '\n' && text[word_end] != '\r' &&
           text[word_end] != '\t') {
      ++word_end;
    }
    const std::size_t word_len = word_end - i;
    if (word_len == 0) {
      break; // trailing whitespace only
    }
    std::string candidate = line;
    if (!candidate.empty()) {
      candidate.push_back(' ');
    }
    candidate.append(text.substr(i, word_len));
    if (measure(candidate) > max_width && !line.empty()) {
      lines.push_back(line);
      line.assign(text.substr(i, word_len)); // new line starts with this word
    } else {
      line = candidate;
    }
    i = word_end;
  }
  if (!line.empty()) {
    lines.push_back(line);
  }
  return lines;
}

namespace {
// Builds the service buttons from a laid-out dock: the DITL's eight 145x25
// button rects split into left/right columns, each sorted top-to-bottom. Each
// physical button is assigned the real service it represents (see the two
// k*ColumnServices tables), so the label and dispatch match the original
// Spaceport screen. `slot` carries the LandedService value (not a packed
// column*4+row index), so the draw/hit-test/navigation resolve the same
// service the number keys select.
std::vector<ServiceButton> BuildServiceButtons(const DockedLayout &layout) {
  std::vector<ServiceButton> buttons;
  buttons.reserve(7);
  for (const auto &item : layout.items) {
    if (item.kind != DockedItemKind::kButton) {
      continue;
    }
    if (const auto service =
            NovaDialog_DockedServiceForDitlItem(item.ditl_index)) {
      buttons.push_back(
          ServiceButton{item.rect, static_cast<std::uint8_t>(*service)});
    }
  }
  return buttons;
}

} // namespace

namespace {

// Human-readable service-row labels for the MVP menu. The original draws these
// as PICT sub-window frames (Nova Graphics 3: 0x2135 Shipyard, 0x2136 Outfit,
// 0x2137 Bar, 0x2139 Mission BBS, 0x213e Trade, ...); we fall back to text so
// the MVP is
// navigable.
const char *ServiceLabel(LandedService t) {
  switch (t) {
  case LandedService::kLaunch:
    return "Leave";
  case LandedService::kRefuel:
    return "Refuel";
  case LandedService::kBuySellCargo:
    return "Trade center";
  case LandedService::kOutfit:
    return "Outfitter";
  case LandedService::kShipyard:
    return "Shipyard";
  case LandedService::kBar:
    return "Bar";
  case LandedService::kMissionBbs:
    return "Mission BBS";
  case LandedService::kCount:
    break;
  }
  return "";
}

// Whether a docked service slot is usable right now, mirroring the original's
// per-slot gating in NovaUi_RedrawTravelActionButtons (0x004a0220) and its
// mouse hit-test (NovaUi_HitTestAndTrackTravelActionButtons 0x0049fe10).
//   Leave            always
//   Refuel/Recharge  credits>0, fuel below capacity, non-hypergate;
//                    (hyper-gate travel_flags bit 0x20 disables services)
//   Trade center, Shipyard, Bar    travel_flags bits 0x2/0x8/0x40
//   Outfitter                       travel_flags bit 0x4
//   Mission BBS                     non-hypergate (travel_flags bit 0x20 clear)
// Unavailable slots render grey (disabled art) and refuse activation.
bool ServiceAvailable(const GameState &state,
                      std::int16_t stellar_id,
                      LandedService t) {
  const auto *st = state.scenario.Stellar(stellar_id);
  const std::uint32_t flags = st ? st->flags : 0U;
  const bool allows_services = (flags & 0x20U) == 0U; // non-hypergate
  const auto &eff = state.cached_stats;
  switch (t) {
  case LandedService::kLaunch:
    return true;
  case LandedService::kRefuel: {
    if (!allows_services || state.player.credits <= 0) {
      return false;
    }
    return state.player.fuel_points + 0.5F < eff.fuel_capacity;
  }
  case LandedService::kBuySellCargo:
    return (flags & 0x2U) != 0U;
  case LandedService::kOutfit:
    return (flags & 0x4U) != 0U;
  case LandedService::kShipyard:
    return (flags & 0x8U) != 0U;
  case LandedService::kBar:
    return (flags & 0x40U) != 0U;
  case LandedService::kMissionBbs:
    return allows_services;
  case LandedService::kCount:
    break;
  }
  return false;
}

// Draws the docked-screen backdrop + panels + header + service list with the
// real screen fonts: the destination title in Chicago (charcoal) and the status
// lines in Geneva. The Spaceport backdrop (`destination_art`) is drawn at
// native size in the centred DLOG window; the destination planet picture
// (`planet_art`, PICT link_a + 0x10000) is drawn 1:1 into the Spaceport DITL's
// 612x285 outer panel at the top-centre (its natural size), over the spaceport.
// The title band, inner status panel, and buttons are positioned from the
// laid-out DITL items
// (`layout`) rather than hardcoded geometry.
void DrawLandedMenu(SdlPlatform &platform,
                    HudRenderer *hud,
                    NovaFontCache &font_cache,
                    const ServicesButtonArt &buttons,
                    const GameState &state,
                    const LandedContext &ctx,
                    SDL_Texture *destination_art,
                    SDL_Texture *planet_art,
                    std::string_view description,
                    const SDL_FRect &panel,
                    const DockedLayout &layout,
                    const std::vector<ServiceButton> &button_rects,
                    std::optional<std::uint8_t> hovered) {
  SDL_Renderer *renderer = platform.renderer();
  const auto *st = state.scenario.Stellar(ctx.stellar_id);

  // The Spaceport backdrop is the DLOG 0x3e8 window artwork. Draw it at its
  // native 618x517 size in the same centred coordinate space as the DITL.
  // The visible area around the centered window is the flat per-system space
  // background: during the docked/landing transition the original fills the
  // gameplay surface with NovaRender_SetSystemSpaceBackgroundColor but skips
  // SpriteWorld_RenderLayers (NovaUi_RedrawGameplayViewportAndRadar
  // 0x0046a870, g_is_system_transition_active != 0), so no starfield or ship
  // sprites are drawn behind the dock.
  const auto *bg_system = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  const std::uint32_t bg = bg_system ? bg_system->bkgnd_color : 0U;
  SDL_SetRenderDrawColor(renderer,
                         static_cast<std::uint8_t>((bg >> 16) & 0xffU),
                         static_cast<std::uint8_t>((bg >> 8) & 0xffU),
                         static_cast<std::uint8_t>(bg & 0xffU),
                         SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  platform.SetPlacement(PlaceWindow(platform.logical_playfield_size()));
  // Flight HUD behind the docked dialog. Ghidra 0x00491f30
  // NovaUi_RunTravelDestinationInteractionLoop redraws the gameplay viewport,
  // the stellar radar panel (NovaUi_DrawStellarRadarPanel 0x0045d600) and the
  // cargo/mission status panel (NovaUi_DrawCargoMissionStatusPanel 0x004612c0)
  // at window-open, before the 618x517 Spaceport window is composited. Because
  // that window is centered, the cockpit strip stays visible around it. The
  // radar is forced empty for the whole docked visit through
  // g_is_system_transition_active.
  if (hud != nullptr) {
    hud->Draw(platform, state, /*force_empty_radar=*/true);
  }
  // The Spaceport DLOG is authored at 618x517. Keep the flight HUD in
  // window-space, then contain only the fixed docked composition so it stays
  // native-sized on large windows and shrinks coherently on small ones.
  platform.SetPlacement(
      PlaceContained({panel.w, panel.h}, platform.logical_playfield_size()));
  if (destination_art != nullptr) {
    const SDL_FRect backdrop_rect = layout.from_ditl ? layout.window : panel;
    SDL_RenderTexture(renderer, destination_art, nullptr, &backdrop_rect);
  }

  const SDL_Color kTitle{255, 255, 255, 255};
  const SDL_Color kBody{255, 255, 255, 255};
  const SDL_Color kPanel{16, 40, 72, 255}; // flat panel frame fill

  // Lay out the panels. The large 612x285 outer panel at the top-centre
  // is the destination-planet frame: the planet picture is drawn into it at its
  // natural size (falling back to a flat frame when no planet PICT loads). The
  // inner (status/description) panel and the title band are handled below.
  SDL_FRect title_band = panel;
  SDL_FRect outer_panel = {0.0F, 0.0F, 0.0F, 0.0F};
  SDL_FRect status_panel = {0.0F, 0.0F, 0.0F, 0.0F};
  for (const auto &item : layout.items) {
    if (item.kind == DockedItemKind::kInnerPanel) {
      status_panel = item.rect;
    } else if (item.kind == DockedItemKind::kOuterPanel) {
      outer_panel = item.rect;
    } else if (item.kind == DockedItemKind::kTitleBand) {
      title_band = item.rect;
    }
  }

  // Destination planet picture in the outer panel (1:1; it is the same 612x285
  // size as the panel). The PICT backdrop supplies the frame artwork; do not
  // add an SDL border over it.
  if (outer_panel.w > 0.0F && outer_panel.h > 0.0F) {
    if (planet_art != nullptr) {
      SDL_RenderTexture(renderer, planet_art, nullptr, &outer_panel);
    } else {
      SDL_SetRenderDrawColor(renderer, kPanel.r, kPanel.g, kPanel.b, kPanel.a);
      SDL_RenderFillRect(renderer, &outer_panel);
    }
  }

  // The inner panel is already part of the Spaceport backdrop. Only provide a
  // flat fallback when the backdrop resource is unavailable; an extra border
  // here was the visible blue rectangle around the description.
  if (status_panel.w > 0.0F && status_panel.h > 0.0F) {
    if (destination_art == nullptr) {
      SDL_SetRenderDrawColor(renderer, kPanel.r, kPanel.g, kPanel.b, kPanel.a);
      SDL_RenderFillRect(renderer, &status_panel);
    }
  }

  // Destination name centred in the DITL header band (Chicago/title font),
  // falling back to a top-of-panel line if the band is unavailable.
  std::string title = st ? st->name : std::string("(unknown stellar)");
  // Item 6 is drawn by the original with its text cursor 18 logical pixels
  // below the rect's top edge; this is a baseline, not the rect midpoint.
  const float title_baseline = title_band.y + 18.0F;
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kChicago,
                        18.0F,
                        kNovaFontStyleRegular,
                        kTitle,
                        title_band.x,
                        title_band.x + title_band.w,
                        title_baseline,
                        title);

  // The stellar's landing description text in the inner content panel (small
  // Geneva dialog font), word-wrapped to the panel width. The description comes
  // from the stellar's "desc" landing-description block
  // (NovaResource_LoadStellar- Description, Ghidra
  // Ui_LoadSelectionDialogResource); when it is absent the panel falls back to
  // the credits/fuel/hull status lines.
  const bool have_status = status_panel.w > 0.0F && status_panel.h > 0.0F;
  constexpr float kDescriptionInset = 4.0F;
  const float body_x = have_status ? status_panel.x + kDescriptionInset
                                   : panel.x + kDescriptionInset;
  float baseline = have_status ? status_panel.y + 13.0F : panel.y + 60.0F;
  const float body_w =
      have_status ? std::max(40.0F, status_panel.w - 2.0F * kDescriptionInset)
                  : std::max(40.0F, panel.w - 2.0F * kDescriptionInset);

  if (!description.empty()) {
    // Word-wrap the stellar description to the panel width (measured with the
    // same Geneva 9pt face used to draw) and lay the lines out from the inner
    // panel top, mirroring how the original fills the docked landing panel.
    constexpr float kDescriptionFontSize = 9.0F;
    constexpr float kLineHeight = 11.0F;
    const int wrap_w = static_cast<int>(std::lround(body_w));
    const auto desc_lines =
        WrapDescriptionLines(description, wrap_w, [&](std::string_view s) {
          return font_cache.TextWidth(NovaFontFamily::kGeneva,
                                      kDescriptionFontSize,
                                      kNovaFontStyleRegular,
                                      s);
        });
    for (const auto &desc_line : desc_lines) {
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    kDescriptionFontSize,
                    kNovaFontStyleRegular,
                    kBody,
                    body_x,
                    baseline,
                    desc_line);
      baseline += kLineHeight;
    }
  } else {
    // Fallback when no landing description resolved: the credits/fuel/hull
    // status lines the panel used to show (kept so a missing desc block never
    // leaves the inner panel blank).
    baseline += 20.0F;
    const std::string credits =
        "Credits: " + std::to_string(state.player.credits);
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kBody,
                  body_x,
                  baseline,
                  credits);

    const auto *eff = &state.cached_stats; // set during landing/refuel/repair
    const float cap = eff ? eff->fuel_capacity : 0.0F;
    char fuel[96];
    std::snprintf(fuel,
                  sizeof(fuel),
                  "Fuel: %.0f / %.0f      Hull: %.0f / %.0f",
                  state.player.fuel_points,
                  cap,
                  state.player.armor_points,
                  eff ? eff->max_armor_points : 0.0F);
    baseline += 18.0F;
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kBody,
                  body_x,
                  baseline,
                  fuel);
  }

  // The service buttons, drawn with the real three-state button art and their
  // labels centred in the body font. Following the original, there is NO
  // keyboard-focus/selection highlight: the pressed ("click") art marks the
  // slot the mouse currently hovers (NovaUi_HitTestAndTrackTravelActionButtons
  // redraws with the hovered index), and unavailable slots are drawn grey with
  // the disabled art (NovaUi_RedrawTravelActionButtons). Label baseline is
  // centred on the +5px-below-centre rule the original uses
  // (NovaUi_DrawThreeStateButton). Label colours follow the original's
  // three-state label table (NovaUi_InitThreeStateButtonArt DAT_007d8350):
  // white on the normal art, 50% grey on the pressed/hover and grey/disabled
  // art -- the shared renderer draws the label in the plain screen font (it
  // sets only the font id + size, never a bold style), so no bold here either.
  constexpr SDL_Color kButtonLabelNormal{255, 255, 255, 255};
  constexpr SDL_Color kButtonLabelGrey{128, 128, 128, 255};
  for (std::size_t i = 0; i < button_rects.size(); ++i) {
    const auto slot = button_rects[i].slot;
    const auto svc = static_cast<LandedService>(slot);
    const bool enabled = ServiceAvailable(state, ctx.stellar_id, svc);
    const bool hovered_by_mouse =
        enabled && hovered.has_value() && *hovered == slot;
    const auto button_state =
        !enabled
            ? ButtonState::kDisabled
            : (hovered_by_mouse ? ButtonState::kHover : ButtonState::kNormal);
    buttons.Draw(platform, button_rects[i].rect, button_state);
    const SDL_Color &label_color = !enabled           ? kButtonLabelGrey
                                   : hovered_by_mouse ? kButtonLabelGrey
                                                      : kButtonLabelNormal;
    const float label_baseline =
        ThreeStateButtonLabelBaseline(button_rects[i].rect);
    NovaText_DrawCentered(platform,
                          font_cache,
                          kThreeStateButtonFontFamily,
                          kThreeStateButtonFontSize,
                          kNovaFontStyleRegular,
                          label_color,
                          button_rects[i].rect.x,
                          button_rects[i].rect.x + button_rects[i].rect.w,
                          label_baseline,
                          ServiceLabel(svc));
  }
}

// Handles one service selection from the docked menu. Returns the exit intent:
// kLaunched when the player leaves the dock, otherwise kServiceComplete (the
// sub-screen ran and control returns to the menu). The buy/sell/outfit/shipyard
// /bar/starmap/mission sub-screens are all mocked for the MVP and return
// immediately; a reconstructed sub-screen would instead open its own modal and
// return kServiceComplete when it closes.
LandedExit DispatchService(SdlPlatform &platform,
                           SdlAudio &audio,
                           GameState &state,
                           LandedContext &ctx,
                           const std::function<void()> &render_background) {
  switch (ctx.selection) {
  case LandedService::kLaunch:
    NovaLog::Info("launching from stellar {} back into space",
                  static_cast<int>(ctx.stellar_id));
    return LandedExit::kLaunched;

  case LandedService::kRefuel: {
    const Stellar *stellar = state.scenario.Stellar(ctx.stellar_id);
    NovaLanded_Refuel(state, stellar != nullptr && stellar->dominated ? 0 : 1);
    return LandedExit::kServiceComplete;
  }

  case LandedService::kBuySellCargo:
  case LandedService::kOutfit:
  case LandedService::kShipyard:
  case LandedService::kBar:
  case LandedService::kMissionBbs: {
    // Render the sub-window as an on-screen dialog over the docked scene.
    // The service content runs in the dispatched modals
    // (NovaLanded_RunSubWindowDialog): buy/sell trade center, outfit and
    // shipyard stores, bar, and mission BBS; each draws its own frame PICT +
    // heading + Leave control.
    const LandedExit dialog_exit =
        NovaLanded_RunSubWindowDialog(platform,
                                      audio,
                                      state,
                                      ctx.selection,
                                      ctx.stellar_id,
                                      render_background);
    if (dialog_exit == LandedExit::kQuit) {
      return LandedExit::kQuit;
    }
    // Otherwise the dialog closed back to the dock menu normally. No service
    // currently hands a launch back to the dock loop; a future service-
    // internal "launch" path would return kLaunched here.
    return LandedExit::kServiceComplete;
  }

  case LandedService::kCount:
    break;
  }
  return LandedExit::kServiceComplete;
}

} // namespace

// ---------------------------------------------------------------------------
// Landed window modal run loop.
// ---------------------------------------------------------------------------
// Ghidra 0x00491f30 NovaUi_RunTravelDestinationInteractionLoop (partial port).
// NovaUi_RunTravelDestinationServicesWindow 0x0047c8e0 runs inline here: the
// landed services modal uses the Spaceport backdrop PICT 0x2134.
LandedExit NovaLanded_RunWindow(SdlPlatform &platform,
                                SdlAudio &audio,
                                GameState &state,
                                LandedContext &ctx,
                                const NovaPreferences &prefs,
                                HudRenderer &hud) {
  const SdlPlatform::ScopedPlacement restore_placement(
      platform, platform.current_placement());
  ProbeUiAutoClear probe_ui(platform);
  platform.probe().AutomationObservedDocked();
  state.gameplay_now_ms = platform.gameplay_ticks_ms();
  NovaLog::Info("opening landed services window at stellar {}",
                static_cast<int>(ctx.stellar_id));

  // Load the docked-screen backdrop and the destination planet picture.
  //
  // destination_art is the full-width dock backdrop: the Spaceport PICT 0x2134
  // (618x517) drawn across the whole 640x480 panel (via
  // g_travel_overlay_sprite_handle, Ghidra FUN_0048e970's base overlay); the
  // earlier 263x185 PICT 0x2137 is only a fallback when 0x2134 is unavailable.
  //
  // planet_art is the destination stellar's own planet picture (the "you are
  // here" surface panorama, PICT 0x2710 + link_a_id, or its custom picture id
  // when it sets one at CustPicID >= 0x80 - mirrors FUN_0048e970's
  // planet-PICT selection. The Spaceport DITL 0x3e8
  // carves a 612x285 outer panel at the top-centre of the docked window that is
  // exactly the pict's natural size, so the planet is drawn there at 1:1, over
  // the spaceport backdrop. When no stellar picture exists (hypergate / missing
  // strip) the panel falls back to a flat frame.
  std::unique_ptr<SdlTexture> destination_art;
  const auto *st_dec = state.scenario.Stellar(ctx.stellar_id);
  const auto load_pict = [&](std::uint16_t pict_id) {
    const auto data = NovaResource_LoadPictData(pict_id);
    const auto img = data ? Resource_LoadPictAsImage(*data) : std::nullopt;
    if (data && img) {
      return SdlTexture::Create(
          platform.renderer(), img->width, img->height, img->rgba_pixels);
    }
    return std::unique_ptr<SdlTexture>{};
  };
  std::unique_ptr<SdlTexture> planet_art;
  if (st_dec) {
    const std::int16_t stell_pict =
        (st_dec->custom_picture_or_gate_transition_frame >= 0x80)
            ? st_dec->custom_picture_or_gate_transition_frame
            : static_cast<std::int16_t>(st_dec->link_a_id + 0x2710);
    if (stell_pict >= 0x80) {
      planet_art = load_pict(static_cast<std::uint16_t>(stell_pict));
      if (planet_art) {
        NovaLog::Info("destination planet PICT 0x{} ({}) for stellar {}",
                      std::to_string(static_cast<int>(stell_pict)),
                      st_dec->name,
                      static_cast<int>(ctx.stellar_id));
      } else {
        NovaLog::Todo(
            "no destination planet PICT {} (link_a {} + 0x10000) for stellar "
            "'{}'; the outer panel stays a flat frame",
            static_cast<int>(stell_pict),
            static_cast<int>(st_dec->link_a_id),
            st_dec->name);
      }
    }
  }
  // Ghidra 0x00491f30 NovaUi_RunTravelDestinationInteractionLoop, ambient
  // slice. The DLOG 0x3e8 window is created with a per-frame interaction
  // callback (0x4928a0, folded into the parent by Ghidra) that plays the
  // stellar's Bible CustSndID as an ambient cue for the whole docked visit --
  // the same lifetime as CustPicID. NovaSound_LoadDecodedById(cust_snd_id)
  // runs at window open only when cust_snd_id >= 0x80 and g_pref_ambient_
  // sounds; the callback then (re)plays it every 0x1e0 + NovaRandom_Range(
  // 0x1e0) 60 Hz ticks (8..16 s, DAT_007d525c initialised to 0 so it fires on
  // the first frame), or gaplessly whenever the previous voice drains when
  // availability_flags (Bible Flags2) kContinuousAmbientSound is set. The
  // decoded handle is unregistered/freed on window close.
  std::optional<NovaSoundData> ambient_sound;
  bool ambient_continuous = false;
  if (st_dec != nullptr && prefs.ambient_sounds &&
      st_dec->cust_snd_id >= 0x80) {
    if (const auto resource = NovaResource_LoadSndData(
            static_cast<std::uint16_t>(st_dec->cust_snd_id))) {
      ambient_sound = NovaSound_Decode(*resource);
    }
    if (!ambient_sound) {
      NovaLog::Todo("docked ambient snd {} for stellar '{}' could not be "
                    "decoded; no CustSndID ambience",
                    static_cast<int>(st_dec->cust_snd_id),
                    st_dec->name);
    } else {
      ambient_continuous =
          (st_dec->availability_flags & Stellar::kContinuousAmbientSound) != 0U;
      NovaLog::Info("docked ambient snd {} for stellar '{}' ({})",
                    static_cast<int>(st_dec->cust_snd_id),
                    st_dec->name,
                    ambient_continuous ? "gapless" : "8..16s retrigger");
    }
  }
  // The original tags the ambient voice with the descriptor at
  // &DAT_00591ab4 (g_nova_control_bits[1512]); 1512 is reused here as the
  // SdlAudio voice key so CountActiveByKey can gate the gapless path. The
  // descriptor width is 32000 (NovaAudio_PreloadGameplayData), so the cue
  // outranks ordinary effects.
  constexpr int kAmbientSoundKey = 1512;
  constexpr int kAmbientPriorityWidth = 32000;
  std::uint64_t next_ambient_ms = 0;
  const auto tick_ambient = [&] {
    if (!ambient_sound) {
      return;
    }
    const std::uint64_t now = platform.gameplay_ticks_ms();
    if (ambient_continuous) {
      if (audio.CountActiveByKey(kAmbientSoundKey) == 0) {
        audio.Play(*ambient_sound,
                   1.0F,
                   1.0F,
                   kAmbientSoundKey,
                   kAmbientPriorityWidth);
      }
      return;
    }
    if (now < next_ambient_ms) {
      return;
    }
    audio.Play(
        *ambient_sound, 1.0F, 1.0F, kAmbientSoundKey, kAmbientPriorityWidth);
    std::uniform_int_distribution<std::int32_t> roll{0, 0x1e0 - 1};
    const std::uint64_t interval_ms =
        static_cast<std::uint64_t>(0x1e0 + roll(state.rng)) * 1000ULL / 60ULL;
    next_ambient_ms = now + interval_ms;
  };
  // DAT_007d525c starts at 0, so the callback fires on the first frame the
  // dock window is up.
  tick_ambient();
  destination_art = load_pict(0x2134);
  if (destination_art) {
    NovaLog::Info("landed dock backdrop PICT 0x2134");
  } else {
    NovaLog::Todo("docked backdrop PICT 0x2134 unavailable; falling back to "
                  "0x2137");
    destination_art = load_pict(0x2137);
  }
  if (!destination_art) {
    NovaLog::Warn("no docked backdrop decoded; drawing a flat backdrop");
  }
  // The docked panel is the native 618x517 Spaceport dialog, centred in the
  // unrestricted window coordinate space. Set the presentation before asking
  // for the window dimensions used by the DLOG/DITL layout.
  platform.SetPlacement(
      PlaceContained({618.0F, 517.0F}, platform.logical_playfield_size()));
  const SDL_FRect panel = DockedPanel(platform);

  // Lay out the docked screen from the real Spaceport DLOG/DITL 0x3e8
  // (centered window + per-item screen rects). Falls back to a hardcoded
  // grid when the dialog resources are unavailable.
  DockedLayout layout;
  NovaDialogWindow_Layout(panel, layout);

  // Load this stellar's landing description (the text shown in the docked inner
  // panel). Mirrors NovaUi_RunTravelDestinationInteractionLoop populating its
  // prompt buffer via Ui_LoadSelectionDialogResource with the destination
  // stellar's resource id.
  std::string description;
  const auto desc = NovaResource_LoadStellarDescription(ctx.stellar_id);
  if (desc) {
    description = desc->text;
    Mission_ExpandStringPlaceholders(state, description);
    NovaLog::Info("landed description for stellar {} ({} chars)",
                  static_cast<int>(ctx.stellar_id),
                  description.size());
  } else {
    NovaLog::Todo("no landing description desc for stellar {}; docked inner "
                  "panel falls back to the status lines",
                  static_cast<int>(ctx.stellar_id));
  }

  // Load the docked buttons' real three-state strip art (normal 0x1d4c..,
  // pressed 0x1d4f.., grey 0x1d52..) and lay out their desk rects. If the
  // strips are unavailable the buttons still render as flat fills behind the
  // labels.
  ServicesButtonArt button_art;
  const bool have_buttons = button_art.Initialize(platform);
  if (!have_buttons) {
    NovaLog::Warn("service button art unavailable");
  }
  const std::vector<ServiceButton> button_rects = BuildServiceButtons(layout);
  constexpr std::array<const char *, 7> kProbeNames{"launch",
                                                    "refuel",
                                                    "trade_center",
                                                    "outfitter",
                                                    "shipyard",
                                                    "mission_bbs",
                                                    "bar"};
  const auto publish_probe_controls = [&] {
    const Placement placement =
        PlaceContained({panel.w, panel.h}, platform.logical_playfield_size());
    const auto window_rect = [&placement](SDL_FRect rect) {
      return placement.ToWindowRect(rect);
    };
    std::vector<std::pair<std::string, SDL_FRect>> probe_controls{
        {"window", window_rect(panel)}};
    for (const auto &button : button_rects) {
      if (button.slot < kProbeNames.size()) {
        probe_controls.emplace_back(kProbeNames[button.slot],
                                    window_rect(button.rect));
      }
    }
    platform.PublishProbeUi("spaceport", std::move(probe_controls));
  };
  // Deliberately do not publish here. The probe surface is only truthful once
  // the input loop below owns the screen: the AvailLoc-3 offer pass and the
  // mission debrief readers between this point and the loop publish their own
  // modals and consume input. Publishing early let a harness observe
  // "spaceport" and fire a click that the offer/reader then swallowed, so the
  // dock looked open while the click never reached it.

  NovaLog::Todo(
      "docked screen geometry (panels, title band, buttons) is laid out from "
      "the real DITL 0x3e8, but the PICT sub-window frames (0x2135..) and the "
      "sub-window modals are out of scope");

  // Font subsystem init (SDL3_ttf) is refcounted and managed lazily by the
  // NovaFontCache itself (TTF_Init on first use, TTF_Quit in ~NovaFontCache),
  // so repeated dockings stay stable. Here we only surface the availability of
  // the faces the original ships/substitutes so a missing bundle is loud.
  NovaFontCache font_cache;
  NovaLog::Info("landed window font families available: Charcoal={} "
                "Geneva={} Times={} Helvetica={} NewYork={}",
                font_cache.IsFamilyAvailable(NovaFontFamily::kChicago),
                font_cache.IsFamilyAvailable(NovaFontFamily::kGeneva),
                font_cache.IsFamilyAvailable(NovaFontFamily::kTimes),
                font_cache.IsFamilyAvailable(NovaFontFamily::kHelvetica),
                font_cache.IsFamilyAvailable(NovaFontFamily::kNewYork));
  if (!font_cache.IsFamilyAvailable(NovaFontFamily::kChicago) ||
      !font_cache.IsFamilyAvailable(NovaFontFamily::kGeneva)) {
    NovaLog::Warn(
        "bundled Charcoal.ttf/Geneva.ttf not found next to the executable "
        "(looked in EV Nova/); text will not render");
  }

  bool entered_sub_screen = false;

  // Re-renders the docked menu each frame: sub-dialogs (BBS, stores, offer
  // windows, text readers) layer themselves over this live background instead
  // of a captured snapshot (deliberate divergence, see
  // docs/dlog_ditl_dialog_format.md).
  const std::function<void()> render_background = [&] {
    DrawLandedMenu(platform,
                   &hud,
                   font_cache,
                   button_art,
                   state,
                   ctx,
                   destination_art ? destination_art->get() : nullptr,
                   planet_art ? planet_art->get() : nullptr,
                   description,
                   panel,
                   layout,
                   button_rects,
                   std::nullopt);
  };

  // Ghidra 0x00491f30: after the interaction loop exits (launch or quit), the
  // escort fleet trade pass (Player_ProcessEscortFleetAtStellar 0x004229d0)
  // runs before the window comes down. Its summary and the payroll defection
  // message are text-reader modals layered over the still-live dock. Wrap every
  // loop exit so the pass runs exactly once.
  const auto show_text = [&](const std::string &text) {
    NovaUi_RunTextReaderDialog(platform, state, text, false, render_background);
  };
  const auto finish = [&](LandedExit exit) {
    audio.StopByKey(kAmbientSoundKey);
    Player_ProcessEscortFleetAtStellar(state, ctx.stellar_id, show_text);
    return exit;
  };

  // Activates the currently-selected service, leaving the dock when the player
  // picks Launch. On a mocked sub-screen (trade/outfit/shipyard/bar/...) the
  // modal stays open (DispatchService returns kServiceComplete). Guards against
  // activating a gated/disabled slot, mirroring the original only accepting
  // usable action buttons. Returns true when the dock is left.
  auto activate_selection = [&]() -> bool {
    if (!ServiceAvailable(state, ctx.stellar_id, ctx.selection)) {
      return false;
    }
    LandedExit exit =
        DispatchService(platform, audio, state, ctx, render_background);
    if (exit == LandedExit::kLaunched) {
      return true;
    }
    entered_sub_screen = false;
    return false;
  };

  // Keep mouse coordinates in the same unrestricted window space as the
  // native-size DLOG/DITL geometry, even if the previous context was flight.
  platform.SetPlacement(
      PlaceContained({618.0F, 517.0F}, platform.logical_playfield_size()));

  // Mission offers with AvailLoc 3 pop as the player docks: the Spaceport
  // loop (NovaUi_RunTravelDestinationInteractionLoop 0x00491f30) sets
  // g_misn_list_page_group = 3 and calls Mission_RunAvailLocOffers(3)
  // (0x00448670) right after the window is up, before its
  // input loop. Reproduce that ordering: render the dock once, then run the
  // offer pass over the live dock (the offer window re-renders it each
  // frame). TODO(decomp):
  // the original also re-runs the pass on Spaceport action 0xf and consumes
  // the DAT_00776af4 recheck timer in the services windows.
  DrawLandedMenu(platform,
                 &hud,
                 font_cache,
                 button_art,
                 state,
                 ctx,
                 destination_art ? destination_art->get() : nullptr,
                 planet_art ? planet_art->get() : nullptr,
                 description,
                 panel,
                 layout,
                 button_rects,
                 std::nullopt);
  platform.Present();
  // Mission resolution runs once on Spaceport entry, before the AvailLoc-3
  // offer pass below. That is the original's position in
  // NovaUi_RunTravelDestinationInteractionLoop (0x00491f30), which calls
  // Mission_TickReactionSlotsForTravelInteraction (0x00443780) a single
  // time after the window is up; the loop's per-action 0xf arm only re-runs
  // the offer pass. The success/failure debrief readers layer over the live
  // dock through render_background.
  Mission_TickReactionSlotsForTravelInteraction(
      state,
      ctx.stellar_id,
      static_cast<std::uint32_t>(platform.gameplay_ticks_ms()),
      [&](const MissionDialogText &message) {
        NovaUi_RunTextReaderDialog(platform,
                                   state,
                                   message.text,
                                   false,
                                   render_background,
                                   message.dialog_variant);
      });
  (void)Mission_RunAvailLocOffers(
      state,
      3,
      static_cast<std::uint32_t>(platform.gameplay_ticks_ms()),
      [&](std::int16_t mission_def) {
        return NovaMission_RunOfferWindow(
            platform, audio, state, mission_def, render_background);
      });

  while (!platform.quit_requested()) {
    // Ambient CustSndID retrigger (0x4928a0 callback), once per frame.
    tick_ambient();
    // Nested modals clear their published controls on return. Restore the
    // Spaceport's semantic surface at the same point its root face resumes.
    publish_probe_controls();
    // Compute the mouse-hovered service (for hover state on the buttons).
    std::optional<std::uint8_t> hovered;
    if (!entered_sub_screen) {
      const auto c = ServiceButtonAt(button_rects, platform.mouse_position());
      // Only enabled slots highlight (the original's hit-test filters gated
      // buttons before returning a hovered index).
      if (c) {
        const auto svc = static_cast<LandedService>(*c);
        hovered =
            ServiceAvailable(state, ctx.stellar_id, svc) ? c : std::nullopt;
      }
    }
    // Draw the current face. When a sub-screen is "open" the original swaps to
    // a nested modal widget; the MVP keeps the same menu surface and only
    // distinguishes via the hint, so we always redraw the list.
    DrawLandedMenu(platform,
                   &hud,
                   font_cache,
                   button_art,
                   state,
                   ctx,
                   destination_art ? destination_art->get() : nullptr,
                   planet_art ? planet_art->get() : nullptr,
                   description,
                   panel,
                   layout,
                   button_rects,
                   hovered);
    // DrawLandedMenu temporarily restores window-space for the HUD. Input and
    // semantic hit rectangles resume the native Spaceport placement.
    platform.SetPlacement(
        PlaceContained({panel.w, panel.h}, platform.logical_playfield_size()));
    platform.Present();

    // Poll discrete raw keys for the modal (dedicated channel, so it never
    // interferes with the spaceflight flight controls). Keyboard shortcuts are
    // the first/mnemonic letter of each service, matching the travel-scene
    // action dispatch in NovaUi_RunTravelDestinationInteractionLoop
    // (0x00491f30): r/f refuel, c/t trade, o outfit, s shipyard, n mission,
    // b bar, Enter/Esc leave. There is no keyboard focus/highlight state.
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      if (in->key == TextKey::escape) {
        // Esc leaves a sub-screen back to the menu, or leaves the dock.
        if (entered_sub_screen) {
          entered_sub_screen = false;
        } else {
          return finish(LandedExit::kLaunched);
        }
        continue;
      }
      if (in->key == TextKey::enter) {
        // Enter leaves the dock, matching the original's Enter/Esc exit code.
        return finish(LandedExit::kLaunched);
      }
      if (in->key == TextKey::primary) {
        // Left-click a service button: activate the hovered (and only if
        // enabled) slot, like the original's mouse-driven services buttons.
        if (!entered_sub_screen) {
          if (const auto slot =
                  ServiceButtonAt(button_rects, platform.mouse_position())) {
            ctx.selection = static_cast<LandedService>(*slot);
            if (ServiceAvailable(state, ctx.stellar_id, ctx.selection)) {
              LandedExit exit = DispatchService(
                  platform, audio, state, ctx, render_background);
              if (exit == LandedExit::kLaunched) {
                return finish(LandedExit::kLaunched);
              }
              entered_sub_screen = false;
              // Do not dispatch another queued key against the just-closed
              // service. The next outer iteration must redraw the dock first,
              // so a following Mission BBS opens over the dock rather than
              // snapshotting the previous store frame.
              break;
            }
          }
        }
        continue;
      }
      if (in->key == TextKey::character) {
        if (entered_sub_screen) {
          continue;
        }
        // First-letter service shortcuts (mirror the travel interaction loop
        // key comparisons). Each is case-insensitive (SDL3 reports letter keys
        // lower-case, but normalise anyway).
        const char kc = static_cast<char>(
            std::tolower(static_cast<unsigned char>(in->character)));
        switch (kc) {
        case 'r':
        case 'f':
          ctx.selection = LandedService::kRefuel;
          break;
        case 'c':
        case 't':
          ctx.selection = LandedService::kBuySellCargo;
          break;
        case 'o':
          ctx.selection = LandedService::kOutfit;
          break;
        case 's':
          ctx.selection = LandedService::kShipyard;
          break;
        case 'n':
          ctx.selection = LandedService::kMissionBbs;
          break;
        case 'b':
          ctx.selection = LandedService::kBar;
          break;
        case 'l':
          // "Launch": a synonym for Enter/Esc (the docked Leave slot).
          return finish(LandedExit::kLaunched);
        default:
          continue;
        }
        if (activate_selection()) {
          return finish(LandedExit::kLaunched);
        }
        // A nested service may have changed the renderer presentation and
        // left its frame visible. Establish a redraw boundary before handling
        // any additional queued input.
        break;
      }
    }
    platform.PaceFrame();
  }
  return finish(LandedExit::kQuit);
}

} // namespace game
