#pragma once

// Clean-room model of the persistent per-pilot game state. The original keeps
// this in a swath of globals (g_ship_states / ShipState, g_system_defs /
// SystemDef, g_outfit_owned_count, g_pilot_data / DAT_007d2290,
// g_intro_cinematic / IntroCinematicData). Here it is grouped into one value
// object owned by NovaRuntime so the reimplementation stays free of hidden
// globals and singletons (AGENTS.md). Each field that maps to a Ghidra
// global/struct names the address in a comment.

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "scenario_data.hpp"
#include "sdl_audio.hpp"
#include "sprite_mask.hpp"

namespace game {

// Ghidra g_system_reputation (0x00733bc8): a per-system int16 global that
// tracks the player's standing with each system. Negative values push factions
// hostile; the destination-interaction dialog compares a target stellar's
// reputation_threshold against the containing system's reputation to decide
// whether landing is denied (Stellar_HandleStellarEntryAndExit / NovaUi_Run-
// TravelDestinationInteractionWindow). Resized by ScenarioData load to match
// the systems table; indexed by 0-based system resource id.
using SystemReputation = std::vector<std::int16_t>;

// Runtime flags for one accepted mission. This mirrors the 20-byte
// MisnRuntimeFlags record used by the original's 16 active-mission slots.
// In-game calendar (Ghidra g_current_game_year_month / g_current_game_day,
// a {year, month, day} short triple at 0x00735458). Months/days are 1-based;
// the original's clock-seeded start can hold months > 12, and the advance
// timer only renormalises them past December on rollover.
struct GameDate {
  std::int16_t year = 1900;
  std::int16_t month = 1;
  std::int16_t day = 1;
};

struct MissionRuntimeFlags {
  bool is_active = false; // +0x00
  // Cleared-to-proceed latch: set at acceptance when the mission has no
  // separate fail/visit stellar, and set by the landing/interaction pass when
  // the destination stellar requires no special handling.
  bool initial_briefing_done = false; // +0x01
  // Objective-complete latch, driven by the per-goal evaluation in
  // Mission_HandleMissionOrSurrenderShipReaction (0x00443c60).
  bool objective_complete = false;           // +0x02
  bool is_failed = false;                    // +0x03
  std::uint16_t flags_primary_at_accept = 0; // +0x04
  // Absolute deadline date, computed at acceptance from the current calendar
  // plus the m\xefsn TimeLimit (Mission_ComputeDateAfterSteps
  // 0x0043f080 writes {year, month, day} here). Zero when the mission has no
  // deadline. (Ghidra named the first two shorts deadline_year_month/_ext
  // from the packed (year,month) argument shape.)
  std::int16_t deadline_year = 0;          // +0x06
  std::int16_t deadline_month = 0;         // +0x08
  std::int16_t deadline_day = 0;           // +0x0a
  std::int32_t elapsed_travel_days = 0;    // +0x0e
  std::uint16_t elapsed_travel_subday = 0; // +0x12
};

// Clean-room active mission state. It intentionally names only the fields
// confirmed by the MisnActive type layout (size 0x8e6); raw_payload keeps the
// remaining text/script/runtime bytes available while those semantics are
// reconstructed. One-to-one mission-slot indexing is preserved.
struct ActiveMission {
  // Resolved TravelStel (+0x00, where the mission's visit/pickup happens)
  // and ReturnStel (+0x04, where the mission completes and pays out).
  std::int16_t travel_stellar_id = -1; // +0x00
  std::int16_t return_stellar_id = -1; // +0x04
  std::int16_t target_ship_count = 0;  // +0x06
  std::int16_t dude_def_index = -1;    // +0x08
  std::int16_t ship_goal = 0;          // +0x0a, Bible ShipGoal
  std::int16_t ship_behavior = 0;      // +0x0c, Bible ShipBehav
  std::int16_t ship_start = 0;         // +0x0e, Bible ShipStart
  std::int16_t current_system_id = -1; // +0x10
  std::int16_t cargo_type_id = -1;     // +0x12
  std::int16_t cargo_qty_tons = 0;     // +0x14
  // Bible PickupMode (+0x16: -1 ignored, 0 at accept, 1 at TravelStel, 2
  // when boarding), DropOffMode (+0x18: 0 at TravelStel, 1 at ReturnStel)
  // and ScanMask (+0x1a, govts whose scans flag the cargo).
  std::int16_t pickup_mode = -1;            // +0x16
  std::int16_t drop_off_mode = -1;          // +0x18
  std::int16_t scan_mask = 0;               // +0x1a
  std::int16_t comp_govt_id = -1;           // +0x1c
  std::int16_t comp_reward_delta = 0;       // +0x1e
  std::int16_t on_resolve_repeat_count = 0; // +0x20
  std::int32_t resource_delta_or_cost = 0;  // +0x22
  std::int16_t goal_counter_a = 0;          // +0x26
  std::int16_t goal_counter_b = 0;          // +0x28
  std::int16_t goal_counter_c = 0;          // +0x2a
  std::int16_t goal_count_remaining = 0;    // +0x2c
  std::int16_t goal_counter_e = 0;          // +0x2e
  std::int16_t mission_target_count = 0;    // +0x30
  // The misn CanAbort flag (payload +0x42, copied at accept by
  // Mission_PopulateMissionSlotFromDef 0x0043f8c0). Gates the player-abort arm
  // of the mission-info window (0x00446150 action 5) and the quick-fail fleet
  // release (0x00440bf0).
  bool can_abort = false; // +0x32
  // Carrying-the-mission-cargo latch: set at acceptance for PickupMode 0,
  // set/cleared by the landing interaction pass.
  bool carrying_resources = false;               // +0x33
  std::int16_t mission_template_id = -1;         // +0x4d
  std::int16_t mission_ship_count_max = 0;       // +0x61
  std::int16_t aux_ships_dude_def_index = -1;    // +0x63
  std::int16_t mission_fleet_metric_b = 0;       // +0x65
  std::int16_t mission_fleet_metric_c = 0;       // +0x67
  std::int16_t rearm_roll_clock = 0;             // +0x69
  std::int16_t mission_ship_count_active = 0;    // +0x6b
  std::uint16_t flags_primary = 0;               // +0x55
  std::uint16_t flags_secondary = 0;             // +0x57
  std::int16_t special_ship_type_index = -1;     // +0x53
  std::int16_t special_ship_name_string_id = -1; // +0x47
  std::int16_t special_ship_name_entry = -1;     // +0x49
  std::int16_t random_text_string_id = -1;       // +0x4f
  std::int16_t random_text_entry = -1;           // +0x51
  std::int16_t spawn_rearm_timer = -1;           // +0x4b
  std::int16_t brief_description_id = -1;        // +0x35
  // Runtime desc-resource ids (+0x35..+0x43, -1 when unset). Slot map from
  // Mission_PopulateMissionSlotFromDef (0x0043f8c0), matching the Bible's
  // m\xefsn desc fields: [0] BriefText, [1] QuickBrief, [2] LoadCargText,
  // [3] DumpCargoText, [4] CompText (success debrief), [5] FailText (failure
  // debrief), [6] slot +0x41 (m\xefsn +0x58, provisional), [7] ShipDoneText.
  std::array<std::int16_t, 8> brief_description_ids{}; // +0x35..+0x43
  // +0x45: days remaining before the mission deadline. Seeded from the m\xefsn
  // TimeLimit at acceptance (-32000 when there is no deadline); the daily
  // driver (ShipClass_RerollShipClassAvailabilityChances 0x00466cb0, not yet
  // ported) decrements it once per game day.
  std::int16_t time_limit_days_remaining = -32000; // +0x45
  // MisnActive's six 255-byte text/script buffers. The resource decoder keeps
  // the source mïsn payload; activation projects these strings to the active
  // record at the offsets used by Mission_PopulateMissionSlotFromDef.
  std::array<std::byte, 255> on_accept_text{};              // +0x1ec
  std::array<std::byte, 255> mission_payload_text_b{};      // +0x2eb
  std::array<std::byte, 255> on_success_text{};             // +0x3ea
  std::array<std::byte, 255> on_failure_text{};             // +0x4e9
  std::array<std::byte, 255> resolve_script_buffer_start{}; // +0x5e8
  std::array<std::byte, 255> state_latch{};                 // +0x6e7
  std::array<std::byte, 0x8e6> raw_payload{};
};

// Results of Mission_ResolveMissionStellarTargets (0x0043d240) needed by
// active-slot population. Locator selection is still a separate subsystem;
// this cache allows the accepted-mission path to remain one-to-one with the
// original without inventing locator semantics.
struct MissionTargetResolution {
  std::int16_t travel_stellar_id = -1;
  std::int16_t travel_system_id = -1;
  std::int16_t return_stellar_id = -1;
  std::int16_t return_system_id = -1;
  std::int16_t cargo_type_id = -1;
  std::int16_t cargo_qty_tons = 0;
  std::int32_t priority_payload = 0;
  std::int16_t reaction_schedule = 0;
  // The offer-row <DL> date: Mission_ResolveMissionStellarTargets (0x0043d240)
  // stores Mission_ComputeDateAfterSteps (0x0043f080) over the definition's
  // TimeLimit. Zeroed when there is no TimeLimit (the original leaves the
  // block fields untouched for steps < 1); the <DL> pass then formats the
  // zero date, matching the original's garbage.
  std::int16_t deadline_year = 0;
  std::int16_t deadline_month = 0;
  std::int16_t deadline_day = 0;
};

} // namespace game

namespace game {

// Ghidra 0x004cd3b0 IntroCinematic_SetupFrames fills this (g_intro_cinematic).
// Up to 4 PICT ids (source_pict_ids), each shown for duration_60h_ticks/i
// 1/60s ticks (clamped to [0,300]). When intro_text_desc_id != -1 the intro
// finishes by showing that desc resource in the generic text-reader dialog.
struct IntroCinematicData {
  // PICT resource ids for the sequenced frame art (0xffff terminates the
  // list); 0 (or negative) means "no art this frame".
  std::array<std::int16_t, 4> source_pict_ids{-1, -1, -1, -1};
  // Per-frame display time in 1/60 s ticks, matching the original clamp.
  std::array<std::int16_t, 4> duration_60h_ticks{0, 0, 0, 0};
  // Bible char resource IntroTextID (block+0x30): the desc shown after the
  // frames via Ui_LoadSelectionDialogResource + the generic text-reader
  // Ui_RunTravelSelectionDialog (the "travel" in that name is a misnomer).
  // -1 = no dialog.
  std::int16_t intro_text_desc_id = -1;

  // Ghidra IntroCinematic_Run opens the text reader when
  // intro_text_desc_id != -1. The new-game flow (IntroCinematic_SetupFrames
  // default) uses 0x7ffd even with no pilot-save block: not a valid desc, so
  // the reader shows empty text, but deliberately not -1 so it still opens.
  [[nodiscard]] bool should_open_intro_text_dialog() const {
    return intro_text_desc_id != -1;
  }
};

// A single ship in a system. The original keeps them all in one global array
// `g_ship_states`, 64 slots each of a `ShipState` (offset 0 = the player). We
// model the common kinematic/combat/identity subset the reimplementation needs
// (movement, targeting, spawn and AI systems). Only the fields
// with a clear Ghidra `ShipState` mapping are named with their offset; deep
// AI/combat fields are added as their systems are reconstructed (see
// Ship_AllocateShipSlotInSystem 0x004254b0). Index 0 in GameState.ships_ is the
// player; GameState.player references it (the original g_ship_states[0]).
struct Ship {
  // --- Kinematics (Ghidra ShipState) ---
  float pos_x = 0.0F;   // +0x18
  float pos_y = 0.0F;   // +0x1C
  float vel_x = 0.0F;   // +0x20
  float vel_y = 0.0F;   // +0x24
  float heading = 0.0F; // +0x44 radians
  float speed = 0.0F;   // +0x48

  // Ghidra ShipState +0x40. Per-instance NPC pilot-skill multiplier seeded by
  // Ship_AllocateShipSlotInSystem /
  // ShipClass_ComputeShipClassSkillVarianceScale (0x004254b0 / 0x0046b870).
  // Player ships leave this at the neutral value.
  float skill_variance_scale = 1.0F;
  // Ghidra ShipState +0xC914. Hostile combat strength divided by allied
  // strength, recomputed by Ship_UpdateShipCombatOddsScore (0x004133F0).
  float ai_odds_score = 0.0F;

