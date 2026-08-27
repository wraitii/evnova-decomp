#include "pict_image.hpp"
#include "log.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return std::to_integer<std::uint16_t>(bytes[offset]) << 8U |
         std::to_integer<std::uint16_t>(bytes[offset + 1]);
}

struct DirectBitsOpcode {
  std::size_t payload = 0;
  std::uint16_t opcode = 0;
};

[[nodiscard]] std::optional<std::size_t>
FindClassicBitsRect(std::span<const std::byte> bytes) {
  // The small preference arrow PICTs are classic bitmap pictures. Their
  // BitsRect opcode is byte-aligned at an odd offset after the v1 header, so
  // they cannot be handled by the word-aligned DirectBitsRect scanner.
  for (std::size_t offset = 10; offset + 1 < bytes.size(); ++offset) {
    if (std::to_integer<std::uint8_t>(bytes[offset]) == 0x90) {
      return offset + 1;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<DirectBitsOpcode>
FindDirectBitsRect(std::span<const std::byte> bytes) {
  // PICT v2 opcodes are word-aligned relative to the start of the resource.
  // This remains a deliberately narrow scanner, but avoiding odd offsets keeps
  // pixel/payload bytes from being mistaken for an opcode in the observed
  // DirectBitsRect resources.
  for (std::size_t offset = 10; offset + 2 <= bytes.size(); offset += 2) {
    // The original decoder accepts both DirectBitsRect opcodes used by the
    // shipped resources. Most PICTs use 0x9a; the Key Settings backdrop
    // (PICT 0x8b) uses the older 0x99 spelling with the same pixmap payload.
    if (ReadBe16(bytes, offset) == 0x0099 ||
        ReadBe16(bytes, offset) == 0x009a) {
      return DirectBitsOpcode{offset + 2, ReadBe16(bytes, offset)};
    }
  }
  return std::nullopt;
}

// Ghidra 0x004fcc00 Pict_DecodePixmapRows (row decoder referenced throughout).
[[nodiscard]] bool DecodePackBitsRow(std::span<const std::byte> encoded,
                                     std::span<std::uint8_t> output,
                                     std::size_t unit_size) {
  std::size_t source = 0;
  std::size_t destination = 0;
  while (source < encoded.size() && destination < output.size()) {
    const auto control = static_cast<std::int8_t>(
        std::to_integer<std::uint8_t>(encoded[source++]));
    if (control >= 0) {
      const auto count = (static_cast<std::size_t>(control) + 1) * unit_size;
      if (source + count > encoded.size() ||
          destination + count > output.size())
        return false;
      for (std::size_t index = 0; index < count; ++index)
        output[destination++] =
            std::to_integer<std::uint8_t>(encoded[source++]);
    } else if (control != -128) {
      const auto count = static_cast<std::size_t>(1 - control);
      if (source + unit_size > encoded.size() ||
          destination + count * unit_size > output.size())
        return false;
      const auto unit = encoded.subspan(source, unit_size);
      source += unit_size;
      for (std::size_t index = 0; index < count; ++index) {
        for (std::size_t component = 0; component < unit_size; ++component)
          output[destination++] =
              std::to_integer<std::uint8_t>(unit[component]);
      }
    }
  }
  // The game's FUN_004fcc00 stops when the source is consumed and does not
  // require a row to fill its full row-bytes (trailing padding is allowed), so
  // only require that all encoded bytes were consumed without overflowing.
  return source == encoded.size() && destination <= output.size();
}

// Ghidra FUN_004fcc00 row-length rule: the row length is stored as ONE byte
// when the pixmap row-byte count is <= 0xfa, otherwise as a big-endian 2-byte
// value. Return (length, bytes_consumed).
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
ReadRowLength(std::span<const std::byte> bytes,
              std::size_t source,
              std::size_t row_bytes) {
  if (source + 2 > bytes.size())
    return std::nullopt;
  if (row_bytes <= 0xfa) {
    return std::make_pair(
        static_cast<std::size_t>(std::to_integer<std::uint8_t>(bytes[source])),
        std::size_t{1});
  }
  const auto length = static_cast<std::size_t>(ReadBe16(bytes, source));
  if (source + 2 + length > bytes.size())
    return std::nullopt;
  return std::make_pair(length, std::size_t{2});
}

} // namespace

namespace {

[[nodiscard]] std::optional<PictImage>
DecodeClassicBitsRect(std::span<const std::byte> pict_data,
                      std::size_t payload) {
  constexpr std::size_t kBitmapHeaderSize = 28;
  if (payload + kBitmapHeaderSize > pict_data.size()) {
    return std::nullopt;
  }
  const auto row_bytes =
      static_cast<std::size_t>(ReadBe16(pict_data, payload) & 0x3fffU);
  const auto top = ReadBe16(pict_data, payload + 2);
  const auto left = ReadBe16(pict_data, payload + 4);
  const auto bottom = ReadBe16(pict_data, payload + 6);
  const auto right = ReadBe16(pict_data, payload + 8);
  const auto width = static_cast<std::size_t>(right - left);
  const auto height = static_cast<std::size_t>(bottom - top);
  if (row_bytes == 0 || bottom <= top || right <= left ||
      width > row_bytes * 8U ||
      width > std::numeric_limits<std::size_t>::max() / 4U / height) {
    return std::nullopt;
  }

  // BitsRect: rowBytes, bounds, source rect, destination rect, transfer mode.
  const std::size_t source = payload + kBitmapHeaderSize;
  if (source + row_bytes * height > pict_data.size()) {
    return std::nullopt;
  }

  PictImage image;
  image.width = static_cast<int>(width);
  image.height = static_cast<int>(height);
  image.rgba_pixels.resize(width * height * 4, 0);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      const auto packed = std::to_integer<std::uint8_t>(
          pict_data[source + y * row_bytes + x / 8U]);
      const bool set = (packed & (1U << (7U - (x % 8U)))) != 0;
      const auto destination = (y * width + x) * 4U;
      if (set) {
        // The arrow pictures are monochrome mask-like art: preserve the
        // background beneath the zero bits and draw the set bits in the Nova
        // dialog highlight colour at upload time's opaque white.
        image.rgba_pixels[destination] = 255;
        image.rgba_pixels[destination + 1] = 255;
        image.rgba_pixels[destination + 2] = 255;
        image.rgba_pixels[destination + 3] = 255;
      }
    }
  }
  return image;
}

} // namespace

