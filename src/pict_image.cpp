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
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint16_t>(bytes[offset]) << 8U) |
      std::to_integer<std::uint16_t>(bytes[offset + 1]));
}

// Ghidra DAT_00570344: operand-size table consumed by the
// Pict_ParseDirectBitsRect opcode walker. Entry n is the payload byte count
// following PICT opcode n; a value of -1 means "skip 2 bytes" (opcode with an
// unknown/variable payload is re-synced past a halfword). Only opcodes below
// 0xa2 reach the table; the BitsRect family, version markers and the other
// special cases are handled by the walker itself.
constexpr std::array<std::int32_t, 0xa2> kOpcodePayloadSize = {
    0,  0,  8,  2,  // 0x00
    2,  2,  4,  4,  // 0x04
    2,  8,  8,  4,  // 0x08
    4,  2,  4,  4,  // 0x0c
    8,  1,  0,  0,  // 0x10
    0,  2,  2,  0,  // 0x14
    0,  0,  6,  6,  // 0x18
    0,  6,  0,  6,  // 0x1c
    8,  4,  6,  2,  // 0x20
    -1, -1, -1, -1, // 0x24
    0,  0,  0,  0,  // 0x28
    -1, -1, -1, -1, // 0x2c
    8,  8,  8,  8,  // 0x30
    8,  8,  8,  8,  // 0x34
    0,  0,  0,  0,  // 0x38
    0,  0,  0,  0,  // 0x3c
    8,  8,  8,  8,  // 0x40
    8,  8,  8,  8,  // 0x44
    0,  0,  0,  0,  // 0x48
    0,  0,  0,  0,  // 0x4c
    8,  8,  8,  8,  // 0x50
    8,  8,  8,  8,  // 0x54
    0,  0,  0,  0,  // 0x58
    0,  0,  0,  0,  // 0x5c
    12, 12, 12, 12, // 0x60
    12, 12, 12, 12, // 0x64
    4,  4,  4,  4,  // 0x68
    4,  4,  4,  4,  // 0x6c
    0,  0,  0,  0,  // 0x70
    0,  0,  0,  0,  // 0x74
    0,  0,  0,  0,  // 0x78
    0,  0,  0,  0,  // 0x7c
    0,  0,  0,  0,  // 0x80
    0,  0,  0,  0,  // 0x84
    0,  0,  0,  0,  // 0x88
    0,  0,  0,  0,  // 0x8c
    0,  0,  -1, -1, // 0x90 (0x90/0x91 handled by the walker)
    -1, -1, -1, -1, // 0x94
    -1, -1, -1, -1, // 0x98 (0x98-0x9b handled by the walker)
    -1, -1, -1, -1, // 0x9c
    2,  0,          // 0xa0
};

// Ghidra 0x004fd0a0 Pict_ParseDirectBitsRect (opcode scan). Walks the PICT v2
// opcode stream from the 0x0011 0x02FF version marker, skipping opcode
// payloads exactly as the original does (fixed sizes from DAT_00570344,
// length-prefixed blocks, and the special-cased opcodes), until a BitsRect
// family opcode (0x90/0x91/0x98-0x9b) is reached. Image dimensions start at
// the v2 frame rect and are overridden by the 10-byte rect blocks carried by
// opcode 0x0001; the BitsRect PixMap bounds are never consulted for size.
// The original also walks version-1 streams here; those reach the classic
// scanner in the caller instead.
struct BitsRectLocation {
  std::size_t payload = 0;
  std::uint16_t opcode = 0;
  std::size_t width = 0;
  std::size_t height = 0;
};

