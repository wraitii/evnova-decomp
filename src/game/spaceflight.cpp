#include "spaceflight.hpp"

#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "hud_renderer.hpp"
#include "intro_cinematic.hpp"
#include "outfit.hpp"
#include "spaceflight_view.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace game {
namespace {

// Frame_TickSystems preserves the original scope order. Unimplemented scopes
// intentionally do nothing; logging them per frame would overwhelm diagnostics.
void Stub_PlayerCore(GameState &state) { (void)state; }

void Stub_Collisions(GameState &state) { (void)state; }

void Stub_DrawStatus(GameState &state) { (void)state; }

void Stub_AiRoutines(GameState &state) { (void)state; }

void Stub_MissionAndMiscHandlers(GameState &state) { (void)state; }

void Stub_CalcAiOdds(GameState &state) { (void)state; }

void Stub_HandleShots(GameState &state) { (void)state; }

void Stub_HandleShips(GameState &state) { (void)state; }

void Stub_MiscHandlers(GameState &state, bool run_full_tick) {
  (void)state;
  (void)run_full_tick;
}

void Stub_BeamHitQueue(GameState &state) { (void)state; }

// Ghidra 0x004186b0 Frame_TickSystems. Reconstructs only the *structure*:
// the scope ordering and the run_full_tick gate. Each scope is a loud stub
// (see above). Called full before drawing and reduced during transitions.
void NovaFrame_TickSystems(GameState &state, bool run_full_tick) {
  // scope 10 "player": always runs.
  Stub_PlayerCore(state);
  // scope 9 "collisions": always runs.
  Stub_Collisions(state);

  if (run_full_tick) {
    Stub_DrawStatus(state);             // scope 0xc
    Stub_AiRoutines(state);             // scope 6 (part 1: targeting setup)
    Stub_MissionAndMiscHandlers(state); // scope 0xb
    Stub_AiRoutines(state);             // scope 6 (part 2: per-ship AI)
    Stub_CalcAiOdds(state);             // scope 0x14
  }

  // Always-run scopes that keep advancing during frozen transitions.
  Stub_HandleShots(state);                 // scope 7
  Stub_HandleShips(state);                 // scope 4/5
  Stub_MiscHandlers(state, run_full_tick); // scope 8
  Stub_BeamHitQueue(state);
}

// Draw one in-game frame. The starfield, stellar bodies and the pilot's ship
// are handled by the SpaceflightView (gh.getId 0x00417600 scope 2 sprite-world
// draw + Frame_RenderViewportBackground); the gov-specific HUD (cockpit PICT,
// life-support bars and readouts) is composited by the HudRenderer over the
// world, unscaled. The world view is centred on the player so the ship sits at
// the play-area centre.
void DrawInGameFrame(SdlPlatform &platform,
                     GameState &state,
                     SpaceflightView &view,
                     HudRenderer &hud) {
  // The free-flight world extends: draw 1:1 across the whole (possibly larger)
  // window with no centre-clipping. The landed modal already restores its own
  // centred playfield each frame, so re-assert the fullscreen viewport here.
  platform.SetFullscreenPlayfield();
  view.Draw(platform, state);
  // HUD overlays the extending world at fixed, unscaled size (the project's
  // resolution policy: more window = more system shown, NOT a bigger HUD).
  hud.Draw(platform, state);
}

// Ghidra 0x00417600 Frame_SpaceflightLoop main loop. Reconstructs the outer
// phase skeleton (setup + full first tick, then per-frame pre-draw/sim,
// drawing, post-draw) and the run_full_tick freeze gate. Simulation is still
// yielded to NovaFrame_TickSystems's stubs, but the rendering is live: stellar
// bodies, the parallax starfield and the player's rotating ship are drawn.
// Integrates the player's heading/throttle from the live keyboard into the
// PlayerShip. This is a lightweight stand-in for Ship_HandlePlayerShipCore's
// movement: turn by a fixed rate per frame, accelerate toward the current
// heading while thrust is held, apply a soft drag, and clamp speed.
// TODO(decomp): replace these provisional turn/thrust/drag constants with the
// ship-class-derived rates (base_turn_rate_deg, base_speed, accel) once the
// movement sim is reconstructed.
void NovaFrame_SpaceflightLoop(SdlPlatform &platform,
                               SdlAudio &audio,
                               GameState &state,
                               bool &returning_to_menu) {
  SpaceflightView view;
  // The government-specific HUD (Interface layout + cockpit PICT) is resolved
  // once on spaceflight entry (Ui_InstallGameplayInterfaceLayout) and re-
  // composited over the world each frame.
  HudRenderer hud;
  hud.Install(platform, state);

  // Preload the fire sounds the player's owned primary weapons use so the
  // first volley's sound is already decoded (mirrors the original preloading
  // the gameplay sound-handle table at startup).
  NovaWeapon_PreloadOwnedFireSounds(state);

  // ---- Pre-loop setup -----------------------------------------------------
  // Ghidra: rebuilds the stellar radar panel, evaluates availability, updates
  // system/stellar display state, then runs a full TickSystems with
  // g_gameplay_time_frozen and draws the first gameplay frame. The radar panel
  // and per-tick sprite display state are still not reconstructed; the stellar
  // availability re-evaluation (the part of System_UpdateSystemAnd-
  // StellarDisplayState that re-homes each stellar to its system and sets
  // is_available / hazard flags) is now live via NovaTargeting_UpdateStellar-
  // Availability. Radar-panel rebuild is a logged divergence.
  NovaTargeting_UpdateStellarAvailability(state);
  NovaLog::Todo("spaceflight pre-loop setup: stellar radar panel rebuild and "
                "per-tick sprite display state still not reconstructed");
  const bool ship_ready = view.EnsureShipSprite(platform, state);
  (void)ship_ready;
  // Ghidra: the ambient starfield is (re)spawned at every spaceflight entry
  // (NovaEffects_QueuedAmbientStarParticles from Ship_RunSpaceflightMode and
  // the travel/landing transitions). We spawn once when the mode starts, then
  // advance it per frame below.
  view.SpawnAmbientStars(platform, state);
  NovaFrame_TickSystems(state, /*run_full_tick=*/true);
  DrawInGameFrame(platform, state, view, hud);
  SDL_RenderPresent(platform.renderer());

  // ---- Main loop ----------------------------------------------------------
  // Ghidra exits when DAT_00596d39 (hard quit) or DAT_00596d38 (the primary
  // mouse command latched in post-draw, i.e. return-to-menu-from-pause) is set.
  // The pause menu is not reconstructed, so Escape/'q' stand in for the
  // return-to-menu latch (documented divergence).
  // Ghidra Frame_SpaceflightLoop scope 1 stores the ship's pre-tick position
  // (_DAT_005997c4/_c8) so the per-frame ambient-star parallax can be computed
  // from the movement delta after simulation.
  float prev_x = state.player.pos_x;
  float prev_y = state.player.pos_y;
  // Real frame-time basis for the per-frame ambient/stellar animation steppers
  // (the original accumulates _g_avg_frame_time_ms).
  std::uint64_t prev_tick_ms = SDL_GetTicks();
  bool target_cycle_was_held = false;
  bool target_action_was_held = false;
  while (!platform.quit_requested() && !returning_to_menu) {
    const std::uint64_t now_ms = SDL_GetTicks();
    const float frame_time_ms =
        std::max(1.0F, static_cast<float>(now_ms - prev_tick_ms));
    prev_tick_ms = now_ms;
    // Player control (heading/throttle) is read here once so the ship flies
    // while the simulation stubs do not, and the same snapshot feeds the
    // travel/jump channel. Movement integrates into PlayerShip.
    const FlightInput input = platform.PollFlightInput();
    const bool target_cycle =
        input.cycle_target_next || input.cycle_target_previous;
    if (target_cycle && !target_cycle_was_held) {
      NovaTargeting_CyclePlayerStellarTarget(state, input.cycle_target_next);
    }
    target_cycle_was_held = target_cycle;
    const bool target_action_pressed =
        input.target_action && !target_action_was_held;
    target_action_was_held = input.target_action;
    NovaPlayer_UpdateFromInput(state, input);
    // Advance the player's fired shots/cooldowns from the previous frame, then
    // handle this frame's fire input. Mirrors Ship_HandlePlayerShipControl
    // firing the primary bank(s) while the fire command is held. frame_time_ms
    // feeds the time-animated shot-frame cadence (Shot_HandleShot's animated
    // branch); static/heading shot sets ignore it.
    NovaWeapon_TickShots(state, frame_time_ms);
    if (input.fire) {
      NovaWeapon_FirePlayerPrimary(state);
      // Play each weapon fire sound queued this frame (a round actually
      // spawned). The firing routine appends the fire_sound slot to
      // GameState.pending_fire_sound_slots; this loop owns the SdlAudio device
      // and plays the decoded sound, then clears the queue. Mirrors the
      // original's per-volley NovaAudio_PlaySpatialByDistance.
      for (const std::int16_t slot : state.pending_fire_sound_slots) {
        if (slot < 0 || slot >= 36) {
          continue;
        }
        const auto &sound = state.weapon_fire_sounds[slot];
        if (sound.has_value()) {
          audio.Play(*sound);
        }
      }
      state.pending_fire_sound_slots.clear();
    }
    // Cross-system hyperspace jump state machine (travel.cpp): engages on the
    // 'j' key near an available travel point, then tick the countdown.
    NovaTravel_Tick(state, input.travel, frame_time_ms);
    // When a jump completed this frame, re-spawn the starfield for the new
    // system (the original's jump completion re-runs
    // NovaEffects_QueuedAmbientStarParticles).
    if (state.travel.just_completed) {
      view.SpawnAmbientStars(platform, state);
    }
    // Seed an automatic target, while retaining a stellar chosen by Tab/
    // Shift+Tab. This keeps navigation purposeful instead of retargeting to
    // whichever body happens to be closest each frame.
    NovaTargeting_UpdatePlayerTarget(state);
    // Target-action command ('e'): the original opens the destination
    // interaction window here. It does not dock or perform a physical stellar
    // collision; any later travel/landing result belongs to that modal's
    // confirmed action path.
    if (target_action_pressed) {
      if (NovaTargeting_CanOpenTravelDestinationInteraction(state)) {
        const auto *st = state.scenario.Stellar(state.travel.selected_stellar_id);
        NovaLog::Todo("target-action: destination-interaction window 0x3f1 for "
                      "stellar {} ({}) is the next reconstruction boundary; "
                      "do not enter Spaceport directly",
                      state.travel.selected_stellar_id,
                      st ? st->name : "(unknown)");
      } else {
        NovaLog::Info("target-action: selected stellar cannot open its "
                      "destination interaction");
      }
    }
    // In-flight shield regeneration (class base + opcode-5 outfit bonuses),
    // scaled by the real frame time. The original's player-update path ticks
    // shields each frame; armor does not regenerate in flight.
    NovaPlayer_TickShieldRecharge(state, frame_time_ms);
    const float delta_x = state.player.pos_x - prev_x;
    const float delta_y = state.player.pos_y - prev_y;
    prev_x = state.player.pos_x;
    prev_y = state.player.pos_y;
    // Unified per-frame world animation pass: advances the ambient starfield by
    // the ship's movement delta (NovaEffects_UpdateAmbientStarParticles) and
    // steps each animated stellar one animation segment (Stellar_UpdateStellar-
    // Sprites), both on the single frame_time_ms cadence (see the header).
    view.AdvanceAnimations(platform, state, frame_time_ms, delta_x, delta_y);
    // Re-derive stellar availability for the current system each tick (scope 3
    // of System_UpdateSystemAndStellarDisplayState). This keeps stellar
    // is_available / hazard state in step as the player moves between systems.
    NovaTargeting_UpdateStellarAvailability(state);

    // Ghidra scope 1 "pre-draw tasks": full TickSystems + ambient particles +
    // cursor update.
    NovaFrame_TickSystems(state, /*run_full_tick=*/true);

    // Ghidra scope 2 "drawing": sprite world present + viewport particles +
    // commit frame.
    DrawInGameFrame(platform, state, view, hud);
    SDL_RenderPresent(platform.renderer());

    // Ghidra scope 3 "post-draw tasks": pump the primary mouse command; when
    // latched bit sets DAT_00596d38 (return to menu) and, while the frame is
    // frozen, runs a *reduced* TickSystems(0). Here Escape/q set the
    // return-to-menu latch; the frozen reduced tick is skipped (no transition
    // is active in this build).
    for (std::optional<TextInput> text_event;
         (text_event = platform.PollTextEvent());) {
      if (text_event->key == TextKey::escape ||
          (text_event->key == TextKey::character &&
           text_event->character == 'q')) {
        returning_to_menu = true;
        break;
      }
    }
    SDL_Delay(16);
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Player ship movement (free flight)
// ---------------------------------------------------------------------------
// GHIDRA 0x0044aa70 Ship_HandlePlayerShipCore drives the player ship; the
// actual per-frame integration lives in 0x00433050 Ship_HandleShip with the
// polar helpers Math_AddPolarVelocity (0x0043b4a0) and
// Math_AddPolarVelocityWithClamp (0x0043b4e0). Reconstructs that movement
// model for the player instead of the old fixed-constant stand-in:
//
//  * Stats come from the ship class (ShipClassDef). The scenario loader scales
//    the raw resource shorts exactly as the original (NovaData_LoadScenario-
//    ResourceTables 0x004bd3c0):
//        accel    -> thrust (px/frame^2):  raw_accel / 10000.0 (DAT_00575e68)
//        speed    -> top speed (px/frame): raw_speed / 640.0 (DAT_00575e48)
//        maneuver -> turn rate (deg/frame): raw_maneuver * 0.1 (DAT_00575e58)
//    Starter (sh.x9an 0x80): accel 0.05, speed 0.625, turn 4.0 deg/frame.
//    These per-frame values hold at the original's reference cadence (the game
//    scales them by g_avg_frame_time_ms; see note at the end).
//
//  * Turn follows the held key at the class turn rate continuously (no
//    heading integration besides the rate), matching Ship_ComputeShipMaxTurn-_
//    RateDeg making the ship bank at max rate under direct control. Heading 0
//    points 'up', increases clockwise (Math_AddPolarVelocity convention).
//
//  * Thrust accelerates along the heading as a polar velocity step clamped
//    per-AXIS to the projection of the class top speed (Math_AddPolarVelocity-
//    WithClamp 0x0043b4e0). The clamp only caps each axis's additional thrust
//    at its polar max projection; it is NOT a vector-magnitude governor, so a
//    ship turning at full thrust can build a small off-axis component that
//    pushes its net speed modestly past the nominal top speed (the authentic
//    EVN drift). The original's throttle is high enough to top out within a
//    few frames.
//
//  * WHEN THROTTLE RELEASED THE SHIP COASTS -- there is NO continuous velocity
//    drag in the original's free-flight path, so a released ship keeps most of
//    its momentum. The old stand-in's per-frame kDrag multiply was the main
//    fidelity bug (made ships feel mushy and never reach a crisp cruise).
//
//  * BRAKE ('s'/down) is a separate reverse-to-rest path (see body), not a
//    second thrust sign, because the original's reverse is governed by
//    ai_desired_speed / reverse_speed_bias rather than a mirrored forward
//    thrust.
//
// NOTE(decomp) scale/cadence: the original integrates over g_avg_frame_time_ms
// (0x00735448, Frame_MeasureFrameTiming 0x00432ea0) so ship motion is cadence-,
// not fixed-step, independent. The reimplementation loop is per-frame fixed
// cadence (SDL_Delay(16) in NovaFrame_SpaceflightLoop), so the class stats are
// used directly as per-frame values and the frames-per-second of the host
// dictates on-screen pace; this restores the *relative* handling between ship
// classes and the inertia feel, but the absolute pace/turn should be re-checked
// against a real capture once the world->viewport transform is reconstructed.
// Pure per-frame movement integration (unit-tested in tests/movement_test.cpp).

// One per-axis step of Ghidra Math_AddPolarVelocityWithClamp (0x0043b4e0), the
// original's single forward-thrust pathway (Ship_HandleShip calls it with
// `ai_forward_thrust_cmd * frame_time` as the base speed). For each velocity
// axis it combines the polar projection of the class top speed (max_proj) with
// the polar projection of the per-frame thrust step (delta) and the current
// velocity, exactly as decoded:
//
//   if (delta <= 0 || max_proj <= 0) {
//       if (delta < 0 && max_proj < 0) { if (max_proj < cur) cur += delta; }
//       else                             cur += delta;
//   } else if (cur < max_proj)          cur += delta;
//
// The clamp therefore only CAPS additional thrust on an axis at its polar max
// projection; it never pulls an existing (e.g. drift-built) component back to
// enforce a vector-magnitude limit. Heading uses the polar convention from
// Math_AddPolarVelocity (0x0043b4a0): vel_x += sin(h)*s ; vel_y -= cos(h)*s.
static void NovaPlayer_AddPolarVelocityClamped(float heading_rad,
                                               float thrust_step,
                                               float max_speed,
                                               float &vel_x,
                                               float &vel_y) {
  const float sin_h = std::sin(heading_rad);
  const float cos_h = std::cos(heading_rad);
  auto axis_step = [](float max_proj, float delta, float cur) -> float {
    if (delta <= 0.0F || max_proj <= 0.0F) {
      if (delta < 0.0F && max_proj < 0.0F) {
        if (max_proj < cur) {
          cur += delta;
        }
      } else {
        cur += delta;
      }
    } else if (cur < max_proj) {
      cur += delta;
    }
    return cur;
  };
  vel_x = axis_step(sin_h * max_speed, sin_h * thrust_step, vel_x);
  vel_y = axis_step(-cos_h * max_speed, -cos_h * thrust_step, vel_y);
}

[[nodiscard]] PlayerMovementStats NovaPlayer_IntegrateMovement(
    PlayerShip &ship, const FlightInput &input, const ShipClass &ship_class) {
  constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
  constexpr float kTwoPi = 6.283185307179586F;

  PlayerMovementStats stats;
  stats.turn_rate_deg_per_frame =
      static_cast<float>(ship_class.turn_rate) * 0.1F;
  stats.max_speed_px_per_frame = static_cast<float>(ship_class.speed) / 640.0F;
  stats.thrust_px_per_frame2 = static_cast<float>(ship_class.accel) / 10000.0F;
  const float turn_rad_per_frame = stats.turn_rate_deg_per_frame * kDegToRad;
  const float reverse_accel = stats.thrust_px_per_frame2 * 2.0F;

  ship.engine_thrust = input.thrust;
  const bool retro_thrust =
      input.brake && !input.thrust; // brake excludes thrust

  // Heading: bank continuously at the class turn rate while a turn key is held.
  if (input.turn_left) {
    ship.heading -= turn_rad_per_frame;
  }
  if (input.turn_right) {
    ship.heading += turn_rad_per_frame;
  }
  ship.heading = std::fmod(ship.heading + kTwoPi, kTwoPi);
  if (ship.heading < 0.0F) {
    ship.heading += kTwoPi;
  }

  if (input.thrust) {
    // Forward thrust: polar step toward the heading, per-axis clamped to the
    // class top speed projection (Math_AddPolarVelocityWithClamp semantics).
    NovaPlayer_AddPolarVelocityClamped(ship.heading,
                                       stats.thrust_px_per_frame2,
                                       stats.max_speed_px_per_frame,
                                       ship.vel_x,
                                       ship.vel_y);
  }

  if (retro_thrust) {
    // Reverse thrust opposes the current velocity: reduce the velocity vector's
    // magnitude toward zero by the reverse-accel step, without a proportional
    // multiplier (so it lands exactly on rest instead of the decaying-decay
    // overshoot a `vel *= (1-k)` with k>1 causes). Direction is preserved.
    const float speed =
        std::sqrt(ship.vel_x * ship.vel_x + ship.vel_y * ship.vel_y);
    if (speed > 1e-4F) {
      const float reduce = std::min(reverse_accel, speed);
      const float scale = (speed - reduce) / speed;
      ship.vel_x *= scale;
      ship.vel_y *= scale;
    } else {
      ship.vel_x = 0.0F;
      ship.vel_y = 0.0F;
    }
  }

  ship.pos_x += ship.vel_x;
  ship.pos_y += ship.vel_y;
  ship.speed = std::sqrt(ship.vel_x * ship.vel_x + ship.vel_y * ship.vel_y);
  return stats;
}

void NovaPlayer_UpdateFromInput(GameState &state, const FlightInput &input) {
  PlayerShip &p = state.player;

  // Resolve the outfit-derived effective movement stats (class base + owned
  // outfit opcode 7/8/9 bonuses), reading the cached snapshot when still
  // valid. The cache is invalidated by the inventory mutation helpers and the
  // new-pilot ship-class reset.
  if (!state.stat_cache_valid) {
    state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
    state.stat_cache_valid = true;
  }
  const PlayerEffectiveStats &eff = state.cached_stats;

  // Map the effective raw stats onto the movement integrator's ShipClass view
  // (it divides raw accel/speed by the loader scale; turn is deg/frame).
  ShipClass effective_class;
  effective_class.accel = eff.thrust_raw;
  effective_class.speed = eff.speed_raw;
  effective_class.turn_rate = eff.turn_raw;
  (void)NovaPlayer_IntegrateMovement(p, input, effective_class);

  // Engine-glow intensity ramp toward the binary thrust target (clean-room
  // stand-in for the original dimming the glow with ai_forward_thrust_cmd
  // throttle; TODO(decomp): drive by thrust magnitude once the command channel
  // exists).
  constexpr float kGlowRiseRate = 0.12F;
  constexpr float kGlowDecayRate = 0.05F;
  p.engine_glow_intensity +=
      (p.engine_thrust ? kGlowRiseRate : -kGlowDecayRate);
  p.engine_glow_intensity = std::clamp(p.engine_glow_intensity, 0.0F, 1.0F);
}

void NovaPlayer_TickShieldRecharge(GameState &state, float frame_time_ms) {
  PlayerShip &p = state.player;
  // Resolve the effective stats (cache when possible) for the current
  // recharge rate and max shield.
  if (!state.stat_cache_valid) {
    state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
    state.stat_cache_valid = true;
  }
  const PlayerEffectiveStats &eff = state.cached_stats;
  // The recharge rate is in shield points per frame at the reference cadence
  // (the movement model / Ship_ComputeShipShieldRechargeRate use per-frame); we
  // scale it by the real frame time so the on-screen pace matches the host.
  const float rate = eff.shield_recharge * frame_time_ms / 1000.0F;
  if (rate <= 0.0F) {
    return;
  }
  p.shield_points = std::min(eff.max_shield_points, p.shield_points + rate);
}

void NovaSpaceflight_Run(SdlPlatform &platform,
                         SdlAudio &audio,
                         GameState &state) {
  NovaLog::Info("entering spaceflight mode");

  // Preflight: the new-game intro cinematic plays on the pilot's first entry
  // (Ghidra DAT_00596d35 == 0 in Ship_RunSpaceflightMode). We set the
  // intro_played latch *after* the intro returns, exactly as the original sets
  // DAT_00596d35 = 0x01 immediately after IntroCinematic_Run(). The intro's
  // skip result already gates (a stub of) the post-intro travel-selection
  // dialog internally, so its return value needs no action here.
  if (!state.intro_played) {
    (void)NovaIntroCinematic_Run(platform, state);
    // Ghidra: DAT_00596d35 = 0x01, the latch IntroCinematic_SetupFrames/
    // Game_ResetNewGameState clear on a new pilot (see new_pilot_flow.cpp).
    state.intro_played = true;
  }

  // Ghidra Ship_RunSpaceflightMode: preflight owns the gameplay surface, runs
  // Frame_SpaceflightLoop, then tears the mode back down to the menu shell.
  bool returning_to_menu = false;
  NovaFrame_SpaceflightLoop(platform, audio, state, returning_to_menu);

  NovaLog::Info("leaving spaceflight mode to the main menu");
}

} // namespace game
