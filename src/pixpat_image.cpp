#include "pixpat_image.hpp"

#include "log.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  if (offset + 2 > bytes.size()) {
    return 0;
  }
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])
                                    << 8U) |
         static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[offset + 1]));
}

[[nodiscard]] std::uint32_t ReadBe32(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  if (offset + 4 > bytes.size()) {
    return 0;
  }
  return std::to_integer<std::uint32_t>(bytes[offset]) << 24U |
         std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16U |
         std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8U |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

// The original byteswaps each 32-bit resource offset and then sign-extends the
// low half before adding it to the payload base (the `(short)` casts in
// Resource_LoadPixPatAsImage 0x0087293e). A non-positive result is rejected
// there too.
[[nodiscard]] std::ptrdiff_t ShortOffset(std::uint32_t value) {
  return static_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}

} // namespace

std::optional<PictImage>
Resource_LoadPixPatAsImage(std::span<const std::byte> ppat_data) {
  // Ghidra 0x004bbd50 Resource_LoadPixPat (wrapper) -> 0x004fdf40
  // thunk_Resource_LoadPixPatAsImage / 0x0087293e Resource_LoadPixPatAsImage
  // (the PixPat decode; runs inline below).
  if (ppat_data.size() < 0x2c || ReadBe16(ppat_data, 0) != 1) {
    NovaLog::Warn("ppat: not a version-1 pixel pattern ({} bytes)",
                  ppat_data.size());
    return std::nullopt;
  }
  const std::ptrdiff_t map_base = ShortOffset(ReadBe32(ppat_data, 2));
  const std::ptrdiff_t pixels_offset = ShortOffset(ReadBe32(ppat_data, 6));
  if (map_base <= 0 || pixels_offset <= 0) {
    NovaLog::Warn("ppat: invalid PixMap/pixel offsets (map {}, pixels {})",
                  map_base,
                  pixels_offset);
    return std::nullopt;
  }
  const auto map = static_cast<std::size_t>(map_base);
  if (map + 0x2e > ppat_data.size()) {
    NovaLog::Warn("ppat: PixMap header out of range (offset {})", map);
    return std::nullopt;
  }

  const std::uint16_t row_bytes_word = ReadBe16(ppat_data, map + 4);
  if ((row_bytes_word & 0xc000U) != 0x8000U) {
    NovaLog::Warn("ppat: unsupported rowBytes flags (0x{:04x})",
                  row_bytes_word);
    return std::nullopt;
  }
  const int top = ReadBe16(ppat_data, map + 6);
  const int left = ReadBe16(ppat_data, map + 8);
  const int bottom = ReadBe16(ppat_data, map + 0xa);
  const int right = ReadBe16(ppat_data, map + 0xc);
  const int width = right - left;
  const int height = bottom - top;
  if (width <= 0 || height <= 0 || width > 0x1000 || height > 0x1000) {
    NovaLog::Warn("ppat: invalid bounds {}x{}", width, height);
    return std::nullopt;
  }
  // hRes/vRes must be zero in the forms the decoder accepts.
  if (ReadBe16(ppat_data, map + 0xe) != 0 ||
      ReadBe16(ppat_data, map + 0x10) != 0) {
    NovaLog::Warn("ppat: unsupported resolution words");
    return std::nullopt;
  }
  const int bits_per_pixel = ReadBe16(ppat_data, map + 0x20);
  // DAT table 0x116 accepts exactly depths 1, 2, 4 and 8.
  if (bits_per_pixel > 8 || ((0x116U >> bits_per_pixel) & 1U) == 0) {
    NovaLog::Warn("ppat: unsupported bits-per-pixel {}", bits_per_pixel);
    return std::nullopt;
  }
  const std::ptrdiff_t table_offset =
      ShortOffset(ReadBe32(ppat_data, map + 0x2a));
  if (table_offset <= 0) {
    NovaLog::Warn("ppat: missing ColorTable");
    return std::nullopt;
  }
  const int row_bytes = row_bytes_word & 0x3fff;
  if (row_bytes <= 0) {
    NovaLog::Warn("ppat: zero rowBytes");
    return std::nullopt;
  }

  // Unpack the MSB-first colour indices. Each row is `row_bytes` payload bytes
  // and `8 / bits_per_pixel` pixels share a byte.
  const int pixels_per_byte = 8 / bits_per_pixel;
  const int index_mask = (1 << bits_per_pixel) - 1;
  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  std::vector<std::uint8_t> indices(pixel_count);
  for (int y = 0; y < height; ++y) {
    const std::size_t row_start =
        static_cast<std::size_t>(pixels_offset) +
        static_cast<std::size_t>(y) * static_cast<std::size_t>(row_bytes);
    for (int x = 0; x < width; ++x) {
      const std::size_t src =
          row_start + static_cast<std::size_t>(x / pixels_per_byte);
      if (src >= ppat_data.size()) {
        NovaLog::Warn("ppat: pixel raster out of range (row {})", y);
        return std::nullopt;
      }
      const std::uint8_t byte = std::to_integer<std::uint8_t>(ppat_data[src]);
      const int shift =
          (8 - bits_per_pixel) - (x % pixels_per_byte) * bits_per_pixel;
      indices[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
              static_cast<std::size_t>(x)] =
          static_cast<std::uint8_t>((byte >> shift) & index_mask);
    }
  }

  // ColorTable -> RGB888 palette. Entries absent from the table (and any index
  // past 255) stay black, matching the original's zero-initialized buffer.
  std::array<std::uint8_t, 256 * 3> palette{};
  {
    const auto table = static_cast<std::size_t>(table_offset);
    if (table + 8 > ppat_data.size()) {
      NovaLog::Warn("ppat: ColorTable header out of range (offset {})", table);
      return std::nullopt;
    }
    const std::size_t entry_count = ReadBe16(ppat_data, table + 6);
    for (std::size_t i = 0; i < entry_count; ++i) {
      const std::size_t entry = table + 8 + i * 8;
      if (entry + 8 > ppat_data.size()) {
        NovaLog::Warn("ppat: truncated ColorTable");
        return std::nullopt;
      }
      const std::size_t index = ReadBe16(ppat_data, entry);
      if (index < 256) {
        palette[index * 3] =
            std::to_integer<std::uint8_t>(ppat_data[entry + 3]);
        palette[index * 3 + 1] =
            std::to_integer<std::uint8_t>(ppat_data[entry + 5]);
        palette[index * 3 + 2] =
            std::to_integer<std::uint8_t>(ppat_data[entry + 7]);
      }
    }
  }

  PictImage out;
  out.width = width;
  out.height = height;
  out.rgba_pixels.resize(pixel_count * 4U);
  for (std::size_t i = 0; i < pixel_count; ++i) {
    const std::size_t index = indices[i];
    out.rgba_pixels[i * 4U] = palette[index * 3];
    out.rgba_pixels[i * 4U + 1] = palette[index * 3 + 1];
    out.rgba_pixels[i * 4U + 2] = palette[index * 3 + 2];
    out.rgba_pixels[i * 4U + 3] = 255;
  }
  return out;
}
