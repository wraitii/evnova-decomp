#include "spaceflight.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "asteroid.hpp"
#include "boarding_plunder.hpp"
#include "collision.hpp"
#include "docked_dialog.hpp"
#include "escort_commands.hpp"
#include "escort_formation.hpp"
#include "game_state.hpp"
#include "hud_overlay.hpp"
#include "hud_renderer.hpp"
#include "impact_effects.hpp"
#include "intro_cinematic.hpp"
#include "landed_window.hpp"
#include "mission.hpp"
#include "mission_script.hpp"
#include "negotiation_dialog.hpp"
#include "outfit.hpp"
#include "player_info_window.hpp"
#include "radar_panel.hpp"
#include "route_map.hpp"
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
// Frame_MeasureFrameTiming (0x00432ea0) floors ordinary spaceflight calls at
// 21 ms and publishes elapsed_ms * 0.03 as the normalized tick scale. Raw
// per-call mutations therefore advance at one unit per 0.63 normalized ticks.
constexpr float kOriginalMaxRateFrameTicks = 21.0F * 0.03F;

constexpr float RawSpaceflightCallTicks(float elapsed_ticks) {
  return elapsed_ticks / kOriginalMaxRateFrameTicks;
}

// Player recently-hit timer (g_player_recently_hit_timer, DAT_0073549c):
// armed to 300 ticks when the player takes a hit, decays one tick per frame
// while at or above g_hyperspace_progress_onset_threshold (0x575540, 0.0);
// gates the disabled auto-repair pass until the post-hit regen-suppression
// window expires.
constexpr float kRecentlyHitRegenCutoff = 0.0F;
// k_one_f32 (0x0057555c, float): death-timer countdown step per original
// spaceflight call. The NPC arm of Ship_HandleShip
// (0x00433050) uses the equal-valued g_cloak_fade_passive_decay (0x00575318)
// for the same step. The port time-adjusts both against the original loop's
// 21 ms minimum frame duration.
constexpr float kJumpTurnaroundTurnRateAddend = 1.0F;
// DAT_00575558 (0x00575558, read as a float; bytes 00 00 70 c3 = -240.0): the
// inactive-player death-timer floor. After Ship_UpdateVisualState's finale
// deactivates the hull the timer keeps draining from ~2.0 down to -240 at
// g_jump_turnaround_turn_rate_addend (1.0) per frame before the core latches
// DAT_00596d38 -- the post-explosion window in which the player watches the
// wreck's effect pool finish. Ghidra types this as a byte; the FCOMP at
// 0x0044af17 reads the 4-byte float.
constexpr float kPlayerDeathInactiveTimerFloor = -240.0F;
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
    // skip_heavy_ai=0: these spawned ships run the full (heavy) AI decision.
    NovaAi_UpdateShipAI(
        state, ship, /*skip_heavy_ai=*/false, now_ms, elapsed_ticks);
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
  if (ship.timed_action_counter > 0 && cls->timed_action_counter_init > 0 &&
      ship.death_timer_active <= half_death_delay) {
    // The original casts both operands to int first, so this is integer
    // division before the 0.4 (DAT_005754a0) scale.
    const int death_delay = static_cast<int>(cls->death_delay_frames);
    const int init = static_cast<int>(cls->timed_action_counter_init);
    int interval =
        static_cast<int>(static_cast<float>(death_delay / init) * 0.4F);
    if (interval < 10) {
      interval = 10;
    }
    if (raw_frame_counter % interval == 0 ||
        ship.timed_action_counter == cls->timed_action_counter_init) {
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
void TickIonizationDecay(GameState &state, Ship &ship, float elapsed_ticks) {
  if (ship.ionization_points <= 0.0F) {
    ship.ionization_points = 0.0F;
    return;
  }
  const float decay_rate = NovaOutfit_ComputeIonizationDecayRate(state, ship);
  ship.ionization_points =
      std::max(0.0F, ship.ionization_points - decay_rate * elapsed_ticks);
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
    // g_cloak_fade_passive_decay = 1.0, paused while gameplay time is
    // frozen); the destruction finale runs in the visual-state pass below.
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
      NovaShip_IntegrateNpcMovement(
          state,
          ship,
          *cls,
          elapsed_ticks,
          static_cast<std::uint32_t>(state.gameplay_now_ms));
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

    NovaShip_IntegrateNpcMovement(
        state,
        ship,
        *cls,
        elapsed_ticks,
        static_cast<std::uint32_t>(state.gameplay_now_ms));

    NovaWeapon_TickNpcWeaponBanks(ship, elapsed_ticks);
    // Ship_HandleShip hands a latched active bank to Weapon_FireShipWeapons.
    NovaWeapon_FireNpcWeaponBank(state, ship);

    // Mission-hail ladder (0x00433050 inline block, after the shield/armor
    // recharge in the original's ordering).
    Mission_TickShipHailLadder(
        state,
        ship,
        static_cast<std::uint32_t>(state.gameplay_now_ms * 60 / 1000));

    // The separate ionization speed clamp remains deferred.
    TickIonizationDecay(state, ship, elapsed_ticks);
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
  }
  // scope 9 "collisions": always runs.
  Stub_Collisions(state);

  if (run_full_tick) {
    Stub_DrawStatus(state);                                      // scope 0xc
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
//   gravity-shield steering     0x0044CFFE -> 0x0044D05B
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

struct PlayerTravelSelectionLatches {
  bool target_cycle_was_held = false;
  bool destination_cycle_was_held = false;
  bool hyperspace_was_held = false;
  std::int16_t prev_travel_stellar = -1;
};

struct PlayerShipTargetLatches {
  bool ship_cycle_was_held = false;
  bool nearest_was_held = false;
};

// Ghidra Ship_HandlePlayerShipCore synthetic CFG: travel-selection commands
// 0x0044B7C4 -> 0x0044BAFE. The wider 0x0044B7C4 -> 0x0044BC1E umbrella
// absorbs reordered mouse/route-map blocks and is not used. Handles the Tab
// stellar cycle, Backslash destination-system cycle and H hyperspace-mode arm.
void PlayerTick_TravelSelectionCommands(GameState &state,
                                        const FlightInput &input,
                                        PlayerTravelSelectionLatches &l) {
  const bool target_cycle =
      input.cycle_target_next || input.cycle_target_previous;
  if (target_cycle && !l.target_cycle_was_held) {
    NovaTargeting_CyclePlayerStellarTarget(state, input.cycle_target_next);
  }
  l.target_cycle_was_held = target_cycle;
  // Destination-SYSTEM cycling (Backslash / Shift+Backslash): rotate the
  // next-jump destination through the systems directly linked to the current
  // one. Mirrors the original's key-binding-13 block in PlayerTick_TargetAnd-
  // TravelCommands (g_playerCycleTravelTargetCommandLatch at
  // 0x0044b8b9..0x0044def6). Edge-latched so held-\ steps one system per
  // press. Setting a destination arms travel mode but does NOT engage the
  // jump -- that stays on the 'j' travel key (NovaTravel_Tick).
  const bool destination_cycle =
      input.cycle_destination_next || input.cycle_destination_previous;
  if (destination_cycle && !l.destination_cycle_was_held) {
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
  l.destination_cycle_was_held = destination_cycle;
  // Hyperspace-mode toggle (H): latch the off-map destination-selection
  // channel (the manual's "press H to set hyperspace mode, then Backslash to
  // pick the destination system"). The latch is cleared when a jump lands.
  if (input.hyperspace_mode && !l.hyperspace_was_held) {
    state.travel.hyperspace_mode = true;
    // Entering hyperspace mode latches plotted-jump mode 3 with no slot
    // (0x0044de28: mode=3, ai_secondary_target_slot=-1).
    state.player.travel_transfer_mode = 3;
    // Ghidra 0x0044b95f: the hyperspace-mode arm block refreshes the
    // route-map overlay like the cycle command.
    RouteMap_Open(state);
  }
  l.hyperspace_was_held = input.hyperspace_mode;
}

// Ghidra Ship_HandlePlayerShipCore disjoint ship-target command blocks. The
// nearest-target command is the clean synthetic CFG 0x0044BE15 -> 0x0044BEB7;
// the backquote cycle is embedded in the reordered mouse/target envelope.
// Per-frame validation remains a separate earlier call at 0x0044ADA1.
void PlayerTick_ShipTargetCommands(GameState &state,
                                   const FlightInput &input,
                                   PlayerShipTargetLatches &l) {
  // Ship-target cycling: backquote (`) / Shift+backquote, with the
  // combat-relevant-only modifier (Alt or 'k'). Mirrors the original's
  // Ship_HandlePlayerShip cycle-target block (0x0044b120): a no-op result or
  // a self-result clears the target, otherwise the new slot is stored and
  // the reticle pulse is re-armed at 256.0 (0x43800000).
  const bool ship_cycle =
      input.cycle_ship_target_next || input.cycle_ship_target_previous;
  if (ship_cycle && !l.ship_cycle_was_held) {
    const std::int16_t next = input.cycle_ship_target_next
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
  l.ship_cycle_was_held = ship_cycle;
  // "Target nearest" command: 'o' selects the nearest hostile combat target
  // (Ship_SelectNearestHostileCombatTarget 0x00462bd0), Alt+'o' the nearest
  // engaged target (0x00462850). A miss (-1) leaves the current target
  // untouched; a new slot re-arms the reticle pulse.
  const bool nearest_pressed =
      input.select_nearest_hostile || input.select_nearest_engaged;
  if (nearest_pressed && !l.nearest_was_held) {
    const std::int16_t slot =
        input.select_nearest_hostile
            ? NovaTargeting_SelectNearestHostileCombatTarget(state)
            : NovaTargeting_SelectNearestEngagedTarget(state);
    if (slot != -1 && slot != state.player.primary_target_ship_slot) {
      state.player.primary_target_ship_slot = slot;
      state.ship_reticle_pulse = 256.0F;
    }
  }
  l.nearest_was_held = nearest_pressed;
}

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
// entering-system arrival message and the escort travel-day daily tick +
// stat-modifier reroll (0x0044fb2d/0x0044fb39) of the original slice are
// TODO(decomp) inside.
void PlayerTick_JumpArrivalBlock(SdlPlatform &platform,
                                 SpaceflightView &view,
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
  // TODO(decomp(0x0044fb39)): Frame_JitterPlayerStatModifiers and
  // Frame_RerollPlayerStatModifiers remain unported. Travel-day world ticks
  // run in FireJump before this arrival refresh.
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
}

// Loop-exit result of PlayerTick_LandCommandDispatch: the Spaceport modal can
// quit the app or block the frame (the loop freezes gameplay time across it).
enum class LandCommandResult {
  kContinue,
  kQuit,
  kBlockedFrame,
};

// Ghidra 0x00462410 System_GetCurrentSystemLinkSpriteHeight (landing use): the
// landing envelope reads Sprite_GetFrameFullHeight (0x00462390) on the target's
// link_a spin set, i.e. the full frame height (bottom - top of the sprite's
// placed bounds +0x20/-+0x1c). The gate (Stellar_HandleStellarEntryAndExit
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
std::int16_t StellarArrivalSpriteFullHeight(SdlPlatform &platform,
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
      displayed->tile_height <= 0) {
    return 0;
  }
  // Tier 2: System_GetCurrentSystemLinkSpriteHeight on link_a.
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
  if (spin == nullptr || spin->frames.empty() || spin->tile_height <= 0) {
    NovaLog::Info("stellar {} link_a spin set {} unavailable; half-span "
                  "fallback 0x96",
                  stellar_id,
                  stellar->link_a_id + 1000);
    return 0x96;
  }
  return static_cast<std::int16_t>(spin->tile_height);
}

// Ghidra 0x0044aa70 PlayerTick_LandCommandDispatch (0x00451f8f ->
// [0x004520ea], synthetic plan score 0.98): the land command (binding 5) with
// its nearest-available-stellar auto-pick and the arrival checks opening the
// Spaceport. Divergence TODO(decomp): the original re-picks the nearest
// available travel stellar every 60 frames while travel mode is armed
// (DAT_007cab1c < 2, g_license_check_frame_counter gate in the decompile);
// the port auto-picks only once per press when nothing is targeted.
LandCommandResult PlayerTick_LandCommandDispatch(SdlPlatform &platform,
                                                 SpaceflightView &view,
                                                 GameState &state) {
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
  const std::int16_t target_sprite_full_height = StellarArrivalSpriteFullHeight(
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
        Stellar_MaxLandingDistance(target_sprite_full_height);
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
        platform, view, state, platform.gameplay_ticks_ms());
    return LandCommandResult::kBlockedFrame;
  }
  LandedContext ctx;
  if (Stellar_Dock(state, ctx, target_sprite_full_height)) {
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
      Stellar_Launch(state, ctx.stellar_id);
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
  // (Sprite_GetFrameFullHeight on each loaded spin set).
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
  PlayerTravelSelectionLatches travel_selection_latches;
  travel_selection_latches.prev_travel_stellar =
      state.travel.selected_stellar_id;
  PlayerShipTargetLatches ship_target_latches;
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
  while (!platform.quit_requested() && !returning_to_menu) {
    const std::uint64_t now_ms = platform.gameplay_ticks_ms();
    state.gameplay_now_ms = now_ms;
    const float frame_time_ms = static_cast<float>(now_ms - prev_tick_ms);
    prev_tick_ms = now_ms;
    // Port stand-in for NovaTime_GetTickCount60Hz's g_frame_tick_count_60hz
    // (see GameState::tick_60hz): re-derived from the wall clock each frame.
    state.tick_60hz = static_cast<std::uint32_t>(now_ms * 60ULL / 1000ULL);
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
    FlightInput input = platform.PollFlightInput();
    const auto binding_held = [&](std::size_t command) {
      const std::uint16_t key = prefs.bindings.cmd_to_key[command];
      return key != 0xff && key != 0xffff &&
             platform.IsOriginalKeyCodeHeld(key);
    };
    // Ship_HandlePlayerShipControl reads every action through
    // g_player_key_bindings. PollFlightInput owns the SDL event pump; replace
    // its convenience defaults with the persisted original command table.
    input.turn_left = binding_held(0x13);
    input.turn_right = binding_held(0x14);
    input.thrust = binding_held(0x15);
    input.reverse = binding_held(0x16);
    input.afterburner = binding_held(0x18);
    input.fire = binding_held(0x02);
    input.fire_secondary = binding_held(0x03);
    input.cycle_secondary = binding_held(0x00);
    input.cycle_secondary_backwards =
        input.cycle_secondary && (platform.IsOriginalKeyCodeHeld(0x2a) ||
                                  platform.IsOriginalKeyCodeHeld(0x36));
    input.clear_secondary = binding_held(0x01);
    input.face_target = binding_held(0x07);
    input.land = binding_held(0x06);
    input.travel = binding_held(0x0e);
    input.starmap = binding_held(0x09);
    input.mission_info = binding_held(0x28);
    input.board = binding_held(0x10);
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
    if (PlayerTick_StatusAndOutfitEvents(
            state, frame_time_ms / (1000.0F / 30.0F), input.eject)) {
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
    // Per-frame player-target validation (Ship_HandlePlayerShipCore 0x0044aa70
    // prologue): the ship target drops when the target ship is inactive,
    // destroyed, entering hyperspace (AI state 0x15) or cloaked past the
    // visibility gate -- which is how a targeted ship jumping out releases the
    // selection. The stellar selection has no per-frame validation.
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
      PlayerTick_TravelSelectionCommands(
          state, input, travel_selection_latches);
      PlayerTick_ShipTargetCommands(state, input, ship_target_latches);
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
    const bool land_pressed =
        !player_tick_consumed && input.land && !land_was_held;
    const bool target_action_pressed =
        !player_tick_consumed && input.target_action && !target_action_was_held;
    const bool board_pressed =
        !player_tick_consumed && input.board && !board_was_held;
    if (!player_tick_consumed) {
      land_was_held = input.land;
      target_action_was_held = input.target_action;
      board_was_held = input.board;
    }
    // The original's movement values are per simulation tick. Its normal
    // cadence is 30 Hz; using a 60 Hz SDL render loop without this conversion
    // advances the player ship at twice the intended speed.
    constexpr float kOriginalTickMs = 1000.0F / 30.0F;
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
    // (matching the disable restriction the original applies through the brake,
    // hold and zoom phases) and while a blocking timed action runs (the
    // original returns from the player core before the weapon block).
    if (!player_tick_consumed && !state.travel.engaging) {
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
      // Escort Commands overlay + order dispatch (PlayerTick_Auxiliary-
      // Commands escort blocks 0x00450ae7..0x00450f67, Ship_CommandPlayer-
      // EscortGroup 0x0045c880): the keys resolve through the binding table
      // (slots 0x2a / 0x2b..0x2f / 0x30..0x33) against the live keyboard
      // state, matching the original's command-active reads.
      const auto &escort_key = prefs.bindings.cmd_to_key;
      const auto escort_held = [&platform](std::uint16_t code) {
        return code != 0xff && platform.IsOriginalKeyCodeHeld(code);
      };
      PlayerTick_EscortCommands(
          state,
          {.panel_toggle_held = escort_held(escort_key[0x2a]),
           .select_group_held = {escort_held(escort_key[0x2b]),
                                 escort_held(escort_key[0x2c]),
                                 escort_held(escort_key[0x2d]),
                                 escort_held(escort_key[0x2e]),
                                 escort_held(escort_key[0x2f])},
           .order_attack_held = escort_held(escort_key[0x30]),
           .order_defend_held = escort_held(escort_key[0x31]),
           .order_hold_held = escort_held(escort_key[0x32]),
           .order_formation_held = escort_held(escort_key[0x33]),
           .arm_modifier_held = escort_held(0x38)},
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
    // TODO(decomp(0x00450fd9)) skipped: the cheat/debug-spawn arm
    // (PlayerTick_CheatAndSpawnCommands, plan score 1.37) -- instant-jump
    // cheat (command 0xe) and the 0x38/0x6f/0x1d/0x6b/0x2a debug ship-spawn
    // combos. Also unported: the FPS toggle and the launch-bay command
    // slices of the same region.
    // Cooldown decay remains live during an engaged jump, but not after an
    // earlier death/timed-action branch has returned from the player core.
    if (!player_tick_consumed) {
      NovaWeapon_TickPlayerWeaponBankCooldowns(state,
                                               frame_time_ms / kOriginalTickMs);
    }
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
      audio.Play(*sound, gain, 1.0F, pending.slot, pending.priority_width);
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
      const float gain = NovaWeapon_ComputeSpatialFireGain(
          state.player.pos_x, state.player.pos_y, pending.src_x, pending.src_y);
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
        audio.Play(*sound,
                   1.0F,
                   1.0F,
                   150 + pending.transition_index,
                   /*priority_width=*/8);
      }
    }
    state.pending_ui_sounds.clear();
    // Cross-system hyperspace jump state machine (travel.cpp): engages on
    // the 'j' key with a plotted destination, then drives the brake and the
    // warp-up hold before the fire. The 'Warp up' voice count gates the fire
    // like the original's NovaAudio_CountActiveByHandle latch.
    if (!player_tick_consumed) {
      NovaTravel_Tick(state,
                      input.travel,
                      frame_time_ms,
                      audio.CountActiveByKey(game::kHyperspaceWarpUpSoundKey) >
                          0);
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
      // position integration while its input-driven thrust/steering blocks are
      // gated off by the fire-restricted flag. Feed a neutral input so the
      // wreck keeps its inertia through the death presentation instead of
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

    // Finish the system-entry branch before commands can observe the new
    // system. In particular, a completed jump clears `engaging`; modal and
    // targeting commands below must see the rebuilt population and selections.
    if (!player_tick_consumed) {
      PlayerTick_JumpArrivalBlock(platform, view, state, now_ms);
    }

    // Galaxy-map command ('m', edge-triggered): open the starmap modal over
    // the current flight scene. Mirrors Ship_HandlePlayerShip (0x0044b120)
    // dispatching NovaUi_RunStarmapWindow when its map gameplay command is
    // active. The modal owns the frame until the player closes it; the jump
    // state machine is untouched by inspection (the map only selects systems).
    // Gated while a jump is engaged (the original fire-restricts the player
    // through the brake, hold and zoom).
    const bool starmap_held = input.starmap;
    if (!player_tick_consumed && starmap_held && !starmap_was_held &&
        !state.travel.engaging) {
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
    if (!player_tick_consumed) {
      starmap_was_held = starmap_held;
    }
    // Active-missions command ('i', edge-triggered; gameplay command 0x28):
    // Ship_HandlePlayerShipCore 0x0044aa70 counts the non-invisible active
    // missions and either plays the denied cue + "You have no active
    // missions." overlay (STR# 0x7d2 0x162, 0xf0 ticks) or opens the
    // mission-computer window (NovaUi_RunMissionComputerWindow 0x00446150).
    // Gates mirrored from 0x00451c87: the command suppresses while the player
    // is disabled (ai_station_hold_timer > 0), a timed action is armed
    // (timed_action_counter > 0), destroyed (death_timer_active > 0) or within
    // the 15-frame arrival grace -- the port's earlier travel.engaging gate
    // (a jump's hold/zoom phases set the hold timer anyway) is subsumed by
    // the disabled check.
    const bool mission_info_held = input.mission_info;
    if (!player_tick_consumed && mission_info_held && !mission_info_was_held &&
        state.player.ai_station_hold_timer <= 0.0F &&
        state.player.timed_action_counter <= 0 &&
        state.player.death_timer_active <= 0.0F &&
        state.arrival_command_grace_frames <= 0) {
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
    if (!player_tick_consumed) {
      mission_info_was_held = mission_info_held;
    }
    // No per-frame stellar auto-seed: the original only sets
    // ai_secondary_target_slot from explicit commands (the land command's
    // nearest-pick below, number keys, click, nearest, starmap route). The
    // selection therefore stays empty until the player targets something, and
    // a ship target and a stellar selection can coexist as in the original.
    // A change to the selected travel stellar re-arms the travel reticle pulse
    // (the original arms _g_travel_target_reticle_pulse whenever
    // ai_secondary_target_slot is assigned a fresh stellar).
    if (!player_tick_consumed &&
        state.travel.selected_stellar_id !=
            travel_selection_latches.prev_travel_stellar) {
      state.travel_reticle_pulse = 256.0F;
      // A fresh selection restarts the landing/docking approach
      // (Stellar_HandleStellarEntryAndExit 0x0045937c re-arms the timer for the
      // new ai_secondary_target_slot).
      state.travel.engage_timer = -1;
      travel_selection_latches.prev_travel_stellar =
          state.travel.selected_stellar_id;
    }
    // Normal arrival (Return) is independent of target action: the original
    // player-ship tick directly invokes Stellar_HandleStellarEntryAndExit here,
    // opening the Spaceport only when the selected ordinary stellar is inside
    // its arrival envelope. The rejection feedback is shown as an on-screen HUD
    // overlay (STR# 0x7d2 messages) instead of a bare log line. Gated while a
    // jump is engaged (disabled through brake + hold + zoom).
    if (!player_tick_consumed && land_pressed && !state.travel.engaging) {
      const LandCommandResult landed =
          PlayerTick_LandCommandDispatch(platform, view, state);
      if (landed == LandCommandResult::kQuit) {
        returning_to_menu = true;
        break;
      }
      if (landed == LandCommandResult::kBlockedFrame) {
        resync_frame_clock();
      }
    }
    // Landing/docking approach progress (NovaUi_UpdateTravelEngagementProgress
    // 0x00459950): runs once per player frame, arms state.travel.engage_timer
    // to 0x2ee while the selected stellar is within 250 px, shows the
    // "cleared to dock/land" overlay, and expires the request back to -1.
    // Gated while a jump is engaged, mirroring the player tick.
    if (!player_tick_consumed && !state.travel.engaging) {
      NovaTravel_UpdateEngagementProgress(state);
    }
    // Target action remains the distinct DLOG 0x3f1 bribe/hostility/script
    // interaction pathway. It is intentionally not substituted for landing.
    // Mirrors Ship_HandlePlayerTargetActionCommand (0x00454910): with a ship
    // primary target the action opens the ship-comm dialog (DLOG 0x3ef); with
    // no target (or the 0x38/0x6f commands held) it opens the destination-
    // interaction window for the selected travel stellar. The player disabled/
    // destroyed gate and the target-ship "entering hyperspace" latch only
    // beep + show an overlay in the original (the clean-room shows the overlay
    // text; the beep is not modelled). Mission-ship defs use the mission
    // interaction window; an accepted single-ship escort mission can replace
    // the hailed personality ship in-place. Gated while a jump is engaged
    // (disabled through brake + hold + zoom).
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
              // mission offer renderer is the current clean-room window shell;
              // it accepts the same definition and paints over the live flight
              // frame while the state-only post-accept arm below performs the
              // replacement.
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
              // time across it.
              resync_frame_clock();
            } else {
              (void)NovaShipComm_RunShipDialog(
                  platform, state, ship_target, view, hud);
              // The comm dialog blocked the loop; freeze gameplay time.
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
        // gameplay time across it.
        resync_frame_clock();
      } else {
        NovaLog::Info("target-action: selected stellar cannot open its "
                      "destination interaction");
      }
    }
    // Board command ('b', edge-triggered): Player_HandleBoardTargetCommand
    // (0x0045a3d0). Like the original's command-latch read in
    // Ship_HandlePlayerShipCore this runs every frame regardless of jump state;
    // its own gates reject un-boardable targets. The dispatch may open the
    // boarding/plunder modal (blocking on the flight loop); the modal renders
    // the live game view beneath itself via SpaceflightView::DrawGameFrame.
    if (!player_tick_consumed && board_pressed) {
      Player_HandleBoardTargetCommand(platform, audio, state, view, hud);
      if (returning_to_menu) {
        break;
      }
      // The boarding/plunder modal blocked the loop; freeze gameplay time.
      resync_frame_clock();
    }
    // Self-destruct + cloak commands (Ghidra 0x0044aa70 PlayerTick_Interaction-
    // CloakAndStatus blocks 0x00451954..0x00451b91 and 0x00451db0..0x00451e6f).
    // Key bindings resolve through the table like the escort commands: the
    // self-destruct needs the 0x38/0x6f arm-modifier pair (both Alt scancodes)
    // plus binding slot 0x12; the cloak toggle is binding slot 0x29 (default
    // DIK 0x16 = U). Both run every frame the player core reaches them; the
    // countdown/toggle latches do their own gating.
    if (!player_tick_consumed) {
      const auto &binding_key = prefs.bindings.cmd_to_key;
      const auto held = [&platform](std::uint16_t code) {
        return code != 0xff && platform.IsOriginalKeyCodeHeld(code);
      };
      const bool arm_modifier_held = platform.IsOriginalKeyCodeHeld(0x38) ||
                                     platform.IsOriginalKeyCodeHeld(0x6f);
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
        const bool shift_held = platform.IsOriginalKeyCodeHeld(0x2a) ||
                                platform.IsOriginalKeyCodeHeld(0x36);
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
        resync_frame_clock();
      }
      PlayerTick_InteractionCloakAndStatus(
          state, held(binding_key[0x29]), frame_time_ms / kOriginalTickMs);
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
    NovaFrame_TickSystems(state,
                          audio,
                          /*run_full_tick=*/true,
                          frame_time_ms / kOriginalTickMs,
                          prefs);

    // Ghidra scope 2 "drawing": sprite world present + viewport particles +
    // commit frame.
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
        std::max(0.0F, state.ship_reticle_pulse - frame_time_ms * 1.8F);
    state.travel_reticle_pulse =
        std::max(0.0F, state.travel_reticle_pulse - frame_time_ms * 1.8F);
    // Decay the hyperspace fire flash: full white at the boom instant, gone
    // in ~60 ms (a single bright frame, matching the original's one-frame
    // centered effect 0x32).
    state.screen_flash_intensity =
        std::max(0.0F, state.screen_flash_intensity - frame_time_ms / 60.0F);
    platform.PaceFrame();
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

// External test seam into the per-ship AI decision pass (Ghidra scope 6 of
// Frame_TickSystems 0x004186b0 -> Ship_UpdateShipAI 0x00401000). Mirrors
// NovaShip_TickNpcShips so tests can model the live AI-then-movement order
// (Stub_AiRoutines runs before Stub_HandleShips in NovaFrame_TickSystems).
void NovaShip_TickNpcAi(GameState &state, float elapsed_ticks) {
  Stub_AiRoutines(state, elapsed_ticks);
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
void Math_AddPolarVelocityWithClamp(float heading_rad,
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
static bool Ship_AccelerateShipTowardPoint(GameState &state,
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
                             float elapsed_ticks,
                             const PlayerMovementOptions &opts) {
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
  // of also applying manual steering in the same tick. The face-target arm
  // likewise suppresses keyboard steering (the parent's local_265 latch gates
  // PlayerTick_TurnInput); when reverse and face-target are held together the
  // original's block ordering is unresolved (TODO(decomp)) and reverse wins
  // here.
  if (!input.reverse && !opts.face_target_armed) {
    if (input.turn_left && !input.turn_right) {
      ship.heading -= turn_rad;
      stats.turn_dir = -1;
    }
    if (input.turn_right && !input.turn_left) {
      ship.heading += turn_rad;
      stats.turn_dir = 1;
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
      stats.turn_dir = delta > 0.0F ? 1 : -1;
      delta = std::clamp(delta, -turn_rad, turn_rad);
      ship.heading = std::fmod(ship.heading + delta + kTwoPi, kTwoPi);
    }
  } else if (opts.face_target_armed) {
    // Ghidra 0x0044aa70 manual-flight auto-turn continuation: with the
    // face-target arm latched, one turn step per frame toward the stored
    // integer heading, stopping (without snapping) once the shortest delta is
    // within a single step -- the original's |delta| <= step exit rejoins the
    // keyboard path for the frame (with sVar8 = 0, i.e. no bank this frame).
    const float desired_rad =
        static_cast<float>(ship.ai_desired_heading_deg) * kDegToRad;
    const float delta = std::remainder(desired_rad - ship.heading, kTwoPi);
    if (std::abs(delta) > turn_rad) {
      ship.heading = std::fmod(
          ship.heading + std::copysign(turn_rad, delta) + kTwoPi, kTwoPi);
      stats.turn_dir = delta > 0.0F ? 1 : -1;
    }
  } else if (input.thrust) {
    if (opts.gravity_shield) {
      // Ghidra 0x0044c9ab thrust arm, gravity-shield variant: thrust
      // accumulates the scalar speed (+0x48), clamped to the effective max
      // speed; the steering block below converts it into velocity.
      ship.speed =
          std::min(ship.speed + stats.thrust_px_per_tick2 * elapsed_ticks,
                   stats.max_speed_px_per_tick);
    } else {
      // Forward thrust: polar step toward the heading, per-axis clamped to
      // the class top speed projection (Math_AddPolarVelocityWithClamp
      // semantics; the original clamps to effective max speed here and to
      // max * 1.8 while the afterburner runs -- see the cap tail in
      // PlayerTick_ManualFlightAndRegeneration for the overspeed mechanics).
      Math_AddPolarVelocityWithClamp(ship.heading,
                                     stats.thrust_px_per_tick2 * elapsed_ticks,
                                     stats.max_speed_px_per_tick,
                                     ship.vel_x,
                                     ship.vel_y);
    }
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

  if (opts.gravity_shield) {
    // Ghidra 0x0044cffe gravity-shield steering block
    // (PlayerTick_GravityShield- Steering). The scalar speed is clamped to the
    // cap global, decays by 33/34 (DAT_005755e0, a double) while
    // fire-restricted, and the velocity rotates toward heading * speed at the
    // thrust-scaled rate. The block's engine-glow ramp toward round(speed * 32
    // * 0.75 / max) capped 24 is owned by the port's glow drive (TODO(decomp)).
    const float scalar_cap = opts.speed_cap_x >= 0.0F
                                 ? opts.speed_cap_x
                                 : stats.max_speed_px_per_tick;
    if (ship.speed > scalar_cap) {
      ship.speed = scalar_cap;
    }
    if (opts.fire_restricted) {
      constexpr float kShieldFireRestrictedSpeedDamp = 33.0F / 34.0F;
      ship.speed *= kShieldFireRestrictedSpeedDamp;
    }
    NovaShip_SteerVelocityTowardShipHeading(
        ship, stats.thrust_px_per_tick2, elapsed_ticks);
  }

  // Per-axis velocity cap. Beyond the per-axis clamp applied *during* thrust
  // (Math_AddPolarVelocityWithClamp), the original player path
  // (velocity-cap block 0x0044d05b) clamps the resulting velocity vector to
  // +/-g_player_speed_cap_x/y every frame before integrating position. The
  // caps are always >= the effective max speed (see the afterburner tail in
  // PlayerTick_ManualFlightAndRegeneration); bare integrator calls fall back to
  // it.
  const float cap_x =
      opts.speed_cap_x >= 0.0F ? opts.speed_cap_x : stats.max_speed_px_per_tick;
  const float cap_y =
      opts.speed_cap_y >= 0.0F ? opts.speed_cap_y : stats.max_speed_px_per_tick;
  ship.vel_x = std::clamp(ship.vel_x, -cap_x, cap_x);
  ship.vel_y = std::clamp(ship.vel_y, -cap_y, cap_y);

  ship.pos_x += ship.vel_x * elapsed_ticks;
  ship.pos_y += ship.vel_y * elapsed_ticks;
  // Gravity-shield ships keep the scalar speed (+0x48) the thrust/steering
  // blocks maintained; the velocity is derived from it, not the reverse.
  if (!opts.gravity_shield) {
    ship.speed = std::sqrt(ship.vel_x * ship.vel_x + ship.vel_y * ship.vel_y);
  }
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
  // capability flag 0x400 before applying any other modifier. The turn-rate
  // helper (0x00463e70) has no such check (disasm-verified), so only speed and
  // thrust are zeroed here; the turn rate still runs the full NPC branch below.
  if ((ship_class.capability_flags & 0x0400U) != 0U) {
    stats.max_speed_px_per_tick = 0.0F;
    stats.thrust_px_per_tick2 = 0.0F;
  }

  // Government SkillMult applies to thrust and max speed when the ship belongs
  // to a government. Turn rate is not government-scaled. The skill-variance
  // scale precedes the government scale in the original (0x004640a0 /
  // 0x004642e0 multiply the GovtDef 0x64 SkillMult field).
  stats.max_speed_px_per_tick *= ship.skill_variance_scale;
  stats.thrust_px_per_tick2 *= ship.skill_variance_scale;
  if (ship.faction_or_government_id >= 0) {
    const Government *g = state.scenario.Government(
        static_cast<std::int16_t>(ship.faction_or_government_id + 0x80));
    if (g != nullptr) {
      stats.max_speed_px_per_tick *= g->skill_mult;
      stats.thrust_px_per_tick2 *= g->skill_mult;
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
// max speed = raw_speed/100 px/tick * government SkillMult,
// thrust = raw_accel/10000*2 px/tick^2 * government SkillMult. NPC
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

  // The movement blocks are gated off only while the maneuver timer is active
  // AND the ship is not in AI state 0x16. Ghidra Ship_HandleShip 0x00433050
  // opens the turn/thrust/bank block when
  // `ai_maneuver_timer_ms <= 0 || Ship_IsShipInAiState0x16(ship)` (the
  // class-0x2ff sentinel arm is handled by the caller's acceptance guard), so
  // a yielding (0x16) ship keeps steering even with an active timer.
  const bool coasting = ship.ai_maneuver_timer_ms > 0.0F;
  // Ghidra 0x00416070 Ship_IsShipInAiState0x16 runs inline here.
  const bool holds_course = coasting && ship.ai_state_code != 0x16;
  const bool fire_restricted = NovaAiShip_IsDisabled(state, ship);

  if (ship.arrival_monitor_active) {
    ship.arrival_monitor_elapsed_ticks += elapsed_ticks;
    const float velocity = std::hypot(ship.vel_x, ship.vel_y);
    const float settled_speed = std::max(10.0F, eff_max_speed + 1.0F);
    const bool lost_slowdown_early =
        ship.arrival_monitor_elapsed_ticks > 2.0F && ship.ai_state_code != 8 &&
        velocity > settled_speed;
    const bool remained_fast_too_long =
        ship.arrival_monitor_elapsed_ticks > 60.0F && velocity > settled_speed;
    if (!ship.arrival_monitor_warning_logged &&
        (lost_slowdown_early || remained_fast_too_long)) {
      NovaLog::Warn(
          "NPC arrival anomaly: slot={} class={} behavior={} state={} "
          "control={} velocity={:.2f} desired={:.2f} thrust={:.3f} "
          "station_hold={:.2f} maneuver={:.2f} age={:.2f} disabled={}",
          ship.ship_instance_id,
          ship.ship_class_id,
          ship.ai_behavior_code,
          ship.ai_state_code,
          ship.ai_control_mode,
          velocity,
          ship.ai_desired_speed,
          ship.ai_forward_thrust_cmd,
          ship.ai_station_hold_timer,
          ship.ai_maneuver_timer_ms,
          ship.arrival_monitor_elapsed_ticks,
          fire_restricted);
      ship.arrival_monitor_warning_logged = true;
    }
    if (velocity <= settled_speed) {
      ship.arrival_monitor_active = false;
    }
  }

  // Ship_HandleShip (0x00433050) applies the disabled/derelict damping before
  // integrating position. g_fire_restricted_ship_velocity_damp (0x00575448) is
  // the double 0.995 (NOT the 0.94 mode-1 brake damp at 0x5750f8): a disabled
  // ship keeps almost all of its velocity and drifts to a stop slowly. The
  // original applies it once per raw spaceflight call; exponentiate by the
  // time-adjusted raw-call count so the stop is frame-rate independent. It
  // also covers the gravity-shield scalar speed.
  if (fire_restricted) {
    constexpr float kFireRestrictedVelocityDamp = 0.995F; // 0x00575448
    const float damp = std::pow(kFireRestrictedVelocityDamp,
                                RawSpaceflightCallTicks(elapsed_ticks));
    ship.vel_x *= damp;
    ship.vel_y *= damp;
    ship.speed *= damp;
  }

  // --- Position integration + inertia-less special case. ---
  // Ship_HandleShip integrates the velocity inherited from the previous frame
  // before calculating this frame's steering/thrust. This ordering matters for
  // arrivals: the first state-8 frame installs the emergence velocity but does
  // not move the ship away from the gate until the following frame.
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
    if (!gravity_shield) {
      ship.speed = std::sqrt(ship.vel_x * ship.vel_x + ship.vel_y * ship.vel_y);
    }
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
  // Ghidra gates the whole turn/regen block on `!Ship_IsShipDisabled(ship) &&
  // !g_gameplay_time_frozen` (uVar10, 0x00433050): a disabled NPC holds its
  // current heading (no rotation) and does not regenerate. The inner arm then
  // additionally needs `ai_maneuver_timer_ms <= 0 || Ship_IsShipInAiState0x16`.
  if (!fire_restricted && !holds_course) {
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

    // Regeneration shares the block's disabled gate; lethal hits must not
    // resurrect a ship whose armor has reached zero.
    if (!NovaAiShip_IsDestroyed(ship)) {
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
      Math_AddPolarVelocityWithClamp(
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
      // once per rendered frame without g_avg_frame_tick_scale. Its gameplay
      // loop waits for a minimum 21 ms frame (Frame_MeasureFrameTiming
      // 0x00432ea0), so normalize the decay to that maximum-rate 47.62 Hz
      // behavior while keeping the clean-room result independent of FPS.
      ship.ai_desired_speed += std::abs(ship.ai_forward_thrust_cmd) *
                               elapsed_ticks / kOriginalMaxRateFrameTicks;
      // Ship_HandleShip (0x00433050) hands the ship to
      // Ship_ResetShipPrimaryAndSecondaryTargets once the desired speed has
      // risen above the negative effective max speed. The threshold uses
      // min(self, lead target) when a valid lead (0..0x3f) is followed
      // (0x00433b26-0x00433b8e). State 8 begins at -50 and advances ~1.165
      // per movement tick before the reset returns the ship to idle and arms
      // the 30..59-tick coast-through timer (armed once: the reset clears the
      // desired speed and thrust command, so the branch is not re-entered).
      float threshold_max_speed = eff_max_speed;
      if (ship.squad_leader_ship_slot >= 0 &&
          ship.squad_leader_ship_slot <= 0x3f &&
          state.SlotInRange(
              static_cast<std::size_t>(ship.squad_leader_ship_slot))) {
        const Ship &lead =
            state.ShipAt(static_cast<std::size_t>(ship.squad_leader_ship_slot));
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
        if (ship.squad_leader_ship_slot == -1 && ship.pers_def_slot != 0x3ff) {
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
  bool jump_glow_active = false;
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
        jump_glow_active = true;
      }
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
  // decrement while not thrusting (LAB_00435197) and while coasting through a
  // reversal (the closed main gate and the tail decrement collapse to one),
  // and gets an extra +2 while banking into a turn (ai_turn_bias_dir set and
  // class sprite_behavior_flags bit 2), with no upper clamp. The renderer's
  // per-frame random flicker/hide threshold lives in
  // NovaShip_TickWeaponSpriteAndRunningLights (Ship_UpdateVisualState).
  // These are integer mutations made once per raw Ship_HandleShip call. Bank
  // fractional normalized time and replay the original ordered state machine
  // at the loop's 21 ms maximum-rate cadence. In particular, the jump +3
  // precedes the banking +2 and ordinary thrust/fade branch in each replay.
  ship.engine_glow_raw_tick_accumulator += elapsed_ticks;
  constexpr float kGlowCadenceEpsilon = 1e-6F;
  const int glow_tick_count = static_cast<int>(
      (ship.engine_glow_raw_tick_accumulator + kGlowCadenceEpsilon) /
      kOriginalMaxRateFrameTicks);
  ship.engine_glow_raw_tick_accumulator -=
      static_cast<float>(glow_tick_count) * kOriginalMaxRateFrameTicks;
  ship.engine_glow_raw_tick_accumulator =
      std::max(0.0F, ship.engine_glow_raw_tick_accumulator);
  for (int glow_tick = 0; glow_tick < glow_tick_count; ++glow_tick) {
    std::int16_t &glow = ship.engine_glow_level;
    if (jump_glow_active) {
      glow =
          static_cast<std::int16_t>(std::min(0x20, static_cast<int>(glow) + 3));
    }
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
      // Ghidra adds +2 with no upper clamp (0x004350ee), so the level can
      // overshoot 0x18; the normal thrust branch pulls it back one step on
      // later frames.
      if (glow < 0x18) {
        glow = static_cast<std::int16_t>(glow + 2);
      }
    }
    if (ship.ai_forward_thrust_cmd <= 0.0F) {
      fade_to_zero();
    } else if (holds_course) {
      // Thrust command present but the maneuver timer is coasting the ship
      // through a reversal: Ghidra's inner check `timer > 0 && !state0x16`
      // jumps to LAB_00435197, the same single decrement the closed main gate
      // would otherwise take through the tail block.
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
  }
  ship.engine_glow_intensity = std::clamp(
      static_cast<float>(ship.engine_glow_level) / 24.0F, 0.0F, 1.0F);

  // Ship_HandleShip 0x00435464..0x004354ae: player-owned hits build this
  // retarget-pressure accumulator in Shot_ResolveShipHitFromWeapon. Its
  // passive decay is normalized simulation time (g_avg_frame_tick_scale),
  // unlike the nearby per-call engine-glow mutations: subtract 0.5 per tick
  // and clamp a crossing to zero.
  if (ship.player_aggro_accumulator > 0.0F) {
    ship.player_aggro_accumulator =
        std::max(0.0F, ship.player_aggro_accumulator - 0.5F * elapsed_ticks);
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

// Jump-arrival call site note: the in-flight jump-arrival block of
// Ship_HandlePlayerShipCore (PlayerTick_SystemTransitionAndArrival
// 0x0044f660) also runs Frame_JitterPlayerStatModifiers + Frame_RerollPlayer-
// StatModifiers at 0x0044fb39, after the escort travel-day daily passes. The
// port's jump-arrival does not run them yet (see the TODO in the loop's
// just_completed block).

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

// Jump-arrival call-site note for the pair below: the in-flight jump-arrival
// block of Ship_HandlePlayerShipCore (PlayerTick_SystemTransitionAndArrival
// 0x0044f660) also runs both functions at 0x0044fb39, after the escort
// travel-day daily passes. The port's jump-arrival does not run them yet
// (see the TODO in the loop's just_completed block).

// Ghidra 0x00431480 Frame_JitterPlayerStatModifiers. The two-step jitter and
// the [85,115] clamp are branch-faithful (NovaRandom_Range(3): 0 -> -1,
// 1 -> +1, 2 -> unchanged).
void NovaFrame_JitterPlayerStatModifiers(GameState &state) {
  for (std::size_t i = 0; i < 2; ++i) {
    const int roll = RollRandom(state, 3);
    if (roll == 0) {
      --state.player_stat_modifier_pct[i];
    } else if (roll == 1) {
      ++state.player_stat_modifier_pct[i];
    }
    state.player_stat_modifier_pct[i] =
        std::clamp<std::int16_t>(state.player_stat_modifier_pct[i], 0x55, 0x73);
  }
}

// Ghidra 0x00431500 Frame_RerollPlayerStatModifiers: [90,114] percent.
void NovaFrame_RerollPlayerStatModifiers(GameState &state) {
  for (std::size_t i = 2; i < 4; ++i) {
    state.player_stat_modifier_pct[i] =
        static_cast<std::int16_t>(RollRandom(state, 0x15) + 0x5a);
  }
}

// Ghidra PlayerTick_TimedActionTransition support: the eject/escape-pod
// transform arm (Ship_HandlePlayerShipCore 0x004510b9..0x00453910), the
// Ship_ResetPlayerShipState (0x004b3350) respawn reset, and the
// Stellar_FindValidRespawnStellar flood (0x00467710 / 0x004677a0).
// ---------------------------------------------------------------------------
namespace {

// Bible oütf ModType entries driving the eject gate.
constexpr std::int16_t kAutoEjectOutfitModType =
    0x14; // ModType 20 "auto-eject"
constexpr std::int16_t kEscapePodOutfitModType =
    0x0b; // ModType 11 "escape pod"
// Launch-bay weapons: weapon_mode_code 99; the carried craft class is
// ammo_type - 0x80 and must set shïp Flags 0x8000 (escape ship type).
constexpr std::int16_t kBayWeaponModeCode = 99;
constexpr std::uint16_t kEscapeShipClassFlag = 0x8000;
// The escape pod ship class: decompile constant 0x2ff is the zero-based index
// (ship id 0x37f).
constexpr std::int16_t kEscapePodShipClassIndex = 0x2ff;
// The escape-pod flight arms the blocking timed action for 0x15e ticks.
constexpr std::int16_t kEscapePodTimedActionTicks = 0x15e;
// Eject gate: the death presentation must be at least half spent
// (death_timer <= ShipClass.death_delay_frames * g_bomb_damage_armor_fraction
// DAT_00575598 = 0.5, the same global the bomb self-damage roll uses) or be
// within g_hyperspace_engage_hold_30hz (0x5755a8, 30) ticks of ending.
constexpr float kEjectDeathDelayScale = kBombDamageArmorFraction;
constexpr float kEjectArmHoldTicks = 30.0F;
// Respawn catch-up: rand(30) + 15 Mission_TickDailyWorldUpdate passes.
constexpr std::int16_t kRespawnDailyUpdateRange = 30;
constexpr std::int16_t kRespawnDailyUpdateBase = 15;
// STR# 0x7d2 entry 0x34: "You abandon your ship for" (fighter variant).
constexpr std::uint16_t kStrAbandonShipFor = 0x34;
// The 0x100 player weapon banks store their live counter at slot 0 of a
// 100-int16 stride.
constexpr std::size_t kPlayerBankStride = 100;

// Ghidra Outfit_HasAutoEjectOutfit (0x00464700): any owned outfit with
// ModType 0x14 (Bible "auto-eject", which requires an escape pod to work).
bool Outfit_HasAutoEjectOutfit(const GameState &state) {
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    if (state.inventory.outfit_owned_count[idx] > 0 &&
        FindOutfitWithModType(state, idx, kAutoEjectOutfitModType) != nullptr) {
      return true;
    }
  }
  return false;
}

// Ghidra Weapon_HasLaunchBayWeapon (0x00464520), player specialization:
// the canonical Ship-taking port lives in weapon.cpp
// (NovaWeapon_HasLaunchBayWeapon); the player's banks are the GameState
// strided arrays, reached via ships[0].
bool Weapon_HasPlayerLaunchBayWeapon(const GameState &state) {
  return NovaWeapon_HasLaunchBayWeapon(state, state.player);
}

// Ghidra ShipClass_FindLaunchBayShipClassId (0x00464590): the zero-based class
// index of the first ejectable bay fighter, -1 when none.
std::int16_t Weapon_FindLaunchBayShipClassIndex(const GameState &state) {
  for (std::size_t bank = 0; bank < 0x100; ++bank) {
    const Weapon *def =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (def == nullptr || def->weapon_mode_code != kBayWeaponModeCode) {
      continue;
    }
    if (state.weapon_bank_ammo[bank * kPlayerBankStride] < 1) {
      continue;
    }
    if (def->ammo_type < 0x80) {
      continue;
    }
    const ShipClass *carried = state.scenario.Ship(def->ammo_type);
    if (carried != nullptr &&
        (carried->capability_flags & kEscapeShipClassFlag) != 0) {
      return static_cast<std::int16_t>(def->ammo_type - 0x80);
    }
  }
  return -1;
}

// Ghidra Outfit_HasSpecialMovementOutfitOrLaunchBay (0x004644a0, renamed
// Outfit_HasEscapePodOrLaunchBay): an owned escape-pod outfit (ModType 0xb)
// or an ejectable launch bay.
bool Outfit_HasEscapePodOrLaunchBay(const GameState &state) {
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    if (state.inventory.outfit_owned_count[idx] > 0 &&
        FindOutfitWithModType(state, idx, kEscapePodOutfitModType) != nullptr) {
      return true;
    }
  }
  return Weapon_HasPlayerLaunchBayWeapon(state);
}

// Zeroes the live (slot 0) counter of every player weapon bank and the per-
// bank cooldown/burst state before a stock reseed (the original sweeps all
// 0x100 banks in the eject/respawn paths).
void ZeroPlayerWeaponBanks(GameState &state) {
  for (std::size_t bank = 0; bank < 0x100; ++bank) {
    state.weapon_bank_ammo[bank * kPlayerBankStride] = 0;
    state.weapon_bank_secondary[bank * kPlayerBankStride] = 0;
  }
  state.weapon_bank_cooldown.fill(0.0F);
  state.weapon_bank_burst_counter.fill(0);
  state.active_shots.clear();
}

// Drops non-persistent outfits (OutfitDef field_0x378 / Flags 0x0004 survive
// a ship replacement).
void ClearNonPersistentOutfits(GameState &state) {
  for (std::size_t idx = 0; idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    const Outfit *def = idx < state.scenario.outfits.size()
                            ? &state.scenario.outfits[idx]
                            : nullptr;
    if (def == nullptr || !def->persistent_on_ship_swap) {
      state.inventory.outfit_owned_count[idx] = 0;
    }
  }
}

// Ghidra Ship_ResetPlayerShipState (0x004b3350), param_1 == 0 (the respawn
// call). Fresh kinematics, the hardcoded fresh class 0, recomputed meters and
// cleared targeting/AI/travel state.
void RespawnResetPlayerShipState(GameState &state) {
  PlayerShip &p = state.player;
  p.pos_x = 50.0F;
  p.pos_y = 50.0F;
  p.vel_x = 0.0F;
  p.vel_y = 0.0F;
  p.speed = 0.0F;
  p.heading = 0.0F;
  p.ship_class_id = 0; // the reset always lands the player in class 0 (id 0x80)
  p.is_active = true;
  p.cloak_transition_latch = 0;
  p.cloak_fade_progress = 0.0F;
  p.travel_transfer_mode = -1;
  p.faction_or_government_id = -1;
  p.ai_behavior_code = -1;
  p.current_system_id = 0; // overwritten by the emergency destination

  // Meters are computed BEFORE the outfit counts are cleared (original
  // order), so the pre-death outfit bonuses still apply to class 0 here.
  state.stat_cache_valid = false;
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  p.shield_points = eff.max_shield_points;
  p.armor_points = eff.max_armor_points;
  p.fuel_points = eff.fuel_capacity;
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  p.primary_target_ship_slot = -1;
  p.ai_secondary_target_slot = -1;
  p.active_weapon_bank_slot = -1;
  p.squad_leader_ship_slot = -1;
  p.mission_fleet_slot = -1;
  p.pers_def_slot = -1;
  p.ai_control_mode = 0;
  p.ai_state_code = 0;
  p.death_timer_active = -999.0F;
  p.death_timer_seeded = false;
  p.destruction_finale_triggered = false;
  p.destruction_visual_triggered = false;
  p.destruction_raw_tick_accumulator = 0.0F;
  p.ionization_points = 0.0F;
  // TODO(decomp(0x004b3350)) skipped: field_0xb0/field_0x60 latch words,
  // weapon_exit_animation_phase, alternate_sprite_cycle_index and
  // last_weapon_fire_time_ms - not modelled on Ship.
  p.sprite_animation_cycle_index = 0;
  p.waypoint_arrival_marker_a = 1;
  p.waypoint_arrival_marker_b = 0;
  p.turn_bank_animation_phase = 0.0F;
  p.ai_turn_bias_dir = 0;
  p.shield_bubble_flash_intensity = 0.0F;
  p.player_aggro_accumulator = 0.0F;
  p.sprite_animation_timer = 0.0F;
  p.skill_variance_scale = 1.0F;
  p.ai_station_hold_timer = 0.0F;
  p.jump_destination_system_id = -1;
  p.engine_glow_level = 0;
  p.velocity_match_target_ship_slot = -1;

  ZeroPlayerWeaponBanks(state);
  NovaWeapon_SeedBanksFromShipStock(state, p.ship_class_id);

  // The plotted starmap route (DAT_00735404, 0x20 slots).
  state.travel.starmap_route.fill(-1);

  ClearNonPersistentOutfits(state);
  state.inventory.cargo_bins.fill(0);
  state.inventory.junk_counts.fill(0);

  // TODO(decomp(0x004b3350)) skipped: DAT_007353f4, g_last_system_for_ambient_
  // rolls, _g_playerSelfDestructCountdown,
  // g_travel_interaction_action_index_b / bribe_random_latch, DAT_00596d32/33
  // and g_travel_countdown - not modelled yet.
  state.travel.selected_stellar_id = -1;
  state.travel.engage_timer = -1;
  state.travel.travel_hint_state = 0x7fff; // hint latch (0x004b3a3b)
  state.distress_cue_active = false;
  state.distress_cue_active_prev = false;
  p.timed_action_counter = -1;

  Mission_RerollOfferingRolls(state);

  // Impact-effect instance timers back to the inactive sentinel.
  for (auto &effect : state.impact_effect_instances) {
    effect.anim_time = -1.0F;
    effect.delay_timer = 0.0F;
  }
  // TODO(decomp(0x004b3350)) skipped: reset the unimplemented 64-entry weapon
  // smoke-puff pool used by Shot_SpawnWeaponSmokePuff (0x004215d0) and
  // Shot_UpdateWeaponSmokePuffs (0x0042c660). No stock Nova weapon enables it;
  // it remains relevant to plug-ins using Flags 0x0200/0x0400 and SmokeSet.

  // Reroll the per-class licensed availability rolls.
  for (auto &roll : state.ship_class_limit_rolls) {
    roll = static_cast<std::int16_t>(RollRandom(state, 100) + 1);
  }
  for (auto &roll : state.ship_class_threshold_rolls) {
    roll = static_cast<std::int16_t>(RollRandom(state, 100) + 1);
  }
}

// Ghidra 0x00467710/0x004677a0 Stellar_FindValidRespawnStellar{,Recursive}:
// the escape-pod respawn destination lookup. Depth-first flood over visible,
// discovered neighbour systems for an available, travel-usable, ship-selling
// (travel_flags & 8) stellar whose reputation threshold the containing system
// meets. Returns the stellar resource id, or -1 when no neighbour qualifies.
// The start system's own stellars are never scanned (you always respawn
// elsewhere).
std::int16_t Stellar_FindValidRespawnStellar(GameState &state,
                                             std::int16_t origin_zero_based) {
  const auto &systems = state.scenario.systems;
  std::vector<char> visited(systems.size(), 0);
  if (origin_zero_based < 0 ||
      static_cast<std::size_t>(origin_zero_based) >= systems.size()) {
    return -1;
  }

  auto neighbour_eligible = [&](std::size_t idx) -> bool {
    const System &sys = systems[idx];
    return sys.is_visible && sys.discovered_this_rebuild &&
           sys.discovery_state > 0;
  };
  auto scan_stellars = [&](std::size_t idx) -> std::int16_t {
    const System &sys = systems[idx];
    for (std::int16_t nav : sys.nav_defs) {
      if (nav < 0x80) {
        continue;
      }
      const Stellar *st = state.scenario.Stellar(nav);
      if (st == nullptr || !st->is_available ||
          !NovaTargeting_IsStellarUsableForTravel(*st) ||
          (st->flags & 8) == 0 || st->min_status == 0x7fff) {
        continue;
      }
      const std::int16_t reputation = idx < state.system_reputation.size()
                                          ? state.system_reputation[idx]
                                          : 0;
      if (st->min_status <= reputation || st->min_status == -0x7fff) {
        return nav;
      }
    }
    return -1;
  };

  // Recursive flood, matching the original's two passes per system: scan
  // neighbours for an eligible stellar first, then recurse into them.
  std::function<std::int16_t(std::size_t)> recurse =
      [&](std::size_t idx) -> std::int16_t {
    visited[idx] = 1;
    const System &sys = systems[idx];
    for (std::int16_t link : sys.links) {
      if (link < 0x80) {
        continue;
      }
      const std::size_t next = static_cast<std::size_t>(link - 0x80);
      if (next >= systems.size() || visited[next] != 0 ||
          !neighbour_eligible(next)) {
        continue;
      }
      if (const std::int16_t found = scan_stellars(next); found >= 0x80) {
        return found;
      }
    }
    for (std::int16_t link : sys.links) {
      if (link < 0x80) {
        continue;
      }
      const std::size_t next = static_cast<std::size_t>(link - 0x80);
      if (next >= systems.size() || visited[next] != 0 ||
          !neighbour_eligible(next)) {
        continue;
      }
      if (const std::int16_t found = recurse(next); found >= 0x80) {
        return found;
      }
    }
    return -1;
  };

  visited[static_cast<std::size_t>(origin_zero_based)] = 1;
  return recurse(static_cast<std::size_t>(origin_zero_based));
}

// Ghidra Ship_HandlePlayerShipCore synthetic CFG: eject/escape-pod transition
// 0x00451024 -> 0x00451630. The destroyed player's eject transform spawns the
// derelict wreck of the old
// hull, runs the old class's OnRetire expression (ShipClassDef+0x4e8 <- shp
// payload 0x4cf), then rebuilds the player as either the escape pod (ship id
// 0x37f, arming the 0x15e-tick respawn timed action) or a carried bay fighter
// (Flags 0x8000 class at 50..79% stats, no timed action), relaunches at top
// speed along the current heading and re-seeds loadout/meters.
void RunPlayerEjectTransform(GameState &state) {
  PlayerShip &p = state.player;

  // Derelict wreck of the abandoned hull (Ship_AllocateShipSlotInSystem
  // (current_system, 0)); combat AI re-targets it below.
  const std::int16_t wreck_slot = static_cast<std::int16_t>(
      NovaShip_AllocateShipSlot(state, p.current_system_id, 0));
  if (wreck_slot >= 0) {
    Ship &wreck = state.ShipAt(static_cast<std::size_t>(wreck_slot));
    wreck.ai_behavior_code = -1;
    wreck.pos_x = p.pos_x;
    wreck.pos_y = p.pos_y;
    wreck.vel_x = p.vel_x;
    wreck.vel_y = p.vel_y;
    wreck.heading = p.heading;
    wreck.ship_class_id = p.ship_class_id;
    wreck.death_timer_active = p.death_timer_active;
    wreck.ionization_points = 0.0F;
    // TODO(decomp(0x00451130)) skipped: field_0xb0/field_0x60 latch words,
    // weapon_exit_animation_phase, alternate_sprite_cycle_index,
    // sprite_animation_cycle_index, ai_turn_bias_dir, sprite_animation_timer
    // wreck copies and the sprite-set assignment - partly unmodelled.
    wreck.shield_points = p.shield_points;
    wreck.armor_points = p.armor_points;
    wreck.boarded_target_latch = 1;
  }

  p.death_timer_active = -1.0F;
  p.death_timer_seeded = false;
  p.destruction_finale_triggered = false;
  p.destruction_visual_triggered = false;
  p.destruction_raw_tick_accumulator = 0.0F;
  p.primary_target_ship_slot = -1;
  p.ai_secondary_target_slot = -1;
  p.active_weapon_bank_slot = -1;

  // Old ship class's OnRetire reaction script.
  if (const ShipClass *old_cls = state.scenario.Ship(
          static_cast<std::int16_t>(p.ship_class_id + 0x80))) {
    (void)Mission_ExecuteReactionScript(state, old_cls->on_retire_expr);
  }

  if (!Weapon_HasPlayerLaunchBayWeapon(state)) {
    // Escape pod: full base shields/armor, 350-tick respawn countdown, and
    // every ship targeting the player drops it.
    p.ship_class_id = kEscapePodShipClassIndex;
    if (const ShipClass *pod = state.scenario.Ship(0x37f)) {
      p.shield_points = static_cast<float>(pod->base_shield);
      p.armor_points = static_cast<float>(pod->base_armor);
    } else {
      NovaLog::Todo("escape pod ship class 0x37f not in scenario tables; "
                    "pod stats left unchanged");
    }
    p.timed_action_counter = kEscapePodTimedActionTicks;
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      Ship &ship = state.ShipAt(slot);
      if (ship.is_active && ship.primary_target_ship_slot == 0) {
        ship.primary_target_ship_slot = -1;
      }
    }
  } else {
    // Bay fighter: 50..79% of the carried class's base stats, no respawn
    // timed action, and the "You abandon your ship for <class>." overlay.
    const std::int16_t fighter_index =
        Weapon_FindLaunchBayShipClassIndex(state);
    const ShipClass *fighter =
        fighter_index >= 0 ? state.scenario.Ship(static_cast<std::int16_t>(
                                 fighter_index + 0x80))
                           : nullptr;
    if (fighter == nullptr) {
      NovaLog::Todo("eject: launch bay present but no Flags-0x8000 fighter "
                    "class resolved; staying in the current class");
    } else {
      p.ship_class_id = fighter_index;
      p.shield_points = static_cast<float>((RollRandom(state, 30) + 50) *
                                           fighter->base_shield) *
                        0.01F;
      p.armor_points = static_cast<float>((RollRandom(state, 30) + 50) *
                                          fighter->base_armor) *
                       0.01F;
      p.fuel_points = static_cast<float>((RollRandom(state, 30) + 50) *
                                         fighter->base_fuel) *
                      0.01F;
      p.timed_action_counter = -1;
      std::string text =
          NovaHud_LoadStringEntry(kStringListFlightText, kStrAbandonShipFor)
              .value_or("") +
          " " + fighter->display_name + ".";
      NovaHud_ShowOverlayMessage(
          state, text, 0xe0, 0xe0, 0xe0, kOverlayDurationAutoRepair);
    }
  }

  // Common tail: NPC retarget pass, launch velocity, loadout rebuild.
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active) {
      continue;
    }
    if (!NovaTargeting_IsShipEligibleForDistressCall(state, ship)) {
      if (ship.squad_leader_ship_slot == 0 &&
          p.ship_class_id == kEscapePodShipClassIndex) {
        ship.squad_leader_ship_slot = -1;
      }
    } else {
      // Combat AI switches onto the fresh wreck.
      if (ship.primary_target_ship_slot == 0) {
        ship.primary_target_ship_slot = wreck_slot;
      }
      if (ship.ai_secondary_target_slot == 0) {
        ship.ai_secondary_target_slot = wreck_slot;
      }
    }
  }
  // TODO(decomp(0x00453830)) skipped: Sprite_AssignSpriteSet / frame reset
  // (the clean-room sprite layer re-derives visuals from the class id).

  // Relaunch: velocity reset, then a full-speed polar step along the heading
  // (Math_AddPolarVelocity 0x0043b4a0, unclamped).
  p.vel_x = 0.0F;
  p.vel_y = 0.0F;
  state.stat_cache_valid = false;
  const PlayerEffectiveStats launch_eff =
      Outfit_ComputePlayerEffectiveStats(state);
  state.cached_stats = launch_eff;
  state.stat_cache_valid = true;
  p.vel_x += std::sin(p.heading) * (launch_eff.speed_raw / 100.0F);
  p.vel_y -= std::cos(p.heading) * (launch_eff.speed_raw / 100.0F);

  // Loadout rebuild: non-persistent outfits are lost, weapon banks reseeded
  // from the new class's stock, and the bank/outfit pools reconciled.
  ClearNonPersistentOutfits(state);
  ZeroPlayerWeaponBanks(state);
  NovaWeapon_SeedBanksFromShipStock(state, p.ship_class_id);
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
  if (const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(p.ship_class_id + 0x80))) {
    for (std::size_t i = 0; i < cls->default_outfit_ids.size(); ++i) {
      const std::int16_t id = cls->default_outfit_ids[i];
      if (id >= 0x80 && cls->default_outfit_counts[i] > 0 &&
          static_cast<std::size_t>(id - 0x80) <
              state.inventory.outfit_owned_count.size()) {
        state.inventory.outfit_owned_count[id - 0x80] =
            static_cast<std::int16_t>(
                state.inventory.outfit_owned_count[id - 0x80] +
                cls->default_outfit_counts[i]);
      }
    }
  }

  // Launch cue: NovaAudio_QueueCenteredSound(DAT_00591a80, 0x32, ...).
  // TODO(decomp(0x00453810)) skipped: the DAT_00591a80 sound handle is not
  // mapped to the port's transition/impact tables yet.
  state.warp_up_sound_pending = false;
  state.warp_out_sound_pending = false;
  p.ai_station_hold_timer = 0.0F;
  p.death_timer_active = -1.0F;
  // The original's timed-action dispatch has already run when the eject
  // transform arms the countdown; suppress the port's timed-action tick for
  // this frame (consumed by the spaceflight loop).
  state.timed_action_suppress_this_frame = true;
}

} // namespace

// Ghidra 0x0044B240 PlayerTick_StatusAndOutfitEvents, internal label of
// Ship_HandlePlayerShipCore. Synthetic CFG: 0x0044B240 ->
// [0x0044B7C4, 0x0044D490]; includes the reordered carried-bomb tails. Runs
// after the hyperspace-exit gate and before every flight-input branch.
bool PlayerTick_StatusAndOutfitEvents(GameState &state,
                                      float elapsed_ticks,
                                      bool eject_command) {
  PlayerShip &p = state.player;

  // --- Player-death bookkeeping (Ship_HandlePlayerShipCore prologue) ------
  // Ship_IsShipDestroyed (0x004688e0: armor <= 0 OR timer running) gates the
  // eject arm, matching the original's check at 0x0045392e -- so armor-only
  // destruction (self-destruct, script kills, collision blasts) enters the
  // sequence too. The presentation itself is owned by Ship_UpdateVisualState
  // (0x00428340), which Frame_TickSystems scope 10 calls on the player right
  // after this core: it seeds the timer (x3 for the player,
  // g_player_death_timer_scale 0x00575378), drives the Explode1 debris cascade
  // while the timer is above 2.0, and runs the Explode2 finale/boom at 2.0.
  // This core only ticks the timer, exactly like the original at 0x004522f3.
  if (p.is_active && NovaAiShip_IsDestroyed(p)) {
    // Fire-restricted damping (0x0044b240, before the eject arm): a destroyed
    // hull is fire-restricted, so the original bleeds 0.5% of each velocity
    // component per raw call here. Exponentiate by the time-adjusted raw-call
    // count so the port matches the original 21 ms maximum-rate cadence. The
    // coast integration runs later in the loop.
    const float restricted_damp =
        std::pow(kPlayerFireRestrictedVelocityDamp,
                 RawSpaceflightCallTicks(elapsed_ticks));
    p.vel_x *= restricted_damp;
    p.vel_y *= restricted_damp;
    // PlayerTick eject block (0x004510b9..0x00453910): while the death
    // presentation runs, the eject command (0x38/0x6f arm pair + binding
    // slot 0x11) with an owned auto-eject outfit transforms the player into
    // the escape pod or a carried bay fighter. Allowed once the presentation
    // is at least half spent (or within 30 ticks of ending).
    if (eject_command && Outfit_HasAutoEjectOutfit(state) &&
        Outfit_HasEscapePodOrLaunchBay(state)) {
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(p.ship_class_id + 0x80));
      const float death_delay =
          cls != nullptr ? static_cast<float>(cls->death_delay_frames) : 0.0F;
      if (p.death_timer_active <= death_delay * kEjectDeathDelayScale ||
          p.death_timer_active <= kEjectArmHoldTicks) {
        RunPlayerEjectTransform(state);
        return true;
      }
    }
    // Ship_UpdateVisualState, called immediately after the core, replays the
    // raw-call timer decrement together with its discrete Explode1 RNG pass.
    return true;
  }

  // --- Inactive branch (0x0044aa70 prologue at 0x0044af14) -----------------
  if (!p.is_active) {
    p.destruction_raw_tick_accumulator +=
        std::max(0.0F, RawSpaceflightCallTicks(elapsed_ticks));
    // While the timer is above DAT_00575558 (-240.0) the hull is gone but the
    // spaceflight loop keeps presenting: the Explode2 area effect spawned by
    // the finale (and the fading debris pool) finish playing before the core
    // latches DAT_00596d38. Only once the timer reaches the floor does the
    // inactive branch fall through to the escape-pod scan / game-over latch.
    if (p.death_timer_active > kPlayerDeathInactiveTimerFloor) {
      while (p.destruction_raw_tick_accumulator + 1.0e-6F >= 1.0F &&
             p.death_timer_active > kPlayerDeathInactiveTimerFloor) {
        p.destruction_raw_tick_accumulator -= 1.0F;
        p.death_timer_active -= kJumpTurnaroundTurnRateAddend;
      }
      p.destruction_raw_tick_accumulator =
          std::max(0.0F, p.destruction_raw_tick_accumulator);
      return true;
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
    const float restricted_damp =
        std::pow(kPlayerFireRestrictedVelocityDamp,
                 RawSpaceflightCallTicks(elapsed_ticks));
    p.vel_x *= restricted_damp;
    p.vel_y *= restricted_damp;
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

// Ghidra 0x0044D490 PlayerTick_TimedActionTransition, internal label of
// Ship_HandlePlayerShipCore. Synthetic CFG: 0x0044D490 -> 0x0044D560,
// including the reordered zero-count respawn branch at 0x0044D570..0x0044DA74;
// 0x0044D560 is the nonzero-count continuation. Reached from status/outfit
// handling while timed_action_counter > 0; consumes the remaining player tick.
bool PlayerTick_TimedActionTransition(GameState &state, float elapsed_ticks) {
  PlayerShip &p = state.player;
  if (p.timed_action_counter <= 0) {
    return false;
  }

  // Blocking movement: full effective thrust along the current heading,
  // per-axis polar-clamped at the effective top speed (Math_AddPolarVelocity-
  // WithClamp 0x0043b4e0), then the position integration.
  if (!state.stat_cache_valid) {
    state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
    state.stat_cache_valid = true;
  }
  const PlayerEffectiveStats &eff = state.cached_stats;
  const float max_speed = eff.speed_raw / 100.0F;
  const float thrust_step = eff.thrust_raw / 10000.0F * 2.0F * elapsed_ticks;
  Math_AddPolarVelocityWithClamp(
      p.heading, thrust_step, max_speed, p.vel_x, p.vel_y);
  p.pos_x += p.vel_x * elapsed_ticks;
  p.pos_y += p.vel_y * elapsed_ticks;

  p.timed_action_counter =
      static_cast<std::int16_t>(p.timed_action_counter - 1);
  if (p.timed_action_counter != 0) {
    return true;
  }

  // --- Countdown reached zero: the death/escape-pod respawn transition ----
  // TODO(decomp(0x0044d570)) skipped: the blocking presentation passes -
  // Frame_CommitFrameAndLatchTransitionWait / Frame_FinishBlockingTransition-
  // Frame, the black DrawContext wipes, NovaEffects ambient-star clear/queue,
  // SWParticles_ResetEntries, SpriteWorld_ReleaseAllSpriteFrames,
  // NovaPlatform_EnsureCursorVisible, and the dësc 13999 death dialog
  // (Ui_LoadSelectionDialogResource + Ui_RunTravelSelectionDialog; dësc 13999
  // is the Bible "message shown after the player uses an escape pod").
  NovaLog::Info("escape-pod respawn: blocking transition (dësc 13999 dialog "
                "not reconstructed)");

  // Abort every active mission (Mission_ClearMisnSlotAssignments(slot, 1)).
  const std::uint32_t now_ms =
      static_cast<std::uint32_t>(state.gameplay_now_ms);
  for (std::size_t slot = 0; slot < state.active_mission_runtime_flags.size();
       ++slot) {
    if (state.active_mission_runtime_flags[slot].is_active) {
      Mission_ClearMisnSlotAssignments(state,
                                       static_cast<std::int16_t>(slot),
                                       /*emit_completion_payload=*/true,
                                       now_ms);
      state.active_mission_runtime_flags[slot].is_active = false;
    }
  }

  // Ship_ResetPlayerShipState(0) + the fresh (class 0) ship's OnPurchase
  // reaction script (ShipClassDef+0x1eb <- shp payload 0x26a).
  RespawnResetPlayerShipState(state);
  if (const ShipClass *cls = state.scenario.Ship(0x80)) {
    (void)Mission_ExecuteReactionScript(state, cls->on_purchase_expr);
  }
  // Ship_DeactivateVacantShipsAndTally(1, 1): the clean-room models the
  // second flag through keep_player_engaged.
  NovaShip_DeactivateVacantShipsAndTally(state, /*keep_player_engaged=*/true);

  // Relocate to the nearest reachable emergency destination (a ship-selling,
  // reputation-acceptable stellar in a visible, discovered neighbour system).
  const std::int16_t dest_stellar =
      Stellar_FindValidRespawnStellar(state, p.current_system_id);
  if (dest_stellar < 0x80) {
    p.current_system_id = 0;
  } else if (const Stellar *st = state.scenario.Stellar(dest_stellar);
             st != nullptr && st->system_id >= 0) {
    p.current_system_id = st->system_id;
  } else {
    p.current_system_id = 0;
  }
  if (const System *sys = state.scenario.System(
          static_cast<std::int16_t>(p.current_system_id + 0x80))) {
    // Starmap pan origin re-centres on the new system.
    state.starmap_pan_x = static_cast<float>(sys->pos_x);
    state.starmap_pan_y = static_cast<float>(sys->pos_y);
  }

  // Weapon banks reloaded from the fresh class's stock (the reset already
  // reseeded them; the original re-runs the sweep here).
  ZeroPlayerWeaponBanks(state);
  NovaWeapon_SeedBanksFromShipStock(state, p.ship_class_id);

  // System_RebuildSystemVisibilityMap(cur, 0, 1).
  NovaSystem_RebuildDiscoveryState(state, p.current_system_id, 0, 1);
  // TODO(decomp(0x0044d814)) skipped: NovaEffects_QueuedAmbientStarParticles.

  // Meters refill. Original quirk (0x0044d83f, verified: both call sites
  // target 0x00463550): the ARMOR refill uses Ship_ComputeShipMaxShieldPoints,
  // so the player returns with armor equal to the max SHIELD value. Preserve.
  state.stat_cache_valid = false;
  const PlayerEffectiveStats refill = Outfit_ComputePlayerEffectiveStats(state);
  p.fuel_points = refill.fuel_capacity;
  p.shield_points = refill.max_shield_points;
  p.armor_points = refill.max_shield_points;
  state.cached_stats = refill;
  state.stat_cache_valid = true;

  // Re-populate the system's NPC/mission ships.
  NovaSystem_PopulateInitialNpcShips(state, p.current_system_id);
  NovaSystem_RestoreMissionFleets(
      state, p.current_system_id, /*copy_player_heading=*/false, now_ms);

  // The world catches up over rand(30) + 15 elapsed game-days.
  const std::int16_t elapsed_days = static_cast<std::int16_t>(
      RollRandom(state, kRespawnDailyUpdateRange) + kRespawnDailyUpdateBase);
  for (std::int16_t day = 0; day < elapsed_days; ++day) {
    Mission_TickDailyWorldUpdate(state);
  }

  // System_UpdateSystemAndStellarDisplayState (0x00432470) display-state pass.
  NovaTargeting_UpdateStellarAvailability(state);

  // Fresh registration number: class display name + four rand(9)+1 digits
  // (never 0).
  if (const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(p.ship_class_id + 0x80))) {
    std::string registration = cls->display_name + " ";
    for (int digit = 0; digit < 4; ++digit) {
      registration += std::to_string(RollRandom(state, 9) + 1);
    }
    p.ship_name = registration;
  }
  // TODO(decomp(0x0044d91e)) skipped: the viewport/radar redraw and the
  // per-panel dirty flags (the clean-room re-renders every frame).

  // Rebuild the per-system reputations from each system government's InitialRec
  // (GovtDef 0x52, payload +0x14) over all 0x800 systems, matching
  // Game_ResetNewGameReputation (0x004b4220).
  const std::size_t system_count =
      std::min(state.scenario.systems.size(), state.system_reputation.size());
  for (std::size_t i = 0; i < system_count; ++i) {
    const std::int16_t govt = state.scenario.systems[i].government_id;
    const Government *gov_def =
        govt >= 0
            ? state.scenario.Government(static_cast<std::int16_t>(govt + 0x80))
            : nullptr;
    state.system_reputation[i] = gov_def != nullptr ? gov_def->initial_rec : 0;
  }

  // Conditional post-respawn auto-save (DAT_00596d2f: save at the first nav
  // stellar of the new system, else slot 0). TODO(decomp(0x0044da30))
  // skipped: the autosave preference flag and PilotFileSaveGame wiring.
  NovaLog::Info("escape-pod respawn complete: system {}, class {}, ' {}'",
                p.current_system_id,
                p.ship_class_id,
                p.ship_name);
  // Frame_FinishBlockingTransitionFrame: presentation pass, skipped above.
  return true;
}

// Ghidra 0x0044CA6B PlayerTick_TurnBankAnimation, internal label of
// Ship_HandlePlayerShipCore. Synthetic CFG: 0x0044CA6B -> 0x0044CB99.
static void TickPlayerTurnBankAnimation(GameState &state,
                                        int turn_dir,
                                        float elapsed_ticks) {
  PlayerShip &p = state.player;
  // Banking classes (sh\x8an Flags & 1) accumulate
  // turn_bank_animation_phase (+0xc8e4) while a turn is held (capped +16 /
  // -16, DAT_005755c0/c4), decay it toward zero at 2x frame rate
  // (DAT_00575618) when straight, and raise ai_turn_bias_dir (+0xc8f8) once
  // the phase passes the +6/-6 hysteresis thresholds (DAT_005755c8 = 6,
  // DAT_00575620 = -6). The sprite layer maps the bias to the bank-left /
  // bank-right alt rows (Ship_UpdateVisualState 0x00428340).
  // turn_dir is the original's sVar8: set by the keyboard steering branch AND
  // by the auto-turn continuation (reverse command, face-target command, jump
  // alignment) whenever a rotation step was applied this frame, so auto-turns
  // bank exactly like keyboard turns.
  p.ai_turn_bias_dir = 0;
  const ShipClass *player_cls =
      state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
  if (player_cls != nullptr && (player_cls->sprite_behavior_flags & 1U) != 0U) {
    constexpr float kBankPhaseCap = 16.0F;
    constexpr float kBankDecayRate = 2.0F;
    constexpr float kBankBiasThreshold = 6.0F;
    if (turn_dir == 0) {
      const float decay = kBankDecayRate * elapsed_ticks;
      if (p.turn_bank_animation_phase > decay) {
        p.turn_bank_animation_phase -= decay;
      } else if (p.turn_bank_animation_phase < -decay) {
        p.turn_bank_animation_phase += decay;
      } else {
        p.turn_bank_animation_phase = 0.0F;
      }
    } else {
      if (turn_dir > 0 && p.turn_bank_animation_phase < kBankPhaseCap) {
        p.turn_bank_animation_phase += elapsed_ticks;
      }
      if (turn_dir < 0 && p.turn_bank_animation_phase > -kBankPhaseCap) {
        p.turn_bank_animation_phase -= elapsed_ticks;
      }
    }
    if (turn_dir >= 1 && p.turn_bank_animation_phase > kBankBiasThreshold) {
      p.ai_turn_bias_dir = 1;
    } else if (turn_dir <= 0 &&
               p.turn_bank_animation_phase < -kBankBiasThreshold) {
      p.ai_turn_bias_dir = -1;
    }
    // Station-hold / maneuver-lock reset arm: while the hold or a maneuver
    // lockout is active the phase snaps to zero (station hold past the
    // 30-tick engage gate) and the glow fades one step (handled by the
    // target-based glow drive below; the original decays it explicitly here).
    if ((p.ai_maneuver_timer_ms > 0.0F || p.ai_station_hold_timer > 0.0F) &&
        p.ai_station_hold_timer > 30.0F) {
      p.turn_bank_animation_phase = 0.0F;
      p.ai_turn_bias_dir = 0;
    }
  }
}

// Ghidra 0x0044aa70 face-target command (0x0044c0b1 -> 0x0044c18a, with the
// out-of-line bearing tails at 0x0044ec90/0x0044ecb7), part of the
// PlayerTick_WeaponCommands region. While held with the station-hold timer
// idle the command stores the integer heading to face (Math_BearingFromPoint-
// ToPoint from the player position, written to ai_desired_heading_deg +0x68)
// and arms the manual-flight auto-turn (the parent's local_265 latch): the
// primary ship target wins unless the 0x38/0x6f arm modifier is held, which
// -- like having no ship target -- faces the selected travel stellar instead
// (the original reads the player's +0x6c slot; the port keeps that selection
// in travel.selected_stellar_id because its travel commands never populate
// the player ship field). With neither targeted the command is inert.
// Returns true when armed: NovaPlayer_IntegrateMovement then suppresses
// keyboard steering and turns one step per frame toward the stored heading
// until within one step (no snap -- the |delta| <= step exit rejoins the
// keyboard path, so the final step-short alignment is kept).
bool PlayerTick_FaceTargetCommand(GameState &state,
                                  const FlightInput &input,
                                  bool arm_modifier_held) {
  PlayerShip &p = state.player;
  if (!input.face_target || p.ai_station_hold_timer > 0.0F) {
    return false;
  }
  const std::int16_t ship_target = p.primary_target_ship_slot;
  const std::int16_t stellar_target = state.travel.selected_stellar_id;
  bool face_stellar = false;
  const Ship *target_ship = nullptr;
  const Stellar *target_stellar = nullptr;
  if (ship_target >= 0 &&
      state.SlotInRange(static_cast<std::size_t>(ship_target))) {
    if (arm_modifier_held && stellar_target >= 0) {
      face_stellar = true;
    } else {
      target_ship = &state.ShipAt(static_cast<std::size_t>(ship_target));
    }
  } else if (stellar_target >= 0) {
    face_stellar = true;
  } else {
    return false;
  }
  if (face_stellar) {
    target_stellar = state.scenario.Stellar(stellar_target);
    if (target_stellar == nullptr) {
      return false;
    }
  }
  // Math_BearingFromPointToPoint: integer degrees, 0 = up, clockwise.
  const float dx = face_stellar
                       ? static_cast<float>(target_stellar->pos_x) - p.pos_x
                       : target_ship->pos_x - p.pos_x;
  const float dy = face_stellar
                       ? static_cast<float>(target_stellar->pos_y) - p.pos_y
                       : target_ship->pos_y - p.pos_y;
  const float bearing_deg =
      std::atan2(dx, -dy) * (180.0F / 3.14159265358979323846F);
  int heading_deg = static_cast<int>(std::lround(bearing_deg));
  heading_deg = ((heading_deg % 360) + 360) % 360;
  p.ai_desired_heading_deg = static_cast<std::int16_t>(heading_deg);
  return true;
}

// Ghidra 0x0044C8D0 PlayerTick_ManualFlightAndRegeneration, internal umbrella
// of Ship_HandlePlayerShipCore. Relevant synthetic CFGs: turn input
// 0x0044C92E -> 0x0044C980; joined afterburner/thrust/glow
// 0x0044C9AB -> 0x0044CA6B; bank animation 0x0044CA6B -> 0x0044CB99; reordered
// afterburner speed-cap/fuel/glow tail 0x00451630 -> 0x004518EF.
void PlayerTick_ManualFlightAndRegeneration(GameState &state,
                                            const FlightInput &input,
                                            float elapsed_ticks,
                                            bool face_target_armed) {
  PlayerShip &p = state.player;
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
      Ship_AccelerateShipTowardPoint(state, elapsed_ticks);

  ShipClass effective_class;
  effective_class.accel = eff.thrust_raw;
  effective_class.speed = eff.speed_raw;
  effective_class.turn_rate = eff.turn_raw;
  // Player tails of Ship_ComputeShipEffectiveThrust (0x004640a0) and
  // Ship_ComputeShipMaxTurnRateDeg (0x00463e70). TODO(decomp): the original
  // turn-damping gate uses ShipState +0x28/+0x5c, which is not fully decoded.
  if (p.ionization_points > 0.0F) {
    const ShipClass *cls =
        state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
    if (cls != nullptr) {
      const float intensity =
          std::min(0.7F, NovaShip_IonizationIntensity(p, *cls));
      effective_class.accel *= (1.0F - intensity);
      if (!input.thrust) {
        effective_class.turn_rate *= (1.0F - intensity);
      }
    }
  }
  if (afterburner_active && !gravity_present) {
    effective_class.speed *= 1.8F;
  }
  // Per-axis velocity caps (Ghidra LAB_00451630 afterburner speed-cap tail,
  // 0x00451630 -> 0x004518ef; g_player_speed_cap_x/y maintained on GameState):
  // while afterburning outside a stellar gravity pull both caps jump to 1.8x
  // the effective max speed (g_afterburner_overspeed_factor 0x00575610);
  // otherwise they decay by effective thrust * 0.4 (DAT_00575680) per frame
  // and are clamped up to the effective max speed. Maintained ahead of the
  // integration so the velocity clamp sees current-frame caps (the original
  // updates them at the frame tail; one-frame lag is not observable). The
  // tail's engine-glow > 24 decay is owned by the port's glow drive.
  {
    const float eff_max_speed = effective_class.speed / 100.0F;
    if (afterburner_active && !gravity_present) {
      constexpr float kAfterburnerOverspeedFactor = 1.8F; // 0x00575610
      state.player_speed_cap_x = eff_max_speed * kAfterburnerOverspeedFactor;
      state.player_speed_cap_y = eff_max_speed * kAfterburnerOverspeedFactor;
    } else {
      const float cap_decay = effective_class.accel / 10000.0F * 2.0F * 0.4F;
      if (eff_max_speed < state.player_speed_cap_x) {
        state.player_speed_cap_x -= cap_decay;
      }
      if (eff_max_speed < state.player_speed_cap_y) {
        state.player_speed_cap_y -= cap_decay;
      }
      if (state.player_speed_cap_x < eff_max_speed) {
        state.player_speed_cap_x = eff_max_speed;
      }
      if (state.player_speed_cap_y < eff_max_speed) {
        state.player_speed_cap_y = eff_max_speed;
      }
    }
  }
  // Gravity-shield movement model (Outfit_ShipHasGravityShieldOutfit 0x0046df70
  // player branch: class flags_secondary 0x40 or an owned inertial dampener).
  PlayerMovementOptions movement_opts;
  movement_opts.face_target_armed = face_target_armed;
  const ShipClass *player_class =
      state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
  movement_opts.gravity_shield =
      (player_class != nullptr &&
       (player_class->flags_secondary & 0x40U) != 0U) ||
      Outfit_HasOwnedEffect(state, OutfitEffect::kInertialDampener);
  movement_opts.fire_restricted = NovaAiShip_IsDisabled(state, p);
  movement_opts.speed_cap_x = state.player_speed_cap_x;
  movement_opts.speed_cap_y = state.player_speed_cap_y;
  // Capture the applied turn direction (keyboard OR auto-turn) for the bank
  // animation.
  const PlayerMovementStats movement_stats = NovaPlayer_IntegrateMovement(
      p, input, effective_class, elapsed_ticks, movement_opts);
  if (afterburner_active) {
    p.fuel_points = std::max(0.0F, p.fuel_points - fuel_burn * elapsed_ticks);
  }

  TickPlayerTurnBankAnimation(state, movement_stats.turn_dir, elapsed_ticks);
  const ShipClass *player_cls =
      state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
  // TODO(decomp(0x0044aa70)) skipped: the player engine-glow state machine is
  // still a clean-room target interpolation. Reconstruct its distinct normal
  // thrust, gravity-shield, afterburner, banking, turnaround, and hyperspace
  // branches before changing its cadence. Those original integer mutations
  // run once per raw spaceflight call and should eventually replay at the
  // 21 ms cadence used by the NPC Ship_HandleShip path above; do not merely
  // put this approximation behind that cadence.
  // ShipState +0xc8d4 is an integer engine/glow control, not a free-running
  // alpha ramp. Normal thrust approaches 24; afterburning extends it to 32;
  // coasting and reverse decrement by one renderer frame. This makes the
  // observable rise/fall and afterburner cap match player control; rendering
  // derives its alpha from that level until SpriteWorld's native glow blend is
  // reconstructed.
  const std::int16_t glow_target =
      afterburner_active ? 32 : (p.engine_thrust ? 24 : 0);
  // Banking boost (Flags & 2 classes): the original adds +2 per frame while
  // ai_turn_bias_dir is set, capped at 0x18 -- equivalent to holding the
  // cruise glow while banking without thrust.
  const std::int16_t effective_glow_target =
      (p.ai_turn_bias_dir != 0 && player_cls != nullptr &&
       (player_cls->sprite_behavior_flags & 2U) != 0U)
          ? std::max<std::int16_t>(glow_target, 24)
          : glow_target;
  if (p.engine_glow_level < effective_glow_target) {
    ++p.engine_glow_level;
  } else if (p.engine_glow_level > effective_glow_target) {
    --p.engine_glow_level;
  }
  p.engine_glow_intensity =
      std::clamp(static_cast<float>(p.engine_glow_level) / 24.0F, 0.0F, 1.0F);
}

// Ghidra Ship_HandlePlayerShipCore 0x0044AA70 synthetic CFG:
// PlayerTick_ShieldAndArmorRegeneration 0x0044CB99 -> 0x0044CCAF.
void PlayerTick_ShieldAndArmorRegeneration(GameState &state,
                                           float frame_time_ms) {
  PlayerShip &p = state.player;
  if (!state.stat_cache_valid) {
    state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
    state.stat_cache_valid = true;
  }
  const PlayerEffectiveStats &eff = state.cached_stats;
  const float tick_scale = frame_time_ms / (1000.0F / 30.0F);
  const bool destroyed = NovaAiShip_IsDestroyed(p);
  const bool disabled = NovaAiShip_IsDisabled(state, p);

  // Shields recover whenever the hull is intact and not disabled (no
  // recently-hit gate, and the original does not clamp to max within the
  // frame -- the < max gate applies to the next frame's addition).
  if (!destroyed && !disabled && p.shield_points < eff.max_shield_points) {
    p.shield_points += std::max(0.0F, eff.shield_recharge) * tick_scale;
  }
  // Armor recovery additionally holds off until the post-hit suppression
  // window (g_player_recently_hit_timer, armed +300 on a hit) has decayed
  // below zero. The rate is the class ArmorRech base (0 on stock ships) plus
  // ModType-29 outfit bonuses (Bible: 1000 = one more point per frame); the
  // cheat-mode latch scales it by k_armor_regen_player_scale_f32 = 50
  // (Ship_ComputeShipArmorRegenRate 0x004638e0 player tail).
  if (!destroyed && !disabled && p.armor_points < eff.max_armor_points &&
      state.recently_hit_timer < 0.0F) {
    float armor_rate = std::max(0.0F, eff.armor_recharge);
    if (state.cheat_mode_active) {
      armor_rate *= 50.0F;
    }
    p.armor_points += armor_rate * tick_scale;
  }
}

// Ghidra Ship_HandlePlayerShipCore 0x0044AA70 synthetic CFG:
// PlayerTick_IonizationAndFuelRegeneration 0x00450717 -> 0x004507B4.
void PlayerTick_IonizationAndFuelRegeneration(GameState &state,
                                              float frame_time_ms) {
  PlayerShip &p = state.player;
  if (!state.stat_cache_valid) {
    state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
    state.stat_cache_valid = true;
  }
  const PlayerEffectiveStats &eff = state.cached_stats;
  const float tick_scale = frame_time_ms / (1000.0F / 30.0F);
  // Ionization decay, then the ionized-velocity damping: each velocity axis
  // is pulled toward (1 - intensity) * effective max speed at
  // DAT_00575670 = 0.025 per frame (a soft ramp, distinct from the hard
  // per-axis clamp in the movement block).
  if (p.ionization_points > 0.0F) {
    TickIonizationDecay(state, p, tick_scale);
    const ShipClass *cls =
        state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
    float intensity =
        cls != nullptr ? NovaShip_IonizationIntensity(p, *cls) : 0.0F;
    intensity = std::min(0.7F, intensity); // _DAT_00575668
    const float ionized_cap = (1.0F - intensity) * (eff.speed_raw / 100.0F);
    const float damp_step = 0.025F * tick_scale;
    if (p.vel_x > ionized_cap) {
      p.vel_x = std::max(ionized_cap, p.vel_x - damp_step);
    } else if (p.vel_x < -ionized_cap) {
      p.vel_x = std::min(-ionized_cap, p.vel_x + damp_step);
    }
    if (p.vel_y > ionized_cap) {
      p.vel_y = std::max(ionized_cap, p.vel_y - damp_step);
    } else if (p.vel_y < -ionized_cap) {
      p.vel_y = std::min(-ionized_cap, p.vel_y + damp_step);
    }
  } else {
    p.ionization_points = 0.0F;
  }

  // Fuel-scoop recharge (Ship_ComputeShipFuelRechargeRate 0x00463b30 via its
  // HandlePlayerShipCore call site): rate cached in the stats snapshot; a
  // negative rate is the "fuel sucking" mode. Then the original clamps fuel
  // to [0, fuel capacity].
  p.fuel_points += eff.fuel_regen_rate * tick_scale;
  p.fuel_points =
      std::clamp(p.fuel_points, 0.0F, static_cast<float>(eff.fuel_capacity));
}

// Ghidra 0x00489210 Ship_RunSpaceflightMode.
void NovaSpaceflight_Run(SdlPlatform &platform,
                         SdlAudio &audio,
                         GameState &state,
                         const NovaPreferences &prefs) {
  NovaLog::Info("entering spaceflight mode");

  // Preflight: the new-game intro cinematic plays on the pilot's first entry
  // (Ghidra DAT_00596d35 == 0 in Ship_RunSpaceflightMode). We set the
  // intro_played latch *after* the intro returns, exactly as the original sets
  // DAT_00596d35 = 0x01 immediately after IntroCinematic_Run(). The intro's
  // skip result already gates (a stub of) the intro text-reader dialog
  // internally, so its return value needs no action here.
  if (!state.intro_played) {
    (void)NovaIntroCinematic_Run(platform, audio, state);
    // Ghidra: DAT_00596d35 = 0x01, the latch IntroCinematic_SetupFrames/
    // Game_ResetNewGameState clear on a new pilot (see new_pilot_flow.cpp).
    state.intro_played = true;
  }

  // Ghidra Ship_RunSpaceflightMode: preflight owns the gameplay surface, runs
  // Frame_SpaceflightLoop, then tears the mode back down to the menu shell.
  bool returning_to_menu = false;
  NovaFrame_SpaceflightLoop(platform, audio, state, returning_to_menu, prefs);
  NovaFrame_CancelCombatChatter(state, audio);

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
    selected = RollRandom(state, count);
  } else if (count == 2) {
    selected = variant;
  } else {
    selected =
        static_cast<std::int16_t>(RollRandom(state, count / 2) * 2 + variant);
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
  audio.Play(*state.active_combat_chatter_sound, 1.0F, 1.0F, sound_id);
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
