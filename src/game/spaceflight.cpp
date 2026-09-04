#include "spaceflight.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "asteroid.hpp"
#include "boarding_plunder.hpp"
#include "collision.hpp"
#include "docked_dialog.hpp"
#include "game_state.hpp"
#include "hud_overlay.hpp"
#include "hud_renderer.hpp"
#include "impact_effects.hpp"
#include "intro_cinematic.hpp"
#include "landed_window.hpp"
#include "mission.hpp"
#include "negotiation_dialog.hpp"
#include "outfit.hpp"
#include "radar_panel.hpp"
#include "ship_ai.hpp"
#include "ship_comm_dialog.hpp"
#include "ship_spawn.hpp"
#include "ship_visual.hpp"
#include "spaceflight_view.hpp"
#include "starmap.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <string>

namespace game {

// --- PlayerTick_StatusAndOutfitEvents constants (Ghidra globals) -----------
// g_fire_restricted_velocity_damp (0x00575570, double): per-frame velocity
// damping while disabled (disabled).
constexpr float kPlayerFireRestrictedVelocityDamp = 0.995F;
// Player recently-hit timer (g_player_recently_hit_timer, DAT_0073549c):
// armed to 300 ticks when the player takes a hit, decays one tick per frame
// while at or above g_hyperspace_progress_onset_threshold (0x575540, 0.0);
// gates the disabled auto-repair pass until the post-hit regen-suppression
// window expires.
constexpr float kRecentlyHitRegenCutoff = 0.0F;
// g_jump_turnaround_turn_rate_addend (0x0057555c, float): death-timer
// countdown step per 30 Hz tick.
constexpr float kJumpTurnaroundTurnRateAddend = 1.0F;
// DAT_00575538: shared zero sentinel for the death-timer / bomb-timer floors.
constexpr float kDeathTimerExpireFloor = 0.0F;
// Disabled auto-repair armor restore: max_armor * fraction + addend.
// g_auto_repair_armor_fraction (DAT_00575588, double 1/3) is the standard
// rate, g_auto_repair_armor_fraction_0x10 (DAT_00575578, double 0.1) the Ship
// Flags 0x10 variant, g_armor_state_addend (DAT_00575580, double 1.0) the
// shared addend. The +1 lifts the hull just above the disable threshold so
// the ship un-disables after the repair.
constexpr float kAutoRepairArmorFraction = 1.0F / 3.0F;
constexpr float kAutoRepairArmorFractionFlag0x10 = 0.1F;
constexpr float kAutoRepairArmorAddend = 1.0F;
// Bomb-detonation self-damage roll: max_armor * 0.5 + 1.0
// (g_bomb_damage_armor_fraction DAT_00575598 / g_armor_state_addend
// DAT_00575580), drawn via Random(roll) and added to max_armor.
constexpr float kBombDamageArmorFraction = 0.5F;
constexpr float kBombDamageArmorAddend = 1.0F;
// g_bomb_detonation_timer floor sentinel (DAT_00575538) and reroll ceiling
// (0x0044da75 rolls Random(100)).
constexpr float kBombDetonationTimerFloor = 0.0F;
constexpr std::int16_t kBombDetonationRerollMax = 100;
// g_bomb_detonation_interval_frames (0x00575590, float): the countdown window
// in 30 Hz reference frames before a carried bomb detonates.
constexpr float kBombDetonationIntervalFrames = 300.0F;
// Outfit ModType codes scanned in the four mod-type slots (decompile offsets
// name-0x26..-0x20, 0x37c-byte def stride).
constexpr std::int16_t kBombEscapePodModType = 0x2F; // self-destruct escape pod
constexpr std::int16_t kBombWeaponModType = 0x32;    // carried bomb
constexpr std::int16_t kAutoRepairOutfitModType = 0x31; // repair system
// STR# 0x7d2 entries used by the block.
constexpr std::uint16_t kStringListFlightText = 0x7D2;
constexpr std::uint16_t kStrAutoRepairEngaged =
    0x25; // "repair systems engaged"
constexpr std::uint16_t kStrEscapePodDeployed = 0x26;
constexpr std::uint16_t kStrBombYou = 0x27; // "You " prefix
constexpr std::uint16_t kStrBombDetonatedSingular = 0x28;
constexpr std::uint16_t kStrBombDetonatedPlural = 0x29;
// g_transition_sound_handle_table (snd 150+i): [4] auto-repair cue, [5]
// distress alert (_DAT_0059155c, snd 155 via NovaSound_LoadDecodedById).
constexpr std::int16_t kAutoRepairSoundTransitionIndex = 4;
constexpr std::int16_t kDistressCueSoundTransitionIndex = 5;
// HUD overlay durations (simulation ticks).
constexpr std::uint64_t kOverlayDurationAutoRepair = 250; // 0xfa
constexpr std::uint64_t kOverlayDurationBomb = 400;       // 0x190
constexpr std::uint64_t kOverlayDurationEscapePod = 360;  // 0x168
// Bomb outfit defs carry a 0x80-biased area-impact effect id in ModVal.
constexpr std::int16_t kBombEffectIdBias = 0x80;
// Frame_ShouldTriggerAutoRepairTick reroll: _DAT_00575830 = 500.0 (the range
// base) and _FLOAT_005757d8 = 0.0 (the low-tick-scale gate).
constexpr float kFrameScaleAutoRepairRerollGate = 0.0F;
constexpr int kAutoRepairFrameRerollRange = 500;

namespace {

// Frame_TickSystems preserves the original scope order. Unimplemented scopes
// intentionally do nothing; logging them per frame would overwhelm diagnostics.
void Stub_PlayerCore(GameState &state) { (void)state; }

// Ghidra scope 9 of Frame_TickSystems (0x004186b0). The original splits
// weapon contact into the sprite-overlap callback Ship_HandleSpritePair-
// Collision (0x004374f0), which runs during sprite-layer processing, and the
// blast-proximity pass Shot_ResolveCollisions (0x00437e20) in this scope.
// Stellar and asteroid contact branches remain deferred.
void Stub_Collisions(GameState &state) {
  NovaWeapon_ResolveDirectShotCollisions(state);
  NovaWeapon_ResolveProjectileCollisions(state);
}

// Ghidra scope 0xc of Frame_TickSystems: the per-tick status/scan pass. Its
// reconstructed member is the radar proximity-scan roll
// (Frame_RollProximityScanDetection 0x0045d030, g_proximity_scan_detected),
// which drives the radar panel's interference static.
void Stub_DrawStatus(GameState &state) {
  Frame_RollProximityScanDetection(state);
}

// Ghidra scope 6 parts 1 & 2 of Frame_TickSystems (0x004186b0). Part 1 ran the
// per-frame targeting setup (Ship_UpdateAutoWeaponSelectionFromTarget etc.);
// part 2 is the per-ship AI decision stage -- the top-level Ship_UpdateShipAI
// (0x00401000) listed in the plan. The clean-room equivalent is
// NovaAi_UpdateShipAI (src/game/ship_ai.cpp), which dispatches the behavior
// supervisors + the Ship_UpdateShipAiState state machine + the
// Ship_ApplyShipAiControls bridge for every active, non-player ship in the
// current system. The original's earlier scope-6 pass only refreshes target
// bookkeeping; it must not run this full state/control update a second time.
void Stub_AiRoutines(GameState &state, float elapsed_ticks) {
  const std::int16_t current_system = state.player.current_system_id;
  // now_ms backs the AI mode/formation timers and must be monotonic across
  // frames; the original reads its global millisecond tick source here.
  const std::uint32_t now_ms = SDL_GetTicks();
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.current_system_id != current_system) {
      continue;
    }
    if (NovaAiShip_IsDestroyed(ship)) {
      ship.ai_state_code = 0x16;
      ship.ai_control_mode = 0;
      ship.ai_forward_thrust_cmd = 0.0F;
      ship.ai_desired_speed = 0.0F;
      ship.vel_x = 0.0F;
      ship.vel_y = 0.0F;
      ship.engine_glow_level = 0;
      ship.engine_glow_intensity = 0.0F;
      continue;
    }
    // Frame_TickSystems skips AI entirely for behavior 0. This is important
    // for state-8 arrivals: they retain their seeded 50-unit inward velocity
    // until a behavior-bearing ship takes over the normal AI path.
    if (ship.ai_behavior_code <= 0) {
      continue;
    }
    // skip_heavy_ai=0: these spawned ships run the full (heavy) AI decision.
    NovaAi_UpdateShipAI(
        state, ship, /*skip_heavy_ai=*/false, now_ms, elapsed_ticks);
  }
}

// Ghidra scope 0xb of Frame_TickSystems (0x004186b0): the per-tick in-system
// NPC/reactivity pass. The original runs: Mission_TickShipInteractionReactions,
// Frame_UpdateCombatChatter, Frame_UpdateScreenFlashTimers,
// Ship_TallyInboundWeaponThreat, then -- the fleet/dude spawn maintenance this
// reimplementation is building toward -- System_TickNpcSpawnMaintenance
// (encounter fleets + random dude ships up to the system's avg_ships cap) and
// Asteroid_Spawn('\x01') (the asteroid ring), before clearing the
// g_ai_misc_event_flag / g_ai_target_refresh_needed latches.
//
// Reconstructed: the mission interaction-reaction pass (0x00443760) and the
// NPC-population slice (NovaSystem_TickNpcSpawnMaintenance, which spawns
// encounter-fleet leads / random dude ships toward avg_ships).
// Deferred: the combat-chatter/screen-flash/threat reaction helpers,
// the asteroid ring and the interaction flags (combat systems not yet
// reconstructed).
void Stub_TickReactionsAndNpcSpawns(GameState &state) {
  // The original's AI mode timers use a global millisecond tick source.
  const std::uint32_t now_ms = SDL_GetTicks();
  Mission_TickShipInteractionReactions(state, now_ms);
  NovaSystem_TickNpcSpawnMaintenance(
      state, state.player.current_system_id, now_ms);
}

void Stub_CalcAiOdds(GameState &state) { (void)state; }

// Ghidra scope 7 of Frame_TickSystems -> Shot_HandleShot (0x00435830). Shot
// movement/lifetime/cooldown bookkeeping runs here, after scope 9
// collision checks, matching the original phase order.
void Stub_HandleShots(GameState &state, float elapsed_ticks) {
  constexpr float kOriginalTickMs = 1000.0F / 30.0F;
  NovaWeapon_TickShots(state, elapsed_ticks * kOriginalTickMs, elapsed_ticks);
}

// Ship_HandleShip (0x00433050): ionization decay uses measured frame time in
// milliseconds, unlike movement's normalized tick unit.
void TickIonizationDecay(GameState &state, Ship &ship, float elapsed_ticks) {
  if (ship.ionization_points <= 0.0F) {
    ship.ionization_points = 0.0F;
    return;
  }
  const float frame_time_ms = elapsed_ticks * (1000.0F / 30.0F);
  const float decay_rate = NovaOutfit_ComputeIonizationDecayRate(state, ship);
  ship.ionization_points =
      std::max(0.0F, ship.ionization_points - decay_rate * frame_time_ms);
}

// Ghidra scope 4/5 of Frame_TickSystems: per-ship simulation. For the NPC
// ships this is Ship_HandleShip (0x00433050), which integrates each active
// ship's AI-written movement commands into its kinematics. Reconstructed for
// the NPC population: the movement/physics block (turning, thrust) via
// NovaShip_IntegrateNpcMovement for every active, non-player ship in the
// current system. The first projectile slice of the weapon/combat scope is
// also wired here; beams, turrets, carrier-bay, disable, and mission effects
// remain deferred.
void Stub_HandleShips(GameState &state, float elapsed_ticks) {
  const std::int16_t current_system = state.player.current_system_id;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.current_system_id != current_system) {
      continue;
    }
    // ---- Ghidra Ship_HandleShip (0x00433050) validation prologue. ----
    // The original deactivates any ship whose class id falls outside [0,0x2ff]
    // or whose class carries the -9999 (0xd8f1) "nonexistent class" sentinel,
    // then range-resets each slot field that has drifted out of its legal bound
    // back to -1 (logging a debug string per reset). ship_class_id is kept
    // zero-based in the clean-room struct, so the range checks mirror the
    // original exactly.
    const std::int16_t class_id = ship.ship_class_id;
    if (class_id < 0 || class_id > 0x2ff) {
      ship.is_active = false;
      continue;
    }
    const ShipClass *cls =
        state.scenario.Ship(static_cast<std::int16_t>(class_id + 0x80));
    if (cls == nullptr || cls->tech_level == kShipClassNonexistentTechLevel) {
      ship.is_active = false;
      continue;
    }
    if (ship.faction_or_government_id < -1 ||
        ship.faction_or_government_id > 0xff) {
      ship.faction_or_government_id = -1;
    }
    if (ship.dude_class_id < -1 || ship.dude_class_id > 0x1ff) {
      ship.dude_class_id = -1;
    }
    if (ship.pers_def_slot < -1 || ship.pers_def_slot > 0x3ff) {
      ship.pers_def_slot = -1;
    }
    if (ship.ai_target_ship_slot < -1 || ship.ai_target_ship_slot > 0x3f) {
      ship.ai_target_ship_slot = -1;
    }
    if (ship.target_stellar_object_id < -1 ||
        ship.target_stellar_object_id > 0x7ff) {
      // Ghidra quirk: this range check resets ai_target_ship_slot, not
      // target_stellar_object_id.
      ship.ai_target_ship_slot = -1;
    }
    if (ship.mission_fleet_slot < -1 || ship.mission_fleet_slot > 0xf) {
      ship.mission_fleet_slot = -1;
    }
    if (ship.primary_target_ship_slot < -1 ||
        ship.primary_target_ship_slot > 0x3f) {
      ship.primary_target_ship_slot = -1;
    }

    // Ship_IsShipDestroyed is an armor/death-timer predicate, not an
    // allocation guard. Once the validation prologue has accepted the slot,
    // a destroyed NPC is inert for the rest of this tick. Ship_HandleShip
    // decrements the death presentation timer here (one tick per frame,
    // g_cloak_fade_passive_decay = 1.0, paused while gameplay time is
    // frozen); the destruction finale runs in the visual-state pass below.
    if (NovaAiShip_IsDestroyed(ship)) {
      ship.ai_state_code = 0x16;
      ship.ai_control_mode = 0;
      ship.ai_forward_thrust_cmd = 0.0F;
      ship.ai_desired_speed = 0.0F;
      ship.vel_x = 0.0F;
      ship.vel_y = 0.0F;
      ship.engine_glow_level = 0;
      ship.engine_glow_intensity = 0.0F;
      if (ship.death_timer_active > 0.0F) {
        ship.death_timer_active -= elapsed_ticks;
      }
      continue;
    }

    NovaShip_IntegrateNpcMovement(
        state, ship, *cls, elapsed_ticks, SDL_GetTicks());

    NovaWeapon_TickNpcWeaponBanks(ship, elapsed_ticks);
    // Ship_HandleShip hands a latched active bank to Weapon_FireShipWeapons.
    NovaWeapon_FireNpcWeaponBank(state, ship);

    // The separate ionization speed clamp remains deferred.
    TickIonizationDecay(state, ship, elapsed_ticks);
  }

  // Ship_UpdateVisualState (0x00428340) destruction pass: runs after every
  // Ship_HandleShip integration (Frame_TickSystems scope order), finishing
  // the death presentation of destroyed hulls and applying the blast/mission
  // bookkeeping exactly once as each wreck expires.
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.current_system_id != current_system) {
      continue;
    }
    if (!NovaAiShip_IsDestroyed(ship)) {
      continue;
    }
    NovaShip_TickDestroyedShipVisualState(state, ship, elapsed_ticks);
  }
}

