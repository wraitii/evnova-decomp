#include "cicn_image.hpp"

#include "log.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace {
// Payload offsets (byte offsets from the start of the cicn resource), taken
// from FUN_004d2bd0's extended color-icon branch.
constexpr std::size_t kMaskRowBytesOffset = 0x04; // BE16; & 0xe000 == 0x8000
constexpr std::size_t kMaskStride = 0x8000;       // bit marking the extended form
constexpr std::size_t kReserved2Offset = 0x0e;    // must be 0 for extended form
constexpr std::size_t kReserved3Offset = 0x10;    // must be 0 for extended form
constexpr std::size_t kBitsPerPixelOffset = 0x20; // BE16: 1, 2, 4 or 8
constexpr std::size_t kMaskRowBytesField = 0x36;  // BE16 mask rowBytes (per row)
constexpr std::size_t kCompRowBytesField = 0x44;  // BE16 complementary rowBytes
constexpr std::size_t kMaskDataOffset = 0x52;     // start of the 1-bit mask bitmap

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  if (offset + 2 > bytes.size()) {
    return 0;
  }
  return static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[offset]) << 8U) |
         static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[offset + 1]));
}

[[nodiscard]] std::int16_t ReadBeI16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::int16_t>(ReadBe16(bytes, offset));
}
} // namespace

