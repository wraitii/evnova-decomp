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
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "scenario_data.hpp"
#include "sdl_audio.hpp"

namespace game {

// Ghidra g_system_reputation (0x00733bc8): a per-system int16 global that
// tracks the player's standing with each system. Negative values push factions
// hostile; the destination-interaction dialog compares a target stellar's
// reputation_threshold against the containing system's reputation to decide
// whether landing is denied (Stellar_ProcessTravelAndLanding / NovaUi_Run-
// TravelDestinationInteractionWindow). Resized by ScenarioData load to match
// the systems table; indexed by 0-based system resource id.
using SystemReputation = std::vector<std::int16_t>;

// Runtime flags for one accepted mission. This mirrors the 20-byte
// MisnRuntimeFlags record used by the original's 16 active-mission slots.
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
  std::int16_t deadline_year_month = 0;      // +0x06
  std::int16_t deadline_year_month_ext = 0;  // +0x08
  std::int16_t deadline_day = 0;             // +0x0a
  std::int32_t elapsed_travel_days = 0;      // +0x0e
  std::uint16_t elapsed_travel_subday = 0;   // +0x12
};

// Clean-room active mission state. It intentionally names only the fields
// confirmed by the MisnActive type layout (size 0x8e6); raw_payload keeps the
// remaining text/script/runtime bytes available while those semantics are
// reconstructed. One-to-one mission-slot indexing is preserved.
struct ActiveMission {
  // Resolved TravelStel (+0x00, where the mission's visit/pickup happens)
  // and ReturnStel (+0x04, where the mission completes and pays out).
  std::int16_t travel_stellar_id = -1;      // +0x00
  std::int16_t return_stellar_id = -1;      // +0x04
  std::int16_t target_ship_count = 0;       // +0x06
  std::int16_t dude_def_index = -1;         // +0x08
  std::int16_t spawn_behavior = 0;          // +0x0a
  std::int16_t fleet_spawn_goal = 0;        // +0x0c
  std::int16_t special_ship_spawn_mode = 0; // +0x0e
  std::int16_t current_system_id = -1;      // +0x10
  std::int16_t cargo_type_id = -1;          // +0x12
  std::int16_t cargo_qty_tons = 0;          // +0x14
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
  bool has_been_visited = false;            // +0x32
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
};

} // namespace game

namespace game {

// Ghidra 0x004cd3b0 IntroCinematic_SetupFrames fills this (g_intro_cinematic).
// Up to 4 PICT ids (source_pict_ids), each shown for duration_60h_ticks/i
// 1/60s ticks (clamped to [0,300]). When post_intro_dest_id != -1 the intro
// finishes by opening the travel-selection dialog for that destination.
struct IntroCinematicData {
  // PICT resource ids for the sequenced frame art (0xffff terminates the
  // list); 0 (or negative) means "no art this frame".
  std::array<std::int16_t, 4> source_pict_ids{-1, -1, -1, -1};
  // Per-frame display time in 1/60 s ticks, matching the original clamp.
  std::array<std::int16_t, 4> duration_60h_ticks{0, 0, 0, 0};
  std::int16_t post_intro_dest_id = -1;