// Ghidra 0x004b9050 Resource_LoadPictAsImage. The WithColorRemap variant
// [0x004b8ed0] shares this decode path; its optional post-load remap through
// the DAT_0085faee table is NOT ported here. That remap is confirmed dead
// code at runtime: both direct callers of 0x004b8ed0 (0x004b9050 and
// FUN_004b9060) pass a zero remap flag, so the post-load color-remap branch
// is never taken and omitting it introduces no visual divergence.
std::optional<PictImage>
Resource_LoadPictAsImage(std::span<const std::byte> pict_data) {
  constexpr std::size_t source_and_destination_rects_size = 18;
  const auto direct_bits = FindDirectBitsRect(pict_data);
  if (!direct_bits) {
    if (const auto classic_bits = FindClassicBitsRect(pict_data)) {
      if (const auto image = DecodeClassicBitsRect(pict_data, *classic_bits)) {
        return image;
      }
    }
    NovaLog::Todo("unsupported PICT: no DirectBitsRect opcode");
    return std::nullopt;
  }

  const auto base = direct_bits->payload;
  // 0x99 stores the PixMap without its four-byte baseAddr; 0x9a includes that
  // field before the same 46-byte PixMap header. PICT 0x8b is the former. The
  // size below is the PixMap header itself; the optional baseAddr is already
  // represented by pixmap_base.
  const bool compact_pixmap = direct_bits->opcode == 0x0099;
  const std::size_t pixmap_base = base + (compact_pixmap ? 0 : 4);
  constexpr std::size_t pixmap_size = 46;
  if (pixmap_base + pixmap_size + source_and_destination_rects_size >
      pict_data.size()) {
    NovaLog::Todo("unsupported PICT: truncated PixMap header");
    return std::nullopt;
  }
  // The 0x8b resource stores 0x804a here; the high bit is a PixMap flag and
  // the actual packed row is 74 bytes.
  const auto row_bytes =
      static_cast<std::size_t>(ReadBe16(pict_data, pixmap_base) & 0x3fffU);
  const auto top = ReadBe16(pict_data, pixmap_base + 2);
  const auto left = ReadBe16(pict_data, pixmap_base + 4);
  const auto bottom = ReadBe16(pict_data, pixmap_base + 6);
  const auto right = ReadBe16(pict_data, pixmap_base + 8);
  const auto pixel_size = ReadBe16(pict_data, pixmap_base + 28);
  const auto component_count = ReadBe16(pict_data, pixmap_base + 30);
  const auto width = static_cast<std::size_t>(right - left);
  const auto height = static_cast<std::size_t>(bottom - top);
  if (bottom <= top || right <= left ||
      width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      height > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      width > std::numeric_limits<std::size_t>::max() / 4 / height) {
    NovaLog::Todo("unsupported PICT DirectBitsRect dimensions");
    return std::nullopt;
  }

  // Decoded row layout per bit depth, matching the game's FUN_004fcc00:
  // 16-bit stays 2 bytes/pixel (big-endian 5-5-5), 32-bit expands to 4
  // bytes/pixel. The packbits unit is 2 for 16-bit and 1 for 32-bit.
  std::size_t unit_size = 1;
  std::size_t row_out_bytes = 0;
  enum class PixelFormat {
    kUnsupported,
    kMonochrome,
    kRgb555,
    kRgb24Planar
  } format = PixelFormat::kUnsupported;
  if (pixel_size == 1 && component_count == 1) {
    unit_size = 1;
    row_out_bytes = row_bytes;
    format = PixelFormat::kMonochrome;
  } else if (pixel_size == 16 && component_count == 3) {
    unit_size = 2;
    row_out_bytes = width * 2;
    format = PixelFormat::kRgb555;
  } else if (pixel_size == 32 && component_count == 3) {
    // The game's FUN_004fd0a0 demotes a 3-component 32-bit source to 24-bit
    // (depth 0x18) before row decode; FUN_004fcc00 then decodes the row with
    // packbits unit 1 into width*3 bytes laid out as R, then G, then B planes
    // and interleaves them. rowBytes is still width*4 (the pixmap's), which
    // only controls whether the row length is 1 or 2 bytes.
    unit_size = 1;
    row_out_bytes = width * 3;
    format = PixelFormat::kRgb24Planar;
  } else {
    NovaLog::Todo(
        "unsupported PICT DirectBitsRect layout ({}-bit, {} components)",
        pixel_size,
        component_count);
    return std::nullopt;
  }
  if (row_bytes < row_out_bytes) {
    NovaLog::Todo("PICT rowBytes {} < expected {}", row_bytes, row_out_bytes);
    return std::nullopt;
  }

  PictImage image;
  image.width = static_cast<int>(width);
  image.height = static_cast<int>(height);
  image.rgba_pixels.resize(width * height * 4);
  std::vector<std::uint8_t> row(row_bytes);
  std::array<std::array<std::uint8_t, 4>, 2> monochrome_colors{
      std::array<std::uint8_t, 4>{0, 0, 0, 255},
      std::array<std::uint8_t, 4>{255, 255, 255, 255}};
  std::size_t source = pixmap_base + pixmap_size;
  if (format == PixelFormat::kMonochrome) {
    // 1-bit DirectBitsRect resources carry a ColorTable between the PixMap
    // and the source/destination rectangles. PICT 0x8b uses two entries.
    if (source + 8 > pict_data.size()) {
      NovaLog::Todo("PICT monochrome color table is truncated");
      return std::nullopt;
    }
    const auto color_count =
        static_cast<std::size_t>(ReadBe16(pict_data, source + 6)) + 1;
    if (color_count > 256 || source + 8 + color_count * 8 > pict_data.size()) {
      NovaLog::Todo("unsupported PICT monochrome color table");
      return std::nullopt;
    }
    for (std::size_t entry = 0; entry < color_count; ++entry) {
      const std::size_t offset = source + 8 + entry * 8;
      const auto index = ReadBe16(pict_data, offset);
      if (index >= monochrome_colors.size()) {
        continue;
      }
      monochrome_colors[index] = {
          static_cast<std::uint8_t>(ReadBe16(pict_data, offset + 2) >> 8U),
          static_cast<std::uint8_t>(ReadBe16(pict_data, offset + 4) >> 8U),
          static_cast<std::uint8_t>(ReadBe16(pict_data, offset + 6) >> 8U),
          255};
    }
    source += 8 + color_count * 8;
  }
  source += source_and_destination_rects_size;
  if (compact_pixmap) {
    // DirectBitsRect 0x99 places a byte-counted preamble in front of the
    // PackBits rows. Pict_ParseDirectBitsRect advances over that block before
    // passing the stream to Pict_DecodePixmapRows.
    if (source + 2 > pict_data.size()) {
      NovaLog::Todo("PICT 0x99 row preamble is truncated");
      return std::nullopt;
    }
    const auto preamble_size =
        static_cast<std::size_t>(ReadBe16(pict_data, source));
    if (source + preamble_size > pict_data.size()) {
      NovaLog::Todo("PICT 0x99 row preamble exceeds resource");
      return std::nullopt;
    }
    source += preamble_size;
  }

  // The game's row decoder FUN_004fcc00 switches on the row byte count: when
  // `rowBytes < 8` the packed row is stored RAW (no per-row length prefix and
  // no packbits) and rowBytes bytes are copied straight into the output row
  // (param_3 < 8 branch), advancing the source by rowBytes each row. Only
  // wider rows (>= 8 bytes) carry the length-prefixed packbits payload. The
  // 2px-wide three-state button middle tiles (rowBytes = 4) hit the raw path,
  // so the always-packbits path below rejected them with a bogus "packbits
  // decode failed" and the button bodies never rendered.
  const bool raw_rows = row_bytes < 8;
  for (int y = 0; y < image.height; ++y) {
    if (raw_rows) {
      // Raw copy: no length prefix, no packbits. Guard the tail so a
      // truncated resource is rejected rather than over-read.
      if (source + row_bytes > pict_data.size()) {
        NovaLog::Todo("PICT row {}: raw data truncated", y);
        return std::nullopt;
      }
      for (std::size_t n = 0; n < row_bytes; ++n) {
        row[n] = std::to_integer<std::uint8_t>(pict_data[source + n]);
      }
      source += row_bytes;
    } else {
      const auto row_length = ReadRowLength(pict_data, source, row_bytes);
      if (!row_length) {
        NovaLog::Todo("PICT row {}: bad length at {}", y, source);
        return std::nullopt;
      }
      source += row_length->second;
      const auto packed = pict_data.subspan(source, row_length->first);
      if (!DecodePackBitsRow(packed, row, unit_size)) {
        NovaLog::Todo("PICT row {}: packbits decode failed", y);
        return std::nullopt;
      }
      source += row_length->first;
    }
    for (std::size_t x = 0; x < width; ++x) {
      const auto destination = (static_cast<std::size_t>(y) * width + x) * 4;
      if (format == PixelFormat::kMonochrome) {
        const auto byte = row[x / 8];
        const auto color_index = (byte >> (7U - (x % 8U))) & 1U;
        const auto color = monochrome_colors[color_index];
        image.rgba_pixels[destination] = color[0];
        image.rgba_pixels[destination + 1] = color[1];
        image.rgba_pixels[destination + 2] = color[2];
        image.rgba_pixels[destination + 3] = color[3];
      } else if (format == PixelFormat::kRgb555) {
        const auto pixel =
            static_cast<std::uint16_t>(row[x * 2]) << 8U | row[x * 2 + 1];
        image.rgba_pixels[destination] =
            static_cast<std::uint8_t>(((pixel >> 10U) & 31U) * 255U / 31U);
        image.rgba_pixels[destination + 1] =
            static_cast<std::uint8_t>(((pixel >> 5U) & 31U) * 255U / 31U);
        image.rgba_pixels[destination + 2] =
            static_cast<std::uint8_t>((pixel & 31U) * 255U / 31U);
        image.rgba_pixels[destination + 3] = 255;
      } else {
        // RG B planar interleave (FUN_004fcc00, depth 0x18).
        image.rgba_pixels[destination] = static_cast<std::uint8_t>(row[x]);
        image.rgba_pixels[destination + 1] =
            static_cast<std::uint8_t>(row[width + x]);
        image.rgba_pixels[destination + 2] =
            static_cast<std::uint8_t>(row[width * 2 + x]);
        image.rgba_pixels[destination + 3] = 255;
      }
    }
  }
  return image;
}