  // Clean-room collision envelope used when no per-frame pixel mask has been
  // resolved (the original always has a prepared sprite frame once its sprite
  // layer is live). The mask binding below takes precedence; this radius stays
  // as the bounding-circle fallback the original uses for the small-shot-span
  // and high-frame-time cases.
  float collision_radius_px = 16.0F;
  // Per-frame opaque-pixel mask for the ship's current hull sprite, resolved
  // from the class's sh\x8an base sheet by the collision refresh pass.
  CollisionMaskBinding collision_mask;

  // --- AI movement state (drives Ship_HandleShip 0x00433050's integrator) ---
  // Whether the ship applies forward thrust this frame. Mirror GHIDRA
  // ShipState.ai_forward_thrust_cmd (+0x30). For the player it is mirrored by
  // `engine_thrust` below for the render glow; the AI writes the same field
  // via the reaction/offence helpers.
  float ai_forward_thrust_cmd = 0.0F; // +0x30
  // Desired scalar speed along the heading. Positive values use normal thrust;
  // negative values select the physics-override path, which sets velocity to
  // heading * abs(desired) instead of integrating acceleration. The signed
  // value is then advanced toward zero by abs(ai_forward_thrust_cmd).
  float ai_desired_speed = 0.0F; // +0x34
  // Desired heading in the game's integer-degrees convention (heading 0 = up,
  // increasing clockwise). Ship_HandleShip turns the ship toward this at
  // Ship_ComputeShipMaxTurnRateDeg deg/frame.
  std::int16_t ai_desired_heading_deg = 0; // +0x68
  // Coast-through-reversal TIMER (NOT a brake). The `_ms` suffix is a legacy
  // misnomer: values are normalized ticks (about 1 per frame at 30 Hz). While
  // positive it suppresses turn/thrust so the ship coasts. Set to random
  // 30..59 on reversal and decremented by normalized elapsed ticks each frame.
  float ai_maneuver_timer_ms = 0.0F; // +0x4C
  // Station-hold timer driving the hold/approach state (ai_station_hold_timer).
  float ai_station_hold_timer = 0.0F; // +0x50
  // Probe/debug-only lifecycle monitor for non-gate NPC arrivals. Spawn paths
  // arm it alongside the original 50 px/tick inward velocity; it has no
  // gameplay effect and lets rare state/control failures survive in the log.
  float arrival_monitor_elapsed_ticks = 0.0F;
  bool arrival_monitor_active = false;
  bool arrival_monitor_warning_logged = false;
  // Wall-clock (SDL ticks) the current AI mode began; used by the jump-sequence
  // and formation positioning timing.
  std::uint32_t ai_mode_start_time_ms = 0; // +0xA4
  // AI turn-bias direction (-1/0/+1) used by ships that bank/lean into turns.
  // Ghidra names the source phase at ShipState +0xC8E4
  // `turn_bank_animation_phase`; the visual updater also reuses that phase for
  // one sprite-behavior animation branch.
  std::int16_t ai_turn_bias_dir = 0; // +0xC8F8
  // Latch requesting this ship to fire its active weapon bank.
  std::int8_t ai_fire_trigger_latch = 0; // +0xBA

  // --- Vital stats ---
  float shield_points = 0.0F;     // +0x54
  float armor_points = 0.0F;      // +0x58
  float ionization_points = 0.0F; // +0x5C (ionization charge meter)
  // Ghidra ShipState +0x60. Running-lights brightness 0..32 driven each frame
  // by Ship_UpdateVisualState (0x00428340) from the class BlinkMode program;
  // 0 hides the light layer. The field is unnamed in the DB and sits between
  // ionization_points and cloak_fade_progress.
  float light_intensity = 0.0F;     // +0x60
  float fuel_points = 0.0F;         // +0x38
  float death_timer_active = -1.0F; // +0x3C
  // Port-side one-shot latch for the alive -> destroyed state transition at
  // the weapon-hit site. The original leaves destruction as an armor state
  // (IsShipDestroyed); the latch only records that NovaTargeting has already
  // cleared the slot's references.
  bool destruction_visual_triggered = false;
  bool destruction_finale_triggered = false;
  // Port-only: latches the first Ship_UpdateVisualState seed of the death
  // presentation. The original reseeds whenever death_timer_active <= 0, but
  // its integer 1.0-per-call countdown always reaches the finale window first.
  // The port replays that discrete countdown and resets this ownership latch
  // whenever the player is relaunched into a fresh hull.
  bool death_timer_seeded = false;
  // Port-side remainder for destruction operations that the original runs
  // once per outer spaceflight call. One whole call is 0.63 normalized ticks
  // at the executable's 21 ms frame floor; banking it keeps the discrete RNG
  // and timed-debris cadence independent of the SDL presentation rate.
  float destruction_raw_tick_accumulator = 0.0F;
  // Ghidra ShipState +0xC8F0. Shot_ResolveShipHitFromWeapon sets this to 32
  // on non-bypass impacts. Ship_UpdateVisualState caps it by remaining shield
  // fraction, applies it to the shan ShieldImageID layer, then decays it by
  // g_avg_frame_tick_scale. That update/render branch remains deferred.
  float shield_bubble_flash_intensity = 0.0F;
  // Ghidra ShipState +0xB9: boarding/boarding-target latch. Set when a ship
  // boards its target (boarding_plunder), cleared for healthy player ships in
  // PlayerTick_StatusAndOutfitEvents, and read by the disable restriction and
  // disable bookkeeping predicates.
  std::int8_t boarded_target_latch = 0;
  // Ghidra ShipState field_0xB0. Weapon on-hit ionization colors are ORed
  // here by Weapon_ApplyWeaponOnHitEffects; the status renderer is deferred.
  std::uint32_t ionization_color = 0;

  // Ghidra ShipState +0xAC: 60 Hz tick (NovaTime_GetTickCount60Hz) the ship
  // last played its mission-ship hail announcement; Ship_HandleShip re-hails
  // only once tick + 0xa8c (2692 ticks, ~45 s) has passed.
  std::int32_t last_mission_hail_tick_60hz = 0;
  // Ghidra ShipState +0xBC: mission-hail latch. Set after a ship has spoken
  // once (Mission_ShowMissionShipAnnouncement callers); the pers Flags 0x80
  // "no repeat hail" gate reads it to keep one-shot hailers silent.
  std::int8_t mission_hail_latch = 0;

  // Ghidra ShipState.jamming_score_1..4 (+0xC926): lazily computed electronic-
  // warfare jamming scores (0..100) per seek channel, consumed by the guided-
  // shot jamming check (NovaAi_GetShipJammingScore, 0x00464810). -1 = uncached;
  // the original reseeds -1 when a ship slot is allocated.
  std::array<std::int16_t, 4> jamming_score{{-1, -1, -1, -1}};

  // --- Identity / placement ---
  std::int16_t ship_class_id = 0;             // +0x76 (zero-based ship index)
  std::int16_t ship_instance_id = 0;          // +0x86 (0 = player)
  std::int16_t current_system_id = 0;         // +0x74
  std::int16_t faction_or_government_id = -1; // +0x98
  std::int16_t dude_class_id = -1;            // +0x78
  std::string ship_name;

  // --- Weapon bank / active selection ---
  std::int16_t active_weapon_bank_slot = 0; // +0x72
  // Ghidra keeps the weapon-bank rows on every ShipState. The player path uses
  // GameState's equivalent 100-stride arrays; NPCs keep their class loadout
  // counters here so AI target/intercept helpers do not accidentally inspect
  // the player's weapons.
  std::array<std::int16_t, 0x100> npc_weapon_bank_ammo{};
  std::array<std::int16_t, 0x100> npc_weapon_bank_secondary{};
  std::array<float, 0x100> npc_weapon_bank_cooldown{};
  // Per-bank burst-cycle tick counter (Ghidra ShipState field_0x17c, a
  // 200-stride int16 array). Driven by Weapon_FireShipWeapons
  // (NovaWeapon_FireNpcWeaponBank); resets with Weapon_InitShipWeaponBursts
  // (0x00413810) when the weapon-bank loadout is (re)built.
  std::array<std::int16_t, 0x100> npc_weapon_bank_burst_counter{};
  std::int16_t npc_weapon_banks_ship_class = -1;

