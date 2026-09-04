#include "pilot_file.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace game {
namespace {

// Block sizes allocated/written by PilotFile_SaveGameCore (0x004c7dd0).
// docs/pilot_save_file_format.md.
constexpr std::size_t kBlock1Size = 0xe952; // PilotState
constexpr std::size_t kBlock2Size = 0x66fe; // FleetState/world state

// FleetState magic written at block2+0x00; the loader rejects anything below
// 300 (0x004cb260).
constexpr std::int16_t kFleetBlockVersion = 300;
// First u16 of a .prf-style prefs file (rejected by the .plt loader as
// "wrong file type").
constexpr std::int16_t kPrefsFileSignature = 0x6b;

// Round a float to the nearest integer the way the original's ROUND()
// helper does for the u16 shield/fuel fields.
[[nodiscard]] std::int16_t RoundToInt16(float value) {
  return static_cast<std::int16_t>(std::lrintf(value));
}

[[nodiscard]] std::uint16_t ReadU16(std::span<const std::byte> bytes,
                                    std::size_t offset) {
  std::uint16_t v = 0;
  std::memcpy(&v, bytes.data() + offset, sizeof(v));
  return v;
}

[[nodiscard]] std::uint32_t ReadU32(std::span<const std::byte> bytes,
                                    std::size_t offset) {
  std::uint32_t v = 0;
  std::memcpy(&v, bytes.data() + offset, sizeof(v));
  return v;
}

void WriteU16(std::vector<std::byte> &out,
              std::size_t offset,
              std::uint16_t v) {
  std::memcpy(out.data() + offset, &v, sizeof(v));
}

void WriteU32(std::vector<std::byte> &out,
              std::size_t offset,
              std::uint32_t v) {
  std::memcpy(out.data() + offset, &v, sizeof(v));
}

// The block-level validity gate, mirroring PilotSave_ValidateBlock
// (0x008725b0): a block whose first u16 is < 0x800 is accepted without
// further checks. TODO(decomp): the original computes a 32-bit rolling
// checksum for u16[0] >= 0x800 (LAB_0046f960); that path is not reconstructed
// and such blocks are treated as invalid here.
[[nodiscard]] bool BlockPassesGate(std::span<const std::byte> block) {
  return block.size() >= 2 && ReadU16(block, 0) < 0x800;
}

} // namespace

PilotFile PilotFile::Fresh() {
  // Ghidra 0x004cd4b0 PilotData_InitializePlayerState, absent-block seed:
  // g_ship_states->credits = 10000, ship_class_id = 0, current_system_id = 0,
  // combat rating = 0, in-game date 1999/1/1, per-govt reputation reset. The
  // reputation/combat-rating fields are not tracked by GameState yet, so
  // only the ship-core defaults and the calendar are seeded here.
  PilotFile fresh;
  fresh.credits = 10000;
  fresh.ship_class_id = 0;
  fresh.current_system_id = 0;
  fresh.date = GameDate{1999, 1, 1};
  return fresh;
}

void PilotFileApply(const PilotFile &pilot_file, GameState &state) {
  // The original copies the seeded/loaded pilot-save block into the live
  // globals (PilotData_InitializePlayerState fresh-seed and IntroCinematic_
  // SetupFrames). Apply the tracked subset into GameState.
  state.pilot.first_name = pilot_file.pilot_name;
  state.pilot.last_name = pilot_file.nickname;

  state.player.credits = pilot_file.credits;
  state.player.ship_class_id = pilot_file.ship_class_id;
  state.player.current_system_id = pilot_file.current_system_id;
  state.player.active_weapon_bank_slot = pilot_file.active_weapon_bank_slot;
  state.date = pilot_file.date;
  state.player.timed_action_counter = pilot_file.timed_action_counter;
  state.player.death_timer_active = pilot_file.death_timer_active;
  state.player.shield_points = pilot_file.shield_points;
  state.player.armor_points = pilot_file.armor_points;
  state.player.fuel_points = pilot_file.fuel_points;
  state.player.pos_x = pilot_file.pos_x;
  state.player.pos_y = pilot_file.pos_y;
  state.player.vel_x = pilot_file.vel_x;
  state.player.vel_y = pilot_file.vel_y;
  state.player.heading = pilot_file.heading;
  state.player.speed = pilot_file.speed;

  state.intro_cinematic.source_pict_ids = pilot_file.intro_source_pict_ids;
  state.intro_cinematic.duration_60h_ticks =
      pilot_file.intro_duration_60h_ticks;
  state.intro_cinematic.post_intro_dest_id = pilot_file.post_intro_dest_id;
  state.intro_played = pilot_file.intro_played;

  state.inventory.cargo_bins = pilot_file.cargo_bins;
  state.inventory.outfit_owned_count = pilot_file.outfit_owned_count;
  state.inventory.junk_counts = pilot_file.junk_counts;
  state.weapon_bank_ammo = pilot_file.weapon_bank_ammo;
  state.weapon_bank_secondary = pilot_file.weapon_bank_secondary;
  state.active_mission_runtime_flags = pilot_file.active_mission_runtime_flags;
  state.active_missions = pilot_file.active_missions;
}

