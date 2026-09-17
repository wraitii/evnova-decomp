#include "gameplay_interface.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../util/byte_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>

namespace game {
namespace {

using evnova::util::ReadBe16;
using evnova::util::ReadBeI16;
using evnova::util::ReadCString;

// Interface-layout FourCC, stored as the negative key -0x6a918b9a in the
// scenario resource map. The archived record id is the government interface_id
// (>= 0x80), preserving the game's write-once layout cache.
constexpr std::uint32_t kInterfaceLayoutType = 0x956e7466U;

// Raw 32-bit colour slot word at `offset`. The archived HUD palette stores
// each colour in a 4-byte group `00 rr gg bb` (a leading zero pad byte then
// three RGB bytes, most-significant of the group last); the game reads it as a
// little-endian word and shifts the three significant bytes into its colour
// globals (Ui_InstallGameplayInterfaceLayout _DAT_0073561c/1e/20 & peers),
// so this is the exact value the layout blob carries. Consumers apply their
// own channel order, so keep it opaque here (the byte-order of the rendered
// colour is resolved where it is actually painted).
[[nodiscard]] std::uint32_t ReadRgb24(std::span<const std::byte> bytes,
                                      std::size_t offset) {
  if (offset + 4 > bytes.size()) {
    return 0;
  }
  return static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(bytes[offset])) |
         static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(bytes[offset + 1]))
             << 8U |
         static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(bytes[offset + 2]))
             << 16U |
         static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(bytes[offset + 3]))
             << 24U;
}

[[nodiscard]] HudPanelRect ReadPanel(std::span<const std::byte> bytes,
                                     std::size_t top) {
  // The resource uses the native QuickDraw Rect memory layout: top, left,
  // bottom, right. Preserve that fact at this boundary rather than making all
  // SDL consumers reason in the original field order.
  return HudPanelRect{ReadBeI16(bytes, top + 2),
                      ReadBeI16(bytes, top),
                      ReadBeI16(bytes, top + 6),
                      ReadBeI16(bytes, top + 4)};
}

} // namespace

HudBarFill HudBar_FillRect(const HudPanelRect &panel, float fraction) {
  HudBarFill out;
  if (!panel.valid()) {
    return out;
  }
  fraction = std::clamp(fraction, 0.0F, 1.0F);
  const float w = static_cast<float>(panel.width());
  const float h = static_cast<float>(panel.height());
  if (panel.height() > panel.width()) {
    // Tall slot: fill anchored to the top, growing downward. The game computes
    // the cut point `top + height*(value/max)` and clamps it to the bottom.
    out.left = static_cast<float>(panel.left);
    out.top = static_cast<float>(panel.top);
    out.width = w;
    out.height = std::floor(h * fraction);
  } else {
    // Wide slot: fill anchored to the left, growing rightward. This is the
    // observed behavior of the shipped cockpit's horizontal life bars.
    out.width = std::floor(w * fraction);
    out.left = static_cast<float>(panel.left);
    out.top = static_cast<float>(panel.top);
    out.height = h;
  }
  return out;
}

HudPanelRect HudPanel_AnchorTopRight(const HudPanelRect &panel,
                                     std::int16_t render_right) {
  const std::int16_t offset =
      static_cast<std::int16_t>(render_right - kGameplayHudStripWidth);
  return HudPanelRect{static_cast<std::int16_t>(panel.left + offset),
                      panel.top,
                      static_cast<std::int16_t>(panel.right + offset),
                      panel.bottom};
}