  // --- Mission / target/AI slots (added to unblock spawn/targeting) ---
  std::int16_t mission_owner_slot = -1; // +0x8A
  // Ghidra ShipState +0xC8CE: inbound-weapon-threat latch consumed by the AI
  // weapon-bank selection path.
  std::int16_t inbound_weapon_threat = 0; // +0xC8CE
  // Ghidra ShipState +0xC8D0: përs personality def slot (g_pers_defs) of the
  // spawned personality ship; -1 when none. 0x3ff marks special sentinels.
  std::int16_t pers_def_slot = -1;      // +0xC8D0
  std::int16_t mission_fleet_slot = -1; // +0xC8D2
  std::int16_t ai_behavior_code = 0;    // +0x88
  std::int16_t ai_state_code = 0;       // +0xC8C8
  std::int16_t ai_control_mode = 0;     // +0xC8CA
  // Ghidra ShipState +0xC8CC. Ship_AllocateShipSlotInSystem initializes this
  // to NovaRandom_Range(3)^2; the remaining consumer is an AI/render cadence
  // branch, so the purpose is authoritative only at this level.
  std::int16_t random_ai_render_cadence = 0; // +0xC8CC
  // Ghidra ShipState +0xC8E0. Combat/sprite animation timer initialized from
  // the class combat-state range and consumed by deferred animation/combat
  // code.
  float sprite_animation_timer = 0.0F; // +0xC8E0
  // Ghidra ShipState +0xC8E4. Phase advanced by Ship_HandleShip's banking
  // animation and mapped to ai_turn_bias_dir (+0xC8F8).
  float turn_bank_animation_phase = 0.0F; // +0xC8E4
  // Ghidra ShipState +0xC8F6. Sprite animation cycle initialized from the
  // class skill-variance range.
  std::int16_t sprite_animation_cycle_index = 0; // +0xC8F6
  // Ghidra ShipState +0xC8E8. Weapon-effects sprite flash level 0..32, raised
  // to 32 at the fire site when the fired WeaponDef carries flags_secondary
  // 0x200 and faded per tick by ShipClass.weapon_glow_decay_rate while
  // positive (a below--1.0 overshoot clamps to -1.0). The original writes the
  // level into the Sprite's RGB tint channels (Sprite.sprite_tint_*).
  float weapon_sprite_flash_level = 0.0F; // +0xC8E8
  // Ghidra ShipState +0xC8D6. The
  // running-lights blink state machine's phase: for square-wave blink it
  // counts blinks within the current group; for triangle pulse it selects the
  // rise (0) or fall (1) leg. Unused by BlinkMode 0/-1/3.
  std::int16_t light_blink_phase = 0; // +0xC8D6
  // Ghidra ShipState +0xC8EC. The
  // running-lights blink timer; counts down in normalized 30 Hz ticks. Shared
  // by the square-wave (on/off/group dwell) and random-pulse (change delay)
  // arms.
  float light_blink_timer = 0.0F; // +0xC8EC
  // Travel-target transfer latch (Ghidra ShipState +0x2Aish): the AI sets this
  // to 2 when it assigns ai_secondary_target_slot a fresh travel stellar (the
  // signal System_UpdateSystemAndStellarDisplayState reads to auto-target the
  // nearest travel point on the arrival/last-known system).
  std::int16_t travel_transfer_mode = 0;      // +0x2A (provisional offset)
  std::int16_t primary_target_ship_slot = -1; // +0x70
  std::int16_t ai_secondary_target_slot =
      -1; // +0x6C (also a travel/stellar slot)
  // Squad leader / behavior anchor (Ghidra ShipState +0x9A
  // squad_leader_ship_slot): the ship
  // this NPC is attached to -- the carrier for behavior-5 fighters, the
  // protected ship (usually the player) for behavior-6 escorts, the assist
  // target for behavior >4 otherwise. -1 = no squad. It is an attachment, NOT
  // a hostile target; the friendly-fire squad-root chain and the formation
  // passes depend on that.
  std::int16_t squad_leader_ship_slot = -1; // +0x9A
  // Stellar this ship is garrisoned at / belongs to (Ghidra ShipState +0x8C
  // defense_fleet_home_stellar_id).
  // Set to a real stellar only by the defense-fleet spawner
  // (NovaStellar_SpawnDefenseFleetShip, Ghidra Stellar_SpawnDefenseFleetShip
  // 0x00421fd0); -1 for every other ship. Two ships sharing this value are
  // squadmates of the same stellar defense fleet and do not shoot each other
  // (Weapon_CanWeaponHitTarget 0x00426ef0 / collision.cpp), and the stellar's
  // own battery shots only hit ships matching its recorded target.
  std::int16_t defense_fleet_home_stellar_id = -1; // +0x8C
  // Escort command selected by the assist supervisor. The clean-room dialog
  // and mission models only use the neutral default so far, but the field is
  // needed to preserve the state-0x05+ branch shape.
  std::int16_t escort_command_code = 0; // +0xC90A (provisional)
  // Ghidra ShipState +0xC90C (provisional): set when an escort-group order
  // changed this ship's command, consumed by the AI/comm-chatter pass.
  std::int16_t escort_command_pending = 0;
  // Formation lead whose engine-glow/formation-offset this ship mirrors in the
  // escort control modes (ShipState +0xC906 formation_leader_ship_slot). <1
  // means "no leader": mode 0x12 (chase leader) falls back to idle control
  // when it is empty.
  std::int16_t formation_leader_ship_slot = -1; // +0xC906
  // Escort wedge formation offset (ShipState field_0x28/0x2c): the world
  // point Ship_SetEscortLaunchOffsetVelocity (0x00413b60) computes for this
  // ship's slot around its leader, which Ship_MoveShipTowardFormationOffset
  // (0x00414390) creeps/snaps position onto. Zeroed by
  // Ship_ResetShipToDefaultCombatState (0x0041e240).
  float formation_offset_x = 0.0F; // +0x28
  float formation_offset_y = 0.0F; // +0x2C
  // Squad/bookkeeping flag bytes (ShipState +0xC0/+0xC1/+0xC2), refreshed
  // every full tick by the Frame_TickSystems (0x004186b0) scope-6 pass:
  // +0xC0 some active ship holds this ship as its squad leader (NOT combat
  // targeting), +0xC1 some ship's
  // formation_leader_ship_slot is this slot, and +0xC2 some ship's
  // resolved_squad_leader_ship_slot is this slot -- the byte
  // Ship_UpdateShipAI (0x00401000) and the player core (0x00451003) gate
  // their per-frame Ship_UpdateEscortFormations pass on.
  bool is_any_ships_squad_leader = false;      // +0xC0
  bool ai_followed_as_leader = false;          // +0xC1
  bool ai_selected_as_resolved_target = false; // +0xC2
  // ShipState +0xBD afterburner latch. Producers: Pers_SpawnShipFromPersDef
  // (0x004235c0) seeds it from Ship_CanShipUseAfterburner (0x0046b260) and
  // forces it on for përs Flags 0x0002; the close-range combat break-off
  // modes (0x6/0x7/0x5 -> 0x11 boost) consume it.
  std::int8_t afterburner_latch = 0; // +0xBD
  // Velocity-match lock (ShipState +0xC8DC): the slot of a ship whose
  // velocity/heading this ship is matching (control mode 0xc/0xf), or -1. A
  // non-self value gates the NPC effective-stats branch
  // (Ship_ComputeShipEffectiveThrust 0x004640a0 applies the DAT_00575788
  // factor while locked) and blocks Stellar_CanShipInitiateJumpSequence; the
  // velocity-match control modes are TODO(decomp), but the field is
  // cleared by Ship_DeactivateVacantShipsAndTally (0x0041ad50) and seeded by
  // Ship_AllocateShipSlotInSystem, so it lives on the struct.
  std::int16_t velocity_match_target_ship_slot = -1; // +0xC8DC
  // Ghidra ShipState +0xC92E: the AI's resolved squad leader, cleared by
  // Ship_ResetShipAiBehaviorRuntimeFields (0x00402810).
  std::int16_t resolved_squad_leader_ship_slot = -1; // +0xC92E
  // Stored evasive heading for control mode 0x10 (Ghidra ShipState raw short
  // at +0x8E, between defense_fleet_home_stellar_id and
  // jump_destination_stellar_id; unnamed in the DB). Ship_ApplyShipAiControls
  // writes current-heading +/-135 deg (instance-id parity sign) when it orders
  // the evasive-break, and mode 0x10 steers at this value until it aligns and
  // drops back to mode 0x6.
  std::int16_t ai_evasive_heading_deg = 0; // +0x8E (Provisional)
  // Ghidra ShipState raw short at +0x90 (unnamed): the travel target cache
  // Ship_ResetShipAiBehaviorRuntimeFields (0x00402810) clears to -1.
  std::int16_t travel_target_cache = -1;         // +0x90 (Provisional)
  std::int16_t jump_destination_stellar_id = -1; // +0x92
  // Ghidra ShipState +0x94 (named jump_destination_system_id in the DB): the
  // system the ship is jumping toward / last jumped in from; -1 none, -2 the
  // mission-spawn sentinel. The player-tick jump branch (0x0044aa70) writes
  // the PRE-jump current_system_id here, so for the player it holds the
  // system just left; mission-fleet respawns orient their arrival bearing on
  // it (System_TickNpcSpawnMaintenance 0x0041d6e0).
  std::int16_t jump_destination_system_id = -1; // +0x94
  std::int16_t ai_hostility_accumulator = 0;    // +0x96
  // Ghidra ShipState +0xC910. Accumulates weapon reload * 1.75 per
  // player-owned hit (Shot_ResolveShipHitFromWeapon), gates the player-retarget
  // chance, and decays by 0.5 per normalized tick in Ship_HandleShip; reset
  // when the ship retargets onto an attacker.
  float player_aggro_accumulator = 0.0F; // +0xC910
  // Engagement patience timer while the cloak/targetability predicate rejects
  // a target; when it expires the AI gives up and clears the primary target
  // (Ship_UpdateShipAiState state 4). -1 means no patience interval active.
  float target_engagement_patience_timer = -1.0F; // +0xC90C
  // Cloak fade/visibility progress (Ghidra ShipState +0x64). The original
  // visual updater integrates this over the 0..32 range; weapon hits do not
  // produce it. Ship_IsShipCloakVisibilityThresholdActive (0x0046c7a0) uses
  // it with the signed transition latch as a targetability gate.
  float cloak_fade_progress = 0.0F; // +0x64
  // Signed cloak transition state (Ghidra ShipState +0xC8D8): positive starts
  // fading into cloak, negative starts fading out, and zero is stable.
  std::int16_t cloak_transition_latch = 0; // +0xC8D8
  // Per-ship cached cloak-scanner presentation capabilities. The original
  // populates +0xC91C/+0xC91E from ModType 30 bits 0x0002/0x0001 (screen/radar)
  // in Ship_UpdateVisualState (0x00428340).
  std::int16_t cloak_scanner_reveal_screen = 0; // +0xC91C
  std::int16_t cloak_scanner_reveal_radar = 0;  // +0xC91E
  // Cached ModType 17 bit 0x0008: cloaking deactivates when the ship takes
  // damage. The visual/state updater refreshes this latch lazily.
  std::int16_t cloak_damage_deactivate_latch = 0; // +0xC920
  // Ghidra ShipState +0xC922. Voice/comm identifier passed to the combat
  // chatter queue when a ship witnesses a kill. TODO(decomp): no producer
  // seeds this yet (personality/dude comm data); consumers read it as 0.
  std::int16_t voice_type_mode = 0; // +0xC922
  // Waypoint arrival marker pair used by ships carrying arrival markers (class
  // sprite_behavior_flags bit 1): waypoint_arrival_marker_a reflects a
  // completed arrival; marker_b counts/suppresses route restarts. -1 = none.
  // Provisional: Ship_UpdateVisualState also reuses these two shorts as the
  // control latch and cycle index for that same sprite-behavior animation.
  std::int16_t waypoint_arrival_marker_a = -1; // +0xC8FA (provisional offset)
  std::int16_t waypoint_arrival_marker_b = -1; // +0xC8FC (provisional offset)

  // Whether this ship is flagged as carrying a mining scoop outfit (ShipState
  // +0xC3 mining_scoop_active, derived by Outfit_HasMiningScoopOutfit). The
  // earlier comment placed this at +0xBE, which is escort_released_mark.
  bool mining_scoop_active = false;

  // --- Ship-comm dialog latches (Provisional; see ship_comm_dialog.cpp) -----
  // The comm window (NovaUi_RunTargetShipCommWindow 0x0047e470) reads/writes
  // three unnamed ShipState bytes. +0xB9 is the boarded-target latch (kept in
  // the main field block above as boarded_target_latch; the comm re-hire
  // writes the same byte), +0xBB gates the dialog's status label
  // (0 = "Fighter" STR# 0x7d2 0xa8, else "Captured Escort" 0xa6:
  // escort_origin_mark), and +0xBC is set after any ship-comm dialog closes
  // (comm_interacted_mark). Names are conservative/Provisional pending the
  // wider escort/fleet system.
  std::int8_t escort_origin_mark = 0;   // +0xBB (Provisional)
  std::int8_t comm_interacted_mark = 0; // +0xBC (Provisional)
  // ShipState +0xBE/+0xBF: the escort-management marks consumed by the auto
  // fleet trade pass Player_ProcessEscortFleetAtStellar (0x004229d0) — +0xBE
  // requests the escort be sold/released at the next shipyard, +0xBF requests
  // the class's UpgradeTo upgrade when affordable. Set by the (unported)
  // escort management window 0x004853a0, cleared by the upgrade pass and on
  // slot allocation / capture.
  std::int8_t escort_released_mark = 0; // +0xBE
  std::int8_t escort_upgrade_mark = 0;  // +0xBF
  // ShipState +0xC8DE post_hit_mode_hint: the AI's post-hit behavior hint
  // (written by the ship-comm escort release and post-hit state handling).
  std::int16_t post_hit_mode_hint = -1; // +0xC8DE

  // --- Misc ---
  std::int16_t timed_action_counter = -1; // +0xC908
  std::int32_t credits = 0;               // +0xA0
  bool is_active = false;                 // +0xB8

  // --- Render/input extras (kept on Ship for simplicity; the original stores
  // the engine-glow level at ShipState +0xc8d4 and per-ship muzzle geometry
  // derived from the sh\x8an descriptor). ---
  // Engine-thrust latch: whether the player is currently applying forward
  // thrust this frame (mirrors Ghidra ShipState.ai_forward_thrust_cmd at +0x30
  // being non-zero). Written by PlayerTick_ManualFlightAndRegeneration and read
  // by the flight render to drive the engine-glow layer.
  bool engine_thrust = false;
  // ShipState +0xc8d4. The player-control path raises this one unit/frame
  // while thrusting and lowers it one unit/frame otherwise; normal thrust caps
  // at 24, while the afterburner path can reach 32. The renderer maps the
  // original integer control level to alpha because the original SpriteWorld
  // blend setup is not reconstructed yet.
  std::int16_t engine_glow_level = 0;
  // Port-side cadence bank for Ship_HandleShip's integer engine-glow state
  // machine. The original mutates +0xC8D4 once per spaceflight call; the SDL
  // loop supplies fractional normalized ticks, so NPCs replay that state
  // machine once per accumulated 21 ms original-rate call.
  float engine_glow_raw_tick_accumulator = 0.0F;
  // Render alpha for the engine-glow layer, recomputed each frame in
  // NovaShip_TickWeaponSpriteAndRunningLights (Ship_UpdateVisualState):
  // (engine_glow_level + rand(0..5) - 4)/32, or 0 when that is below 2.
  // The movement code also writes a provisional level/24 here, but the visual
  // tick overwrites it before the frame is drawn.
  float engine_glow_intensity = 0.0F;