void Stub_MiscHandlers(GameState &state, bool run_full_tick) {
  (void)state;
  (void)run_full_tick;
}

void Stub_BeamHitQueue(GameState &state, float elapsed_ticks) {
  NovaWeapon_TickBeamHitQueue(state, elapsed_ticks);
}

// Ghidra 0x004186b0 Frame_TickSystems. Reconstructs only the *structure*:
// the scope ordering and the run_full_tick gate. Each scope is a loud stub
// (see above). Called full before drawing and reduced during transitions.
void NovaFrame_TickSystems(GameState &state,
                           bool run_full_tick,
                           float elapsed_ticks) {
  // g_player_disable_message_shown reset site (Ghidra 0x00417669 in the
  // spaceflight frame loop): the latch suppresses the duplicate destruction
  // overlay within one frame only.
  state.player_disable_message_shown = false;
  // scope 10 "player": always runs.
  Stub_PlayerCore(state);
  // scope 9 "collisions": always runs.
  Stub_Collisions(state);

  if (run_full_tick) {
    Stub_DrawStatus(state);                // scope 0xc
    Stub_TickReactionsAndNpcSpawns(state); // scope 0xb
    // The first original scope-6 pass only refreshes target
    // flags/reacquisition; the full clean-room AI update belongs here, once,
    // after spawning.
    Stub_AiRoutines(state, elapsed_ticks);
    Stub_CalcAiOdds(state); // scope 0x14
  }

  // Always-run scopes that keep advancing during frozen transitions.
  Stub_HandleShots(state, elapsed_ticks);  // scope 7
  Stub_HandleShips(state, elapsed_ticks);  // scope 4/5
  Stub_MiscHandlers(state, run_full_tick); // scope 8
  Stub_BeamHitQueue(state, elapsed_ticks);
}