std::optional<PictImage>
Resource_LoadCicnAsImage(std::span<const std::byte> cicn_data) {
  if (cicn_data.size() < 0x54) {
    NovaLog::Warn("cicn: payload too small ({})", cicn_data.size());
    return std::nullopt;
  }
  // The extended color-icon form is selected when the high bits of the mask
  // rowbytes word are 0x8000 and the two reserved words are zero. (FUN_004d2bd0
  // rejects any other header outright.)
  const std::uint16_t mask_word = ReadBe16(cicn_data, kMaskRowBytesOffset);
  if ((mask_word & 0xe000U) != kMaskStride ||
      ReadBe16(cicn_data, kReserved2Offset) != 0 ||
      ReadBe16(cicn_data, kReserved3Offset) != 0) {
    NovaLog::Warn("cicn: unsupported (non-extended) header (mask=0x{:04x})",
                  mask_word);
    return std::nullopt;
  }

  const int width =
      ReadBeI16(cicn_data, 0x0c) - ReadBeI16(cicn_data, 0x08); // right - left
  const int height =
      ReadBeI16(cicn_data, 0x0a) - ReadBeI16(cicn_data, 0x06); // bottom - top
  if (width <= 0 || height <= 0 || width > 0x800 || height > 0x800) {
    NovaLog::Warn("cicn: invalid dimensions {}x{}", width, height);
    return std::nullopt;
  }
  const int bpp = ReadBe16(cicn_data, kBitsPerPixelOffset);
  if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8) {
    NovaLog::Warn("cicn: unsupported bits-per-pixel {}", bpp);
    return std::nullopt;
  }

  // The colour-index raster's per-row stride and the mask bitmap's row bytes.
  const std::uint16_t pixel_stride = mask_word & 0x1fffU;
  const std::uint16_t mask_row_bytes = ReadBe16(cicn_data, kMaskRowBytesField);
  // The palette count header sits at kMaskDataOffset + ((BE16@0x44) +
  // maskRowBytes) * height (iVar5 in FUN_004d2bd0).
  const std::size_t i_var5 =
      static_cast<std::size_t>(ReadBe16(cicn_data, kCompRowBytesField)) *
          static_cast<std::size_t>(height) +
      static_cast<std::size_t>(mask_row_bytes) * static_cast<std::size_t>(height);
  const std::size_t palette_header = kMaskDataOffset + i_var5;
  if (palette_header + 8 > cicn_data.size()) {
    NovaLog::Warn("cicn: palette header out of range (offset {})", palette_header);
    return std::nullopt;
  }

  // Palette: count+1 entries, each [index u16][r u8][g u8][b u8] (8 bytes).
  const std::uint16_t entry_count =
      static_cast<std::uint16_t>(ReadBe16(cicn_data, palette_header + 6) + 1U);
  const std::size_t palette_start = palette_header + 8;
  std::array<std::uint8_t, 3> colors[256]{};
  std::array<bool, 256> have_color{};
  have_color[0] = true;              // index 0 -> opaque white
  colors[0] = {255, 255, 255};
  std::size_t palette_end = palette_start;
  bool valid = true;
  for (std::uint16_t i = 0; i < entry_count; ++i) {
    const std::size_t e = palette_start + static_cast<std::size_t>(i) * 8U;
    if (e + 8 > cicn_data.size()) {
      valid = false;
      break;
    }
    const std::uint16_t index = ReadBe16(cicn_data, e);
    if (index < 256) {
      colors[index] = {std::to_integer<std::uint8_t>(cicn_data[e + 2]),
                       std::to_integer<std::uint8_t>(cicn_data[e + 4]),
                       std::to_integer<std::uint8_t>(cicn_data[e + 6])};
      have_color[index] = true;
      palette_end = e + 8;
    }
  }
  if (!valid) {
    NovaLog::Warn("cicn: truncated palette");
    return std::nullopt;
  }
  if (palette_end + static_cast<std::size_t>(height) * pixel_stride >
      cicn_data.size()) {
    NovaLog::Warn("cicn: colour raster out of range");
    return std::nullopt;
  }

  PictImage out;
  out.width = width;
  out.height = height;
  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  out.rgba_pixels.assign(pixel_count * 4U, 0);

  // Unpack the per-pixel colour index. Each index occupies `bpp` bits read in
  // most-significant-first order from the colour raster (the game's 8/4/2/1
  // bit loops in FUN_004d2bd0).
  for (int row = 0; row < height; ++row) {
    const std::size_t src =
        palette_end + static_cast<std::size_t>(row) * pixel_stride;
    auto *dst = &out.rgba_pixels[static_cast<std::size_t>(row) *
                                  static_cast<std::size_t>(width) * 4U];
    int bit = 0; // bit index (MSB-first) within the row's bytes
    for (int col = 0; col < width; ++col) {
      const std::uint8_t byte = std::to_integer<std::uint8_t>(
          cicn_data[src + static_cast<std::size_t>(bit / 8)]);
      const int shift = 8 - (bit % 8) - bpp;
      std::uint8_t index = 0;
      if (shift >= 0) {
        index = static_cast<std::uint8_t>((byte >> shift) & ((1U << bpp) - 1U));
      } else {
        // Index straddles a byte boundary: high bits from this byte, low from
        // the next.
        const int hi = -shift;
        index = static_cast<std::uint8_t>((byte << bpp) & ((1U << bpp) - 1U));
        if (src + static_cast<std::size_t>(bit / 8) + 1 < cicn_data.size()) {
          const std::uint8_t nxt = std::to_integer<std::uint8_t>(
              cicn_data[src + static_cast<std::size_t>(bit / 8) + 1]);
          index = static_cast<std::uint8_t>(
              index | ((nxt >> (8 - hi)) & ((1U << bpp) - 1U)));
        }
      }
      auto *px = &dst[static_cast<std::size_t>(col) * 4U];
      if (have_color[static_cast<std::size_t>(index)]) {
        px[0] = colors[index][0];
        px[1] = colors[index][1];
        px[2] = colors[index][2];
        px[3] = 255;
      } else {
        px[0] = px[1] = px[2] = px[3] = 0; // no colour entry -> transparent
      }
      bit += bpp;
    }
  }

  // Mask bitmap: 1 bit/pixel; a set bit means opaque, clear means transparent.
  for (int row = 0; row < height; ++row) {
    const std::size_t src =
        kMaskDataOffset + static_cast<std::size_t>(row) * mask_row_bytes;
    const std::size_t dst =
        static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4U;
    for (int x = 0; x < width; ++x) {
      const std::uint8_t bit =
          1U << (7U - static_cast<std::uint8_t>(x % 8));
      const bool set =
          (std::to_integer<std::uint8_t>(cicn_data[src + x / 8]) & bit) != 0;
      auto *px = &out.rgba_pixels[dst + static_cast<std::size_t>(x) * 4U];
      if (!set) {
        px[0] = px[1] = px[2] = px[3] = 0; // outside the icon -> transparent
      }
    }
  }
  return out;
}