  // Per-turret-group quadrant rotation state (next barrel to fire), -1 until
  // the first shot chooses a random quadrant (Weapon_SelectTurretQuadrant
  // 0x0046c320; original per-ship state at ShipState +0xc8fe, one slot per
  // turret group). The barrel geometry itself is per CLASS: see
  // ShipClass.muzzle_* (scenario_data.hpp).
  std::array<std::int8_t, 4> muzzle_quadrant{-1, -1, -1, -1};
};

// Historical name for `Ship`; kept so existing player-ship helpers compile
// unchanged after the rename. New code should use `Ship` directly.
using PlayerShip = Ship;

// The new pilot's identity and new-game dialog selections, filled by
// Menu_RunPilotSelectionDialog (0x0048a7e0). Ghidra keeps the names in
// DAT_007d20b7 (Full Name, also the <name>.plt file stem) / DAT_007d21b7
// (Nickname), the character-template choice in DAT_007d22b7, and the Gender
// popup selection in DAT_007d23b7 (first char latched into DAT_00734c1c).
struct PilotData {
  std::string first_name;
  std::string last_name;
  std::int16_t start_type_code = 0; // PilotData_ResolveStartType result
  // Row-4 Strict Play checkbox (the new-pilot flag, DAT_00596d2f).
  bool strict_play = false;
  // Row-11 Gender popup (MENU 0x1f4); 'm' latch = DAT_00734c1c.
  bool male = true;
  // Row-13 Character popup (MENU 0x1f5): the selected ch r template name
  // (DAT_007d22b7), empty when the 0xc1e variant left the popup offscreen.
  std::string character_template;
};

// Persistent scenario-control state (the original pilot NCB/control-bit
// payload).  Availability expressions read it and OnPurchase/OnSell/OnRetire
// scripts mutate it; keeping it in GameState makes those effects saveable
// rather than ephemeral UI latches.
struct PilotControlState {
  // The Bible defines exactly 10,000 Nova control bits (b0..b9999).
  static constexpr std::size_t kControlBitCount = 10000;
  std::bitset<kControlBitCount> bits;
  std::bitset<0x800> explored_systems;
  bool registered = true;
  bool male = true;

  // One-shot outfit-effect latches (Ghidra DAT_007d4c08 / DAT_007d4c09). The
  // outfitter clears both on entry (NovaUi_RunOutfitterInteractionLoop
  // 0x0048ea70); Outfit_GrantOutfitToPlayer (0x00427770) sets the map latch
  // whenever a ModType-16 map outfit is granted and the record latch when a
  // ModType-21 record-clear actually runs; NovaLanded_CanBuyOutfit
  // (0x00491950) refuses to sell a map outfit while the map latch is set (or a
  // record-clean outfit while the record latch is set). Net effect: one map
  // purchase (and one record clean) per outfitter visit. UI latches, not
  // saveable state.
  bool map_grant_latch = false;    // DAT_007d4c08
  bool record_grant_latch = false; // DAT_007d4c09

  // Fresh pilots begin with the ordinary passenger-ferry service enabled.
  // Shipped ferry missions use (P0 & b311) & !b312; b312 is a later
  // progression/lockout bit. This is provisional until the full pilot-control
  // block is restored from save data, but keeps the baseline mission board
  // usable in the clean-room runtime.
  PilotControlState() { bits.set(311); }

  [[nodiscard]] bool ControlBit(std::uint32_t bit) const {
    return bit < kControlBitCount && bits.test(bit);
  }

