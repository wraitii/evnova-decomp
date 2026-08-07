#pragma once

// Gameplay interface layout ("interface" family, FourCC 0x95 0x6e 0x74 0x66
// stored as the *negative* key -0x6a918b9a in ScenarioData_FindObjectKey;
// U+ interface resource for the player ship's government). Each set of
// government HUD panels carries one layout record that positions every in-
// flight HUD panel around the space viewport and names the cockpit PICT that
// backs them.
//
// Ghidra Ui_InstallGameplayInterfaceLayout (0x004cda50) loads the layout for
// the current government interface and stashes the panel rects + colors + font
// into g_*_panel_* globals. The panels are thin bars/boxes laid out in a single
// coordinate space spanning the HUD strip; the rects here are the exact values
// the game writes (big-endian shorts in the archived payload, word-swapped in
// memory by FUN_004ce700 / FUN_004ce660 via the field table at DAT_00576092).

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace game {

// One HUD panel rect as the interface layout stores it. The original reads
// these as a 4-field (left, top, right, bottom) short quad.
struct HudPanelRect {
  std::int16_t left = 0;
  std::int16_t top = 0;
  std::int16_t right = 0;
  std::int16_t bottom = 0;

  [[nodiscard]] bool valid() const {
    return top < bottom && left < right;
  }
  [[nodiscard]] int width() const { return right > left ? right - left : 0; }
  [[nodiscard]] int height() const { return bottom > top ? bottom - top : 0; }
};

// The decoded gameplay interface layout for one government interface.
//
// Byte offsets follow Ui_InstallGameplayInterfaceLayout: the fighter/industry
// HUD colors are 24-bit RGB words at +0x00/+0x04/+0x10/+0x14/+0x20/+0x2c/+0x38/
// +0x3c (stored in the payload as the raw little-endian RGB24 word, which reads
// back as a distinct (r,g,b) triple); the three life-support bars sit at +0x18
// (shield), +0x24 (armor), +0x30 (fuel); the readout panels at +0x40 (travel
// status), +0x48 (weapon ammo), +0x50 (target status), +0x58 (cargo/mission
// status). +0x60 is the display font family name (Pascal string after
// CString_ToPascalStringInPlace), +0xa0/+0xa2 are the two font sizes, and +0xa4
// is the cockpit/interface background PICT resource id (clamped to >= 0x80).
struct GameplayInterfaceLayout {
  HudPanelRect shield_panel; // +0x18 g_player_shield_panel_*
  HudPanelRect armor_panel;  // +0x24 g_player_armor_panel_*
  HudPanelRect fuel_panel;   // +0x30 g_player_fuel_panel_*
  HudPanelRect travel_status_panel; // +0x40 g_travel_status_panel_*
  HudPanelRect weapon_ammo_panel;   // +0x48 g_weapon_ammo_panel_*
  HudPanelRect target_status_panel; // +0x50 g_target_status_panel_*
  HudPanelRect cargo_status_panel;  // +0x58 g_cargo_status_panel_*

  // HUD colour palette (raw little-endian 32-bit word per colour slot at the
  // colour offsets +0x00/+0x04/+0x10/+0x14/+0x20/+0x2c/+0x38/+0x3c). Each is
  // the value the archive carries; the consumer picks its own channel order
  // (see gameplay_interface.cpp). The meaning of each slot is provisional
  // (value text / label / bar fills per NovaUi_SetupGameplayPanelColors).
  std::uint32_t color_word[8] = {0, 0, 0, 0, 0, 0, 0, 0};

  std::string font_family_name; // +0x60
  std::uint16_t font_size = 0;  // +0xa0
  std::uint16_t font_size_2 = 0; // +0xa2
  // +0xa4: the cockpit PICT resource backing this interface (clamped >= 0x80).
  std::uint16_t interface_bg_pict_id = 0x80;
};

// Ghidra Ui_InstallGameplayInterfaceLayout (0x004cda50) layout lookup. Loads
// the game's archive-held interface-layout record for the given government
// interface id (>= 0x80) and decodes the panel rects/colors/font/bg PICT. The
// government interface id comes from Government::interface_id (GovtDef payload
// +0xac), see scenario_data.hpp. Returns nullopt when the record is absent or
// too short.
[[nodiscard]] std::optional<GameplayInterfaceLayout>
NovaResource_LoadGameplayInterfaceLayout(std::uint16_t interface_id);

} // namespace game
