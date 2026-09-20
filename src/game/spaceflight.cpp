#include "spaceflight.hpp"
#include "spaceflight_internal.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "asteroid.hpp"
#include "boarding_plunder.hpp"
#include "collision.hpp"
#include "command_input.hpp"
#include "docked_dialog.hpp"
#include "escort_commands.hpp"
#include "escort_formation.hpp"
#include "flight_automation.hpp"
#include "frame_timing.hpp"
#include "game_state.hpp"
#include "hud_overlay.hpp"
#include "hud_renderer.hpp"
#include "impact_effects.hpp"
#include "intro_cinematic.hpp"
#include "landed_window.hpp"
#include "mission.hpp"
#include "mission_script.hpp"
#include "negotiation_dialog.hpp"
#include "nova_font.hpp"
#include "nova_random.hpp"
#include "outfit.hpp"
#include "player_info_window.hpp"
#include "radar_panel.hpp"
#include "route_map.hpp"
#include "selection_text_dialog.hpp"
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

// Spaceflight timing values used by the coordinator.
constexpr float kOriginalMaxRateFrameMs = 21.0F;
constexpr float kOriginalTickMs = 1000.0F / 30.0F;

// g_transition_sound_handle_table (snd 150+i): [4] auto-repair cue. The
// combat-alert cue is the separate snd 370 "Red Alert" handle (Ghidra
// g_nova_control_bits[144] / 0x0059155c), not a transition-table slot.
// NovaAudio_PreloadGameplayData (0x004b0957-0x004b0962) also decodes snd 370
// into a dedicated handle for the first-hostile alert; the port plays it from
// the contiguous gameplay_sounds cache (ids 200..455).
constexpr std::size_t kRedAlertGameplaySoundIndex = 370 - 200;

namespace {

// Frame_TickSystems preserves the original scope order. Unimplemented scopes
// intentionally do nothing; logging them per frame would overwhelm diagnostics.
void Stub_PlayerCore(GameState &state) { (void)state; }

// Ghidra scope 9 of Frame_TickSystems (0x004186b0). The original splits
// weapon contact into the sprite-overlap callbacks Ship_HandleSpritePair-
// Collision (0x004374f0) and Asteroid_HandleSpritePairCollision (0x00436f70),
// which run during sprite-layer processing, plus the blast-proximity pass
// Shot_ResolveCollisions (0x00437e20) in this scope. The stellar contact
// branch and the beam-vs-asteroid arm of Shot_UpdateBeamHitQueue remain
// deferred.
void Stub_Collisions(GameState &state) {
  NovaWeapon_ResolveDirectShotCollisions(state);
  NovaWeapon_ResolveProjectileCollisions(state);
  // The original's sprite layer also pairs ships against the freeflight-object
  // layer during the same overlap pass (mining-scoop collection).
  NovaWeapon_ResolveFreeflightScoop(state);
}

// Ghidra scope 0xc of Frame_TickSystems: the per-tick status/scan pass. Its
// reconstructed member is the radar proximity-scan roll
// (Frame_RollProximityScanDetection 0x0045d030, g_proximity_scan_detected),
// which drives the radar panel's interference static.
void Stub_DrawStatus(GameState &state) {
  Frame_RollProximityScanDetection(state);
}

// Ghidra scope 6 parts 1 & 2 of Frame_TickSystems (0x004186b0). Part 1 ran the
// per-frame targeting setup (Ship_EscortFireAtUnprovokedTarget etc.);
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
  const std::uint32_t now_ms =
      static_cast<std::uint32_t>(state.gameplay_now_ms);
  // Frame_TickSystems (0x004186b0) scope-6 leader-flag pass: snapshot AI
  // targets, reacquire dead leaders, and refresh the +0xC0/+0xC1/+0xC2
  // leader bytes that gate the per-frame escort formation updates.
  Ship_TickLeaderFlags(state);
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
      ship.engine_glow_level = 0;
      ship.engine_glow_intensity = 0.0F;
      // Do NOT clear vel_x/vel_y here. Ship_HandleShip (0x00433050) integrates
      // a wreck's position from its residual velocity and only applies the
      // 0.995 fire-restricted damp, so destroyed hulls coast out instead of
      // stopping dead. The ship pass (Stub_HandleShips) runs after this pass
      // and performs that integration.
      continue;
    }
    // Frame_TickSystems calls Ship_UpdateShipAI for every active NPC in the
    // current system. Behavior 0 merely skips the behavior-supervisor
    // dispatch inside that function; the state/control tail must still run.
    // In particular, state-8 arrivals need mode 0x0a armed every frame or
    // their seeded 50-unit inward velocity never decays.
    // The original's second argument only gates its AI-update throttle
    // (g_ai_update_period); the port runs at maximum cadence and does not model
    // that throttle, so the argument is not carried here.
    NovaAi_UpdateShipAI(state, ship, now_ms, elapsed_ticks);
  }
}

// Ghidra scope 0xb of Frame_TickSystems (0x004186b0): the per-tick in-system
// NPC/reactivity pass. The original runs: Mission_TickShipInteractionReactions,
// NovaFrame_UpdateCombatChatter, Frame_TickHudOverlayAndRouteMapTimers,
// Ship_TallyInboundWeaponThreat, then System_TickNpcSpawnMaintenance
// (encounter fleets + random dude ships up to the system's avg_ships cap) and
// Asteroid_Spawn('\x01') (the asteroid ring), before clearing the
// g_ai_misc_event_flag / g_ai_target_refresh_needed latches.
//
// This pass performs the mission interaction-reaction slice (0x00443760) and
// the NPC-population slice (NovaSystem_TickNpcSpawnMaintenance, which spawns
// encounter-fleet leads / random dude ships toward avg_ships).
// The overlay helper's HUD expiry and route-map deadline are represented by
// their wall-clock state elsewhere in the port. The asteroid ring and
// interaction flags are still absent.
void Stub_TickReactionsAndNpcSpawns(GameState &state,
                                    SdlAudio &audio,
                                    float elapsed_ticks) {
  // Frame_MeasureFrameTiming (0x00432ea0) admits one original spaceflight
  // iteration per 21 ms. Scope 0xb contains discrete counters and RNG draws,
  // so replay whole calls instead of tying them to the presentation rate.
  state.npc_maintenance_raw_tick_accumulator +=
      RawSpaceflightCallTicks(std::max(0.0F, elapsed_ticks));
  while (state.npc_maintenance_raw_tick_accumulator >= 1.0F) {
    state.npc_maintenance_raw_tick_accumulator -= 1.0F;
    // The original's AI mode timers use a global millisecond tick source.
    const std::uint32_t now_ms =
        static_cast<std::uint32_t>(state.gameplay_now_ms);
    Mission_TickShipInteractionReactions(state, now_ms);
    NovaFrame_UpdateCombatChatter(state, audio);
    NovaWeapon_TallyInboundWeaponThreat(state);
    // This countdown is normalized by g_avg_frame_tick_scale in the original;
    // one replayed 21 ms call therefore contributes 0.63 normalized ticks.
    NovaSystem_UpdateReinforcementCountdown(state, kOriginalMaxRateFrameTicks);
    NovaSystem_TickNpcSpawnMaintenance(
        state, state.player.current_system_id, now_ms);
  }
}

void Stub_CalcAiOdds(GameState &state) {
  const std::int16_t current_system = state.player.current_system_id;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (ship.is_active && ship.current_system_id == current_system) {
      NovaAi_UpdateShipCombatOddsScore(state, ship);
    }
  }
}

// Ghidra scope 7 of Frame_TickSystems -> Shot_HandleShot (0x00435830). Shot
// movement/lifetime/cooldown bookkeeping runs here, after scope 9
// collision checks, matching the original phase order.
void Stub_HandleShots(GameState &state,
                      float elapsed_ticks,
                      const NovaPreferences &prefs) {
  NovaWeapon_TickShots(state, elapsed_ticks, &prefs);
}

// Ghidra Ship_HandleShip (0x00433050) destruction debris-puff arm. Once the
// death timer is running, the wreck emits a directional debris fragment at the
// exact death-timer halfway point (gated by the personality Flags 0x2 /
// government Flags 0x100 rule) and then through the class timed-action cascade
// during the second half. This is separate from the Ship_UpdateVisualState
// (0x00428340) Explode1/Explode2 area impacts owned by ship_visual.cpp. Runs
// for NPCs only: Ship_HandlePlayerShipCore (0x0044aa70) has no such arm.
// `ship.death_timer_active` has already been decremented by death_timer_step
// for this frame by the caller and was > 0 before the decrement.
void TickShipHandleDestructionDebrisPuffs(GameState &state,
                                          Ship &ship,
                                          std::int16_t raw_frame_counter) {
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls == nullptr) {
    return;
  }
  const float half_death_delay =
      static_cast<float>(cls->death_delay_frames) * 0.5F;

  // Halfway puff. The original compares the decremented float timer to
  // field_0x10 * g_death_delay_half_fraction_f64 (0.5) directly, which is exact
  // because it steps by a constant 1.0: the integer timer only lands on the
  // half when DeathDelay is even (an odd DeathDelay gives an x.5 target the
  // integer sequence never hits). The clean-room logical-call scheduler steps
  // by the same 1.0; the crossing form plus parity guard states that quirk
  // directly and remains robust if this helper's caller changes.
  const float previous_timer = ship.death_timer_active + 1.0F;
  const bool half_landing = (cls->death_delay_frames % 2) == 0;
  if (ship.pers_def_slot != -1 && half_landing &&
      ship.death_timer_active <= half_death_delay &&
      previous_timer > half_death_delay) {
    bool spawn_puff = false;
    if (ship.pers_def_slot >= 0 &&
        ship.pers_def_slot <
            static_cast<std::int16_t>(state.scenario.pers_defs.size())) {
      const PersDef &pers =
          state.scenario
              .pers_defs[static_cast<std::size_t>(ship.pers_def_slot)];
      if ((pers.flags_primary & 0x0002U) != 0U) {
        if (ship.faction_or_government_id == -1) {
          spawn_puff = true;
        } else {
          const Government *govt =
              state.scenario.GovernmentByIndex(ship.faction_or_government_id);
          spawn_puff = govt != nullptr && (govt->flags_primary & 0x0100U) == 0U;
        }
      }
    }
    if (spawn_puff) {
      NovaEffects_SpawnShipDestructionDebrisPuff(state, ship);
      // Ship_HandleShip queues DAT_00591a80 after the visual helper returns,
      // including when its 32-slot fading-effect pool was already full.
      state.pending_destruction_sounds.push_back({ship.pos_x, ship.pos_y});
    }
  }

  // Timed-action cascade. interval = max(10, trunc((DeathDelay /
  // TimedActionInit) * 0.4)) (DAT_005754a0), divided by the frame tick scale
  // when it is positive (_DAT_005753b0 = 0.0). Fires while the timer is in the
  // second half, decrementing the counter so the class emits exactly its
  // initial counter worth of fragments.
  if (ship.timed_action_counter > 0 && cls->escape_pod_count > 0 &&
      ship.death_timer_active <= half_death_delay) {
    // The original casts both operands to int first, so this is integer
    // division before the 0.4 (DAT_005754a0) scale.
    const int death_delay = static_cast<int>(cls->death_delay_frames);
    const int init = static_cast<int>(cls->escape_pod_count);
    int interval =
        static_cast<int>(static_cast<float>(death_delay / init) * 0.4F);
    if (interval < 10) {
      interval = 10;
    }
    if (raw_frame_counter % interval == 0 ||
        ship.timed_action_counter == cls->escape_pod_count) {
      ship.timed_action_counter =
          static_cast<std::int16_t>(ship.timed_action_counter - 1);
      NovaEffects_SpawnShipDestructionDebrisPuff(state, ship);
      // Ghidra 0x00433050 queues the cue independently of fragment-pool
      // admission, so a visually saturated battle does not lose this sound.
      state.pending_destruction_sounds.push_back({ship.pos_x, ship.pos_y});
    }
  }
}

// Ship_HandleShip (0x00433050) ionization decay tail + Ship_ComputeIonization-
// DecayRate (0x0046c080): both NPC and player paths multiply the rate by
// g_avg_frame_tick_scale (30 Hz tick units), NOT milliseconds.
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
    if (ship.squad_leader_ship_slot < -1 ||
        ship.squad_leader_ship_slot > 0x3f) {
      ship.squad_leader_ship_slot = -1;
    }
    if (ship.defense_fleet_home_stellar_id < -1 ||
        ship.defense_fleet_home_stellar_id > 0x7ff) {
      // Ghidra quirk: this range check resets squad_leader_ship_slot, not
      // defense_fleet_home_stellar_id.
      ship.squad_leader_ship_slot = -1;
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
    // decrements the death presentation timer here (one tick per original
    // spaceflight call,
    // k_unit_f32 = 1.0, paused while gameplay time is frozen); the destruction
    // finale runs in the visual-state pass below.
    if (NovaAiShip_IsDestroyed(ship)) {
      ship.ai_state_code = 0x16;
      ship.ai_control_mode = 0;
      ship.ai_forward_thrust_cmd = 0.0F;
      ship.ai_desired_speed = 0.0F;
      ship.engine_glow_level = 0;
      ship.engine_glow_intensity = 0.0F;
      // Ship_HandleShip's position integration precedes Ship_UpdateVisualState
      // in the containing scope. Keep the wreck's current-frame coast before
      // any finale deactivates it.
      NovaShip_IntegrateNpcMovement(state, ship, *cls, elapsed_ticks);
      ship.destruction_raw_tick_accumulator +=
          std::max(0.0F, RawSpaceflightCallTicks(elapsed_ticks));
      // Clean-room scheduler: the accumulator does not model an original
      // ShipState field. It decouples the original discrete per-call body from
      // SDL presentation frequency.
      std::uint16_t raw_counter_bits =
          std::bit_cast<std::uint16_t>(state.shot_guidance_frame_counter);
      while (ship.is_active &&
             ship.destruction_raw_tick_accumulator + 1.0e-6F >= 1.0F) {
        ship.destruction_raw_tick_accumulator -= 1.0F;
        ship.destruction_raw_tick_accumulator =
            std::max(0.0F, ship.destruction_raw_tick_accumulator);
        if (ship.death_timer_active > 0.0F) {
          ship.death_timer_active -= 1.0F;
          TickShipHandleDestructionDebrisPuffs(
              state, ship, std::bit_cast<std::int16_t>(raw_counter_bits));
        }
        NovaShip_TickDestroyedShipVisualStateRawCall(state, ship);
        raw_counter_bits = static_cast<std::uint16_t>(raw_counter_bits + 1U);
      }
      continue;
    }

    NovaShip_IntegrateNpcMovement(state, ship, *cls, elapsed_ticks);

    NovaWeapon_TickNpcWeaponBanks(ship, elapsed_ticks);
    // Ship_HandleShip hands a latched active bank to Weapon_FireShipWeapons.
    NovaWeapon_FireNpcWeaponBank(state, ship);

    // Mission-hail ladder (0x00433050 inline block, after the shield/armor
    // recharge in the original's ordering).
    Mission_TickShipHailLadder(
        state,
        ship,
        static_cast<std::uint32_t>(state.gameplay_now_ms * 60 / 1000));

    // Ionization decay tail (0x0043373f/0x00434394) plus the ionized-velocity
    // ramp. Ship_ComputeShipEffectiveMaxSpeed (0x004642e0) is
    // ionization-independent (it never calls Ship_GetIonizationIntensity), so
    // computing it here before the decay is equivalent to the original's
    // post-decay call at 0x004343d5.
    spaceflight_detail::NovaShip_UpdateIonizationCharge(
        state,
        ship,
        NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, ship, *cls),
        elapsed_ticks);
  }

  // Ship_UpdateVisualState (0x00428340) destruction pass: runs after every
  // Ship_HandleShip integration (Frame_TickSystems scope order), finishing
  // the death presentation of destroyed hulls and applying the blast/mission
  // bookkeeping exactly once as each wreck expires. The cloak-fade slice of
  // the same updater ticks for every active hull (the player's fade runs in
  // PlayerTick_InteractionCloakAndStatus).
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.current_system_id != current_system) {
      continue;
    }
    NovaShip_TickCloakFadeState(state, ship, elapsed_ticks);
    // Ship_UpdateVisualState's weapon-flash fade + running-lights blink run
    // for every active hull each frame (the same per-ship pass).
    NovaShip_TickWeaponSpriteAndRunningLights(state, ship, elapsed_ticks);
    // Ship_UpdateVisualState's sprite-frame composition (base row-select state
    // machines + the AltImageID overlay cycle).
    NovaShip_TickSpriteAnimation(state, ship, elapsed_ticks);
    // Destroyed NPCs run their visual slice inline with Ship_HandleShip above
    // so each ship preserves the original handler -> visual ordering.
  }
}