  void SetControlBit(std::uint32_t bit, bool value) {
    if (bit < kControlBitCount) {
      bits.set(bit, value);
    }
  }
};

// In-flight Escort Commands overlay + group-order state (Ghidra g_target_
// category_panel_timer, g_selected_target_category, g_target_category_
// command[4], g_target_category_key_latch[5], g_escort_command_key_latch[4],
// DAT_007cab49 toggle latch, DAT_007cab56 attached-count cache,
// g_target_category_key_time_ms). Reconstructed in escort_commands.cpp.
struct EscortCommandState {
  static constexpr std::size_t kGroupCount = 4;
  // > 0 while the overlay is shown (Ui_ShowTargetCategoryPanel 0x0049e8d0
  // writes 0x20; decayed only while the toggle key is held past 0x1e0 ticks,
  // and cleared by the toggle or on system arrival).
  std::int16_t panel_timer = 0;
  // Selected group: -1 = All Ships, 0..3 = class_category group.
  std::int16_t selected_category = -1;
  // Current order per group: 0 Formation, 1 Defend, 2 Attack, 3 Return to
  // Hangar, 4 Hold Position (STR# 0x7d2 0x91..0x95).
  std::array<std::int16_t, kGroupCount> group_command{};
  std::array<std::uint8_t, 5> select_key_latch{};
  std::array<std::uint8_t, kGroupCount> order_key_latch{};
  std::uint8_t panel_toggle_latch = 0;
  // Filtered attached-ship count cache (DAT_007cab56), -1 when stale.
  std::int16_t attached_count_cache = -1;
  std::int64_t key_time_60hz = 0;
  // One-frame close request from the toggle (the original's local_265).
  bool close_pending = false;
};

// Travel-selection / cross-system jump state. Ghidra keeps the jump/landing
// interaction in ship fields (ai_secondary_target_slot as the travel slot,
// g_travel_selected_stellar_id / g_travel_engage_timer) plus a set of
// Stellar_* helpers (Stellar_FindNearestAvailableTravelStellar /
// Stellar_CanShipInitiateJumpSequence / the hypergate jump sequence in
// Stellar_HandlePlayerHyperspaceSequence). The reimplementation models the
// cross-system hyperspace jump as an explicit state machine (travel.cpp) so it
// is reconstructable without the NPC-fleet, landing or dialog systems.
struct TravelState {
  // The travel-slot index (0..15) of the travel point the player is engaging,
  // or -1 when no travel is active. Mirrors Ghidra ai_secondary_target_slot
  // used as the adjacency-slot selector. The paired destination system is
  // System.links[slot].
  std::int16_t travel_slot = -1;
  // The zero-based system id the player plotted as the next jump destination
  // via the galaxy starmap, -1 when nothing is plotted. When a jump completes,
  // this is cleared. 'j' jumps toward this system if it is directly linked
  // from the current system; otherwise the jump falls back to the nearest
  // available travel point.
  std::int16_t starmap_destination_system_id = -1;
  // The plotted starmap route (Ghidra DAT_00735404, 32 shorts; 0xffff = empty
  // slot): route[0] is the current system and route[1..] the planned hops.
  // Shift-clicking systems in the starmap appends/truncates hops; a plain
  // click plots only the immediate destination above. When a jump arrives at
  // route[1] the hop is consumed (System_NormalizePlannedRouteToCurrentSystem
  // 0x004a7fc0) and the HUD travel slot re-syncs from the next hop
  // (NovaUi_SyncTravelSelectionFromStarmapRoute 0x004a8080).
  std::array<std::int16_t, 0x20> starmap_route = [] {
    std::array<std::int16_t, 0x20> route{};
    route.fill(-1);
    return route;
  }();
  // The stellar resource id currently targeted for travel/landing, mirroring
  // the original's ai_secondary_target_slot with travel_transfer_mode == 2.
  // -1 when nothing is targeted. Only set by explicit player commands (Tab
  // cycle, land command's nearest auto-pick, starmap plot) -- the original
  // never auto-seeds it per frame -- and cleared on system arrival.
  std::int16_t selected_stellar_id = -1;
  // True while the selection came from an explicit player command rather than
  // an auto-pick. Vestigial since the per-frame nearest-stellar auto-seed was
  // removed (the original has none); kept for the starmap-plot path.
  bool selected_stellar_is_manual = false;
  // Ghidra g_travel_engage_timer (0x00597a0e). Landing/docking approach
  // progress: -1 disarmed; NovaUi_UpdateTravelEngagementProgress arms it to
  // 0x2ee when the selected stellar is within 250 px on both axes, increments
  // it past 0x2ec, and expires it (> 0x7ff) back to -1 clearing the selection.
  // The normal-arrival gate (Stellar_HandleStellarEntryAndExit 0x00457e05)
  // requires >= 0x2ee before it will dock.
  std::int16_t engage_timer = -1;
  // Legacy latch retained for future arrival/docked reconstruction. Target
  // selection and target action do not set it: they only select a travel
  // stellar and open its destination-interaction window.
  bool landed_this_frame = false;
  // The stellar resource id the player is jumping from (the travel point that
  // was engaged), -1 unless travel is active. The new-game flow also uses
  // selected_dest_id as its initial target.
  std::int16_t engaged_stellar_id = -1;
  // The resolved destination system resource id for the engaged jump.
  std::int16_t destination_system_id = -1;
  // Hyperspace-mode latch (H): when set, leading Backslash destination-system
  // presses (cycle_destination_*) choose the next jump system, and the HUD
  // travel panel reads as "Hyperspace". Mirrors the original's command 0x60
  // travel_transfer_mode-3 channel plus the manual's "press H to set the
  // nav computer to hyperspace mode". Cleared when a jump completes or on
  // system change.
  bool hyperspace_mode = false;
  // The visible jump transition, driven by `jump_phase` until arrival.
  bool engaging = false;
  // Whether a jump completed this frame (consumed by the spaceflight loop to
  // re-spawn the starfield once). Set at the fire moment (the system change),
  // cleared each tick.
  bool just_completed = false;
  // Brake to a near stop, then the stationary warp-up hold (whose tail is the
  // tunnel: the ship accelerates along the jump bearing while the Warp up cue
  // finishes), then the fire/arrival. The fire hurls the ship 1350 px past
  // the destination center at max speed and returns control to normal flight
  // immediately; there is no post-fire tunnel in the new system.
  enum class JumpPhase { kIdle, kBrake, kHold };
  JumpPhase jump_phase = JumpPhase::kIdle;
  // Whether Warp up has been started for this jump.
  bool warp_up_started = false;
  // The one-shot flight-tutorial hint state (Ghidra DAT_007cab1c, a global
  // stepped by the per-second hint overlay block of Ship_HandlePlayerShipCore
  // 0x0044aa70; -3 -> -2 -> -1 -> 0 -> 1 -> 2). Values >= 3 only occur as the
  // 0x7fff latch set at the jump hold-begin (0x0044c561), hyperspace arrival
  // (0x0044f83f) and Ship_ResetPlayerShipState (0x004b3a3b; TODO(decomp) when
  // the death respawn is ported); while latched, landing shows the launch
  // departure message (Stellar_RunDockAndLaunchSequence tail 0x00456323)
  // instead of re-arming the hints. A landing taken below 3 resets to -1. A
  // fresh pilot starts at -3 (0xfffd, new-game state reset 0x0048a600). The
  // hint overlay texts themselves (the DAT_0072exxxcc queue) are TODO(decomp).
  std::int16_t travel_hint_state = -3;
  // Hold-phase elapsed time in 30 Hz simulation ticks. The fire lands once the
  // hold passes g_hyperspace_engage_hold_30hz (0x5755a8, 30 ticks) AND the Warp
  // up cue has finished playing (the original's NovaAudio_CountActiveByHandle
  // gate, mirrored through hold_audio_latch).
  float hold_ticks = 0.0F;
  bool hold_audio_latch = false;
  // Rising-edge latch for the "entered jump range" cue (Ghidra DAT_007cab34):
  // set while a plotted (mode-3) jump is armed and the ship is far enough
  // from the system center to jump, cleared otherwise. The transition-sound
  // cue fires on the 0 -> 1 edge (see TickJumpRangeCue in travel.cpp).
  bool jump_range_cue_latch = false;
  // Tunnel ramp clock in 1/60 s ticks, accumulated since the hold began (the
  // original stamps ai_mode_start_time_ms when the hold starts, 0x0044c4e9,
  // and reads its 60 Hz tick counter in the tunnel block -- NOT ms despite
  // the ai_mode_start_time_ms name; see NovaTime_GetTickCount60Hz). Drives
  // the in-tunnel position ramp: progress = elapsed*multiplier
  // /(364*0.01) - 35/multiplier px/tick, capped at 50, applied while the hull
  // faces the jump bearing (onset ~2.1 s, cap ~5.2 s, boom at the ~6.1 s cue
  // end for a mult=1 stock ship).
  float tunnel_elapsed_60hz = 0.0F;
  // The jump direction in the reimpl's radians heading convention (0 = up,
  // clockwise): the bearing from the departure point toward the destination
  // system center. The brake/hold align the hull onto this bearing for the
  // zoom thrust, and the arrival spawn is offset from the destination center
  // along it.
  float jump_heading_rad = 0.0F;
};

// The outfit-driven effective ship stats (mirrors the cached outputs of the
// Ghidra Ship_ComputeShip* helpers). Stored on GameState so the spaceflight
// loop reads a cached snapshot instead of re-scanning the 0x200-entry outfit
// table every frame (the original caches these in _DAT_00735688/90/98...;
// stat_cache_valid tracks that cache's dirty state).
struct PlayerEffectiveStats {
  float max_shield_points = 0.0F; // 0x00463550
  float max_armor_points = 0.0F;  // 0x004637a0
  float fuel_capacity = 0.0F;     // 0x00463a20 (clamped [0,32000])
  float cargo_capacity = 0.0F;    // opcode 2 (+ class cargo_holds)
  float thrust_raw = 0.0F;        // 0x004640a0 raw accel (opcode 7); the
                                  // spaceflight loop /10000 for px/frame^2
  float speed_raw = 0.0F;         // opcode 8 raw speed
  float turn_raw = 0.0F;          // opcode 9 raw turn (deg/frame after *0.1)
  float shield_recharge = 0.0F;   // 0x00463b30 opcode 5
  float armor_recharge = 0.0F;    // opcode 29 armor repair rate
  float fuel_regen_rate = 0.0F;   // 0x00463b30 fuel-scoop units per frame
                                  // (class FuelRegen + opcode 18 scoops)
  int max_guns = 0;               // class MaxGun + opcode 45
  int max_turrets = 0;            // class MaxTur + opcode 46
};

// The discrete player-owned inventory: stackable outfit counts, the 6 cargo
// compartments, and the junk quantities. Mirrors the original globals
// g_outfit_owned_count, ShipState.field_0x7a..0x84 (cargo bins) and the
// g_junk_defs strided array.
struct PlayerInventory {
  // How many of each outfit the player owns, indexed by (outfit id - 0x80).
  // Ghidra g_outfit_owned_count[0x200].
  std::array<std::int16_t, 0x200> outfit_owned_count{};
  // The 6 standard cargo compartments (Ghidra ShipState.field_0x7a..0x84,
  // tons each).
  std::array<std::int16_t, 6> cargo_bins{};
  // Junk quantities (Ghidra g_junk_defs strided array), summed for the
  // total-held cargo bookkeeping.
  std::array<std::int16_t, 0x80> junk_counts{};
};

// A single fired round (clean-room stand-in for one Ghidra ShotState).
// TODO(decomp): once Shot_SpawnShotFromWeapon and the shot sprite-world are
// reconstructed this moves onto the real shot records; here it only carries the
// physics/visual fields the basic firing path consumes.
struct ActiveShot {
  // Which weapon fired this round (zero-based weapon id; used as the bank
  // slot and to look up the WeaponDef stats for drawing/lifetime).
  std::int16_t weapon_id = -1;
  // Ghidra ShotState.owner_ship_slot / target_ship_slot (+0x2c/+0x2a).
  // Owner attribution is needed for friendly-fire and aggro gates; a target
  // of -1 means that this basic projectile may collide with any eligible ship.
  std::int16_t owner_ship_slot = -1;
  std::int16_t target_ship_slot = -1;
  // Ghidra ShotState.system_id (+0x2e). Shots from another system are stale
  // and are discarded without applying damage.
  std::int16_t system_id = -1;
  float pos_x = 0.0F; // world x
  float pos_y = 0.0F; // world y
  float vel_x = 0.0F; // px/frame
  float vel_y = 0.0F;
  // Remaining lifetime in reference-cadence frames (WeaponDef Count).
  int life_frames = 0;
  // Fractional lifetime used by the frame loop's normalized 30 Hz cadence;
  // life_frames is retained as the rounded/debug-facing view used by the
  // renderer and existing tests.
  float life_ticks_remaining = 0.0F;
  // Provisional circle envelope replacing the original shot sprite half-span
  // and pixel-mask test when the shot sprite mask has not been resolved by the
  // collision refresh pass.
  float collision_radius_px = 2.0F;
  // Per-frame opaque-pixel mask for the shot's current weapon sprite frame
  // (heading-selected or animation-cycled).
  CollisionMaskBinding collision_mask;
  // Set by collision or expiry; the simulation removes these after the pass.
  bool consumed = false;
  // Ghidra ShotState.damage_decay_elapsed_ticks / damage_decay_points. The
  // elapsed value accumulates normalized 30 Hz ticks; each completed Bible
  // Decay interval adds one point of direct-hit mass/energy damage reduction.
  float damage_decay_elapsed_ticks = 0.0F;
  std::int16_t damage_decay_points = 0;
  // Ghidra ShotState.impact_variant, used later by impact visual/effect code.
  std::int8_t impact_variant = 0;
  // Ghidra ShotState.linked_shot_generation (+0x36), zeroed by
  // Shot_SpawnShotFromWeapon and set to (parent + 1) by
  // Shot_SpawnLinkedShotsOnImpact. Compared against the weapon's
  // range_link_extra_count (Bible SubLimit) to stop recursive submunitions.
  std::int16_t linked_shot_generation = 0;
  // Time-animated shot-frame stepping (Ghidra ShotState.frame_cycle_index / +
  // anim_elapsed). For a weapon whose flags_primary bit 0 is SET the shot uses
  // Shot_HandleShot's animated branch: anim_elapsed accumulates normalized
  // 30 Hz ticks and each time it crosses the weapon's animation-frame delay,
  // frame_cycle_index advances (wrapping at the frame count). Only the
  // animated-branch shot sets need these; static/heading sets leave them
  // unused.
  int frame_cycle_index = 0; // ShotState.frame_cycle_index (+0x3e)
  float anim_elapsed = 0.0F; // ShotState.anim_elapsed, in ms
  // Ghidra ShotState.heading_deg (+0x20): flight heading in game degrees
  // [0,360). Guided shots integrate this toward the target bearing and rebuild
  // velocity from it each frame; the renderer maps it to the shot sprite frame
  // (frame_count * heading / 360).
  float heading_deg = 0.0F;
  // Ghidra ShotState.guidance_state (+0x30): guidance state latch. 0 =
  // normal homing, 1 = asteroid-decoy tracking (target_ship_slot then indexes
  // the asteroid pool), 998 = inert (target lost), 999 = interference weave.
  std::int16_t guidance_state = 0;
  // Ghidra ShotState +0x40: guided-weapon durability against point-defense
  // hits. A value below 1 makes the next PD hit destroy the shot immediately.
  std::int16_t point_defense_durability = 0;
  // Ghidra ShotState.lock_quality_0..3 (+0x38): per-seek-channel jam
  // vulnerability rolls seeded from the weapon's jam_vuln at spawn
  // (Random(0..vuln)); compared against the target's jamming score in
  // NovaWeapon_UpdateShotGuidance.
  std::array<std::int16_t, 4> lock_quality{};
};

// Ghidra g_beam_hit_queue / Shot_QueueBeamHit (0x00427a90). The original
// keeps 0x40 immediate beam records at a 0x22-byte stride. Rendering fields
// are retained here even though SDL beam drawing is deferred, so the gameplay
// queue and its lifetime match the source before visual work is added.
struct BeamHit {
  float source_x = 0.0F;
  float source_y = 0.0F;
  float target_x = 0.0F;
  float target_y = 0.0F;
  std::int16_t lifetime_ticks = -2; // < -1 means inactive in the original
  // Port-only sub-tick accumulator. The original decrements lifetime_ticks by
  // exactly 1 per fixed 30-tick/s TickSystems call, but this port drives the
  // sim once per rendered frame with a fractional elapsed_ticks (0.5 at 60fps).
  // lifetime_ticks stays the authoritative whole-tick count (matching the
  // 0x22-byte record); this float absorbs the remainder so beams expire at the
  // correct wall-clock time instead of stalling when truncated to int16.
  float lifetime_remainder = 0.0F;
  // Ghidra record +0x12: decay-phase counter. Only incremented while the
  // beam's lifetime sits at 0 with a positive WeaponDef Decay value (Bible
  // "Decay"); holds the beam on screen until counter + beam_falloff >= 0x10
  // and drives the corona-shrink / lightning-fade in the beam renderer.
  std::int16_t animation_counter = 0;
  std::int16_t weapon_id = -1;
  std::int16_t owner_ship_slot = -1;
  std::int16_t target_ship_slot = -1;
  // Mode-10 point defense targets the fixed shot pool rather than a ship.
  // Kept separate in the vector port so a shot index can never be mistaken
  // for a ship slot by the direct-hit resolver.
  std::int16_t target_shot_slot = -1;
  std::int16_t turret_quadrant = -1;
  std::int16_t turret_group_id = -1;
  std::int16_t forced_targeting = -1;
  std::int8_t impact_variant = 0;
  bool impact_resolved = false;
};

// Ghidra ImpactEffectInstance (g_impact_effect_instances_ptr, 0x005912b8),
// 32 entries at a 0x18-byte stride. `anim_time < 0` is the inactive sentinel;
// delay_timer keeps the original delayed child-impact behavior.
struct ImpactEffectInstance {
  float pos_x = 0.0F;
  float pos_y = 0.0F;
  std::int16_t effect_id = -1;
  float anim_time = -1.0F;
  float delay_timer = 0.0F;
};

// Ghidra FadingEffectSpriteState (g_fading_effect_pool, 0x00596d00),
// 32 entries at a 0x18-byte stride. These are the directional fragments
// emitted while a destroyed ship's death presentation is running. Lifetime
// and velocity use the original normalized frame-time units.
struct FadingEffectInstance {
  float pos_x = 0.0F;
  float pos_y = 0.0F;
  float vel_x = 0.0F;
  float vel_y = 0.0F;
  float lifetime_ticks = -1.0F;
  float heading_radians = 0.0F;
};

// Ghidra SWParticle (g_swparticles_entries, 0x2c-byte stride). A single-pixel
// soft particle emitted by weapon impact bursts and asteroid-debris packages.
// Positions and velocities are 8.8 fixed-point: the original stores world
// coordinates shifted left 8 and integrates velocity directly per 30 Hz tick
// (the whole pool is allocated with gravity 0 at 0x004abd8d's
// SWParticles_AllocatePool(100000, 0)). `life_ticks` counts down and the slot
// frees at zero; `blend_mode` is the original's surface blend selector (0x20
// from every weapon callsite). The clean-room uses a growable vector instead
// of the 100000-
// entry fixed pool, so slot recycling order does not matter.
struct SwParticle {
  std::int16_t life_ticks = 0;    // +0x00 remaining ticks; <= 0 inactive
  std::int32_t pos_x = 0;         // +0x04 8.8 fixed world x
  std::int32_t pos_y = 0;         // +0x08 8.8 fixed world y
  std::int32_t vel_x = 0;         // +0x14 8.8 fixed x velocity per tick
  std::int32_t vel_y = 0;         // +0x18 8.8 fixed y velocity per tick
  std::int16_t blend_mode = 0x20; // +0x24 surface blend selector
  std::uint32_t color = 0;        // +0x28 0x00RRGGBB
};

// Ghidra FreeflightObjectState (g_freeflight_objects_ptr, 0x005914a8),
// 64 entries at a 0x28-byte stride. Generic in-flight cosmetic objects:
// the cargo/junk pods spawned by Player_RedistributeFleetCargoOverflow's
// jettison pass (Ship_SpawnFreeflightObjectForShip 0x0041f800), plus the
// beam-hit / effect-package / launched-drone variants spawned by
// Ship_SpawnFreeflightObjectAtPosition (0x0041fb50). Positions and lifetimes
// use the original normalized 30 Hz frame-time units; `lifetime_ticks < 0`
// is the inactive sentinel.
struct FreeflightObjectState {
  float pos_x = 0.0F;           // +0x04
  float pos_y = 0.0F;           // +0x08
  float vel_x = 0.0F;           // +0x0c
  float vel_y = 0.0F;           // +0x10
  float lifetime_ticks = -1.0F; // +0x14
  // Animation frame accumulator (advanced by spin_rate each tick and wrapped
  // in [0, kFreeflightSpinFrames) by the tick).
  float frame_counter = 0.0F;  // +0x18
  std::int16_t system_id = -1; // +0x1c
  // Sprite-set index into the 500+index spin table (0 = the stock cargo/junk
  // spin set for the jettison variant).
  std::int16_t sprite_set_index = 0; // +0x1e
  // Frame advance per tick: one of -1, 0 or +1 (Ghidra draw-mode).
  std::int16_t spin_rate = 0; // +0x20
  std::int16_t extra = 0;     // +0x22 (SpawnAtPosition payload id)
  // Persistent latch: set by the at-position variant (launched drones /
  // shuttles, 0x0041fb50) and clear for the jettison pods. The freeflight
  // scoop arm of Ship_HandleSpritePairCollision (0x004374f0) only collects
  // objects with this set, so asteroid YieldType resource-boxes are scoopable
  // while jettisoned cargo pods are not.
  bool persistent = false; // +0x24

  // Per-frame opaque-pixel mask for the object's current spin frame, resolved
  // by RefreshCollisionMasks (collision.cpp) for the mining-scoop overlap.
  CollisionMaskBinding collision_mask;