GameplayViewportGeometry
GameplayGeometry_FromSurface(const HudPanelRect &surface_rect) {
  // Ghidra 0x00488380 NovaView_UpdateGameplayViewport (mirrored exactly). The
  // cockpit frame PICT is the shipped resource 8000 (1024x768); the game
  // centres it in the shared offscreen surface rect (DAT_00597954..5a), nudges
  // it up/left by 0x3c for small viewports, then derives the HUD origin (its
  // centre point) and the HUD anchor (origin - 0x200/-0x180).
  constexpr std::int16_t kFrameWidth = 1024;
  constexpr std::int16_t kFrameHeight = 768;
  constexpr std::int16_t kNarrowViewport = 0x300;  // 768
  constexpr std::int16_t kShortViewport = 0x281;   // 641
  constexpr std::int16_t kHalfPanelWidth = 0x200;  // 512
  constexpr std::int16_t kHalfPanelHeight = 0x180; // 384
  constexpr std::int16_t kNudge = 0x3c;            // 60

  GameplayViewportGeometry g;
  g.surface_rect = surface_rect;

  // Centre the 1024x768 frame image in the surface (top-left aligned to the
  // midpoints; the image may overhang the surface when the surface is small).
  std::int16_t left = static_cast<std::int16_t>(
      surface_rect.left +
      (static_cast<int>(surface_rect.width()) - kFrameWidth) / 2);
  std::int16_t top = static_cast<std::int16_t>(
      surface_rect.top +
      (static_cast<int>(surface_rect.height()) - kFrameHeight) / 2);
  std::int16_t right = static_cast<std::int16_t>(left + kFrameWidth);
  std::int16_t bottom = static_cast<std::int16_t>(top + kFrameHeight);

  // Restrict the viewport nudges to genuinely small surfaces.
  if (surface_rect.width() < kNarrowViewport) {
    top = static_cast<std::int16_t>(top - kNudge);
    bottom = static_cast<std::int16_t>(bottom - kNudge);
  }
  if (surface_rect.height() < kShortViewport) {
    left = static_cast<std::int16_t>(left - kNudge);
    right = static_cast<std::int16_t>(right - kNudge);
  }

  g.frame_rect = HudPanelRect{left, top, right, bottom};

  // HUD origin = the frame image centre ((right+left+1)>>1, (bottom+top+1)>>1).
  g.hud_panel_origin_x = static_cast<std::int16_t>((right + left + 1) >> 1);
  g.hud_panel_origin_y = static_cast<std::int16_t>((bottom + top + 1) >> 1);
  g.hud_panel_anchor_x =
      static_cast<std::int16_t>(g.hud_panel_origin_x - kHalfPanelWidth);
  g.hud_panel_anchor_y =
      static_cast<std::int16_t>(g.hud_panel_origin_y - kHalfPanelHeight);
  return g;
}

// Ghidra 0x004cda50 Ui_InstallGameplayInterfaceLayout.
std::optional<GameplayInterfaceLayout>
NovaResource_LoadGameplayInterfaceLayout(std::uint16_t interface_id) {
  const auto payload = NovaResource_Load(kInterfaceLayoutType, interface_id);
  if (!payload || payload->size() < 0xa6) {
    if (!payload) {
      NovaLog::Info("gameplay interface layout: no 'interface' record for "
                    "interface {:#04x}",
                    interface_id);
    }
    return std::nullopt;
  }
  const std::span<const std::byte> bytes{*payload};
  GameplayInterfaceLayout out;
  out.radar_panel = ReadPanel(bytes, 0x08); // RadarArea
  out.shield_panel = ReadPanel(bytes, 0x18);
  out.armor_panel = ReadPanel(bytes, 0x24);
  out.fuel_panel = ReadPanel(bytes, 0x30);
  out.travel_status_panel = ReadPanel(bytes, 0x40);
  out.weapon_ammo_panel = ReadPanel(bytes, 0x48);
  out.target_status_panel = ReadPanel(bytes, 0x50);
  out.cargo_status_panel = ReadPanel(bytes, 0x58);
  // Colour slots at +0x00/+0x04/+0x10/+0x14/+0x20/+0x2c/+0x38/+0x3c.
  constexpr std::size_t kColorOffsets[8] = {
      0x00, 0x04, 0x10, 0x14, 0x20, 0x2c, 0x38, 0x3c};
  for (std::size_t i = 0; i < 8; ++i) {
    out.color_word[i] = ReadRgb24(bytes, kColorOffsets[i]);
  }
  out.font_family_name = ReadCString(bytes, 0x60);
  out.font_size = ReadBe16(bytes, 0xa0);
  out.font_size_2 = ReadBe16(bytes, 0xa2);
  out.interface_bg_pict_id =
      std::max<std::uint16_t>(0x80, ReadBe16(bytes, 0xa4));
  NovaLog::Info("gameplay interface layout {:#04x}: radar ({},{})-({},{}), "
                "shield ({},{})-({},{}), "
                "armor ({},{})-({},{}), fuel ({},{})-({},{}), target "
                "({},{})-({},{}), bg PICT {}, font '{}' {}",
                interface_id,
                out.radar_panel.left,
                out.radar_panel.top,
                out.radar_panel.right,
                out.radar_panel.bottom,
                out.shield_panel.left,
                out.shield_panel.top,
                out.shield_panel.right,
                out.shield_panel.bottom,
                out.armor_panel.left,
                out.armor_panel.top,
                out.armor_panel.right,
                out.armor_panel.bottom,
                out.fuel_panel.left,
                out.fuel_panel.top,
                out.fuel_panel.right,
                out.fuel_panel.bottom,
                out.target_status_panel.left,
                out.target_status_panel.top,
                out.target_status_panel.right,
                out.target_status_panel.bottom,
                out.interface_bg_pict_id,
                out.font_family_name,
                out.font_size);
  return out;
}

} // namespace game