  // Ghidra IntroCinematic_Run opens the post-intro travel-selection dialog
  // when post_intro_dest_id != -1. The new-game flow
  // (IntroCinematic_SetupFrames default) uses 0x7ffd even with no pilot-save
  // block, which is "no stellar yet" but deliberately not -1 so the dialog
  // still opens.
  [[nodiscard]] bool should_open_post_intro_dialog() const {
    return post_intro_dest_id != -1;
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

  // Clean-room collision envelope used until the original SpriteLayer pixel
  // masks are represented by the simulation. The original derives this from
  // the current ship sprite's half-span; keeping it explicit lets collision
  // tests and future sprite loading replace the provisional 16px default
  // without changing combat code.
  float collision_radius_px = 16.0F;

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
  float shield_points = 0.0F;       // +0x54
  float armor_points = 0.0F;        // +0x58
  float ionization_points = 0.0F;   // +0x5C (ionization charge meter)
  float fuel_points = 0.0F;         // +0x38
  float death_timer_active = -1.0F; // +0x3C
  // Port-side one-shot latch for the first destruction-presentation slice.
  // The original drives this through Ship_HandleShip's fading-effect pool
  // (0x00428090 / 0x0043b170); the exact class-specific debris sprite is not
  // represented yet, so the shared impact animation is queued once instead.
  bool destruction_visual_triggered = false;
  // Briefly keeps the destroyed hull as a dim wreck while its burst plays.
  float destruction_visual_timer_ms = 0.0F;
  bool destruction_finale_triggered = false;
  // Shot_ResolveShipHitFromWeapon refreshes this on non-bypass impacts. The
  // timer consumer is still deferred, so the field remains provisional.
  float hit_reaction_timer = 0.0F;
  // Ghidra ShipState field_0xB0. Weapon on-hit ionization colors are ORed
  // here by Weapon_ApplyWeaponOnHitEffects; the status renderer is deferred.
  std::uint32_t ionization_color = 0;

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
  // Travel-target transfer latch (Ghidra ShipState +0x2Aish): the AI sets this
  // to 2 when it assigns ai_secondary_target_slot a fresh travel stellar (the
  // signal System_UpdateSystemAndStellarDisplayState reads to auto-target the
  // nearest travel point on the arrival/last-known system).
  std::int16_t travel_transfer_mode = 0;      // +0x2A (provisional offset)
  std::int16_t primary_target_ship_slot = -1; // +0x70
  std::int16_t ai_secondary_target_slot =
      -1;                                // +0x6C (also a travel/stellar slot)
  std::int16_t ai_target_ship_slot = -1; // +0x9A
  std::int16_t target_stellar_object_id = -1; // +0x8C
  // Escort command selected by the assist supervisor. The clean-room dialog
  // and mission models only use the neutral default so far, but the field is
  // needed to preserve the state-0x05+ branch shape.
  std::int16_t escort_command_code = 0; // +0xC90A (provisional)
  // Formation lead whose engine-glow/formation-offset this ship mirrors in the
  // escort control modes (ShipState +0xC906 formation_leader_ship_slot). <1
  // means "no leader": mode 0x12 (chase leader) falls back to idle control
  // when it is empty.
  std::int16_t formation_leader_ship_slot = -1; // +0xC906
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
  // Ghidra ShipState +0xC92E: the AI's resolved-target slot, cleared by
  // Ship_ResetShipAiBehaviorRuntimeFields (0x00402810).
  std::int16_t resolved_ai_target_ship_slot = -1; // +0xC92E
  // Stored evasive heading for control mode 0x10 (Ghidra ShipState raw short
  // at +0x8E, between target_stellar_object_id and jump_destination_stellar_id;
  // unnamed in the DB). Ship_ApplyShipAiControls writes current-heading +/-135
  // deg (instance-id parity sign) when it orders the evasive-break, and mode
  // 0x10 steers at this value until it aligns and drops back to mode 0x6.
  std::int16_t ai_evasive_heading_deg = 0; // +0x8E (Provisional)
  // Ghidra ShipState raw short at +0x90 (unnamed): the travel target cache
  // Ship_ResetShipAiBehaviorRuntimeFields (0x00402810) clears to -1.
  std::int16_t travel_target_cache = -1;         // +0x90 (Provisional)
  std::int16_t jump_destination_stellar_id = -1; // +0x92
  std::int16_t ai_hostility_accumulator = 0;     // +0x96
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
  // Waypoint arrival marker pair used by ships carrying arrival markers (class
  // sprite_behavior_flags bit 1): waypoint_arrival_marker_a reflects a
  // completed arrival; marker_b counts/suppresses route restarts. -1 = none.
  // Provisional: Ship_UpdateVisualState also reuses these two shorts as the
  // control latch and cycle index for that same sprite-behavior animation.
  std::int16_t waypoint_arrival_marker_a = -1; // +0xC8FA (provisional offset)
  std::int16_t waypoint_arrival_marker_b = -1; // +0xC8FC (provisional offset)

  // Whether this ship is flagged as carrying a mining scoop outfit (ShipState
  // +0xBE mining_scoop_active, derived by Outfit_HasMiningScoopOutfit).
  bool mining_scoop_active = false;

  // --- Ship-comm dialog latches (Provisional; see ship_comm_dialog.cpp) -----
  // The comm window (NovaUi_RunTargetShipCommWindow 0x0047e470) reads/writes
  // three unnamed ShipState bytes. +0xB9 is set when a hailed escort is
  // re-hired (escort_rehired_mark), +0xBB gates the dialog's status label
  // (0 = "Fighter" STR# 0x7d2 0xa8, else "Captured Escort" 0xa6:
  // escort_origin_mark), and +0xBC is set after any ship-comm dialog closes
  // (comm_interacted_mark). Names are conservative/Provisional pending the
  // wider escort/fleet system.
  std::int8_t escort_rehired_mark = 0;  // +0xB9 (Provisional)
  std::int8_t escort_origin_mark = 0;   // +0xBB (Provisional)
  std::int8_t comm_interacted_mark = 0; // +0xBC (Provisional)
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
  // being non-zero). Written by NovaPlayer_UpdateFromInput and read by the
  // flight render to drive the engine-glow layer.
  bool engine_thrust = false;
  // ShipState +0xc8d4. The player-control path raises this one unit/frame
  // while thrusting and lowers it one unit/frame otherwise; normal thrust caps
  // at 24, while the afterburner path can reach 32. The renderer maps the
  // original integer control level to alpha because the original SpriteWorld
  // blend setup is not reconstructed yet.
  std::int16_t engine_glow_level = 0;
  // Derived render value, always engine_glow_level / 24 clamped to [0,1].
  float engine_glow_intensity = 0.0F;

  // Weapon-exit (muzzle) geometry for the player's ship, decoded from the
  // ship's sh\x8an descriptor (see ShipVisualDescriptor.turret_muzzles) when
  // the ship sprite is ensured. Shots spawn at the barrel of their weapon's
  // turret group rather than the ship centre (Ghidra Weapon_ApplyTurretSpread-
  // Velocity 0x0046c5c0 via Weapon_SelectTurretQuadrant 0x0046c320), which is
  // what makes the Light Blaster (and other guns) visibly fire from the nose.
  // Indexed [turret_group][quadrant], 4 groups x 4 quadrant barrels.
  std::array<std::array<std::int16_t, 4>, 4> muzzle_lateral{};
  std::array<std::array<std::int16_t, 4>, 4> muzzle_forward{};
  std::array<std::array<std::int16_t, 4>, 4> muzzle_drop{};
  // Per-axis weapon-exit compress scale (field_0xaa4/0xaa8, x 0.01).
  float muzzle_scale_x = 0.0F;
  float muzzle_scale_y = 0.0F;
  // Whether muzzle geometry has been loaded for the current ship class. Cleared
  // and repopulated each time the ship class changes / the sprite is ensured.
  bool muzzle_ready = false;
  // Per-turret-group quadrant rotation state (next barrel to fire), -1 until
  // the first shot chooses a random quadrant (Weapon_SelectTurretQuadrant).
  std::array<std::int8_t, 4> muzzle_quadrant{-1, -1, -1, -1};
};

// Historical name for `Ship`; kept so existing player-ship helpers compile
// unchanged after the rename. New code should use `Ship` directly.
using PlayerShip = Ship;

// The new pilot's identity (first/last name) and start configuration, filled
// by the pilot-naming dialog in the new-game flow. Ghidra keeps these in
// DAT_007d20b7 (first) / DAT_007d21b7 (last) and DAT_007d22b7 (start type).
struct PilotData {
  std::string first_name;
  std::string last_name;
  std::int16_t start_type_code = 0;     // PilotData_ResolveStartType result
  std::int16_t selected_reputation = 0; // pilot-selection-dialog choice
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
  std::bitset<0x100> active_ranks;
  bool registered = true;
  bool male = true;

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
  // The stellar resource id currently auto-targeted for travel/landing (the
  // nearest playable stellar in the current system), mirroring the original's
  // auto-set ai_secondary_target_slot / travel_transfer_mode == 2. -1 when no
  // stellar qualifies. Set each frame by NovaTargeting_UpdatePlayerTarget.
  std::int16_t selected_stellar_id = -1;
  // Once the player cycles targets, retain that choice while it remains a
  // valid stellar in the current system. Otherwise UpdatePlayerTarget seeds
  // this field from the nearest eligible stellar, matching entry behavior.
  bool selected_stellar_is_manual = false;
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
  // Brake, align, warm up, then zoom to arrival.
  enum class JumpPhase { kIdle, kBrake, kHold, kWarmup, kZoom };
  JumpPhase jump_phase = JumpPhase::kIdle;
  // Whether Warp up has been started for this jump.
  bool warp_up_started = false;
  // Jump-phase accumulators (ms): frame_time accumulates each tick so the
  // phase timing is frame-rate independent and unit-testable. hold_elapsed_ms
  // drives the stationary-alignment hold; zoom_elapsed_ms drives the
  // acceleration-zoom tunnel (approx. of 350/jump_duration_multiplier; the
  // class multiplier is not yet decoded -- TODO(decomp)).
  float hold_elapsed_ms = 0.0F;
  float zoom_elapsed_ms = 0.0F;
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
  // and pixel-mask test. Set by the common spawn path.
  float collision_radius_px = 2.0F;
  // Set by collision or expiry; the simulation removes these after the pass.
  bool consumed = false;
  // Ghidra ShotState.fuse_elapsed. Proximity-fuse behavior is deferred, but
  // the accumulator belongs on the shot record so adding it will not require
  // another storage migration.
  float fuse_elapsed = 0.0F;
  // Ghidra ShotState.impact_variant, used later by impact visual/effect code.
  std::int8_t impact_variant = 0;
  // Ghidra ShotState +0x1e: optional impact-package row selected by the
  // chained-impact resolver. -1 means this round has no package dispatch.
  std::int16_t impact_package_id = -1;
  // Time-animated shot-frame stepping (Ghidra ShotState.frame_cycle_index / +
  // anim_elapsed). For a weapon whose flags_primary bit 0 is SET the shot uses
  // Shot_HandleShot's animated branch: anim_elapsed accumulates frame time and
  // each time it crosses the weapon's shot_anim_frame_dwell, frame_cycle_index
  // advances (wrapping at the frame count). Only the animated-branch shot
  // sets need these; static/heading sets (Light Blaster) leave them unused.
  int frame_cycle_index = 0; // ShotState.frame_cycle_index (+0x3e)
  float anim_elapsed = 0.0F; // ShotState.anim_elapsed, in ms
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
  std::int16_t animation_counter = 0;
  std::int16_t weapon_id = -1;
  std::int16_t owner_ship_slot = -1;
  std::int16_t target_ship_slot = -1;
  std::int16_t turret_quadrant = -1;
  std::int16_t turret_group_id = -1;
  std::int16_t forced_targeting = -1;
  std::int8_t impact_variant = 0;
  // Optional impact-package row for the queued beam's terminal hit.
  std::int16_t impact_package_id = -1;
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
  // Sprite handle / state_code for this record; the drift renderer assigns a
  // sprite set by wander_type and ticks/arm its frame counter from +0x54.
  std::int32_t state_code = 0; // +0x00
  float target_pos_x = 0.0F;   // +0x04
  float target_pos_y = 0.0F;   // +0x08
  float target_vel_x = 0.0F;   // +0x0c
  float target_vel_y = 0.0F;   // +0x10
  // Wander phase / lifetime accumulator. Spawned as a random value in
  // [0, pertype lifetime) (Asteroid_SpawnRecord) and advanced by wander_speed
  // each tick, wrapping via the sprite descriptor's frame count
  // (Asteroid_UpdateSprites).
  float wander_radius = 0.0F;          // +0x14
  float wander_speed = 0.0F;           // +0x18
  std::int16_t wander_table_value = 0; // +0x1c
  std::int16_t wander_type = 0; // +0x1e (index into the asteroid-type table)
  bool active = false;          // +0x20

