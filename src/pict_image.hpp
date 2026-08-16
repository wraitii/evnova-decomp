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

// Ghidra: 0x004b8ed0 Resource_LoadPictAsImageWithColorRemap (subset) + row
// decode Pict_DecodePixmapRows (FUN_004fcc00). Decodes a PICT v2
// DirectBitsRect (opcodes 0x99/0x9A/0x9B) into opaque RGBA. Handles the compact
// 1-bit/color-table format used by Key Settings PICT 0x8b, 16-bit 5-5-5
// (2 bytes/pixel, big-endian), and 32-bit sources that the game demotes to its
// 24-bit RGB path (packbits unit 1, R/G/B planar interleave). Row lengths and
// packbits follow the game's algorithm: rows wider than 8 bytes carry a
// per-row length (1 byte for narrow <= 0xfa rows, else BE16) then packbits;
// rows narrower than 8 bytes (e.g. the 2px-wide three-state button middle
// tiles, rowBytes 4) are stored RAW and copied with no length prefix and no
// packbits. The destination-art PICTs (0x2137/0x2138), the three-state button
// strips (0x1d4c..0x1d54 + masks 0x1db0..) and the refuel-activity icons all
// load through it.
[[nodiscard]] std::optional<PictImage>
Resource_LoadPictAsImage(std::span<const std::byte> pict_data);
