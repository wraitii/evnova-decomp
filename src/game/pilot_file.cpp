#include "pilot_file.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "hud_overlay.hpp"
#include "mission.hpp"
#include "new_pilot_flow.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"
#include "ship_spawn.hpp"
#include "targeting.hpp"
#include "travel.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <numbers>
#include <random>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

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
constexpr std::string_view kLastPilotMarkerName = "Last Pilot";

[[nodiscard]] std::string ReadBoundedCString(std::span<const std::byte> bytes,
                                             std::size_t offset,
                                             std::size_t limit) {
  std::string out;
  for (std::size_t i = 0; i < limit; ++i) {
    const char c = static_cast<char>(bytes[offset + i]);
    if (c == '\0') {
      break;
    }
    out.push_back(c);
  }
  return out;
}

void WriteBoundedCString(std::vector<std::byte> &bytes,
                         std::size_t offset,
                         std::size_t capacity,
                         std::string_view value) {
  const std::size_t count = std::min(value.size(), capacity - 1);
  std::memcpy(bytes.data() + offset, value.data(), count);
  bytes[offset + count] = std::byte{0};
}

[[nodiscard]] bool
RecordLastPilotPath(const std::filesystem::path &marker_dir,
                    const std::filesystem::path &pilot_path) {
  const auto marker_path = marker_dir / kLastPilotMarkerName;
  std::ofstream marker(marker_path, std::ios::binary | std::ios::trunc);
  const std::string saved_path = pilot_path.string();
  marker.write(saved_path.c_str(),
               static_cast<std::streamsize>(saved_path.size() + 1));
  if (!marker) {
    NovaLog::Error("pilot marker: could not write '{}'", marker_path.string());
    return false;
  }
  return true;
}

// Truncate toward zero, matching the original's x87 FIST + residual/sign
// correction (0x004c7dd0 PilotFile_SaveGameCore), not round-to-nearest.
[[nodiscard]] std::int16_t RoundToInt16(float value) {
  return static_cast<std::int16_t>(value);
}

[[nodiscard]] std::uint16_t ReadU16(std::span<const std::byte> bytes,
                                    std::size_t offset,
                                    bool big_endian = false) {
  const auto a = std::to_integer<std::uint16_t>(bytes[offset]);
  const auto b = std::to_integer<std::uint16_t>(bytes[offset + 1]);
  return big_endian ? static_cast<std::uint16_t>((a << 8U) | b)
                    : static_cast<std::uint16_t>(a | (b << 8U));
}

[[nodiscard]] std::uint32_t ReadU32(std::span<const std::byte> bytes,
                                    std::size_t offset,
                                    bool big_endian = false) {
  const auto first = ReadU16(bytes, offset, big_endian);
  const auto second = ReadU16(bytes, offset + 2, big_endian);
  return big_endian ? (static_cast<std::uint32_t>(first) << 16U) | second
                    : first | (static_cast<std::uint32_t>(second) << 16U);
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

// Ghidra 0x008725b0 PilotSave_DecodeBlock. Plain blocks (first u16 < 0x800)
// are left alone. Otherwise it tail-calls the symmetric XOR transform at
// 0x0046f960 with (data, size, 0xb36a210f).
void DecodePilotBlock(std::span<std::byte> block, bool force = false) {
  if (block.size() < 2 || (!force && ReadU16(block, 0) < 0x800)) {
    return;
  }
  std::uint32_t key = 0xb36a210f;
  for (std::size_t offset = 0; offset < block.size();) {
    for (unsigned byte = 0; byte < 4 && offset < block.size();
         ++byte, ++offset) {
      block[offset] ^=
          static_cast<std::byte>((key >> (24U - byte * 8U)) & 0xffU);
    }
    key = (key + 0xdeadbeefU) ^ 0xdeadbeefU;
  }
}

} // namespace

std::optional<std::filesystem::path> PilotFileSaveDirectory() {
  char *raw = SDL_GetPrefPath("Ambrosia Software", "EV Nova");
  if (raw == nullptr) {
    NovaLog::Error("pilot save: SDL_GetPrefPath failed: {}", SDL_GetError());
    return std::nullopt;
  }
  const std::filesystem::path directory{raw};
  SDL_free(raw);
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    NovaLog::Error("pilot save: could not create '{}': {}",
                   directory.string(),
                   ec.message());
    return std::nullopt;
  }
  return directory;
}

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
  // The absent-block engagement default is the loader's <1 fallback
  // (destroyed_days_remaining -1, live strength reset to capacity).
  fresh.stellar_destroyed_days_remaining.fill(-1);
  fresh.disaster_days_remaining.fill(-1);
  fresh.disaster_active_stellars.fill(-1);
  fresh.cron_duration_counters.fill(-1);
  fresh.cron_holdoff_counters.fill(-1);
  fresh.escort_ship_class_ids.fill(-1);
  fresh.fighter_ship_class_ids.fill(-1);
  fresh.fighter_voice_types.fill(-1);
  return fresh;
}

void PilotFileSeedPersonalityPresence(const ScenarioData &scenario,
                                      PilotFile &record) {
  const std::size_t count =
      std::min(scenario.pers_defs.size(), record.pers_alive_flags.size());
  for (std::size_t i = 0; i < count; ++i) {
    record.pers_alive_flags[i] = scenario.pers_defs[i].alive ? 1 : 0;
    record.pers_grudge_flags[i] = scenario.pers_defs[i].grudge ? 1 : 0;
  }
}

