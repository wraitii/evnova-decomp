#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/pilot_file.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using game::PilotFile;
using game::PilotFileApply;
using game::PilotFileCollectFromState;
using game::PilotFileDelete;
using game::PilotFileDeserialize;
using game::PilotFileLoadSave;
using game::PilotFileProbeExists;
using game::PilotFileSaveGame;
using game::PilotFileSerialize;
using game::PilotLoadError;

[[nodiscard]] PilotFile SampleRecord() {
  PilotFile p = PilotFile::Fresh();
  p.pilot_name = "Jacqueline";
  p.nickname = "Maclean";
  p.ship_name = "Vengeance";
  p.jump_dest_stellar = 0x123;
  p.credits = 123456;
  p.player_combat_rating_points = 987654;
  p.ship_class_id = 3;
  p.current_system_id = 7;
  p.active_weapon_bank_slot = 2;
  p.timed_action_counter = 99;
  p.death_timer_active = -0.5F;
  p.shield_points = 500.4F;
  p.armor_points = 300.2F;
  p.fuel_points = 77.6F;
  p.pos_x = 12.0F;
  p.pos_y = -34.0F;
  p.heading = 1.25F;
  p.intro_played = true;
  p.strict_play = true;
  p.male = false;
  p.cargo_bins = {3, 0, 5, 1, 0, 2};
  p.system_discovery[0] = 2;
  p.system_discovery[0x7ff] = 1;
  p.system_reputation[3] = -1234;
  p.system_reputation[0x7ff] = 5678;
  p.outfit_owned_count[0] = 1;
  p.outfit_owned_count[0x1ff] = -2;
  p.weapon_bank_ammo[5 * 100] = 7;
  p.weapon_bank_secondary[5 * 100] = 11;
  p.weapon_bank_ammo[0xff * 100 + 42] = 99; // non-first slot: not serialized
  p.junk_counts[0x7f] = 13;
  p.control_bits[42] = 1;
  p.control_bits[9999] = 0x8a;
  p.stellar_saved_bytes[7] = 0x5a;
  p.stellar_present_ship_counts[7] = 23;
  p.stellar_availability_rolls[7] = 81;
  p.stellar_engage_access[7] = 33;
  p.reinforcement_retrigger_delay[5] = 12;
  p.target_category_command = {1, -1, 2, 0};
  p.escort_ship_class_ids[0] = 1003;
  p.escort_upgrade_flags[0] = 1;
  p.escort_released_flags[0] = 1;
  p.fighter_ship_class_ids[0] = 4;
  p.fighter_voice_types[0] = 2;
  p.disaster_days_remaining[3] = 12;
  p.disaster_active_stellars[3] = 44;
  p.cron_duration_counters[5] = 9;
  p.cron_holdoff_counters[5] = -1;
  p.rank_active_flags[6] = 1;
  p.active_mission_runtime_flags[2].is_active = true;
  p.active_mission_runtime_flags[2].initial_briefing_done = true;
  p.active_mission_runtime_flags[2].deadline_day = 17;
  p.active_mission_runtime_flags[2].elapsed_travel_days = 1234;
  p.active_missions[2].carrying_resources = true;
  p.active_missions[2].mission_template_id = 0x91;
  p.active_missions[2].return_stellar_id = 0x123;
  p.active_missions[2].resource_delta_or_cost = -4567;
  p.active_missions[2].raw_payload[0x6e] = std::byte{0xa5};
  return p;
}