PilotFile PilotFileCollectFromState(const GameState &state) {
  PilotFile out;
  out.pilot_name = state.pilot.first_name;
  out.nickname = state.pilot.last_name;
  out.ship_name = state.player.ship_name;

  out.credits = state.player.credits;
  out.ship_class_id = state.player.ship_class_id;
  out.current_system_id = state.player.current_system_id;
  out.date = state.date;
  out.active_weapon_bank_slot = state.player.active_weapon_bank_slot;
  out.timed_action_counter = state.player.timed_action_counter;
  out.death_timer_active = state.player.death_timer_active;
  out.shield_points = state.player.shield_points;
  out.armor_points = state.player.armor_points;
  out.fuel_points = state.player.fuel_points;
  out.pos_x = state.player.pos_x;
  out.pos_y = state.player.pos_y;
  out.vel_x = state.player.vel_x;
  out.vel_y = state.player.vel_y;
  out.heading = state.player.heading;
  out.speed = state.player.speed;

  out.intro_source_pict_ids = state.intro_cinematic.source_pict_ids;
  out.intro_duration_60h_ticks = state.intro_cinematic.duration_60h_ticks;
  out.post_intro_dest_id = state.intro_cinematic.post_intro_dest_id;
  out.intro_played = state.intro_played;

  out.cargo_bins = state.inventory.cargo_bins;
  out.outfit_owned_count = state.inventory.outfit_owned_count;
  out.junk_counts = state.inventory.junk_counts;
  out.weapon_bank_ammo = state.weapon_bank_ammo;
  out.weapon_bank_secondary = state.weapon_bank_secondary;
  out.active_mission_runtime_flags = state.active_mission_runtime_flags;
  out.active_missions = state.active_missions;
  return out;
}