void PilotFileApply(const PilotFile &pilot_file, GameState &state) {
  // The original copies the seeded/loaded pilot-save block into the live
  // globals (PilotData_InitializePlayerState fresh-seed and IntroCinematic_
  // SetupFrames). Apply the tracked subset into GameState.
  state.pilot.first_name = pilot_file.pilot_name;
  state.pilot.last_name = pilot_file.nickname;
  state.player.ship_name = pilot_file.ship_name;

  state.player.credits = pilot_file.credits;
  state.player_combat_rating_points = pilot_file.player_combat_rating_points;
  state.player.ship_class_id = pilot_file.ship_class_id;
  state.player.current_system_id = pilot_file.current_system_id;
  state.player.active_weapon_bank_slot = pilot_file.active_weapon_bank_slot;
  state.date = pilot_file.date;
  state.date_prefix = pilot_file.date_prefix;
  state.date_suffix = pilot_file.date_suffix;
  state.ship_paint_rgb5 = pilot_file.ship_paint_rgb5;
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
  state.intro_cinematic.intro_text_desc_id = pilot_file.intro_text_desc_id;
  state.intro_played = pilot_file.intro_played;
  state.pilot.strict_play = pilot_file.strict_play;
  // g_player_is_male is one global: PilotData.male carries the dialog selection
  // and PilotControlState.male is what the mission {g} expansion reads.
  state.pilot.male = pilot_file.male;
  state.control.male = pilot_file.male;

  state.reinforcement_retrigger_delay =
      pilot_file.reinforcement_retrigger_delay;
  for (std::size_t i = 0; i < state.target_category_command.size(); ++i) {
    state.target_category_command[i] =
        std::max<std::int16_t>(pilot_file.target_category_command[i], -1);
  }

  state.inventory.cargo_bins = pilot_file.cargo_bins;
  const std::size_t system_count = std::min(state.scenario.systems.size(),
                                            pilot_file.system_discovery.size());
  for (std::size_t i = 0; i < system_count; ++i) {
    state.scenario.systems[i].discovery_state = pilot_file.system_discovery[i];
  }
  state.system_reputation.assign(system_count, 0);
  std::copy_n(pilot_file.system_reputation.begin(),
              system_count,
              state.system_reputation.begin());
  state.inventory.outfit_owned_count = pilot_file.outfit_owned_count;
  state.inventory.junk_counts = pilot_file.junk_counts;
  state.control.persisted_bit_bytes = pilot_file.control_bits;
  for (std::size_t i = 0; i < pilot_file.control_bits.size(); ++i) {
    state.control.bits.set(i, pilot_file.control_bits[i] != 0);
  }
  state.player_stat_modifier_pct = pilot_file.stat_modifier_pct;
  state.weapon_count_by_class = pilot_file.weapon_count_by_class;
  state.weapon_secondary_count_by_class =
      pilot_file.weapon_secondary_count_by_class;
  state.active_mission_runtime_flags = pilot_file.active_mission_runtime_flags;
  state.active_missions = pilot_file.active_missions;
  const std::size_t pers_count = std::min(state.scenario.pers_defs.size(),
                                          pilot_file.pers_alive_flags.size());
  for (std::size_t i = 0; i < pers_count; ++i) {
    PersDef &pers = state.scenario.pers_defs[i];
    if (pers.ai_behavior_code < 1) {
      pers.alive = false;
    } else if (!pers.loaded_latch || pilot_file.pers_alive_flags[i] == 0) {
      pers.alive = false;
      pers.grudge = false;
    } else {
      pers.alive = true;
      pers.grudge = pilot_file.pers_grudge_flags[i] != 0;
    }
  }
  const std::size_t stellar_count = std::min(
      state.scenario.stellars.size(), pilot_file.stellar_dominated.size());
  for (std::size_t i = 0; i < stellar_count; ++i) {
    auto &stellar = state.scenario.stellars[i];
    stellar.dominated =
        stellar.is_defined ? pilot_file.stellar_dominated[i] : 0;
    if (!stellar.is_defined || stellar.dominated) {
      stellar.present_ship_count = 0;
      stellar.domination_days = 0;
    } else {
      stellar.present_ship_count = pilot_file.stellar_present_ship_counts[i];
      stellar.domination_days = pilot_file.stellar_domination_days[i];
    }
  }
  // Per-stellar engagement access + live strength (block2+0x4d90). The
  // original writes every one of the 0x800 slots regardless of is_defined.
  const std::size_t engage_count =
      std::min(state.scenario.stellars.size(),
               pilot_file.stellar_destroyed_days_remaining.size());
  for (std::size_t i = 0; i < engage_count; ++i) {
    Stellar &stellar = state.scenario.stellars[i];
    const std::int16_t access = pilot_file.stellar_destroyed_days_remaining[i];
    if (access < 1) {
      stellar.destroyed_days_remaining = -1;
      stellar.strength = stellar.strength_capacity;
    } else {
      stellar.destroyed_days_remaining = access;
      stellar.strength = -1;
    }
  }

  // Escort group-order code per active, non-player, squad-leading ship
  // (LoadSave 0x004cb260 applies g_target_category_command after the block2
  // restore). Ships with no scenario class are left untouched.
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.squad_leader_ship_slot != 0) {
      continue;
    }
    const ShipClass *ship_class = state.scenario.Ship(
        static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    if (ship_class == nullptr) {
      continue;
    }
    const std::int16_t category = ship_class->class_category;
    if (category < 0 || static_cast<std::size_t>(category) >=
                            state.target_category_command.size()) {
      continue;
    }
    ship.escort_command_code = state.target_category_command[category];
  }

  const std::size_t disaster_count =
      std::min(state.scenario.disaster_defs.size(),
               pilot_file.disaster_days_remaining.size());
  for (std::size_t i = 0; i < disaster_count; ++i) {
    auto &disaster = state.scenario.disaster_defs[i];
    if (!disaster.present) {
      disaster.days_remaining = 0;
      disaster.active_stellar = -1;
      disaster.started_once = false;
    } else {
      disaster.days_remaining = pilot_file.disaster_days_remaining[i];
      disaster.active_stellar = pilot_file.disaster_active_stellars[i];
    }
  }
  for (std::size_t i = 0; i < state.cron_event_states.size(); ++i) {
    auto &runtime = state.cron_event_states[i];
    if (i >= state.scenario.cron_events.size() ||
        !state.scenario.cron_events[i].present) {
      runtime = {};
      continue;
    }
    runtime.duration_counter = pilot_file.cron_duration_counters[i];
    runtime.holdoff_counter = pilot_file.cron_holdoff_counters[i];
    runtime.is_active =
        runtime.duration_counter >= 0 || runtime.holdoff_counter >= 0;
  }
  const std::size_t rank_count = std::min(state.scenario.ranks.size(),
                                          pilot_file.rank_active_flags.size());
  for (std::size_t i = 0; i < rank_count; ++i) {
    auto &rank = state.scenario.ranks[i];
    rank.active = rank.defined && pilot_file.rank_active_flags[i] != 0;
  }
}