TEST_CASE("PilotFile .plt serialize/deserialize round-trips the tracked "
          "subset") {
  const PilotFile p = SampleRecord();
  const auto bytes = PilotFileSerialize(p, p.jump_dest_stellar);

  PilotFile out;
  const auto err = PilotFileDeserialize(bytes, out);
  REQUIRE(err == PilotLoadError::kOk);

  CHECK(out.nickname == "Maclean");
  CHECK(out.ship_name == "Vengeance");
  // pilot_name is NOT serialized: the original derives it from the file name
  // on load (the trailer carries the ship name).
  CHECK(out.jump_dest_stellar == 0x123);
  CHECK(out.credits == 123456);
  CHECK(out.player_combat_rating_points == 987654);
  CHECK(out.escort_ship_class_ids[0] == 1003);
  CHECK(out.escort_upgrade_flags[0] == 1);
  CHECK(out.escort_released_flags[0] == 1);
  CHECK(out.fighter_ship_class_ids[0] == 4);
  CHECK(out.fighter_voice_types[0] == 2);
  CHECK(out.ship_class_id == 3);
  CHECK(out.fuel_points == 77.0F); // 77.6 truncated toward zero, stored as u16
  CHECK(out.intro_played);
  CHECK(out.strict_play);
  CHECK_FALSE(out.male);
  CHECK(out.cargo_bins == p.cargo_bins);
  CHECK(out.system_discovery == p.system_discovery);
  CHECK(out.system_reputation == p.system_reputation);
  CHECK(out.outfit_owned_count[0] == 1);
  CHECK(out.outfit_owned_count[0x1ff] == -2);
  CHECK(out.weapon_bank_ammo[5 * 100] == 7);
  CHECK(out.weapon_bank_secondary[5 * 100] == 11);
  // Non-first bank slots are not persisted by the original format.
  CHECK(out.weapon_bank_ammo[0xff * 100 + 42] == 0);
  CHECK(out.junk_counts[0x7f] == 13);
  CHECK(out.control_bits == p.control_bits);
  CHECK(out.stellar_saved_bytes[7] == 0x5a);
  CHECK(out.stellar_present_ship_counts[7] == 23);
  CHECK(out.stellar_availability_rolls[7] == 81);
  CHECK(out.stellar_engage_access[7] == 33);
  CHECK(out.reinforcement_retrigger_delay[5] == 12);
  CHECK(out.target_category_command == p.target_category_command);
  CHECK(out.disaster_days_remaining[3] == 12);
  CHECK(out.disaster_active_stellars[3] == 44);
  CHECK(out.cron_duration_counters[5] == 9);
  CHECK(out.cron_holdoff_counters[5] == -1);
  CHECK(out.rank_active_flags[6] == 1);
  CHECK(out.active_mission_runtime_flags[2].is_active);
  CHECK(out.active_mission_runtime_flags[2].initial_briefing_done);
  CHECK(out.active_mission_runtime_flags[2].deadline_day == 17);
  CHECK(out.active_mission_runtime_flags[2].elapsed_travel_days == 1234);
  CHECK(out.active_missions[2].carrying_resources);
  CHECK(out.active_missions[2].mission_template_id == 0x91);
  CHECK(out.active_missions[2].return_stellar_id == 0x123);
  CHECK(out.active_missions[2].resource_delta_or_cost == -4567);
  CHECK(out.active_missions[2].raw_payload[0x6e] == std::byte{0xa5});
}

