#pragma once

// Clean-room model of the per-pilot save *record*. This is NOT a living
// gameplay object: the original keeps the running game in globals
// (g_ship_states / ShipState, g_outfit_owned_count, g_system_defs, ...) and
// only materializes a pilot file as a serialized snapshot on demand --
// (a) seeded transiently during the new-game flow
//   (PilotData_InitializePlayerState 0x004cd4b0 creates/grows the block,
//    IntroCinematic_SetupFrames 0x004cd3b0 reads the intro frames from it),
// (b) read back into the globals when continuing a pilot
//   (PilotFile_LoadSave 0x004cb260 copies the .plt block into the globals).
//
// In the original the on-disk .plt is a large binary block (offsets up to
// 0xe94e) holding ship state, outfit/weapon tables, system discovery, per-govt
// reputations, missions/FleetState, dates, etc. The vast majority of those
// fields are not reconstructed yet (the live GameState does not track them).
// So this build carries a focused PilotFile covering exactly the fields the
// current GameState tracks, each mapped to its Ghidra .plt block offset when
// known. The remaining original fields are absent until their subsystems are
// reconstructed; this record is over the tracked subset only. The pilot file
// is never serialized in this build (no .plt writer): it is the in-memory
// record the new-game flow seeds and applies to GameState.

#include "game_state.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace game {

// Mirrors the persistent per-pilot save record. Fields not yet meaningful to
// GameState (faction reputations, system discovery, missions/FleetState,
// dates, per-stellar availability) are deliberately not present; see above.
struct PilotFile {
  // The pilot's identity/callsign. On disk the file is <nova_files><name>.plt;
  // in-memory the original keys the pilot-save registry block by this name
  // (ResourceData_AccessByKey 0x63688a72; block+0x32 carries the name/opener).
  std::string pilot_name;

  // -- Player ship core (Ghidra .plt offsets from PilotFile_LoadSave) --------
  std::int32_t credits = 0;       // block+0x281a
  std::int16_t ship_class_id = 0; // block+0x02 (0 = default class)
  std::int16_t current_system_id = 0;
  std::int16_t active_weapon_bank_slot = 0;
  std::int16_t timed_action_counter = -1;
  float death_timer_active = -1.0F;
  float shield_points = 0.0F; // block+0x04 (g_ship_states->shield_points)
  float armor_points = 0.0F;  // block+0x06
  float fuel_points = 0.0F;   // block+0x08
  float pos_x = 0.0F;
  float pos_y = 0.0F;
  float vel_x = 0.0F;
  float vel_y = 0.0F;
  float heading = 0.0F; // radians
  float speed = 0.0F;

  // -- Intro cinematic (IntroCinematic_SetupFrames reads block+0x20/0x28/0x30).
  std::array<std::int16_t, 4> intro_source_pict_ids{-1, -1, -1, -1};
  std::array<std::int16_t, 4> intro_duration_60h_ticks{0, 0, 0, 0};
  std::int16_t post_intro_dest_id = -1;

  // -- Ownership tables (g_outfit_owned_count, weapon banks) -----------------
  std::array<std::int16_t, 0x200> outfit_owned_count{};
  std::array<std::int16_t, 0x100> weapon_bank_ammo{};
  std::array<std::int16_t, 0x100> weapon_bank_secondary{};

  // Fresh default for a brand-new pilot, mirroring
  // PilotData_InitializePlayerState's absent-block seed: 10000 credits, ship
  // class 0, current system 0, no intro configured yet. The new-game flow then
  // overlays the pilot's name, start config and intro frames.
  [[nodiscard]] static PilotFile Fresh();
};

// Apply a newly-seeded pilot record into the live GameState, so the intro
// cinematic and spaceflight modes read one consistent record. This mirrors
// the original's block-to-global copy (PilotData_InitializePlayerState /
// IntroCinematic_SetupFrames after the block is created). The record is held
// in memory only: the reimplementation does not write .plt files, so the
// persistent/load side (PilotFile_LoadSave) is not reconstructed.
void PilotFileApply(const PilotFile &pilot_file, GameState &state);

} // namespace game