// Ghidra 0x00417600 Frame_SpaceflightLoop main loop. Reconstructs the outer
// phase skeleton (setup + full first tick, then per-frame pre-draw/sim,
// drawing, post-draw) and the run_full_tick freeze gate. Simulation is still
// yielded to NovaFrame_TickSystems's stubs, but the rendering is live: stellar
// bodies, the parallax starfield and the player's rotating ship are drawn.
// Integrates the player's heading/throttle from the live keyboard into the
// PlayerShip. This is a lightweight stand-in for Ship_HandlePlayerShipCore's
// movement. Player steering, thrust, afterburner fuel burn, stellar gravity,
// and engine-glow control are reconstructed below; AI/combat scopes remain
// intentionally stubbed.
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
  // The radar resolves stellar blip sizes through the view's sprite store
  // (Sprite_GetShotHalfSpan on each loaded spin set).
  hud.AttachSpriteStore(&view.sprite_store());

  // Preload the complete gameplay sound handle table before the first frame.
  // The original does this during session setup; limiting the cache to owned
  // weapon fire sounds made impact, cloak, and other cues silent.
  NovaWeapon_PreloadGameplaySounds(state);

  // Keep the small legacy cache warm for callers that address weapon sounds
  // by their 0..35 fire-sound slot.
  NovaWeapon_PreloadOwnedFireSounds(state);
  // Preload the hyperspace jump sounds: snd 128 'Warp up' (the rising
  // 'hyperspace imminent' cue played as the ship accelerates into the jump
  // zoom; snd 129 'Warp up.x2' is the faster engine variant) and snd 130
  // 'Warp out' (the ~2.5 s boom at the fire/arrival instant). Mirrors
  // FUN_004b0740 preloading the jump handles
  // (NovaSound_LoadDecodedById(0x80/0x81/0x82) -> snd 128/129/130) so the
  // first jump plays them without a decode hitch. Missing resources -> the
  // jump plays silently (travel.cpp falls back to fixed durations).
  if (!state.warp_up_sound.has_value()) {
    if (const auto resource =
            NovaResource_LoadSndData(static_cast<std::uint16_t>(128))) {
      if (auto decoded = NovaSound_Decode(*resource)) {
        state.warp_up_sound = std::move(*decoded);
      }
    }
  }
  if (!state.warp_out_sound.has_value()) {
    if (const auto resource =
            NovaResource_LoadSndData(static_cast<std::uint16_t>(130))) {
      if (auto decoded = NovaSound_Decode(*resource)) {
        state.warp_out_sound = std::move(*decoded);
      }
    }
  }

  // ---- Pre-loop setup -----------------------------------------------------
  // Ghidra: rebuilds the stellar radar panel, evaluates availability, updates
  // system/stellar display state, then runs a full TickSystems with
  // g_gameplay_time_frozen and draws the first gameplay frame. The radar panel
  // and per-tick sprite display state are still not reconstructed; the stellar
  // availability re-evaluation (the part of System_UpdateSystemAnd-
  // StellarDisplayState that re-homes each stellar to its system and sets
  // is_available / hazard flags) runs inside NovaResources_EvaluateAvailabil-
  // ity. Radar-panel rebuild is a logged divergence.
  NovaResources_EvaluateAvailability(state);
  NovaLog::Todo("spaceflight pre-loop setup: stellar radar panel rebuild and "
                "per-tick sprite display state still not reconstructed");
  const bool ship_ready = view.EnsureShipSprite(platform, state);
  (void)ship_ready;
  // Restore the current system's asteroid / drift-debris population on entry
  // (Asteroid_InitSystem 0x004216B0): spawns the asteroid record quota and
  // pre-warms all 16 asteroid-pool slots with wander targets around the
  // player. The encounter-fleet population is Step 5.
  NovaAsteroid_InitSystem(state);
  // Ghidra: the ambient starfield is (re)spawned at every spaceflight entry
  // (NovaEffects_QueuedAmbientStarParticles from Ship_RunSpaceflightMode and
  // the travel/landing transitions). We spawn once when the mode starts, then
  // advance it per frame below.
  view.SpawnAmbientStars(platform, state);
  NovaFrame_TickSystems(state,
                        /*run_full_tick=*/true,
                        /*elapsed_ticks=*/1.0F);
  view.DrawGameFrame(platform, state, hud);
  platform.Present();

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
  bool destination_cycle_was_held = false;
  bool hyperspace_was_held = false;
  bool ship_cycle_was_held = false;
  bool nearest_was_held = false;
  // Secondary-weapon command edges (PlayerTick_WeaponCommands): the cycle is
  // edge-resolved against g_playerSecondaryCycleCommandLatch semantics (set
  // on execution, cleared on release); the clear-selection arm is already
  // single-shot but stays consistent with the same was-held pattern.
  bool secondary_cycle_was_held = false;
  bool clear_secondary_was_held = false;
  bool starmap_was_held = false;
  bool mission_info_was_held = false;
  bool land_was_held = false;
  bool target_action_was_held = false;
  bool board_was_held = false;
  std::int16_t prev_travel_stellar = state.travel.selected_stellar_id;
  // Gameplay time freezes while a blocking modal owns the loop (the original's
  // g_gameplay_time_frozen around interaction windows); every modal return
  // site calls resync_frame_clock() so the wall-clock gap is never integrated
  // as one giant flight frame (ship integration, ambient stars, shield
  // recharge and the reticle decay all scale by frame_time_ms).
  const auto resync_frame_clock = [&] { prev_tick_ms = SDL_GetTicks(); };
  while (!platform.quit_requested() && !returning_to_menu) {
    const std::uint64_t now_ms = SDL_GetTicks();
    const float frame_time_ms =
        std::max(1.0F, static_cast<float>(now_ms - prev_tick_ms));
    prev_tick_ms = now_ms;
    // Player control (heading/throttle) is read here once so the ship flies
    // while the simulation stubs do not, and the same snapshot feeds the
    // travel/jump channel. Movement integrates into PlayerShip.
    const FlightInput input = platform.PollFlightInput();
    // Escape/'q' are latched by PollFlightInput (it owns the SDL event drain
    // the old PollTextEvent-based check relied on); return to the menu.
    if (input.escape_pressed) {
      returning_to_menu = true;
      break;
    }
    // PlayerTick_StatusAndOutfitEvents (0x0044aa70 block 0x0044b240): death
    // bookkeeping, disabled damping, disabled auto-repair, the periodic
    // distress cue, and carried-bomb detonation. Runs ahead of the flight
    // input pass, matching the original's dispatch order. `true` means the
    // death/inactive branch consumed the frame (flight input is skipped).
    // Once the bookkeeping latches game_over_pending the original returns to
    // the pilot/menu flow via its restart command; TODO(decomp(0x0044abf8))
    // skipped: that command channel is not reconstructed, so the port leaves
    // flight mode instead.
    // True when PlayerTick_StatusAndOutfitEvents' death branch consumed the
    // frame (flight input is skipped, matching the original's early return).
    bool player_status_consumed = false;
    if (NovaPlayer_TickStatusAndOutfitEvents(
            state, frame_time_ms / (1000.0F / 30.0F))) {
      if (state.game_over_pending) {
        returning_to_menu = true;
        break;
      }
      player_status_consumed = true;
    }
    // DAT_007354a5: the escape-pod bomb variant latches an immediate return
    // to the menu shell after its deployment overlay.
    if (state.return_to_menu_pending) {
      state.return_to_menu_pending = false;
      returning_to_menu = true;
      break;
    }
    // Per-frame player-target validation (Ship_HandlePlayerShipCore 0x0044aa70
    // prologue): the ship target drops when the target ship is inactive,
    // destroyed, entering hyperspace (AI state 0x15) or cloaked past the
    // visibility gate -- which is how a targeted ship jumping out releases the
    // selection. The stellar selection has no per-frame validation.
    NovaTargeting_ValidatePlayerTarget(state);
    // Ghidra System_UpdateSystemAndStellarDisplayState (0x00432470) scope B:
    // per-tick recompute of the transient discovered_this_rebuild latch (the
    // starmap's one-jump-ahead window), so script reveals (mïsn X opcode) get
    // their grey neighbour ring without waiting for the next system entry.
    NovaSystem_RebuildDiscoveredLatch(state);
    const bool target_cycle =
        input.cycle_target_next || input.cycle_target_previous;
    if (target_cycle && !target_cycle_was_held) {
      NovaTargeting_CyclePlayerStellarTarget(state, input.cycle_target_next);
    }
    target_cycle_was_held = target_cycle;
    // Destination-SYSTEM cycling (Backslash / Shift+Backslash): rotate the
    // next-jump destination through the systems directly linked to the
    // current one. Mirrors the original's key-binding-13 block in
    // PlayerTick_TargetAndTravelCommands (g_playerCycleTravelTargetCommandLatch
    // at 0x0044b8b9..0x0044def6). Edge-latched so held-\ steps one
    // system per press. Setting a destination arms travel mode but does NOT
    // engage the jump -- that stays on the 'j' travel key (NovaTravel_Tick).
    const bool destination_cycle =
        input.cycle_destination_next || input.cycle_destination_previous;
    if (destination_cycle && !destination_cycle_was_held) {
      const std::int16_t dest = NovaTravel_CycleDestinationSystem(
          state, input.cycle_destination_next);
      if (dest >= 0) {
        // Destination cycled; travel mode is armed but the jump awaits 'j'.
        NovaLog::Info("backslash: destination system {}", dest);
      } else {
        NovaLog::Info("backslash: no travelable destination from this system");
      }
    }
    destination_cycle_was_held = destination_cycle;
    // Hyperspace-mode toggle (H): latch the off-map destination-selection
    // channel (the manual's "press H to set hyperspace mode, then Backslash to
    // pick the destination system"). The latch is cleared when a jump lands.
    if (input.hyperspace_mode && !hyperspace_was_held) {
      state.travel.hyperspace_mode = true;
      // Entering hyperspace mode latches plotted-jump mode 3 with no slot
      // (0x0044de28: mode=3, ai_secondary_target_slot=-1).
      state.player.travel_transfer_mode = 3;
    }
    hyperspace_was_held = input.hyperspace_mode;
    // Ship-target cycling: backquote (`) / Shift+backquote, with the
    // combat-relevant-only modifier (Alt or 'k'). Mirrors the original's
    // Ship_HandlePlayerShip cycle-target block (0x0044b120): a no-op result or
    // a self-result clears the target, otherwise the new slot is stored and
    // the reticle pulse is re-armed at 256.0 (0x43800000).
    const bool ship_cycle =
        input.cycle_ship_target_next || input.cycle_ship_target_previous;
    if (ship_cycle && !ship_cycle_was_held) {
      const std::int16_t next =
          input.cycle_ship_target_next
              ? NovaTargeting_FindNextPlayerCycleTarget(
                    state,
                    state.player.primary_target_ship_slot,
                    state.player.current_system_id,
                    input.cycle_ship_include_combat)
              : NovaTargeting_FindPreviousPlayerCycleTarget(
                    state,
                    state.player.primary_target_ship_slot,
                    state.player.current_system_id,
                    input.cycle_ship_include_combat);
      if (next == state.player.primary_target_ship_slot ||
          next == state.player.ship_instance_id) {
        state.player.primary_target_ship_slot = -1;
      } else {
        state.player.primary_target_ship_slot = next;
        state.ship_reticle_pulse = 256.0F;
      }
    }
    ship_cycle_was_held = ship_cycle;
    // "Target nearest" command: 'o' selects the nearest hostile combat target
    // (Ship_SelectNearestHostileCombatTarget 0x00462bd0), Alt+'o' the nearest
    // engaged target (0x00462850). A miss (-1) leaves the current target
    // untouched; a new slot re-arms the reticle pulse.
    const bool nearest_pressed =
        input.select_nearest_hostile || input.select_nearest_engaged;
    if (nearest_pressed && !nearest_was_held) {
      const std::int16_t slot =
          input.select_nearest_hostile
              ? NovaTargeting_SelectNearestHostileCombatTarget(state)
              : NovaTargeting_SelectNearestEngagedTarget(state);
      if (slot != -1 && slot != state.player.primary_target_ship_slot) {
        state.player.primary_target_ship_slot = slot;
        state.ship_reticle_pulse = 256.0F;
      }
    }
    nearest_was_held = nearest_pressed;
    // Click-to-target (PlayerTick_MouseTargetAndControlCommands 0x0044e019):
    // the pass captures the pre-click ship target, clears the target when the
    // click lands on the player's own sprite, picks a ship under the cursor,
    // and only scans stellar sprites when the ship target is unchanged from
    // the pre-click value (no ship hit, or the already-targeted ship was
    // clicked). A stellar hit selects it as the travel target and re-arms the
    // travel reticle pulse at 256.0 (0x43800000).
    if (input.primary_clicked) {
      const std::int16_t pre_click_target =
          state.player.primary_target_ship_slot;
      if (view.ClickInPlayerSprite(
              platform, state, input.mouse_x, input.mouse_y)) {
        state.player.primary_target_ship_slot = -1;
      }
      const std::int16_t picked =
          view.PickShipAt(platform, state, input.mouse_x, input.mouse_y);
      if (picked != -1) {
        state.player.primary_target_ship_slot = picked;
        state.ship_reticle_pulse = 256.0F;
      }
      if (state.player.primary_target_ship_slot == pre_click_target) {
        const std::int16_t stellar =
            view.PickStellarAt(platform, state, input.mouse_x, input.mouse_y);
        if (stellar >= 0x80 && stellar != state.travel.selected_stellar_id) {
          state.travel.selected_stellar_id = stellar;
          state.travel.selected_stellar_is_manual = true;
          state.travel_reticle_pulse = 256.0F;
        }
      }
    }
    const bool land_pressed = input.land && !land_was_held;
    land_was_held = input.land;
    const bool target_action_pressed =
        input.target_action && !target_action_was_held;
    target_action_was_held = input.target_action;
    const bool board_pressed = input.board && !board_was_held;
    board_was_held = input.board;
    // The original's movement values are per simulation tick. Its normal
    // cadence is 30 Hz; using a 60 Hz SDL render loop without this conversion
    // advances the player ship at twice the intended speed.
    constexpr float kOriginalTickMs = 1000.0F / 30.0F;
    // During an engaged hyperspace jump the jump state machine owns the ship
    // (heading/velocity/glow) for the brake, alignment hold and zoom thrust;
    // the player's normal movement integration is suspended so it does not
    // overwrite the jump's flight. NovaTravel_Tick below drives the phases.
    if (!player_status_consumed && !state.travel.engaging) {
      NovaPlayer_UpdateFromInput(state, input, frame_time_ms / kOriginalTickMs);
    }
    // PlayerTick_WeaponCommands (0x0044aa70 block 0x0044BEB0) +
    // PlayerTick_WeaponCycleContinuation (0x0044EAB4): primary fire loop,
    // selected-secondary fire, unfirable-bank auto-clear, the wrapped
    // secondary-bank cycle and the clear-selection arm. The original gates
    // the fire arms on the station-hold/maneuver timers and the
    // disabled state inside the dispatch; the port additionally
    // suspends the whole pass while the jump state machine owns the ship
    // (matching the disable restriction the original applies through the brake,
    // hold and zoom phases).
    if (!state.travel.engaging) {
      const bool cycle_secondary =
          input.cycle_secondary && !secondary_cycle_was_held;
      const bool clear_secondary =
          input.clear_secondary && !clear_secondary_was_held;
      NovaWeapon_TickPlayerWeaponCommands(
          state,
          {.fire_primary_held = input.fire,
           .fire_secondary_held = input.fire_secondary,
           .cycle_secondary = cycle_secondary,
           .cycle_secondary_backwards = input.cycle_secondary_backwards,
           .clear_secondary = clear_secondary},
          frame_time_ms / kOriginalTickMs);
      secondary_cycle_was_held = input.cycle_secondary;
      clear_secondary_was_held = input.clear_secondary;
    }
    // Cooldown-decay tail of PlayerTick_WeaponCommands: runs unconditionally
    // in the original player tick, including during engaged jumps (the
    // ammo>0 gate and the ionization pin live inside).
    NovaWeapon_TickPlayerWeaponBankCooldowns(state,
                                             frame_time_ms / kOriginalTickMs);
    // Play every fire sound latched this frame by the player or NPC firing
    // routines (a round actually spawned). The firing routines append
    // GameState.pending_fire_sounds; this loop owns the SdlAudio device, plays
    // each decoded sound with the original's distance attenuation
    // (NovaAudio_PlaySpatialByDistance, NPC fire sourced at the firing ship
    // against the player ship as listener; the player's own fire is
    // src == listener and plays at full volume), then clears the queue. Runs
    // every frame regardless of the fire input so NPC volleys are audible.
    for (const auto &pending : state.pending_fire_sounds) {
      if (pending.slot < 0 || pending.slot >= 36) {
        continue;
      }
      // Weapon fire slots 0..35 map onto the gameplay snd table (id 200+slot),
      // which is fully preloaded; fall back to the legacy owned-weapon cache.
      const std::size_t index = static_cast<std::size_t>(pending.slot);
      const auto *sound = state.gameplay_sounds[index].has_value()
                              ? &*state.gameplay_sounds[index]
                              : (state.weapon_fire_sounds[index].has_value()
                                     ? &*state.weapon_fire_sounds[index]
                                     : nullptr);
      if (sound == nullptr) {
        continue;
      }
      // Weapon.flags bit 0x10: suppress a retrigger while this fire sound is
      // still playing (NovaAudio_CountActiveByHandle gate in the original).
      if (pending.suppress_if_active &&
          audio.CountActiveByKey(pending.slot) > 0) {
        continue;
      }
      const float gain = NovaWeapon_ComputeSpatialFireGain(
          state.player.pos_x, state.player.pos_y, pending.src_x, pending.src_y);
      audio.Play(*sound, gain, 1.0F, pending.slot);
    }
    state.pending_fire_sounds.clear();
    // Impact sounds use the original 300..363 snd range, which is already
    // covered by the contiguous gameplay_sounds cache at offsets 100..163.
    // Collision/effect code only queues source coordinates; this loop owns the
    // SDL audio device and applies the same spatial attenuation as weapon fire.
    for (const auto &pending : state.pending_impact_sounds) {
      if (pending.slot < 0 || pending.slot >= 64) {
        continue;
      }
      const std::size_t cache_index =
          static_cast<std::size_t>(100 + pending.slot);
      if (!state.gameplay_sounds[cache_index].has_value()) {
        continue;
      }
      const float gain = NovaWeapon_ComputeSpatialFireGain(
          state.player.pos_x, state.player.pos_y, pending.src_x, pending.src_y);
      audio.Play(
          *state.gameplay_sounds[cache_index], gain, 1.0F, 300 + pending.slot);
    }
    state.pending_impact_sounds.clear();
    constexpr std::size_t kDestructionSoundIndex = 372 - 200;
    for (const auto &pending : state.pending_destruction_sounds) {
      if (!state.gameplay_sounds[kDestructionSoundIndex].has_value()) {
        continue;
      }
      const float gain = NovaWeapon_ComputeSpatialFireGain(
          state.player.pos_x, state.player.pos_y, pending.src_x, pending.src_y);
      audio.Play(
          *state.gameplay_sounds[kDestructionSoundIndex], gain, 1.0F, 372);
    }
    state.pending_destruction_sounds.clear();
    // Centered UI cues from the boarding system (transition-table handles,
    // snd 150 + index). The flight loop owns the audio device; the modal
    // windows play their cues through the same queue. Mirrors
    // NovaAudio_QueueCenteredSound(handle, count, ...). The lazy decode runs
    // here so every gameplay-side queuer (jump-range cue, auto-repair,
    // distress alert) sees a loaded table even without a boarding pass.
    EnsureTransitionSounds(state);
    for (const auto &pending : state.pending_ui_sounds) {
      if (pending.transition_index < 0 ||
          pending.transition_index >=
              static_cast<std::int16_t>(state.transition_sounds.size())) {
        continue;
      }
      const auto &sound = state.transition_sounds[static_cast<std::size_t>(
          pending.transition_index)];
      if (!sound.has_value()) {
        continue;
      }
      for (std::int16_t repeat = 0; repeat < pending.count; ++repeat) {
        audio.Play(*sound, 1.0F, 1.0F, 150 + pending.transition_index);
      }
    }
    state.pending_ui_sounds.clear();
    // Cross-system hyperspace jump state machine (travel.cpp): engages on
    // the 'j' key with a plotted destination, then drives the brake and the
    // warp-up hold before the fire. The 'Warp up' voice count gates the fire
    // like the original's NovaAudio_CountActiveByHandle latch.
    NovaTravel_Tick(state,
                    input.travel,
                    frame_time_ms,
                    audio.CountActiveByKey(game::kHyperspaceWarpUpSoundKey) >
                        0);
    // Play the hyperspace jump sounds latched by the travel state machine
    // (the 'Warp up' cue as the zoom thrust begins and the 'Warp out' boom at
    // the fire/arrival, synced with the screen flash). The loop owns the
    // SdlAudio device, so travel only latches flags. Mirrors the original's
    // Stellar_TriggerHyperspaceAudioOnce one-shot (g_playerHyperspaceAudio-
    // Latch gating NovaAudio_QueueCenteredSound)
    // [Ghidra 0x00431420].
    if (state.warp_up_sound_pending) {
      if (state.warp_up_sound.has_value()) {
        // The fire gate in NovaTravel_Tick waits for this voice to finish, and
        // the tunnel ramp schedule is cue-relative, so this rate sets the
        // whole jump cadence. The original stages the cue at rate
        // 1.0/jump_duration_multiplier (0x0046ab00: 65536/multiplier
        // fixed-point; chassis-derived 0.91..2.08 -> rates 0.48..1.1). The
        // port pins the multiplier to 1.0 (not decoded into ShipClass yet),
        // so the cue plays at rate 1.0: 6.08 s of rising cue, ~2.1 s of
        // stationary hold, ramp onset, boom as the cue resolves.
        audio.Play(
            *state.warp_up_sound, 1.0F, 1.0F, game::kHyperspaceWarpUpSoundKey);
      }
      state.warp_up_sound_pending = false;
    }
    if (state.warp_up_cancel_pending) {
      audio.StopByKey(game::kHyperspaceWarpUpSoundKey);
      state.warp_up_cancel_pending = false;
    }
    if (state.warp_out_sound_pending) {
      if (state.warp_out_sound.has_value()) {
        audio.Play(*state.warp_out_sound);
      }
      state.warp_out_sound_pending = false;
    }

    // Galaxy-map command ('m', edge-triggered): open the starmap modal over
    // the current flight scene. Mirrors Ship_HandlePlayerShip (0x0044b120)
    // dispatching NovaUi_RunStarmapWindow when its map gameplay command is
    // active. The modal owns the frame until the player closes it; the jump
    // state machine is untouched by inspection (the map only selects systems).
    // Gated while a jump is engaged (the original fire-restricts the player
    // through the brake, hold and zoom).
    const bool starmap_held = input.starmap;
    if (starmap_held && !starmap_was_held && !state.travel.engaging) {
      const StarmapResult map_result =
          NovaStarmap_RunWindow(platform, state, -1, &view, &hud);
      if (map_result.exit == StarmapExit::kQuit) {
        returning_to_menu = true;
        break;
      }
      // Plot the map-selected system as the next jump destination so the HUD
      // shows the plotted jump and 'j' engages it. The map itself applies
      // plain-click plots and route re-arms internally (matching the
      // original's in-session dispatch), so an empty result leaves the armed
      // state untouched.
      if (map_result.destination_system_id >= 0) {
        NovaTravel_PlotStarmapDestination(state,
                                          map_result.destination_system_id);
      }
      // Refresh the travel reticle after the map may have re-selected the
      // travel stellar (the original re-arms the travel pulse on map return
      // via NovaUi_MarkTravelAndStatusPanelsDirty).
      state.travel_reticle_pulse = 256.0F;
      // The map blocked the loop; freeze gameplay time across it.
      resync_frame_clock();
    }
    starmap_was_held = starmap_held;
    // Active-missions command ('i', edge-triggered; gameplay command 0x28):
    // Ship_HandlePlayerShipCore 0x0044aa70 counts the non-invisible active
    // missions and either plays the denied cue + "You have no active
    // missions." overlay (STR# 0x7d2 0x162, 0xf0 ticks) or opens the
    // mission-computer window (NovaUi_RunMissionComputerWindow 0x00446150).
    // Gated while a jump is engaged like the other interaction windows.
    const bool mission_info_held = input.mission_info;
    if (mission_info_held && !mission_info_was_held && !state.travel.engaging) {
      std::size_t visible_missions = 0;
      for (std::size_t slot = 0;
           slot < state.active_mission_runtime_flags.size();
           ++slot) {
        const auto &flags = state.active_mission_runtime_flags[slot];
        if (flags.is_active && (flags.flags_primary_at_accept & 0x400) == 0U) {
          ++visible_missions;
        }
      }
      if (visible_missions == 0) {
        state.pending_ui_sounds.push_back({3, 1});
        NovaHud_ShowOverlayMessage(
            state,
            NovaHud_LoadStringEntry(0x7d2, 0x162)
                .value_or("You have no active missions."),
            static_cast<std::uint64_t>(0xf0));
      } else {
        NovaMission_RunMissionInfoWindow(platform, audio, state, view, hud);
        // The mission-computer window blocked the loop; freeze gameplay time
        // across it.
        resync_frame_clock();
      }
    }
    mission_info_was_held = mission_info_held;
    // When a jump completed this frame, re-spawn the starfield for the new
    // system (the original's jump completion re-runs
    // NovaEffects_QueuedAmbientStarParticles).
    if (state.travel.just_completed) {
      // Cross-system travel re-initializes the asteroids for the new system,
      // matching the original's jump-completion re-run of Asteroid_InitSystem.
      NovaAsteroid_InitSystem(state);
      view.SpawnAmbientStars(platform, state);
      // Ship_DeactivateVacantShipsAndTally ('\0') runs at system-entry
      // (NovaMainLoop_Run 0x00486880's 0x90 latch and Stellar_ProcessTravel-
      // AndLanding 0x00457580): the ships left behind by the departure system
      // are vacant (idle wanderers/parked; only non-disabled ships
      // engaging the player survive), so the cohort is swept before the new
      // system gets its immediate scattered avg_ships population from the tail
      // of System_RebuildInitialNpcAndMissionPopulation. Per-tick maintenance
      // only replenishes later losses through the visible arrival paths.
      // Without this sweep the old system's ships would linger in their
      // previous current_system_id and reappear (still active) whenever the
      // player jumps back.
      NovaShip_DeactivateVacantShipsAndTally(state,
                                             /*keep_player_engaged=*/false);
      NovaSystem_RestoreMissionFleets(state,
                                      state.player.current_system_id,
                                      /*copy_player_heading=*/false,
                                      SDL_GetTicks());
      // Offering rolls redraw on every system arrival (Stellar_ProcessTravel
      // AndLanding 0x00458802: roll 1..100 per definition, then re-evaluate
      // the mission lists).
      Mission_RerollOfferingRolls(state);
      NovaSystem_PopulateInitialNpcShips(state, state.player.current_system_id);
      // The player's primary target ship lived in the departure system; the
      // vacancy sweep deactivated it (and its slot may be reused by a fresh
      // spawn), so clear the selection and the reticle pulse -- the original
      // resets primary_target_ship_slot on system entry.
      state.player.primary_target_ship_slot = -1;
      state.ship_reticle_pulse = 0.0F;
      // Re-arm the landed system's stellar availability immediately (mirrors
      // system-entry re-deriving display state) and clear any manual travel
      // selection left over from the departure system, so the automatic target
      // below starts from the new system's own stellars rather than a stale
      // one. Without this the travel/land reticle can point at the previous
      // system's target for a frame.
      NovaTargeting_UpdateStellarAvailability(state);
      state.travel.selected_stellar_id = -1;
      state.travel.selected_stellar_is_manual = false;
    }
    // No per-frame stellar auto-seed: the original only sets
    // ai_secondary_target_slot from explicit commands (the land command's
    // nearest-pick below, number keys, click, nearest, starmap route). The
    // selection therefore stays empty until the player targets something, and
    // a ship target and a stellar selection can coexist as in the original.
    // A change to the selected travel stellar re-arms the travel reticle pulse
    // (the original arms _g_travel_target_reticle_pulse whenever
    // ai_secondary_target_slot is assigned a fresh stellar).
    if (state.travel.selected_stellar_id != prev_travel_stellar) {
      state.travel_reticle_pulse = 256.0F;
      prev_travel_stellar = state.travel.selected_stellar_id;
    }
    // Normal arrival (Return) is independent of target action: the original
    // player-ship tick directly invokes Stellar_ProcessTravelAndLanding here,
    // opening the Spaceport only when the selected ordinary stellar is inside
    // its arrival envelope. The rejection feedback is shown as an on-screen HUD
    // overlay (STR# 0x7d2 messages) instead of a bare log line. Gated while a
    // jump is engaged (disabled through brake + hold + zoom).
    if (land_pressed && !state.travel.engaging) {
      // The original's land command (binding 5 in Ship_HandlePlayerShipCore
      // 0x0044aa70) auto-picks the nearest available travel stellar when no
      // stellar is currently targeted (travel_transfer_mode != 2 or
      // ai_secondary_target_slot == -1) before running the arrival checks.
      if (state.travel.selected_stellar_id < 0) {
        const std::int16_t nearest =
            NovaTargeting_FindNearestAvailableTravelStellar(state);
        if (nearest >= 0x80) {
          state.travel.selected_stellar_id = nearest;
          state.travel_reticle_pulse = 256.0F;
        }
      }
      LandedContext ctx;
      if (NovaLanding_EnterDocked(state, ctx)) {
        NovaLog::Info("arrival accepted at stellar {}; opening Spaceport",
                      ctx.stellar_id);
        // Mission resolution (Mission_TickReactionSlotsForTravelInteraction
        // 0x00443780, including the success/failure debrief readers) runs
        // once on Spaceport entry inside NovaLanded_RunWindow, matching its
        // position in NovaUi_RunTravelDestinationInteractionLoop
        // (0x00491f30): after the window is up, before the AvailLoc-3 offer
        // pass, with debriefs layered over the dock.
        const LandedExit exit = NovaLanded_RunWindow(platform, state, ctx);
        if (exit == LandedExit::kQuit) {
          returning_to_menu = true;
          break;
        }
        // Launch itself advances nothing: the landing already ticked the
        // daily world update once (Stellar_TravelToSystem 0x00455e10), and
        // the original launch block does not touch the calendar.
        // TODO(decomp(0x0044d870)) skipped: the 15..44-day daily-driver loop
        // belongs to the death/escape-pod respawn (PlayerTick_TimedAction-
        // Transition 0x0044d490), not to launch -- previously misattributed
        // here and charging 16..45 days per landing.
        if (exit == LandedExit::kLaunched) {
          // Stellar_TravelToSystem tail (0x00456323): the "leaving
          // <stellar> on <date>" overlay shows as the player departs.
          NovaHud_ShowLaunchDepartureMessage(state, ctx.stellar_id);
        }
        // Docking blocked the loop for the whole landing; freeze gameplay
        // time across it (launch re-enters flight with a fresh clock).
        resync_frame_clock();
      } else {
        const auto *st =
            state.scenario.Stellar(state.travel.selected_stellar_id);
        const bool is_station = st != nullptr && (st->flags & 0x10U) != 0U;
        NovaHud_ShowLandingDenial(state, ctx.denial, is_station);
      }
    }
    // Target action remains the distinct DLOG 0x3f1 bribe/hostility/script
    // interaction pathway. It is intentionally not substituted for landing.
    // Mirrors Ship_HandlePlayerTargetActionCommand (0x00454910): with a ship
    // primary target the action opens the ship-comm dialog (DLOG 0x3ef); with
    // no target (or the 0x38/0x6f commands held) it opens the destination-
    // interaction window for the selected travel stellar. The player disabled/
    // destroyed gate and the target-ship "entering hyperspace" latch only
    // beep + show an overlay in the original (the clean-room shows the overlay
    // text; the beep is not modelled). Mission-ship defs are not modelled, so
    // the original's NovaUi_RunMissionShipInteractionWindow branch never
    // triggers here (TODO(decomp)). Gated while a jump is engaged
    // (disabled through brake + hold + zoom).
    if (target_action_pressed && !state.travel.engaging) {
      const std::int16_t ship_target = state.player.primary_target_ship_slot;
      if (ship_target > 0 &&
          state.SlotInRange(static_cast<std::size_t>(ship_target))) {
        if (NovaAiShip_IsDestroyed(state.player) ||
            state.player.ai_station_hold_timer > 0.0F) {
          NovaLog::Info("target-action: player disabled/destroyed; hail "
                        "ignored");
        } else {
          const Ship &target =
              state.ShipAt(static_cast<std::size_t>(ship_target));
          if (target.ai_station_hold_timer > 0.0F) {
            // Ship is launching/entering hyperspace: cannot hail.
            const bool restricted = NovaAiShip_IsDisabled(state, target);
            // Entry numbers exactly as the original passes them (1-based):
            // 0x35 = "No response." (disabled target), 0x36 = "Unable
            // to send hail - target ship is entering hyperspace."
            const auto text =
                NovaHud_LoadStringEntry(0x7d2, restricted ? 0x35 : 0x36);
            NovaHud_ShowOverlayMessage(state,
                                       text.value_or("Unable to send hail."));
          } else if (!NovaShipComm_TargetEligibleForHail(state, target)) {
            const auto text =
                NovaHud_LoadStringEntry(0x7d2, 0x35); // "No response."
            NovaHud_ShowOverlayMessage(state,
                                       text.value_or("Unable to send hail."));
          } else {
            (void)NovaShipComm_RunShipDialog(
                platform, state, ship_target, view, hud);
            // The comm dialog blocked the loop; freeze gameplay time.
            resync_frame_clock();
          }
        }
      } else if (NovaTargeting_CanOpenTravelDestinationInteraction(state)) {
        const std::int16_t dialog_stellar = state.travel.selected_stellar_id;
        const NegotiationExit exit = NovaNegotiation_RunDestinationDialog(
            platform, state, dialog_stellar, view, hud);
        if (exit == NegotiationExit::kQuit) {
          returning_to_menu = true;
          break;
        }
        // The destination-interaction window blocked the loop; freeze
        // gameplay time across it.
        resync_frame_clock();
        if (exit == NegotiationExit::kProceedToLand) {
          // The player paid an accepted bribe in the interaction window (the
          // only path out of that window that docks; landing itself stays on
          // the normal second-E request flow). The original's bribe handoff
          // sets g_travel_selected_stellar_id + g_travel_engage_timer (the
          // travel-to-system warp) rather than requiring the 250-unit arrival
          // envelope, so co-locate the ship at the destination before the
          // normal dock gate.
          const auto *st =
              state.scenario.Stellar(state.travel.selected_stellar_id);
          if (st != nullptr) {
            state.player.pos_x = static_cast<float>(st->pos_x);
            state.player.pos_y = static_cast<float>(st->pos_y);
          }
          LandedContext ctx;
          if (NovaLanding_EnterDocked(state, ctx)) {
            NovaLog::Info("destination-interaction dialog granted landing at "
                          "stellar {}; opening Spaceport",
                          ctx.stellar_id);
            const LandedExit landed =
                NovaLanded_RunWindow(platform, state, ctx);
            if (landed == LandedExit::kQuit) {
              returning_to_menu = true;
              break;
            }
            // Launch advances nothing (see the dock-exit note above); the
            // landing tick already covered the calendar.
            // TODO(decomp(0x0044d870)) skipped: 15..44-day loop belongs to
            // the death respawn (0x0044d490), not launch.
            if (landed == LandedExit::kLaunched) {
              NovaHud_ShowLaunchDepartureMessage(state, ctx.stellar_id);
            }
            resync_frame_clock();
          } else {
            NovaLog::Warn("destination-interaction dialog staged a landing at "
                          "stellar {} but arrival was refused ({})",
                          ctx.stellar_id,
                          static_cast<int>(ctx.denial));
          }
        }
      } else {
        NovaLog::Info("target-action: selected stellar cannot open its "
                      "destination interaction");
      }
    }
    // Board command ('b', edge-triggered): Ship_HandlePlayerBoardTargetCommand
    // (0x0045a3d0). Like the original's command-latch read in
    // Ship_HandlePlayerShipCore this runs every frame regardless of jump state;
    // its own gates reject un-boardable targets. The dispatch may open the
    // boarding/plunder modal (blocking on the flight loop); the modal renders
    // the live game view beneath itself via SpaceflightView::DrawGameFrame.
    if (board_pressed) {
      NovaBoarding_HandleBoardTargetCommand(platform, audio, state, view, hud);
      if (returning_to_menu) {
        break;
      }
      // The boarding/plunder modal blocked the loop; freeze gameplay time.
      resync_frame_clock();
    }
    // In-flight shield regeneration (class base + opcode-5 outfit bonuses),
    // scaled by the real frame time. The original's player-update path ticks
    // shields each frame; armor does not regenerate in flight.
    NovaPlayer_TickShieldRecharge(state, frame_time_ms);
    // Expire any transient HUD overlay message once its wall-clock deadline
    // passes (the draw path is const over state).
    NovaHud_TickOverlay(state);
    const float delta_x = state.player.pos_x - prev_x;
    const float delta_y = state.player.pos_y - prev_y;
    prev_x = state.player.pos_x;
    prev_y = state.player.pos_y;
    // Unified per-frame world animation pass: advances the ambient starfield by
    // the ship's movement delta (NovaEffects_UpdateAmbientStarParticles) and
    // steps each animated stellar one animation segment (Stellar_UpdateStellar-
    // Sprites), both on the single frame_time_ms cadence (see the header).
    view.AdvanceAnimations(platform, state, frame_time_ms, delta_x, delta_y);
    // Re-derive visibility + stellar availability each tick
    // (NovaResources_EvaluateAvailability 0x00448090 runs per frame in the
    // original's flight loop, refreshing is_visible from the Visibility NCBs
    // and re-homing the stellars — its tail is the scope-3
    // System_UpdateSystemAndStellarDisplayState availability pass). This
    // keeps story-flag twin swaps (system government changes) in step while
    // in flight.
    NovaResources_EvaluateAvailability(state);

    // Ghidra scope 1 "pre-draw tasks": full TickSystems + ambient particles +
    // cursor update.
    NovaFrame_TickSystems(
        state, /*run_full_tick=*/true, frame_time_ms / kOriginalTickMs);

    // Ghidra scope 2 "drawing": sprite world present + viewport particles +
    // commit frame.
    view.DrawGameFrame(platform, state, hud);
    platform.Present();

    // Ghidra scope 3 "post-draw tasks": pump the primary mouse command; when
    // latched bit sets DAT_00596d38 (return to menu) and, while the frame is
    // frozen, runs a *reduced* TickSystems(0). Escape/'q' are captured by
    // PollFlightInput above (it owns the SDL event drain) and set the
    // return-to-menu latch; the frozen reduced tick is skipped (no transition
    // is active in this build).
    // Reticle pulse decay (NovaUi_UpdateShipTargetReticle 0x0042ede0): the
    // pulse falls from 256.0 toward 0.0 each frame by
    // g_avg_frame_time_ms * 60.0 (DAT_005753c8 = 60.0). The avg frame time
    // is an EMA whose steady state is delta_ms * 0.03 (0x00432f76..8a:
    // avg = (avg*3 + delta*0.03) * 0.25), so the faithful rate is
    // delta_ms * 0.03 * 60 = delta_ms * 1.8: at 60fps that is ~30px/frame,
    // settling the 256px grow-out in ~0.15s. (Earlier builds used 0.06/ms,
    // a 30x-too-slow ~4.3s settle.)
    state.ship_reticle_pulse =
        std::max(0.0F, state.ship_reticle_pulse - frame_time_ms * 1.8F);
    state.travel_reticle_pulse =
        std::max(0.0F, state.travel_reticle_pulse - frame_time_ms * 1.8F);
    // Decay the hyperspace fire flash: full white at the boom instant, gone
    // in ~60 ms (a single bright frame, matching the original's one-frame
    // centered effect 0x32).
    state.screen_flash_intensity =
        std::max(0.0F, state.screen_flash_intensity - frame_time_ms / 60.0F);
    SDL_Delay(16);
  }
}

} // namespace