TEST_CASE("PilotFile applies persistent system state to live scenario rows") {
  PilotFile pilot = SampleRecord();
  game::GameState state;
  state.scenario.systems.resize(4);
  state.scenario.stellars.resize(8);
  state.scenario.stellars[6].is_defined = true;
  state.scenario.stellars[7].is_defined = true;
  pilot.stellar_present_ship_counts[6] = 17;
  pilot.stellar_availability_rolls[6] = 62;
  pilot.stellar_engage_access[6] = 17;
  pilot.stellar_engage_access[7] = -1;
  state.scenario.stellars[6].strength_capacity = 250;
  state.scenario.stellars[7].strength_capacity = 400;
  // One active, squad-leading escort for the group-order restore loop.
  state.scenario.ships.resize(1);
  state.scenario.ships[0].class_category = 2;
  state.ShipAt(1).is_active = true;
  state.ShipAt(1).squad_leader_ship_slot = 0;
  state.ShipAt(1).ship_class_id = 0;
  state.scenario.disaster_defs.resize(4);
  state.scenario.disaster_defs[3].present = true;
  state.scenario.cron_events.resize(6);
  state.scenario.cron_events[5].present = true;
  state.scenario.ranks.resize(7);
  state.scenario.ranks[6].defined = true;

  PilotFileApply(pilot, state);

  CHECK(state.scenario.systems[0].discovery_state == 2);
  CHECK(state.scenario.systems[1].discovery_state == 0);
  REQUIRE(state.system_reputation.size() == 4);
  CHECK(state.system_reputation[3] == -1234);
  CHECK(state.control.ControlBit(42));
  CHECK(state.control.ControlBit(9999));
  CHECK(state.control.persisted_bit_bytes[9999] == 0x8a);
  CHECK(state.scenario.stellars[6].present_ship_count == 17);
  CHECK(state.scenario.stellars[6].availability_roll == 62);
  CHECK(state.scenario.stellars[7].persistent_state_byte == 0x5a);
  CHECK(state.scenario.stellars[7].present_ship_count == 0);
  CHECK(state.scenario.stellars[7].availability_roll == 0);
  CHECK(state.scenario.stellars[6].engage_access == 17);
  CHECK(state.scenario.stellars[6].strength == -1);
  CHECK(state.scenario.stellars[7].engage_access == -1);
  CHECK(state.scenario.stellars[7].strength == 400);
  CHECK(state.reinforcement_retrigger_delay[5] == 12);
  CHECK(state.target_category_command[2] == 2);
  CHECK(state.ShipAt(1).escort_command_code == 2);
  CHECK(state.pilot.strict_play);
  CHECK_FALSE(state.control.male);
  CHECK(state.scenario.disaster_defs[3].days_remaining == 12);
  CHECK(state.scenario.disaster_defs[3].active_stellar == 44);
  CHECK(state.cron_event_states[5].is_active);
  CHECK(state.cron_event_states[5].duration_counter == 9);
  CHECK(state.cron_event_states[5].holdoff_counter == -1);
  CHECK(state.scenario.ranks[6].active);

  const PilotFile collected = PilotFileCollectFromState(state);
  CHECK(collected.system_discovery[0] == 2);
  CHECK(collected.system_reputation[3] == -1234);
  CHECK(collected.control_bits[42] == 1);
  CHECK(collected.control_bits[9999] == 0x8a);
  CHECK(collected.stellar_present_ship_counts[6] == 17);
  CHECK(collected.stellar_engage_access[6] == 17);
  CHECK(collected.stellar_engage_access[7] == -1);
  CHECK(collected.reinforcement_retrigger_delay[5] == 12);
  CHECK(collected.target_category_command == pilot.target_category_command);
  CHECK(collected.strict_play);
  CHECK_FALSE(collected.male);
  CHECK(collected.disaster_days_remaining[3] == 12);
  CHECK(collected.cron_duration_counters[5] == 9);
  CHECK(collected.rank_active_flags[6] == 1);
}

TEST_CASE("PilotFile normalizes invalid negative escort commands") {
  PilotFile pilot = PilotFile::Fresh();
  pilot.target_category_command = {-2, -1, 0, 3};

  const auto bytes = PilotFileSerialize(pilot, 0);
  PilotFile decoded;
  REQUIRE(PilotFileDeserialize(bytes, decoded) == PilotLoadError::kOk);
  CHECK((decoded.target_category_command ==
         std::array<std::int16_t, 4>{-1, -1, 0, 3}));

  game::GameState state;
  PilotFileApply(decoded, state);
  CHECK((state.target_category_command ==
         std::array<std::int16_t, 4>{-1, -1, 0, 3}));
}