  // Pool size for the 16-slot AsteroidState table.
  static constexpr std::size_t kPoolSize = 16;
};

// Everything about the running pilot's world. Replaces the Game_Reset* set of
// globals for the transient not-yet-reconstructed subsystems with explicit
// flags so we can log exactly what is and is not preserved.
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

  // Seeded PRNG backing the new-game flow's random opener strings and start
  // selection. The original uses a global NovaRandom; this is kept local to
  // the state so runs are reproducible when seeded identically.
  std::mt19937 rng{42};
  // Ghidra g_player_combat_rating_points: aggregate combat-rating score the
  // afterburner eligibility roll (Ship_CanShipUseAfterburner 0x0046b260)
  // divides by the ship class's Strength. Producers (kill/rating accrual)
  // are not yet reconstructed, so it stays 0 (TODO(decomp)).
  std::int32_t player_combat_rating_points = 0;
  bool game_active = false;  // Ghidra DAT_00596d28
  bool intro_played = false; // Ghidra DAT_00596d35: cleared on new pilot so
                             // the intro cinematic plays on first flight.
  PilotData pilot;
  PilotControlState control;
  TravelState travel;
  IntroCinematicData intro_cinematic;
  // The transient HUD overlay message (see HudOverlayState). Kept on GameState
  // per AGENTS.md (no hidden globals) and rendered by the HudRenderer.
  HudOverlayState hud_overlay;

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
  // NovaEffects_QueueCenteredResource at jump engage -- the 'boom' white
  // frame). Set to 1.0 when the boom fires at the jump's arrival, then decayed
  // by the spaceflight loop; the in-game frame draw overlays a white fullscreen
  // rect with this alpha. 0 when no flash is active.
  float screen_flash_intensity = 0.0F;

