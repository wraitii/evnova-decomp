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
#include <random>
#include <optional>
#include <string>

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
  // when post_intro_dest_id != -1. The new-game flow (IntroCinematic_SetupFrames
  // default) uses 0x7ffd even with no pilot-save block, which is "no stellar
  // yet" but deliberately not -1 so the dialog still opens.
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
  float heading = 0.0F;      // radians
  float speed = 0.0F;
  float shield_points = 0.0F;   // g_ship_states->shield_points
  float armor_points = 0.0F;    // g_ship_states->armor_points
  float fuel_points = 0.0F;     // g_ship_states->fuel_points
  float death_timer_active = -1.0F; // g_ship_states->death_timer_active
  std::int16_t ship_class_id = 0;   // g_ship_states->ship_class_id
  std::int16_t current_system_id = 0; // g_ship_states->current_system_id
  std::int16_t active_weapon_bank_slot = 0;
  std::int16_t timed_action_counter = -1; // g_ship_states->timed_action_counter
  std::int32_t credits = 0;             // g_ship_states->credits
  bool is_active = false;               // g_ship_states->is_active
};

// The new pilot's identity (first/last name) and start configuration, filled
// by the pilot-naming dialog in the new-game flow. Ghidra keeps these in
// DAT_007d20b7 (first) / DAT_007d21b7 (last) and DAT_007d22b7 (start type).
struct PilotData {
  std::string first_name;
  std::string last_name;
  std::int16_t start_type_code = 0; // PilotData_ResolveStartType result
  std::int16_t selected_reputation = 0; // pilot-selection-dialog choice
};

// Travel-selection / post-intro destination plumbing. Ghidra:
// Stellar_FindNearestAvailableTravelStellar + Stellar_SetTravelDestination.
// The new-game flow picks the first available adjacent system as the first
// jump target after the intro.
struct TravelState {
  std::int16_t selected_dest_id = -1; // g_stellar_selected, opens as dialog
  // post_intro_dest_id is hoisted into IntroCinematicData.
};

// Everything about the running pilot's world. Replaces the Game_Reset* set of
// globals for the transient not-yet-reconstructed subsystems with explicit
// flags so we can log exactly what is and is not preserved.
struct GameState {
  // Seeded PRNG backing the new-game flow's random opener strings and start
  // selection. The original uses a global NovaRandom; this is kept local to
  // the state so runs are reproducible when seeded identically.
  std::mt19937 rng{42};

  bool game_active = false; // Ghidra DAT_00596d28
  bool intro_played = false; // Ghidra DAT_00596d35: cleared on new pilot so
                             // the intro cinematic plays on first flight.
  PilotData pilot;
  PlayerShip player;
  TravelState travel;
  IntroCinematicData intro_cinematic;

  // Outfit/weapon ownership counts indexed by outfit id. The new-game flow
  // zeroes all and then seeds them from the starting ship class's default
  // outfit list. Ghidra g_outfit_owned_count (0x200 entries).
  // NOTE(decomp): outfit/weapon class tables are not reconstructed yet, so
  // this array is present but not populated; see new_pilot_flow.cpp.
  std::array<std::int16_t, 0x200> outfit_owned_count{};
  // Per-weapon ammo/secondary counters (Ghidra g_ship_states
  // weapon_bank_ammo_0 / weapon_bank_secondary_counter_0). The original
  // walks these as two-dimensional [bank][weapon] tables with a 0x100 stride;
  // that inner layout is not reconstructed, so a flat placeholder bank array is
  // carried and left zero by the new-game inventory stub.
  std::array<std::int16_t, 0x100> weapon_bank_ammo{};
  std::array<std::int16_t, 0x100> weapon_bank_secondary{};
};

} // namespace game
