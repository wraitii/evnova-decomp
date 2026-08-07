#include "gameplay_interface.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>

namespace game {
namespace {

// Interface-layout FourCC, stored as the negative key -0x6a918b9a in the
// scenario resource map. The archived record id is the government interface_id
// (>= 0x80), preserving the game's write-once layout cache.
constexpr std::uint32_t kInterfaceLayoutType = 0x956e7466U;

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  if (offset + 2 > bytes.size()) {
    return 0;
  }
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) << 8U |
      std::to_integer<std::uint8_t>(bytes[offset + 1]));
}

[[nodiscard]] std::int16_t ReadBeI16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::int16_t>(ReadBe16(bytes, offset));
}

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
  return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8U |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2])) << 16U |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3])) << 24U;
}

// NUL-terminated C string at `offset` (bounded by the payload). The family
// name is stored as a C string and Pascal-ized in memory; we surface it as-is.
[[nodiscard]] std::string ReadCString(std::span<const std::byte> bytes,
                                      std::size_t offset) {
  if (offset >= bytes.size()) {
    return {};
  }
  const auto *begin = reinterpret_cast<const char *>(bytes.data() + offset);
  const auto max_len = bytes.size() - offset;
  const void *nul = std::memchr(begin, '\0', max_len);
  const std::size_t len =
      nul == nullptr
          ? max_len
          : static_cast<std::size_t>(static_cast<const char *>(nul) - begin);
  return std::string{begin, len};
}

[[nodiscard]] HudPanelRect ReadPanel(std::span<const std::byte> bytes,
                                     std::size_t left) {
  return HudPanelRect{ReadBeI16(bytes, left),
                      ReadBeI16(bytes, left + 2),
                      ReadBeI16(bytes, left + 4),
                      ReadBeI16(bytes, left + 6)};
}

} // namespace

std::optional<GameplayInterfaceLayout>
NovaResource_LoadGameplayInterfaceLayout(std::uint16_t interface_id) {
  const auto payload =
      NovaResource_Load(kInterfaceLayoutType, interface_id);
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
  out.shield_panel = ReadPanel(bytes, 0x18);
  out.armor_panel = ReadPanel(bytes, 0x24);
  out.fuel_panel = ReadPanel(bytes, 0x30);
  out.travel_status_panel = ReadPanel(bytes, 0x40);
  out.weapon_ammo_panel = ReadPanel(bytes, 0x48);
  out.target_status_panel = ReadPanel(bytes, 0x50);
  out.cargo_status_panel = ReadPanel(bytes, 0x58);
  // Colour slots at +0x00/+0x04/+0x10/+0x14/+0x20/+0x2c/+0x38/+0x3c.
  constexpr std::size_t kColorOffsets[8] = {0x00, 0x04, 0x10, 0x14,
                                            0x20, 0x2c, 0x38, 0x3c};
  for (std::size_t i = 0; i < 8; ++i) {
    out.color_word[i] = ReadRgb24(bytes, kColorOffsets[i]);
  }
  out.font_family_name = ReadCString(bytes, 0x60);
  out.font_size = ReadBe16(bytes, 0xa0);
  out.font_size_2 = ReadBe16(bytes, 0xa2);
  out.interface_bg_pict_id =
      std::max<std::uint16_t>(0x80, ReadBe16(bytes, 0xa4));
  NovaLog::Info("gameplay interface layout {:#04x}: shield ({},{})-({},{}), "
                "armor ({},{})-({},{}), fuel ({},{})-({},{}), target "
                "({},{})-({},{}), bg PICT {}, font '{}' {}",
                interface_id,
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
