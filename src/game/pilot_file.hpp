#pragma once

// Clean-room model of the per-pilot save *record*. This is NOT a living
// gameplay object: the original keeps the running game in globals
// (g_ship_states / ShipState, g_outfit_owned_count, g_system_defs, ...) and
// only materializes a pilot file as a serialized snapshot on demand --
// (a) seeded transiently during the new-game flow
//   (PilotData_InitializePlayerState 0x004cd4b0 creates/grows the block,
//    IntroCinematic_SetupFrames 0x004cd3b0 reads the intro frames from it),
// (b) read back into the globals when continuing a pilot
//   (PilotFile_LoadSave 0x004cb260 copies the .plt block into the globals),
// (c) written out to disk by PilotFile_SaveGameCore (0x004c7dd0).
//
// In the original the on-disk .plt is a large binary block (offsets up to
// 0xe94e) holding ship state, outfit/weapon tables, system discovery, per-govt
// reputations, missions/FleetState, dates, etc. The vast majority of those
// fields are not reconstructed yet (the live GameState does not track them).
// So this build carries a focused PilotFile covering exactly the fields the
// current GameState tracks, each mapped to its Ghidra .plt block offset when
// known. The remaining original fields are absent until their subsystems are
// reconstructed; this record is over the tracked subset only. Serialization
// (PilotFileSerialize / PilotFileDeserialize) mirrors the original block
// layout and offsets for the tracked fields and zero-fills the rest, so a
// reimplementation save is structurally a valid .plt but does not round-trip
// the untracked original state. See docs/pilot_save_file_format.md.

#include "game_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace game {

// Mirrors the persistent per-pilot save record. Fields not yet meaningful to
// GameState (faction reputations, system discovery, missions/FleetState,
// dates, per-stellar availability) are deliberately not present; see above.
struct PilotFile {
  // The pilot's identity/callsign. On disk the file is <nova_files><name>.plt;
  // in-memory the original keys the pilot-save registry block by this name
  // (ResourceData_AccessByKey 0x63688a72; block+0x32 carries the name/opener).
  std::string pilot_name;
  // The pilot's nickname/callsign suffix (Ghidra DAT_005999cc), stored in the
  // .plt FleetState block at +0x5d98. The new-game flow uses it as the second
  // generated opener string (last name).
  std::string nickname;
  // The player ship's name (Ghidra DAT_00599acc). Serialized as the .plt
  // trailer C-string; not yet applied to any GameState visuals.
  std::string ship_name;

  // -- Player ship core (Ghidra .plt offsets from PilotFile_LoadSave) --------
  // The saved jump/travel destination stellar id (block1+0x00). The loader
  // places the player at this stellar on restore; -1 means none.
  std::int16_t jump_dest_stellar = -1;
  std::int32_t credits = 0;       // block1+0x281a
  std::int16_t ship_class_id = 0; // block1+0x02 (0 = default class)
  std::int16_t current_system_id = 0;
  std::int16_t active_weapon_bank_slot = 0;
  // The in-game calendar (block1+0x14/+0x16/+0x18 month/day/year).
  GameDate date{};
  std::int16_t timed_action_counter = -1;
  float death_timer_active = -1.0F;
  float shield_points = 0.0F; // block1+0x10 as u16 (rounded; not read back)
  float armor_points = 0.0F;  // not serialized (recomputed on load)
  float fuel_points = 0.0F;   // block1+0x12 as u16 (rounded)
  float pos_x = 0.0F;
  float pos_y = 0.0F;
  float vel_x = 0.0F;
  float vel_y = 0.0F;
  float heading = 0.0F; // radians
  float speed = 0.0F;

  // -- Intro cinematic (IntroCinematic_SetupFrames reads block+0x20/0x28/0x30).
  std::array<std::int16_t, 4> intro_source_pict_ids{-1, -1, -1, -1};
  std::array<std::int16_t, 4> intro_duration_60h_ticks{0, 0, 0, 0};
  // Bible char resource IntroTextID (block+0x30); -1 = no dialog.
  std::int16_t intro_text_desc_id = -1;
  // Seen-intro-screen latch (Ghidra DAT_00596d35), block2+0x3086.
  bool intro_played = false;

  // -- Ownership tables (g_outfit_owned_count, weapon banks) -----------------
  std::array<std::int16_t, 6>
      cargo_bins{}; // block1+0x04 (ShipState field_0x7a..)
  // Persistent system fog and standing tables. Both occupy the original
  // fixed 0x800-slot address space; PilotFileApply projects the loaded prefix
  // into the scenario's currently defined systems.
  std::array<std::int16_t, GameState::kMaxSystems> system_discovery{};
  std::array<std::int16_t, GameState::kMaxSystems> system_reputation{};
  std::array<std::int16_t, 0x200> outfit_owned_count{};     // block1+0x101a
  std::array<std::int16_t, 0x100 * 100> weapon_bank_ammo{}; // block1+0x241a
  std::array<std::int16_t, 0x100 * 100>
      weapon_bank_secondary{}; // block1+0x261a
  // Junk item quantities (Ghidra g_junk_defs strided counts), block2+0x3488.
  std::array<std::int16_t, 0x80> junk_counts{};
  // The persisted player stat modifier quartet (DAT_007353f6..0x7353fd,
  // percentages; see GameState.player_stat_modifier_pct). SaveGameCore writes
  // them at FleetState block2 +0x3588/+0x358a/+0x358c/+0x358e and LoadSave
  // restores all four (0x004cb260).
  std::array<std::int16_t, 4> stat_modifier_pct{100, 100, 100, 100};

