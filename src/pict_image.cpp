#include "pict_image.hpp"
#include "log.hpp"

#include <cstddef>
#include <limits>

namespace {

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return std::to_integer<std::uint16_t>(bytes[offset]) << 8U |
         std::to_integer<std::uint16_t>(bytes[offset + 1]);
}

[[nodiscard]] std::optional<std::size_t>
FindDirectBitsRect(std::span<const std::byte> bytes) {
  // PICT v2 opcodes are word-aligned relative to the start of the resource.
  // This remains a deliberately narrow scanner, but avoiding odd offsets keeps
  // pixel/payload bytes from being mistaken for an opcode in the observed
  // DirectBitsRect resources.
  for (std::size_t offset = 10; offset + 2 <= bytes.size(); offset += 2) {
    if (ReadBe16(bytes, offset) == 0x009a) {
      return offset + 2;
    }
  }
  return std::nullopt;
}

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
  return source == encoded.size() && destination == output.size();
}

} // namespace

std::optional<PictImage>
Resource_LoadPictAsImage(std::span<const std::byte> pict_data) {
  constexpr std::size_t pixmap_size = 50;
  constexpr std::size_t source_and_destination_rects_size = 18;
  const auto direct_bits = FindDirectBitsRect(pict_data);
  if (!direct_bits ||
      *direct_bits + pixmap_size + source_and_destination_rects_size >
          pict_data.size()) {
    NovaLog::Todo("unsupported PICT: no DirectBitsRect opcode");
    return std::nullopt;
  }

  const auto base = *direct_bits;
  const auto row_bytes =
      static_cast<std::size_t>(ReadBe16(pict_data, base + 4) & 0x3fffU);
  const auto top = ReadBe16(pict_data, base + 6);
  const auto left = ReadBe16(pict_data, base + 8);
  const auto bottom = ReadBe16(pict_data, base + 10);
  const auto right = ReadBe16(pict_data, base + 12);
  const auto pixel_size = ReadBe16(pict_data, base + 32);
  const auto component_count = ReadBe16(pict_data, base + 34);
  const auto width = static_cast<std::size_t>(right - left);
  const auto height = static_cast<std::size_t>(bottom - top);
  if (bottom <= top || right <= left || pixel_size != 16 ||
      component_count != 3 || row_bytes < width * 2 ||
      width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      height > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      width > std::numeric_limits<std::size_t>::max() / 4 / height) {
    NovaLog::Todo(
        "unsupported PICT DirectBitsRect layout ({}-bit, {} components)",
        pixel_size, component_count);
    return std::nullopt;
  }

  PictImage image;
  image.width = static_cast<int>(width);
  image.height = static_cast<int>(height);
  image.rgba_pixels.resize(width * height * 4);
  std::vector<std::uint8_t> row(row_bytes);
  std::size_t source = base + pixmap_size + source_and_destination_rects_size;
  for (int y = 0; y < image.height; ++y) {
    if (source + 2 > pict_data.size())
      return std::nullopt;
    const auto packed_size =
        static_cast<std::size_t>(ReadBe16(pict_data, source));
    source += 2;
    if (source + packed_size > pict_data.size() ||
        !DecodePackBitsRow(pict_data.subspan(source, packed_size), row, 2))
      return std::nullopt;
    source += packed_size;
    for (int x = 0; x < image.width; ++x) {
      const auto pixel =
          static_cast<std::uint16_t>(row[static_cast<std::size_t>(x) * 2])
              << 8U |
          row[static_cast<std::size_t>(x) * 2 + 1];
      const auto destination =
          static_cast<std::size_t>((y * image.width + x) * 4);
      image.rgba_pixels[destination] =
          static_cast<std::uint8_t>(((pixel >> 10U) & 31U) * 255U / 31U);
      image.rgba_pixels[destination + 1] =
          static_cast<std::uint8_t>(((pixel >> 5U) & 31U) * 255U / 31U);
      image.rgba_pixels[destination + 2] =
          static_cast<std::uint8_t>((pixel & 31U) * 255U / 31U);
      image.rgba_pixels[destination + 3] = 255;
    }
  }
  return image;
}