void Stub_MiscHandlers(GameState &state, bool run_full_tick) {
  // Ghidra Frame_TickSystems scope 8 itself runs on reduced ticks too; its
  // individual members decide whether frozen gameplay advances. This port has
  // no separate frozen-state latch, and its only reduced-tick caller is the
  // frozen transition path, so run_full_tick is presently the scheduling
  // surrogate. The reimplemented member here is
  // Stellar_HandleShipStellarCrash (0x0043aed0), immediately after the
  // gravity pull in the original ordering. Stellar animation itself lives in
  // the SDL view (AdvanceStellarAnimation).
  if (run_full_tick) {
    // Scope 8 order around the stellar passes: Stellar_UpdateStellarSprites
    // (renderer), Stellar_TickStellarDefenseBatteries (0x0042d890), then the
    // gravity pull / fatal crash passes. The ambient sprite update is owned by
    // the SDL view.
    NovaStellar_TickStellarDefenseBatteries(state);
    NovaStellar_HandleShipStellarCrash(state);
  }
}

void Stub_BeamHitQueue(GameState &state, float elapsed_ticks) {
  NovaWeapon_TickBeamHitQueue(state, elapsed_ticks);
}

// Ghidra 0x004186b0 Frame_TickSystems. Reconstructs only the *structure*:
// the scope ordering and the run_full_tick gate. Each scope is a loud stub
// (see above). Called full before drawing and reduced during transitions.
void NovaFrame_TickSystems(GameState &state,
                           SdlAudio &audio,
                           bool run_full_tick,
                           float elapsed_ticks,
                           const NovaPreferences &prefs) {
  // g_avg_frame_tick_scale for this tick: the normalized 30 Hz scale the
  // collision mask-vs-circle decision (Ship_HandleSpritePairCollision
  // 0x004374f0) and other cadence consumers read. NovaWeapon_TickShots (scope
  // 7) re-asserts the same value; this earlier write lets scope 9 collisions
  // see the current tick rather than the previous one.
  state.last_frame_tick_scale = elapsed_ticks > 0.0F ? elapsed_ticks : 0.0F;
  // g_player_disable_message_shown reset site (Ghidra 0x00417669 in the
  // spaceflight frame loop): the latch suppresses the duplicate destruction
  // overlay within one frame only.
  state.player_disable_message_shown = false;
  // scope 10 "player": always runs.
  Stub_PlayerCore(state);
  // Ship_UpdateVisualState (0x00428340) runs on the player immediately after
  // the core in Frame_TickSystems scope 10. For the player this seeds the
  // death presentation (x3, g_player_death_timer_scale 0x00575378), drives the
  // Explode1 debris cascade, and runs the Explode2 finale/boom that deactivates
  // the hull. The live player core runs in the spaceflight loop ahead of this
  // call, so the scope-10 ordering is preserved.
  NovaShip_TickDestroyedShipVisualState(state, state.player, elapsed_ticks);
  // The player's Ship_UpdateVisualState weapon-flash fade and running-lights
  // blink (scope 10).
  if (state.player.is_active) {
    NovaShip_TickWeaponSpriteAndRunningLights(
        state, state.player, elapsed_ticks);
    NovaShip_TickSpriteAnimation(state, state.player, elapsed_ticks);
  }
  // scope 9 "collisions": always runs.
  Stub_Collisions(state);

  if (run_full_tick) {
    Stub_DrawStatus(
        state); // Full-tick proximity-scan roll (original scope 0xc).
    Stub_TickReactionsAndNpcSpawns(state, audio, elapsed_ticks); // scope 0xb
    // The first original scope-6 pass only refreshes target
    // flags/reacquisition; the full clean-room AI update belongs here, once,
    // after spawning.
    Stub_AiRoutines(state, elapsed_ticks);
    Stub_CalcAiOdds(state); // scope 0x14
  }

  // Always-run scopes that keep advancing during frozen transitions.
  Stub_HandleShots(state, elapsed_ticks, prefs); // scope 7
  Stub_HandleShips(state, elapsed_ticks);        // scope 4/5
  Stub_MiscHandlers(state, run_full_tick);       // scope 8
  Stub_BeamHitQueue(state, elapsed_ticks);
}

// ---------------------------------------------------------------------------
// Explicit splits of Ship_HandlePlayerShipCore (0x0044aa70). The core is one
// Metrowerks-collapsed routine; the loop below is its per-frame dispatch and
// these helpers are its named regions (see the plate comment and the
// PlayerTick_* labels in Ghidra). Each helper cites its anchor block.
//
// Audited synthetic-region inventory. These are CFG regions, not original
// source-level functions; source helpers may combine adjacent regions when
// they share one gameplay concern. Navigation umbrellas are called out rather
// than presented as synthetic regions.
//   velocity matching           0x0044AB85 -> 0x0044AD77
//   player-target validation    0x0044ADA1 -> 0x0044AEFB
//   travel-selection commands   0x0044B7C4 -> 0x0044BAFE
//   nearest-target command      0x0044BE15 -> 0x0044BEB7
//   weapon commands             0x0044BEB7 -> 0x0044C0B1
//   face-target command         0x0044C0B1 -> 0x0044C18A (ported:
//                               PlayerTick_FaceTargetCommand + the
//                               IntegrateMovement auto-turn branch)
//   clear jump-status overlay   0x0044C310 -> 0x0044C31D
//   escort jump warning         0x0044C31D -> 0x0044C4DA
//   hyperspace completion audio 0x0044C75C -> 0x0044C84D
//   turn input                  0x0044C92E -> 0x0044C980
//   normalize heading           0x0044C980 -> 0x0044C9AB
//   afterburner command         0x0044C9AB -> 0x0044CA3D
//   thrust and engine glow      0x0044CA3D -> 0x0044CA6B
//   turn-bank animation         0x0044CA6B -> 0x0044CB99
//   shield/armor regeneration   0x0044CB99 -> 0x0044CCAF
//   hyperspace tunnel accel     0x0044CCAF -> 0x0044CFFE
//   inertialess steering        0x0044CFFE -> 0x0044D05B
//   velocity-cap clamp          0x0044D05B -> 0x0044D0C3
//   timed-action transition     0x0044D490 -> 0x0044D560
//   route-map click              0x0044E035 -> [0x0044BC1E, 0x0044E490]
//   jump arrival                0x0044FA72 -> 0x0044FD2E
//   ionization/fuel regen       0x00450717 -> 0x004507B4
//   cheat commands               0x00450FD9 -> 0x00450FE6
//   eject/escape-pod transition  0x00451024 -> 0x00451630
//   afterburner speed/fuel tail  0x00451630 -> 0x004518EF
//   self-destruct state machine 0x00451954 -> 0x00451B91 (ported:
//                               PlayerTick_SelfDestructCommand)
//   special-interaction window  0x00451B91 -> 0x00451C4F (ported:
//                               the P-key Player Info modal, see
//                               docs/player_info_window.md and
//                               src/game/player_info_window.cpp;
//                               jettison execution 0x0041F330 is wired
//                               through the modal's confirmed flag and the
//                               in-flight Alt+cmd dump channel 0x00451907)
//   mission-computer window     0x00451C87 -> 0x00451DB0 (ported in the loop's
//                               mission_info block)
//   cloak-toggle command        0x00451DB0 -> 0x00451E60 (ported:
//                               PlayerTick_InteractionCloakAndStatus)
//   active-cloak upkeep         0x00451E60 -> 0x00451E6F (ported:
//                               PlayerTick_InteractionCloakAndStatus; the
//                               junk-market regeneration 0x00451E6F ->
//                               0x00451F8F is
//                               TODO(decomp): junk resource defs unmodelled)
//   land-command dispatch       0x00451F8F -> 0x004520EA
//   route-map zoom commands     0x0045216E -> 0x004548A7
// PlayerTick_TravelSelectionCommands at 0x0044B7C4 names the clean travel-
// selection slice ending at 0x0044BAFE. Ship cycling, nearest-target selection,
// validation, mouse targeting and later interaction commands are separate.
// PlayerTick_InteractionCloakAndStatus at 0x00451940 is a navigation umbrella,
// not a synthetic-region entry: its body has incoming edges at 0x00451942 and
// 0x00451954 and must be decomposed from valid internal entries.
// ---------------------------------------------------------------------------

// Ghidra Ship_HandlePlayerShipCore synthetic CFG: travel-selection commands
// 0x0044B7C4 -> 0x0044BAFE. The wider 0x0044B7C4 -> 0x0044BC1E umbrella
// absorbs reordered mouse/route-map blocks and is not used. Handles the Tab
// stellar cycle, the clear-target command (slot 8 / slot 0xc), the Backslash
// destination-system cycle and the H hyperspace-mode arm. The edge latches live
// in GameState::command_latches so NovaUi_MarkTravelAndStatusPanelsDirty can
// re-arm them across a modal/mode boundary.
void PlayerTick_TravelSelectionCommands(GameState &state,
                                        const FlightInput &input,
                                        bool arm_modifier_held) {
  PlayerCommandLatches &latches = state.command_latches;
  const bool target_cycle =
      input.cycle_target_next || input.cycle_target_previous;
  if (target_cycle && !latches.target_cycle_was_held) {
    NovaTargeting_CyclePlayerStellarTarget(state, input.cycle_target_next);
  }
  latches.target_cycle_was_held = target_cycle;
  // Clear-target command (Ghidra 0x0044B7C4 primary-ship arm +
  // 0x0044DDD5 travel arm, sharing g_playerClearTargetCommandLatch).
  //   slot 8 + arm modifier -> clear the primary ship target
  //                            (0x0044b804 transition cue 1, +0x70 = -1)
  //   slot 8 (no arm)       -> clear the travel/landing selection and reset
  //                            travel_transfer_mode = -1 / travel slot = -1
  //   slot 0xc              -> clear the travel selection; when not already in
  //                            plotted-jump mode 3, latch mode 3 and re-sync
  //                            the starmap route
  //                            (NovaUi_SyncTravelSelectionFromStarmapRoute).
  // The shared latch means slot 8+arm+slot 0xc clears only the ship target.
  const bool clear_command = input.clear_target;
  const bool hyperspace_command = input.hyperspace_mode;
  if (clear_command && arm_modifier_held && !latches.clear_target_was_held) {
    latches.clear_target_was_held = true;
    if (state.player.primary_target_ship_slot != -1) {
      state.pending_ui_sounds.push_back({1, 1});
      state.player.primary_target_ship_slot = -1;
    }
  }
  const bool travel_clear =
      (clear_command && !arm_modifier_held) || hyperspace_command;
  if (travel_clear && state.player.ai_station_hold_timer <= 0.0F) {
    if (!latches.clear_target_was_held) {
      latches.clear_target_was_held = true;
      // 0x0044ddce queues transition cue 1.
      state.pending_ui_sounds.push_back({1, 1});
      state.travel.selected_stellar_id = -1;
      state.travel.selected_stellar_is_manual = false;
      state.travel.engage_timer = -1;
      if (clear_command) {
        // Slot-8 variant resets the plotted-jump latch to idle.
        state.player.travel_transfer_mode = -1;
        state.travel.travel_slot = -1;
        state.travel.hyperspace_mode = false;
      }
      if (hyperspace_command && state.player.travel_transfer_mode != 3) {
        // Slot-0xc variant latches plotted-jump mode 3 with no slot and re-
        // syncs from the plotted starmap route (0x0044de28).
        state.travel.hyperspace_mode = true;
        state.player.travel_transfer_mode = 3;
        state.travel.travel_slot = -1;
        NovaStarmap_SyncTravelSelectionFromRoute(state);
      }
    }
    if (hyperspace_command) {
      // Ghidra 0x0044b899: the command tail refreshes the route-map overlay
      // (NovaUi_UpdateTravelSelectionOverlay).
      RouteMap_Open(state);
    }
  } else {
    // 0x0044b8b2: the shared latch releases once neither arm is active, and
    // also when the station-hold gate blocks the travel clear.
    latches.clear_target_was_held = false;
  }
  // Destination-SYSTEM cycling (Backslash / Shift+Backslash): rotate the
  // next-jump destination through the systems directly linked to the current
  // one. Mirrors the original's key-binding-13 block in PlayerTick_TargetAnd-
  // TravelCommands (g_playerCycleTravelTargetCommandLatch at
  // 0x0044b8b9..0x0044def6). Edge-latched so held-\ steps one system per
  // press. Setting a destination arms travel mode but does NOT engage the
  // jump -- that stays on the 'j' travel key (NovaTravel_Tick).
  const bool destination_cycle =
      input.cycle_destination_next || input.cycle_destination_previous;
  if (destination_cycle && !latches.destination_cycle_was_held) {
    const std::int16_t dest =
        NovaTravel_CycleDestinationSystem(state, input.cycle_destination_next);
    // Ghidra 0x0044b8ab: the core refreshes the route-map overlay after the
    // destination command (NovaUi_UpdateTravelSelectionOverlay) -- the
    // overlay opens/re-stamps regardless of whether a candidate was found.
    RouteMap_Open(state);
    if (dest >= 0) {
      // Destination cycled; travel mode is armed but the jump awaits 'j'.
      NovaLog::Info("backslash: destination system {}", dest);
    } else {
      NovaLog::Info("backslash: no travelable destination from this system");
    }
  }
  latches.destination_cycle_was_held = destination_cycle;
}

// Ghidra Ship_HandlePlayerShipCore disjoint ship-target command blocks. The
// nearest-target command is the clean synthetic CFG 0x0044BE15 -> 0x0044BEB7;
// the backquote cycle is embedded in the reordered mouse/target envelope.
// Per-frame validation remains a separate earlier call at 0x0044ADA1.
void PlayerTick_ShipTargetCommands(GameState &state, const FlightInput &input) {
  PlayerCommandLatches &latches = state.command_latches;
  // Ship-target cycling: backquote (`) / Shift+backquote, with the
  // player-squad-only modifier (raw DIK 0x1d/0x6b = Left/Right Ctrl). Mirrors
  // the original's Ship_HandlePlayerShip cycle-target block (0x0044b120): a
  // no-op result or a self-result clears the target, otherwise the new slot is
  // stored and the reticle pulse is re-armed at 256.0 (0x43800000).
  const bool ship_cycle =
      input.cycle_ship_target_next || input.cycle_ship_target_previous;
  if (ship_cycle && !latches.ship_cycle_was_held) {
    const std::int16_t next = input.cycle_ship_target_next
                                  ? NovaTargeting_FindNextPlayerCycleTarget(
                                        state,
                                        state.player.primary_target_ship_slot,
                                        state.player.current_system_id,
                                        input.cycle_ship_escorts)
                                  : NovaTargeting_FindPreviousPlayerCycleTarget(
                                        state,
                                        state.player.primary_target_ship_slot,
                                        state.player.current_system_id,
                                        input.cycle_ship_escorts);
    if (next == state.player.primary_target_ship_slot ||
        next == state.player.ship_instance_id) {
      state.player.primary_target_ship_slot = -1;
    } else {
      state.player.primary_target_ship_slot = next;
      state.ship_reticle_pulse = 256.0F;
    }
  }
  latches.ship_cycle_was_held = ship_cycle;
  // "Target nearest" command: 'o' selects the nearest hostile combat target
  // (Ship_SelectNearestHostileCombatTarget 0x00462bd0), Alt+'o' the nearest
  // engaged target (0x00462850). A miss (-1) leaves the current target
  // untouched; a new slot re-arms the reticle pulse.
  const bool nearest_pressed =
      input.select_nearest_hostile || input.select_nearest_engaged;
  if (nearest_pressed && !latches.nearest_target_was_held) {
    const std::int16_t slot =
        input.select_nearest_hostile
            ? NovaTargeting_SelectNearestHostileCombatTarget(state)
            : NovaTargeting_SelectNearestEngagedTarget(state);
    if (slot != -1 && slot != state.player.primary_target_ship_slot) {
      state.player.primary_target_ship_slot = slot;
      state.ship_reticle_pulse = 256.0F;
    }
  }
  latches.nearest_target_was_held = nearest_pressed;
}

} // namespace