std::vector<std::byte> PilotFileSerialize(const PilotFile &pilot_file,
                                          std::int16_t jump_dest_stellar) {
  std::vector<std::byte> block1(kBlock1Size, std::byte{0});
  std::vector<std::byte> block2(kBlock2Size, std::byte{0});

  // -- Block1 (PilotState) field fills. Offsets are the .plt layout from
  // docs/pilot_save_file_format.md (PilotFile_SaveGameCore 0x004c7dd0).
  WriteU16(block1, 0x00, static_cast<std::uint16_t>(jump_dest_stellar));
  WriteU16(block1, 0x02, static_cast<std::uint16_t>(pilot_file.ship_class_id));
  for (std::size_t i = 0; i < pilot_file.cargo_bins.size(); ++i) {
    WriteU16(block1,
             0x04 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.cargo_bins[i]));
  }
  // +0x10 shield, +0x12 fuel as rounded u16. The loader restores fuel but
  // recomputes shield from class+outfits (see PilotFileDeserialize).
  WriteU16(block1,
           0x10,
           static_cast<std::uint16_t>(RoundToInt16(pilot_file.shield_points)));
  WriteU16(block1,
           0x12,
           static_cast<std::uint16_t>(RoundToInt16(pilot_file.fuel_points)));
  // +0x14/+0x16/+0x18 month/day/year: the in-game calendar
  // (g_current_game_year_month / g_current_game_day).
  WriteU16(block1, 0x14, static_cast<std::uint16_t>(pilot_file.date.month));
  WriteU16(block1, 0x16, static_cast<std::uint16_t>(pilot_file.date.day));
  WriteU16(block1, 0x18, static_cast<std::uint16_t>(pilot_file.date.year));
  for (std::size_t i = 0; i < pilot_file.outfit_owned_count.size(); ++i) {
    WriteU16(block1,
             0x101a + 2 * i,
             static_cast<std::uint16_t>(pilot_file.outfit_owned_count[i]));
  }
  // +0x241a/+0x261a weapon bank ammo/secondary: the original persists only the
  // first slot of each 100-slot bank (weapon_bank_ammo[i*100]).
  for (std::size_t i = 0; i < 0x100; ++i) {
    WriteU16(block1,
             0x241a + 2 * i,
             static_cast<std::uint16_t>(pilot_file.weapon_bank_ammo[i * 100]));
    WriteU16(
        block1,
        0x261a + 2 * i,
        static_cast<std::uint16_t>(pilot_file.weapon_bank_secondary[i * 100]));
  }
  WriteU32(block1, 0x281a, static_cast<std::uint32_t>(pilot_file.credits));

  // MissionRuntimeFlags (0x281e, 16 x 0x14). These are explicit little-endian
  // fields in the file, while the remaining padding at +0x0c is preserved as
  // zero (the live clean-room type has no confirmed meaning there).
  constexpr std::size_t kRuntimeFlagsOffset = 0x281e;
  constexpr std::size_t kRuntimeFlagsStride = 0x14;
  for (std::size_t slot = 0;
       slot < pilot_file.active_mission_runtime_flags.size();
       ++slot) {
    const auto &flags = pilot_file.active_mission_runtime_flags[slot];
    const auto offset = kRuntimeFlagsOffset + slot * kRuntimeFlagsStride;
    block1[offset + 0x00] = flags.is_active ? std::byte{1} : std::byte{0};
    block1[offset + 0x01] =
        flags.initial_briefing_done ? std::byte{1} : std::byte{0};
    block1[offset + 0x02] =
        flags.objective_complete ? std::byte{1} : std::byte{0};
    block1[offset + 0x03] = flags.is_failed ? std::byte{1} : std::byte{0};
    WriteU16(block1, offset + 0x04, flags.flags_primary_at_accept);
    WriteU16(
        block1, offset + 0x06, static_cast<std::uint16_t>(flags.deadline_year));
    WriteU16(block1,
             offset + 0x08,
             static_cast<std::uint16_t>(flags.deadline_month));
    WriteU16(
        block1, offset + 0x0a, static_cast<std::uint16_t>(flags.deadline_day));
    WriteU32(block1,
             offset + 0x0e,
             static_cast<std::uint32_t>(flags.elapsed_travel_days));
    WriteU16(block1, offset + 0x12, flags.elapsed_travel_subday);
  }

  // MisnActive (0x295e, 16 x 0x8e6). Start with the opaque record so unknown
  // script/text state is retained, then overlay the fields confirmed by the
  // Ghidra type layout. This is deliberately not a native-struct memcpy.
  constexpr std::size_t kActiveMissionsOffset = 0x295e;
  constexpr std::size_t kActiveMissionStride = 0x8e6;
  for (std::size_t slot = 0; slot < pilot_file.active_missions.size(); ++slot) {
    const auto &mission = pilot_file.active_missions[slot];
    const auto offset = kActiveMissionsOffset + slot * kActiveMissionStride;
    std::memcpy(block1.data() + offset,
                mission.raw_payload.data(),
                mission.raw_payload.size());
    const auto put_i16 = [&](std::size_t field, std::int16_t value) {
      WriteU16(block1, offset + field, static_cast<std::uint16_t>(value));
    };
    put_i16(0x00, mission.travel_stellar_id);
    put_i16(0x04, mission.return_stellar_id);
    put_i16(0x06, mission.target_ship_count);
    put_i16(0x08, mission.dude_def_index);
    put_i16(0x0a, mission.spawn_behavior);
    put_i16(0x0c, mission.fleet_spawn_goal);
    put_i16(0x0e, mission.special_ship_spawn_mode);
    put_i16(0x10, mission.current_system_id);
    put_i16(0x12, mission.cargo_type_id);
    put_i16(0x14, mission.cargo_qty_tons);
    put_i16(0x16, mission.pickup_mode);
    put_i16(0x18, mission.drop_off_mode);
    put_i16(0x1a, mission.scan_mask);
    put_i16(0x1c, mission.comp_govt_id);
    put_i16(0x1e, mission.comp_reward_delta);
    put_i16(0x20, mission.goal_count_remaining);
    WriteU32(block1,
             offset + 0x22,
             static_cast<std::uint32_t>(mission.resource_delta_or_cost));
    put_i16(0x2c, mission.goal_count_remaining);
    block1[offset + 0x32] = mission.can_abort ? std::byte{1} : std::byte{0};
    block1[offset + 0x33] =
        mission.carrying_resources ? std::byte{1} : std::byte{0};
    for (std::size_t i = 0; i < mission.brief_description_ids.size(); ++i) {
      put_i16(0x35 + i * sizeof(std::int16_t),
              mission.brief_description_ids[i]);
    }
    put_i16(0x47, mission.special_ship_name_string_id);
    put_i16(0x49, mission.special_ship_name_entry);
    put_i16(0x4b, mission.spawn_rearm_timer);
    put_i16(0x4d, mission.mission_template_id);
    put_i16(0x4f, mission.random_text_string_id);
    put_i16(0x51, mission.random_text_entry);
    put_i16(0x53, mission.special_ship_type_index);
    WriteU16(block1, offset + 0x55, mission.flags_primary);
    WriteU16(block1, offset + 0x57, mission.flags_secondary);
    put_i16(0x61, mission.mission_ship_count_max);
    put_i16(0x63, mission.aux_ships_dude_def_index);
    put_i16(0x6b, mission.mission_ship_count_active);
  }

  // -- Block2 (FleetState/world state) field fills.
  WriteU16(block2, 0x00, static_cast<std::uint16_t>(kFleetBlockVersion));
  // +0x02 ongoing/new-pilot latch (DAT_00596d2f) and +0x04 strict-play latch
  // (DAT_00734c1c): not tracked by GameState. TODO(decomp).
  WriteU16(block2, 0x3086, pilot_file.intro_played ? 1 : 0);
  for (std::size_t i = 0; i < pilot_file.junk_counts.size(); ++i) {
    WriteU16(block2,
             0x3488 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.junk_counts[i]));
  }
  // +0x5d98 pilot nickname C-string, capped at 0x40 bytes like the original
  // CString_CopyBounded(&DAT_005999cc, ..., 0x40).
  const std::size_t nick_len =
      std::min<std::size_t>(pilot_file.nickname.size(), 0x3f);
  if (nick_len > 0) {
    std::memcpy(block2.data() + 0x5d98, pilot_file.nickname.data(), nick_len);
  }
  block2[0x5d98 + nick_len] = std::byte{0};

  // File framing: [u32 block1 size][block1][u32 block2 size][block2]
  // [ship-name C-string] (FUN_004f22b0 / Stream_WriteLocked /
  // FUN_004f22e0 in the original).
  std::vector<std::byte> out;
  out.reserve(4 + kBlock1Size + 4 + kBlock2Size + pilot_file.ship_name.size() +
              1);
  const auto append_u32 = [&out](std::uint32_t v) {
    const std::array<std::byte, 4> le{static_cast<std::byte>(v & 0xff),
                                      static_cast<std::byte>((v >> 8) & 0xff),
                                      static_cast<std::byte>((v >> 16) & 0xff),
                                      static_cast<std::byte>((v >> 24) & 0xff)};
    out.insert(out.end(), le.begin(), le.end());
  };
  append_u32(static_cast<std::uint32_t>(kBlock1Size));
  out.insert(out.end(), block1.begin(), block1.end());
  append_u32(static_cast<std::uint32_t>(kBlock2Size));
  out.insert(out.end(), block2.begin(), block2.end());
  for (const char c : pilot_file.ship_name) {
    out.push_back(static_cast<std::byte>(c));
  }
  out.push_back(std::byte{0});
  return out;
}