[[nodiscard]] std::optional<BitsRectLocation>
WalkToBitsRect(std::span<const std::byte> bytes) {
  if (bytes.size() < 12) {
    return std::nullopt;
  }
  std::size_t version_pos = 10;
  while (version_pos < bytes.size() &&
         std::to_integer<std::uint8_t>(bytes[version_pos]) == 0) {
    ++version_pos;
  }
  if (version_pos + 3 > bytes.size() ||
      std::to_integer<std::uint8_t>(bytes[version_pos]) != 0x11) {
    return std::nullopt;
  }
  const auto version = std::to_integer<std::uint8_t>(bytes[version_pos + 1]);
  if (version != 2 ||
      std::to_integer<std::uint8_t>(bytes[version_pos + 2]) != 0xff) {
    // Version 1 pictures (and unknown versions) fall back to the classic
    // scanner in the caller.
    return std::nullopt;
  }

  std::int32_t width = static_cast<std::int16_t>(ReadBe16(bytes, 8)) -
                       static_cast<std::int16_t>(ReadBe16(bytes, 4));
  std::int32_t height = static_cast<std::int16_t>(ReadBe16(bytes, 6)) -
                        static_cast<std::int16_t>(ReadBe16(bytes, 2));

  std::size_t pos = version_pos + 3;
  while (pos + 2 <= bytes.size()) {
    if ((pos & 1U) != 0U) {
      // The original consumes one pad byte when the stream drifts off the
      // word alignment before reading the next opcode.
      ++pos;
      if (pos + 2 > bytes.size()) {
        break;
      }
    }
    const auto opcode = ReadBe16(bytes, pos);
    pos += 2;
    if (opcode < 0xa2) {
      if (opcode == 0x0001) {
        if (pos + 2 > bytes.size()) {
          return std::nullopt;
        }
        const auto block_size = ReadBe16(bytes, pos);
        // A 10-byte block whose payload parses as a rect updates the image
        // dimensions (the guarded branch of the original).
        if (block_size == 10 && pos + 10 <= bytes.size()) {
          const auto top = ReadBe16(bytes, pos + 2);
          const auto left = ReadBe16(bytes, pos + 4);
          const auto bottom = ReadBe16(bytes, pos + 6);
          const auto right = ReadBe16(bytes, pos + 8);
          if (bottom > top && right > left) {
            width = right - left;
            height = bottom - top;
          }
        }
        if (pos + block_size > bytes.size()) {
          return std::nullopt;
        }
        pos += block_size;
        continue;
      }
      if (opcode == 0x0012 || opcode == 0x0013 || opcode == 0x0014) {
        NovaLog::Todo("unsupported PICT pattern opcode {:#06x}", opcode);
        return std::nullopt;
      }
      if (opcode == 0x001b) {
        pos += 6;
        continue;
      }
      if (opcode >= 0x0070 && opcode <= 0x0077) {
        if (pos + 2 > bytes.size()) {
          return std::nullopt;
        }
        pos += ReadBe16(bytes, pos);
        continue;
      }
      if ((opcode >= 0x0090 && opcode <= 0x0091) ||
          (opcode >= 0x0098 && opcode <= 0x009b)) {
        if (width <= 0 || height <= 0) {
          return std::nullopt;
        }
        return BitsRectLocation{pos,
                                opcode,
                                static_cast<std::size_t>(width),
                                static_cast<std::size_t>(height)};
      }
      if (opcode == 0x00a1) {
        if (pos + 4 > bytes.size()) {
          return std::nullopt;
        }
        pos += ReadBe16(bytes, pos + 2) + 4U;
        continue;
      }
      const auto skip = kOpcodePayloadSize[opcode];
      pos += skip < 0 ? 2U : static_cast<std::size_t>(skip);
      continue;
    }
    if (opcode == 0x0c00) {
      pos += 24;
      continue;
    }
    if (opcode == 0x0028) {
      if (pos + 5 > bytes.size()) {
        return std::nullopt;
      }
      pos += 5U + std::to_integer<std::uint8_t>(bytes[pos + 4]);
      continue;
    }
    if (opcode == 0x8200 || opcode == 0x8201) {
      // TODO(decomp(0x004fd0a0)) skipped: QuickTime-compressed PICT payloads;
      // the original hands them to the QuickTime decompressor. No shipped
      // Nova dialog PICT uses this encoding.
      NovaLog::Todo("QuickTime-compressed PICT unsupported");
      return std::nullopt;
    }
    if (opcode == 0x00ff || opcode == 0xffff) {
      // End of picture without an image opcode.
      return std::nullopt;
    }
    if ((opcode >= 0x00d0 && opcode <= 0x00fe) || opcode >= 0x8100) {
      if (pos + 2 > bytes.size()) {
        return std::nullopt;
      }
      pos += ReadBe16(bytes, pos);
      continue;
    }
    if (opcode > 0x00ff && opcode < 0x8000) {
      pos += (opcode >> 7) & 0xffU;
      continue;
    }
    // [0x00a2,0x00af], [0x00b0,0x00cf] and [0x8000,0x80ff] carry no payload.
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

// The small preference arrow PICTs are classic bitmap pictures. Their
// BitsRect opcode is byte-aligned at an odd offset after the v1 header, so
// they cannot be handled by the word-aligned v2 opcode walker; the original
// walks those streams with single-byte opcodes, which this narrow scanner
// approximates by searching for the 0x90 BitsRect byte directly.
[[nodiscard]] std::optional<std::size_t>
FindClassicBitsRect(std::span<const std::byte> bytes) {
  for (std::size_t offset = 10; offset + 1 < bytes.size(); ++offset) {
    if (std::to_integer<std::uint8_t>(bytes[offset]) == 0x90) {
      return offset + 1;
    }
  }
  return std::nullopt;
}

using Palette = std::array<std::array<std::uint8_t, 3>, 256>;

// Ghidra DAT_005705cc: the game's built-in 8-bit palette, copied over any
// image that decodes without an inline ColorTable. White entry 0, black rest.
[[nodiscard]] Palette DefaultPalette() {
  Palette palette{};
  palette[0] = {255, 255, 255};
  return palette;
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
  const auto bits = WalkToBitsRect(pict_data);
  if (!bits) {
    if (const auto classic_bits = FindClassicBitsRect(pict_data)) {
      if (const auto image = DecodeClassicBitsRect(pict_data, *classic_bits)) {
        return image;
      }
    }
    NovaLog::Todo("unsupported PICT: no BitsRect opcode");
    return std::nullopt;
  }

  const auto opcode = bits->opcode;
  // DirectPixMapRect (0x9a/0x9b) stores a 4-byte baseAddr before rowBytes and
  // never carries an inline ColorTable; the BitsRect/PackBitsRect variants
  // (0x90/0x91/0x98/0x99) start at rowBytes and do.
  const bool direct = opcode == 0x009a || opcode == 0x009b;
  const bool region_variant =
      opcode == 0x0091 || opcode == 0x0099 || opcode == 0x009b;
  const auto width = bits->width;
  const auto height = bits->height;

  std::size_t pos = bits->payload;
  if (direct) {
    pos += 4;
  }
  if (pos + 2 > pict_data.size()) {
    NovaLog::Todo("unsupported PICT: truncated rowBytes field");
    return std::nullopt;
  }
  const auto row_bytes_raw = ReadBe16(pict_data, pos);
  const auto row_bytes = static_cast<std::size_t>(row_bytes_raw & 0x7fffU);
  // rowBytes + bounds; the game takes the image dimensions from the frame
  // (and opcode-0x0001 blocks), never from the PixMap bounds.
  pos += 10;

  // Ghidra 0x004FCA30 FUN_004fca30 runs inline here: instead of copying and
  // byte-swapping the 36-byte PixMap body, the port reads its big-endian fields
  // directly from the resource stream.
  // The PixMap body follows the bounds for DirectPixMapRect and for any
  // packed (high rowBytes bit) variant; plain 1-bit BitMaps stop at bounds.
  std::size_t pixel_size = 1;
  std::size_t component_count = 1;
  if (direct || (row_bytes_raw & 0x8000U) != 0) {
    constexpr std::size_t pixmap_body_size = 36;
    if (pos + pixmap_body_size > pict_data.size()) {
      NovaLog::Todo("unsupported PICT: truncated PixMap header");
      return std::nullopt;
    }
    pixel_size = ReadBe16(pict_data, pos + 18);
    component_count = ReadBe16(pict_data, pos + 20);
    pos += pixmap_body_size;
  }

  // Decoded row layout per bit depth, matching the game's FUN_004fcc00:
  // 8-bit indexed expands through the palette, 16-bit stays 2 bytes/pixel
  // (big-endian 5-5-5), 32-bit expands to 4 bytes/pixel. The packbits unit is
  // 2 for 16-bit and 1 otherwise.
  enum class PixelFormat {
    kIndexed8,
    kMonochrome,
    kRgb555,
    kRgb24Planar,
  };
  PixelFormat format = PixelFormat::kIndexed8;
  std::size_t unit_size = 1;
  std::size_t row_out_bytes = 0;
  if (pixel_size == 8 && component_count == 1) {
    row_out_bytes = width;
    format = PixelFormat::kIndexed8;
  } else if (pixel_size == 1 && component_count == 1) {
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
    row_out_bytes = width * 3;
    format = PixelFormat::kRgb24Planar;
  } else {
    // TODO(decomp(0x004fcc00)) skipped: 32-bit 4-component sources take the
    // unported depth-0x20 row path; no shipped Nova resource uses it.
    NovaLog::Todo("unsupported PICT BitsRect layout ({}-bit, {} components)",
                  pixel_size,
                  component_count);
    return std::nullopt;
  }
  if (format == PixelFormat::kMonochrome && width > row_bytes * 8U) {
    NovaLog::Todo("PICT 1-bit width {} exceeds row bytes {}", width, row_bytes);
    return std::nullopt;
  }
  if (row_bytes < row_out_bytes) {
    NovaLog::Todo("PICT rowBytes {} < expected {}", row_bytes, row_out_bytes);
    return std::nullopt;
  }

  Palette palette = DefaultPalette();
  if (!direct && (row_bytes_raw & 0x8000U) != 0) {
    // Inline ColorTable: ctSeed (4), ctFlags (2), ctSize (2), then
    // ctSize+1 entries of (value, red, green, blue) halfwords. When ctFlags
    // bit 15 is set the entry values are ignored and indexes are sequential;
    // the original clamps writes to the 256-entry palette.
    constexpr std::size_t color_table_header_size = 8;
    if (pos + color_table_header_size > pict_data.size()) {
      NovaLog::Todo("PICT color table header is truncated");
      return std::nullopt;
    }
    const auto ct_flags = ReadBe16(pict_data, pos + 4);
    const auto entry_count =
        static_cast<std::size_t>(ReadBe16(pict_data, pos + 6)) + 1U;
    if (pos + color_table_header_size + entry_count * 8 > pict_data.size()) {
      NovaLog::Todo("PICT color table ({} entries) overruns resource",
                    entry_count);
      return std::nullopt;
    }
    pos += color_table_header_size;
    const bool sequential = (ct_flags & 0x8000U) != 0;
    for (std::size_t entry = 0; entry < entry_count; ++entry) {
      const auto index =
          sequential ? entry
                     : static_cast<std::size_t>(ReadBe16(pict_data, pos));
      if (index < palette.size()) {
        palette[index] = {
            static_cast<std::uint8_t>(ReadBe16(pict_data, pos + 2) >> 8U),
            static_cast<std::uint8_t>(ReadBe16(pict_data, pos + 4) >> 8U),
            static_cast<std::uint8_t>(ReadBe16(pict_data, pos + 6) >> 8U)};
      }
      pos += 8;
    }
  }

  // Source rect, destination rect, transfer mode.
  pos += 18;
  if (region_variant) {
    // Rgn variants carry a region whose own size field includes its 2 size
    // bytes, so advancing by the value skips exactly the region.
    if (pos + 2 > pict_data.size()) {
      NovaLog::Todo("PICT region size field is truncated");
      return std::nullopt;
    }
    pos += ReadBe16(pict_data, pos);
  }
  if (pos > pict_data.size()) {
    NovaLog::Todo("PICT row data starts past the resource");
    return std::nullopt;
  }

  PictImage image;
  image.width = static_cast<int>(width);
  image.height = static_cast<int>(height);
  image.rgba_pixels.resize(width * height * 4);
  std::vector<std::uint8_t> row(row_bytes);

  // The game's row decoder FUN_004fcc00 switches on the masked row byte
  // count: when it is 0 the pixmap's expected row size is used, when < 8 the
  // packed row is stored RAW (no per-row length prefix and no packbits) and
  // copied straight into the output row, and only wider rows (>= 8 bytes)
  // carry the length-prefixed packbits payload (1-byte length for <= 0xfa,
  // else big-endian 2 bytes). The 2px-wide three-state button middle tiles
  // (rowBytes 4) take the raw path.
  const auto effective_row_bytes = row_bytes != 0 ? row_bytes : row_out_bytes;
  const bool raw_rows = effective_row_bytes < 8;
  for (std::size_t y = 0; y < height; ++y) {
    if (raw_rows) {
      // Raw copy: no length prefix, no packbits. Guard the tail so a
      // truncated resource is rejected rather than over-read.
      if (pos + effective_row_bytes > pict_data.size()) {
        NovaLog::Todo("PICT row {}: raw data truncated", y);
        return std::nullopt;
      }
      for (std::size_t n = 0; n < effective_row_bytes; ++n) {
        row[n] = std::to_integer<std::uint8_t>(pict_data[pos + n]);
      }
      pos += effective_row_bytes;
    } else {
      const auto row_length =
          ReadRowLength(pict_data, pos, effective_row_bytes);
      if (!row_length) {
        NovaLog::Todo("PICT row {}: bad length at {}", y, pos);
        return std::nullopt;
      }
      pos += row_length->second;
      const auto packed = pict_data.subspan(pos, row_length->first);
      if (!DecodePackBitsRow(packed, row, unit_size)) {
        NovaLog::Todo("PICT row {}: packbits decode failed", y);
        return std::nullopt;
      }
      pos += row_length->first;
    }
    for (std::size_t x = 0; x < width; ++x) {
      std::uint8_t red = 0;
      std::uint8_t green = 0;
      std::uint8_t blue = 0;
      if (format == PixelFormat::kIndexed8) {
        const auto color = palette[row[x]];
        red = color[0];
        green = color[1];
        blue = color[2];
      } else if (format == PixelFormat::kMonochrome) {
        // Ghidra 0x004FCAC0 FUN_004fcac0 expands 1-bit rows MSB-first into
        // 0/1 indices; its 2/4-bit arms remain unported.
        const auto byte = row[x / 8];
        const auto color = palette[(byte >> (7U - (x % 8U))) & 1U];
        red = color[0];
        green = color[1];
        blue = color[2];
      } else if (format == PixelFormat::kRgb555) {
        const auto pixel = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(row[x * 2]) << 8U) | row[x * 2 + 1]);
        red = static_cast<std::uint8_t>(((pixel >> 10U) & 31U) * 255U / 31U);
        green = static_cast<std::uint8_t>(((pixel >> 5U) & 31U) * 255U / 31U);
        blue = static_cast<std::uint8_t>((pixel & 31U) * 255U / 31U);
      } else {
        // RGB planar interleave (FUN_004fcc00, depth 0x18).
        red = row[x];
        green = row[width + x];
        blue = row[width * 2 + x];
      }
      const auto destination = (y * width + x) * 4U;
      image.rgba_pixels[destination] = red;
      image.rgba_pixels[destination + 1] = green;
      image.rgba_pixels[destination + 2] = blue;
      image.rgba_pixels[destination + 3] = 255;
    }
  }
  return image;
}