// External test seam into the per-ship simulation pass (Ghidra scope 4/5 of
// Frame_TickSystems 0x004186b0 -> Ship_HandleShip 0x00433050). Stub_HandleShips
// lives in the anonymous namespace above; this wrapper exposes it to unit tests
// without external linkage polluting the otherwise-internal stub set.
void NovaShip_TickNpcShips(GameState &state, float elapsed_ticks) {
  Stub_HandleShips(state, elapsed_ticks);
}

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
//        accel    -> thrust (px/tick^2):   raw_accel / 10000.0 (DAT_00575e68)
//                                                 then *2.0 runtime
//                                                 (DAT_005757a8)
//        speed    -> top speed (px/tick):  raw_speed / 100.0 (DAT_00575e48)
//        maneuver -> turn rate (deg/tick): raw_maneuver * 0.1 (DAT_00575e58)
//    Starter (sh.x9an 0x80): accel 0.05, turn 4.0 deg/tick, top speed 4
//    px/tick. The original scales these values by its frame-time-derived tick
//    count.
//
//  * Turn follows the held key at round(effective turn rate) integer degrees
//    per tick. Outfit opcode-9 is folded into the effective stats before this
//    integrator; the original's ionization damping awaits reconstruction of
//    that combat state. Heading 0 points 'up', increases clockwise.
//
//  * Thrust accelerates along the heading as a polar velocity step clamped
//    per-AXIS to the projection of the class top speed (Math_AddPolarVelocity-
//    WithClamp 0x0043b4e0). The clamp only caps each axis's additional thrust
//    at its polar max projection; it is NOT a vector-magnitude governor, so a
//    ship turning at full thrust can build a small off-axis component that
//    pushes its net speed modestly past the nominal top speed (the authentic
//    EVN drift). Separately, the final velocity vector is then hard-capped
//    each frame to +/-the effective max-speed component (DAT_005997bc/c0 in
//    Ship_HandlePlayerShipControl 0x0044e019), pulling back any excess built
//    up off-axis, so the cap bounds the net speed. The original's throttle is
//    high enough to top out within a few frames.
//
//  * Inertia: most ships preserve momentum when throttle is released -- there
//    is NO continuous velocity drag in the original's free-flight path, so a
//    released ship keeps most of its momentum. The old stand-in's per-frame
//    kDrag multiply was the main fidelity bug. Inertia-less ships are the
//    special case where base_accel == 0 && base_speed == 0: Ship_HandleShip
//    (0x00433050) zeroes their velocity every frame, pinning them in place.
//
//  * REVERSE ('s'/down) turns the ship toward the heading opposite its current
//    velocity, then continues to coast; it does not apply retro-thrust. This is
//    Ship_HandlePlayerShipControl's early Ship_TurnShipTowardHeading path.
//
// NOTE(decomp) scale/cadence: the original integrates over g_avg_frame_time_ms
// (0x00735448, Frame_MeasureFrameTiming 0x00432ea0), so ship motion is
// cadence-independent. The reimplementation normalizes measured SDL elapsed
// time to the original's 30 Hz simulation-tick basis.
// Pure tick-scaled movement integration (unit-tested in
// tests/movement_test.cpp).

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
// Declared in spaceflight.hpp so both the player and NPC integrators share one
// faithful port.
void NovaPlayer_AddPolarVelocityClamped(float heading_rad,
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

// Ghidra 0x0043adb0 Stellar_TickStellarGravityPull (player-side port; NPC
// iteration/crash consequences not reconstructed). The protected_from_gravity
// check below ports Stellar_ShipHasGravityShielding (0x0046e120) for the
// player via owned outfit effects.
// Ghidra 0x0046e2f0 Ship_AccelerateShipTowardPoint. Stellar_TickStellar-
// GravityPull passes gravity * frame_time as max_accel, divides by the squared
// separation (with a tiny-distance floor), then adds the polar result to the
// ship velocity. The player is exempt with either opcode 38 (inertial
// dampener, used by Outfit_ShipHasGravityShieldOutfit) or opcode 41 (gravity
// resistance, added by Stellar_ShipHasGravityShielding).
static bool NovaPlayer_ApplyStellarGravity(GameState &state,
                                           float elapsed_ticks) {
  const System *const system = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (system == nullptr || elapsed_ticks <= 0.0F) {
    return false;
  }

  bool gravity_present = false;
  const bool protected_from_gravity =
      Outfit_HasOwnedEffect(state, OutfitEffect::kInertialDampener) ||
      Outfit_HasOwnedEffect(state, OutfitEffect::kGravityResist);
  for (const std::int16_t stellar_id : system->nav_defs) {
    const Stellar *const stellar = state.scenario.Stellar(stellar_id);
    if (stellar == nullptr || stellar->gravity == 0) {
      continue;
    }
    gravity_present = true;
    if (protected_from_gravity) {
      continue;
    }
    const float dx = static_cast<float>(stellar->pos_x) - state.player.pos_x;
    const float dy = static_cast<float>(stellar->pos_y) - state.player.pos_y;
    const float distance_sq = std::max(dx * dx + dy * dy, 1.0F);
    const float distance = std::sqrt(distance_sq);
    const float accel =
        static_cast<float>(stellar->gravity) * elapsed_ticks / distance_sq;
    state.player.vel_x += dx / distance * accel;
    state.player.vel_y += dy / distance * accel;
  }
  return gravity_present;
}

[[nodiscard]] PlayerMovementStats
NovaPlayer_IntegrateMovement(PlayerShip &ship,
                             const FlightInput &input,
                             const ShipClass &ship_class,
                             float elapsed_ticks) {
  constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
  constexpr float kTwoPi = 6.283185307179586F;

  PlayerMovementStats stats;
  elapsed_ticks = std::max(0.0F, elapsed_ticks);
  // Ship_HandlePlayerShipControl rounds Ship_ComputeShipMaxTurnRateDeg before
  // multiplying by frame time. Our caller supplies the effective raw
  // maneuver (base + opcode-9 bonuses), so preserve that integer gate here.
  stats.turn_rate_deg_per_tick =
      std::round(static_cast<float>(ship_class.turn_rate) * 0.1F);
  // Loader-verified scales (NovaData_LoadScenarioResourceTables 0x004bd3c0):
  //   accel (offset 0x04) -> base_accel via /DAT_00575e68=10000.0;
  //   speed (offset 0x06) -> base_speed  via /DAT_00575e48=100.0   (NOT 640);
  //   turn  (offset 0x08) -> base_turn_rate_deg via *DAT_00575e58=0.1.
  stats.max_speed_px_per_tick = static_cast<float>(ship_class.speed) / 100.0F;
  // Ship_ComputeShipEffectiveThrust (0x004640a0) multiplies the loaded accel
  // by DAT_005757a8 = 2.0 before the player applies it as the thrust step, so
  // the effective thrust is 2*accel/10000 (the x2 is applied here).
  stats.thrust_px_per_tick2 =
      static_cast<float>(ship_class.accel) / 10000.0F * 2.0F;
  const float turn_rad =
      stats.turn_rate_deg_per_tick * kDegToRad * elapsed_ticks;

  ship.engine_thrust = input.thrust && !input.reverse;

  // Reverse uses the original's automatic turn-toward-velocity path instead
  // of also applying manual steering in the same tick.
  if (!input.reverse) {
    if (input.turn_left) {
      ship.heading -= turn_rad;
    }
    if (input.turn_right) {
      ship.heading += turn_rad;
    }
    ship.heading = std::fmod(ship.heading + kTwoPi, kTwoPi);
    if (ship.heading < 0.0F) {
      ship.heading += kTwoPi;
    }
  }

  if (input.reverse) {
    // Ghidra 0x0044e019: the reverse command finds the current velocity's
    // bearing, adds 180 degrees, and calls Ship_TurnShipTowardHeading. It
    // turns the hull around while preserving its velocity; it is not braking.
    const float speed = std::hypot(ship.vel_x, ship.vel_y);
    if (speed > 1e-4F) {
      const float reverse_heading =
          std::atan2(ship.vel_x, -ship.vel_y) + 3.14159265358979323846F;
      const float desired = std::fmod(reverse_heading + kTwoPi, kTwoPi);
      float delta = std::remainder(desired - ship.heading, kTwoPi);
      delta = std::clamp(delta, -turn_rad, turn_rad);
      ship.heading = std::fmod(ship.heading + delta + kTwoPi, kTwoPi);
    }
  } else if (input.thrust) {
    // Forward thrust: polar step toward the heading, per-axis clamped to the
    // class top speed projection (Math_AddPolarVelocityWithClamp semantics).
    NovaPlayer_AddPolarVelocityClamped(ship.heading,
                                       stats.thrust_px_per_tick2 *
                                           elapsed_ticks,
                                       stats.max_speed_px_per_tick,
                                       ship.vel_x,
                                       ship.vel_y);
  }

  // Inertia-less ships (base_accel == 0 && base_speed == 0) are stationary: the
  // original Ship_HandleShip (0x00433050) zeroes their velocity every frame, so
  // they act as pinned/immovable objects rather than coasting forever.
  if (ship_class.accel == 0.0F && ship_class.speed == 0.0F) {
    ship.vel_x = 0.0F;
    ship.vel_y = 0.0F;
    ship.speed = 0.0F;
    return stats;
  }

  // Max-speed hard cap. Beyond the per-axis clamp applied *during* thrust
  // (NovaPlayer_AddPolarVelocityClamped), the original player path
  // (Ship_HandlePlayerShipControl 0x0044e019) clamps the resulting velocity
  // vector to +/-the effective max speed component every frame (DAT_005997bc /
  // DAT_005997c0) before integrating position. This pulls back any excess
  // component (drift-built, recoil/knockback, gravity) that exceeds max speed.
  ship.vel_x = std::clamp(
      ship.vel_x, -stats.max_speed_px_per_tick, stats.max_speed_px_per_tick);
  ship.vel_y = std::clamp(
      ship.vel_y, -stats.max_speed_px_per_tick, stats.max_speed_px_per_tick);

  ship.pos_x += ship.vel_x * elapsed_ticks;
  ship.pos_y += ship.vel_y * elapsed_ticks;
  ship.speed = std::sqrt(ship.vel_x * ship.vel_x + ship.vel_y * ship.vel_y);
  return stats;
}

// -------------------------------------------------------------------------
// NPC ship movement
// -------------------------------------------------------------------------

namespace {

// Ship_GetIonizationIntensity (0x0046c160): the NPC path has no outfit
// opcode-0x28 additions, so its normalized intensity is simply the status
// ionization meter divided by the class capacity. The original returns zero for
// a non-positive capacity and clamps the resulting stat multiplier below.
[[nodiscard]] float NovaShip_IonizationIntensity(const Ship &ship,
                                                 const ShipClass &ship_class) {
  if (ship.ionization_points <= 0.0F || ship_class.ionization_capacity <= 0) {
    return 0.0F;
  }
  return ship.ionization_points /
         static_cast<float>(ship_class.ionization_capacity);
}

} // namespace

// Ghidra 0x00463e70 Ship_ComputeShipMaxTurnRateDeg (NPC branch; the player
// path applies the same base*0.1 + opcode-9/rule inside NovaPlayer_Integrate-
// Movement).
NpcEffectiveStats NovaShip_ComputeEffectiveStats(const GameState &state,
                                                 const Ship &ship,
                                                 const ShipClass &ship_class) {
  NpcEffectiveStats stats;
  stats.turn_rate_deg_per_tick =
      static_cast<float>(ship_class.turn_rate) * 0.1F;
  stats.max_speed_px_per_tick = static_cast<float>(ship_class.speed) / 100.0F;
  stats.thrust_px_per_tick2 =
      static_cast<float>(ship_class.accel) / 10000.0F * 2.0F;
  // Ship_ComputeShipEffectiveThrust (0x004640a0) and
  // Ship_ComputeShipEffectiveMaxSpeed (0x004642e0) return zero for the NPC
  // capability flag 0x400 before applying any other modifier.
  if ((ship_class.capability_flags & 0x0400U) != 0U) {
    return {};
  }

  // Government combat_rating_scale applies to thrust and max speed when the
  // ship belongs to a government. Turn rate is not government-scaled. The
  // skill-variance scale precedes the government scale in the original.
  stats.max_speed_px_per_tick *= ship.skill_variance_scale;
  stats.thrust_px_per_tick2 *= ship.skill_variance_scale;
  if (ship.faction_or_government_id >= 0) {
    const Government *g = state.scenario.Government(
        static_cast<std::int16_t>(ship.faction_or_government_id + 0x80));
    if (g != nullptr) {
      stats.max_speed_px_per_tick *= g->combat_rating_scale;
      stats.thrust_px_per_tick2 *= g->combat_rating_scale;
    }
  }

  // DAT_00575788 = 1/3. A non-self velocity-match lock damps all three
  // movement stats, including turn rate; a self-lock is deliberately neutral.
  const bool velocity_matched =
      ship.velocity_match_target_ship_slot != -1 &&
      ship.velocity_match_target_ship_slot != ship.ship_instance_id;
  if (velocity_matched) {
    constexpr float kVelocityMatchScale = 1.0F / 3.0F;
    stats.max_speed_px_per_tick *= kVelocityMatchScale;
    stats.thrust_px_per_tick2 *= kVelocityMatchScale;
    stats.turn_rate_deg_per_tick *= kVelocityMatchScale;
  }

  // Mission ships use DAT_005757a8 = 2.0 for thrust, speed and the special
  // low-turn-rate correction. The latter is applied below with the same
  // ordering as Ship_ComputeShipMaxTurnRateDeg.
  if (ship.pers_def_slot == 0x03ff) {
    stats.max_speed_px_per_tick *= 2.0F;
    stats.thrust_px_per_tick2 *= 2.0F;
  }

  // Ship_ComputeShipMaxTurnRateDeg applies the mission correction before the
  // general one-degree floor, and ionization damping only while the ship is not
  // thrusting. DAT_00575790 is 6.0 and DAT_00575784 is 1.0.
  if (ship.pers_def_slot == 0x03ff && stats.turn_rate_deg_per_tick < 6.0F) {
    stats.turn_rate_deg_per_tick += 1.0F;
  }
  if (stats.turn_rate_deg_per_tick >= 1.0F) {
    stats.turn_rate_deg_per_tick = std::max(stats.turn_rate_deg_per_tick, 1.0F);
  }
  const float intensity =
      std::min(0.7F, NovaShip_IonizationIntensity(ship, ship_class));
  if (intensity > 0.0F) {
    // Effective thrust always carries the ionization multiplier. The turn
    // helper applies the same damping only in its non-thrusting branch; max
    // speed has no ionization term in the original helper.
    stats.thrust_px_per_tick2 *= (1.0F - intensity);
    if (ship.ai_forward_thrust_cmd <= 0.0F) {
      stats.turn_rate_deg_per_tick *= (1.0F - intensity);
    }
  }
  return stats;
}

// Ports the movement block of Ghidra Ship_HandleShip (0x00433050) for the
// AI-driven NPC ships. The AI layer (Ship_UpdateShipAiState / the behavior
// supervisors) writes ai_desired_heading_deg (degrees, 0 = up, clockwise),
// ai_desired_speed (scalar speed along the heading) and ai_forward_thrust_cmd;
// here the integrator turns the hull toward the desired heading at the class
// turn rate and applies thrust / position integration.
//
// The AI turn is CONTINUOUS (Ship_HandleShip uses the raw
// Ship_ComputeShipMaxTurnRateDeg rate) unlike the player keyboard path which
// rounds to integer degrees/frame before integrating. Thrust follows the
// original three-branch model keyed on ai_desired_speed; ai_forward_thrust_cmd
// carries the RAW effective thrust value (NovaAi_ApplyControls writes
// Ship_ComputeShipEffectiveThrust, NOT 1.0 -- writing 1.0 would make NPCs
// accelerate ~50x too fast):
//   desired == 0 : free-coast (the thrust command is applied as a per-axis
//                  clamped step toward the max-speed projection; gated on
//                  ai_station_hold_timer <= 0).
//   desired >  0 : forward thrust toward `desired` speed, per-axis clamped to
//                  the polar projection of `desired` (Math_AddPolarVelocity-
//                  WithClamp 0x0043b4e0).
//   desired <  0 : physics override; set velocity to heading * abs(desired)
//                  instead of integrating thrust, then decay desired toward
//                  zero by abs(ai_forward_thrust_cmd).
// ai_maneuver_timer_ms is a coast-through-reversal TIMER (not a brake): while
// >0 it suppresses both turning and thrust (the ship holds heading and coasts);
// it counts down by normalized elapsed ticks each frame and is re-set to a
// random 30..59 ticks when the AI decides to reverse into open space.
//
// Stats come from NovaShip_ComputeEffectiveStats (Ship_ComputeShipEffective-
// Thrust / EffectiveMaxSpeed NPC branch): turn = raw_maneuver*0.1 deg/tick,
// max speed = raw_speed/100 px/tick * government combat_rating_scale,
// thrust = raw_accel/10000*2 px/tick^2 * government combat_rating_scale. NPC
// ships carry no outfit inventory; ionization capacity comes from the class.
// TODO(decomp): opcode 7/8/9 outfit bonuses, the per-ship skill_variance_scale
// (+0x40) factor and disable/ionization damping when the NPC outfit/combat
// state is reconstructed.
//
// Gravity-shield ships (ShipClassDef.flags_secondary bit 0x40) keep a scalar
// `speed` (integrated in the thrust block below) and steer their velocity
// through NovaShip_SteerVelocityTowardShipHeading (0x0043b020) in the position
// block, matching the original's two-regime movement model.
void NovaShip_IntegrateNpcMovement(GameState &state,
                                   Ship &ship,
                                   const ShipClass &ship_class,
                                   float elapsed_ticks,
                                   std::uint32_t now_ms) {
  constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
  constexpr float kTwoPi = 6.283185307179586F;
  constexpr float kFullCircleDeg = 360.0F;
  elapsed_ticks = std::max(0.0F, elapsed_ticks);

  // Derived effective stats (see header/block comment above).
  // The turn-rate floor mirrors Ship_ComputeShipMaxTurnRateDeg's NPC branch:
  // the returned rate is clamped up to a minimum floor (gh.data _DAT_00575784
  // = 1.0 deg/frame) whenever the base class rate itself is already at/above
  // that floor (so genuinely sluggish classes keep their low rate). For a clean
  // NPC the computed rate equals the base, so this floor is currently inert; it
  // becomes active only once ionization / disable damping lowers the rate
  // below its base (TODO(decomp)). Kept to match the original's NPC branch.
  const NpcEffectiveStats stats =
      NovaShip_ComputeEffectiveStats(state, ship, ship_class);
  const float base_turn_deg = stats.turn_rate_deg_per_tick;
  float eff_turn_deg = base_turn_deg;
  if (base_turn_deg >= 1.0F) {
    eff_turn_deg = std::max(eff_turn_deg, 1.0F);
  }
  const float eff_max_speed = stats.max_speed_px_per_tick;
  const float eff_thrust = stats.thrust_px_per_tick2;
  // Gravity-shield ships (ShipClassDef.flags_secondary bit 0x40, and not in
  // ai_control_mode 0x0c) keep a scalar Ship.speed and steer their velocity
  // through Ship_SteerVelocityTowardShipHeading instead of vector thrust.
  const bool gravity_shield = NovaShip_HasGravityShield(ship, ship_class);

  // The movement blocks are gated off while the maneuver timer is active, and
  // for the defunct AI state (0x16) the ship also holds course. The original
  // also gates off ships whose class is the 0x2ff sentinel; those fall through
  // to the inactive-guard at the top of Ship_HandleShip in practice.
  const bool coasting = ship.ai_maneuver_timer_ms > 0.0F;
  const bool holds_course = coasting || ship.ai_state_code == 0x16;
  const bool fire_restricted = NovaAiShip_IsDisabled(state, ship);

  // Ship_HandleShip (0x00433050) applies the disabled/derelict damping before
  // integrating position. DAT_00575448 is the float 0.94: disabled
  // ships keep drifting, but lose 6% of each velocity component per frame.
  // This is deliberately separate from AI suppression; it also applies while
  // the ship is coasting and to the gravity-shield scalar speed.
  if (fire_restricted) {
    constexpr float kFireRestrictedVelocityDamp = 0.94F; // DAT_00575448
    ship.vel_x *= kFireRestrictedVelocityDamp;
    ship.vel_y *= kFireRestrictedVelocityDamp;
    ship.speed *= kFireRestrictedVelocityDamp;
  }

  // --- Turn toward the desired heading (continuous AI turn rate). ---
  // The original computes the shortest signed angular delta in degrees from
  // the current heading to = ai_desired_heading_deg, stepping it (in either
  // direction) by eff_max_turn_deg each frame, snapping to the exact heading
  // once within one step. The snap-once branch writes the desired heading
  // directly; otherwise the ship rotates by a full turn step in the direction
  // that closes the angle fastest. ai_turn_bias_dir mirrors the original's
  // turn-bank signal (Ship_HandleShip writes +0xc8f8 from the
  // turn_bank_animation_phase field at +0xc8e4
  // tilt anim: +1 while banking one way, -1 the other, 0 otherwise), which the
  // engine-glow block below consumes for its turn-bias +2 bump.
  ship.ai_turn_bias_dir = 0;
  if (!holds_course) {
    const float cur_deg = ship.heading / kDegToRad;
    // Shortest signed delta [-180, 180] degrees from current to desired.
    const float delta_deg = std::remainder(
        static_cast<float>(ship.ai_desired_heading_deg) - cur_deg,
        kFullCircleDeg);
    const float max_turn_deg = eff_turn_deg * elapsed_ticks;
    if (std::abs(delta_deg) <= max_turn_deg) {
      ship.heading =
          static_cast<float>(ship.ai_desired_heading_deg) * kDegToRad;
    } else {
      ship.heading =
          (cur_deg + std::copysign(max_turn_deg, delta_deg)) * kDegToRad;
      ship.ai_turn_bias_dir = delta_deg > 0.0F ? 1 : -1;
    }
    ship.heading = std::fmod(ship.heading, kFullCircleDeg / kDegToRad);
    if (ship.heading < 0.0F) {
      ship.heading += kTwoPi;
    }

    // Ship_HandleShip regenerates only while the ship is not disabled.
    // In particular, disabled NPCs must not restore shields and lethal hits
    // must not resurrect a ship whose armor has reached zero. The original
    // also leaves this whole movement/regen block disabled while coasting.
    if (!fire_restricted && !NovaAiShip_IsDestroyed(ship)) {
      const float max_shield = static_cast<float>(ship_class.base_shield);
      if (ship.shield_points < max_shield) {
        ship.shield_points = std::min(
            max_shield,
            ship.shield_points +
                static_cast<float>(ship_class.shield_recharge) * elapsed_ticks);
      }
      const float max_armor = static_cast<float>(ship_class.base_armor);
      if (ship.armor_points < max_armor) {
        ship.armor_points = std::min(
            max_armor,
            ship.armor_points +
                static_cast<float>(ship_class.armor_recharge) * elapsed_ticks);
      }
    }
  }

  // --- Forward / reverse thrust along the heading. ---
  // When the ship is coasting (ai_maneuver_timer_ms > 0) or defunct it skips
  // thrust entirely and holds velocity. Otherwise, when a forward-thrust
  // command is present, apply the three-branch ai_desired_speed model (the
  // original gates this whole block on ai_forward_thrust_cmd != 0, so a
  // stopped ship with no thrust command drifts without applying any new
  // velocity). The coast case (desired == 0) is a clamped step toward the
  // class top speed; desired > 0 throttles toward it; desired < 0 is the
  // physics-override path. Non-gravity-shield ships apply heading-aligned
  // thrust; gravity-shield ships accumulate a scalar `speed` clamped at the
  // same caps.
  if (!holds_course && ship.ai_forward_thrust_cmd != 0.0F) {
    const float desired = ship.ai_desired_speed;
    auto add_polar = [&](float speed) {
      // Math_AddPolarVelocity (0x0043b4a0): vel_x += sin(h)*s ; vel_y -=
      // cos(h)*s (heading 0 = up / -y, increasing clockwise).
      ship.vel_x += std::sin(ship.heading) * speed;
      ship.vel_y -= std::cos(ship.heading) * speed;
    };
    auto add_polar_clamped = [&](float thrust_step, float cap) {
      // Math_AddPolarVelocityWithClamp (0x0043b4e0), via the shared
      // player/NPC helper -- Math_AddPolarVelocityWithClamp clamp semantics.
      NovaPlayer_AddPolarVelocityClamped(
          ship.heading, thrust_step, cap, ship.vel_x, ship.vel_y);
    };
    const float thrust_step = ship.ai_forward_thrust_cmd * elapsed_ticks;

    if (desired == 0.0F) {
      // Coast branch. Non-shield: per-axis clamped step toward the class top
      // speed (with a zero command this is a no-op). Gravity-shield: scalar
      // `speed` clamped at the class top speed. The original gates this branch
      // on ai_station_hold_timer <= 0 (a ship parked at a hold point does not
      // coast-accelerate).
      if (ship.ai_station_hold_timer <= 0.0F) {
        if (!gravity_shield) {
          add_polar_clamped(thrust_step, eff_max_speed);
        } else {
          ship.speed =
              std::clamp(ship.speed + thrust_step, 0.0F, eff_max_speed);
        }
      }
    } else if (desired > 0.0F) {
      // Forward thrust toward the requested speed.
      if (!gravity_shield) {
        add_polar_clamped(thrust_step, desired);
      } else {
        ship.speed = std::clamp(ship.speed + thrust_step, 0.0F, desired);
      }
    } else {
      // Physics override: replace the velocity with abs(desired) along the
      // current heading. Negative throttle is not reverse acceleration; its
      // magnitude decays the signed desired value toward zero below.
      if (!gravity_shield) {
        ship.vel_x = 0.0F;
        ship.vel_y = 0.0F;
        add_polar(std::abs(desired));
      } else {
        ship.speed = std::abs(desired);
      }
      // NOTE(decomp) deliberate divergence: the original advances this decay
      // per rendered frame without scaling by g_avg_frame_time_ms
      // (Ship_HandleShip 0x00433050, at 0x00433b03: desired +=
      // fabsf(ai_forward_thrust_cmd)), while capping frame time at 21 ms
      // (Frame_MeasureFrameTiming 0x00432ea0), so its arrival slowdown
      // collapsed faster on faster machines (up to ~1.6x at the ~47 fps
      // ceiling). We deliberately normalize the decay to the 30 Hz simulation
      // cadence (elapsed_ticks) so the glide duration is machine-independent.
      ship.ai_desired_speed +=
          std::abs(ship.ai_forward_thrust_cmd) * elapsed_ticks;
      // Ship_HandleShip (0x00433050) hands the ship to
      // Ship_ResetShipPrimaryAndSecondaryTargets once the desired speed has
      // risen above the negative effective max speed. The threshold uses
      // min(self, lead target) when a valid lead (0..0x3f) is followed
      // (0x00433b26-0x00433b8e). State 8 begins at -50 and advances ~1.165
      // per movement tick before the reset returns the ship to idle and arms
      // the 30..59-tick coast-through timer (armed once: the reset clears the
      // desired speed and thrust command, so the branch is not re-entered).
      float threshold_max_speed = eff_max_speed;
      if (ship.ai_target_ship_slot >= 0 && ship.ai_target_ship_slot <= 0x3f &&
          state.SlotInRange(
              static_cast<std::size_t>(ship.ai_target_ship_slot))) {
        const Ship &lead =
            state.ShipAt(static_cast<std::size_t>(ship.ai_target_ship_slot));
        const ShipClass *lead_class = state.scenario.Ship(
            static_cast<std::int16_t>(lead.ship_class_id + 0x80));
        if (lead_class != nullptr) {
          const NpcEffectiveStats lead_stats =
              NovaShip_ComputeEffectiveStats(state, lead, *lead_class);
          threshold_max_speed =
              std::min(threshold_max_speed, lead_stats.max_speed_px_per_tick);
        }
      }
      if (ship.ai_desired_speed >= -threshold_max_speed) {
        NovaAi_ResetShipPrimaryAndSecondaryTargets(ship);
        if (gravity_shield) {
          // Gravity-shield completion quirk (0x00434f7e): the original zeroes
          // the vector velocity and re-commands the scalar override at the
          // threshold max speed instead of keeping the glide velocity.
          ship.ai_desired_speed = threshold_max_speed;
          ship.vel_x = 0.0F;
          ship.vel_y = 0.0F;
        }
        if (ship.ai_target_ship_slot == -1 && ship.pers_def_slot != 0x3ff) {
          std::uniform_int_distribution<std::int32_t> dist(30, 59);
          ship.ai_maneuver_timer_ms = static_cast<float>(dist(state.rng));
        }
      }
    }
  }

  // Ghidra Ship_HandleShip (0x00433050), jump-spin-up departure block:
  // Ship_ApplyShipAiControls arms ai_station_hold_timer in control mode 4,
  // but the visible departure movement is applied here. The original first
  // damps the stopped ship, then advances its position along the already-
  // aligned heading with a time-ramped jump speed; this is a position step,
  // not ordinary thrust into vel_x/vel_y.
  const bool jump_spinup_control =
      ship.ai_station_hold_timer > 0.0F &&
      (ship.ai_state_code == 2 || ship.ai_state_code == 3 ||
       ship.ai_state_code == 0xb) &&
      (ship.ai_control_mode == 4 || ship.ai_control_mode == 0xd) &&
      !fire_restricted;
  if (jump_spinup_control) {
    constexpr float kJumpVelocityDamp = 0.8F;      // DAT_00575488
    constexpr float kJumpProgressSubtract = 35.0F; // DAT_00575490
    constexpr float kJumpProgressCap = 50.0F;      // DAT_00575388
    constexpr float kJumpDurationScale = 0.01F;    // DOUBLE_00575368
    constexpr float kJumpDurationMs = 350.0F;

    ship.vel_x *= kJumpVelocityDamp;
    ship.vel_y *= kJumpVelocityDamp;

    const float current_heading_deg = ship.heading / kDegToRad;
    const float heading_delta_deg = std::remainder(
        static_cast<float>(ship.ai_desired_heading_deg) - current_heading_deg,
        360.0F);
    const bool aligned = std::abs(heading_delta_deg) <=
                         stats.turn_rate_deg_per_tick * elapsed_ticks;
    if (!aligned) {
      // Ghidra resets the mode-start timestamp while the ship is still
      // turning, so the jump-speed ramp begins only after alignment.
      ship.ai_mode_start_time_ms = now_ms;
    } else {
      // The original uses elapsed wall-clock time multiplied by the ship-class
      // jump_duration_multiplier, divided by duration_ms * 0.01, then subtracts
      // 35. That multiplier is not decoded into ShipClass yet; the base 1.0
      // value preserves the stock timing and is marked as a follow-up gap.
      const float elapsed_jump_ms =
          static_cast<float>(now_ms - ship.ai_mode_start_time_ms);
      float jump_progress =
          elapsed_jump_ms / (kJumpDurationMs * kJumpDurationScale) -
          kJumpProgressSubtract;
      jump_progress = std::clamp(jump_progress, 0.0F, kJumpProgressCap);
      if (jump_progress > 0.0F) {
        // Ghidra's Math_AddPolarVelocity is passed &ship->pos_x here
        // (0x004347e8): the jump ramp directly moves the position. Keeping
        // this out of the ordinary velocity preserves the original launch
        // cadence and prevents mode 4 from turning the ramp into a slow
        // acceleration curve.
        ship.pos_x += std::sin(ship.heading) * jump_progress * elapsed_ticks;
        ship.pos_y -= std::cos(ship.heading) * jump_progress * elapsed_ticks;
        ship.engine_glow_level = static_cast<std::int16_t>(
            std::min(0x20, static_cast<int>(ship.engine_glow_level) + 3));
      }
    }
  }

  // --- Position integration + inertia-less special case. ---
  // Inertia-less ships (base_accel == 0 && base_speed == 0) are pinned: the
  // original zeroes their velocity every frame. Gravity-shield ships turn their
  // scalar `speed` into an actual velocity via Ship_SteerVelocityToward-
  // ShipHeading first. Others integrate pos += vel * frame_time.
  if (ship_class.accel == 0.0F && ship_class.speed == 0.0F) {
    ship.vel_x = 0.0F;
    ship.vel_y = 0.0F;
    ship.speed = 0.0F;
  } else {
    if (gravity_shield) {
      NovaShip_SteerVelocityTowardShipHeading(ship, eff_thrust, elapsed_ticks);
    }
    ship.pos_x += ship.vel_x * elapsed_ticks;
    ship.pos_y += ship.vel_y * elapsed_ticks;
    // Vector-path ships keep `speed` as the velocity magnitude; gravity-shield
    // ships keep the scalar `speed` written by the thrust block and only the
    // steer helper converts it to vel (Ship_HandleShip does not overwrite the
    // scalar for gravity-shield ships).
    if (!gravity_shield) {
      ship.speed = std::sqrt(ship.vel_x * ship.vel_x + ship.vel_y * ship.vel_y);
    }
  }

  // --- Coast-through-reversal timer countdown. ---
  // Despite its legacy field name, the original stores normalized simulation
  // ticks here: Frame_MeasureFrameTiming scales elapsed milliseconds by 0.03
  // before publishing g_avg_frame_time_ms.
  if (ship.ai_maneuver_timer_ms > 0.0F) {
    ship.ai_maneuver_timer_ms =
        std::max(0.0F, ship.ai_maneuver_timer_ms - elapsed_ticks);
  }

  // --- Engine glow level (Ghidra ShipState field_0xc8d4). ---
  // Mirrors Ship_HandleShip's glow drive: the level ramps toward 0x20 (32)
  // under a full burn, toward 0x18 (24) under low throttle (thrust command
  // below 2x the effective thrust, gh.data DAT_0057531c = 2.0), fades by one
  // decrement while not thrusting (LAB_00435197) and again while reversing, and
  // gets an extra +2 toward 0x18 while banking into a turn (ai_turn_bias_dir
  // set and class sprite_behavior_flags bit 2). The renderer maps level/24
  // clamped to [0,1] onto the engine-glow alpha, exactly as for the player.
  {
    std::int16_t &glow = ship.engine_glow_level;
    auto fade_to_zero = [&]() { // LAB_00435197: single decrement toward 0.
      if (glow > 0) {
        glow = static_cast<std::int16_t>(glow - 1);
      }
    };
    // Ship_HandleShip only derives the bank signal for classes with the
    // banking flag (bit 0), then applies the glow boost when the engine-glow
    // flag (bit 1) is also present.
    if (ship.ai_turn_bias_dir != 0 &&
        (ship_class.sprite_behavior_flags & 1U) != 0U &&
        (ship_class.sprite_behavior_flags & 2U) != 0U) {
      if (glow < 0x18) {
        glow = static_cast<std::int16_t>(glow + 2);
        if (glow > 0x18) {
          glow = 0x18;
        }
      }
    }
    if (ship.ai_forward_thrust_cmd <= 0.0F) {
      fade_to_zero();
    } else if (ship.ai_maneuver_timer_ms > 0.0F || ship.ai_state_code == 0x16) {
      // Thrust command present but the ship is coasting through a reversal (or
      // defunct): the original jumps to the fade label here too.
      fade_to_zero();
    } else if (ship.ai_forward_thrust_cmd < eff_thrust * 2.0F) {
      // Low throttle: settle the glow at the 0x18 cruise level.
      if (glow < 0x18) {
        glow = static_cast<std::int16_t>(glow + 1);
      } else if (glow > 0x18) {
        glow = static_cast<std::int16_t>(glow - 1);
      }
    } else {
      // Full burn: raise toward the 0x20 cap.
      if (glow < 0x20) {
        glow = static_cast<std::int16_t>(glow + 1);
      }
    }
    ship.engine_glow_intensity =
        std::clamp(static_cast<float>(glow) / 24.0F, 0.0F, 1.0F);
  }
}

// Port of Ghidra Ship_SteerVelocityTowardShipHeading (0x0043b020). Takes the
// gravity-shield ship's scalar `speed` + `heading`, resets the velocity to the
// heading*speed vector, then relaxes it back toward the previous frame's
// velocity by a per-axis step of eff_thrust * 4.0 * frame_time (never
// overshooting the prior velocity), yielding a smooth velocity rotation. The
// turn-scale 4.0 mirrors _DAT_005754ac (provisional).
void NovaShip_SteerVelocityTowardShipHeading(Ship &ship,
                                             float eff_thrust,
                                             float elapsed_ticks) {
  const float prev_vel_x = ship.vel_x;
  const float prev_vel_y = ship.vel_y;
  // Reset the vector velocity to forward-heading * speed (Ship.speed is the
  // gravity-shield scalar), via Math_AddPolarVelocity semantics.
  const float new_vx = std::sin(ship.heading) * ship.speed;
  const float new_vy = -std::cos(ship.heading) * ship.speed;
  const float step = eff_thrust * 4.0F * elapsed_ticks;
  // new + clamp(prev - new, -step, +step): move the heading snap back toward
  // the previous velocity at most `step` per axis, never crossing it.
  ship.vel_x = new_vx + std::clamp(prev_vel_x - new_vx, -step, step);
  ship.vel_y = new_vy + std::clamp(prev_vel_y - new_vy, -step, step);
}

// ---------------------------------------------------------------------------
// Ghidra PlayerTick_StatusAndOutfitEvents (internal label of
// Ship_HandlePlayerShipCore 0x0044aa70, block 0x0044b240..0x0044b7c4 plus the
// carried-bomb tails at 0x0044da75/0x0044daa0 and the death-bookkeeping
// prologue of the parent).
// ---------------------------------------------------------------------------

namespace {

// Mirrors the original's NovaRandom_Range(n) -> integer in [0, n) (draws from
// GameState.rng like the other clean-room roll sites).
std::int16_t RollRandom(GameState &state, std::int32_t n) {
  if (n <= 0) {
    return 0;
  }
  std::uniform_int_distribution<std::int32_t> dist{0, n - 1};
  return static_cast<std::int16_t>(dist(state.rng));
}

// Outfit-def scan shared by Frame_ShouldTriggerAutoRepairTick (0x0046e540) and
// the carried-bomb detonation scans: the original walks the four mod-type
// words of every owned outfit (decompile `name - 0x26 + i*2`, 0x37c-byte def
// stride) and reports the first matching def.
const Outfit *FindOutfitWithModType(const GameState &state,
                                    std::size_t outfit_index,
                                    std::int16_t mod_type) {
  if (outfit_index >= state.scenario.outfits.size()) {
    return nullptr;
  }
  const Outfit &def = state.scenario.outfits[outfit_index];
  if (def.mod_type == mod_type) {
    return &def;
  }
  for (const std::int16_t alt : def.alt_mod_types) {
    if (alt == mod_type) {
      return &def;
    }
  }
  return nullptr;
}

// Ghidra Frame_ShouldTriggerAutoRepairTick (0x0046e540): random gate (1-in-N,
// N = 500 with the low-tick-scale reroll; 500 / frame tick scale otherwise),
// a destroyed-ship veto, and the repair outfit's ModType 0x31 presence among
// owned outfits (player path). The original additionally checks fire-
// restriction; the player caller already gates on it.
bool Frame_ShouldTriggerAutoRepairTick(GameState &state) {
  const int roll_range =
      state.last_frame_tick_scale <= kFrameScaleAutoRepairRerollGate
          ? kAutoRepairFrameRerollRange
          : static_cast<int>(static_cast<float>(kAutoRepairFrameRerollRange) /
                             state.last_frame_tick_scale);
  if (RollRandom(state, roll_range) != 0) {
    return false;
  }
  if (NovaAiShip_IsDestroyed(state.player)) {
    return false;
  }
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    if (state.inventory.outfit_owned_count[idx] > 0 &&
        FindOutfitWithModType(state, idx, kAutoRepairOutfitModType) !=
            nullptr) {
      return true;
    }
  }
  return false;
}

// Ghidra PlayerTick_StatusAndOutfitEvents sub-branch 0x0044ab50: on death with
// a carried bomb, open the escape-pod selection dialog (first owned outfit
// with ModType 0x2f / ModVal > 0).
void RunDeathEscapePodScan(GameState &state) {
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    if (state.inventory.outfit_owned_count[idx] <= 0) {
      continue;
    }
    const Outfit *pod =
        FindOutfitWithModType(state, idx, kBombEscapePodModType);
    if (pod == nullptr || pod->mod_val <= 0) {
      continue;
    }
    // TODO(decomp(0x0044ab6c)) skipped: NovaUi_HideTravelSelectionSprite,
    // NovaUi_RedrawGameplayViewportAndRadar, NovaPlatform_EnsureCursorVisible,
    // Ui_LoadSelectionDialogResource / Ui_RunTravelSelectionDialog and
    // NovaUi_MarkTravelAndStatusPanelsDirty are presentation passes of the
    // interactive selection-dialog shell, not reconstructed yet.
    NovaLog::Info("escape pod dialog requested (outfit {})", pod->name);
    break;
  }
}