PilotLoadError PilotFileDeserialize(std::span<const std::byte> bytes,
                                    PilotFile &out) {
  // Parse the [u32 size][data] framing. Returns kMissingOrEmptyFile when a
  // size header is 0 or the block is truncated (original: -0x2b).
  if (bytes.size() < 8) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  const std::size_t size1 = ReadU32(bytes, 0);
  if (size1 == 0 || 4 + size1 > bytes.size()) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  const std::span<const std::byte> block1 = bytes.subspan(4, size1);
  const std::size_t size2_offset = 4 + size1;
  if (size2_offset + 4 > bytes.size()) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  const std::size_t size2 = ReadU32(bytes, size2_offset);
  if (size2 == 0 || size2_offset + 4 + size2 > bytes.size()) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  const std::span<const std::byte> block2 =
      bytes.subspan(size2_offset + 4, size2);
  const std::span<const std::byte> trailer =
      bytes.subspan(size2_offset + 4 + size2);

  // Ghidra 0x004cb260: the whole restore is gated per block by
  // PilotSave_ValidateBlock (0x008725b0) — u16[0] < 0x800 is accepted, larger
  // values (which would include a jump_dest of -1) skip that block's restore.
  bool repairs = false;
  // The original reads fixed offsets without bounds checks (it assumes
  // full-size blocks from the same game build). To avoid UB on truncated
  // input, tracked fields are only restored when the block covers them.
  constexpr std::size_t kBlock1TrackedEnd =
      0x295e + GameState::kMaxActiveMissions * 0x8e6;
  constexpr std::size_t kBlock2TrackedEnd = 0x5d98 + 0x40;
  if (BlockPassesGate(block1) && block1.size() >= kBlock1TrackedEnd) {
    // jump destination stellar id.
    out.jump_dest_stellar = static_cast<std::int16_t>(ReadU16(block1, 0x00));
    out.ship_class_id = static_cast<std::int16_t>(ReadU16(block1, 0x02));
    if (out.ship_class_id < 0 || out.ship_class_id >= 0x300) {
      // Original: class fallback loop over g_ship_class_defs. TODO(decomp):
      // validate against the loaded ship-class table; here only bounds.
      NovaLog::Todo("pilot load: ship class {} out of range; falling back to 0",
                    out.ship_class_id);
      out.ship_class_id = 0;
      repairs = true;
    }
    for (std::size_t i = 0; i < out.cargo_bins.size(); ++i) {
      out.cargo_bins[i] =
          static_cast<std::int16_t>(ReadU16(block1, 0x04 + 2 * i));
    }
    // +0x10 shield: the original recomputes shield/armor from class+outfits
    // and does NOT read the stored value back (Ship_ComputeShipMaxShieldPoints
    // / Ship_ComputeShipMaxArmor). TODO(decomp): recompute-equivalent once the
    // outfit math is reconstructed; fuel is restored from +0x12.
    out.fuel_points = static_cast<float>(ReadU16(block1, 0x12));
    // +0x14/+0x16/+0x18 month/day/year: the in-game calendar.
    out.date.month = static_cast<std::int16_t>(ReadU16(block1, 0x14));
    out.date.day = static_cast<std::int16_t>(ReadU16(block1, 0x16));
    out.date.year = static_cast<std::int16_t>(ReadU16(block1, 0x18));
    for (std::size_t i = 0; i < out.outfit_owned_count.size(); ++i) {
      out.outfit_owned_count[i] =
          static_cast<std::int16_t>(ReadU16(block1, 0x101a + 2 * i));
      // Original zeroes outfits whose def no longer exists. TODO(decomp).
    }
    for (std::size_t i = 0; i < 0x100; ++i) {
      out.weapon_bank_ammo[i * 100] =
          static_cast<std::int16_t>(ReadU16(block1, 0x241a + 2 * i));
      out.weapon_bank_secondary[i * 100] =
          static_cast<std::int16_t>(ReadU16(block1, 0x261a + 2 * i));
      // Original zeroes banks whose weapon def no longer exists. TODO(decomp).
    }
    out.credits = static_cast<std::int32_t>(ReadU32(block1, 0x281a));
    constexpr std::size_t kRuntimeFlagsOffset = 0x281e;
    constexpr std::size_t kRuntimeFlagsStride = 0x14;
    for (std::size_t slot = 0; slot < out.active_mission_runtime_flags.size();
         ++slot) {
      auto &flags = out.active_mission_runtime_flags[slot];
      const auto offset = kRuntimeFlagsOffset + slot * kRuntimeFlagsStride;
      flags.is_active =
          std::to_integer<unsigned char>(block1[offset + 0x00]) != 0;
      flags.initial_briefing_done =
          std::to_integer<unsigned char>(block1[offset + 0x01]) != 0;
      flags.objective_complete =
          std::to_integer<unsigned char>(block1[offset + 0x02]) != 0;
      flags.is_failed =
          std::to_integer<unsigned char>(block1[offset + 0x03]) != 0;
      flags.flags_primary_at_accept = ReadU16(block1, offset + 0x04);
      flags.deadline_year =
          static_cast<std::int16_t>(ReadU16(block1, offset + 0x06));
      flags.deadline_month =
          static_cast<std::int16_t>(ReadU16(block1, offset + 0x08));
      flags.deadline_day =
          static_cast<std::int16_t>(ReadU16(block1, offset + 0x0a));
      flags.elapsed_travel_days =
          static_cast<std::int32_t>(ReadU32(block1, offset + 0x0e));
      flags.elapsed_travel_subday = ReadU16(block1, offset + 0x12);
    }
    constexpr std::size_t kActiveMissionsOffset = 0x295e;
    constexpr std::size_t kActiveMissionStride = 0x8e6;
    for (std::size_t slot = 0; slot < out.active_missions.size(); ++slot) {
      auto &mission = out.active_missions[slot];
      const auto offset = kActiveMissionsOffset + slot * kActiveMissionStride;
      std::memcpy(mission.raw_payload.data(),
                  block1.data() + offset,
                  mission.raw_payload.size());
      const auto get_i16 = [&](std::size_t field) {
        return static_cast<std::int16_t>(ReadU16(block1, offset + field));
      };
      mission.travel_stellar_id = get_i16(0x00);
      mission.return_stellar_id = get_i16(0x04);
      mission.target_ship_count = get_i16(0x06);
      mission.dude_def_index = get_i16(0x08);
      mission.spawn_behavior = get_i16(0x0a);
      mission.fleet_spawn_goal = get_i16(0x0c);
      mission.special_ship_spawn_mode = get_i16(0x0e);
      mission.current_system_id = get_i16(0x10);
      mission.cargo_type_id = get_i16(0x12);
      mission.cargo_qty_tons = get_i16(0x14);
      mission.pickup_mode = get_i16(0x16);
      mission.drop_off_mode = get_i16(0x18);
      mission.scan_mask = get_i16(0x1a);
      mission.comp_govt_id = get_i16(0x1c);
      mission.comp_reward_delta = get_i16(0x1e);
      mission.resource_delta_or_cost =
          static_cast<std::int32_t>(ReadU32(block1, offset + 0x22));
      mission.goal_count_remaining = get_i16(0x2c);
      mission.can_abort =
          std::to_integer<unsigned char>(block1[offset + 0x32]) != 0;
      mission.carrying_resources =
          std::to_integer<unsigned char>(block1[offset + 0x33]) != 0;
      for (std::size_t i = 0; i < mission.brief_description_ids.size(); ++i) {
        mission.brief_description_ids[i] =
            get_i16(0x35 + i * sizeof(std::int16_t));
      }
      mission.special_ship_name_string_id = get_i16(0x47);
      mission.special_ship_name_entry = get_i16(0x49);
      mission.spawn_rearm_timer = get_i16(0x4b);
      mission.mission_template_id = get_i16(0x4d);
      mission.random_text_string_id = get_i16(0x4f);
      mission.random_text_entry = get_i16(0x51);
      mission.special_ship_type_index = get_i16(0x53);
      mission.flags_primary = ReadU16(block1, offset + 0x55);
      mission.flags_secondary = ReadU16(block1, offset + 0x57);
      mission.mission_ship_count_max = get_i16(0x61);
      mission.aux_ships_dude_def_index = get_i16(0x63);
      mission.mission_ship_count_active = get_i16(0x6b);
    }
  } else {
    NovaLog::Todo("pilot load: block1 rejected by the validity gate or too "
                  "short (u16[0] = 0x{:x}, size {}); restore skipped",
                  block1.size() >= 2 ? ReadU16(block1, 0) : 0,
                  block1.size());
  }

  // Block2 gate + FleetState magic checks (original: 0x6b -> -0x2d,
  // < 300 -> -0x2a).
  if (!BlockPassesGate(block2) || block2.size() < kBlock2TrackedEnd) {
    NovaLog::Todo("pilot load: block2 rejected by the validity gate or too "
                  "short; restore skipped");
  } else {
    const std::int16_t magic = static_cast<std::int16_t>(ReadU16(block2, 0));
    if (magic == kPrefsFileSignature) {
      return PilotLoadError::kWrongFileType;
    }
    if (magic < kFleetBlockVersion) {
      return PilotLoadError::kInvalidFleetBlock;
    }
    out.intro_played = ReadU16(block2, 0x3086) != 0;
    for (std::size_t i = 0; i < out.junk_counts.size(); ++i) {
      out.junk_counts[i] =
          static_cast<std::int16_t>(ReadU16(block2, 0x3488 + 2 * i));
      // Original zeroes junk whose def no longer exists. TODO(decomp).
    }
    // +0x5d98 nickname C-string (0x40 cap, NUL-terminated).
    out.nickname.clear();
    for (std::size_t i = 0; i < 0x40 && i < block2.size() - 0x5d98; ++i) {
      const auto c = static_cast<char>(block2[0x5d98 + i]);
      if (c == '\0') {
        break;
      }
      out.nickname.push_back(c);
    }
    // +0x3086 seen-intro (done), +0x3088/0x3288 disasters, +0x3590/0x3990 cron,
    // +0x3d90/0x4d90 availability rolls, +0x5dde system cues: untracked.
    // TODO(decomp).
  }

  // Trailer: ship-name C-string (read via FUN_004f2350 in the original).
  out.ship_name.clear();
  for (const auto b : trailer) {
    const auto c = static_cast<char>(b);
    if (c == '\0') {
      break;
    }
    out.ship_name.push_back(c);
  }
  out.ship_name.shrink_to_fit();

  return repairs ? PilotLoadError::kRepairsApplied : PilotLoadError::kOk;
}

