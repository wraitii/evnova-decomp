#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

struct PictImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba_pixels;
};

// Ghidra: 0x004b8ed0 Resource_LoadPictAsImageWithColorRemap (subset). Decodes
// a PICT v2 DirectBitsRect (opcodes 0x9A/0x9B) into opque RGBA. Handles
// 16-bit 5-5-5 (2 bytes/pixel, big-endian) and 32-bit sources that the game
// demotes to its 24-bit RGB path (packbits unit 1, R/G/B planar interleave,
// fun FUN_004fcc00). Row lengths and packbits follow the game's algorithm
// (1-byte length for narrow <= 0xfa rows, else BE16; unit = 2 for 16-bit,
// 1 otherwise). The destination-art PICTs (0x2137/0x2138), the three-state
// button strips (0x1d4c..0x1d54 + masks 0x1db0..) and the refuel-activity icons
// all load
// through it.
[[nodiscard]] std::optional<PictImage>
Resource_LoadPictAsImage(std::span<const std::byte> pict_data);