  // Mission persistence from PilotState block1. The original copies these
  // records byte-for-byte at +0x281e and +0x295e. Keeping them in the save
  // snapshot makes accepted missions survive a save/load cycle even while
  // their script and text fields remain only partially understood.
  std::array<MissionRuntimeFlags, GameState::kMaxActiveMissions>
      active_mission_runtime_flags{};
  std::array<ActiveMission, GameState::kMaxActiveMissions> active_missions{};

  // Fresh default for a brand-new pilot, mirroring
  // PilotData_InitializePlayerState's absent-block seed: 10000 credits, ship
  // class 0, current system 0, no intro configured yet. The new-game flow then
  // overlays the pilot's name, start config and intro frames.
  [[nodiscard]] static PilotFile Fresh();
};

// Apply a newly-seeded pilot record into the live GameState, so the intro
// cinematic and spaceflight modes read one consistent record. This mirrors
// the original's block-to-global copy (PilotData_InitializePlayerState /
// IntroCinematic_SetupFrames after the block is created).
void PilotFileApply(const PilotFile &pilot_file, GameState &state);

// See pilot_file.hpp. Iterates the family in registry order (resource id
// order in the archive world); only entries whose flags bit 0 is set are
// considered, and the first match wins (the original keeps scanning but its
// name buffer is already filled by then). The original also grows each
// visited entry to 0x16a bytes (ResourceData_EnsureBlockSize) and caches the
// found block in DAT_00863d60; archive blocks are fixed-size here, and the
// stock .Trader block is exactly 0x16a bytes, so both are no-ops. The
// in-memory registry slice (session-created pilots) is not reconstructed.
// TODO(decomp) once a .plt writer feeds the registry.
[[nodiscard]] std::string PilotData_FindActivePilotName();

// Ghidra 0x004cd290 PilotData_FindActivePilotName: returns the registered
// name of the first pilot-save family entry (0x63688a72) whose per-entry
// flags at block+0x132 have bit 0 set ("active"), or an empty string when no
// entry is active (the original's default is the empty Pascal string at
// DAT_0056df7d). Used to preselect the most recently active pilot in the
// new-pilot dialog's Character popup.

// Collect the live GameState into a PilotFile record (the inverse of
// PilotFileApply; the original saver reads the globals directly).
[[nodiscard]] PilotFile PilotFileCollectFromState(const GameState &state);

// ---------------------------------------------------------------------------
// .plt disk persistence.
//
// The on-disk format (docs/pilot_save_file_format.md) is:
//   [u32 block1 size][block1 data 0xe952][u32 block2 size][block2 data 0x66fe]
//   [ship-name C-string trailer]
// PilotFileSerialize writes the Windows little-endian form. Deserialize also
// accepts converted classic-Mac payloads as the documented compatibility
// divergence; the PilotFile* path helpers add the file I/O.
// ---------------------------------------------------------------------------

// Error codes returned by PilotFileDeserialize / PilotFileLoadSave, mirroring
// the original PilotFile_LoadSave (0x004cb260) returns.
enum class PilotLoadError : int {
  kOk = 0,
  kMissingOrEmptyFile = -0x2b, // 0xffffffd5: file open/read failure or size 0
  kInvalidFleetBlock = -0x2a,  // 0xffffffd6: block2 first u16 < 300
  kWrongFileType = -0x2d,  // 0xffffffd3: block2 first u16 == 0x6b (.prf-like)
  kRepairsApplied = -0x2e, // 0xffffffd2: restored with fallbacks (see log)
};

// Serialize the tracked PilotFile subset into the .plt byte layout. Mirrors
// PilotFile_SaveGameCore (0x004c7dd0) for the tracked fields; untracked
// regions are zero-filled. jump_dest_stellar is the destination written at
// block1+0x00 (the caller's current travel destination).
[[nodiscard]] std::vector<std::byte>
PilotFileSerialize(const PilotFile &pilot_file, std::int16_t jump_dest_stellar);

// Parse a .plt byte stream into `out` (mirrors PilotFile_LoadSave 0x004cb260
// restore of the tracked subset, including the PilotSave_ValidateBlock gate at
// 0x008725b0). Returns PilotLoadError; kRepairsApplied is returned when a
// fallback was used (state is still restored). `out` is only modified on
// kOk/kRepairsApplied.
[[nodiscard]] PilotLoadError
PilotFileDeserialize(std::span<const std::byte> bytes, PilotFile &out);

// Save <nova_files_dir>/<pilot name>.plt from the live state. Mirrors
// PilotFile_SaveGame (0x004c7db0) + PilotFile_SaveGameCore (0x004c7dd0).
// jump_dest_stellar is the player's current jump/travel destination. Returns
// false on I/O failure. Does not write the last-pilot marker file
// (PilotFile_RecordLastPilotPath 0x004c7d40) -- TODO(decomp): wire it once the
// marker file name (string-table id 0x82/4) is resolved.
[[nodiscard]] bool
PilotFileSaveGame(const std::filesystem::path &nova_files_dir,
                  const GameState &state,
                  std::int16_t jump_dest_stellar);

// Load an explicit .plt path into the live state. Mirrors PilotFile_LoadSave
// (0x004cb260) including deriving the pilot name from the file name (substring
// after the last ':', extension stripped). Returns PilotLoadError (kOk on
// success).
[[nodiscard]] PilotLoadError
PilotFileLoadSave(const std::filesystem::path &path, GameState &state);

// Whether a pilot save file at the resolved path exists. Mirrors
// PilotFile_ProbeExists (0x004cd030).
[[nodiscard]] bool PilotFileProbeExists(const std::filesystem::path &path);

// Delete a pilot save file. Mirrors PilotFile_Delete (0x004cd040) — only
// removes the file when it exists.
void PilotFileDelete(const std::filesystem::path &path);

} // namespace game
