#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/preferences.hpp"

#include <array>
#include <filesystem>
#include <optional>

namespace {

// The original game data is kept local and unversioned; skip data-dependent
// tests when the archives are not present relative to the working directory.
[[nodiscard]] bool DataAvailable() {
  return std::filesystem::exists("EV Nova/Nova.rez");
}

} // namespace

TEST_CASE("Settings DLOG 0xfa3 is the 336x296 preferences window") {
  if (!DataAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  const auto def = NovaResource_LoadDialogDefinition(0xfa3);
  REQUIRE(def);
  // Window size = DLOG bounds right-bottom minus left-top.
  CHECK(def->right - def->left == 336);
  CHECK(def->bottom - def->top == 296);
  // Centred on the 640x480 playfield ((640-336)/2, (480-296)/2).
  CHECK((640 - (def->right - def->left)) / 2 == 152);
  CHECK((480 - (def->bottom - def->top)) / 2 == 92);
  // Links the same-id DITL.
  CHECK(def->dialog_item_list_id == 0xfa3);
}

TEST_CASE("Preferences DITL 0xfa3 carries the option titles per item") {
  if (!DataAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  const auto items = NovaResource_LoadDialogItems(0xfa3);
  REQUIRE(items);
  // The DITL parser yields 26 entries (25 real items + the trailing
  // terminator).
  CHECK(items->size() > 20);

  const auto item = [&](std::size_t i) -> const NovaDialogItem & {
    return (*items)[i];
  };

  // Item titles come verbatim from the DITL Pascal strings.
  CHECK(item(0).title == "OK");
  CHECK(item(1).title == "Share Processor Time");
  CHECK(item(3).title == "Sound Volume:");
  CHECK(item(7).title == "Intro Music");
  CHECK(item(8).title == "QuickTime Movies");
  CHECK(item(9).title == "Smoke Trails");
  CHECK(item(10).title == "Run in a window");
  CHECK(item(11).title == "Ship Animations");
  CHECK(item(12).title == "Engine Glows");
  CHECK(item(13).title == "Running Lights");
  CHECK(item(14).title == "Weapon Effects");
  CHECK(item(15).title == "Key Settings");
  CHECK(item(17).title == "Parallax Starfield");
  CHECK(item(19).title == "Ambient Sounds");
  CHECK(item(20).title == "Hyperspace Effects");
  CHECK(item(21).title == "Check For Updates");
  CHECK(item(22).title == "Brightness:");

  // Interactive control types line up with the decomp ordinals.
  CHECK(item(0).type == 4);     // OK button
  CHECK(item(15).type == 4);    // Key Settings button
  CHECK(item(5).type == 0x40);  // sound-down arrow
  CHECK(item(25).type == 0x40); // brightness-up arrow

  // The two runtime value boxes are the static-text items.
  CHECK(item(4).type == 8);
  CHECK(item(23).type == 8);
}

TEST_CASE("Preferences defaults match NovaPrefs_ResetToDefaults") {
  game::NovaPreferences prefs;
  prefs.ResetToDefaults();

  CHECK(prefs.intro_music == true);
  CHECK(prefs.sound_volume == 5);
  CHECK(prefs.brightness == 3);
  CHECK(prefs.share_processor_time == true);
  CHECK(prefs.quicktime_movies == false); // inverted flag: 0 = on
  CHECK(prefs.smoke_trails == false);
  CHECK(prefs.ship_animations == true);
  CHECK(prefs.engine_glows == true);
  CHECK(prefs.running_lights == true);
  CHECK(prefs.weapon_effects == true);
  CHECK(prefs.parallax_starfield == true);
  CHECK(prefs.ambient_sounds == true);
  CHECK(prefs.hyperspace_effects == false);
  CHECK(prefs.check_for_updates == true);

  // Flight slots are the ASCII-lowercase row (confirmed by the player-control
  // decomp): forward 'a' = 0x61, turn-left 'c' = 0x63, turn-right 'd' = 0x64,
  // reverse 'f' = 0x66. Afterburner is DIK_Z = 0x2c.
  CHECK(prefs.bindings.cmd_to_key[0x15] == 0x61);
  CHECK(prefs.bindings.cmd_to_key[0x13] == 0x63);
  CHECK(prefs.bindings.cmd_to_key[0x14] == 0x64);
  CHECK(prefs.bindings.cmd_to_key[0x16] == 0x66);
  CHECK(prefs.bindings.cmd_to_key[0x18] == 0x2c);
  // Unbound default is 0xff.
  CHECK(prefs.bindings.cmd_to_key[0x21] == 0xff);
  CHECK(prefs.bindings.cmd_to_key[0x22] == 0xff);
  CHECK(prefs.bindings.cmd_to_key.size() == 0x52);
}
