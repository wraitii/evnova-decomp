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
// now (movement, targeting, and later the spawn + AI systems). Only the fields
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

  // --- AI movement state (drives Ship_HandleShip 0x00433050's integrator) ---
  // Whether the ship applies forward thrust this frame. Mirror GHIDRA
  // ShipState.ai_forward_thrust_cmd (+0x30). For the player it is mirrored by
  // `engine_thrust` below for the render glow; the AI writes the same field
  // via the reaction/offence helpers.
  float ai_forward_thrust_cmd = 0.0F; // +0x30
  // Desired scalar speed along the heading: >0 means forward thrust, <=0 means
  // the absolute-set reverse path (vel set to heading*abs(desired)).
  float ai_desired_speed = 0.0F; // +0x34
  // Desired heading in the game's integer-degrees convention (heading 0 = up,
  // increasing clockwise). Ship_HandleShip turns the ship toward this at
  // Ship_ComputeShipMaxTurnRateDeg deg/frame.
  std::int16_t ai_desired_heading_deg = 0; // +0x68
  // Coast-through-reversal TIMER (NOT a brake): while >0 it suppresses both the
  // turn-to-heading and forward-thrust blocks so the ship holds heading and
  // coasts. Set to random 30..60 when the AI decides to reverse, counts down by
  // frame time each frame.
  float reverse_speed_bias = 0.0F; // +0x4C
  // Station-hold timer driving the hold/approach state (ai_station_hold_timer).
  float ai_station_hold_timer = 0.0F; // +0x50
  // Wall-clock (SDL ticks) the current AI mode began; used by the jump-sequence
  // and formation positioning timing.
  std::uint32_t ai_mode_start_time_ms = 0; // +0xA4
  // AI turn-bias direction (-1/0/+1) used by ships that bank/lean into turns.
  std::int16_t ai_turn_bias_dir = 0; // +0xC8F8
  // Latch requesting this ship to fire its active weapon bank.
  std::int8_t ai_fire_trigger_latch = 0; // +0xBA

  // --- Vital stats ---
  float shield_points = 0.0F;       // +0x54
  float armor_points = 0.0F;        // +0x58
  float fuel_points = 0.0F;         // +0x38
  float death_timer_active = -1.0F; // +0x3C

  // --- Identity / placement ---
  std::int16_t ship_class_id = 0;             // +0x76 (zero-based ship index)
  std::int16_t ship_instance_id = 0;          // +0x86 (0 = player)
  std::int16_t current_system_id = 0;         // +0x74
  std::int16_t faction_or_government_id = -1; // +0x98
  std::int16_t dude_class_id = -1;            // +0x78
  std::string ship_name;

  // --- Weapon bank / active selection ---
  std::int16_t active_weapon_bank_slot = 0; // +0x72
  // Ghidra keeps the 6 weapon-bank ammo counters at +0xC8 (a 100-stride row) on
  // the ShipState. The reimplementation instead tracks player ammo centrally in
  // GameState.weapon_bank_ammo/key secondary (weapon.cpp). The per-bank
  // cooldown lives in GameState.weapon_bank_cooldown. Added here only when the
  // NPC weapon path is reconstructed (Ship_AllocateShipSlotInSystem seeds these
  // from the ShipClassDef loadout).

  // --- Mission / target/AI slots (added to unblock spawn/targeting) ---
  std::int16_t mission_owner_slot = -1; // +0x8A
  std::int16_t mission_ship_slot = -1;  // +0xC8CE (mission-ship slot link)
  std::int16_t mission_fleet_slot = -1; // +0xC8D2
  std::int16_t ai_behavior_code = 0;    // +0x88
  std::int16_t ai_state_code = 0;       // +0xC8C8
  std::int16_t ai_control_mode = 0;     // +0xC8CA
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
  // Velocity-match lock (ShipState +0xC8DC): the slot of a ship whose
  // velocity/heading this ship is matching (control mode 0xc/0xf), or -1. A
  // non-self value gates the NPC effective-stats branch
  // (Ship_ComputeShipEffectiveThrust 0x004640a0 applies the DAT_00575788
  // factor while locked) and blocks Stellar_CanShipInitiateJumpSequence; the
  // velocity-match control modes are Phase 5/8 TODO(decomp), but the field is
  // cleared by Ship_DeactivateVacantShipsAndTally (0x0041ad50) and seeded by
  // Ship_AllocateShipSlotInSystem, so it lives on the struct now.
  std::int16_t velocity_match_target_ship_slot = -1; // +0xC8DC
  std::int16_t jump_destination_stellar_id = -1;     // +0x92
  std::int16_t ai_hostility_accumulator = 0;         // +0x96
  // Disable/surrender-pressure patience timer: counts down (by frame time)
  // while the ship fails to gain disable pressure over its target; when it runs
  // out the AI gives up and clears the primary target (Ship_UpdateShipAiState
  // state 4). -1 = no patience pressure tracked (set by
  // Ship_CanShipApplyDisablePressure- ToTarget 0x00464a90 on each successful
  // check).
  float target_disable_patience_timer = -1.0F; // +0xC90C
  // Disable-threshold progress (Ghidra ShipState +0x64, provisional): grows
  // while the ship takes disable pressure. Ship_CheckShipDisableThresholdState
  // (0x0046c7a0) treats progress > 16.0 (or > 24.0 with disable_state_latch
  // >= 0, > 8.0 with latch < 0) as "at/below the disable threshold", which
  // makes the ship un-targetable without a cloak-scanner outfit. The disable/
  // status-effect subsystem is not reconstructed yet, so this stays 0 and the
  // gate passes (ships are cycleable).
  float disable_threshold_progress = 0.0F; // +0x64
  // Disable-state latch (Ghidra ShipState +0xC8D8, short): sign selects the
  // disable-threshold boundary (24.0 vs 8.0) in Ship_CheckShipDisableThreshold-
  // State. Written by the disable subsystem (Phase 5); 0 by default.
  std::int16_t disable_state_latch = 0; // +0xC8D8
  // Waypoint arrival marker pair used by ships carrying arrival markers (class
  // sprite_behavior_flags bit 1): waypoint_arrival_marker_a reflects a
  // completed arrival; marker_b counts/suppresses route restarts. -1 = none.
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
  // (written by the ship-comm escort release and the disable subsystem).
  std::int16_t post_hit_mode_hint = -1; // +0xC8DE

  // --- Misc ---
  std::int16_t timed_action_counter = -1; // +0xC908
  std::int32_t credits = 0;               // +0xA0
  bool is_active = false;                 // +0xB8

  // --- Player-only render/input extras (kept on Ship for simplicity; the
  // original stores the engine-glow level at ShipState +0xc8d4 and per-ship
  // muzzle geometry derived from the sh\x8an descriptor). ---
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
  static constexpr std::size_t kControlBitCount = 65536;
  std::bitset<kControlBitCount> bits;
  std::bitset<0x800> explored_systems;
  bool registered = true;
  bool male = true;

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
  // Jump-sequence countdown in 1/60s tick intervals (the original's
  // Stellar_GetJumpSequenceDurationMs / ai_station_hold_timer gate). During
  // this phase the ship coasts and the sequence plays; at zero the jump
  // completes and the system changes.
  int jump_countdown_ticks = 0;
  // Whether the engaged jump has finished declaring a destination and is now
  // in the transition. The original jumps directly into the hyperspace flight;
  // we keep a boolean so the loop knows to hand off to the completion path.
  bool engaging = false;
  // Whether a jump completed this frame (consumed by the spaceflight loop to
  // re-spawn the starfield once). Cleared each tick.
  bool just_completed = false;
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
  float pos_x = 0.0F; // world x
  float pos_y = 0.0F; // world y
  float vel_x = 0.0F; // px/frame
  float vel_y = 0.0F;
  // Remaining lifetime in reference-cadence frames (WeaponDef Count).
  int life_frames = 0;
  // Time-animated shot-frame stepping (Ghidra ShotState.frame_cycle_index / +
  // anim_elapsed). For a weapon whose flags_primary bit 0 is SET the shot uses
  // Shot_HandleShot's animated branch: anim_elapsed accumulates frame time and
  // each time it crosses the weapon's shot_anim_frame_dwell, frame_cycle_index
  // advances (wrapping at the frame count). Only the animated-branch shot
  // sets need these; static/heading sets (Light Blaster) leave them unused.
  int frame_cycle_index = 0; // ShotState.frame_cycle_index (+0x3e)
  float anim_elapsed = 0.0F; // ShotState.anim_elapsed, in ms
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

  // Parsed scenario data (ships/outfits/weapons/stellars/systems), loaded once
  // so the gameplay loops can look up classes by id. Empty until a game is
  // created (mirrors the original lazily loading scenario tables in
  // NovaData_LoadScenarioResourceTables on the new-game path).
  ScenarioData scenario;

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

  // Decoded player weapon fire sounds, keyed by the weapon's `fire_sound`
  // slot. Ghidra Weapon_FirePlayerWeaponBank resolves the weapon's
  // fire_sound_slot through the preloaded g_gameplay_sound_handle_table and
  // plays it via NovaAudio_PlaySpatialByDistance once a shot actually spawns;
  // this clean-room cache mirrors that table (index = slot, -1 slot = none is
  // left empty). A slot maps to the snd resource id 200 + slot (verified: the
  // Light Blaster's slot 8 is "Light Blaster.sfil" id 208).
  std::array<std::optional<NovaSoundData>, 36> weapon_fire_sounds{};

  // Fire-sound slots whose weapons actually fired a shot this frame (one
  // entry per primary-bank volley). The firing routine appends a weapon's
  // fire_sound slot whenever a round is spawned (mirroring
  // Weapon_-FirePlayerWeaponBank playing the fire sound after volley_fired > 0
  // only); the spaceflight loop, which owns the SdlAudio device, drains and
  // clears it and plays each cached slot. Keeps SDL out of the pure weapon
  // path.
  std::vector<std::int16_t> pending_fire_sound_slots;
};

} // namespace game
