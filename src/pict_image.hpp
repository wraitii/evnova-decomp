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

// Ghidra: 0x004b9050 Resource_LoadPictAsImage (+ optional post-load color
// remap in the WithColorRemap variant 0x004b8ed0, confirmed dead at runtime:
// all callers pass a zero remap flag) + opcode walker Pict_ParseDirectBitsRect
// (0x004fd0a0, with the DAT_00570344 operand-size table) + row decode
// Pict_DecodePixmapRows (FUN_004fcc00). Locates the BitsRect opcode by
// walking the PICT v2 opcode stream, then decodes the BitsRect family
// (0x90/0x91/0x98/0x99/0x9a/0x9b): 8-bit indexed with inline ColorTable
// (sequential-index ctFlags bit, default DAT_005705cc palette), classic 1-bit
// BitsRect form used by the 11x9 preferences arrow PICTs (0x86/0x87) and the
// Key Settings backdrop 0x8b, 16-bit 5-5-5 (2 bytes/pixel, big-endian), and
// 32-bit sources that the game demotes to its 24-bit RGB path (packbits unit
// 1, R/G/B planar interleave). Image dimensions come from the frame rect /
// opcode-0x0001 blocks, never the PixMap bounds. Row lengths and packbits
// follow the game's algorithm: the masked rowBytes decides the per-row
// prefix (1 byte for <= 0xfa, else BE16); rows narrower than 8 bytes (e.g.
// the 2px-wide three-state button middle tiles, rowBytes 4) are stored RAW
// with no length prefix and no packbits. Rgn variants (0x91/0x99/0x9b) skip
// their region block before the rows. The destination-art PICTs
// (0x2137/0x2138), the three-state button strips (0x1d4c..0x1d54 + masks
// 0x1db0..), the refuel-activity icons and the new-pilot dialog PICTs
// 0x81/0x82 all load through it.
[[nodiscard]] std::optional<PictImage>
Resource_LoadPictAsImage(std::span<const std::byte> pict_data);