// Ghidra 0x0045c7a0 NovaUi_MarkTravelAndStatusPanelsDirty. The name is a
// misnomer: it does not mark panels dirty, it sets every player-command edge
// latch (the 0x007cab35..0x007cab53 swath) so a key held across a modal or mode
// transition must be released and re-pressed before its command fires again.
// Original call sites: the launch tail (0x00456128), Ship_HandlePlayerShipCore
// modal arms (0x0044afa9/0x0044bd2d/0x00451c41/0x00451daa),
// Ship_HandlePlayerTargetActionCommand (0x004549fd..), the hypergate/wormhole
// transfers, the boarding/plunder window and the escort-management window. The
// port wires the launch tail, the flight-loop entry, the starmap, mission
// computer, mission-ship interaction, ship-comm, destination-interaction,
// boarding/plunder, Player Info and hypergate-map returns; the wormhole path
// has no blocking modal.
void NovaUi_MarkTravelAndStatusPanelsDirty(GameState &state) {
  PlayerCommandLatches &latches = state.command_latches;
  latches.target_cycle_was_held = true;
  latches.destination_cycle_was_held = true;
  latches.clear_target_was_held = true;
  latches.ship_cycle_was_held = true;
  latches.nearest_target_was_held = true;
  latches.secondary_cycle_was_held = true;
  latches.clear_secondary_was_held = true;
  latches.starmap_was_held = true;
  latches.mission_info_was_held = true;
  latches.land_was_held = true;
  latches.dismiss_was_held = true;
  latches.target_action_was_held = true;
  latches.board_was_held = true;
  latches.return_to_menu_was_held = true;
  // g_playerDisableSurrenderCommandLatch (cloak toggle) and
  // g_playerRouteMapZoomCommandLatch. The original also arms
  // g_playerFpsToggleCommandLatch and the not-yet-modelled
  // DAT_007cab38/3b/3c/3d/3e/3f/40/41/43/49 slots; those commands have no port
  // consumer to swallow.
  state.cloak_command_latch = 1;
  state.route_map.zoom_command_latch = true;
  // g_target_category_key_latch[5] / g_escort_command_key_latch[4] /
  // DAT_007cab49 (the escort panel toggle); the original arms all three.
  state.escort.select_key_latch.fill(1);
  state.escort.order_key_latch.fill(1);
  state.escort.panel_toggle_latch = 1;
}

namespace {

// Ghidra Ship_HandlePlayerShipCore navigation region
// PlayerTick_MouseTargetAndControlCommands 0x0044E019. Route-map clicks first
// enter the clean multi-exit subregion 0x0044E035 ->
// [0x0044BC1E, 0x0044E490]; unconsumed clicks then follow the original
// self/ship/stellar hit-test order.
void PlayerTick_MouseTargetAndControlCommands(SdlPlatform &platform,
                                              SpaceflightView &view,
                                              GameState &state,
                                              const FlightInput &input) {
  if (!input.primary_clicked) {
    return;
  }
  const RouteMapClickResult route_map_click =
      RouteMap_HandleClick(state,
                           platform,
                           static_cast<float>(input.mouse_x),
                           static_cast<float>(input.mouse_y));
  if (route_map_click != RouteMapClickResult::kNotHandled &&
      route_map_click != RouteMapClickResult::kOutside) {
    return;
  }

  const std::int16_t pre_click_target = state.player.primary_target_ship_slot;
  if (view.ClickInPlayerSprite(platform, state, input.mouse_x, input.mouse_y)) {
    state.player.primary_target_ship_slot = -1;
  }
  const std::int16_t picked =
      view.PickShipAt(platform, state, input.mouse_x, input.mouse_y);
  if (picked != -1) {
    state.player.primary_target_ship_slot = picked;
    state.ship_reticle_pulse = 256.0F;
  }
  if (state.player.primary_target_ship_slot != pre_click_target) {
    return;
  }
  const std::int16_t stellar =
      view.PickStellarAt(platform, state, input.mouse_x, input.mouse_y);
  if (stellar >= 0x80) {
    const bool changed = stellar != state.travel.selected_stellar_id;
    state.travel.selected_stellar_id = stellar;
    state.travel.selected_stellar_is_manual = true;
    // A system arrival resets this to -1.  Selecting a stellar must restore
    // mode 2 or NovaUi_UpdateTravelTargetReticle deliberately hides it.
    state.player.travel_transfer_mode = 2;
    if (changed) {
      state.travel_reticle_pulse = 256.0F;
    }
  }
}

// Ghidra 0x0044aa70 PlayerTick_JumpArrivalBlock (0x0044fa72 -> [0x0044fd2e],
// synthetic plan score 0.76): cross-system jump arrival -- asteroid/starfield
// re-init, the vacant-ship sweep, escort adoption + mission-fleet restoration
// inside the -999 station-hold window, offering rerolls, the scattered NPC
// population and the arrival ambush, then the target/selection reset. The
// entering-system arrival message and the escort travel-day daily tick
// (0x0044fb2d) of the original slice remain TODO(decomp) inside; the
// stat-modifier jitter/reroll pair now runs here (0x0044fa10/0x0044fa15).
void PlayerTick_JumpArrivalBlock(SdlPlatform &platform,
                                 SpaceflightView &view,
                                 HudRenderer &hud,
                                 GameState &state,
                                 std::uint64_t now_ms) {
  // When a jump completed this frame, re-spawn the starfield for the new
  // system (the original's jump completion re-runs
  // NovaEffects_QueuedAmbientStarParticles).
  if (!state.travel.just_completed) {
    return;
  }
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
  // Escort adoption (System_RebuildInitialNpcAndMissionPopulation
  // 0x0041af90, reached from the jump-arrival block at 0x0044fa91):
  // attached ships (squad_leader_ship_slot == 0) that survived the sweep
  // are adopted into the arrival system (Ship_ResetShipToDefaultCombatState
  // 0x0041e240, flag = 0: no refill on a jump), the wedge snaps around
  // the player, and because the player core windows its station-hold
  // timer at -999 around the rebuild (0x0044fa83 / 0x0044faa2) each
  // attached ship is pushed ~892 px behind and flung forward at 50 px/tick
  // -- escorts stream in behind the jumping player.
  state.player.ai_station_hold_timer = -999.0F;
  // Ghidra 0x0044fa10/0x0044fa15: the arrival random-walks the first stat
  // modifier pair and rerolls the second before the mission spawn refresh
  // (travel-day world ticks already ran in FireJump).
  NovaFrame_JitterPlayerStatModifiers(state);
  NovaFrame_RerollPlayerStatModifiers(state);
  // Ghidra 0x0044fa1a: after the travel-day/stat refresh and before population
  // restoration, arm ShipStart-1 mission fleets for their delayed jump-in.
  Mission_RefreshActiveMissionSpawnState(state);
  NovaSystem_RestorePlayerEscorts(state, /*refill=*/false, now_ms);
  NovaSystem_RestoreMissionFleets(state,
                                  state.player.current_system_id,
                                  /*copy_player_heading=*/false,
                                  platform.gameplay_ticks_ms());
  // Offering rolls redraw on every system arrival (Stellar_ProcessTravel
  // AndLanding 0x00458802: roll 1..100 per definition, then re-evaluate
  // the mission lists).
  Mission_RerollOfferingRolls(state);
  NovaSystem_PopulateInitialNpcShips(state, state.player.current_system_id);
  // Mission_TrySpawnMissionShipAmbush (0x00426dd0) runs at the tail of
  // Stellar_HandleStellarEntryAndExit's system-transition slice, after the
  // population rebuild. TODO(decomp): the follow-player ShipBehav 0 fleet
  // jump-in arm of that slice is not reconstructed yet; the refresh/rearm
  // pass above is live.
  Mission_TrySpawnMissionShipAmbush(state);
  // 0x0044faa2: the -999 hold-timer window closes right after the
  // rebuild returns.
  state.player.ai_station_hold_timer = 0.0F;
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
  // Arrival reset (0x0044f803): the landing/docking approach timer is wiped.
  state.travel.engage_timer = -1;
  NovaTravel_ProcessArrivalPayroll(state, [&](const std::string &text) {
    NovaUi_RunTextReaderDialog(platform, state, text, false, [&] {
      view.DrawGameFrame(platform, state, hud);
    });
  });
}

// Loop-exit result of PlayerTick_LandCommandDispatch: the Spaceport modal can
// quit the app or block the frame (the loop freezes gameplay time across it).
enum class LandCommandResult {
  kContinue,
  kQuit,
  kBlockedFrame,
};

// Ghidra 0x00462410 System_GetCurrentSystemLinkSpriteWidth (landing use): the
// landing envelope reads Sprite_GetFrameFullWidth (0x00462390) on the target's
// link_a spin set, i.e. the full frame width (right - left of the sprite's
// placed bounds +0x20/+0x1c). The gate (Stellar_HandleStellarEntryAndExit
// 0x00458e33/0x00458f7d) applies a two-tier fallback before this runs:
//   - the stellar's ambient sprite (StellarDef+0x0) not prepared -> the gate
//     uses 0x4b (75) directly, without calling this;
//   - otherwise this runs and, when the link_a spin set is missing/unprepared
//     (an active stellar can be displaying its link_b set) or the stellar is
//     not in the current system, logs and returns 0x96 (150).
// The port approximates "ambient sprite prepared" as "the stellar's displayed
// spin set resolves" (link_b when active, else link_a). Returns 0 only for
// the first tier; the 0x96 tier returns 150 so the envelope becomes
// round(150 * 1.75) = 262 as in the original.
std::int16_t StellarArrivalSpriteFullWidth(SdlPlatform &platform,
                                           SpaceflightView &view,
                                           const GameState &state,
                                           std::int16_t stellar_id) {
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr) {
    return 0;
  }
  // Tier 1: no prepared ambient sprite -> the gate's own 0x4b fallback.
  const std::int16_t displayed_link =
      NovaTargeting_StellarSpriteLinkId(*stellar);
  if (displayed_link < 0 || displayed_link > 0xff) {
    return 0;
  }
  const SpriteAsset *displayed = view.sprite_store().Spin(
      platform.renderer(), static_cast<std::uint16_t>(displayed_link + 1000));
  if (displayed == nullptr || displayed->frames.empty() ||
      displayed->tile_width <= 0) {
    return 0;
  }
  // Tier 2: System_GetCurrentSystemLinkSpriteWidth on link_a.
  if (stellar->link_a_id < 0 || stellar->link_a_id > 0xff ||
      stellar->system_id != state.player.current_system_id) {
    NovaLog::Info("stellar {} link half-span fallback 0x96: link_a_id={}, "
                  "system {} vs current {}",
                  stellar_id,
                  stellar->link_a_id,
                  stellar->system_id,
                  state.player.current_system_id);
    return 0x96;
  }
  const SpriteAsset *spin = view.sprite_store().Spin(
      platform.renderer(),
      static_cast<std::uint16_t>(stellar->link_a_id + 1000));
  if (spin == nullptr || spin->frames.empty() || spin->tile_width <= 0) {
    NovaLog::Info("stellar {} link_a spin set {} unavailable; half-span "
                  "fallback 0x96",
                  stellar_id,
                  stellar->link_a_id + 1000);
    return 0x96;
  }
  return static_cast<std::int16_t>(spin->tile_width);
}

