#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/new_pilot_flow.hpp"
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
  p.date_prefix = "Before ";
  p.date_suffix = " A.G.";
  p.ship_paint_rgb5 = {3, 17, 29};
  p.pers_alive_flags[3] = 1;
  p.pers_grudge_flags[3] = 1;
  p.cargo_bins = {3, 0, 5, 1, 0, 2};
  p.system_discovery[0] = 2;
  p.system_discovery[0x7ff] = 1;
  p.system_reputation[3] = -1234;
  p.system_reputation[0x7ff] = 5678;
  p.outfit_owned_count[0] = 1;
  p.outfit_owned_count[0x1ff] = -2;
  p.weapon_count_by_class[5 * 100] = 7;
  p.weapon_secondary_count_by_class[5 * 100] = 11;
  p.weapon_count_by_class[0xff * 100 + 42] =
      99; // non-first slot: not serialized
  p.junk_counts[0x7f] = 13;
  p.control_bits[42] = 1;
  p.control_bits[9999] = 0x8a;
  p.stellar_dominated[7] = 0x5a;
  p.stellar_present_ship_counts[7] = 23;
  p.stellar_domination_days[7] = 81;
  p.stellar_destroyed_days_remaining[7] = 33;
  p.reinforcement_retrigger_delay[5] = 12;
  p.target_category_command = {1, -1, 2, 0};
  p.escort_ship_class_ids[0] = 1003;
  p.escort_upgrade_flags[0] = 1;
  p.escort_pending_sale_flags[0] = 1;
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
  CHECK(out.escort_pending_sale_flags[0] == 1);
  CHECK(out.fighter_ship_class_ids[0] == 4);
  CHECK(out.fighter_voice_types[0] == 2);
  CHECK(out.ship_class_id == 3);
  CHECK(out.fuel_points == 77.0F); // 77.6 truncated toward zero, stored as u16
  CHECK(out.intro_played);
  CHECK(out.strict_play);
  CHECK_FALSE(out.male);
  CHECK(out.date_prefix == "Before ");
  CHECK(out.date_suffix == " A.G.");
  CHECK(out.ship_paint_rgb5 == p.ship_paint_rgb5);
  CHECK(out.pers_alive_flags[3] == 1);
  CHECK(out.pers_grudge_flags[3] == 1);
  CHECK(out.cargo_bins == p.cargo_bins);
  CHECK(out.system_discovery == p.system_discovery);
  CHECK(out.system_reputation == p.system_reputation);
  CHECK(out.outfit_owned_count[0] == 1);
  CHECK(out.outfit_owned_count[0x1ff] == -2);
  CHECK(out.weapon_count_by_class[5 * 100] == 7);
  CHECK(out.weapon_secondary_count_by_class[5 * 100] == 11);
  // Non-first bank slots are not persisted by the original format.
  CHECK(out.weapon_count_by_class[0xff * 100 + 42] == 0);
  CHECK(out.junk_counts[0x7f] == 13);
  CHECK(out.control_bits == p.control_bits);
  CHECK(out.stellar_dominated[7] == 0x5a);
  CHECK(out.stellar_present_ship_counts[7] == 23);
  CHECK(out.stellar_domination_days[7] == 81);
  CHECK(out.stellar_destroyed_days_remaining[7] == 33);
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
  pilot.stellar_domination_days[6] = 62;
  pilot.stellar_destroyed_days_remaining[6] = 17;
  pilot.stellar_destroyed_days_remaining[7] = -1;
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
  state.scenario.pers_defs.resize(4);
  state.scenario.pers_defs[3].ai_behavior_code = 2;
  state.scenario.pers_defs[3].loaded_latch = true;

  PilotFileApply(pilot, state);

  CHECK(state.scenario.systems[0].discovery_state == 2);
  CHECK(state.scenario.systems[1].discovery_state == 0);
  REQUIRE(state.system_reputation.size() == 4);
  CHECK(state.system_reputation[3] == -1234);
  CHECK(state.control.ControlBit(42));
  CHECK(state.control.ControlBit(9999));
  CHECK(state.control.persisted_bit_bytes[9999] == 0x8a);
  CHECK(state.scenario.stellars[6].present_ship_count == 17);
  CHECK(state.scenario.stellars[6].domination_days == 62);
  CHECK(state.scenario.stellars[7].dominated == 0x5a);
  CHECK(state.scenario.stellars[7].present_ship_count == 0);
  CHECK(state.scenario.stellars[7].domination_days == 0);
  CHECK(state.scenario.stellars[6].destroyed_days_remaining == 17);
  CHECK(state.scenario.stellars[6].strength == -1);
  CHECK(state.scenario.stellars[7].destroyed_days_remaining == -1);
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
  CHECK(state.scenario.pers_defs[3].alive);
  CHECK(state.scenario.pers_defs[3].grudge);
  CHECK(state.date_prefix == "Before ");
  CHECK(state.date_suffix == " A.G.");
  CHECK(state.ship_paint_rgb5 == pilot.ship_paint_rgb5);

  const PilotFile collected = PilotFileCollectFromState(state);
  CHECK(collected.system_discovery[0] == 2);
  CHECK(collected.system_reputation[3] == -1234);
  CHECK(collected.control_bits[42] == 1);
  CHECK(collected.control_bits[9999] == 0x8a);
  CHECK(collected.stellar_present_ship_counts[6] == 17);
  CHECK(collected.stellar_destroyed_days_remaining[6] == 17);
  CHECK(collected.stellar_destroyed_days_remaining[7] == -1);
  CHECK(collected.reinforcement_retrigger_delay[5] == 12);
  CHECK(collected.target_category_command == pilot.target_category_command);
  CHECK(collected.strict_play);
  CHECK_FALSE(collected.male);
  CHECK(collected.disaster_days_remaining[3] == 12);
  CHECK(collected.cron_duration_counters[5] == 9);
  CHECK(collected.rank_active_flags[6] == 1);
  CHECK(collected.pers_alive_flags[3] == 1);
  CHECK(collected.pers_grudge_flags[3] == 1);
}

