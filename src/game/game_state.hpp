#pragma once

// Clean-room model of the persistent per-pilot game state. The original keeps
// this in a swath of globals (g_ship_states / ShipState, g_system_defs /
// SystemDef, g_outfit_owned_count, g_pilot_data / DAT_007d2290,
// g_intro_cinematic / IntroCinematicData). Here it is grouped into one value
// object owned by NovaRuntime so the reimplementation stays free of hidden
// globals and singletons (AGENTS.md). Each field that maps to a Ghidra
// global/struct names the address in a comment.

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "scenario_data.hpp"

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

// Ghidra 0x004b3350 Ship_ResetPlayerShipState resets g_ship_states. The
// reimplementation tracks only the fields meaningfully needed for the
// new-pilot flow and early game loop so far; deep AI/combat fields are
// present and reset but unused until spaceflight is reconstructed.
struct PlayerShip {
  float pos_x = 0.0F;
  float pos_y = 0.0F;
  float vel_x = 0.0F;
  float vel_y = 0.0F;
  float heading = 0.0F; // radians
  float speed = 0.0F;
  float shield_points = 0.0F;         // g_ship_states->shield_points
  float armor_points = 0.0F;          // g_ship_states->armor_points
  float fuel_points = 0.0F;           // g_ship_states->fuel_points
  float death_timer_active = -1.0F;   // g_ship_states->death_timer_active
  std::int16_t ship_class_id = 0;     // g_ship_states->ship_class_id
  std::int16_t current_system_id = 0; // g_ship_states->current_system_id
  std::int16_t active_weapon_bank_slot = 0;
  std::int16_t timed_action_counter = -1; // g_ship_states->timed_action_counter
  std::int32_t credits = 0;               // g_ship_states->credits
  bool is_active = false;                 // g_ship_states->is_active
  // Engine-thrust latch: whether the player is currently applying forward
  // thrust this frame (mirrors Ghidra ShipState.ai_forward_thrust_cmd at +0x30
  // being non-zero). Written by NovaPlayer_UpdateFromInput and read by the
  // flight render to drive the engine-glow layer.
  bool engine_thrust = false;
  // Engine-glow intensity, 0..1, ramped toward the target by the movement
  // update (rise while thrusting, decay when not). Clean-room approximation of
  // the original dimming the glow sprite with throttle; on the live movement
  // sim this is binary thrust, so it is a soft fade rather than a per-degree
  // throttle fade (TODO(decomp)).
  float engine_glow_intensity = 0.0F;
};

// The new pilot's identity (first/last name) and start configuration, filled
// by the pilot-naming dialog in the new-game flow. Ghidra keeps these in
// DAT_007d20b7 (first) / DAT_007d21b7 (last) and DAT_007d22b7 (start type).
struct PilotData {
  std::string first_name;
  std::string last_name;
  std::int16_t start_type_code = 0;     // PilotData_ResolveStartType result
  std::int16_t selected_reputation = 0; // pilot-selection-dialog choice
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
  // Whether the selected stellar is a landing target (travel_flags & 0x2) as
  // opposed to a jump/hypergate point; drives the HUD label and the landing
  // interaction. Derived from the selected stellar, not stored separately.
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
};

// Everything about the running pilot's world. Replaces the Game_Reset* set of
// globals for the transient not-yet-reconstructed subsystems with explicit
// flags so we can log exactly what is and is not preserved.
struct GameState {
  // Seeded PRNG backing the new-game flow's random opener strings and start
  // selection. The original uses a global NovaRandom; this is kept local to
  // the state so runs are reproducible when seeded identically.
  std::mt19937 rng{42};

  bool game_active = false;  // Ghidra DAT_00596d28
  bool intro_played = false; // Ghidra DAT_00596d35: cleared on new pilot so
                             // the intro cinematic plays on first flight.
  PilotData pilot;
  PlayerShip player;
  TravelState travel;
  IntroCinematicData intro_cinematic;

  // Parsed scenario data (ships/outfits/weapons/stellars/systems), loaded once
  // so the gameplay loops can look up classes by id. Empty until a game is
  // created (mirrors the original lazily loading scenario tables in
  // NovaData_LoadScenarioResourceTables on the new-game path).
  ScenarioData scenario;

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
};

} // namespace game