// Ghidra 0x0044aa70 PlayerTick_LandCommandDispatch (0x00451f8f ->
// [0x004520ea], synthetic plan score 0.98): the land command (binding 5) with
// its nearest-available-stellar auto-pick and the arrival checks opening the
// Spaceport. Divergence TODO(decomp): the original re-picks the nearest
// available travel stellar every 60 frames while travel mode is armed
// (DAT_007cab1c < 2, g_license_check_frame_counter gate in the decompile);
// the port auto-picks only once per press when nothing is targeted.
LandCommandResult PlayerTick_LandCommandDispatch(SdlPlatform &platform,
                                                 SdlAudio &audio,
                                                 SpaceflightView &view,
                                                 HudRenderer &hud,
                                                 GameState &state,
                                                 const NovaPreferences &prefs) {
  // The original's land command (binding 5 in Ship_HandlePlayerShipCore
  // 0x0044aa70) auto-picks the nearest available travel stellar when no
  // stellar is currently targeted (travel_transfer_mode != 2 or
  // ai_secondary_target_slot == -1) before running the arrival checks.
  if (state.travel.selected_stellar_id < 0) {
    const std::int16_t nearest =
        NovaTargeting_FindNearestAvailableTravelStellar(state);
    if (nearest >= 0x80) {
      state.travel.selected_stellar_id = nearest;
      state.player.travel_transfer_mode = 2;
      state.travel_reticle_pulse = 256.0F;
    }
  }
  const std::int16_t target_sprite_full_width = StellarArrivalSpriteFullWidth(
      platform, view, state, state.travel.selected_stellar_id);
  // A freshly selected stellar has not been approached yet. The original's
  // travel-arm branch (0x00459160) initialises g_travel_engage_timer and
  // returns without running the arrival gate; NovaUi_UpdateTravelEngagement-
  // Progress arms it on the next tick. Avoid a misleading "too far" denial.
  if (state.travel.engage_timer < 0) {
    state.travel.engage_timer = 0;
    return LandCommandResult::kContinue;
  }
  const Stellar *target =
      state.scenario.Stellar(state.travel.selected_stellar_id);
  if (target != nullptr && (target->availability_flags & 0x3000U) != 0U) {
    if (!NovaTravel_PlayerMeetsStellarAccess(
            state, state.travel.selected_stellar_id)) {
      const std::uint16_t entry =
          (target->availability_flags & Stellar::kHypergate) != 0 ? 0x51 : 0x53;
      NovaHud_ShowOverlayMessage(
          state,
          NovaHud_LoadStringEntry(0x7d2, entry)
              .value_or((target->availability_flags & Stellar::kHypergate) != 0
                            ? "You are not authorized to use this hypergate."
                            : "You are not authorized to use this stellar."),
          0xfa,
          0x00,
          0x0c,
          0xf0U);
      state.pending_ui_sounds.push_back({1, 1});
      state.travel.engage_timer = -1;
      state.travel.selected_stellar_id = -1;
      return LandCommandResult::kContinue;
    }
    const float arrival_axis_range =
        Stellar_MaxLandingDistance(target_sprite_full_width);
    const bool within_envelope =
        std::abs(state.player.pos_x - static_cast<float>(target->pos_x)) <
            arrival_axis_range &&
        std::abs(state.player.pos_y - static_cast<float>(target->pos_y)) <
            arrival_axis_range;
    if (!within_envelope || state.travel.engage_timer < 0x2ee) {
      NovaHud_ShowLandingDenial(
          state, LandedDenial::kTooFar, /*is_station=*/false);
      return LandCommandResult::kContinue;
    }
    if (std::abs(state.player.vel_x) > 0.75F ||
        std::abs(state.player.vel_y) > 0.75F ||
        state.player.ai_maneuver_timer_ms > 0.0F) {
      NovaHud_ShowLandingDenial(
          state, LandedDenial::kTooFast, /*is_station=*/false);
      return LandCommandResult::kContinue;
    }

    const std::int16_t source = state.travel.selected_stellar_id;
    std::int16_t destination = -1;
    RestrictedTravelKind kind = RestrictedTravelKind::kWormhole;
    if ((target->availability_flags & 0x1000U) != 0U) {
      // Ghidra 0x00456480 Stellar_EnterHypergate: the original opens the
      // galaxy map in linked-destination mode, then accepts only a system
      // reached by the source's HyperLink1-8 table.
      kind = RestrictedTravelKind::kHypergate;
      const StarmapResult map = NovaStarmap_RunWindow(
          platform, state, state.player.current_system_id, &view, nullptr);
      if (map.exit == StarmapExit::kQuit) {
        return LandCommandResult::kQuit;
      }
      // Stellar_EnterHypergate's map return (0x00456480) arms the player
      // command latches like an ordinary starmap return, so a held Escape must
      // be released before it can re-fire.
      NovaUi_MarkTravelAndStatusPanelsDirty(state);
      destination = NovaTravel_ResolveHypergateDestination(
          state, source, map.destination_system_id);
      if (destination < 0) {
        NovaHud_ShowOverlayMessage(
            state,
            NovaHud_LoadStringEntry(0x7d2, 0x32)
                .value_or("No hypergate destination selected."),
            static_cast<std::uint64_t>(0xfaU));
        return LandCommandResult::kBlockedFrame;
      }
    } else {
      destination = NovaTravel_SelectWormholeDestination(state, source);
      if (destination < 0) {
        std::string message =
            NovaHud_LoadStringEntry(0x7d2, 0x54).value_or("Unable");
        message += " ";
        message += NovaHud_LoadStringEntry(0x7d2, 0x56)
                       .value_or("to use this wormhole.");
        NovaHud_ShowOverlayMessage(
            state, message, static_cast<std::uint64_t>(0xfaU));
        state.pending_ui_sounds.push_back({3, 1});
        return LandCommandResult::kContinue;
      }
    }
    if (!NovaTravel_CompleteRestrictedTravel(state, destination, kind)) {
      NovaLog::Warn("restricted travel from stellar {} selected invalid "
                    "destination {}",
                    source,
                    destination);
      return LandCommandResult::kContinue;
    }
    PlayerTick_JumpArrivalBlock(
        platform, view, hud, state, platform.gameplay_ticks_ms());
    return LandCommandResult::kBlockedFrame;
  }
  LandedContext ctx;
  if (Stellar_Dock(state, ctx, target_sprite_full_width)) {
    NovaLog::Info("arrival accepted at stellar {}; opening Spaceport",
                  ctx.stellar_id);
    // Mission resolution (Mission_TickReactionSlotsForTravelInteraction
    // 0x00443780, including the success/failure debrief readers) runs
    // once on Spaceport entry inside NovaLanded_RunWindow, matching its
    // position in NovaUi_RunTravelDestinationInteractionLoop
    // (0x00491f30): after the window is up, before the AvailLoc-3 offer
    // pass, with debriefs layered over the dock.
    const LandedExit exit =
        NovaLanded_RunWindow(platform, audio, state, ctx, prefs, hud);
    if (exit == LandedExit::kQuit) {
      return LandCommandResult::kQuit;
    }
    // Launch runs the launch tail below (which contains the original's
    // single daily world tick, 0x00456033).
    // TODO(decomp(0x0044d870)) skipped: the 15..44-day daily-driver loop
    // belongs to the death/escape-pod respawn (PlayerTick_TimedAction-
    // Transition 0x0044d490), not to launch -- previously misattributed
    // here and charging 16..45 days per landing.
    if (exit == LandedExit::kLaunched) {
      // Stellar_RunDockAndLaunchSequence launch tail (0x00455f99..0x00456268):
      // velocity/position reset, shield/armor refill, the daily world
      // tick, stat-modifier jitter/reroll, autosave, random launch
      // heading, travel-selection reset and the shot wipe.
      Stellar_Launch(state);
      // The docked modal advances the probe's virtual frame clock. Refresh
      // the snapshot before arming the departure overlay so its deadline is
      // relative to the actual launch instant rather than modal entry.
      state.gameplay_now_ms = platform.gameplay_ticks_ms();
      // Stellar_RunDockAndLaunchSequence tail (0x00456323): the "leaving
      // <stellar> on <date>" overlay shows as the player departs.
      NovaHud_ShowLaunchDepartureMessage(state, ctx.stellar_id);
    }
    // Docking blocked the loop for the whole landing; freeze gameplay
    // time across it (launch re-enters flight with a fresh clock).
    return LandCommandResult::kBlockedFrame;
  }
  const auto *st = state.scenario.Stellar(state.travel.selected_stellar_id);
  const bool is_station = st != nullptr && (st->flags & 0x10U) != 0U;
  NovaHud_ShowLandingDenial(state, ctx.denial, is_station);
  if (ctx.denial == LandedDenial::kUnauthorized) {
    // 0x00458708: a failed authorization attempt disarms the approach and
    // clears the selected stellar after presenting the dedicated denial.
    state.pending_ui_sounds.push_back({1, 1});
    state.travel.engage_timer = -1;
    state.travel.selected_stellar_id = -1;
    state.travel.selected_stellar_is_manual = false;
  }
  return LandCommandResult::kContinue;
}

// Ghidra 0x00451f8f PlayerTick_FlightTutorialHints (synthetic CFG 0x00451f8f ->
// [0x004520ea]): the fresh-pilot flight tutorial, emitted once a second
// (g_spaceflight_frame_counter % 60 == 0) while the hint state is below 2.
//  -3: near a landing stellar, the "Welcome to Nova ..." welcome (STR# 0x7d2
//      0x16 docking / 0x17 landing lead + stellar name + 0x18 + the land key
//      name + 0x19), then state = -2.
//  -1: after the first launch, the "hyperspace to another system" hint
//      (0x1a + map key + 0x1b + jump key + 0x1c), then state = 0.
//  -2/0/1/2: hyperspace-range hints. Inside the 2,000,000 px^2 "not yet far
//      enough" radius with state <= 0, show 0x1e and latch state = 1; once
//      latched, beyond 10,000,000 px^2 show 0x1f and latch state = 2.
// The range hints are suppressed while the player ship is disabled
// (Ship_IsShipDisabled, stock local_251); the welcome/hyperspace hints are
// not. Each cue queues transition-sound slot 4 and uses a 0x200-tick overlay.
// Key names come from String_ExpandControlCode (0x004f1990); an unbound slot
// expands to the empty string, matching the original's map miss.
void TickFlightTutorialHints(GameState &state, const NovaPreferences &prefs) {
  if (state.travel.travel_hint_state >= 2 ||
      state.spaceflight_frame_counter % 60 != 0) {
    return;
  }
  const float dist_sq = state.player.pos_x * state.player.pos_x +
                        state.player.pos_y * state.player.pos_y;
  const bool disabled = NovaAiShip_IsDisabled(state, state.player);
  const auto load = [](std::uint16_t entry) {
    return NovaHud_LoadStringEntry(0x7d2, entry).value_or(std::string{});
  };
  const auto binding_name = [&](std::size_t slot) {
    const std::uint16_t key = prefs.bindings.cmd_to_key[slot];
    if (key == 0xff || key == 0xffff) {
      return std::string{};
    }
    return NovaPrefs_KeyCodeDisplayName(key);
  };

  if (state.travel.travel_hint_state == -3) {
    const std::string docking = load(0x16);
    const std::string landing = load(0x17);
    if (!docking.empty() && !landing.empty()) {
      const std::int16_t nearest =
          NovaTargeting_FindNearestAvailableTravelStellar(state);
      if (nearest != -1) {
        state.pending_ui_sounds.push_back({4, 1});
        const Stellar *stellar = state.scenario.Stellar(nearest);
        const bool is_station =
            stellar != nullptr && (stellar->flags & 0x10U) != 0U;
        std::string msg = is_station ? docking : landing;
        msg += " ";
        if (stellar != nullptr) {
          msg += stellar->name;
        }
        msg += " ";
        msg += load(0x18);
        msg += binding_name(5); // Land command.
        msg += load(0x19);
        NovaHud_ShowOverlayMessage(
            state, std::move(msg), static_cast<std::uint64_t>(0x200U));
      }
    }
    state.travel.travel_hint_state = -2;
  }

  if (state.travel.travel_hint_state == -1) {
    const std::string intro = load(0x1a);
    if (!intro.empty()) {
      const std::int16_t nearest =
          NovaTargeting_FindNearestAvailableTravelStellar(state);
      if (nearest != -1) {
        state.pending_ui_sounds.push_back({4, 1});
        std::string msg = intro;
        msg += binding_name(9); // Starmap command.
        msg += load(0x1b);
        msg += binding_name(0x0e); // Travel/jump command.
        msg += load(0x1c);
        NovaHud_ShowOverlayMessage(
            state, std::move(msg), static_cast<std::uint64_t>(0x200U));
      }
    }
    state.travel.travel_hint_state = 0;
  }

  constexpr float kNotYetFarEnoughSq = 2000000.0F; // 0x00575650
  constexpr float kSafeRangeSq = 10000000.0F;      // 0x00575658
  if (dist_sq <= kNotYetFarEnoughSq || state.travel.travel_hint_state > 0) {
    if (dist_sq > kSafeRangeSq && state.travel.travel_hint_state == 1 &&
        !disabled) {
      const std::string text = load(0x1f);
      if (!text.empty()) {
        state.pending_ui_sounds.push_back({4, 1});
        NovaHud_ShowOverlayMessage(
            state, text, static_cast<std::uint64_t>(0x200U));
        state.travel.travel_hint_state = 2;
      }
    }
  } else if (!disabled) {
    const std::string text = load(0x1e);
    if (!text.empty()) {
      state.pending_ui_sounds.push_back({4, 1});
      NovaHud_ShowOverlayMessage(
          state, text, static_cast<std::uint64_t>(0x200U));
      state.travel.travel_hint_state = 1;
    }
  }
}

// Ghidra 0x0044aa70 self-destruct block (0x00451954 -> 0x00451b91, internal
// label of the PlayerTick_InteractionCloakAndStatus umbrella). The command is
// the 0x38/0x6f arm-modifier pair (the port reads both Alt scancodes, as the
// board/eject commands do) plus key binding 0x12 (default DIK 0x0c, minus).
// Arm, while disarmed (< 0) and not destroyed: 150.0 countdown, the "Self
// destruct armed." overlay (STR# 0x7d2 0x181 + 0x182, 0x28 ticks) and the
// transition-table 3 cue. While armed, releasing the command aborts (0x181 +
// 0x183 overlay, table 1 cue); holding it decrements the countdown by the
// frame tick scale, and on every 30th-tick boundary at or below 120.0
// (DAT_00575688) beeps (table 2) and posts the remaining whole seconds
// (round(countdown / 30.0), STR# 0x7d2 0x181 + 0x18b + 0x185 + ".", 0x46
// ticks). At countdown <= 1.0 (g_jump_turnaround_turn_rate_addend
// 0x0057555c) with the command still held the ship detonates: shields 0,
// armor -1 (the death/respawn path takes over), primary target cleared, and
// every hull docked to the player in control mode 0xf
// (Ship_IsShipDockedWithTargetInControlMode0x0F 0x00415b00: targeting the
// player, |dx|/|dy| <= 16.0, DAT_00575090) gets armor -1.0 as well.
// The armor-panel dirty flags the original sets on the 10-frame cadence are
// implied by the port's immediate-mode HUD.
void PlayerTick_SelfDestructCommand(GameState &state,
                                    bool self_destruct_held,
                                    float elapsed_ticks) {
  Ship &p = state.player;
  float &countdown = state.player_self_destruct_countdown;
  if (self_destruct_held && countdown < 0.0F && !NovaAiShip_IsDestroyed(p)) {
    state.pending_ui_sounds.push_back({3, 0xf});
    std::string text =
        NovaHud_LoadStringEntry(0x7d2, 0x181).value_or("Self destruct");
    text += " ";
    text += NovaHud_LoadStringEntry(0x7d2, 0x182).value_or("armed.");
    NovaHud_ShowOverlayMessage(state, text, static_cast<std::uint64_t>(0x28));
    countdown = 150.0F;
  }
  // Armed countdown: abort on release, decay + warn while held. The 30-tick
  // boundary message runs on the truncated tick counter, exactly like the
  // original's trunc(countdown) % 30 == 0 check.
  bool command_block_ran = false;
  if (countdown > 0.0F) {
    command_block_ran = true;
    if (!self_destruct_held) {
      state.pending_ui_sounds.push_back({1, 0xf});
      std::string text =
          NovaHud_LoadStringEntry(0x7d2, 0x181).value_or("Self destruct");
      text += " ";
      text += NovaHud_LoadStringEntry(0x7d2, 0x183).value_or("aborted.");
      NovaHud_ShowOverlayMessage(state, text, static_cast<std::uint64_t>(0x28));
      countdown = -1.0F;
    } else {
      // g_armor_state_addend (0x00575580, 0.0) <= countdown: always true
      // while armed, so the decay runs every held frame.
      countdown -= elapsed_ticks;
      const auto whole_ticks = static_cast<int>(countdown);
      if (countdown <= 120.0F && whole_ticks % 30 == 0) {
        const float seconds =
            countdown / 30.0F; // g_hyperspace_engage_hold_30hz
        const auto whole_seconds = static_cast<int>(seconds);
        std::string text =
            NovaHud_LoadStringEntry(0x7d2, 0x181).value_or("Self destruct");
        text += " ";
        text += NovaHud_LoadStringEntry(0x7d2, 0x18b).value_or("in");
        text += " ";
        text += std::to_string(whole_seconds);
        text += " ";
        text += NovaHud_LoadStringEntry(0x7d2, 0x185).value_or("seconds");
        text += ".";
        state.pending_ui_sounds.push_back({2, 0xf});
        NovaHud_ShowOverlayMessage(
            state, text, static_cast<std::uint64_t>(0x46));
      }
    }
  }
  // Detonation: countdown reached 1.0 (g_jump_turnaround_turn_rate_addend)
  // on a frame the command block ran with the command still held.
  if (countdown <= 1.0F && command_block_ran && self_destruct_held) {
    state.pending_ui_sounds.push_back({1, 0xf});
    const auto text = NovaHud_LoadStringEntry(0x7d2, 0x186);
    NovaHud_ShowOverlayMessage(state,
                               text.value_or("Self destruct activated."),
                               static_cast<std::uint64_t>(0x28));
    p.shield_points = 0.0F;
    p.armor_points = -1.0F;
    p.primary_target_ship_slot = -1;
    // g_shipAvailabilityCachesDirty = 1: the port recomputes availability
    // per query (no cache), so only the docked-fighter sweep matters.
    // Ghidra 0x00415b00 Ship_IsShipDockedWithTargetInControlMode0x0F runs
    // inline here: control mode 0xf, target-is-player (primary or secondary),
    // and |dx|/|dy| within DAT_00575090 = 16.0.
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      Ship &ship = state.ShipAt(slot);
      if (!ship.is_active || ship.ai_control_mode != 0xf) {
        continue;
      }
      const bool targets_player =
          ship.primary_target_ship_slot == p.ship_instance_id ||
          ship.ai_secondary_target_slot == p.ship_instance_id;
      constexpr float kDockedRange = 16.0F; // DAT_00575090
      if (targets_player && std::abs(ship.pos_x - p.pos_x) <= kDockedRange &&
          std::abs(ship.pos_y - p.pos_y) <= kDockedRange) {
        ship.armor_points = -1.0F;
      }
    }
    countdown = -1.0F;
  }
}

