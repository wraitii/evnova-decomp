#pragma once

#include "sdl_audio.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// BRGR container access is a reconstruction adapter, not a direct Ghidra
// function. It parses the container entry table (a 12-byte header plus a
// packed (offset, size, name) table) and the big-endian resource.map, then
// resolves (type, id) pairs to archive regions. This replaces the previous
// hardcoded region indices, which mis-assigned the odd sp\x95n resources
// (601/603/605/606) and the splash PICTs.

// Resource type codes as stored in resource.map records (big-endian FourCCs).
constexpr std::uint32_t kResourceTypeSprites = 0x7370956e; // "sp\x95n"
constexpr std::uint32_t kResourceTypeColors = 0x639a6c72;  // "c\x9alr"
constexpr std::uint32_t kResourceTypeRleSheet8 =
    0x726c9138; // "rl\x9138" (8-bit sheets)
constexpr std::uint32_t kResourceTypeRleSheet16 =
    0x726c9144; // "rl\x91D" (16-bit sheets)
constexpr std::uint32_t kResourceTypePict = 0x50494354; // "PICT"
constexpr std::uint32_t kResourceTypeSnd =
    0x736e6420; // "snd " (AIFF-style sounds)

// Ghidra: FUN_004ce250 + FUN_004cdfa0 (resource lookup by type + id). Returns
// the raw resource payload; the first archive holding a matching record wins,
// mirroring the game's archive search order.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_Load(std::uint32_t type_code, std::uint16_t resource_id);

// Ghidra: FUN_004ce2a0 + FUN_004ce030 (n-th record of a type, 1-based). The
// game reads the c\x9alr style through this accessor rather than by id.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadNthOfType(std::uint32_t type_code, std::size_t ordinal);

// Resolves a file name inside the Nova Files data folder to an absolute path
// on disk (used for streaming assets such as background music), searching the
// known candidates for the game install. Returns nullopt if not found.
[[nodiscard]] std::optional<std::filesystem::path>
NovaResource_LocateFile(const std::string &file_name);

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
  // Ghidra: DAT_007d2524/26 loaded from c\x9alr +0xe0; the main-screen logo
  // (sp\x95n 606) anchor in the 1024x768 backdrop space.
  NovaMenuPoint logo_origin{};
  // Ghidra: c\x9alr +0xe4; sp\x95n 607 is the small center preview whose
  // frames correspond to the six menu actions plus one idle frame.
  NovaMenuPoint center_preview_origin{};
  // Ghidra: c\x9alr +0xe8/+0xec/+0xf0; sp\x95n 608-610 are the three
  // pre-rendered horizontal reveal strips behind the two button columns.
  std::array<NovaMenuPoint, 3> row_reveal_origins{};
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
NovaResource_LoadSndData(std::uint16_t resource_id);

// Ghidra: 0x004d6e60 FUN_004d6e60 + 0x004d6900 FUN_004d6900. Decodes a Nova
// "snd " resource payload into device-agnostic PCM. The menu blips (ids
// 600/601) use the uncompressed 8-bit mono 'NONE' form: FUN_004d6d30 scans the
// format-2 rate table for a sub-structure offset, and the 8-bit sample data
// starts at that offset + 0x16. The game expands each 8-bit sample to 16-bit
// by (v + 0x80) | (v + 0x80) << 8 (a quirk preserved for fidelity).
//
// The classic header's 16.16 sample rate is preserved (600/601 use about
// 11127 Hz). The format-1 extended form used by snd 602/603 contains mono Apple
// IMA4 packets; those are decoded to PCM here because the original delegated
// them to its platform audio backend. Other AIFC forms remain unsupported.
[[nodiscard]] std::optional<NovaSoundData>
NovaSound_Decode(std::span<const std::byte> resource_data);

// Ghidra: FUN_004ce250 loads the "snd " (0x736e6420) resource family from the
// Nova Sounds.rez archive. Sound ids 600..603 are the intro/travel/loading
// transition/ambience sounds preloaded by NovaAudio_PreloadTransitionEffects
// (Ghidra 0x0048b250) into DAT_007d24b8; the menu hover/confirm blips live in
// the same family.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadPictData(std::uint16_t resource_id);

// Ghidra: title-screen backdrop PICT 0x1f40 in Nova Titles 1.rez. The
// original 1024x768 artwork shows a starship interior looking out at a
// planet; it is the backdrop behind the main-menu buttons (c\x9alr button
// origins share this 1024x768 coordinate space).
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadMainMenuBackdropData();

// Ghidra: main-screen logo PICT 0x1f4a (sp\x95n 606, 654x209 tiles in a 7-frame
// vertical sheet). The animated "ESCAPE VELOCITY: NOVA" title drawn above the
// backdrop; its on-screen position comes from the c\x9alr offsets at +0xe0.
[[nodiscard]] std::optional<std::vector<std::byte>>
NovaResource_LoadMainMenuLogoData();
