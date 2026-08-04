#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// BRGR container access is a reconstruction adapter, not a direct Ghidra
// function. It parses the container entry table (a 12-byte header plus a
// packed (offset, size, name) table) and the big-endian resource.map, then
// resolves (type, id) pairs to archive regions. This replaces the previous
// hardcoded region indices, which mis-assigned the odd sp\x95n resources
// (601/603/605/606) and the splash PICTs.

// Resource type codes as stored in resource.map records (big-endian FourCCs).
constexpr std::uint32_t kResourceTypeSprites = 0x7370956e;   // "sp\x95n"
constexpr std::uint32_t kResourceTypeColors = 0x639a6c72;    // "c\x9alr"
constexpr std::uint32_t kResourceTypeRleSheet8 = 0x726c9138; // "rl\x9138" (8-bit sheets)
constexpr std::uint32_t kResourceTypeRleSheet16 = 0x726c9144;// "rl\x91D" (16-bit sheets)
constexpr std::uint32_t kResourceTypePict = 0x50494354;      // "PICT"

// Ghidra: FUN_004ce250 + FUN_004cdfa0 (resource lookup by type + id). Returns
// the raw resource payload; the first archive holding a matching record wins,
// mirroring the game's archive search order.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_Load(std::uint32_t type_code, std::uint16_t resource_id);

// Ghidra: FUN_004ce2a0 + FUN_004ce030 (n-th record of a type, 1-based). The
// game reads the c\x9alr style through this accessor rather than by id.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadNthOfType(std::uint32_t type_code, std::size_t ordinal);

// On-disk layout documented as the "sp\x95n" resource in the Nova Bible.
// Field 0/1 are the sprite-graphics and mask resource ids (rl\x91D sheets for
// the menu buttons, PICT for the main-screen logo 606).
struct NovaSpriteDefinition {
  std::uint16_t sprites_resource_id = 0;
  std::uint16_t mask_resource_id = 0;
  std::uint16_t tile_width = 0;
  std::uint16_t tile_height = 0;
  std::uint16_t tiles_x = 0;
  std::uint16_t tiles_y = 0;
};

struct NovaRgbColor {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
};

struct NovaMenuPoint {
  std::int16_t x = 0;
  std::int16_t y = 0;
};

// The portions of c\x9alr used by the title screen. Its coordinate system is
// the original 1024x768 main-menu backdrop. Big-endian scalar fields.
struct NovaMainMenuStyle {
  NovaRgbColor menu_bright;
  NovaRgbColor menu_dim;
  std::uint16_t menu_font_size = 0;
  std::array<NovaMenuPoint, 6> button_origins{};
};

[[nodiscard]] std::optional<NovaSpriteDefinition>
NovaSpriteDefinition_Parse(std::span<const std::byte> resource_data);

// Ghidra: FUN_004b4e10 resolves sp\x95n resources for FUN_004ad960's main-menu
// focus sprites 600-605 and main-screen logo 606.
[[nodiscard]] std::optional<NovaSpriteDefinition>
NovaResource_LoadMainMenuSpriteDefinition(std::uint16_t sprite_id);

[[nodiscard]] std::optional<NovaMainMenuStyle>
NovaMainMenuStyle_Parse(std::span<const std::byte> resource_data);

// Ghidra: NovaData_LoadScenarioResourceTables reads the first c\x9alr record
// (FUN_004ce2a0(0x639a6c72, 1)) and copies these fields into menu
// color/position globals.
[[nodiscard]] std::optional<NovaMainMenuStyle> NovaResource_LoadMainMenuStyle();

// Ghidra: 0x004ce250 resource acquisition beneath Resource_LoadPictAsImage.
// Covers the loading splash PICT 0x1fa4 and the startup splash PICT 0x83.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadPictData(std::uint16_t resource_id);