// Ghidra 0x0044aa70 cloak command + active-cloak upkeep (0x00451db0 ->
// 0x00451e6f, internal label of the PlayerTick_InteractionCloakAndStatus
// umbrella). Command (binding 0x29, default DIK 0x16 = U, edge-latched through
// g_playerDisableSurrenderCommandLatch): suppressed (latch cleared) while the
// player is destroyed (death_timer_active > 0), disabled
// (ai_station_hold_timer > 0) or fire-restricted (the port's established
// NovaAiShip_IsDisabled approximation of the parent's local fire-restriction
// flag); on the first accepted frame, Ship_CanMaintainCloakState failing
// plays the denied cue (table 3), otherwise the fade flips via
// Ship_OnShipCloakStateEntered/Cleared (cleared when a fade is already
// running toward or sitting in cloak).
// The per-frame upkeep then runs for every active cloak: the visual fade
// slice (NovaShip_TickCloakFadeState), the force-clear when the cloak can no
// longer be maintained, and while the ship sits past the visibility threshold
// the outfit-driven fuel drain (both paths clamp at 0.0) and the shield drop
// (Flags 0x0004 outfit) or per-tick shield drain. Drain scale DAT_00575690 =
// 2.9045e-5 per drain-unit and tick (decoded 0x37f38c54); the panel-dirty
// flags on the 10-frame cadence are implied by the immediate-mode HUD. The
// player's Ship_UpdateVisualState cloak slice is co-located here because the
// port has no per-frame player visual pass.
void PlayerTick_InteractionCloakAndStatus(GameState &state,
                                          bool cloak_command_held,
                                          float elapsed_ticks) {
  Ship &p = state.player;
  const bool destroyed = p.death_timer_active > 0.0F;
  const bool disabled = p.ai_station_hold_timer > 0.0F;
  const bool fire_restricted = NovaAiShip_IsDisabled(state, p);
  if (!cloak_command_held || destroyed || disabled || fire_restricted) {
    state.cloak_command_latch = 0;
  } else if (state.cloak_command_latch == 0) {
    state.cloak_command_latch = 1;
    if (!NovaAiShip_CanMaintainCloakState(state, p)) {
      state.pending_ui_sounds.push_back({3, 1});
    } else if (p.cloak_fade_progress > 0.0F || p.cloak_transition_latch > 0) {
      NovaAi_OnShipCloakStateCleared(p);
    } else {
      NovaAi_OnShipCloakStateEntered(state, p);
    }
  }
  // Active-cloak upkeep.
  NovaShip_TickCloakFadeState(state, p, elapsed_ticks);
  if (NovaTargeting_ShipAtCloakVisibilityThreshold(p)) {
    if (!NovaAiShip_CanMaintainCloakState(state, p) || fire_restricted) {
      NovaAi_OnShipCloakStateCleared(p);
    }
    constexpr float kCloakDrainPerTick = 2.9045e-5F; // DAT_00575690
    const std::int16_t fuel_drain = NovaOutfit_GetCloakFuelDrainFlags(state, p);
    if (fuel_drain > 0) {
      p.fuel_points -=
          static_cast<float>(fuel_drain) * kCloakDrainPerTick * elapsed_ticks;
      if (p.fuel_points <= 0.0F) {
        p.fuel_points = 0.0F;
      }
    }
    if (NovaOutfit_HasCloakShieldDropOnActivation(state, p)) {
      p.shield_points = 0.0F;
    } else {
      const std::int16_t shield_drain =
          NovaOutfit_GetCloakShieldDrainFlags(state, p);
      if (shield_drain > 0 &&
          static_cast<float>(shield_drain) <= p.shield_points) {
        p.shield_points -= static_cast<float>(shield_drain) *
                           kCloakDrainPerTick * elapsed_ticks;
        if (p.shield_points <= 0.0F) {
          p.shield_points = 0.0F;
        }
      }
    }
  }
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
                               bool &returning_to_menu,
                               const NovaPreferences &prefs) {
  SpaceflightView view;
  // The government-specific HUD (Interface layout + cockpit PICT) is resolved
  // once on spaceflight entry (Ui_InstallGameplayInterfaceLayout) and re-
  // composited over the world each frame.
  HudRenderer hud;
  hud.Install(platform, state);
  // The radar resolves stellar blip sizes through the view's sprite store
  // (Sprite_GetFrameFullWidth on each loaded spin set).
  hud.AttachSpriteStore(&view.sprite_store());
  // Resolve the hyperspace flash colour from the preference once: the CE
  // build's 0x00872384 forces black when the raw g_hyperspace_effects byte is
  // non-zero and applies the requested white otherwise. The raw preference
  // byte is inverted relative to the Settings checkbox, so the default
  // (hyperspace_effects == false) is effects-on white. See GameState.
  state.screen_flash_black = NovaPrefs_HyperspaceFlashIsBlack(prefs);

  // Preload the complete gameplay sound handle table before the first frame.
  // The original does this during session setup; limiting the cache to owned
  // weapon fire sounds made impact, cloak, and other cues silent.
  NovaWeapon_PreloadGameplaySounds(state);

  // Keep the small legacy cache warm for callers that address weapon sounds
  // by their 0..35 fire-sound slot.
  NovaWeapon_PreloadOwnedFireSounds(state);
  // Preload the hyperspace jump sounds: snd 128 'Warp up' (the rising
  // 'hyperspace imminent' cue played as the ship accelerates into the jump
  // zoom; snd 129 'Warp up.x2' is the faster noengine variant) and snd 130
  // 'Warp out' (the ~2.5 s boom at the fire/arrival instant). Mirrors
  // FUN_004b0740 preloading the jump handles
  // (NovaSound_LoadDecodedById(0x80/0x81/0x82) -> snd 128/129/130) so the
  // first jump plays them without a decode hitch. Missing resources -> the
  // jump plays silently (travel.cpp falls back to fixed durations).
  //
  // The original also derives each cue's 60 Hz tick length from its
  // SoundHeader here, `numFrames * 60 / sampleRate` (0x004b0a07), keeping the
  // 0x15e = 350 fallback on a missing/broken resource; the decoded clean-room
  // sound is mono, so samples.size() is the frame count.
  const auto sound_duration_60hz = [](const NovaSoundData &sound) {
    if (sound.sample_rate <= 0 || sound.samples.empty()) {
      return std::int16_t{350};
    }
    const std::int64_t ticks = static_cast<std::int64_t>(sound.samples.size()) *
                               60 / sound.sample_rate;
    return static_cast<std::int16_t>(
        std::clamp<std::int64_t>(ticks, 1, 0x7fff));
  };
  if (!state.warp_up_sound.has_value()) {
    if (const auto resource =
            NovaResource_LoadSndData(static_cast<std::uint16_t>(128))) {
      if (auto decoded = NovaSound_Decode(*resource)) {
        state.warp_up_sound = std::move(*decoded);
      }
    }
  }
  if (state.warp_up_sound.has_value()) {
    state.jump_duration_engine_60hz = sound_duration_60hz(*state.warp_up_sound);
  }
  if (!state.warp_up_x2_sound.has_value()) {
    if (const auto resource =
            NovaResource_LoadSndData(static_cast<std::uint16_t>(129))) {
      if (auto decoded = NovaSound_Decode(*resource)) {
        state.warp_up_x2_sound = std::move(*decoded);
      }
    }
  }
  if (state.warp_up_x2_sound.has_value()) {
    state.jump_duration_noengine_60hz =
        sound_duration_60hz(*state.warp_up_x2_sound);
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
  // Publish the live viewport half-size before any asteroid spawn (the
  // original's g_viewport_center_x/y are set by the interface layout pass).
  view.SyncGameplayViewport(platform, state);
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
  state.gameplay_now_ms = platform.gameplay_ticks_ms();
  NovaFrame_TickSystems(state,
                        audio,
                        /*run_full_tick=*/true,
                        /*elapsed_ticks=*/1.0F,
                        prefs);
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
  state.gameplay_now_ms = platform.gameplay_ticks_ms();
  std::uint64_t prev_tick_ms = state.gameplay_now_ms;
  // Player command edge latches (Ghidra 0x007cab35..0x007cab53). They live on
  // GameState so NovaUi_MarkTravelAndStatusPanelsDirty re-arms them at modal
  // and mode boundaries; the loop seeds the travel-selection tracker from the
  // current selection.
  state.command_latches.prev_travel_stellar = state.travel.selected_stellar_id;
  // The original's flight-loop entry calls NovaInputQueue_FlushAllCommands
  // twice (0x00417600), so a command key held across the intro/menu transition
  // is not active on the first frame. The port cannot clear SDL's live
  // keyboard state, so it arms every latch here instead: a held key must be
  // released and re-pressed before its command fires.
  NovaUi_MarkTravelAndStatusPanelsDirty(state);
  // Gameplay time freezes while a blocking modal owns the loop (the original's
  // g_gameplay_time_frozen around interaction windows); every modal return
  // site calls resync_frame_clock() so the wall-clock gap is never integrated
  // as one giant flight frame (ship integration, ambient stars, shield
  // recharge and the reticle decay all scale by frame_time_ms).
  const auto resync_frame_clock = [&] {
    prev_tick_ms = platform.gameplay_ticks_ms();
    state.gameplay_now_ms = prev_tick_ms;
  };
  // Route-map overlay session assets (Ghidra FUN_004ab9d4 creates the
  // surface + CICNs once at flight-interface setup; the port caches the
  // marker textures + label font for the whole flight loop).
  RouteMapView route_map_view;
  route_map_view.Load(platform);
  FlightAutomationController automation;
  // The simulation advances in fixed 30 Hz quanta. Rendering remains the
  // outer loop, so accelerated mode can execute many logical ticks before one
  // presentation without handing AI, travel, or lower-level control routines
  // a render-sized delta.
  float simulation_accumulator_ms = kOriginalTickMs;
  double simulation_tick_ms =
      static_cast<double>(state.gameplay_now_ms) - kOriginalTickMs;
  while (!platform.quit_requested() && !returning_to_menu) {
    const std::uint64_t host_now_ms = platform.gameplay_ticks_ms();
    state.gameplay_now_ms = host_now_ms;
    const float host_frame_time_ms =
        static_cast<float>(host_now_ms - prev_tick_ms);
    prev_tick_ms = host_now_ms;
    // Port stand-in for NovaTime_GetTickCount60Hz's g_frame_tick_count_60hz
    // (see GameState::tick_60hz): re-derived from the wall clock each frame.
    state.tick_60hz = static_cast<std::uint32_t>(host_now_ms * 60ULL / 1000ULL);
    // Keep the gameplay viewport half-size current (g_viewport_center_x/y);
    // asteroid spawn scatters over it.
    view.SyncGameplayViewport(platform, state);
    // Arrival command grace (Ghidra latches g_license_check_frame_counter to
    // -15 in the Stellar_HandleStellarEntryAndExit rebuild epilogue,
    // 0x004586a6): counts down each frame; the gated interaction command blocks
    // skip while it is positive.
    if (state.arrival_command_grace_frames > 0) {
      --state.arrival_command_grace_frames;
    }
    // Player control (heading/throttle) is read here once so the ship flies
    // while the simulation stubs do not, and the same snapshot feeds the
    // travel/jump channel. Movement integrates into PlayerShip.
    FlightInput frame_input = platform.PollFlightInput();
    // Persisted key lookups go through the shared command service so the
    // flight loop and the modal dialogs resolve rebinds identically.
    const auto binding_held = [&platform](std::size_t command) {
      return NovaInput_IsCommandActive(platform, command);
    };
    // Shared modifier pair (Left/Right Shift, DIK 0x2a/0x36) for the backward
    // cycling commands and the eject arm-modifier pair (0x38/0x6f = Alt).
    const bool shift_held = platform.IsOriginalKeyCodeHeld(0x2a) ||
                            platform.IsOriginalKeyCodeHeld(0x36);
    const bool arm_modifier_held = platform.IsOriginalKeyCodeHeld(0x38) ||
                                   platform.IsOriginalKeyCodeHeld(0x6f);
    // Ship_HandlePlayerShipControl reads every action through
    // g_player_key_bindings. PollFlightInput owns the SDL event pump and the
    // clean-room fixed keys (Tab/Ctrl); fill every original binding-table
    // command from the persisted table here.
    frame_input.turn_left = binding_held(0x13);
    frame_input.turn_right = binding_held(0x14);
    frame_input.thrust = binding_held(0x15);
    frame_input.reverse = binding_held(0x16);
    frame_input.afterburner = binding_held(0x18);
    frame_input.fire = binding_held(0x02);
    frame_input.fire_secondary = binding_held(0x03);
    frame_input.cycle_secondary = binding_held(0x00);
    frame_input.cycle_secondary_backwards =
        frame_input.cycle_secondary && shift_held;
    frame_input.clear_secondary = binding_held(0x01);
    frame_input.face_target = binding_held(0x07);
    // Target-action command (PlayerTick_InteractionCloakAndStatus hail
    // dispatch): the original reads g_nova_control_bits[34] -- binding slot
    // 0x04 -- through NovaInput_IsCommandActiveWithGameplayGuards
    // (0x00451c4f). NovaPrefs_ResetKeyBindings defaults it to DIK 0x15 = Y.
    frame_input.target_action = binding_held(0x04);
    // Clear-target command (binding slot 8, default DIK 0x31 = N): primary
    // ship target with the arm modifier, otherwise the travel selection.
    frame_input.clear_target = binding_held(0x08);
    // Land (binding slot 5, default DIK 0x26 = L): auto-pick the nearest
    // available travel stellar and run the arrival checks.
    frame_input.land = binding_held(0x05);
    // HUD/panel dismiss (binding slot 6, default DIK 0x1c = Return): clears
    // the transient HUD message, the route-map overlay and the Escort Commands
    // panel at 0x00450ae7.
    frame_input.dismiss = binding_held(0x06);
    frame_input.travel = binding_held(0x0e);
    frame_input.starmap = binding_held(0x09);
    frame_input.mission_info = binding_held(0x28);
    frame_input.board = binding_held(0x10);
    // Destination-system cycle (binding slot 0x0d, default DIK 0x2b =
    // Backslash), Shift for the backward direction.
    frame_input.cycle_destination_next = binding_held(0x0d) && !shift_held;
    frame_input.cycle_destination_previous = binding_held(0x0d) && shift_held;
    // Hyperspace-mode arm (binding slot 0x0c, default DIK 0x23 = H).
    frame_input.hyperspace_mode = binding_held(0x0c);
    // Ship-target cycle (binding slot 0x0a, default DIK 0x29 = backquote),
    // Shift for the backward direction.
    frame_input.cycle_ship_target_next = binding_held(0x0a) && !shift_held;
    frame_input.cycle_ship_target_previous = binding_held(0x0a) && shift_held;
    // Nearest hostile (binding slot 0x0b, default DIK 0x13 = R) vs. nearest
    // engaged target under the 0x38/0x6f arm modifier.
    frame_input.select_nearest_hostile =
        binding_held(0x0b) && !arm_modifier_held;
    frame_input.select_nearest_engaged =
        binding_held(0x0b) && arm_modifier_held;
    // Eject (binding slot 0x11, default DIK 0x2d = X) plus the arm modifier.
    frame_input.eject = arm_modifier_held && binding_held(0x11);
    // Probe automation is an optional input producer. It runs after both SDL/
    // probe input and persisted bindings have populated the snapshot, but
    // before any player-command edge latch observes it.
    if (const auto request = platform.probe().ConsumeAutomationRequest()) {
      switch (request->kind) {
      case ProbeAutomationRequest::Kind::kLandAt: {
        // Resolve the target's real per-axis arrival envelope from its
        // displayed sprite (Stellar_MaxLandingDistance) so the autopilot stops
        // inside the actual gate rather than a fixed radius.
        float envelope = -1.0F;
        if (const auto resolved = FlightAutomationController::ResolveStellar(
                state, request->target)) {
          envelope = Stellar_MaxLandingDistance(
              StellarArrivalSpriteFullWidth(platform, view, state, *resolved));
        }
        (void)automation.LandAt(
            state, request->target, host_now_ms, request->timeout_ms, envelope);
        break;
      }
      case ProbeAutomationRequest::Kind::kJumpTo:
        (void)automation.JumpTo(
            state, request->target, host_now_ms, request->timeout_ms);
        break;
      case ProbeAutomationRequest::Kind::kDestroyShip:
        (void)automation.DestroyShip(
            state,
            request->target,
            host_now_ms,
            request->timeout_ms,
            static_cast<std::int16_t>(request->ship_id),
            request->allow_missing);
        break;
      case ProbeAutomationRequest::Kind::kCancel:
        automation.Cancel();
        break;
      }
    }
    if (platform.probe().ConsumeAutomationDocked()) {
      automation.ObservedDocked();
    }
    // Ordinary mode preserves the original 21 ms outer-call cadence. Probe
    // acceleration intentionally switches to 30 Hz outer frames to reduce
    // scheduler overhead; raw-call consumers still replay their 21 ms calls
    // internally from the normalized step size.
    const float simulation_quantum_ms =
        platform.accelerated() ? kOriginalTickMs : kOriginalMaxRateFrameMs;
    simulation_accumulator_ms += host_frame_time_ms;
    while (simulation_accumulator_ms >= simulation_quantum_ms) {
      simulation_accumulator_ms -= simulation_quantum_ms;
      simulation_tick_ms += simulation_quantum_ms;
      const std::uint64_t now_ms =
          static_cast<std::uint64_t>(std::max(0.0, simulation_tick_ms));
      const float frame_time_ms = simulation_quantum_ms;
      FlightInput input = frame_input;
      automation.Tick(state, now_ms, input, frame_time_ms / kOriginalTickMs);
      const auto &automation_status = automation.status();
      const auto goal_name = [&] {
        switch (automation_status.goal) {
        case FlightAutomationGoal::kLandAt:
          return "land_at";
        case FlightAutomationGoal::kJumpTo:
          return "jump_to";
        case FlightAutomationGoal::kDestroy:
          return "destroy";
        case FlightAutomationGoal::kNone:
          return "none";
        }
        return "none";
      }();
      const auto phase_name = [&] {
        switch (automation_status.phase) {
        case FlightAutomationPhase::kIdle:
          return "idle";
        case FlightAutomationPhase::kSelect:
          return "select";
        case FlightAutomationPhase::kMove:
          return "move";
        case FlightAutomationPhase::kEngage:
          return "engage";
        case FlightAutomationPhase::kWaiting:
          return "waiting";
        case FlightAutomationPhase::kComplete:
          return "complete";
        case FlightAutomationPhase::kFailed:
          return "failed";
        case FlightAutomationPhase::kCancelled:
          return "cancelled";
        }
        return "failed";
      }();
      // A request accepted by the probe but not yet consumed by the flight loop
      // leaves the controller on the previous goal. Suppress a stale "complete"
      // so a harness waiting on it cannot match the goal that just finished and
      // race ahead of the queued one.
      const char *published_phase =
          platform.probe().HasPendingAutomationRequest() &&
                  automation_status.phase == FlightAutomationPhase::kComplete
              ? "select"
              : phase_name;
      const auto probe_json_escape = [](std::string_view value) {
        // JSON must be valid UTF-8; the target is probe-authored UTF-8 while
        // the detail may embed raw MacRoman game strings.
        const std::string encoded = NovaText_EncodeUtf8(value);
        std::string escaped;
        escaped.reserve(encoded.size());
        for (const char c : encoded) {
          if (c == '"' || c == '\\') {
            escaped.push_back('\\');
          }
          escaped.push_back(c == '\n' || c == '\r' ? ' ' : c);
        }
        return escaped;
      };
      platform.probe().PublishAutomationStatus(
          std::string{"{\"goal\":\""} + goal_name + "\",\"phase\":\"" +
          published_phase + "\",\"target\":\"" +
          probe_json_escape(automation_status.target) + "\",\"detail\":\"" +
          probe_json_escape(automation_status.detail) + "\"}");
      // The original's cancel/skip command (binding slot 0x17, default Escape)
      // returns to the menu, edge-latched through DAT_007cab53 (0x00417b7e):
      // fire once on the leading edge, then clear once the bound key is
      // released. NovaUi_MarkTravelAndStatusPanelsDirty re-arms the latch at
      // the launch tail (0x00456128) and at flight-loop entry, so an Escape
      // held across the dock launch (or the intro skip) does not immediately
      // re-fire here and bounce straight back to the menu.
      const bool cancel_held = binding_held(0x17);
      if (cancel_held) {
        if (!state.command_latches.return_to_menu_was_held) {
          state.command_latches.return_to_menu_was_held = true;
          returning_to_menu = true;
          break;
        }
      } else {
        state.command_latches.return_to_menu_was_held = false;
      }
      // PlayerTick_StatusAndOutfitEvents (0x0044aa70 block 0x0044b240): death
      // bookkeeping, disabled damping, disabled auto-repair, the periodic
      // combat-alert cue, and carried-bomb detonation. Runs ahead of the flight
      // input pass, matching the original's dispatch order. `true` means the
      // death/inactive branch consumed the frame (flight input is skipped).
      // Once the bookkeeping latches game_over_pending the original returns to
      // the pilot/menu flow via its restart command; TODO(decomp(0x0044abf8))
      // skipped: that command channel is not reconstructed, so the port leaves
      // flight mode instead.
      // True when PlayerTick_StatusAndOutfitEvents' death branch consumed the
      // frame (flight input is skipped, matching the original's early return).
      bool player_status_consumed = false;
      if (PlayerTick_StatusAndOutfitEvents(
              state, frame_time_ms / (1000.0F / 30.0F), input.eject)) {
        // The eject transform arms the one-frame timed-action suppression, but
        // because the death branch consumed this frame the loop never reaches
        // the suppression consumer below. Clear it so it cannot also swallow
        // the next frame's pod movement.
        state.timed_action_suppress_this_frame = false;
        if (state.game_over_pending) {
          // The original latches DAT_00596d38 during the tick and only exits at
          // the top of the next loop iteration, so the frame that latches
          // game-over is still presented (the wreck's Explode2 gets its final
          // frames before the menu). Do not break here; the loop condition ends
          // the run after this frame is drawn.
          returning_to_menu = true;
        }
        player_status_consumed = true;
      }
      // PlayerTick_TimedActionTransition (0x0044d490): while a blocking timed
      // action is armed (escape-pod flight) the original returns from the
      // player core after moving the ship and ticking the countdown; flight
      // input and the weapon/target command blocks are skipped. On zero the
      // respawn transition runs inside this call. The eject frame suppresses
      // the tick once (the original's dispatch has already passed when the
      // eject transform arms the countdown).
      bool timed_action_active = false;
      if (!player_status_consumed) {
        if (state.timed_action_suppress_this_frame) {
          state.timed_action_suppress_this_frame = false;
        } else {
          timed_action_active = PlayerTick_TimedActionTransition(
              state, frame_time_ms / (1000.0F / 30.0F));
        }
      }
      // DAT_007354a5: the escape-pod bomb variant latches an immediate return
      // to the menu shell after its deployment overlay.
      if (state.return_to_menu_pending) {
        state.return_to_menu_pending = false;
        returning_to_menu = true;
        break;
      }
      // Per-frame player-target validation (Ship_HandlePlayerShipCore
      // 0x0044aa70 prologue): the ship target drops when the target ship is
      // inactive, destroyed, entering hyperspace (AI state 0x15) or cloaked
      // past the visibility gate -- which is how a targeted ship jumping out
      // releases the selection. The stellar selection has no per-frame
      // validation.
      const bool player_tick_consumed =
          player_status_consumed || timed_action_active;
      if (!player_tick_consumed) {
        NovaTargeting_ValidatePlayerTarget(state);
      }
      // Ghidra System_UpdateSystemAndStellarDisplayState (0x00432470) scope B:
      // per-tick recompute of the transient discovered_this_rebuild latch (the
      // starmap's one-jump-ahead window), so script reveals (mïsn X opcode) get
      // their grey neighbour ring without waiting for the next system entry.
      NovaSystem_RebuildDiscoveredLatch(state);
      if (!player_tick_consumed) {
        PlayerTick_TravelSelectionCommands(state, input, arm_modifier_held);
        PlayerTick_ShipTargetCommands(state, input);
      }
      // Route-map overlay zoom + auto-dismiss (Ghidra 0x0045216e /
      // 0x0042f23e, the PlayerTick_AuxiliaryCommands zoom block). Zoom keys
      // are raw scancodes: minus/equals and the numpad -/+ pair; the
      // modifier-combo guard covers shift/ctrl/alt
      // (TODO(decomp): original commands 0x6b/0x6f not modelled).
      if (!player_tick_consumed) {
        RouteMap_Tick(
            state,
            {.zoom_in_held = platform.IsOriginalKeyCodeHeld(0x0c) ||
                             platform.IsOriginalKeyCodeHeld(0x4a),
             .zoom_out_held = platform.IsOriginalKeyCodeHeld(0x0d) ||
                              platform.IsOriginalKeyCodeHeld(0x4e),
             .modifier_combo_held = platform.IsOriginalKeyCodeHeld(0x2a) ||
                                    platform.IsOriginalKeyCodeHeld(0x36) ||
                                    platform.IsOriginalKeyCodeHeld(0x1d) ||
                                    platform.IsOriginalKeyCodeHeld(0x38)});
      }
      if (!player_tick_consumed) {
        PlayerTick_MouseTargetAndControlCommands(platform, view, state, input);
      }
      const bool land_pressed = !player_tick_consumed && input.land &&
                                !state.command_latches.land_was_held;
      const bool target_action_pressed =
          !player_tick_consumed && input.target_action &&
          !state.command_latches.target_action_was_held;
      const bool board_pressed = !player_tick_consumed && input.board &&
                                 !state.command_latches.board_was_held;
      if (!player_tick_consumed) {
        state.command_latches.land_was_held = input.land;
        state.command_latches.target_action_was_held = input.target_action;
        state.command_latches.board_was_held = input.board;
      }
      // The original's movement values are per simulation tick. Its normal
      // cadence is 30 Hz; using a 60 Hz SDL render loop without this conversion
      // advances the player ship at twice the intended speed.
      // During an engaged hyperspace jump the jump state machine owns the ship
      // (heading/velocity/glow) for the brake, alignment hold and zoom thrust;
      // the player's normal movement integration is suspended so it does not
      // overwrite the jump's flight. NovaTravel_Tick below drives the phases.
      // Ship_HandlePlayerShipCore (0x00451003): with escorts resolved on the
      // player, the player core refreshes their wedge offsets every frame
      // (smooth mode). The +0xC2 flag comes from Ship_TickLeaderFlags in
      // the scope-6 pass. Gated off while the jump state machine owns the ship,
      // matching the original's engage path bypassing this command block.
      // PlayerTick_WeaponCommands (0x0044aa70 block 0x0044BEB0) +
      // PlayerTick_WeaponCycleContinuation (0x0044EAB4): primary fire loop,
      // selected-secondary fire, unfirable-bank auto-clear, the wrapped
      // secondary-bank cycle and the clear-selection arm. The original gates
      // the fire arms on the station-hold/maneuver timers and the
      // disabled state inside the dispatch; the port additionally
      // suspends the whole pass while the jump state machine owns the ship
      // (matching the disable restriction the original applies through the
      // brake, hold and zoom phases) and while a blocking timed action runs
      // (the original returns from the player core before the weapon block).
      if (!player_tick_consumed && !state.travel.engaging) {
        const bool cycle_secondary =
            input.cycle_secondary &&
            !state.command_latches.secondary_cycle_was_held;
        const bool clear_secondary =
            input.clear_secondary &&
            !state.command_latches.clear_secondary_was_held;
        NovaWeapon_TickPlayerWeaponCommands(
            state,
            {.fire_primary_held = input.fire,
             .fire_secondary_held = input.fire_secondary,
             .cycle_secondary = cycle_secondary,
             .cycle_secondary_backwards = input.cycle_secondary_backwards,
             .clear_secondary = clear_secondary},
            frame_time_ms / kOriginalTickMs);
        state.command_latches.secondary_cycle_was_held = input.cycle_secondary;
        state.command_latches.clear_secondary_was_held = input.clear_secondary;
        // HUD/panel dismiss (Ghidra 0x00450ae7 auxiliary block): the rising
        // edge of binding slot 6 clears the transient HUD overlay message and
        // the route-map overlay, and closes the Escort Commands panel (the
        // original sets local_265, consumed at 0x004529a8). The port routes
        // the panel close through EscortCommandState::close_pending so the
        // escort tick below performs the same clear.
        const bool dismiss_pressed =
            input.dismiss && !state.command_latches.dismiss_was_held;
        state.command_latches.dismiss_was_held = input.dismiss;
        if (dismiss_pressed) {
          NovaHud_ClearOverlayMessage(state);
          state.route_map.overlay_visible = false;
          if (state.escort.panel_timer > 0) {
            state.escort.close_pending = true;
          }
        }
        // Escort Commands overlay + order dispatch (PlayerTick_Auxiliary-
        // Commands escort blocks 0x00450ae7..0x00450f67, Ship_CommandPlayer-
        // EscortGroup 0x0045c880): the keys resolve through the binding table
        // (rebindable slots 0x2a / 0x30..0x33 plus fixed number-key codes
        // 2..6 for group selection) against the live keyboard state.
        const auto &escort_key = prefs.bindings.cmd_to_key;
        const auto escort_held = [&platform](std::uint16_t code) {
          return code != 0xff && platform.IsOriginalKeyCodeHeld(code);
        };
        PlayerTick_EscortCommands(
            state,
            {.panel_toggle_held = escort_held(escort_key[0x2a]),
             // g_panel_suppressed_commands is the fixed physical-key list
             // {1,2,3,4,5} in DIK numbering ({2,3,4,5,6}), not five entries
             // in g_player_key_bindings. These category selectors therefore
             // remain the number row even when ordinary commands are rebound.
             .select_group_held =
                 {escort_held(kEscortGroupSelectionKeyCodes[0]),
                  escort_held(kEscortGroupSelectionKeyCodes[1]),
                  escort_held(kEscortGroupSelectionKeyCodes[2]),
                  escort_held(kEscortGroupSelectionKeyCodes[3]),
                  escort_held(kEscortGroupSelectionKeyCodes[4])},
             .order_attack_held = escort_held(escort_key[0x30]),
             .order_defend_held = escort_held(escort_key[0x31]),
             .order_hold_held = escort_held(escort_key[0x32]),
             .order_formation_held = escort_held(escort_key[0x33]),
             .arm_modifier_held = escort_held(0x38) || escort_held(0x6f)},
            state.gameplay_now_ms * 60 / 1000);
      }
      // Face-target command (Ghidra 0x0044aa70 block 0x0044c0b1 -> 0x0044c18a,
      // binding slot 7; the port binds R -- see FlightInput::face_target):
      // while held, stores the heading to face and arms the manual-flight
      // auto-turn that PlayerTick_ManualFlightAndRegeneration consumes this
      // frame. The original dispatches this directly after the weapon commands.
      bool face_target_armed = false;
      if (!player_tick_consumed && !state.travel.engaging) {
        const bool face_arm_modifier = platform.IsOriginalKeyCodeHeld(0x38) ||
                                       platform.IsOriginalKeyCodeHeld(0x6f);
        face_target_armed =
            PlayerTick_FaceTargetCommand(state, input, face_arm_modifier);
      }
      // Ghidra 0x00450710 PlayerTick_AuxiliaryCommands coverage map: the port
      // reconstructs the fuel-scoop tail
      // (PlayerTick_IonizationAndFuelRegeneration), the
      // escort/category command group (above), the route-map zoom block
      // (0x0045216e, RouteMap_Tick) and the ionization/fuel regeneration arm
      // (0x00450717, PlayerTick_IonizationAndFuelRegeneration).
      // TODO(decomp(0x00450fd9)) skipped: the g_cheat_mode_active arms of this
      // region are unported. PlayerTick_CheatAndSpawnCommands (0x00450fd9):
      // instant-jump command 0xe, plus the 0x38/0x6f/0x1d/0x6b/0x2a debug
      // ship-spawn combos (see the 0x00452b71 TODO in the
      // InteractionCloakAndStatus block below). Neighbouring unported cheats:
      // map-wipe 0x00450801; F6 reload / ammo+shield+armor grant, binding slot
      // 0x23 0x004509aa; cheat target command 0x41 at 0x00450a62; the F12 FPS
      // overlay toggle, binding slot 0x34, at 0x00450f6d; and the launch-bay
      // command slice. All gated on g_cheat_mode_active; tracked in
      // decomp-progress.tsv (PlayerTick_AuxiliaryCommands row).
      // Cooldown decay remains live during an engaged jump, but not after an
      // earlier death/timed-action branch has returned from the player core.
      if (!player_tick_consumed) {
        NovaWeapon_TickPlayerWeaponBankCooldowns(
            state, frame_time_ms / kOriginalTickMs);
      }
      // Play every fire sound latched this frame by the player or NPC firing
      // routines (a round actually spawned). The firing routines append
      // GameState.pending_fire_sounds; this loop owns the SdlAudio device,
      // plays each decoded sound with the original's distance attenuation
      // (NovaAudio_PlaySpatialByDistance, NPC fire sourced at the firing ship
      // against the player ship as listener; the player's own fire is
      // src == listener and plays at full volume), then clears the queue. Runs
      // every frame regardless of the fire input so NPC volleys are audible.
      for (const auto &pending : state.pending_fire_sounds) {
        if (pending.slot < 0 || pending.slot >= 36) {
          continue;
        }
        // Weapon fire slots 0..35 map onto the gameplay snd table (id
        // 200+slot), which is fully preloaded; fall back to the legacy
        // owned-weapon cache.
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
        const float gain = NovaWeapon_ComputeSpatialFireGain(state.player.pos_x,
                                                             state.player.pos_y,
                                                             pending.src_x,
                                                             pending.src_y);
        audio.Play(*sound, gain, 1.0F, pending.slot, pending.priority_width);
      }
      state.pending_fire_sounds.clear();
      // Impact sounds use the original 300..363 snd range, which is already
      // covered by the contiguous gameplay_sounds cache at offsets 100..163.
      // Collision/effect code only queues source coordinates; this loop owns
      // the SDL audio device and applies the same spatial attenuation as weapon
      // fire.
      for (const auto &pending : state.pending_impact_sounds) {
        if (pending.slot < 0 || pending.slot >= 64) {
          continue;
        }
        const std::size_t cache_index =
            static_cast<std::size_t>(100 + pending.slot);
        if (!state.gameplay_sounds[cache_index].has_value()) {
          continue;
        }
        const float gain = NovaWeapon_ComputeSpatialFireGain(state.player.pos_x,
                                                             state.player.pos_y,
                                                             pending.src_x,
                                                             pending.src_y);
        audio.Play(*state.gameplay_sounds[cache_index],
                   gain,
                   1.0F,
                   300 + pending.slot,
                   /*priority_width=*/6);
      }
      state.pending_impact_sounds.clear();
      constexpr std::size_t kDestructionSoundIndex = 372 - 200;
      for (const auto &pending : state.pending_destruction_sounds) {
        if (!state.gameplay_sounds[kDestructionSoundIndex].has_value()) {
          continue;
        }
        const float gain = NovaWeapon_ComputeSpatialFireGain(state.player.pos_x,
                                                             state.player.pos_y,
                                                             pending.src_x,
                                                             pending.src_y);
        audio.Play(*state.gameplay_sounds[kDestructionSoundIndex],
                   gain,
                   1.0F,
                   372,
                   /*priority_width=*/5);
      }
      state.pending_destruction_sounds.clear();
      // Centered UI cues from the boarding system (transition-table handles,
      // snd 150 + index). The flight loop owns the audio device; the modal
      // windows play their cues through the same queue. Mirrors
      // NovaAudio_QueueCenteredSound(handle, priority_width, ...). The lazy
      // decode runs
      // here so every gameplay-side queuer (jump-range cue, auto-repair)
      // sees a loaded table even without a boarding pass.
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
        audio.Play(*sound,
                   1.0F,
                   1.0F,
                   150 + pending.transition_index,
                   pending.priority_width);
      }
      state.pending_ui_sounds.clear();
      // Combat alert (Ghidra 0x0044d410-0x0044d46b): the rising edge of
      // Ship_IsAnyShipThreatToPlayerSquad queues the dedicated snd 370 "Red
      // Alert" handle via NovaAudio_QueueCenteredSound(handle, 5, ...). The
      // original keeps a separate handle (g_nova_control_bits[144]); the port
      // plays the same sample from the preloaded gameplay_sounds cache.
      if (state.pending_red_alert) {
        state.pending_red_alert = false;
        if (state.gameplay_sounds[kRedAlertGameplaySoundIndex].has_value()) {
          audio.Play(*state.gameplay_sounds[kRedAlertGameplaySoundIndex],
                     1.0F,
                     1.0F,
                     370,
                     /*priority_width=*/5);
        }
      }
      // Cross-system hyperspace jump state machine (travel.cpp): engages on
      // the 'j' key with a plotted destination, then drives the brake and the
      // warp-up hold before the fire. The 'Warp up' voice count gates the fire
      // like the original's NovaAudio_CountActiveByHandle latch.
      if (!player_tick_consumed) {
        NovaTravel_Tick(
            state,
            input.travel,
            frame_time_ms,
            audio.CountActiveByKey(game::kHyperspaceWarpUpSoundKey) > 0);
        // Fresh-pilot flight tutorial (Ship_HandlePlayerShipCore 0x00451f8f):
        // runs on the player tick independently of the land/jump commands.
        TickFlightTutorialHints(state, prefs);
      }
      // The original dispatches the hyperspace command before the manual-flight
      // region. A newly engaged jump therefore owns this frame immediately;
      // normal steering must not contribute one final integration step.
      if (!player_tick_consumed && !state.travel.engaging) {
        PlayerTick_ManualFlightAndRegeneration(
            state, input, frame_time_ms / kOriginalTickMs, face_target_armed);
      } else if (!timed_action_active && state.player.is_active &&
                 NovaAiShip_IsDestroyed(state.player)) {
        // Destroyed hull coast (Ship_HandlePlayerShipCore 0x0044aa70): the
        // original still applies stellar gravity, the per-axis speed caps and
        // position integration while its input-driven thrust/steering blocks
        // are gated off by the fire-restricted flag. Feed a neutral input so
        // the wreck keeps its inertia through the death presentation instead of
        // stopping dead.
        PlayerTick_ManualFlightAndRegeneration(state,
                                               FlightInput{},
                                               frame_time_ms / kOriginalTickMs,
                                               /*face_target_armed=*/false);
      }
      if (!player_tick_consumed) {
        PlayerTick_ShieldAndArmorRegeneration(state, frame_time_ms);
      }
      if (!player_tick_consumed && !state.travel.engaging &&
          state.player.ai_selected_as_resolved_target) {
        Ship_UpdateEscortFormations(state, state.player, /*snap=*/false);
      }
      // Play the hyperspace jump sounds latched by the travel state machine
      // (the 'Warp up' cue as the zoom thrust begins and the 'Warp out' boom at
      // the fire/arrival, synced with the screen flash). The loop owns the
      // SdlAudio device, so travel only latches flags. Mirrors the original's
      // Stellar_TriggerHyperspaceAudioOnce one-shot (g_playerHyperspaceAudio-
      // Latch gating NovaAudio_QueueCenteredSound)
      // [Ghidra 0x00431420].
      if (state.warp_up_sound_pending) {
        if (state.warp_up_sound.has_value()) {
          // The fire gate in NovaTravel_Tick waits for this voice to finish,
          // and the tunnel ramp schedule is cue-relative, so this rate sets the
          // whole jump cadence. The original descriptor stores the reciprocal
          // as a duration scale (0x0046ab00: 65536/multiplier fixed-point),
          // while SDL's playback_rate is a speed multiplier. Pass the chassis
          // multiplier directly (0.91..2.08), giving cue duration
          // base_duration / multiplier.
          audio.Play(*state.warp_up_sound,
                     1.0F,
                     game::NovaTravel_PlayerJumpDurationMultiplier(state),
                     game::kHyperspaceWarpUpSoundKey,
                     /*priority_width=*/0x32);
        }
        state.warp_up_sound_pending = false;
      }
      if (state.warp_up_cancel_pending) {
        audio.StopByKey(game::kHyperspaceWarpUpSoundKey);
        state.warp_up_cancel_pending = false;
      }
      if (state.warp_out_sound_pending) {
        if (state.warp_out_sound.has_value()) {
          audio.Play(*state.warp_out_sound,
                     1.0F,
                     1.0F,
                     /*sound_key=*/130,
                     /*priority_width=*/0x32);
        }
        state.warp_out_sound_pending = false;
      }

      // Finish the system-entry branch before commands can observe the new
      // system. In particular, a completed jump clears `engaging`; modal and
      // targeting commands below must see the rebuilt population and
      // selections.
      if (!player_tick_consumed) {
        PlayerTick_JumpArrivalBlock(platform, view, hud, state, now_ms);
      }

      // Galaxy-map command ('m', edge-triggered): open the starmap modal over
      // the current flight scene. Mirrors Ship_HandlePlayerShip (0x0044b120)
      // dispatching NovaUi_RunStarmapWindow when its map gameplay command is
      // active. The modal owns the frame until the player closes it; the jump
      // state machine is untouched by inspection (the map only selects
      // systems). Gated while a jump is engaged (the original fire-restricts
      // the player through the brake, hold and zoom).
      const bool starmap_held = input.starmap;
      if (!player_tick_consumed && starmap_held &&
          !state.command_latches.starmap_was_held && !state.travel.engaging) {
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
        // travel stellar. The original re-arms every command edge latch on map
        // return (NovaUi_MarkTravelAndStatusPanelsDirty), so a held Escape
        // (or map key) must be released before it re-fires.
        NovaUi_MarkTravelAndStatusPanelsDirty(state);
        state.travel_reticle_pulse = 256.0F;
        // The map blocked the loop; freeze gameplay time across it.
        resync_frame_clock();
      }
      if (!player_tick_consumed) {
        state.command_latches.starmap_was_held = starmap_held;
      }
      // Active-missions command ('i', edge-triggered; gameplay command 0x28):
      // Ship_HandlePlayerShipCore 0x0044aa70 counts the non-invisible active
      // missions and either plays the denied cue + "You have no active
      // missions." overlay (STR# 0x7d2 0x162, 0xf0 ticks) or opens the
      // mission-computer window (NovaUi_RunMissionComputerWindow 0x00446150).
      // Gates mirrored from 0x00451c87: the command suppresses while the player
      // is disabled (ai_station_hold_timer > 0), a timed action is armed
      // (timed_action_counter > 0), destroyed (death_timer_active > 0) or
      // within the 15-frame arrival grace -- the port's earlier travel.engaging
      // gate (a jump's hold/zoom phases set the hold timer anyway) is subsumed
      // by the disabled check.
      const bool mission_info_held = input.mission_info;
      if (!player_tick_consumed && mission_info_held &&
          !state.command_latches.mission_info_was_held &&
          state.player.ai_station_hold_timer <= 0.0F &&
          state.player.timed_action_counter <= 0 &&
          state.player.death_timer_active <= 0.0F &&
          state.arrival_command_grace_frames <= 0) {
        std::size_t visible_missions = 0;
        for (std::size_t slot = 0;
             slot < state.active_mission_runtime_flags.size();
             ++slot) {
          const auto &flags = state.active_mission_runtime_flags[slot];
          if (flags.is_active &&
              (flags.flags_primary_at_accept & 0x400) == 0U) {
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
          // across it, and swallow the held info/close keys (the original arms
          // the command latches on the modal return).
          NovaUi_MarkTravelAndStatusPanelsDirty(state);
          resync_frame_clock();
        }
      }
      if (!player_tick_consumed) {
        state.command_latches.mission_info_was_held = mission_info_held;
      }
      // No per-frame stellar auto-seed: the original only sets
      // ai_secondary_target_slot from explicit commands (the land command's
      // nearest-pick below, number keys, click, nearest, starmap route). The
      // selection therefore stays empty until the player targets something, and
      // a ship target and a stellar selection can coexist as in the original.
      // A change to the selected travel stellar re-arms the travel reticle
      // pulse (the original arms _g_travel_target_reticle_pulse whenever
      // ai_secondary_target_slot is assigned a fresh stellar).
      if (!player_tick_consumed &&
          state.travel.selected_stellar_id !=
              state.command_latches.prev_travel_stellar) {
        state.travel_reticle_pulse = 256.0F;
        // A fresh selection restarts the landing/docking approach
        // (Stellar_HandleStellarEntryAndExit 0x0045937c re-arms the timer for
        // the new ai_secondary_target_slot).
        state.travel.engage_timer = -1;
        state.command_latches.prev_travel_stellar =
            state.travel.selected_stellar_id;
      }
      // Normal arrival (Return) is independent of target action: the original
      // player-ship tick directly invokes Stellar_HandleStellarEntryAndExit
      // here, opening the Spaceport only when the selected ordinary stellar is
      // inside its arrival envelope. The rejection feedback is shown as an
      // on-screen HUD overlay (STR# 0x7d2 messages) instead of a bare log line.
      // Gated while a jump is engaged (disabled through brake + hold + zoom).
      if (!player_tick_consumed && land_pressed && !state.travel.engaging) {
        const LandCommandResult landed = PlayerTick_LandCommandDispatch(
            platform, audio, view, hud, state, prefs);
        if (landed == LandCommandResult::kQuit) {
          returning_to_menu = true;
          break;
        }
        if (landed == LandCommandResult::kBlockedFrame) {
          resync_frame_clock();
          // A landing/service modal may have returned to flight with a large
          // accelerated-tick batch still queued. End this batch at the modal
          // boundary so the next outer frame can publish/observe the docked
          // state before probe automation emits another landing edge.
          break;
        }
      }
      // Landing/docking approach progress
      // (NovaUi_UpdateTravelEngagementProgress 0x00459950): runs once per
      // player frame, arms state.travel.engage_timer to 0x2ee while the
      // selected stellar is within 250 px, shows the "cleared to dock/land"
      // overlay, and expires the request back to -1. Gated while a jump is
      // engaged, mirroring the player tick.
      if (!player_tick_consumed && !state.travel.engaging) {
        NovaTravel_UpdateEngagementProgress(state);
      }
      // Target action remains the distinct DLOG 0x3f1 bribe/hostility/script
      // interaction pathway. It is intentionally not substituted for landing.
      // Mirrors Ship_HandlePlayerTargetActionCommand (0x00454910): with a ship
      // primary target the action opens the ship-comm dialog (DLOG 0x3ef); with
      // no target (or the 0x38/0x6f commands held) it opens the destination-
      // interaction window for the selected travel stellar. The player
      // disabled/ destroyed gate and the target-ship "entering hyperspace"
      // latch only beep + show an overlay in the original (the clean-room shows
      // the overlay text; the beep is not modelled). Mission-ship defs use the
      // mission interaction window; an accepted single-ship escort mission can
      // replace the hailed personality ship in-place. Gated while a jump is
      // engaged (disabled through brake + hold + zoom).
      if (!player_tick_consumed && target_action_pressed &&
          !state.travel.engaging) {
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
              const std::int16_t pers_slot = target.pers_def_slot;
              const PersDef *pers =
                  pers_slot >= 0 && static_cast<std::size_t>(pers_slot) <
                                        state.scenario.pers_defs.size()
                      ? &state.scenario
                             .pers_defs[static_cast<std::size_t>(pers_slot)]
                      : nullptr;
              if (pers != nullptr && pers->link_mission_id != -1 &&
                  (pers->flags_primary & 0x0200U) == 0U &&
                  Mission_CheckMissionShipInteractionEligibility(
                      state,
                      pers->link_mission_id,
                      /*interaction_context=*/true)) {
                // The original sets g_travel_scene_ctx and the speaking-ship
                // latch around NovaUi_RunMissionShipInteractionWindow. The
                // mission offer renderer is the current clean-room window
                // shell; it accepts the same definition and paints over the
                // live flight frame while the state-only post-accept arm below
                // performs the replacement.
                state.mission_speaker_ship_slot = ship_target;
                const MissionOfferResult result = NovaMission_RunOfferWindow(
                    platform,
                    state,
                    pers->link_mission_id,
                    /*landed_stellar_id=*/-1,
                    [&platform, &state, &view, &hud]() {
                      view.DrawGameFrame(platform, state, hud);
                    });
                state.mission_speaker_ship_slot = -1;
                if (result == MissionOfferResult::kAccepted) {
                  (void)Mission_HandleAcceptedShipInteraction(
                      state,
                      ship_target,
                      static_cast<std::uint32_t>(state.gameplay_now_ms));
                }
                // The mission interaction modal blocked the loop; freeze game
                // time across it, and swallow the held interaction/cancel keys
                // (the original arms the player command latches on return).
                NovaUi_MarkTravelAndStatusPanelsDirty(state);
                resync_frame_clock();
              } else {
                (void)NovaShipComm_RunShipDialog(
                    platform, state, ship_target, view, hud);
                // The comm dialog blocked the loop; freeze gameplay time and
                // swallow the held hail/cancel keys.
                NovaUi_MarkTravelAndStatusPanelsDirty(state);
                resync_frame_clock();
              }
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
          // gameplay time across it and swallow the held target/cancel keys.
          NovaUi_MarkTravelAndStatusPanelsDirty(state);
          resync_frame_clock();
        } else {
          NovaLog::Info("target-action: selected stellar cannot open its "
                        "destination interaction");
        }
      }
      // Board command ('b', edge-triggered): Player_HandleBoardTargetCommand
      // (0x0045a3d0). Like the original's command-latch read in
      // Ship_HandlePlayerShipCore this runs every frame regardless of jump
      // state; its own gates reject un-boardable targets. The dispatch may open
      // the boarding/plunder modal (blocking on the flight loop); the modal
      // renders the live game view beneath itself via
      // SpaceflightView::DrawGameFrame.
      if (!player_tick_consumed && board_pressed) {
        Player_HandleBoardTargetCommand(platform, audio, state, view, hud);
        if (returning_to_menu) {
          break;
        }
        // The boarding/plunder modal blocked the loop; freeze gameplay time
        // and swallow the held board/cancel keys.
        NovaUi_MarkTravelAndStatusPanelsDirty(state);
        resync_frame_clock();
      }
      // Self-destruct + cloak commands (Ghidra 0x0044aa70
      // PlayerTick_Interaction- CloakAndStatus blocks 0x00451954..0x00451b91
      // and 0x00451db0..0x00451e6f). Key bindings resolve through the table
      // like the escort commands: the self-destruct needs the 0x38/0x6f
      // arm-modifier pair (both Alt scancodes) plus binding slot 0x12; the
      // cloak toggle is binding slot 0x29 (default DIK 0x16 = U). Both run
      // every frame the player core reaches them; the countdown/toggle latches
      // do their own gating.
      if (!player_tick_consumed) {
        const auto &binding_key = prefs.bindings.cmd_to_key;
        const auto held = [&platform](std::uint16_t code) {
          return code != 0xff && platform.IsOriginalKeyCodeHeld(code);
        };
        PlayerTick_SelfDestructCommand(state,
                                       arm_modifier_held &&
                                           held(binding_key[0x12]),
                                       frame_time_ms / kOriginalTickMs);
        // In-flight cargo dump (Ghidra 0x0044aa70 block 0x00451907): the
        // arm-modifier pair (0x38/0x6f) plus binding slot 0x0f dumps the fleet
        // cargo. Shift held selects the non-mission-only variant (cVar6 = 0);
        // without Shift everything, mission cargo included, is jettisoned
        // (cVar6 = 1). The original calls Player_RedistributeFleetCargoOverflow
        // while the command is held; the pass is idempotent after the first
        // frame because the cargo is already empty.
        if (arm_modifier_held && held(binding_key[0x0f])) {
          Player_RedistributeFleetCargoOverflow(
              state, /*jettison_all=*/!shift_held, now_ms);
        }
        // Player Info window (Ghidra 0x00451b91 -> 0x00451c4f, binding slot
        // 0x19 = the manual's P-key dialog). The original hides the travel-
        // selection sprite, redraws viewport+radar and resets the average
        // frame-time accumulator after the modal closes; the port's modal
        // runs synchronously and the next frame's draw covers the refresh.
        // The mission-computer window (0x00451c87 -> 0x00451db0) that follows
        // this block is TODO(decomp).
        if (held(binding_key[0x19])) {
          const PlayerInfoWindowResult info_result =
              NovaPlayerInfo_RunWindow(platform, state, view, hud);
          // The original runs Player_RedistributeFleetCargoOverflow(1) inside
          // the window loop when the Cargo page's Jettison action is confirmed
          // (0x00499c10); the port surfaces the confirmation and applies it
          // here, where the sim clock is in scope for the mission teardown.
          if (info_result.jettison_confirmed) {
            Player_RedistributeFleetCargoOverflow(
                state, /*jettison_all=*/true, now_ms);
          }
          // Swallow the held info/close keys so the key that closed the
          // window must be released before it re-fires.
          NovaUi_MarkTravelAndStatusPanelsDirty(state);
          resync_frame_clock();
        }
        PlayerTick_InteractionCloakAndStatus(
            state, held(binding_key[0x29]), frame_time_ms / kOriginalTickMs);
        // TODO(decomp(0x00452b71)) skipped: the g_cheat_mode_active debug/cheat
        // arms that live in this region are unported. The ship-spawn block
        // (0x00452b71): while binding slot 0x1e / raw key 0x6a is held
        // (first-press latch), the 0x38/0x6f/0x1d/0x6b/0x2a modifier combos
        // spawn an encounter fleet / random dude / përs ship and set it as the
        // primary target. Adjacent gated arms in 0x00452c60..0x004531xx: kill
        // primary target (slot 0x1f), disabled/board target (slot 0x21),
        // armor=1 kill (slot 0x22), debug credits +50000 (slot 0x24), and the
        // slot 0x26/0x27 cheats. The auxiliary-region cheats (map-wipe
        // 0x00450801, F6 reload / grant slot 0x23 0x004509aa, cheat target
        // command 0x00450a62, PlayerTick_CheatAndSpawnCommands 0x00450fd9, F12
        // FPS overlay toggle slot 0x34 0x00450f6d) are listed at the
        // PlayerTick_AuxiliaryCommands TODO above. All are gated on
        // g_cheat_mode_active and tracked in decomp-progress.tsv.
      }
      // Late auxiliary regeneration region: ionization decay and velocity
      // damping, followed by fuel-scoop recharge.
      if (!player_tick_consumed) {
        PlayerTick_IonizationAndFuelRegeneration(state, frame_time_ms);
      }
      // Expire any transient HUD overlay message once its wall-clock deadline
      // passes (the draw path is const over state).
      NovaHud_TickOverlay(state);
      const float delta_x = state.player.pos_x - prev_x;
      const float delta_y = state.player.pos_y - prev_y;
      prev_x = state.player.pos_x;
      prev_y = state.player.pos_y;
      // Unified per-frame world animation pass: advances the ambient starfield
      // by the ship's movement delta (NovaEffects_UpdateAmbientStarParticles)
      // and steps each animated stellar one animation segment
      // (Stellar_UpdateStellar- Sprites), both on the single frame_time_ms
      // cadence (see the header).
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
      NovaFrame_TickSystems(state,
                            audio,
                            /*run_full_tick=*/true,
                            frame_time_ms / kOriginalTickMs,
                            prefs);
    }

    // Ghidra scope 2 "drawing": sprite world present + viewport particles +
    // commit frame.
    // A shipyard purchase/capture can change the player class while this
    // flight loop remains alive. Re-run the class-aware cache gate before
    // every draw; it is a no-op unless the class changed, and refreshes the
    // hull/effect sheets plus muzzle geometry when it did.
    (void)view.EnsureShipSprite(platform, state);
    view.DrawGameFrame(platform, state, hud);
    // Route-map overlay blit (Ghidra 0x00439bd0, drawn over the frame after
    // the HUD overlay message, before the target-category panel): draws only
    // while the overlay flag is up, alpha-faded per the interaction timer.
    route_map_view.Draw(platform, state);
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
    // settling the 256px grow-out in ~0.15s.
    state.ship_reticle_pulse =
        std::max(0.0F, state.ship_reticle_pulse - host_frame_time_ms * 1.8F);
    state.travel_reticle_pulse =
        std::max(0.0F, state.travel_reticle_pulse - host_frame_time_ms * 1.8F);
    // Advance the hyperspace flash at the render cadence. The mode gates the
    // rate (1.5 s Mac fade-in/fade-out vs ~60 ms one-frame fallback); travel
    // only latches the hold fade once when its scalar crosses the threshold.
    NovaTravel_AdvanceScreenFlash(state, host_frame_time_ms);
    platform.PaceFrame();
  }
}

} // namespace