// Message composer for the bomb/escape-pod overlays: STR# 0x7d2 entry 0x27
// ("You ") + outfit LC name/plural + entry 0x28/0x29. The original copies the
// name out of stride-0x100 Pascal-string arrays indexed by outfit id; the
// clean-room uses the decoded LC fields.
std::string ComposeBombOverlay(const GameState &state,
                               std::size_t outfit_index,
                               bool plural) {
  std::string text =
      NovaHud_LoadStringEntry(kStringListFlightText, kStrBombYou).value_or("") +
      " ";
  const Outfit &def = state.scenario.outfits[outfit_index];
  text += plural ? def.lc_plural : def.lc_name;
  text += " ";
  text += NovaHud_LoadStringEntry(kStringListFlightText,
                                  plural ? kStrBombDetonatedPlural
                                         : kStrBombDetonatedSingular)
              .value_or("");
  return text;
}

// Ghidra 0x0044daa0 escape-pod bomb variant (carried bomb outfit class 1):
// kills shields/armor immediately, stops the hyperspace audio, shows the
// deployment overlay, and returns to the menu shell (DAT_007354a5).
void DetonateEscapePodBomb(GameState &state) {
  PlayerShip &p = state.player;
  p.armor_points = -1.0F;
  p.shield_points = -1.0F;
  p.ai_station_hold_timer = -1.0F;

  std::string text;
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    const auto owned = state.inventory.outfit_owned_count[idx];
    if (owned <= 0) {
      continue;
    }
    if (FindOutfitWithModType(state, idx, kBombEscapePodModType) == nullptr) {
      continue;
    }
    // Singular/plural choice reproduces the original's name-flag ladder: the
    // plural name needs owned >= 2 and a non-empty plural, the singular needs
    // owned == 1 and a non-empty name; otherwise no name message is shown.
    const Outfit &pod_def = state.scenario.outfits[idx];
    const bool has_plural = !pod_def.lc_plural.empty();
    const bool has_singular = !pod_def.lc_name.empty();
    if (owned < 2 || !has_plural) {
      if (owned != 1 || !has_singular) {
        break; // deployment only, no composed name message
      }
    }
    text = ComposeBombOverlay(state, idx, owned >= 2);
    break;
  }
  if (text.empty()) {
    // DAT_007354a4 can hold a custom latched message; the fallback is STR#
    // 0x7d2 entry 0x26. The custom-message source is not reconstructed yet
    // (TODO(decomp)).
    text = NovaHud_LoadStringEntry(kStringListFlightText, kStrEscapePodDeployed)
               .value_or("");
  }
  NovaHud_ShowOverlayMessage(
      state, text, 0xe0, 0xe0, 0xe0, kOverlayDurationEscapePod);

  // NovaAudio_UnregisterCallbacks on the warp-up / warp-up-x2 handles (the
  // port's pending one-shots are drained by the loop; clear the latches).
  state.warp_up_sound_pending = false;
  state.warp_out_sound_pending = false;
  state.return_to_menu_pending = true;
}

