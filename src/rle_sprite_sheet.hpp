#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

struct RleSpriteFrame {
  std::vector<std::uint8_t> rgba_pixels;
};

struct RleSpriteSheet {
  int width = 0;
  int height = 0;
  std::vector<RleSpriteFrame> frames;
};

// Ghidra: the rl\x91D resource path through
// Sprite_CreateFromMultiFrameResource (0x00474e90), the resource byte-swap
// helper (0x00472000), and the unscaled command renderer (0x00471e10).
// Decodes the original 16-bit RGB555 command streams into SDL-ready RGBA.
[[nodiscard]] std::optional<RleSpriteSheet>
RleSpriteSheet_Decode16(std::span<const std::byte> resource_data);
