#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/new_pilot_flow.hpp"
#include "game/nova_name_text.hpp"
#include "game/pilot_file.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
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

struct TemporaryDirectory {
  std::filesystem::path path;

  ~TemporaryDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

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
  p.weapon_mounted[5] = 7;
  p.weapon_ammo[5] = 11;
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
  p.active_mission_runtime_flags[2].travel_stellar_reached = true;
  p.active_mission_runtime_flags[2].deadline_day = 17;
  p.active_mission_runtime_flags[2].deadline_time_components = {
      0x3456, 0x1234, 0x5678, 0x9abc};
  p.active_missions[2].carrying_resources = true;
  p.active_missions[2].mission_template_id = 0x91;
  p.active_missions[2].return_stellar_id = 0x123;
  p.active_missions[2].resource_delta_or_cost = -4567;
  p.active_missions[2].on_resolve_repeat_count = 8;
  p.active_missions[2].goal_counter_a = 9;
  p.active_missions[2].goal_counter_b = 10;
  p.active_missions[2].goal_counter_c = 11;
  p.active_missions[2].goal_count_remaining = 12;
  p.active_missions[2].goal_counter_e = 13;
  p.active_missions[2].mission_target_count = 14;
  p.active_missions[2].aux_ship_system_locator = 15;
  p.active_missions[2].aux_ships_spawned = 16;
  p.active_missions[2].rearm_roll_clock = 17;
  p.active_missions[2].on_accept_text[0] = std::byte{'A'};
  p.active_missions[2].on_refuse_text[1] = std::byte{'R'};
  p.active_missions[2].on_success_text[2] = std::byte{'S'};
  p.active_missions[2].on_failure_text[3] = std::byte{'F'};
  p.active_missions[2].on_abort_text[4] = std::byte{'B'};
  p.active_missions[2].on_ship_done_text[5] = std::byte{'D'};
  p.active_missions[2].raw_payload[0x6e] = std::byte{0xa5};
  return p;
}

TEST_CASE("player reset preserves the christened ship name") {
  game::GameState state;
  state.player.ship_name = "Vengeance";
  state.player.is_active = false;
  state.player.death_timer_active = 12.0F;

  game::NovaShip_ResetPlayerShipState(state);

  CHECK(state.player.ship_name == "Vengeance");
  CHECK(state.player.is_active);
  CHECK(state.player.death_timer_active == -999.0F);
}

TEST_CASE("fresh pilot application retains the christened ship name") {
  game::GameState state;
  state.player.ship_name = "Vengeance";
  game::PilotFile record = game::PilotFile::Fresh();
  record.pilot_name = "Jacqueline";
  record.nickname = "Maclean";
  record.ship_name = state.player.ship_name;

  game::PilotFileApply(record, state);

  CHECK(state.pilot.first_name == "Jacqueline");
  CHECK(state.pilot.last_name == "Maclean");
  CHECK(state.player.ship_name == "Vengeance");
}