  // Parsed scenario data (ships/outfits/weapons/stellars/systems), loaded once
  // so the gameplay loops can look up classes by id. Empty until a game is
  // created (mirrors the original lazily loading scenario tables in
  // NovaData_LoadScenarioResourceTables on the new-game path).
  ScenarioData scenario;

  // Mission runtime tables. The original stores 16 accepted missions and
  // their 20-byte runtime flag records in separate globals; keeping the same
  // slot count makes later activation/resolve work one-to-one with Ghidra.
  static constexpr std::size_t kMaxActiveMissions = 16;
  std::array<ActiveMission, kMaxActiveMissions> active_missions{};
  std::array<MissionRuntimeFlags, kMaxActiveMissions>
      active_mission_runtime_flags{};
  std::array<MissionTargetResolution, 1000> mission_target_resolutions{};
  // Mission/system cue bytes are persisted in FleetState at 0x5dde. The
  // exact cue meanings remain provisional, but the table shape is known.
  std::array<std::uint16_t, 0x80> system_cues{};

  // Mission-script side effects that need to be consumed by UI/audio layers.
  // They are explicit latches rather than hidden globals, matching the
  // clean-room state ownership rule.
  std::vector<std::int16_t> pending_script_sounds;
  std::int16_t pending_script_message_string_list = -1;
  bool script_forced_leave_landing = false;

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

