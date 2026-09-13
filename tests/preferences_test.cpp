#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "game/preferences.hpp"
#include "pict_image.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

TEST_CASE("Preferences slider controls use the native PICT arrow art") {
  if (!DataAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  for (const auto pict_id : {std::uint16_t{0x86}, std::uint16_t{0x87}}) {
    const auto data = NovaResource_LoadPictData(pict_id);
    REQUIRE(data);
    const auto image = Resource_LoadPictAsImage(*data);
    REQUIRE(image);
    CHECK(image->width == 11);
    CHECK(image->height == 9);
  }
}

TEST_CASE("Key Settings DLOG and DITL preserve the three-column layout") {
  if (!DataAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  const auto def = NovaResource_LoadDialogDefinition(0xfa2);
  REQUIRE(def);
  CHECK(def->right - def->left == 594);
  CHECK(def->bottom - def->top == 353);
  CHECK(def->dialog_item_list_id == 0xfa2);

  const auto items = NovaResource_LoadDialogItems(0xfa2);
  REQUIRE(items);
  REQUIRE(items->size() >= 38);
  // Item 4 is the backdrop frame. Items 5..38 are the 34 key cells; the
  // parser stores them zero-based, matching the handler's ordinal-5 logic.
  CHECK((*items)[3].right - (*items)[3].left == 582);
  CHECK((*items)[3].bottom - (*items)[3].top == 307);
  CHECK((*items)[3].left == 6);
  CHECK((*items)[3].top == 5);
  CHECK((*items)[4].left == 102);
  CHECK((*items)[4].right == 183);
  CHECK((*items)[4].top == 19);
  CHECK((*items)[37].left == 500);
  CHECK((*items)[37].right == 581);
}

TEST_CASE("Key Settings backdrop PICT 0x8b decodes") {
  if (!DataAvailable()) {
    SKIP("Nova .rez archives not present");
  }
  const auto data = NovaResource_LoadPictData(0x8b);
  REQUIRE(data);
  const auto image = Resource_LoadPictAsImage(*data);
  REQUIRE(image);
  CHECK(image->width == 582);
  CHECK(image->height == 307);
  CHECK(image->rgba_pixels.size() ==
        static_cast<std::size_t>(image->width * image->height * 4));
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

  // Flight slots use the normalized navigation row from the original key-name
  // map: forward Up=0x61, turn-left Left=0x63, turn-right Right=0x64, and
  // reverse Down=0x66. Afterburner is DIK_Z=0x2c.
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

TEST_CASE("Original preference file round-trips modeled settings and keys") {
  const auto path = std::filesystem::temp_directory_path() /
                    "evnova_preferences_round_trip.prf";
  std::filesystem::remove(path);

  game::NovaPreferences written;
  written.ResetToDefaults();
  written.intro_music = false;
  written.sound_volume = 8;
  written.brightness = 6;
  written.smoke_trails = true;
  written.starmap_show_borders = true;
  written.bindings.cmd_to_key[0x15] = 0x20;
  REQUIRE(game::NovaPrefs_SaveToFile(path, written));
  CHECK(std::filesystem::file_size(path) == 0x8c);

  game::NovaPreferences loaded;
  loaded.ResetToDefaults();
  REQUIRE(game::NovaPrefs_LoadFromFile(path, loaded));
  CHECK(loaded.intro_music == false);
  CHECK(loaded.sound_volume == 8);
  CHECK(loaded.brightness == 6);
  CHECK(loaded.smoke_trails == true);
  CHECK(loaded.starmap_show_borders == true);
  CHECK(loaded.bindings.cmd_to_key[0x15] == 0x20);

  std::filesystem::remove(path);
}

TEST_CASE("Preference loader rejects versions other than 0x69") {
  const auto path = std::filesystem::temp_directory_path() /
                    "evnova_preferences_wrong_version.prf";
  std::filesystem::remove(path);
  {
    std::array<char, 0x8c> bytes{};
    bytes[0] = 0x68;
    std::ofstream stream(path, std::ios::binary);
    REQUIRE(stream.write(bytes.data(), bytes.size()));
  }
  game::NovaPreferences prefs;
  prefs.ResetToDefaults();
  CHECK_FALSE(game::NovaPrefs_LoadFromFile(path, prefs));
  std::filesystem::remove(path);
}