// External test seam for the fresh-pilot flight tutorial (Ghidra 0x00451f8f).
// TickFlightTutorialHints lives in the anonymous namespace above; this wrapper
// exposes it to unit tests.
void PlayerTick_FlightTutorialHints(GameState &state,
                                    const NovaPreferences &prefs) {
  TickFlightTutorialHints(state, prefs);
}

// External test seam into the per-ship simulation pass (Ghidra scope 4/5 of
// Frame_TickSystems 0x004186b0 -> Ship_HandleShip 0x00433050). Stub_HandleShips
// lives in the anonymous namespace above; this wrapper exposes it to unit tests
// without external linkage polluting the otherwise-internal stub set.
void NovaShip_TickNpcShips(GameState &state, float elapsed_ticks) {
  Stub_HandleShips(state, elapsed_ticks);
}

// External test seam into the per-ship AI decision pass (Ghidra scope 6 of
// Frame_TickSystems 0x004186b0 -> Ship_UpdateShipAI 0x00401000). Mirrors
// NovaShip_TickNpcShips so tests can model the live AI-then-movement order
// (Stub_AiRoutines runs before Stub_HandleShips in NovaFrame_TickSystems).
void NovaShip_TickNpcAi(GameState &state, float elapsed_ticks) {
  Stub_AiRoutines(state, elapsed_ticks);
}