TEST_CASE("PilotFileApply leaves the live secondary selection untouched") {
  // ShipState.active_weapon_bank_slot (+0x72) is live runtime state: the
  // saver/loader (0x004c7dd0/0x004cb260) never touch it, and
  // Ship_ResetPlayerShipState (0x004b3350) supplies -1. Apply must preserve.
  game::GameState state;
  const game::PilotFile record = game::PilotFile::Fresh();

  state.player.active_weapon_bank_slot = 7;
  game::PilotFileApply(record, state);
  CHECK(state.player.active_weapon_bank_slot == 7);

  state.player.active_weapon_bank_slot = -1;
  game::PilotFileApply(record, state);
  CHECK(state.player.active_weapon_bank_slot == -1);
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
  CHECK(out.weapon_mounted[5] == 7);
  CHECK(out.weapon_ammo[5] == 11);
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
  CHECK(out.active_mission_runtime_flags[2].travel_stellar_reached);
  CHECK(out.active_mission_runtime_flags[2].deadline_day == 17);
  CHECK(out.active_mission_runtime_flags[2].deadline_time_components ==
        p.active_mission_runtime_flags[2].deadline_time_components);
  CHECK(out.active_missions[2].carrying_resources);
  CHECK(out.active_missions[2].mission_template_id == 0x91);
  CHECK(out.active_missions[2].return_stellar_id == 0x123);
  CHECK(out.active_missions[2].resource_delta_or_cost == -4567);
  CHECK(out.active_missions[2].on_resolve_repeat_count == 8);
  CHECK(out.active_missions[2].goal_counter_a == 9);
  CHECK(out.active_missions[2].goal_counter_b == 10);
  CHECK(out.active_missions[2].goal_counter_c == 11);
  CHECK(out.active_missions[2].goal_count_remaining == 12);
  CHECK(out.active_missions[2].goal_counter_e == 13);
  CHECK(out.active_missions[2].mission_target_count == 14);
  CHECK(out.active_missions[2].aux_ship_system_locator == 15);
  CHECK(out.active_missions[2].aux_ships_spawned == 16);
  CHECK(out.active_missions[2].rearm_roll_clock == 17);
  CHECK(out.active_missions[2].on_accept_text[0] == std::byte{'A'});
  CHECK(out.active_missions[2].on_refuse_text[1] == std::byte{'R'});
  CHECK(out.active_missions[2].on_success_text[2] == std::byte{'S'});
  CHECK(out.active_missions[2].on_failure_text[3] == std::byte{'F'});
  CHECK(out.active_missions[2].on_abort_text[4] == std::byte{'B'});
  CHECK(out.active_missions[2].on_ship_done_text[5] == std::byte{'D'});
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

TEST_CASE("retired crön slots persist as inactive and stay retired") {
  game::GameState state;
  state.scenario.cron_events.resize(6);
  state.scenario.cron_events[5].present = true;
  // A slot that has finished: is_active is false, but deactivation leaves the
  // counters non-negative, which PilotFileApply would read back as active.
  state.cron_event_states[5].is_active = false;
  state.cron_event_states[5].duration_counter = 0;
  state.cron_event_states[5].holdoff_counter = 0;

  const PilotFile record = PilotFileCollectFromState(state);
  // Ghidra 0x004c7dd0 writes the 0xffff sentinel pair for an inactive slot.
  CHECK(record.cron_duration_counters[5] == -1);
  CHECK(record.cron_holdoff_counters[5] == -1);

  const auto bytes = PilotFileSerialize(record, record.jump_dest_stellar);
  PilotFile loaded;
  const auto err = PilotFileDeserialize(bytes, loaded);
  REQUIRE(
      (err == PilotLoadError::kOk || err == PilotLoadError::kRepairsApplied));
  CHECK(loaded.cron_duration_counters[5] == -1);
  CHECK(loaded.cron_holdoff_counters[5] == -1);

  game::GameState restored;
  restored.scenario.cron_events.resize(6);
  restored.scenario.cron_events[5].present = true;
  PilotFileApply(loaded, restored);
  CHECK_FALSE(restored.cron_event_states[5].is_active);
}

TEST_CASE("new-pilot stellar reset derives domination from flags 0x20") {
  game::GameState state;
  state.scenario.stellars.resize(3);
  auto &dominated = state.scenario.stellars[0];
  dominated.availability_flags = 0x20; // Bible "starts the game dominated"
  dominated.strength_capacity = 5;
  dominated.strength = 99;
  auto &destroyed = state.scenario.stellars[1];
  destroyed.availability_flags = 0x40; // Bible "starts the game destroyed"
  destroyed.schedule_days = 7;
  destroyed.strength = 99;
  auto &plain = state.scenario.stellars[2];
  plain.strength_capacity = 9;
  plain.strength = 3;

  game::NovaNewPilot_ResetStellarStrengthForNewGame(state);

  CHECK(dominated.dominated == 1);
  CHECK(dominated.strength == 5);
  CHECK(dominated.destroyed_days_remaining == 0);
  CHECK(destroyed.dominated == 0);
  CHECK(destroyed.strength == -1);
  CHECK(destroyed.destroyed_days_remaining == 7);
  CHECK(plain.dominated == 0);
  CHECK(plain.strength == 9);
  CHECK(plain.destroyed_days_remaining == 0);

  // Starts-destroyed with a negative schedule seed pins the countdown to 1.
  destroyed.schedule_days = -3;
  game::NovaNewPilot_ResetStellarStrengthForNewGame(state);
  CHECK(destroyed.destroyed_days_remaining == 1);
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
  saved_state.player.weapon_banks[5].mounted = 7;
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
  // +0x72 is live runtime state: the pre-load reset leaves -1 and the saved
  // data never carries it (see the dedicated apply/fixture tests).
  CHECK(loaded_state.player.active_weapon_bank_slot == -1);
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
  CHECK(loaded_state.player.weapon_banks[5].mounted == 7);
  CHECK_FALSE(loaded_state.active_missions[0].mission_fleet_name.empty());
  CHECK_FALSE(loaded_state.active_missions[0].mission_text_name_b.empty());
  CHECK(loaded_state.active_missions[0].mission_ship_count_active == 7);
  CHECK(loaded_state.active_missions[0].aux_ships_spawned == 0);
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

TEST_CASE("archived pilot fixtures survive a non-destructive canonical disk "
          "round trip",
          "[pilot-fixtures]") {
  const std::filesystem::path fixture_dir = "docs/assets/pilots";
  if (!std::filesystem::is_directory(fixture_dir)) {
    SKIP("optional docs/assets/pilots fixtures are not installed");
  }

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  TemporaryDirectory temporary{
      std::filesystem::temp_directory_path() /
      ("evnova-pilot-roundtrip-" + std::to_string(nonce))};
  REQUIRE(std::filesystem::create_directory(temporary.path));

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

    // The fixture is opened read-only. All output is confined to the unique
    // temporary directory, so a failure cannot alter the archived input.
    std::ifstream source(entry.path(), std::ios::binary);
    REQUIRE(source);
    const std::string raw{std::istreambuf_iterator<char>(source),
                          std::istreambuf_iterator<char>()};
    std::vector<std::byte> source_bytes(raw.size());
    std::memcpy(source_bytes.data(), raw.data(), raw.size());

    PilotFile decoded;
    const PilotLoadError decode_result =
        PilotFileDeserialize(source_bytes, decoded);
    REQUIRE((decode_result == PilotLoadError::kOk ||
             decode_result == PilotLoadError::kRepairsApplied));

    const std::vector<std::byte> canonical =
        PilotFileSerialize(decoded, decoded.jump_dest_stellar);
    const auto copy_path = temporary.path / entry.path().filename();
    {
      std::ofstream copy(copy_path, std::ios::binary | std::ios::trunc);
      REQUIRE(copy);
      copy.write(reinterpret_cast<const char *>(canonical.data()),
                 static_cast<std::streamsize>(canonical.size()));
      REQUIRE(copy);
    }

    std::ifstream copy(copy_path, std::ios::binary);
    REQUIRE(copy);
    const std::string copied_raw{std::istreambuf_iterator<char>(copy),
                                 std::istreambuf_iterator<char>()};
    std::vector<std::byte> copied_bytes(copied_raw.size());
    std::memcpy(copied_bytes.data(), copied_raw.data(), copied_raw.size());
    PilotFile reloaded;
    REQUIRE(PilotFileDeserialize(copied_bytes, reloaded) ==
            PilotLoadError::kOk);

    // Canonical serialization must be byte-stable after the disk reload. This
    // compares every persisted byte, including opaque mission-record regions.
    CHECK(PilotFileSerialize(reloaded, reloaded.jump_dest_stellar) ==
          canonical);
  }
  if (fixture_count == 0) {
    SKIP("no optional .plt fixtures found in docs/assets/pilots");
  }
}

TEST_CASE("pilot save stores the 0-based stellar index, not a resource id") {
  using game::PilotFileStellarIndexFromResourceId;
  // 0x004c7dd0/0x004cb260 treat block1+0x00 as a g_stellar_defs index;
  // the port's stellar ids are 0x80-based resource ids.
  CHECK(PilotFileStellarIndexFromResourceId(0x80) == 0);
  CHECK(PilotFileStellarIndexFromResourceId(0x81) == 1);
  CHECK(PilotFileStellarIndexFromResourceId(0xea) == 106);
  CHECK(PilotFileStellarIndexFromResourceId(-1) == -1);
  // The new-game "no nav stellar" fallback saves index 0 (see
  // Menu_RunNewGameFlow 0x00489d70); it must pass through untouched.
  CHECK(PilotFileStellarIndexFromResourceId(0) == 0);

  game::GameState state;
  REQUIRE(state.scenario.LoadFromArchives());
  // Pick a defined stellar away from the origin so a lost position (0,0) or an
  // off-by-0x80 lookup on the wrong (undefined) slot is detectable.
  const game::Stellar *departure = nullptr;
  std::int16_t departure_resource_id = -1;
  for (std::size_t i = 0; i < state.scenario.stellars.size(); ++i) {
    const game::Stellar &stellar = state.scenario.stellars[i];
    if (stellar.is_defined && (stellar.pos_x != 0 || stellar.pos_y != 0)) {
      departure = &stellar;
      departure_resource_id = static_cast<std::int16_t>(i + 0x80);
      break;
    }
  }
  REQUIRE(departure != nullptr);

  const auto dir =
      std::filesystem::temp_directory_path() / "evnova_pilot_index_test";
  std::filesystem::create_directories(dir);
  const auto path = dir / "Index Pilot.plt";
  std::filesystem::remove(path);

  state.pilot.first_name = "Index Pilot";
  state.player.ship_class_id = 0;
  REQUIRE(PilotFileSaveGame(
      dir, state, PilotFileStellarIndexFromResourceId(departure_resource_id)));

  game::GameState loaded;
  REQUIRE(loaded.scenario.LoadFromArchives());
  game::NovaShip_ResetPlayerShipState(loaded);
  const auto result = PilotFileLoadSave(path, loaded);
  CHECK((result == PilotLoadError::kOk ||
         result == PilotLoadError::kRepairsApplied));
  // PilotFile_LoadSave 0x004cb260: pos = g_stellar_defs[jump_dest].map_x/y.
  CHECK(loaded.player.pos_x == static_cast<float>(departure->pos_x));
  CHECK(loaded.player.pos_y == static_cast<float>(departure->pos_y));

  PilotFileDelete(path);
  std::filesystem::remove(dir / "Last Pilot");
}

TEST_CASE("pilot save directory mirrors the Nova Pilots subfolder",
          "[pilot_file]") {
  // Prefs_SetPilotsPathPrefix (0x004bd0c0) resolves EVNova.ini [130] S3
  // ("Pilots") beside the install. The port mirrors that with a "Pilots"
  // subdirectory under SDL's per-user writable preference path instead of
  // writing pilots at its root.
  const auto directory = game::PilotFileSaveDirectory();
  REQUIRE(directory.has_value());
  CHECK(directory->filename() == "Pilots");
  CHECK(std::filesystem::is_directory(*directory));
}

TEST_CASE("NameString_StripSubtitleSuffix matches the original Pascal trim",
          "[new_pilot]") {
  // Ghidra 0x004cd230: truncate at the LAST ';', trim only the run of spaces
  // before it. A name without a semicolon is returned completely unchanged,
  // trailing spaces included (the trim lives in the found-semicolon branch).
  using game::NovaText_StripSubtitleSuffix;
  CHECK(NovaText_StripSubtitleSuffix("Base Name;Subtitle ") == "Base Name");
  CHECK(NovaText_StripSubtitleSuffix("Base  ;Sub") == "Base");
  CHECK(NovaText_StripSubtitleSuffix("Name;") == "Name");
  CHECK(NovaText_StripSubtitleSuffix("A;B ;C") == "A;B");
  CHECK(NovaText_StripSubtitleSuffix("A ; ;X") == "A");
  CHECK(NovaText_StripSubtitleSuffix("A;;X") == "A");
  CHECK(NovaText_StripSubtitleSuffix("; ;X") == ";");
  CHECK(NovaText_StripSubtitleSuffix(";;X") == ";");
  CHECK(NovaText_StripSubtitleSuffix(";Sub") == "");
  CHECK(NovaText_StripSubtitleSuffix("   ;Sub") == "");
  CHECK(NovaText_StripSubtitleSuffix("   ;X") == "");
  CHECK(NovaText_StripSubtitleSuffix(" ") == " ");
  CHECK(NovaText_StripSubtitleSuffix("") == "");
  // No semicolon: unchanged, including trailing spaces.
  CHECK(NovaText_StripSubtitleSuffix("NoSemi") == "NoSemi");
  CHECK(NovaText_StripSubtitleSuffix("NoSemi  ") == "NoSemi  ");
}

TEST_CASE("NameString_StripSubtitleSuffix matches the literal 0x004cd230 loop",
          "[new_pilot]") {
  // Exhaustive check over short strings of 'A', ' ' and ';', plus a trailing
  // suffix, against a direct transcription of the Ghidra loop (1-based
  // Pascal semantics). This pins every prefix edge, including strings whose
  // prefix is only spaces and semicolons.
  using game::NovaText_StripSubtitleSuffix;
  const auto reference = [](std::string_view input) {
    const int len = static_cast<int>(input.size());
    // 1-based copy; index 0 unused.
    std::string s = " ";
    s.append(input);
    int bVar2 = len;
    int uVar4 = len;
    bool found = false;
    int uVar3 = uVar4;
    if (len != 0) {
      do {
        const char c = s[static_cast<std::size_t>(uVar4)];
        if (found) {
          if (c == ' ') {
            uVar3 = uVar4 - 1;
          } else if (c != ';') {
            bVar2 = uVar4;
            break;
          }
        } else if (c == ';') {
          found = true;
          uVar3 = uVar4 - 1;
        }
        bVar2 = uVar3;
        --uVar4;
      } while (0 < uVar4);
    }
    return s.substr(1, static_cast<std::size_t>(bVar2));
  };

  constexpr std::string_view kAlphabet = "A ;";
  for (int length = 0; length <= 5; ++length) {
    int combinations = 1;
    for (int i = 0; i < length; ++i) {
      combinations *= 3;
    }
    for (int code = 0; code < combinations; ++code) {
      std::string value;
      int digits = code;
      for (int i = 0; i < length; ++i) {
        value.push_back(kAlphabet[static_cast<std::size_t>(digits % 3)]);
        digits /= 3;
      }
      for (const std::string_view suffix : {std::string_view{""},
                                            std::string_view{"X"},
                                            std::string_view{"Z;Q"}}) {
        const std::string candidate = value + std::string{suffix};
        INFO("input='" << candidate << "'");
        CHECK(NovaText_StripSubtitleSuffix(candidate) == reference(candidate));
      }
    }
  }
}

TEST_CASE("PilotData_PickStartingSystem preserves the original RNG draws",
          "[new_pilot]") {
  using game::PilotData_PickStartingSystem;

  SECTION("all-invalid template keeps system 0 and consumes no RNG") {
    game::CharacterTemplate tmpl;
    tmpl.systems = {-1, 0x7f, -1, 0x7f};
    std::mt19937 rng{1234};
    std::mt19937 reference{1234};
    CHECK(PilotData_PickStartingSystem(tmpl, rng) == 0);
    CHECK(rng() == reference());
  }

  SECTION("rejection loop draws the same number of values") {
    game::CharacterTemplate tmpl;
    tmpl.systems = {0x81, 0x82, -1, -1};
    std::mt19937 rng{7};
    std::mt19937 reference{7};
    const std::int16_t got = PilotData_PickStartingSystem(tmpl, rng);
    std::uniform_int_distribution<int> roll{0, 3};
    std::int16_t expected = 0;
    do {
      expected = tmpl.systems[static_cast<std::size_t>(roll(reference))];
    } while (expected < 0x80);
    CHECK(got == expected - 0x80);
    CHECK(rng() == reference());
  }

  SECTION("single valid slot always resolves to it") {
    game::CharacterTemplate tmpl;
    tmpl.systems = {-1, -1, 0x85, -1};
    std::mt19937 rng{99};
    CHECK(PilotData_PickStartingSystem(tmpl, rng) == 0x85 - 0x80);
  }
}

} // namespace