  // Pool size for the 64-slot FreeflightObjectState table.
  static constexpr std::size_t kPoolSize = 0x40;
};

// Transient on-screen HUD overlay message state, mirroring the original's
// g_hud_overlay_msg_buffer / g_hud_overlay_msg_color pair written by
// NovaHud_ShowOverlayMessage (0x0047e2d0) and replayable by
// NovaHud_ShowCachedOverlayMessage (0x0047e430). Clean-room: the drawn text
// colour is carried as an RGB, and a wall-clock expiry replaces the original's
// fixed frame durations so the spaceflight loop can clear it. Game logic only
// calls NovaHud_ShowOverlayMessage to arm it; the HudRenderer draws it.
struct HudOverlayState {
  // Whether a message is currently live (the original's g_hud_overlay_msg_color
  // > 0 gate). Cleared once the expiry passes.
  bool active = false;
  // The cached message text (g_hud_overlay_msg_buffer), shown at the bottom of
  // the flight viewport.
  std::string message;
  // Drawn text colour (RGB). Matches the overlay text colour reference
  // (Ghidra DAT_00733b44, the 00 rr gg bb colour the show call passes).
  std::uint8_t red = 0xe0;
  std::uint8_t green = 0xe0;
  std::uint8_t blue = 0xe0;
  // Absolute wall-clock deadline (SDL_GetTicks ms) after which the message
  // disappears. Set by NovaHud_ShowOverlayMessage from its duration.
  std::uint64_t expiry_ms = 0;
};

// A single asteroid / drift-debris drift record (the r\xf6id asteroid/manoeuvre
// family). The original keeps 16 of these in one global pool
// `g_asteroid_states` (16 x 0x24 bytes) and spawns them with
// Asteroid_SpawnRecord (0x00421e60) from effect packages (impact debris) and
// the travel-scene walker; the per-tick drift is Asteroid_UpdateSprites
// (0x00436910). Layout mirrors the Ghidra AsteroidState so a future
// sprite/drift layer can port verbatim.
struct AsteroidState {
  // Placeholder for the Ghidra per-record `Sprite *` (+0x00) cloned from
  // g_asteroid_sprite_sets[wander_type] (FUN_004af020). The clean-room keeps
  // sprites in the view's SpriteStore cache, so this slot is unused.
  std::int32_t sprite_handle = 0; // +0x00
  float target_pos_x = 0.0F;      // +0x04
  float target_pos_y = 0.0F;      // +0x08
  float target_vel_x = 0.0F;      // +0x0c
  float target_vel_y = 0.0F;      // +0x10
  // Sprite animation phase. Seeded from the type sprite set's frame count
  // (+0x54) at spawn and advanced by wander_speed each tick, wrapping via
  // that frame count (Asteroid_UpdateSprites 0x00436910).
  float wander_frame_accumulator = 0.0F; // +0x14
  float wander_speed = 0.0F;             // +0x18
  // Integrity counter seeded from AsteroidDef.strength. Weapon splash
  // decrements it by the weapon's shield damage (x10 when the weapon has
  // flags_secondary 0x8000); below zero the destruction package runs
  // (NovaUi_ResolveWeaponSplashImpact 0x00436ff0 ->
  // Asteroid_SpawnDestructionPackage 0x00462550); <= -32000 retires the record
  // in Asteroid_UpdateSprites.
  std::int16_t integrity = 0;   // +0x1c
  std::int16_t wander_type = 0; // +0x1e (index into the asteroid-type table)

  // Clean-room circle fallback for the original's per-frame pixel-mask overlap
  // (Sprite_TestPixelMaskOverlap) when no asteroid sprite mask has been
  // resolved. All shipped asteroid spin sets (800..815) are 50x50, so the
  // half-span is 25.
  float collision_radius_px = 25.0F;
  // Per-frame opaque-pixel mask for the asteroid's current wander sprite.
  CollisionMaskBinding collision_mask;

  bool active = false; // +0x20

  // Pool size for the 16-slot AsteroidState table.
  static constexpr std::size_t kPoolSize = 16;
};

// Everything about the running pilot's world. Replaces the Game_Reset* set of
// globals for the transient not-yet-reconstructed subsystems with explicit
// flags so we can log exactly what is and is not preserved.
//
// Accepted padding (clang-analyzer-optin.performance.Padding): the checker's
// optimal order puts the `player` reference before the `ships_` array it binds
// to, which would be a use-before-construction; the fields stay grouped by
// subsystem instead.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct GameState {
  // Size of the original global ship array `g_ship_states` (0x40 slots).
  // Index 0 is the player; the remaining slots hold NPC ships.
  static constexpr std::size_t kMaxShips = 0x40;

  // The ship slots (mirrors `g_ship_states`). Index 0 is the player; the
  // reimplementation keeps it reachable both through `ships[0]` and the
  // convenience reference `player`. `ships_` is private; use the accessors
  // below (or GameState.player for the index-0 ship).
  std::array<Ship, kMaxShips> ships_;

  // Convenience alias for ships_[0] (the player). Kept so the large existing
  // `state.player` codebase needs no churn; the same object lives in the ship
  // array, matching the original g_ship_states[0].
  Ship &player;

  // Binds `player` to ships_[0]. GameState is never copied or moved in this
  // codebase, so the reference member is safe; it prohibits assignment of the
  // whole struct (no caller does).
  GameState() : player(ships_[0]) {}

  [[nodiscard]] Ship &ShipAt(std::size_t slot) { return ships_[slot]; }

  [[nodiscard]] const Ship &ShipAt(std::size_t slot) const {
    return ships_[slot];
  }

  [[nodiscard]] bool SlotInRange(std::size_t slot) const {
    return slot < kMaxShips;
  }

  // Ghidra g_ship_squad_leader_slot_snapshot (DAT_00591100): per-tick copy of
  // every ship's squad_leader_ship_slot taken by the Frame_TickSystems scope-6
  // pass before Ship_ReacquireSquadLeader (0x004156a0) runs, so leadership
  // succession can find squadmates of the stale leader.
  std::array<std::int16_t, kMaxShips> squad_leader_slot_snapshot{};

  // Seeded PRNG backing the new-game flow's random opener strings and start
  // selection. The original uses a global NovaRandom; this is kept local to
  // the state so runs are reproducible when seeded identically.
  std::mt19937 rng{42};
  // Ghidra g_player_combat_rating_points: aggregate combat-rating score.
  // Produced by Frame_AddCombatRatingPoints (0x0046f1e0, ported in
  // collision.cpp) on player kills; consumed by the afterburner eligibility
  // roll (Ship_CanShipUseAfterburner 0x0046b260), the combat-odds score
  // (Ship_UpdateShipCombatOddsScore 0x004133f0), and the mode-6 evasive-break
  // gate (NovaAi_PlayerCombatRatingGate 0x0046b330).
  std::int32_t player_combat_rating_points = 0;
  // Ghidra DAT_007353f6..0x7353fd: four persisted player stat modifiers held
  // as percentages. [0]/[1] random-walk +-1 with 2-in-3 probability, clamped
  // to [0x55,0x73] = [85,115], at Frame_JitterPlayerStatModifiers 0x00431480;
  // [2]/[3] reroll to rand(0x15)+0x5a = [90,114] at Frame_RerollPlayerStat-
  // Modifiers 0x00431500. Both run at the launch tail and at the in-flight
  // jump arrival; Ship_ResetPlayerShipState (0x004b3e36) seeds 100. Persisted
  // in the pilot save at FleetState block2 +0x3588. Provisional: the gameplay
  // consumer is not decoded (TODO(decomp)).
  std::array<std::int16_t, 4> player_stat_modifier_pct{100, 100, 100, 100};
  bool game_active = false;  // Ghidra DAT_00596d28
  bool intro_played = false; // Ghidra DAT_00596d35: cleared on new pilot so
                             // the intro cinematic plays on first flight.
  PilotData pilot;
  PilotControlState control;
  TravelState travel;
  EscortCommandState escort;

  // In-flight route-map overlay state (the small non-blocking galaxy chart).
  // Ghidra g_routeMapVisibleFlag 0x007354b2 (1 while the overlay is up;
  // cleared 500 ticks after the last interaction -- 0x0042f23e),
  // g_routeMapInteractionTick60hz 0x00597970 (60Hz tick of the last overlay
  // interaction; basis of the fade at 0x00439bd0), g_route_map_zoom_scale
  // 0x005759e0 (x1.3333/x0.75 steps, capped [0.5, 2.0], reset to 1.0 on
  // open) and g_playerRouteMapZoomCommandLatch 0x007cab47 (zoom edge latch).
  // See route_map.hpp for the full chain.
  struct {
    bool overlay_visible = false;
    std::uint32_t interaction_tick_60hz = 0;
    float zoom_scale = 1.0F;
    bool zoom_command_latch = false;
  } route_map;

  // Galaxy starmap view state (Ghidra g_starmap_zoom 0x005759d8,
  // g_starmap_pan_origin_x/y 0x005997b4). Zoom is a world->screen DIVISOR
  // (bigger = more zoomed out; the original's data default 0.5625 opens the
  // map at ~178% magnification). The pan origin is the world point projected
  // to the map panel centre; the original resets it to the current system's
  // position on jump arrival (0x0044f8a6) and leaves it untouched on map
  // open, so the view persists across map sessions within a game.
  float starmap_zoom = 0.5625F;
  float starmap_pan_x = 0.0F;
  float starmap_pan_y = 0.0F;
  IntroCinematicData intro_cinematic;
  // The transient HUD overlay message (see HudOverlayState). Kept on GameState
  // per AGENTS.md (no hidden globals) and rendered by the HudRenderer.
  HudOverlayState hud_overlay;

  // --- Stellar radar / minimap (Ghidra globals consumed by
  // NovaUi_DrawStellarRadarPanel 0x0045d600) -----------------------------
  // Ghidra g_proximity_scan_detected (0x007caba0): per-tick interference roll
  // latch (Frame_RollProximityScanDetection 0x0045d030). While set the radar
  // draws sensor static instead of contact blips.
  bool proximity_scan_detected = false;

  // Target-reticle pulse values (Ghidra g_travel_target_reticle_pulse
  // DAT_00735490 / g_ship_target_reticle_pulse DAT_00735494). Set to 256.0
  // (0x43800000) when the corresponding target is (re)selected, then decay
  // toward 0.0 at the original's rate: each frame the updaters subtract
  // g_avg_frame_time_ms * 60.0 (DAT_005753c8), and the avg frame time's EMA
  // steady state is delta_ms * 0.03, so the effective rate is delta_ms * 1.8
  // (see the spaceflight loop). The pulse drives the bracket "grow out then
  // settle" offset.
  float travel_reticle_pulse = 0.0F;
  float ship_reticle_pulse = 0.0F;

  // Full-screen flash intensity [0..1] at the hyperspace fire moment (the
  // original's centered effect 0x32 queued via
  // NovaAudio_QueueCenteredSound at jump engage -- the 'boom' white
  // frame). Set to 1.0 when the boom fires at the jump's arrival, then decayed
  // by the spaceflight loop; the in-game frame draw overlays a white fullscreen
  // rect with this alpha. 0 when no flash is active.
  float screen_flash_intensity = 0.0F;

  // Parsed scenario data (ships/outfits/weapons/stellars/systems), loaded once
  // so the gameplay loops can look up classes by id. Empty until a game is
  // created (mirrors the original lazily loading scenario tables in
  // NovaData_LoadScenarioResourceTables on the new-game path).
  ScenarioData scenario;

  // Non-SDL frame-mask cache for the collision refresh pass. Shared so a
  // GameState copy keeps the same lazily-decoded masks without re-reading the
  // archives; null until the first collision pass needs it.
  std::shared_ptr<SpriteMaskStore> sprite_mask_store;
  // When true (default) the direct-contact pass refreshes each entity's
  // current-frame sprite mask before testing. Tests that exercise collision
  // *logic* (damage, lifetime, splash) disable this so they stay independent of
  // the shipped sprite geometry; mask-specific tests inject bindings directly.
  bool collision_masks_enabled = true;