// ---------------------------------------------------------------------------
// Ghidra 0x00489210 Ship_RunSpaceflightMode.
void NovaSpaceflight_Run(SdlPlatform &platform,
                         SdlAudio &audio,
                         GameState &state,
                         const NovaPreferences &prefs) {
  NovaLog::Info("entering spaceflight mode");

  // Preflight: the new-game intro cinematic plays on the pilot's first entry
  // (Ghidra g_intro_played == 0 in Ship_RunSpaceflightMode). We set the
  // intro_played latch *after* the intro returns, exactly as the original sets
  // g_intro_played = 0x01 immediately after IntroCinematic_Run(). The intro's
  // skip result already gates (a stub of) the intro text-reader dialog
  // internally, so its return value needs no action here.
  if (!state.intro_played) {
    (void)NovaIntroCinematic_Run(platform, audio, state, prefs);
    // Ghidra: g_intro_played = 0x01, the latch IntroCinematic_SetupFrames/
    // Game_ResetNewGameState clear on a new pilot (see new_pilot_flow.cpp).
    state.intro_played = true;
  }

  // Ghidra Ship_RunSpaceflightMode: preflight owns the gameplay surface, runs
  // Frame_SpaceflightLoop, then tears the mode back down to the menu shell.
  bool returning_to_menu = false;
  NovaFrame_SpaceflightLoop(platform, audio, state, returning_to_menu, prefs);
  NovaFrame_CancelCombatChatter(state, audio);
  // NovaMainLoop_Run's mode-exit teardown (0x00486c8a..0x00486cbe) raises the
  // five "clear transient sprites" latches before the menu shell resumes, so
  // re-entering flight with the same pilot (GameState persists on the app)
  // starts from an empty effect/freeflight/asteroid slate. The port clears
  // synchronously on the way out.
  NovaWeapon_ClearTransientCombatState(state);

  NovaLog::Info("leaving spaceflight mode to the main menu");
}

