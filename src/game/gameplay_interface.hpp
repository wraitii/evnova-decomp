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

// One HUD panel rect in conventional screen coordinates. The archived
// interface resource stores its four shorts in QuickDraw Rect order:
// (top, left, bottom, right); NovaResource_LoadGameplayInterfaceLayout maps
// those values into this representation.
struct HudPanelRect {
  std::int16_t left = 0;
  std::int16_t top = 0;
  std::int16_t right = 0;
  std::int16_t bottom = 0;

  [[nodiscard]] bool valid() const { return top < bottom && left < right; }

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
  HudPanelRect shield_panel;        // +0x18 g_player_shield_panel_*
  HudPanelRect armor_panel;         // +0x24 g_player_armor_panel_*
  HudPanelRect fuel_panel;          // +0x30 g_player_fuel_panel_*
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

  std::string font_family_name;  // +0x60
  std::uint16_t font_size = 0;   // +0xa0
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

// ---------------------------------------------------------------------------
// Gameplay viewport/frame geometry
// ---------------------------------------------------------------------------
// Ghidra NovaView_UpdateGameplayViewport (0x00488380) computes the in-game
// camera geometry every frame. The game works in a fixed 1024x768 logical
// canvas (the shared offscreen surface DAT_00597950; the 1024x768 frame PICT
// 8000 is centred in it). This is frame geometry, distinct from the gameplay
// status strip, whose panels are right-edge anchored by
// HudPanel_AnchorTopRight:
//
//   * g_hud_panel_origin : the central point of the frame workspace.
//   * g_hud_panel_anchor : origin - (0x200, 0x180), retained for frame
//     compositing diagnostics.
//
// The host display scales this canvas to the window (the original window is
// 640x480, the same 1024->640 0.625 scale the main menu uses).
struct GameplayViewportGeometry {
  // The workspace rect the frame PICT is centred in. Normally the full
  // 0,0,1024,768 canvas; smaller (e.g. low-res) windows produce a sub-rect
  // that triggers the up/left nudge below.
  HudPanelRect surface_rect{};
  // Where the 1024x768 cockpit frame PICT landed after centering + nudging
  // (used only to derive the origin; kept for diagnostics).
  HudPanelRect frame_rect{};
  // Centred HUD origin (canvas x/y of the workspace centre).
  std::int16_t hud_panel_origin_x = 0;
  std::int16_t hud_panel_origin_y = 0;
  // Top-left of the 1024x768 HUD panel: origin - (+0x200, +0x180).
  std::int16_t hud_panel_anchor_x = 0;
  std::int16_t hud_panel_anchor_y = 0;
};

// Mirrors NovaView_UpdateGameplayViewport's arithmetic for the given workspace
// surface rect. `surface_rect` is the shared offscreen surface (DAT_00597954..
// 5a); the cockpit frame PICT is taken to be 1024x768 (the shipped resource
// 8000). The small-viewport nudges (offset up by 0x3c when the surface is <
// 0x300 wide, left by 0x3c when < 0x281 tall) are reproduced exactly.
[[nodiscard]] GameplayViewportGeometry
GameplayGeometry_FromSurface(const HudPanelRect &surface_rect);

// The filled portion of a single life-support bar, in the game's 1024x768
// canvas coordinates. Reproduces the fill geometry of NovaUi_DrawPlayerShield-
// Bar (0x0045ea66), NovaUi_DrawPlayerArmorBar (0x0045ebe8) and
// NovaUi_DrawPlayerFuelLevelBar (0x0045f086): the game selects the fill axis
// from the panel rect's own aspect.
//
//   * Tall slot (height > width) -- the life bars -- is anchored to the TOP
//     and grows downward: the fill occupies [top, top + height*fraction].
//   * Wide slot (height <= width, a horizontal gauge) is anchored to the LEFT
//     and grows rightward: the fill occupies [left, left + width*fraction].
//
// fraction is clamped to [0,1] and the fill is always within the panel rect.
struct HudBarFill {
  float left = 0.0F;
  float top = 0.0F;
  float width = 0.0F;
  float height = 0.0F;

  [[nodiscard]] bool empty() const { return width <= 0.0F || height <= 0.0F; }
};

[[nodiscard]] HudBarFill HudBar_FillRect(const HudPanelRect &panel,
                                         float fraction);

// The original uses DAT_0088c020 (0xc2) as the width of the top-right cockpit
// strip. Each panel rect is translated horizontally by render_right - 0xc2.
constexpr std::int16_t kGameplayHudStripWidth = 0xc2;

[[nodiscard]] HudPanelRect HudPanel_AnchorTopRight(const HudPanelRect &panel,
                                                   std::int16_t render_right);

} // namespace game