TEST_CASE("PilotFile .plt round-trips through a real file and derives the "
          "pilot name from the path") {
  const auto dir = std::filesystem::temp_directory_path() / "evnova_pilot_test";
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / "Test Pilot.plt";
  std::filesystem::remove(path);

  PilotFile p = SampleRecord();
  p.pilot_name = "Test Pilot";

  // Save from a live state, then load back into a fresh state.
  game::GameState saved_state;
  saved_state.pilot.first_name = p.pilot_name;
  saved_state.pilot.last_name = p.nickname;
  saved_state.player.ship_name = p.ship_name;
  saved_state.player.credits = p.credits;
  saved_state.player.ship_class_id = p.ship_class_id;
  saved_state.player.fuel_points = p.fuel_points;
  saved_state.player_combat_rating_points = p.player_combat_rating_points;
  saved_state.inventory.outfit_owned_count[0] = 1;
  saved_state.weapon_bank_ammo[5 * 100] = 7;
  REQUIRE(PilotFileProbeExists(path) == false);
  REQUIRE(PilotFileSaveGame(dir, saved_state, p.jump_dest_stellar));
  CHECK(PilotFileProbeExists(path) == true);

  game::GameState loaded_state;
  const auto err = PilotFileLoadSave(path, loaded_state);
  CHECK(err == PilotLoadError::kOk);
  CHECK(loaded_state.pilot.first_name == "Test Pilot");
  CHECK(loaded_state.pilot.last_name == "Maclean");
  CHECK(loaded_state.player.credits == p.credits);
  CHECK(loaded_state.player.ship_class_id == p.ship_class_id);
  CHECK(loaded_state.player_combat_rating_points ==
        p.player_combat_rating_points);
  // Fuel is stored as a truncated u16 in the file (block1+0x12).
  CHECK(loaded_state.player.fuel_points == 77.0F);
  CHECK(loaded_state.inventory.outfit_owned_count[0] == 1);
  CHECK(loaded_state.weapon_bank_ammo[5 * 100] == 7);

  PilotFileDelete(path);
  CHECK(PilotFileProbeExists(path) == false);
}

TEST_CASE("PilotFile load rejects missing/empty saves") {
  const auto dir = std::filesystem::temp_directory_path() / "evnova_pilot_test";
  const std::filesystem::path missing = dir / "no such pilot.plt";
  std::filesystem::remove(missing);
  game::GameState state;
  CHECK(PilotFileLoadSave(missing, state) ==
        PilotLoadError::kMissingOrEmptyFile);

  // A file with only the size headers (empty blocks) is also missing-data.
  const auto empty = dir / "empty.plt";
  {
    std::ofstream f(empty, std::ios::binary);
    const std::array<std::byte, 8> zeros{};
    f.write(reinterpret_cast<const char *>(zeros.data()),
            static_cast<std::streamsize>(zeros.size()));
  }
  CHECK(PilotFileLoadSave(empty, state) == PilotLoadError::kMissingOrEmptyFile);
  std::filesystem::remove(empty);
}

TEST_CASE("PilotFile load rejects wrong FleetState magic") {
  const PilotFile p = SampleRecord();
  auto bytes = PilotFileSerialize(p, p.jump_dest_stellar);
  // Block2 starts after [u32 size][block1 data][u32 size]; its first u16 is
  // the magic.
  const std::size_t block2_offset = 4 + 0xe952 + 4;
  REQUIRE(bytes.size() >= block2_offset + 2);
  // Version 299 -> invalid fleet block.
  bytes[block2_offset] = std::byte{0x2b};
  bytes[block2_offset + 1] = std::byte{0x01};
  PilotFile out;
  CHECK(PilotFileDeserialize(bytes, out) == PilotLoadError::kInvalidFleetBlock);
  // 0x6b -> looks like a prefs file.
  bytes[block2_offset] = std::byte{0x6b};
  bytes[block2_offset + 1] = std::byte{0x00};
  CHECK(PilotFileDeserialize(bytes, out) == PilotLoadError::kWrongFileType);
}

TEST_CASE("PilotFile decodes an obfuscated pilot block") {
  const PilotFile p = SampleRecord();
  auto bytes = PilotFileSerialize(p, p.jump_dest_stellar);
  std::uint32_t key = 0xb36a210f;
  for (std::size_t offset = 4; offset < 4 + 0xe952;) {
    for (unsigned byte = 0; byte < 4 && offset < 4 + 0xe952; ++byte, ++offset) {
      bytes[offset] ^=
          static_cast<std::byte>((key >> (24U - byte * 8U)) & 0xffU);
    }
    key = (key + 0xdeadbeefU) ^ 0xdeadbeefU;
  }
  PilotFile out;
  const auto err = PilotFileDeserialize(bytes, out);
  CHECK(err == PilotLoadError::kOk);
  CHECK(out.ship_class_id == p.ship_class_id);
  CHECK(out.credits == p.credits);
  CHECK(out.date.year == p.date.year);
}

