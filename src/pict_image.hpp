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

// Ghidra: 0x004b8ed0 Resource_LoadPictAsImageWithColorRemap. Initial scope:
// PICT v2 DirectBitsRect, 16-bit RGB555.
[[nodiscard]] std::optional<PictImage>
Resource_LoadPictAsImage(std::span<const std::byte> pict_data);