bool PilotFileSaveGame(const std::filesystem::path &nova_files_dir,
                       const GameState &state,
                       std::int16_t jump_dest_stellar) {
  // Ghidra 0x004c7db0 PilotFile_SaveGame -> 0x004c7dd0 PilotFile_SaveGameCore:
  // builds <nova_files><pilot name>.plt, serializes the state blocks, writes
  // them, then records the last-pilot marker (PilotFile_RecordLastPilotPath
  // 0x004c7d40 -- not wired, see header).
  PilotFile record = PilotFileCollectFromState(state);
  const std::vector<std::byte> bytes =
      PilotFileSerialize(record, jump_dest_stellar);
  const std::filesystem::path path =
      nova_files_dir / (record.pilot_name + ".plt");
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    NovaLog::Error("pilot save: could not open '{}' for writing",
                   path.string());
    return false;
  }
  file.write(reinterpret_cast<const char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!file) {
    NovaLog::Error("pilot save: short write to '{}'", path.string());
    return false;
  }
  NovaLog::Debug(
      "pilot save: wrote {} bytes to '{}'", bytes.size(), path.string());
  return true;
}

PilotLoadError PilotFileLoadSave(const std::filesystem::path &path,
                                 GameState &state) {
  // Ghidra 0x004cb260 PilotFile_LoadSave. Reads the whole file, restores the
  // tracked subset, then derives the pilot name from the file path.
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  const std::string raw{std::istreambuf_iterator<char>(file),
                        std::istreambuf_iterator<char>()};
  if (raw.empty()) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  std::vector<std::byte> bytes(raw.size());
  std::memcpy(bytes.data(), raw.data(), raw.size());

  PilotFile record;
  const PilotLoadError err = PilotFileDeserialize(bytes, record);
  if (err != PilotLoadError::kOk && err != PilotLoadError::kRepairsApplied) {
    return err;
  }

  // The original derives DAT_005997cc (pilot name) from the .plt path:
  // substring after the last ':' (FUN_004d6150 = strrchr), then cut at the
  // first '.', dropping the ".plt" extension.
  std::string name = path.filename().string();
  if (const auto dot = name.find('.'); dot != std::string::npos) {
    name.resize(dot);
  }
  record.pilot_name = name;
  // current_system_id is not serialized; the loader resolves it from the saved
  // jump destination stellar (Ghidra 0x004cb260 fallback chain).
  record.current_system_id = [&]() -> std::int16_t {
    if (record.jump_dest_stellar >= 0) {
      if (const auto *st =
              state.scenario.Stellar(record.jump_dest_stellar + 0x80)) {
        if (st->system_id >= 0) {
          return st->system_id;
        }
      }
    }
    // Original also tries System_FindSystemContainingStellar then any explored
    // system, then 0. TODO(decomp): the save's per-system discovery block
    // (u16[0x800] of SystemDef.discovery_state at block1 + 0x1a,
    // PilotFile_SaveGameCore 0x004c7dd0 / LoadSave 0x004cb260) is not yet
    // serialized by this clean-room save format; restoring it should re-mark
    // each visited system (NovaSystem_MarkSystemVisited) and rebuild the map
    // reveal from the restored system.
    return 0;
  }();

  PilotFileApply(record, state);
  // The original's session-start path (PilotData_AutoresumeLastPilot
  // 0x004ca120 / Menu_OpenPilotFileDialog 0x004c9e90) runs Ship_ResetPlayer-
  // ShipState (which latches the flight-hint state to 0x7fff, 0x004b3a3b)
  // before PilotFile_LoadSave, and LoadSave itself never writes DAT_007cab1c
  // -- so a resumed pilot starts with the launch departure message armed.
  state.travel.travel_hint_state = 0x7fff;
  NovaLog::Debug("pilot load: '{}' restored (jump dest stellar {}, system {})",
                 path.string(),
                 record.jump_dest_stellar,
                 record.current_system_id);
  return err;
}

