#include "rle_sprite_sheet.hpp"

#include "util/byte_reader.hpp"

#include <limits>
#include <utility>

namespace {

using evnova::util::ReadBe16;
using evnova::util::ReadBe32;

constexpr std::size_t kHeaderSize = 0x10;

void StoreRgb555(std::span<std::uint8_t> rgba_pixels,
                 std::size_t pixel_index,
                 std::uint16_t pixel) {
  const auto destination = pixel_index * 4;
  rgba_pixels[destination] =
      static_cast<std::uint8_t>(((pixel >> 10U) & 31U) * 255U / 31U);
  rgba_pixels[destination + 1] =
      static_cast<std::uint8_t>(((pixel >> 5U) & 31U) * 255U / 31U);
  rgba_pixels[destination + 2] =
      static_cast<std::uint8_t>((pixel & 31U) * 255U / 31U);
  rgba_pixels[destination + 3] = 255;
}

[[nodiscard]] bool DecodeFrame(std::span<const std::byte> resource_data,
                               std::size_t &source,
                               int width,
                               int height,
                               RleSpriteFrame &frame) {
  const auto row_size = static_cast<std::size_t>(width) * 2;
  int row = -1;
  std::size_t row_position = 0;
  std::optional<std::size_t> row_end;

  while (source + 4 <= resource_data.size()) {
    const auto command = ReadBe32(resource_data, source);
    source += 4;
    const auto opcode = command >> 24U;
    const auto count = static_cast<std::size_t>(command & 0x00ffffffU);

    if (opcode == 0) {
      return (!row_end || source - 4 == *row_end) && row + 1 == height;
    }
    if (opcode == 1) {
      if ((row_end && source - 4 != *row_end) || row + 1 >= height ||
          count > resource_data.size() - source) {
        return false;
      }
      ++row;
      row_position = 0;
      row_end = source + count;
      continue;
    }
    if (row < 0 || !row_end || source > *row_end ||
        count > row_size - row_position) {
      return false;
    }

    if (opcode == 2) {
      if ((count & 1U) != 0) {
        return false;
      }
      const auto padded_count = (count + 3U) & ~std::size_t{3U};
      if (padded_count > *row_end - source) {
        return false;
      }
      for (std::size_t offset = 0; offset < count; offset += 2) {
        const auto pixel = ReadBe16(resource_data, source + offset);
        const auto pixel_index =
            static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
            (row_position + offset) / 2;
        StoreRgb555(frame.rgba_pixels, pixel_index, pixel);
      }
      source += padded_count;
    } else if (opcode == 3) {
      // The frame starts transparent; advancing the destination preserves it.
    } else if (opcode == 4) {
      if ((count & 1U) != 0 || *row_end - source < 4) {
        return false;
      }
      for (std::size_t offset = 0; offset < count; offset += 2) {
        const auto pattern_offset = offset & 2U;
        const auto pixel = ReadBe16(resource_data, source + pattern_offset);
        const auto pixel_index =
            static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
            (row_position + offset) / 2;
        StoreRgb555(frame.rgba_pixels, pixel_index, pixel);
      }
      source += 4;
    } else {
      return false;
    }
    row_position += count;
  }
  return false;
}

} // namespace

std::optional<RleSpriteSheet>
RleSpriteSheet_Decode16(std::span<const std::byte> resource_data) {
  if (resource_data.size() < kHeaderSize) {
    return std::nullopt;
  }
  const auto width = static_cast<std::size_t>(ReadBe16(resource_data, 0));
  const auto height = static_cast<std::size_t>(ReadBe16(resource_data, 2));
  const auto pixel_depth = ReadBe16(resource_data, 4);
  const auto frame_count = static_cast<std::size_t>(ReadBe16(resource_data, 8));
  if (width == 0 || height == 0 || frame_count == 0 || pixel_depth != 16 ||
      width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      height > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      width > std::numeric_limits<std::size_t>::max() / 4 / height ||
      frame_count > resource_data.size() / 4) {
    return std::nullopt;
  }

  RleSpriteSheet sheet;
  sheet.width = static_cast<int>(width);
  sheet.height = static_cast<int>(height);
  sheet.frames.reserve(frame_count);
  std::size_t source = kHeaderSize;
  for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
    RleSpriteFrame frame;
    frame.rgba_pixels.resize(width * height * 4, 0);
    if (!DecodeFrame(resource_data, source, sheet.width, sheet.height, frame)) {
      return std::nullopt;
    }
    sheet.frames.push_back(std::move(frame));
  }
  return sheet;
}