  // Lightweight ground-truth of fired shots (projectiles / beams) in flight,
  // reconstructed for the player's primary weapon. The original keeps these in
  // the ShotState swath (g_shot_states) with full sprite/guidance/collision
  // bookkeeping (Shot_SpawnShotFromWeapon 0x0041fd30); this clean-room model
  // carries only the fields the firing + flight renderer need so far, and the
  // projectile is drawn via its weapon's shot sprite set (spin id + 3000),
  // oriented by its velocity as Shot_HandleShot picks the heading frame.
  // Each entry is one fired round at a given world position/velocity.
  std::vector<ActiveShot> active_shots;
  std::array<BeamHit, 0x40> beam_hit_queue{};
  std::array<ImpactEffectInstance, 0x20> impact_effect_instances{};
  std::array<FadingEffectInstance, 0x20> fading_effect_instances{};

  // The 16-slot asteroid / drift-debris pool (mirrors the original
  // `g_asteroid_states`). Shared by Asteroid_SpawnRecord (spawn), the future
  // Asteroid_InitSystem / Asteroid_Spawn (Steps 3/4) and the step 5 per-tick
  // drift. The records are ASTEROID / drift-debris chars (r\xf6id family), not
  // NPC ships. Slots are found by scanning for `active == false`.
  std::array<AsteroidState, AsteroidState::kPoolSize> asteroid_pool{};

  // "no asteroids" latch set by Asteroid_InitSystem (0x004216B0) when the
  // current system declares asteroid_count < 1. The original writes a 1 byte
  // into the random-encounter fleet-def scratch area
  // (g_random_encounter_fleet_defs[0x4d].availability_expression[0x94]);
  // the clean-room stores it here since that scratch buffer is not modelled.
  bool no_asteroids_latch = false;
  // One-shot audio requests consumed by the spaceflight loop.
  bool warp_up_sound_pending = false;
  bool warp_out_sound_pending = false;

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
  // the SDL audio device). Mirrors NovaEffects_QueueCenteredResource with the
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
  // LoadStringResourceCopyById(0x96 + i) for i in 0..4, i.e. snd 150 + i.
  // The boarding system uses indices 1..4 (snd 151..154); index 5 is spare.
  std::array<std::optional<NovaSoundData>, 6> transition_sounds{};

  // Decoded hyperspace jump sounds (the original preloads them via
  // FUN_004b0740: LoadStringResourceCopyById(0x80/0x81/0x82) into the jump
  // handles g_random_encounter_fleet_defs[0].availability_expression
  // +0x8c/+0x90/+0x94).
  // snd 128 Warp up is played during the pre-jump hold; snd 130 Warp out at
  // fire/arrival. Empty means the travel state uses its fallback timing.
  std::optional<NovaSoundData> warp_up_sound;
  std::optional<NovaSoundData> warp_out_sound;
};

} // namespace game