// Ghidra Frame_QueueCombatChatter (0x00426ce0). The original writes three
// globals; the clean-room latches the same triple on GameState.
void NovaFrame_QueueCombatChatter(GameState &state,
                                  std::int16_t kind,
                                  std::int16_t government_id,
                                  std::int16_t variant) {
  state.pending_combat_chatter_kind = kind;
  state.pending_combat_chatter_government_id = government_id;
  state.pending_combat_chatter_variant = variant;
}

// Ghidra 0x004311F0 Frame_UpdateCombatChatter.
void NovaFrame_UpdateCombatChatter(GameState &state, SdlAudio &audio) {
  if (state.active_combat_chatter_sound.has_value() &&
      audio.CountActiveByKey(state.active_combat_chatter_sound_id) == 0) {
    state.active_combat_chatter_sound.reset();
    state.active_combat_chatter_sound_id = -1;
  }

  if (state.pending_combat_chatter_kind == -1) {
    return;
  }
  // NovaAudio_UnregisterCallbacks does not stop an active voice. SDL voices
  // have no completion callbacks to unregister, so retaining the keyed PCM is
  // the equivalent state until a later tick observes that the stream drained.
  if (state.active_combat_chatter_sound.has_value()) {
    return;
  }

  const std::int16_t kind = state.pending_combat_chatter_kind;
  state.pending_combat_chatter_kind = -1;
  if (kind < 0 || kind >= 3) {
    return;
  }

  std::int16_t voice_type = 0;
  const std::int16_t government_id = state.pending_combat_chatter_government_id;
  if (government_id >= 0 && government_id < 0x100 &&
      static_cast<std::size_t>(government_id) <
          state.scenario.governments.size()) {
    voice_type =
        state.scenario.governments[static_cast<std::size_t>(government_id)]
            .voice_type_code;
  }
  if (voice_type < 0 || voice_type >= 8) {
    return;
  }

  const std::int16_t base_id =
      static_cast<std::int16_t>(1000 + voice_type * 100 + kind * 10);
  std::int16_t count = 0;
  while (count < 9 &&
         NovaResource_LoadSndData(static_cast<std::uint16_t>(base_id + count))
             .has_value()) {
    ++count;
  }
  if (count <= 0) {
    return;
  }

  const std::int16_t variant = state.pending_combat_chatter_variant;
  std::int16_t selected = 0;
  if (variant < 0 || variant > 1 || (count & 1) != 0) {
    selected = RandomBelow(state, count);
  } else if (count == 2) {
    selected = variant;
  } else {
    selected =
        static_cast<std::int16_t>(RandomBelow(state, count / 2) * 2 + variant);
  }

  const std::int16_t sound_id = static_cast<std::int16_t>(base_id + selected);
  const auto resource =
      NovaResource_LoadSndData(static_cast<std::uint16_t>(sound_id));
  if (!resource.has_value()) {
    return;
  }
  auto decoded = NovaSound_Decode(*resource);
  if (!decoded.has_value()) {
    NovaLog::Warn("combat chatter snd {} could not be decoded", sound_id);
    return;
  }
  state.active_combat_chatter_sound = std::move(*decoded);
  state.active_combat_chatter_sound_id = sound_id;
  audio.Play(*state.active_combat_chatter_sound,
             1.0F,
             1.0F,
             sound_id,
             /*priority_width=*/0x0f);
}

// Ghidra 0x004313C0 Frame_CancelCombatChatter.
void NovaFrame_CancelCombatChatter(GameState &state, SdlAudio &audio) {
  if (state.active_combat_chatter_sound.has_value() &&
      audio.CountActiveByKey(state.active_combat_chatter_sound_id) == 0) {
    state.active_combat_chatter_sound.reset();
    state.active_combat_chatter_sound_id = -1;
  }
  state.pending_combat_chatter_kind = -1;
  state.pending_combat_chatter_government_id = -1;
  state.pending_combat_chatter_variant = -1;
}

} // namespace game