PilotFile PilotFileCollectFromState(const GameState &state) {
  PilotFile out;
  out.pilot_name = state.pilot.first_name;
  out.nickname = state.pilot.last_name;
  out.ship_name = state.player.ship_name;

  out.credits = state.player.credits;
  out.player_combat_rating_points = state.player_combat_rating_points;
  out.ship_class_id = state.player.ship_class_id;
  out.current_system_id = state.player.current_system_id;
  out.date = state.date;
  out.date_prefix = state.date_prefix;
  out.date_suffix = state.date_suffix;
  out.ship_paint_rgb5 = state.ship_paint_rgb5;
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
  out.intro_text_desc_id = state.intro_cinematic.intro_text_desc_id;
  out.intro_played = state.intro_played;
  out.strict_play = state.pilot.strict_play;
  out.male = state.control.male;

  out.cargo_bins = state.inventory.cargo_bins;
  const std::size_t system_count =
      std::min(state.scenario.systems.size(), out.system_discovery.size());
  for (std::size_t i = 0; i < system_count; ++i) {
    out.system_discovery[i] = state.scenario.systems[i].discovery_state;
  }
  std::copy_n(
      state.system_reputation.begin(),
      std::min(state.system_reputation.size(), out.system_reputation.size()),
      out.system_reputation.begin());
  out.outfit_owned_count = state.inventory.outfit_owned_count;
  out.junk_counts = state.inventory.junk_counts;
  for (std::size_t i = 0; i < out.control_bits.size(); ++i) {
    const std::uint8_t persisted = state.control.persisted_bit_bytes[i];
    out.control_bits[i] = (persisted != 0) == state.control.bits.test(i)
                              ? persisted
                          : state.control.bits.test(i) ? 1
                                                       : 0;
  }
  out.stat_modifier_pct = state.player_stat_modifier_pct;
  out.weapon_count_by_class = state.weapon_count_by_class;
  out.weapon_secondary_count_by_class = state.weapon_secondary_count_by_class;
  out.active_mission_runtime_flags = state.active_mission_runtime_flags;
  out.active_missions = state.active_missions;
  const std::size_t pers_count =
      std::min(state.scenario.pers_defs.size(), out.pers_alive_flags.size());
  for (std::size_t i = 0; i < pers_count; ++i) {
    out.pers_alive_flags[i] = state.scenario.pers_defs[i].alive ? 1 : 0;
    out.pers_grudge_flags[i] = state.scenario.pers_defs[i].grudge ? 1 : 0;
  }
  out.reinforcement_retrigger_delay = state.reinforcement_retrigger_delay;
  out.target_category_command = state.target_category_command;
  out.escort_ship_class_ids.fill(-1);
  out.fighter_ship_class_ids.fill(-1);
  out.fighter_voice_types.fill(-1);
  std::size_t escort_row = 0;
  std::size_t fighter_row = 0;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.squad_leader_ship_slot != 0 ||
        ship.mission_fleet_slot >= 0) {
      continue;
    }
    if (ship.ai_behavior_code == 6 && escort_row < 0x40) {
      out.escort_ship_class_ids[escort_row] = static_cast<std::int16_t>(
          ship.ship_class_id + (ship.escort_origin_mark != 0 ? 1000 : 0));
      out.escort_upgrade_flags[escort_row] = ship.escort_upgrade_mark;
      out.escort_pending_sale_flags[escort_row] = ship.escort_pending_sale_mark;
      ++escort_row;
    } else if (ship.ai_behavior_code == 5 && fighter_row < 0x40) {
      out.fighter_ship_class_ids[fighter_row] = ship.ship_class_id;
      out.fighter_voice_types[fighter_row] = ship.voice_type_mode;
      ++fighter_row;
    }
  }
  const std::size_t stellar_count =
      std::min(state.scenario.stellars.size(), out.stellar_dominated.size());
  for (std::size_t i = 0; i < stellar_count; ++i) {
    const auto &stellar = state.scenario.stellars[i];
    out.stellar_dominated[i] = stellar.dominated;
    out.stellar_present_ship_counts[i] =
        static_cast<std::int16_t>(stellar.present_ship_count);
    out.stellar_domination_days[i] = stellar.domination_days;
    out.stellar_destroyed_days_remaining[i] = stellar.destroyed_days_remaining;
  }
  // Define the slots past the scenario's table with the loader's inactive
  // fallback so a save from a small scenario stays structurally valid.
  std::fill(out.stellar_destroyed_days_remaining.begin() + stellar_count,
            out.stellar_destroyed_days_remaining.end(),
            static_cast<std::int16_t>(-1));
  const std::size_t disaster_count = std::min(
      state.scenario.disaster_defs.size(), out.disaster_days_remaining.size());
  for (std::size_t i = 0; i < disaster_count; ++i) {
    out.disaster_days_remaining[i] =
        state.scenario.disaster_defs[i].days_remaining;
    out.disaster_active_stellars[i] =
        state.scenario.disaster_defs[i].active_stellar;
  }
  for (std::size_t i = 0; i < state.cron_event_states.size(); ++i) {
    out.cron_duration_counters[i] = state.cron_event_states[i].duration_counter;
    out.cron_holdoff_counters[i] = state.cron_event_states[i].holdoff_counter;
  }
  const std::size_t rank_count =
      std::min(state.scenario.ranks.size(), out.rank_active_flags.size());
  for (std::size_t i = 0; i < rank_count; ++i) {
    out.rank_active_flags[i] = state.scenario.ranks[i].active ? 1 : 0;
  }
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
  for (std::size_t i = 0; i < pilot_file.system_discovery.size(); ++i) {
    WriteU16(block1,
             0x1a + 2 * i,
             static_cast<std::uint16_t>(pilot_file.system_discovery[i]));
    WriteU16(block1,
             0x141a + 2 * i,
             static_cast<std::uint16_t>(pilot_file.system_reputation[i]));
  }
  for (std::size_t i = 0; i < pilot_file.outfit_owned_count.size(); ++i) {
    WriteU16(block1,
             0x101a + 2 * i,
             static_cast<std::uint16_t>(pilot_file.outfit_owned_count[i]));
  }
  // +0x241a/+0x261a weapon bank ammo/secondary: the original persists only the
  // first slot of each 100-slot bank (weapon_count_by_class[i*100]).
  for (std::size_t i = 0; i < 0x100; ++i) {
    WriteU16(
        block1,
        0x241a + 2 * i,
        static_cast<std::uint16_t>(pilot_file.weapon_count_by_class[i * 100]));
    WriteU16(block1,
             0x261a + 2 * i,
             static_cast<std::uint16_t>(
                 pilot_file.weapon_secondary_count_by_class[i * 100]));
  }
  WriteU32(block1, 0x281a, static_cast<std::uint32_t>(pilot_file.credits));
  WriteU32(block1,
           0xe94e,
           static_cast<std::uint32_t>(pilot_file.player_combat_rating_points));
  // Escort/fleet tables (block1 +0xe6ce..+0xe8ce).
  for (std::size_t i = 0; i < 0x40; ++i) {
    WriteU16(block1,
             0xe6ce + 2 * i,
             static_cast<std::uint16_t>(pilot_file.escort_ship_class_ids[i]));
    WriteU16(block1,
             0xe74e + 2 * i,
             static_cast<std::uint16_t>(pilot_file.fighter_ship_class_ids[i]));
    WriteU16(block1,
             0xe7ce + 2 * i,
             static_cast<std::uint16_t>(pilot_file.escort_upgrade_flags[i]));
    WriteU16(
        block1,
        0xe84e + 2 * i,
        static_cast<std::uint16_t>(pilot_file.escort_pending_sale_flags[i]));
    WriteU16(block1,
             0xe8ce + 2 * i,
             static_cast<std::uint16_t>(pilot_file.fighter_voice_types[i]));
  }
  for (std::size_t i = 0; i < pilot_file.control_bits.size(); ++i) {
    block1[0xb7be + i] = static_cast<std::byte>(pilot_file.control_bits[i]);
  }
  for (std::size_t i = 0; i < pilot_file.stellar_dominated.size(); ++i) {
    block1[0xdece + i] =
        static_cast<std::byte>(pilot_file.stellar_dominated[i]);
  }

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
    for (std::size_t i = 0; i < flags.deadline_time_components.size(); ++i) {
      WriteU16(block1,
               offset + 0x0c + i * sizeof(std::uint16_t),
               flags.deadline_time_components[i]);
    }
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
    put_i16(0x0a, mission.ship_goal);
    put_i16(0x0c, mission.ship_behavior);
    put_i16(0x0e, mission.ship_start);
    put_i16(0x10, mission.current_system_id);
    put_i16(0x12, mission.cargo_type_id);
    put_i16(0x14, mission.cargo_qty_tons);
    put_i16(0x16, mission.pickup_mode);
    put_i16(0x18, mission.drop_off_mode);
    put_i16(0x1a, mission.scan_mask);
    put_i16(0x1c, mission.comp_govt_id);
    put_i16(0x1e, mission.comp_reward_delta);
    put_i16(0x20, mission.on_resolve_repeat_count);
    WriteU32(block1,
             offset + 0x22,
             static_cast<std::uint32_t>(mission.resource_delta_or_cost));
    put_i16(0x26, mission.goal_counter_a);
    put_i16(0x28, mission.goal_counter_b);
    put_i16(0x2a, mission.goal_counter_c);
    put_i16(0x2c, mission.goal_count_remaining);
    put_i16(0x2e, mission.goal_counter_e);
    put_i16(0x30, mission.mission_target_count);
    block1[offset + 0x32] = mission.can_abort ? std::byte{1} : std::byte{0};
    block1[offset + 0x33] =
        mission.carrying_resources ? std::byte{1} : std::byte{0};
    put_i16(0x45, mission.time_limit_days_remaining);
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
    put_i16(0x65, mission.mission_fleet_metric_b);
    put_i16(0x67, mission.mission_fleet_metric_c);
    put_i16(0x69, mission.rearm_roll_clock);
    put_i16(0x6b, mission.mission_ship_count_active);
    const auto copy_payload = [&](std::size_t field, const auto &payload) {
      std::memcpy(
          block1.data() + offset + field, payload.data(), payload.size());
    };
    copy_payload(0x1ec, mission.on_accept_text);
    copy_payload(0x2eb, mission.on_refuse_text);
    copy_payload(0x3ea, mission.on_success_text);
    copy_payload(0x4e9, mission.on_failure_text);
    copy_payload(0x5e8, mission.on_abort_text);
    copy_payload(0x6e7, mission.on_ship_done_text);
  }

  // -- Block2 (FleetState/world state) field fills.
  WriteU16(block2, 0x00, static_cast<std::uint16_t>(kFleetBlockVersion));
  // +0x02 ongoing/new-pilot latch (g_strict_play) and +0x04 gender latch
  // (g_player_is_male). The original writes the male latch raw, but the loader
  // only accepts the exact value 1, so emit 0/1.
  WriteU16(block2, 0x02, pilot_file.strict_play ? 1 : 0);
  WriteU16(block2, 0x04, pilot_file.male ? 1 : 0);
  WriteU16(block2, 0x3086, pilot_file.intro_played ? 1 : 0);
  for (std::size_t i = 0; i < pilot_file.pers_alive_flags.size(); ++i) {
    WriteU16(block2,
             0x1006 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.pers_alive_flags[i]));
    WriteU16(block2,
             0x1806 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.pers_grudge_flags[i]));
  }
  for (std::size_t i = 0; i < pilot_file.stellar_present_ship_counts.size();
       ++i) {
    WriteU16(
        block2,
        0x0006 + 2 * i,
        static_cast<std::uint16_t>(pilot_file.stellar_present_ship_counts[i]));
    WriteU16(block2,
             0x2086 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.stellar_domination_days[i]));
  }
  for (std::size_t i = 0; i < pilot_file.disaster_days_remaining.size(); ++i) {
    WriteU16(block2,
             0x3088 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.disaster_days_remaining[i]));
    WriteU16(
        block2,
        0x3288 + 2 * i,
        static_cast<std::uint16_t>(pilot_file.disaster_active_stellars[i]));
  }
  for (std::size_t i = 0; i < pilot_file.cron_duration_counters.size(); ++i) {
    WriteU16(block2,
             0x3590 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.cron_duration_counters[i]));
    WriteU16(block2,
             0x3990 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.cron_holdoff_counters[i]));
  }
  for (std::size_t i = 0; i < pilot_file.rank_active_flags.size(); ++i) {
    WriteU16(block2,
             0x5dde + 2 * i,
             static_cast<std::uint16_t>(pilot_file.rank_active_flags[i]));
  }
  for (std::size_t i = 0; i < pilot_file.reinforcement_retrigger_delay.size();
       ++i) {
    WriteU16(block2,
             0x3d90 + 2 * i,
             static_cast<std::uint16_t>(
                 pilot_file.reinforcement_retrigger_delay[i]));
  }
  for (std::size_t i = 0;
       i < pilot_file.stellar_destroyed_days_remaining.size();
       ++i) {
    WriteU16(block2,
             0x4d90 + 2 * i,
             static_cast<std::uint16_t>(
                 pilot_file.stellar_destroyed_days_remaining[i]));
  }
  for (std::size_t i = 0; i < pilot_file.target_category_command.size(); ++i) {
    WriteU16(block2,
             0x5d90 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.target_category_command[i]));
  }
  for (std::size_t i = 0; i < pilot_file.junk_counts.size(); ++i) {
    WriteU16(block2,
             0x3488 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.junk_counts[i]));
  }
  // +0x3588..+0x358e stat modifier quartet (SaveGameCore 0x004c7dd0 writes
  // DAT_007353f6/f8/fa/fc here; LoadSave 0x004cb260 restores all four).
  for (std::size_t i = 0; i < pilot_file.stat_modifier_pct.size(); ++i) {
    WriteU16(block2,
             0x3588 + 2 * i,
             static_cast<std::uint16_t>(pilot_file.stat_modifier_pct[i]));
  }
  // +0x5d98 pilot nickname C-string, capped at 0x40 bytes like the original
  // CString_CopyBounded(&g_player_nickname, ..., 0x40).
  const std::size_t nick_len =
      std::min<std::size_t>(pilot_file.nickname.size(), 0x3f);
  if (nick_len > 0) {
    std::memcpy(block2.data() + 0x5d98, pilot_file.nickname.data(), nick_len);
  }
  block2[0x5d98 + nick_len] = std::byte{0};
  for (std::size_t i = 0; i < pilot_file.ship_paint_rgb5.size(); ++i) {
    WriteU16(block2, 0x5dd8 + 2 * i, pilot_file.ship_paint_rgb5[i]);
  }
  WriteBoundedCString(block2, 0x5ede, 0x10, pilot_file.date_prefix);
  WriteBoundedCString(block2, 0x5eee, 0x10, pilot_file.date_suffix);

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
  std::vector<std::byte> block1(bytes.begin() + 4, bytes.begin() + 4 + size1);
  const std::size_t size2_offset = 4 + size1;
  if (size2_offset + 4 > bytes.size()) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  const std::size_t size2 = ReadU32(bytes, size2_offset);
  if (size2 < 2 || size2_offset + 4 + size2 > bytes.size()) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  std::vector<std::byte> block2(bytes.begin() + size2_offset + 4,
                                bytes.begin() + size2_offset + 4 + size2);
  const std::span<const std::byte> trailer =
      bytes.subspan(size2_offset + 4 + size2);

  DecodePilotBlock(block2);
  // Converted classic-Mac pilots retain big-endian block payloads. Windows
  // pilots are little-endian. FleetState's version word distinguishes them.
  const bool big_endian = ReadU16(block2, 0) != kFleetBlockVersion &&
                          ReadU16(block2, 0, true) == kFleetBlockVersion;
  // TODO(decomp(0x008725b0)) skipped: compatibility divergence for converted
  // Mac pilots. The original Mac build reads this gate natively; after wrapping
  // its block in Windows framing, the ciphertext can resemble a little-endian
  // plaintext jump id (Alien.plt begins 0x00b3), so force the transform once
  // the FleetState byte order identifies a Mac payload.
  DecodePilotBlock(block1, big_endian);
  bool repairs = false;
  // The original reads fixed offsets without bounds checks (it assumes
  // full-size blocks from the same game build). To avoid UB on truncated
  // input, tracked fields are only restored when the block covers them.
  // The original reads block1 without bounds checks; the tracked tail now
  // includes the combat-rating word at +0xe94e, so require the full block.
  constexpr std::size_t kBlock1TrackedEnd = kBlock1Size;
  constexpr std::size_t kBlock2TrackedEnd = 0x5d98 + 0x40;
  if (block1.size() >= kBlock1TrackedEnd) {
    // jump destination stellar id.
    out.jump_dest_stellar =
        static_cast<std::int16_t>(ReadU16(block1, 0x00, big_endian));
    out.ship_class_id =
        static_cast<std::int16_t>(ReadU16(block1, 0x02, big_endian));
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
          static_cast<std::int16_t>(ReadU16(block1, 0x04 + 2 * i, big_endian));
    }
    // +0x10 shield: the original recomputes shield/armor from class+outfits
    // and does NOT read the stored value back (Ship_ComputeShipMaxShieldPoints
    // / Ship_ComputeShipMaxArmor). TODO(decomp): recompute-equivalent once the
    // outfit math is reconstructed; fuel is restored from +0x12.
    out.fuel_points = static_cast<float>(ReadU16(block1, 0x12, big_endian));
    // +0x14/+0x16/+0x18 month/day/year: the in-game calendar.
    out.date.month =
        static_cast<std::int16_t>(ReadU16(block1, 0x14, big_endian));
    out.date.day = static_cast<std::int16_t>(ReadU16(block1, 0x16, big_endian));
    out.date.year =
        static_cast<std::int16_t>(ReadU16(block1, 0x18, big_endian));
    for (std::size_t i = 0; i < out.system_discovery.size(); ++i) {
      out.system_discovery[i] =
          static_cast<std::int16_t>(ReadU16(block1, 0x1a + 2 * i, big_endian));
      out.system_reputation[i] = static_cast<std::int16_t>(
          ReadU16(block1, 0x141a + 2 * i, big_endian));
    }
    for (std::size_t i = 0; i < out.outfit_owned_count.size(); ++i) {
      out.outfit_owned_count[i] = static_cast<std::int16_t>(
          ReadU16(block1, 0x101a + 2 * i, big_endian));
      // Original zeroes outfits whose def no longer exists. TODO(decomp).
    }
    for (std::size_t i = 0; i < 0x100; ++i) {
      out.weapon_count_by_class[i * 100] = static_cast<std::int16_t>(
          ReadU16(block1, 0x241a + 2 * i, big_endian));
      out.weapon_secondary_count_by_class[i * 100] = static_cast<std::int16_t>(
          ReadU16(block1, 0x261a + 2 * i, big_endian));
      // Original zeroes banks whose weapon def no longer exists. TODO(decomp).
    }
    out.credits =
        static_cast<std::int32_t>(ReadU32(block1, 0x281a, big_endian));
    out.player_combat_rating_points =
        static_cast<std::int32_t>(ReadU32(block1, 0xe94e, big_endian));
    for (std::size_t i = 0; i < 0x40; ++i) {
      out.escort_ship_class_ids[i] = static_cast<std::int16_t>(
          ReadU16(block1, 0xe6ce + 2 * i, big_endian));
      out.fighter_ship_class_ids[i] = static_cast<std::int16_t>(
          ReadU16(block1, 0xe74e + 2 * i, big_endian));
      out.escort_upgrade_flags[i] = static_cast<std::int16_t>(
          ReadU16(block1, 0xe7ce + 2 * i, big_endian));
      out.escort_pending_sale_flags[i] = static_cast<std::int16_t>(
          ReadU16(block1, 0xe84e + 2 * i, big_endian));
      out.fighter_voice_types[i] = static_cast<std::int16_t>(
          ReadU16(block1, 0xe8ce + 2 * i, big_endian));
    }
    for (std::size_t i = 0; i < out.control_bits.size(); ++i) {
      out.control_bits[i] = std::to_integer<std::uint8_t>(block1[0xb7be + i]);
    }
    for (std::size_t i = 0; i < out.stellar_dominated.size(); ++i) {
      out.stellar_dominated[i] =
          std::to_integer<std::uint8_t>(block1[0xdece + i]);
    }
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
      flags.flags_primary_at_accept =
          ReadU16(block1, offset + 0x04, big_endian);
      flags.deadline_year =
          static_cast<std::int16_t>(ReadU16(block1, offset + 0x06, big_endian));
      flags.deadline_month =
          static_cast<std::int16_t>(ReadU16(block1, offset + 0x08, big_endian));
      flags.deadline_day =
          static_cast<std::int16_t>(ReadU16(block1, offset + 0x0a, big_endian));
      for (std::size_t i = 0; i < flags.deadline_time_components.size(); ++i) {
        flags.deadline_time_components[i] = ReadU16(
            block1, offset + 0x0c + i * sizeof(std::uint16_t), big_endian);
      }
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
        return static_cast<std::int16_t>(
            ReadU16(block1, offset + field, big_endian));
      };
      mission.travel_stellar_id = get_i16(0x00);
      mission.return_stellar_id = get_i16(0x04);
      mission.target_ship_count = get_i16(0x06);
      mission.dude_def_index = get_i16(0x08);
      mission.ship_goal = get_i16(0x0a);
      mission.ship_behavior = get_i16(0x0c);
      mission.ship_start = get_i16(0x0e);
      mission.current_system_id = get_i16(0x10);
      mission.cargo_type_id = get_i16(0x12);
      mission.cargo_qty_tons = get_i16(0x14);
      mission.pickup_mode = get_i16(0x16);
      mission.drop_off_mode = get_i16(0x18);
      mission.scan_mask = get_i16(0x1a);
      mission.comp_govt_id = get_i16(0x1c);
      mission.comp_reward_delta = get_i16(0x1e);
      mission.on_resolve_repeat_count = get_i16(0x20);
      mission.resource_delta_or_cost =
          static_cast<std::int32_t>(ReadU32(block1, offset + 0x22, big_endian));
      mission.goal_counter_a = get_i16(0x26);
      mission.goal_counter_b = get_i16(0x28);
      mission.goal_counter_c = get_i16(0x2a);
      mission.goal_count_remaining = get_i16(0x2c);
      mission.goal_counter_e = get_i16(0x2e);
      mission.mission_target_count = get_i16(0x30);
      mission.can_abort =
          std::to_integer<unsigned char>(block1[offset + 0x32]) != 0;
      mission.carrying_resources =
          std::to_integer<unsigned char>(block1[offset + 0x33]) != 0;
      mission.time_limit_days_remaining = get_i16(0x45);
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
      mission.flags_primary = ReadU16(block1, offset + 0x55, big_endian);
      mission.flags_secondary = ReadU16(block1, offset + 0x57, big_endian);
      mission.mission_ship_count_max = get_i16(0x61);
      mission.aux_ships_dude_def_index = get_i16(0x63);
      mission.mission_fleet_metric_b = get_i16(0x65);
      mission.mission_fleet_metric_c = get_i16(0x67);
      mission.rearm_roll_clock = get_i16(0x69);
      mission.mission_ship_count_active = get_i16(0x6b);
      const auto copy_payload = [&](auto &payload, std::size_t field) {
        std::memcpy(
            payload.data(), block1.data() + offset + field, payload.size());
      };
      copy_payload(mission.on_accept_text, 0x1ec);
      copy_payload(mission.on_refuse_text, 0x2eb);
      copy_payload(mission.on_success_text, 0x3ea);
      copy_payload(mission.on_failure_text, 0x4e9);
      copy_payload(mission.on_abort_text, 0x5e8);
      copy_payload(mission.on_ship_done_text, 0x6e7);
    }
  } else {
    NovaLog::Todo("pilot load: block1 too short (size {}); restore skipped",
                  block1.size());
  }

  // Block2 gate + FleetState magic checks (original: 0x6b -> -0x2d,
  // < 300 -> -0x2a).
  if (block2.size() < kBlock2TrackedEnd) {
    NovaLog::Todo("pilot load: block2 too short; restore skipped");
  } else {
    const std::int16_t magic =
        static_cast<std::int16_t>(ReadU16(block2, 0, big_endian));
    if (magic == kPrefsFileSignature) {
      return PilotLoadError::kWrongFileType;
    }
    if (magic < kFleetBlockVersion) {
      return PilotLoadError::kInvalidFleetBlock;
    }
    out.strict_play = ReadU16(block2, 0x02, big_endian) == 1;
    out.male = ReadU16(block2, 0x04, big_endian) == 1;
    out.intro_played = ReadU16(block2, 0x3086, big_endian) != 0;
    for (std::size_t i = 0; i < out.pers_alive_flags.size(); ++i) {
      out.pers_alive_flags[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x1006 + 2 * i, big_endian));
      out.pers_grudge_flags[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x1806 + 2 * i, big_endian));
    }
    for (std::size_t i = 0; i < out.stellar_present_ship_counts.size(); ++i) {
      out.stellar_present_ship_counts[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x0006 + 2 * i, big_endian));
      out.stellar_domination_days[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x2086 + 2 * i, big_endian));
    }
    for (std::size_t i = 0; i < out.disaster_days_remaining.size(); ++i) {
      out.disaster_days_remaining[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x3088 + 2 * i, big_endian));
      out.disaster_active_stellars[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x3288 + 2 * i, big_endian));
    }
    for (std::size_t i = 0; i < out.junk_counts.size(); ++i) {
      out.junk_counts[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x3488 + 2 * i, big_endian));
      // Original zeroes junk whose def no longer exists. TODO(decomp).
    }
    // +0x3588..+0x358e stat modifier quartet (LoadSave 0x004cb260 restores
    // DAT_007353f6/f8/fa/fc from here).
    for (std::size_t i = 0; i < out.stat_modifier_pct.size(); ++i) {
      out.stat_modifier_pct[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x3588 + 2 * i, big_endian));
    }
    for (std::size_t i = 0; i < out.cron_duration_counters.size(); ++i) {
      out.cron_duration_counters[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x3590 + 2 * i, big_endian));
      out.cron_holdoff_counters[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x3990 + 2 * i, big_endian));
    }
    for (std::size_t i = 0; i < out.rank_active_flags.size(); ++i) {
      out.rank_active_flags[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x5dde + 2 * i, big_endian));
    }
    for (std::size_t i = 0; i < out.reinforcement_retrigger_delay.size(); ++i) {
      out.reinforcement_retrigger_delay[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x3d90 + 2 * i, big_endian));
    }
    for (std::size_t i = 0; i < out.stellar_destroyed_days_remaining.size();
         ++i) {
      out.stellar_destroyed_days_remaining[i] = static_cast<std::int16_t>(
          ReadU16(block2, 0x4d90 + 2 * i, big_endian));
    }
    for (std::size_t i = 0; i < out.target_category_command.size(); ++i) {
      const auto saved = static_cast<std::int16_t>(
          ReadU16(block2, 0x5d90 + 2 * i, big_endian));
      out.target_category_command[i] = std::max<std::int16_t>(saved, -1);
    }
    for (std::size_t i = 0; i < out.ship_paint_rgb5.size(); ++i) {
      out.ship_paint_rgb5[i] = ReadU16(block2, 0x5dd8 + 2 * i, big_endian);
    }
    out.date_prefix = ReadBoundedCString(block2, 0x5ede, 0x0f);
    out.date_suffix = ReadBoundedCString(block2, 0x5eee, 0x0f);
    // TODO(decomp(0x004cb260)) skipped: compatibility divergence. Converted
    // pilots may carry a Pascal nickname independently of scalar byte order
    // (Archer (PC).plt is little-endian but still has a Pascal nickname).
    out.nickname.clear();
    const auto pascal_length = std::to_integer<unsigned char>(block2[0x5d98]);
    const bool pascal_nickname = pascal_length > 0 && pascal_length < 0x40 &&
                                 block2[0x5d99 + pascal_length] == std::byte{0};
    const std::size_t nickname_start = pascal_nickname ? 1 : 0;
    const std::size_t nickname_limit = pascal_nickname ? pascal_length : 0x40;
    for (std::size_t i = 0; i < nickname_limit; ++i) {
      const auto c = static_cast<char>(block2[0x5d98 + nickname_start + i]);
      if (c == '\0') {
        break;
      }
      out.nickname.push_back(c);
    }
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
  return RecordLastPilotPath(nova_files_dir, path);
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
  PilotLoadError result = PilotFileDeserialize(bytes, record);
  if (result != PilotLoadError::kOk &&
      result != PilotLoadError::kRepairsApplied) {
    return result;
  }

  // The original derives g_player_name (pilot name) from the .plt path:
  // substring after the last ':' (FUN_004d6150 = strrchr), then cut at the
  // first '.', dropping the ".plt" extension.
  std::string name = path.filename().string();
  if (const auto dot = name.find('.'); dot != std::string::npos) {
    name.resize(dot);
  }
  record.pilot_name = name;

  // Definition-aware inventory repairs. PilotFile_LoadSave clears positive
  // quantities whose scenario resource is absent and returns -0x2e while
  // still loading the rest of the pilot.
  for (std::size_t i = 0;
       !state.scenario.outfits.empty() && i < record.outfit_owned_count.size();
       ++i) {
    if (record.outfit_owned_count[i] > 0 &&
        (i >= state.scenario.outfits.size() ||
         state.scenario.outfits[i].name.empty())) {
      record.outfit_owned_count[i] = 0;
      result = PilotLoadError::kRepairsApplied;
    }
  }
  for (std::size_t i = 0; !state.scenario.weapons.empty() && i < 0x100; ++i) {
    const bool missing = i >= state.scenario.weapons.size() ||
                         state.scenario.weapons[i].name.empty();
    if (missing && (record.weapon_count_by_class[i * 100] > 0 ||
                    record.weapon_secondary_count_by_class[i * 100] > 0)) {
      record.weapon_count_by_class[i * 100] = 0;
      record.weapon_secondary_count_by_class[i * 100] = 0;
      result = PilotLoadError::kRepairsApplied;
    }
  }
  for (std::size_t i = 0;
       !state.scenario.junk_defs.empty() && i < record.junk_counts.size();
       ++i) {
    if (record.junk_counts[i] > 0 && (i >= state.scenario.junk_defs.size() ||
                                      !state.scenario.junk_defs[i].present)) {
      record.junk_counts[i] = 0;
      result = PilotLoadError::kRepairsApplied;
    }
  }
  for (std::size_t i = 0;
       !state.scenario.ranks.empty() && i < record.rank_active_flags.size();
       ++i) {
    if (record.rank_active_flags[i] != 0 &&
        (i >= state.scenario.ranks.size() ||
         !state.scenario.ranks[i].defined)) {
      record.rank_active_flags[i] = 0;
      result = PilotLoadError::kRepairsApplied;
    }
  }

  // The original repairs a class whose definition is absent, then reports
  // kRepairsApplied. ScenarioData stores only defined ship rows, so an
  // out-of-range resource lookup is the clean-room equivalent of its
  // tech_level == -9999 sentinel check.
  const auto *saved_class = state.scenario.Ship(
      static_cast<std::int16_t>(record.ship_class_id + 0x80));
  if (!state.scenario.ships.empty() &&
      (saved_class == nullptr ||
       saved_class->tech_level == kShipClassNonexistentTechLevel)) {
    const std::int16_t missing_class_id = record.ship_class_id;
    const auto first_defined =
        std::ranges::find_if(state.scenario.ships, [](const ShipClass &ship) {
          return ship.tech_level != kShipClassNonexistentTechLevel;
        });
    record.ship_class_id =
        first_defined == state.scenario.ships.end()
            ? 0
            : static_cast<std::int16_t>(
                  std::distance(state.scenario.ships.begin(), first_defined));
    const std::string_view fallback_name =
        first_defined == state.scenario.ships.end()
            ? std::string_view{"<none>"}
            : std::string_view{first_defined->display_name};
    NovaLog::Warn("pilot load: saved ship class {} (resource {:#x}) is not "
                  "defined; substituting class {} '{}' and reporting repairs "
                  "(an unavailable plugin is a likely cause)",
                  missing_class_id,
                  missing_class_id + 0x80,
                  record.ship_class_id,
                  fallback_name);
    result = PilotLoadError::kRepairsApplied;
  }
  // current_system_id is not serialized; the loader resolves it from the saved
  // jump destination stellar (Ghidra 0x004cb260 fallback chain).
  if (record.jump_dest_stellar < 0) {
    const auto fallback =
        std::ranges::find_if(state.scenario.stellars,
                             [](const Stellar &st) { return st.is_defined; });
    if (fallback != state.scenario.stellars.end()) {
      record.jump_dest_stellar = static_cast<std::int16_t>(
          std::distance(state.scenario.stellars.begin(), fallback));
    }
    result = PilotLoadError::kRepairsApplied;
  }
  record.current_system_id = [&]() -> std::int16_t {
    const auto valid_explored = [&](std::int16_t system_id) {
      return system_id >= 0 &&
             static_cast<std::size_t>(system_id) <
                 record.system_discovery.size() &&
             record.system_discovery[static_cast<std::size_t>(system_id)] != 0;
    };
    if (record.jump_dest_stellar >= 0) {
      const auto resource_id =
          static_cast<std::int16_t>(record.jump_dest_stellar + 0x80);
      if (const auto *st = state.scenario.Stellar(resource_id);
          st != nullptr && valid_explored(st->system_id)) {
        return st->system_id;
      }
      const auto containing = NovaTargeting_FindSystemContainingStellar(
          state.scenario, resource_id);
      if (valid_explored(containing)) {
        return containing;
      }
    }
    const auto explored = std::ranges::find_if(
        record.system_discovery, [](std::int16_t value) { return value != 0; });
    if (explored != record.system_discovery.end()) {
      return static_cast<std::int16_t>(
          std::distance(record.system_discovery.begin(), explored));
    }
    return 0;
  }();

  // Ghidra 0x004cb260 places a restored player at the saved destination
  // stellar and chooses a fresh heading with NovaRandom_Range(360). The port
  // stores headings in radians rather than the original integer degrees.
  if (record.jump_dest_stellar >= 0) {
    if (const auto *stellar =
            state.scenario.Stellar(record.jump_dest_stellar + 0x80)) {
      record.pos_x = static_cast<float>(stellar->pos_x);
      record.pos_y = static_cast<float>(stellar->pos_y);
      std::uniform_int_distribution<int> heading_degrees(0, 359);
      record.heading = static_cast<float>(heading_degrees(state.rng)) *
                       std::numbers::pi_v<float> / 180.0F;
    }
  }

  PilotFileApply(record, state);
  static_cast<void>(RecordLastPilotPath(path.parent_path(), path));

  // Recreate the two saved player-fleet classes after the persistent state is
  // live. Invalid class rows are skipped and make the successful load report
  // repairs, matching the original.
  if (!state.scenario.ships.empty()) {
    NovaShip_DeactivateVacantShipsAndTally(state, /*keep_player_engaged=*/true);
  }
  for (std::size_t i = 0; !state.scenario.ships.empty() && i < 0x40; ++i) {
    const auto restore_ship = [&](std::int16_t encoded_class,
                                  bool fighter) -> Ship * {
      if (encoded_class < 0) {
        return nullptr;
      }
      const std::int16_t ship_class =
          fighter ? encoded_class
                  : static_cast<std::int16_t>(encoded_class % 1000);
      const ShipClass *definition =
          state.scenario.Ship(static_cast<std::int16_t>(ship_class + 0x80));
      if (definition == nullptr ||
          definition->tech_level == kShipClassNonexistentTechLevel) {
        result = PilotLoadError::kRepairsApplied;
        return nullptr;
      }
      const int slot =
          NovaShipClass_SpawnEscortShipFromClass(state, ship_class, -1);
      return slot < 0 ? nullptr : &state.ShipAt(static_cast<std::size_t>(slot));
    };
    if (Ship *escort = restore_ship(record.escort_ship_class_ids[i], false)) {
      escort->escort_origin_mark =
          record.escort_ship_class_ids[i] >= 1000 ? 1 : 0;
      escort->escort_upgrade_mark =
          static_cast<std::int8_t>(record.escort_upgrade_flags[i] != 0);
      escort->escort_pending_sale_mark =
          static_cast<std::int8_t>(record.escort_pending_sale_flags[i] != 0);
    }
    if (Ship *fighter = restore_ship(record.fighter_ship_class_ids[i], true)) {
      fighter->ai_behavior_code = 5;
      fighter->escort_origin_mark = 0;
      NovaShip_ResetAiBehaviorRuntimeFields(*fighter);
      if (record.fighter_voice_types[i] != -1) {
        fighter->voice_type_mode = record.fighter_voice_types[i];
      }
    }
  }

  for (std::size_t i = 0; i < state.active_missions.size(); ++i) {
    if (!state.active_mission_runtime_flags[i].is_active) {
      continue;
    }
    const GameDate deadline = Mission_ComputeDateAfterSteps(
        state, state.active_missions[i].time_limit_days_remaining);
    state.active_mission_runtime_flags[i].deadline_year = deadline.year;
    state.active_mission_runtime_flags[i].deadline_month = deadline.month;
    state.active_mission_runtime_flags[i].deadline_day = deadline.day;
    ActiveMission &mission = state.active_missions[i];
    mission.mission_fleet_name.clear();
    mission.mission_text_name_b.clear();
    if (mission.special_ship_name_string_id > 0x7f &&
        mission.special_ship_name_entry > 0) {
      mission.mission_fleet_name =
          NovaHud_LoadStringEntry(
              static_cast<std::uint16_t>(mission.special_ship_name_string_id),
              static_cast<std::uint16_t>(mission.special_ship_name_entry))
              .value_or("")
              .substr(0, 0x3e);
    }
    if (mission.random_text_string_id > 0x7f && mission.random_text_entry > 0) {
      mission.mission_text_name_b =
          NovaHud_LoadStringEntry(
              static_cast<std::uint16_t>(mission.random_text_string_id),
              static_cast<std::uint16_t>(mission.random_text_entry))
              .value_or("")
              .substr(0, 0x3e);
    }
    if ((mission.flags_primary & 0x10U) != 0U) {
      mission.mission_ship_count_active = mission.mission_ship_count_max;
    }
    mission.rearm_roll_clock = static_cast<std::int16_t>(
        std::uniform_int_distribution<int>{0x46, 0x8b}(state.rng));
    mission.mission_fleet_metric_c = 0;
  }
  NovaOutfit_RecomputeOutfitDerivedState(state);
  const auto effective = Outfit_ComputePlayerEffectiveStats(state);
  state.player.shield_points = effective.max_shield_points;
  state.player.armor_points = effective.max_armor_points;
  // LoadSave performs this refresh twice around destination/system repair; at
  // this point the restored pilot state and final current system are both in
  // place, so this is the equivalent final pass.
  NovaResources_EvaluateAvailability(state);
  NovaSystem_RebuildDiscoveredLatch(state);
  if (state.player.current_system_id >= 0 &&
      static_cast<std::size_t>(state.player.current_system_id) <
          state.scenario.systems.size()) {
    const auto &system =
        state.scenario
            .systems[static_cast<std::size_t>(state.player.current_system_id)];
    state.starmap_pan_x = static_cast<float>(system.pos_x);
    state.starmap_pan_y = static_cast<float>(system.pos_y);
  }
  Ship &player = state.player;
  player.waypoint_arrival_marker_a = 0;
  player.turn_bank_animation_phase = 0.0F;
  if (const ShipClass *ship_class = state.scenario.Ship(
          static_cast<std::int16_t>(player.ship_class_id + 0x80))) {
    player.waypoint_arrival_marker_b =
        static_cast<std::int16_t>(ship_class->skill_variance_percent - 1);
  }
  player.shield_bubble_flash_intensity = 0.0F;
  player.weapon_sprite_flash_level = 0.0F;
  player.player_aggro_accumulator = 0.0F;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &escort = state.ShipAt(slot);
    if (!escort.is_active || escort.squad_leader_ship_slot != 0) {
      continue;
    }
    const ShipClass *escort_class = state.scenario.Ship(
        static_cast<std::int16_t>(escort.ship_class_id + 0x80));
    if (escort_class == nullptr ||
        (escort_class->sprite_behavior_flags & 0x80U) == 0U) {
      escort.waypoint_arrival_marker_b = 0;
    } else {
      escort.waypoint_arrival_marker_b = player.waypoint_arrival_marker_b;
    }
  }
  std::uniform_int_distribution<int> availability_roll(1, 100);
  for (std::size_t i = 0; i < state.ship_class_limit_rolls.size(); ++i) {
    state.ship_class_threshold_rolls[i] =
        static_cast<std::int16_t>(availability_roll(state.rng));
    state.ship_class_limit_rolls[i] =
        static_cast<std::int16_t>(availability_roll(state.rng));
  }
  for (std::size_t i = 0; i < state.outfit_stock_rolls.size(); ++i) {
    state.outfit_stock_rolls[i] =
        static_cast<std::int16_t>(availability_roll(state.rng));
  }
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
  return result;
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

PilotLoadError PilotData_AutoresumeLastPilot(GameState &state) {
  const auto save_directory = PilotFileSaveDirectory();
  if (!save_directory) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  const std::filesystem::path marker = *save_directory / kLastPilotMarkerName;
  if (!PilotFileProbeExists(marker)) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  std::ifstream file(marker, std::ios::binary);
  const std::string raw{std::istreambuf_iterator<char>(file),
                        std::istreambuf_iterator<char>()};
  if (raw.empty()) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  const std::string saved_path(raw.c_str(), strnlen(raw.c_str(), raw.size()));
  if (saved_path.empty()) {
    return PilotLoadError::kMissingOrEmptyFile;
  }
  std::filesystem::path path{saved_path};
  if (path.is_relative() && !PilotFileProbeExists(path)) {
    path = marker.parent_path() / path;
  }
  NovaShip_ResetPlayerShipState(state);
  return PilotFileLoadSave(path, state);
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