// Ghidra 0x0044b446 bomb detonation: message, impact effect from the def's
// 0x80-biased ModVal, outfit removal, and an armor-piercing self-damage roll
// of max_armor * fraction + addend (bypass_shields, force_armor_only,
// suppress_retarget, no aggro).
void DetonateCarriedBomb(GameState &state) {
  PlayerShip &p = state.player;
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    if (state.inventory.outfit_owned_count[idx] <= 0) {
      continue;
    }
    const Outfit *bomb = FindOutfitWithModType(state, idx, kBombWeaponModType);
    if (bomb == nullptr) {
      continue;
    }
    NovaHud_ShowOverlayMessage(
        state,
        ComposeBombOverlay(
            state, idx, state.inventory.outfit_owned_count[idx] >= 2),
        0xe0,
        0xe0,
        0xe0,
        kOverlayDurationBomb);
    if (bomb->mod_val >= kBombEffectIdBias &&
        bomb->mod_val < kBombEffectIdBias + 0x40) {
      NovaEffects_SpawnAreaImpact(
          state,
          p.pos_x,
          p.pos_y,
          static_cast<std::int16_t>(bomb->mod_val - kBombEffectIdBias),
          0,
          true);
    }
    state.inventory.outfit_owned_count[idx] = 0;

    const ShipClass *cls =
        state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
    const float max_armor = cls ? static_cast<float>(cls->base_armor) : 0.0F;
    const int roll =
        RollRandom(state,
                   static_cast<int>(max_armor * kBombDamageArmorFraction +
                                    kBombDamageArmorAddend));
    NovaCollision_ResolveShipHitFromWeaponSlot(
        state,
        0,
        p.pos_x,
        p.pos_y,
        0,
        static_cast<std::int16_t>(roll + max_armor),
        0,
        0xFFFF,
        false,
        false,
        true,
        true,
        0);
    break;
  }
  // Ghidra g_player_status_panel_dirty + Outfit_RecomputeOutfitDerivedState:
  // the outfit pool changed, so derived stats must be recomputed.
  state.stat_cache_valid = false;
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
}

} // namespace