bool PilotFileProbeExists(const std::filesystem::path &path) {
  // Ghidra 0x004cd030: tries to open the file; nonzero when it exists.
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

void PilotFileDelete(const std::filesystem::path &path) {
  // Ghidra 0x004cd040: guarded by ProbeExists so only existing files are
  // touched (permadeath path).
  if (PilotFileProbeExists(path)) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

std::string PilotData_FindActivePilotName() {
  // Ghidra 0x004cd290: scans the 0x63688a72 family in registry order; the
  // first entry with flags bit 0 set at block+0x132 donates its metadata
  // name. No active entry -> the empty default (DAT_0056df7d). The u16 flags
  // word is big-endian like the rest of the resource block.
  for (const auto &[type_code, id] : NovaResource_AllKeys()) {
    if (type_code != kResourceTypeCharacter) {
      continue;
    }
    const auto block = NovaResource_LoadNamed(kResourceTypeCharacter, id);
    if (!block || block->bytes.size() < 0x134) {
      continue;
    }
    const auto flags = static_cast<std::uint16_t>(
        (std::to_integer<unsigned>(block->bytes[0x132]) << 8U) |
        std::to_integer<unsigned>(block->bytes[0x133]));
    if ((flags & 1) != 0) {
      return block->name;
    }
  }
  return {};
}

} // namespace game