TEST_CASE("archived pilot fixtures have recognizable .plt framing",
          "[pilot-fixtures]") {
  const std::filesystem::path fixture_dir = "docs/assets/pilots";
  if (!std::filesystem::is_directory(fixture_dir)) {
    SKIP("optional docs/assets/pilots fixtures are not installed");
  }

  struct FixtureExpectation {
    std::string_view file;
    std::string_view nickname;
    std::int16_t year;
    std::int16_t month;
    std::int16_t day;
    std::int32_t credits;
    std::int32_t combat_rating;
    std::int16_t ship_class;
    std::int16_t jump_stellar;
    std::size_t discovered_systems;
  };

  constexpr std::array expectations{
      FixtureExpectation{
          "Alien.plt", "Dark Knight", 1178, 10, 6, 1296635129, -1, 0, 106, 537},
      FixtureExpectation{"Archer (PC).plt",
                         "Archer",
                         1178,
                         8,
                         31,
                         519967550,
                         352715,
                         37,
                         0,
                         537},
      FixtureExpectation{
          "Hunter.plt", "Maverick", 1177, 11, 22, 48522725, -1, 15, 0, 534},
      FixtureExpectation{"Pirate Hunter.plt",
                         "Hunter",
                         1177,
                         7,
                         28,
                         1075610999,
                         -1,
                         252,
                         280,
                         534},
      FixtureExpectation{
          "Plank.plt", "Planky", 1185, 10, 23, 12251587, -1, 163, 169, 458},
      FixtureExpectation{"Rick Hunter.plt",
                         "Pirate Hunter",
                         1177,
                         8,
                         8,
                         2522959,
                         -1,
                         252,
                         232,
                         535},
  };

  std::size_t fixture_count = 0;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(fixture_dir)) {
    std::string extension = entry.path().extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (!entry.is_regular_file() || extension != ".plt") {
      continue;
    }
    ++fixture_count;
    CAPTURE(entry.path());
    const auto expected = std::ranges::find_if(
        expectations, [&](const FixtureExpectation &candidate) {
          return candidate.file == entry.path().filename().string();
        });
    REQUIRE(expected != expectations.end());
    std::ifstream stream(entry.path(), std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    PilotFile record;
    const auto result = PilotFileDeserialize(bytes, record);
    CHECK((result == PilotLoadError::kOk ||
           result == PilotLoadError::kRepairsApplied));
    game::GameState state;
    CHECK(PilotFileLoadSave(entry.path(), state) == result);
    CHECK(state.pilot.first_name ==
          expected->file.substr(0, expected->file.size() - 4));
    CHECK(state.pilot.last_name == expected->nickname);
    CHECK_FALSE(state.pilot.strict_play);
    CHECK(state.control.male);
    CHECK(record.nickname == expected->nickname);
    CHECK(record.date.year == expected->year);
    CHECK(record.date.month == expected->month);
    CHECK(record.date.day == expected->day);
    CHECK(record.credits == expected->credits);
    CHECK(record.player_combat_rating_points == expected->combat_rating);
    CHECK(record.ship_class_id == expected->ship_class);
    CHECK(record.jump_dest_stellar == expected->jump_stellar);
    CHECK(static_cast<std::size_t>(std::ranges::count_if(
              record.system_discovery, [](std::int16_t value) {
                return value != 0;
              })) == expected->discovered_systems);
    // Archer's class 37 belongs to its plugin set. Raw decoding must retain
    // the exact id; a scenario-aware load may repair it without those plugins.
  }
  if (fixture_count == 0) {
    SKIP("no optional .plt fixtures found in docs/assets/pilots");
  }
}

} // namespace