  // Mission runtime tables. The original stores 16 accepted missions and
  // their 20-byte runtime flag records in separate globals; keeping the same
  // slot count makes later activation/resolve work one-to-one with Ghidra.
  static constexpr std::size_t kMaxActiveMissions = 16;
  std::array<ActiveMission, kMaxActiveMissions> active_missions{};
  std::array<MissionRuntimeFlags, kMaxActiveMissions>
      active_mission_runtime_flags{};
  std::array<MissionTargetResolution, 1000> mission_target_resolutions{};
  // Ghidra DAT_00734c20: per-definition offering roll, drawn 1..100 for every
  // mission definition on system arrival (Stellar_HandleStellarEntryAndExit
  // 0x00458802) and initialised at game start. Mission_EvaluateMissionLists
  // offers a definition only when roll <= AvailRandom (>=100 always offered);
  // Mission_ActivateMissionAtSlot's duplicate arm zeroes the roll (0x0043f5a5)
  // so a resolved mission is not re-offered until the next warp-in.
  std::array<std::int16_t, 1000> mission_offering_rolls{};
  // Ghidra DAT_00773eed: per-definition "interaction already shown" latch
  // (one byte per mission definition). Set when an offer interaction window
  // ends without accepting (Mission_TriggerReturnMissionInteractions 0x00448670
  // return -1 arm); cleared wholesale the next time the interaction walk runs
  // in a context other than 3 (DAT_00774ae2).
  std::array<std::uint8_t, 1000> mission_interaction_shown{};
  // In-game calendar (g_current_game_year_month/g_current_game_day). Seeded
  // by the new-game flow (real clock date with year + 250) and advanced once
  // per NovaMission_TickDailyWorldUpdate call.
  GameDate date{};

  // crön event runtime half (g_cron_event_states 0x005914c0, one 0x350-stride
  // block per crön def; the date-window/odds/script halves live on
  // CronEventDef). The tick (Mission_TickDailyCronEvents 0x00439500) runs
  // once per game-day.
  struct CronEventState {
    bool is_active = false;            // block +0x00
    std::int16_t duration_counter = 0; // block +0x26 (days left active)
    std::int16_t holdoff_counter = 0;  // block +0x28 (pre/post holdoff wait)
  };

  std::array<CronEventState, 0x200> cron_event_states{};
  // Per-definition daily outfit "in stock" rolls (OutfitDef +0x1e; rerolled
  // to rand(100)+1 each game-day by the world-update tail of 0x00466cb0, and
  // zeroed while the player owns the outfit -- a same-day restock latch).
  std::array<std::int16_t, 0x200> outfit_stock_rolls{};
  // Per-class daily availability rolls (ShipClassDef +0xa2a/+0xa2c, the
  // "licensed" slots; the static unlicensed halves are ShipClass::buy_random
  // /hire_random). Rerolled to rand(100)+1 each game-day. The shipyard buy
  // list offers a class while limit_roll <= buy_random; the hire lane gates
  // on the threshold pair (hire lane itself is TODO(decomp)).
  std::array<std::int16_t, 0x200> ship_class_limit_rolls{};
  std::array<std::int16_t, 0x200> ship_class_threshold_rolls{};
  // Ghidra DAT_00774ae2: the context the interaction walk last ran in.
  std::int16_t mission_interaction_context = 0;
  // Ghidra DAT_0077430e: the ship instance currently speaking a mission-ship
  // announcement (latched around Mission_ShowMissionShipAnnouncement calls;
  // -1 none). The <OSN> mission-text wildcard expands to the speaker's
  // personality display name.
  std::int16_t mission_speaker_ship_slot = -1;
  // Ghidra g_travel_scene_ctx (0x007d2b78): the landing DLOG 1000 window
  // handle, nonzero while the travel-destination window owns the world --
  // i.e. during Mission_TickReactionSlotsForTravelInteraction's landing pass
  // (NovaUi_RunTravelDestinationInteractionLoop 0x00491f30 clears it right
  // after the pass). Gates the deadline quick-fail (0x00443c60) and the
  // Mission_ClearMisnSlotAssignments despawn arm (0x00440aa0).
  bool in_travel_scene = false;
  // Ghidra DAT_00776af4: post-interaction recheck timer
  // (NovaTime_GetTickCount60Hz + NovaRandom_Range(0x1e) + 0x1e). Only the
  // services windows consume it; the port stores it for the future consumers.
  std::int32_t mission_interaction_recheck_at_ms = 0;
  // Ghidra g_recently_activated_rank_id (0x007354b6): the slot index of the
  // most recently activated rank, or -1. Rank_Activate writes it and
  // Rank_Deactivate clears it when it names that rank; <RRK> expands the
  // full name at this slot (Stellar_BuildTravelDestinationDescription
  // 0x004444f0). Initialized to -1 by Ship_InitGameplayDataTables
  // (0x004b0c20).
  std::int16_t recently_activated_rank_id = -1;

  // Mission-script side effects that need to be consumed by UI/audio layers.
  // They are explicit latches rather than hidden globals, matching the
  // clean-room state ownership rule.
  // Ghidra g_pending_transient_sound_id (0x00734c1a): the most recent P-opcode
  // sound id, or -1. The original overwrites a single slot, so this is scalar
  // rather than a queue. TODO(decomp): no UI/audio consumer is wired yet.
  std::int16_t pending_transient_sound_id = -1;
  // Ghidra g_pending_overlay_message (0x007354d0) stages the composed overlay
  // text; the port instead stores the Q opcode's STR# resource id and shows
  // the picked entry immediately. TODO(decomp): model the staged buffer.
  std::int16_t pending_script_message_string_id = -1;
  bool script_forced_leave_landing = false;
  // Ghidra g_script_mission_context_slot (0x00776b00): the mission slot whose
  // payload script is executing, or -1 when none. Mission_RunMisnScriptPayload
  // sets it around the engine call and the engine's Q case expands mission
  // text tags for slots 0..15; NovaResources_LoadMisnResourceDefs resets it to
  // 0xffff on load.
  std::int16_t script_mission_context_slot = -1;

  // Per-system faction reputation (Ghidra g_system_reputation 0x00733bc8).
  // Indexed by 0-based system resource id and sized to the systems table on
  // load. The destination-interaction dialog decrements the containing
  // system's reputation when the player attacks a stellar's government. The
  // faction-combat reaction hook (Government_ProcessFactionCombatEvent
  // 0x00466fc0) and the mission reaction script hook (Mission_ExecuteReaction-
  // Script 0x00448020) that the dialog invokes on an attack are deferred with
  // TODO(decomp) in negotiation_dialog.cpp; this field models the reputation
  // data those hooks read/write.
  SystemReputation system_reputation;

  // Mutable SystemDef +0xC4/+0xC8 reinforcement state. The scenario System
  // retains the immutable ReinfFleet/ReinfTime/ReinfIntrval resource fields.
  static constexpr std::size_t kMaxSystems = 0x800;
  std::array<std::int16_t, kMaxSystems> reinforcement_retrigger_delay{};
  std::array<float, kMaxSystems> reinforcement_countdown{};

  // The player's owned outfits, cargo and junk. The new-game flow zeroes it
  // then seeds the outfit counts from the starting ship class's default item
  // list (see new_pilot_flow.cpp). Ghidra g_outfit_owned_count + the ship
  // cargo/junk globals; see outfit.hpp/outfit.cpp for the aggregation layer.
  PlayerInventory inventory;

  // Effective-stats cache (Outfit_ComputePlayerEffectiveStats). `true` once
  // the cache is populated and nothing (outfit ownership, ship class) has
  // changed since. Invalidated by the inventory mutation helpers and whenever
  // the ship class changes.
  bool stat_cache_valid = false;
  PlayerEffectiveStats cached_stats{};

  // The original stores 0x100 weapon banks with a 100-element stride.
  std::array<std::int16_t, 0x100 * 100> weapon_bank_ammo{};
  std::array<std::int16_t, 0x100 * 100> weapon_bank_secondary{};
  // Per-weapon-bank cooldown, in reference-cadence ticks remaining before the
  // bank may fire again (Ghidra ShipState.weapon_bank_cooldown_0, a float per
  // bank). Mirrors the original: after firing, the bank's cooldown is set to
  // the weapon's reload/cooldown value and counts down each frame; the firing
  // routine only fires banks whose cooldown has elapsed. Indices are the
  // zero-based weapon id (bank slot).
  std::array<float, 0x100> weapon_bank_cooldown{};

  // Per-bank burst-cycle counter (Ghidra ShipState field_0x17c, a 200-stride
  // int16 array). Advanced once per fired volley by the fire path; when it
  // reaches Weapon_GetWeaponFireIntervalTicks (0x0046f270) the bank wraps to
  // 0 and preloads burst_reset_cooldown. Player banks; NPCs keep
  // Ship.npc_weapon_bank_burst_counter.
  std::array<std::int16_t, 0x100> weapon_bank_burst_counter{};

  // g_playerSecondaryCycleCommandLatch (DAT_007cab42): secondary-weapon-cycle
  // command edge latch, set when the cycle command executes and cleared when
  // the key is released; MarkTravel-style modal exits re-arm it so a held key
  // does not re-fire (TODO(decomp): swallow site).
  bool secondary_cycle_command_latch = false;

  // Lightweight ground-truth of fired shots (projectiles / beams) in flight,
  // reconstructed for the player's primary weapon. The original keeps these in
  // the ShotState swath (g_shot_states) with full sprite/guidance/collision
  // bookkeeping (Shot_SpawnShotFromWeapon 0x0041fd30); this clean-room model
  // carries only the fields the firing + flight renderer need so far, and the
  // projectile is drawn via its weapon's shot sprite set (spin id + 3000),
  // oriented by its velocity as Shot_HandleShot picks the heading frame.
  // Each entry is one fired round at a given world position/velocity.
  std::vector<ActiveShot> active_shots;

  // Normalized simulation scale of the most recent frame (the port's equivalent
  // of g_avg_frame_tick_scale at the time a shot spawned / last ticked). Spawn-
  // time rolls that the original expresses as Random(100 / frame_scale) read
  // this instead of threading elapsed ticks through every fire path.
  float last_frame_tick_scale = 1.0F;
  // Port-wide simulated-frame counter. Several still-display-cadenced systems
  // use this pending their own reconstruction.
  std::int16_t spaceflight_frame_counter = 0;
  // Logical mirror of Ghidra g_spaceflight_frame_counter (0x00597992) for the
  // raw-call portions of Shot_UpdateShotGuidance.
  std::int16_t shot_guidance_frame_counter = 0;
  float shot_guidance_frame_counter_accumulator = 0.0F;
  // Player self-destruct countdown (Ghidra _g_playerSelfDestructCountdown):
  // -1.0 disarmed. The self-destruct command arms 150.0; it decays by the
  // frame tick scale and detonates the player ship at <= 1.0
  // (Ship_HandlePlayerShipCore self-destruct block 0x00451954..0x00451b91).
  float player_self_destruct_countdown = -1.0F;
  // Cloak toggle command latch (Ghidra g_playerDisableSurrenderCommandLatch,
  // the binding-0x29 command in the PlayerTick_InteractionCloakAndStatus
  // region): set on the first held frame with the toggle gates passing,
  // cleared on release or while a gate rejects the command.
  std::int8_t cloak_command_latch = 0;
  // Arrival command grace. The original latches g_license_check_frame_counter
  // to -15 in the Stellar_HandleStellarEntryAndExit rebuild epilogue
  // (0x004586a6), and the interaction command blocks (special interaction
  // 0x00451bf0, mission computer 0x00451cf6) skip while it is negative. The
  // port counts a separate signed budget down to zero instead (its own frame
  // counter is unsigned and shared with cadence consumers).
  std::int16_t arrival_command_grace_frames = 0;
  // Player per-axis speed caps (Ghidra g_player_speed_cap_x 0x005997bc /
  // g_player_speed_cap_y 0x005997c0): the per-frame velocity clamp targets of
  // the manual-flight block. While the afterburner runs outside a stellar
  // gravity pull both caps jump to 1.8x the effective max speed
  // (g_afterburner_overspeed_factor 0x00575610); otherwise they decay by
  // effective thrust * 0.4 (DAT_00575680) per frame and are clamped up to the
  // effective max speed, so they are always >= max speed.
  float player_speed_cap_x = 0.0F;
  float player_speed_cap_y = 0.0F;
  // Port stand-in for g_frame_tick_count_60hz (0x00865858, NovaTime_
  // GetTickCount60Hz 0x004d5e10): wall-clock 1/60 s ticks. The original bumps
  // the global from the input-helper thread once per 16.664 ms; the port
  // re-derives it from the flight-loop frame clock (now_ms * 60 / 1000,
  // truncated) once per frame. ai_mode_start_time_ms stamps compare against
  // this counter.
  std::uint32_t tick_60hz = 0;
  // Ghidra g_target_category_command (0x007354c4, short[4]): the escort-group
  // command the player issued per ship-class category. Initialized to -1 by
  // Ship_InitGameplayDataTables (0x004b0c20); the player-core command dispatch
  // (0x00450d88, unported) writes the dialog selection and resets idle
  // categories to 0. Until that slice is ported the table stays -1, which the
  // assist supervisor (0x004048a0) coerces to command 0 (formation).
  std::array<std::int16_t, 4> target_category_command{{-1, -1, -1, -1}};