// Ghidra PlayerTick_StatusAndOutfitEvents: internal label of
// Ship_HandlePlayerShipCore (0x0044aa70), block 0x0044b240..0x0044b7c4 with the
// carried-bomb tails at 0x0044da75/0x0044daa0. Runs after the hyperspace-exit
// gate and before the manual-flight block, ahead of every flight-input branch.
bool NovaPlayer_TickStatusAndOutfitEvents(GameState &state,
                                          float elapsed_ticks) {
  PlayerShip &p = state.player;

  // --- Player-death bookkeeping (Ship_HandlePlayerShipCore prologue) ------
  // Ship_UpdateVisualState (0x00428340) deactivates any destroyed ship once
  // its death-timer presentation finishes; that pass is not reconstructed for
  // the player yet, so the player-side slice is bridged here.
  if (p.is_active && p.death_timer_active > kDeathTimerExpireFloor) {
    // Death presentation running: tick the timer down; the original skips the
    // whole status/outfit block for the dying ship (inactive-branch return).
    p.death_timer_active -= elapsed_ticks;
    if (p.death_timer_active <= kDeathTimerExpireFloor) {
      // Presentation finished: run the Ship_UpdateVisualState (0x00428340)
      // destruction finale (blast damage to nearby hulls; the mission arm is
      // player-exempt), which deactivates the hull and clears its target.
      NovaShip_RunShipDestructionFinale(state, p);
    }
    return true;
  }

  // --- Inactive branch (0x0044aa70 prologue) -------------------------------
  if (!p.is_active) {
    if (p.death_timer_active > kDeathTimerExpireFloor) {
      // Ghidra _g_jump_turnaround_turn_rate_addend decrement; floor sentinel
      // is DAT_00575538.
      p.death_timer_active -= kJumpTurnaroundTurnRateAddend;
      if (p.death_timer_active > kDeathTimerExpireFloor) {
        return true;
      }
    }
    if (state.bomb_outfit_class != 0) {
      RunDeathEscapePodScan(state);
    }
    // Ghidra DAT_00596d38, consumed by the spaceflight loop.
    state.game_over_pending = true;
    // Restart command (0x0044abf8): the original polls key-binding slot 0x20
    // through NovaInput_IsCommandActiveWithGameplayGuards and calls
    // Ship_ResetPlayerShipState to relaunch. TODO(decomp(0x0044abf8)) skipped:
    // the gameplay-command input channel is not reconstructed; the spaceflight
    // loop returns to the menu shell on the death latch instead.
    return true;
  }

  // --- Fire-restricted damping (0x0044b240) --------------------------------
  const bool fire_restricted = NovaAiShip_IsDisabled(state, p);
  if (fire_restricted) {
    p.vel_x *= kPlayerFireRestrictedVelocityDamp;
    p.vel_y *= kPlayerFireRestrictedVelocityDamp;
  } else {
    p.boarded_target_latch = 0;
  }

  // g_player_recently_hit_timer (DAT_0073549c): armed to 300 ticks when the
  // player takes a hit, decays one tick per frame while at or above the
  // cutoff; suppresses armor regeneration and the disabled auto-repair pass
  // until it falls back below the cutoff.
  if (kRecentlyHitRegenCutoff <= state.recently_hit_timer) {
    state.recently_hit_timer -= elapsed_ticks;
  }

  // --- Disabled auto-repair (0x0044b2a5) -----------------------------------
  if (fire_restricted && state.recently_hit_timer < kRecentlyHitRegenCutoff &&
      Frame_ShouldTriggerAutoRepairTick(state)) {
    const ShipClass *cls =
        state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
    if (cls != nullptr) {
      const float max_armor = static_cast<float>(cls->base_armor);
      p.armor_points =
          (cls->capability_flags & 0x10) != 0
              ? max_armor * kAutoRepairArmorFractionFlag0x10 +
                    kAutoRepairArmorAddend
              : max_armor * kAutoRepairArmorFraction + kAutoRepairArmorAddend;
    }
    NovaHud_ShowOverlayMessage(
        state,
        NovaHud_LoadStringEntry(kStringListFlightText, kStrAutoRepairEngaged)
            .value_or(""),
        0xe0,
        0xe0,
        0xe0,
        kOverlayDurationAutoRepair);
    state.pending_ui_sounds.push_back(
        GameState::PendingUiSound{kAutoRepairSoundTransitionIndex, 1});
  }

  // --- Distress-call cue (0x0044b3a8, tail at 0x0044d410) ------------------
  // Every 60th frame the original re-evaluates Ship_AreAnyShipsEligibleFor-
  // DistressCall; a rising edge with no blocking timed action plays the
  // distress alert (g_transition_sound_handle_table[5], snd 155) five times.
  if (state.spaceflight_frame_counter % 60 == 0) {
    state.distress_cue_active_prev = state.distress_cue_active;
    state.distress_cue_active =
        NovaAi_AreAnyShipsEligibleForDistressCall(state);
    if (state.distress_cue_active && !state.distress_cue_active_prev &&
        p.timed_action_counter < 1) {
      state.pending_ui_sounds.push_back(
          GameState::PendingUiSound{kDistressCueSoundTransitionIndex, 5});
      // The original also sets g_travel_countdown = 0x1e when
      // g_pref_sound_volume < 2; the preference and the countdown consumer are
      // not modelled yet (TODO(decomp)).
    }
  }

  // --- Carried-bomb countdown / detonation (0x0044b3d4) ---------------------
  if (state.bomb_outfit_class == 0) {
    state.bomb_detonation_timer = 0.0F;
  } else if (state.bomb_detonation_timer < kBombDetonationTimerFloor) {
    // 0x0044da75 tail: expired timer rerolls a whole number of intervals.
    state.bomb_detonation_timer =
        static_cast<float>(RollRandom(state, kBombDetonationRerollMax));
  } else if (!NovaAiShip_IsDestroyed(p)) {
    state.bomb_detonation_timer += elapsed_ticks;
    if (state.bomb_detonation_timer > kBombDetonationIntervalFrames) {
      state.pending_impact_sounds.push_back(
          GameState::PendingImpactSound{0, p.pos_x, p.pos_y});
      if (state.bomb_outfit_class == 1) {
        DetonateEscapePodBomb(state);
      } else {
        DetonateCarriedBomb(state);
      }
    }
  }
  return false;
}