TEST_CASE("new-pilot record seeding preserves loader-marked personalities") {
  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  // Tutorial 006's derelict target: përs resource 642, slot 642 - 0x80.
  constexpr std::size_t kTutorialDerelictPers = 642 - 0x80;
  REQUIRE(state.scenario.pers_defs.size() > kTutorialDerelictPers);
  REQUIRE(state.scenario.pers_defs[kTutorialDerelictPers].alive);

  // A fresh record's pers flags are zero; the new-pilot flow must project the
  // loader-marked table before PilotFileApply or every personality is cleared.
  PilotFile record = PilotFile::Fresh();
  game::PilotFileSeedPersonalityPresence(state.scenario, record);
  PilotFileApply(record, state);

  CHECK(state.scenario.pers_defs[kTutorialDerelictPers].alive);
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
  saved_state.weapon_count_by_class[5 * 100] = 7;
  saved_state.active_mission_runtime_flags[0].is_active = true;
  saved_state.active_missions[0].special_ship_name_string_id = 0x89;
  saved_state.active_missions[0].special_ship_name_entry = 1;
  saved_state.active_missions[0].random_text_string_id = 0x89;
  saved_state.active_missions[0].random_text_entry = 2;
  saved_state.active_missions[0].flags_primary = 0x10;
  saved_state.active_missions[0].mission_ship_count_max = 7;
  REQUIRE(PilotFileProbeExists(path) == false);
  REQUIRE(PilotFileSaveGame(dir, saved_state, p.jump_dest_stellar));
  CHECK(PilotFileProbeExists(path) == true);
  const auto marker_path = dir / "Last Pilot";
  REQUIRE(std::filesystem::is_regular_file(marker_path));
  {
    std::ifstream marker(marker_path, std::ios::binary);
    const std::string marker_value{std::istreambuf_iterator<char>(marker),
                                   std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(marker_value.empty());
    CHECK(std::string_view(marker_value.c_str()) == path.string());
  }

  game::GameState loaded_state;
  loaded_state.scenario.ships.resize(4);
  loaded_state.scenario.ships[3].base_shield = 500;
  loaded_state.scenario.ships[3].base_armor = 300;
  loaded_state.scenario.ships[3].base_fuel = 100;
  loaded_state.player.is_active = false;
  loaded_state.player.death_timer_active = 12.0F;
  loaded_state.player.death_timer_seeded = true;
  loaded_state.player.destruction_visual_triggered = true;
  loaded_state.player.destruction_finale_triggered = true;
  loaded_state.game_over_pending = true;
  loaded_state.return_to_menu_pending = true;
  game::NovaShip_ResetPlayerShipState(loaded_state);
  const auto err = PilotFileLoadSave(path, loaded_state);
  CHECK(err == PilotLoadError::kOk);
  CHECK(loaded_state.pilot.first_name == "Test Pilot");
  CHECK(loaded_state.pilot.last_name == "Maclean");
  CHECK(loaded_state.player.credits == p.credits);
  CHECK(loaded_state.player.ship_class_id == p.ship_class_id);
  CHECK(loaded_state.player.is_active);
  CHECK(loaded_state.player.death_timer_active == -1.0F);
  CHECK_FALSE(loaded_state.player.death_timer_seeded);
  CHECK_FALSE(loaded_state.player.destruction_visual_triggered);
  CHECK_FALSE(loaded_state.player.destruction_finale_triggered);
  CHECK(loaded_state.player.armor_points == 300.0F);
  CHECK_FALSE(loaded_state.game_over_pending);
  CHECK_FALSE(loaded_state.return_to_menu_pending);
  CHECK(loaded_state.player_combat_rating_points ==
        p.player_combat_rating_points);
  // Fuel is stored as a truncated u16 in the file (block1+0x12).
  CHECK(loaded_state.player.fuel_points == 77.0F);
  CHECK(loaded_state.inventory.outfit_owned_count[0] == 1);
  CHECK(loaded_state.weapon_count_by_class[5 * 100] == 7);
  CHECK_FALSE(loaded_state.active_missions[0].mission_fleet_name.empty());
  CHECK_FALSE(loaded_state.active_missions[0].mission_text_name_b.empty());
  CHECK(loaded_state.active_missions[0].mission_ship_count_active == 7);
  CHECK(loaded_state.active_missions[0].mission_fleet_metric_c == 0);
  CHECK(loaded_state.active_missions[0].rearm_roll_clock >= 0x46);
  CHECK(loaded_state.active_missions[0].rearm_roll_clock <= 0x8b);

  PilotFileDelete(path);
  std::filesystem::remove(marker_path);
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
    std::string_view ship_name;
    std::int16_t year;
    std::int16_t month;
    std::int16_t day;
    std::int32_t credits;
    std::int32_t combat_rating;
    std::int16_t ship_class;
    std::int16_t jump_stellar;
    std::size_t discovered_systems;
    // Active rank slots recovered from block2 +0x5dde (0-based rank id).
    std::vector<std::int16_t> active_ranks;
  };

  const std::array expectations{
      FixtureExpectation{"Alien.plt",
                         "Dark Knight",
                         "Vell-os Javelin",
                         1178,
                         10,
                         6,
                         1296635129,
                         34915,
                         0,
                         106,
                         537,
                         {1, 2, 8, 10, 13, 17, 19}},
      FixtureExpectation{"Archer (PC).plt",
                         "Archer",
                         "Serenity",
                         1178,
                         8,
                         31,
                         519967550,
                         352715,
                         37,
                         0,
                         537,
                         {10, 17, 18, 19, 21, 23}},
      FixtureExpectation{"Hunter.plt",
                         "Maverick",
                         "Fed Carrier ",
                         1177,
                         11,
                         22,
                         48522725,
                         31979,
                         15,
                         0,
                         534,
                         {17, 19}},
      FixtureExpectation{"Pirate Hunter.plt",
                         "Hunter",
                         "Aurora Thunderforge ",
                         1177,
                         7,
                         28,
                         1075610999,
                         0,
                         252,
                         280,
                         534,
                         {}},
      FixtureExpectation{"Plank.plt",
                         "Planky",
                         "Vengence Reaper",
                         1185,
                         10,
                         23,
                         12251587,
                         25251,
                         163,
                         169,
                         458,
                         {8, 17}},
      FixtureExpectation{"Rick Hunter.plt",
                         "Pirate Hunter",
                         "Pirate Hunter II",
                         1177,
                         8,
                         8,
                         2522959,
                         0,
                         252,
                         232,
                         535,
                         {}},
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
    // The .plt restore only latches rank slots whose definition is present
    // (the repair loop zeroes flags for absent records), so give the load a
    // full defined table to exercise the active-flag apply path.
    state.scenario.ranks.assign(0x80, {});
    for (auto &rank : state.scenario.ranks) {
      rank.defined = true;
    }
    CHECK(PilotFileLoadSave(entry.path(), state) == result);
    for (std::size_t slot = 0; slot < record.rank_active_flags.size(); ++slot) {
      const bool expected_active =
          std::ranges::find(expected->active_ranks,
                            static_cast<std::int16_t>(slot)) !=
          expected->active_ranks.end();
      CHECK((record.rank_active_flags[slot] != 0) == expected_active);
      CHECK(state.scenario.ranks[slot].active == expected_active);
    }
    CHECK(state.pilot.first_name ==
          expected->file.substr(0, expected->file.size() - 4));
    CHECK(state.pilot.last_name == expected->nickname);
    CHECK_FALSE(state.pilot.strict_play);
    CHECK(state.control.male);
    CHECK(record.nickname == expected->nickname);
    CHECK(record.ship_name == expected->ship_name);
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
