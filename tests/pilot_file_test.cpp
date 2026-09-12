#include <catch2/catch_test_macros.hpp>

#include "game/game_state.hpp"
#include "game/pilot_file.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
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
  p.cargo_bins = {3, 0, 5, 1, 0, 2};
  p.outfit_owned_count[0] = 1;
  p.outfit_owned_count[0x1ff] = -2;
  p.weapon_bank_ammo[5 * 100] = 7;
  p.weapon_bank_secondary[5 * 100] = 11;
  p.weapon_bank_ammo[0xff * 100 + 42] = 99; // non-first slot: not serialized
  p.junk_counts[0x7f] = 13;
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
  CHECK(out.ship_class_id == 3);
  CHECK(out.fuel_points == 77.0F); // 77.6 truncated toward zero, stored as u16
  CHECK(out.intro_played);
  CHECK(out.cargo_bins == p.cargo_bins);
  CHECK(out.outfit_owned_count[0] == 1);
  CHECK(out.outfit_owned_count[0x1ff] == -2);
  CHECK(out.weapon_bank_ammo[5 * 100] == 7);
  CHECK(out.weapon_bank_secondary[5 * 100] == 11);
  // Non-first bank slots are not persisted by the original format.
  CHECK(out.weapon_bank_ammo[0xff * 100 + 42] == 0);
  CHECK(out.junk_counts[0x7f] == 13);
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

TEST_CASE("PilotFile load skips a block1 rejected by the validity gate") {
  const PilotFile p = SampleRecord();
  auto bytes = PilotFileSerialize(p, p.jump_dest_stellar);
  // A jump destination of -1 (0xffff) makes block1's first u16 >= 0x800, which
  // fails the 0x008725b0 gate; the original then skips the block1 restore but
  // still restores block2 (and returns 0). Mirror that. Block1 data starts
  // right after the u32 size header.
  bytes[4] = std::byte{0xff};
  bytes[5] = std::byte{0xff};
  PilotFile out;
  const auto err = PilotFileDeserialize(bytes, out);
  CHECK(err == PilotLoadError::kOk);
  CHECK(out.ship_class_id == 0);   // block1 restore skipped
  CHECK(out.intro_played == true); // block2 restore still ran
  CHECK(out.junk_counts[0x7f] == 13);
}

} // namespace