void NovaPlayer_UpdateFromInput(GameState &state,
                                const FlightInput &input,
                                float elapsed_ticks) {
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

  const float fuel_burn = Outfit_GetPlayerAfterburnerFuelBurnRate(state);
  const bool afterburner_active = input.afterburner && !input.reverse &&
                                  fuel_burn <= p.fuel_points &&
                                  fuel_burn > 0.0F;
  // Ghidra's old name g_player_in_gravity_well is misleading: this is the
  // opcode-15 afterburner latch. Stellar_TickStellarGravityPull independently
  // sets g_gravity_pull_active, which disables the afterburner's boosted speed
  // branch but does not prevent normal thrust.
  const bool gravity_present =
      NovaPlayer_ApplyStellarGravity(state, elapsed_ticks);

  // Map the effective raw stats onto the movement integrator's ShipClass view
  // (it divides raw accel/speed by the loader scale; turn is deg/tick).
  ShipClass effective_class;
  effective_class.accel = eff.thrust_raw;
  effective_class.speed = eff.speed_raw;
  effective_class.turn_rate = eff.turn_raw;
  if (afterburner_active && !gravity_present) {
    // DAT_00575610 = 1.8: the afterburner control branch raises the speed cap
    // while no stellar gravity pull is active.
    effective_class.speed *= 1.8F;
  }
  (void)NovaPlayer_IntegrateMovement(p, input, effective_class, elapsed_ticks);

  if (afterburner_active) {
    p.fuel_points = std::max(0.0F, p.fuel_points - fuel_burn * elapsed_ticks);
  }

  TickIonizationDecay(state, p, elapsed_ticks);

  // ShipState +0xc8d4 is an integer engine/glow control, not a free-running
  // alpha ramp. Normal thrust approaches 24; afterburning extends it to 32;
  // coasting and reverse decrement by one renderer frame. This makes the
  // observable rise/fall and afterburner cap match player control; rendering
  // derives its alpha from that level until SpriteWorld's native glow blend is
  // reconstructed.
  const std::int16_t glow_target =
      afterburner_active ? 32 : (p.engine_thrust ? 24 : 0);
  if (p.engine_glow_level < glow_target) {
    ++p.engine_glow_level;
  } else if (p.engine_glow_level > glow_target) {
    --p.engine_glow_level;
  }
  p.engine_glow_intensity =
      std::clamp(static_cast<float>(p.engine_glow_level) / 24.0F, 0.0F, 1.0F);

  // Ghidra Ship_HandlePlayerShipCore (0x0044aa70) runs Misn_TickActiveMission-
  // Timers (0x00448910) as part of the per-frame player maintenance. The
  // original also re-ticks after mission script execution and in
  // Stellar_ProcessTravelAndLanding (0x004588b1); those clean-room call sites
  // are not reconstructed yet (TODO(decomp)).
  Misn_TickActiveMissionTimers(state);
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
  // The recharge rate is in shield points per 30 Hz reference frame. Convert
  // measured wall-clock time to that same normalized frame unit.
  const float rate = eff.shield_recharge * (frame_time_ms / (1000.0F / 30.0F));
  if (rate <= 0.0F) {
    return;
  }
  p.shield_points = std::min(eff.max_shield_points, p.shield_points + rate);
}

// Ghidra 0x00489210 Ship_RunSpaceflightMode.
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
    (void)NovaIntroCinematic_Run(platform, audio, state);
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

// Ghidra Frame_QueueCombatChatter (0x00426ce0). The original writes three
// globals; the clean-room latches the same triple on GameState. The consumer
// pass (Frame_UpdateCombatChatter, plays the STR# comm audio/visuals) remains
// TODO(decomp).
void NovaFrame_QueueCombatChatter(GameState &state,
                                  std::int16_t kind,
                                  std::int16_t government_id,
                                  std::int16_t variant) {
  state.pending_combat_chatter_kind = kind;
  state.pending_combat_chatter_government_id = government_id;
  state.pending_combat_chatter_variant = variant;
}

} // namespace game