  // --- PlayerTick_StatusAndOutfitEvents (0x0044aa70 block 0x0044b240) ------
  // g_player_carried_bomb_outfit_class: 0 = no carried bomb, 1 = escape-pod
  // variant, otherwise the bomb outfit's def id. Writers (bomb purchase/
  // loading) are deferred TODO(decomp); it stays 0 until then.
  std::int16_t bomb_outfit_class = 0;
  // g_bomb_detonation_timer: countdown toward g_bomb_detonation_interval_
  // frames while a bomb is carried; rerolled (Random(100)) once it expires.
  float bomb_detonation_timer = 0.0F;
  // g_player_recently_hit_timer (Ghidra DAT_0073549c): armed to 300 ticks
  // when the player takes a hit (Shot_ResolveShipHitFromWeapon), decays one
  // tick per frame while at or above the cutoff; suppresses armor
  // regeneration and the disabled auto-repair pass until below the cutoff.
  // Outfit_RecomputeOutfitDerivedState (0x0046d4b0) resets it to -1.0.
  float recently_hit_timer = 0.0F;
  // g_player_reinforcement_inhibit_all: set by
  // Outfit_RecomputeOutfitDerivedState when the player owns a ModType 0x2c
  // outfit with ModVal -1 (Bible "no reinforcements"). The original only ever
  // sets it, never clears it.
  bool reinforcement_inhibit_all = false;
  // Ghidra DAT_00596d3a: cheat-mode latch. When set, the cheat-command suite
  // in Ship_HandlePlayerShipCore becomes live (free refits/rearm, target
  // destruction, etc., TODO(decomp): toggle commands not ported) and a few
  // helpers change behavior: Outfit_HasPlayerOwnedOutfitType0x0F_Cached
  // (0x00464760) reports the afterburner as owned, and
  // Ship_ComputeShipArmorRegenRate (0x004638e0) scales the player's armor
  // regen by k_armor_regen_player_scale_f32 (0x00575770) = 50. Stays false in
  // normal play until the cheat command block is reconstructed.
  bool cheat_mode_active = false;
  // Ghidra g_player_disable_message_shown (0x007354aa): set when the disable
  // arm of Shot_ResolveShipHitFromWeapon shows the STR# 0x7d2 0x11f "ship
  // disabled" overlay, so the destruction arm of the same hit path suppresses
  // the duplicate STR# 0x7d2 0x120 "ship destroyed" overlay. Reset each
  // flight frame by the status tick (TODO(decomp): reset site).
  bool player_disable_message_shown = false;
  // DAT_00596d36 / DAT_00596d37: current + previous 60-frame
  // any-distress-eligible-ship probe, for the distress-alert rising edge.
  bool distress_cue_active = false;
  bool distress_cue_active_prev = false;
  // DAT_00596d38: player death bookkeeping finished (latched for the
  // game-over/return-to-menu flow, consumed by the spaceflight loop).
  bool game_over_pending = false;
  // Port channel for the escape-pod bomb arm's immediate return to the menu
  // shell. The original's restart command channel (0x0044abf8) is not
  // reconstructed (TODO(decomp)); the nearby global DAT_007354a5 is the
  // general "player did a command this frame" flag (see its Ghidra
  // pre-comment), not a menu-return latch.
  bool return_to_menu_pending = false;
  // One-frame latch set by RunPlayerEjectTransform (Ghidra eject block
  // 0x004510b9): the original's timed-action dispatch has already passed when
  // the eject transform arms the escape-pod countdown, so the pod does not
  // move until the next frame. The spaceflight loop consumes this to skip the
  // PlayerTick_TimedActionTransition call on the eject frame only.
  bool timed_action_suppress_this_frame = false;

  // Port-only accumulator for Frame_TickSystems scope 0xb. The original
  // executes its discrete mission reactions, RNG rolls, and NPC population
  // maintenance once per spaceflight call, whose scheduler has a 21 ms floor.
  // Bank normalized 30 Hz time here and replay whole original-rate calls so
  // integer countdowns and the shared RNG stream do not follow display Hz.
  float npc_maintenance_raw_tick_accumulator = 0.0F;

  std::array<BeamHit, 0x40> beam_hit_queue{};
  std::array<ImpactEffectInstance, 0x20> impact_effect_instances{};
  std::array<FadingEffectInstance, 0x20> fading_effect_instances{};
  std::array<FreeflightObjectState, FreeflightObjectState::kPoolSize>
      freeflight_objects{};
  // Ghidra SWParticle pool (g_swparticles_entries). The original integrates
  // particles once per outer flight iteration, whose maximum cadence is one
  // call per 21 ms. Fractional elapsed time is banked here and whole logical
  // calls are stepped (see NovaEffects_TickSwParticles).
  std::vector<SwParticle> sw_particles;
  float sw_particle_tick_accumulator = 0.0F;

  // Ghidra g_pending_combat_chatter_{kind,government_id,variant}
  // (0x007353fe/0x00735400/0x00735402). Written by Frame_QueueCombatChatter
  // (0x00426ce0) when a ship witnesses a kill; consumed by
  // Frame_UpdateCombatChatter (0x004311f0). NovaAudio_PreloadGameplayData
  // initializes all three request words to -1.
  std::int16_t pending_combat_chatter_kind = -1;
  std::int16_t pending_combat_chatter_government_id = -1;
  std::int16_t pending_combat_chatter_variant = -1;
  // SDL replacement for g_pending_combat_chatter_handle (0x00591a8c). The
  // decoded PCM stays owned until its keyed SdlAudio voice has drained.
  std::optional<NovaSoundData> active_combat_chatter_sound;
  std::int16_t active_combat_chatter_sound_id = -1;

  // The 16-slot asteroid / drift-debris pool (mirrors the original
  // `g_asteroid_states`). Shared by Asteroid_SpawnRecord (spawn), the future
  // Asteroid_InitSystem / Asteroid_Spawn (Steps 3/4) and the step 5 per-tick
  // drift. The records are ASTEROID / drift-debris chars (r\xf6id family), not
  // NPC ships. Slots are found by scanning for `active == false`.
  std::array<AsteroidState, AsteroidState::kPoolSize> asteroid_pool{};

  // Half the logical flight play area, in pixels (Ghidra g_viewport_center_x
  // 0x005997b8 / g_viewport_center_y 0x005997ba, set by
  // Ship_InitializeMainInterface 0x004ac380 from the render owner rect). The
  // spaceflight view keeps this in sync with the live viewport each frame;
  // Asteroid_Spawn scatters new records within `viewport_center_x + 0x80` (x) /
  // `viewport_center_y` (y) of the player, so a wrong/stale value makes the
  // whole field bunch on the player.
  int viewport_center_x = 320;
  int viewport_center_y = 200;

  // "no asteroids" latch set by Asteroid_InitSystem (0x004216B0) when the
  // current system declares asteroid_count < 1. The original writes a 1 byte
  // into the random-encounter fleet-def scratch area
  // (g_random_encounter_fleet_defs[0x4d].availability_expression[0x94]);
  // the clean-room stores it here since that scratch buffer is not modelled.
  bool no_asteroids_latch = false;
  // One-shot audio requests consumed by the spaceflight loop.
  bool warp_up_sound_pending = false;
  bool warp_out_sound_pending = false;
  // Cancel a playing 'Warp up' cue (SdlAudio::StopByKey on its key). Set by
  // the disabled-jump collapse (NovaTravel_Tick), mirroring the original's
  // NovaAudio_UnregisterCallbacks on the warp-up handle at 0x0044b0d0.
  bool warp_up_cancel_pending = false;

  // Decoded player weapon fire sounds, keyed by the weapon's `fire_sound`
  // slot. Ghidra Weapon_FirePlayerWeaponBank resolves the weapon's
  // fire_sound_slot through the preloaded g_gameplay_sound_handle_table and
  // plays it via NovaAudio_PlaySpatialByDistance once a shot actually spawns;
  // this clean-room cache mirrors that table (index = slot, -1 slot = none is
  // left empty). A slot maps to the snd resource id 200 + slot (verified: the
  // Light Blaster's slot 8 is "Light Blaster.sfil" id 208).
  std::array<std::optional<NovaSoundData>, 36> weapon_fire_sounds{};

  // Ghidra FUN_004b0740 preloads the contiguous gameplay snd handle range
  // 200..455. Sharing this cache keeps impact, cloak, and other gameplay cues
  // from being silently omitted when only weapon slots are loaded.
  static constexpr std::uint16_t kGameplaySoundFirstId = 200;
  static constexpr std::size_t kGameplaySoundCount = 256;
  std::array<std::optional<NovaSoundData>, kGameplaySoundCount>
      gameplay_sounds{};

  // Fire sounds whose weapons actually fired a shot this frame (one entry
  // per primary-bank volley, plus one per NPC ship that fired). The firing
  // routines append a PendingFireSound whenever a round spawns (mirroring the
  // original's volley_fired > 0 gate around NovaAudio_PlaySpatialByDistance);
  // the spaceflight loop, which owns the SdlAudio device, drains and clears
  // it and plays each cached sound with the original's distance attenuation.
  // Keeps SDL out of the pure weapon path.
  struct PendingFireSound {
    std::int16_t slot = -1; // 0..35 fire-sound slot (snd resource id 200+slot)
    float src_x = 0.0F;     // source (firing ship) position, world px
    float src_y = 0.0F;
    // Weapon.flags (flags_primary) bit 0x10: the original counts the sound
    // handle already playing (NovaAudio_CountActiveByHandle) and suppresses a
    // retrigger while it is, so this fire sound never stacks.
    bool suppress_if_active = false;
  };

  std::vector<PendingFireSound> pending_fire_sounds;

  // Impact sounds are queued by the simulation and drained by the flight
  // loop, keeping SDL audio out of collision/effect logic. Slots map to snd
  // resources 300..363 (Ghidra g_impact_sound_handle_table).
  struct PendingImpactSound {
    std::int16_t slot = -1;
    float src_x = 0.0F;
    float src_y = 0.0F;
  };

  std::vector<PendingImpactSound> pending_impact_sounds;

  // snd 372, the dedicated ship-destruction sound (not an impact slot).
  struct PendingDestructionSound {
    float src_x = 0.0F;
    float src_y = 0.0F;
  };

  std::vector<PendingDestructionSound> pending_destruction_sounds;

  // Centered UI cue requests consumed by the spaceflight loop (the loop owns
  // the SDL audio device). Mirrors NovaAudio_QueueCenteredSound with the
  // transition-table handles: the boarding command denial beeps are
  // g_transition_sound_handle_table[3] (snd 153), the "boarded" fanfare is
  // table[4] (snd 154) with the original's repeat count, and the boarding
  // window plays table[2]/table[3] per action. See
  // docs/boarding_plunder_capture.md.
  struct PendingUiSound {
    std::int16_t transition_index = 0; // 0..5 into transition_sounds
    std::int16_t count = 1;            // the original's repeat count
  };

  std::vector<PendingUiSound> pending_ui_sounds;

  // Decoded UI/transition cue cache (snd 150..155). Ghidra
  // g_transition_sound_handle_table (00591560) is preloaded by
  // NovaAudio_PreloadGameplayData (0x004b0740) with
  // NovaSound_LoadDecodedById(0x96 + i) for i in 0..4, i.e. snd 150 + i.
  // The boarding system uses indices 1..4 (snd 151..154); index 5 is spare.
  std::array<std::optional<NovaSoundData>, 6> transition_sounds{};

  // Decoded hyperspace jump sounds (the original preloads them via
  // FUN_004b0740: NovaSound_LoadDecodedById(0x80/0x81/0x82) into the jump
  // handles g_random_encounter_fleet_defs[0].availability_expression
  // +0x8c/+0x90/+0x94).
  // snd 128 Warp up is played during the pre-jump hold; snd 130 Warp out at
  // fire/arrival. Empty means the travel state uses its fallback timing.
  std::optional<NovaSoundData> warp_up_sound;
  std::optional<NovaSoundData> warp_out_sound;
};

} // namespace game
