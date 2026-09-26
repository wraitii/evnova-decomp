#include "mac_resource_fork.hpp"

#include "util/byte_reader.hpp"

#include <fstream>
#include <optional>
#include <span>
#include <vector>

namespace evnova::rez {
namespace {

using evnova::util::ReadBe16;
using evnova::util::ReadBe32;

constexpr std::size_t kForkHeaderSize = 16;
constexpr std::size_t kMapHeaderSize =
    28; // header copy + next-map/fref/attrs + 2 offsets
constexpr std::size_t kTypeEntrySize = 8;
constexpr std::size_t kRefEntrySize = 12;
constexpr std::uint16_t kNoName = 0xffff;
constexpr std::uint32_t kAppleSingleMagic = 0x00051600;
constexpr std::uint32_t kAppleDoubleMagic = 0x00051607;
constexpr std::uint32_t kAppleSingleVersion = 0x00020000;
constexpr std::uint32_t kAppleResourceForkEntry = 2;

[[nodiscard]] bool
InBounds(std::size_t offset, std::size_t length, std::size_t size) {
  return offset <= size && length <= size - offset;
}

struct ForkHeader {
  std::size_t data_offset = 0;
  std::size_t map_offset = 0;
  std::size_t data_length = 0;
  std::size_t map_length = 0;
};

// Validates the 16-byte fork header against a file of `file_size` bytes.
[[nodiscard]] std::optional<ForkHeader>
ReadForkHeader(std::span<const std::byte> bytes, std::size_t file_size) {
  if (bytes.size() < kForkHeaderSize) {
    return std::nullopt;
  }
  const ForkHeader header{
      .data_offset = static_cast<std::size_t>(ReadBe32(bytes, 0)),
      .map_offset = static_cast<std::size_t>(ReadBe32(bytes, 4)),
      .data_length = static_cast<std::size_t>(ReadBe32(bytes, 8)),
      .map_length = static_cast<std::size_t>(ReadBe32(bytes, 12)),
  };
  if (header.data_offset == 0 || header.map_offset == 0 ||
      header.map_length < kMapHeaderSize ||
      !InBounds(header.data_offset, header.data_length, file_size) ||
      !InBounds(header.map_offset, header.map_length, file_size)) {
    return std::nullopt;
  }
  return header;
}

} // namespace

std::optional<std::vector<std::byte>>
ReadFileBytes(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input) {
    return std::nullopt;
  }
  const auto end = input.tellg();
  if (end <= 0) {
    return std::nullopt;
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input) {
    return std::nullopt;
  }
  return bytes;
}

std::optional<std::vector<std::byte>>
ReadFilePrefix(const std::filesystem::path &path, std::size_t max_bytes) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::nullopt;
  }
  std::vector<std::byte> bytes(max_bytes);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(input.gcount()));
  if (bytes.empty()) {
    return std::nullopt;
  }
  return bytes;
}

bool IsResourceForkHeader(std::span<const std::byte> header,
                          std::size_t file_size) {
  return ReadForkHeader(header, file_size).has_value();
}

bool IsAppleSingleOrDoubleHeader(std::span<const std::byte> header) {
  if (header.size() < 8) {
    return false;
  }
  const auto magic = ReadBe32(header, 0);
  return (magic == kAppleSingleMagic || magic == kAppleDoubleMagic) &&
         ReadBe32(header, 4) == kAppleSingleVersion;
}

std::optional<ResourceFork>
ParseResourceFork(std::span<const std::byte> fork_bytes) {
  const auto size = fork_bytes.size();
  if (size < kForkHeaderSize + kMapHeaderSize) {
    return std::nullopt;
  }
  const auto header = ReadForkHeader(fork_bytes, size);
  if (!header) {
    return std::nullopt;
  }
  const auto data_offset = header->data_offset;
  const auto map_offset = header->map_offset;
  const auto map_length = header->map_length;

  const auto type_list_offset =
      static_cast<std::size_t>(ReadBe16(fork_bytes, map_offset + 24));
  const auto name_list_offset =
      static_cast<std::size_t>(ReadBe16(fork_bytes, map_offset + 26));
  if (!InBounds(map_offset + type_list_offset, 2, size)) {
    return std::nullopt;
  }
  const auto type_list = map_offset + type_list_offset;
  const auto type_count =
      static_cast<std::size_t>(ReadBe16(fork_bytes, type_list)) + 1;
  if (!InBounds(type_list + 2, type_count * kTypeEntrySize, size)) {
    return std::nullopt;
  }
  const auto name_list = map_offset + name_list_offset;
  const auto map_end = map_offset + map_length;

  ResourceFork fork;
  fork.bytes.assign(fork_bytes.begin(), fork_bytes.end());
  for (std::size_t type = 0; type < type_count; ++type) {
    const auto type_entry = type_list + 2 + type * kTypeEntrySize;
    const auto type_code = ReadBe32(fork.bytes, type_entry);
    const auto resource_count =
        static_cast<std::size_t>(ReadBe16(fork.bytes, type_entry + 4)) + 1;
    const auto ref_list = type_list + static_cast<std::size_t>(
                                          ReadBe16(fork.bytes, type_entry + 6));
    if (!InBounds(ref_list, resource_count * kRefEntrySize, map_end)) {
      return std::nullopt;
    }
    for (std::size_t resource = 0; resource < resource_count; ++resource) {
      const auto entry = ref_list + resource * kRefEntrySize;
      const auto resource_id = ReadBe16(fork.bytes, entry);
      const auto name_offset = ReadBe16(fork.bytes, entry + 2);
      const auto attributes =
          std::to_integer<std::uint8_t>(fork.bytes[entry + 4]);
      const auto data_offset_field =
          static_cast<std::size_t>(
              std::to_integer<std::uint8_t>(fork.bytes[entry + 5]))
              << 16U |
          static_cast<std::size_t>(
              std::to_integer<std::uint8_t>(fork.bytes[entry + 6]))
              << 8U |
          static_cast<std::size_t>(
              std::to_integer<std::uint8_t>(fork.bytes[entry + 7]));

      const auto data_start = data_offset + data_offset_field;
      if (!InBounds(data_start, 4, size)) {
        continue;
      }
      const auto payload_length =
          static_cast<std::size_t>(ReadBe32(fork.bytes, data_start));
      const auto payload_offset = data_start + 4;
      if (!InBounds(payload_offset, payload_length, size)) {
        continue;
      }

      std::string name;
      if (name_offset != kNoName) {
        const auto name_start = name_list + name_offset;
        if (name_start < size) {
          const auto length =
              std::to_integer<std::uint8_t>(fork.bytes[name_start]);
          if (InBounds(name_start + 1, length, size)) {
            const auto *data = reinterpret_cast<const char *>(
                fork.bytes.data() + name_start + 1);
            name.assign(data, length);
          }
        }
      }

      fork.resources.push_back({
          .type_code = type_code,
          .resource_id = resource_id,
          .attributes = attributes,
          .name = std::move(name),
          .offset = payload_offset,
          .size = payload_length,
      });
    }
  }
  return fork;
}

std::optional<ResourceFork>
ParseResourceForkOrAppleDouble(std::span<const std::byte> data) {
  if (data.size() >= 26 && IsAppleSingleOrDoubleHeader(data)) {
    const auto entry_count = static_cast<std::size_t>(ReadBe16(data, 24));
    if (!InBounds(26, entry_count * 12, data.size())) {
      return std::nullopt;
    }
    for (std::size_t entry = 0; entry < entry_count; ++entry) {
      const auto base = 26 + entry * 12;
      if (ReadBe32(data, base) != kAppleResourceForkEntry) {
        continue;
      }
      const auto offset = static_cast<std::size_t>(ReadBe32(data, base + 4));
      const auto length = static_cast<std::size_t>(ReadBe32(data, base + 8));
      if (!InBounds(offset, length, data.size())) {
        return std::nullopt;
      }
      return ParseResourceFork(data.subspan(offset, length));
    }
    return std::nullopt;
  }
  return ParseResourceFork(data);
}

std::optional<ResourceFork>
LoadResourceFork(const std::filesystem::path &path) {
#if defined(__APPLE__)
  // macOS exposes the HFS resource fork as the "..namedfork/rsrc" pseudo-file.
  if (auto bytes = ReadFileBytes(path / "..namedfork" / "rsrc")) {
    if (auto fork = ParseResourceForkOrAppleDouble(*bytes)) {
      return fork;
    }
  }
#endif
  // AppleDouble keeps the fork in a "._name" sidecar beside the data file, how
  // an unpacked Mac archive looks on a non-HFS filesystem.
  const auto sidecar = path.parent_path() / ("._" + path.filename().string());
  if (auto bytes = ReadFileBytes(sidecar)) {
    if (auto fork = ParseResourceForkOrAppleDouble(*bytes)) {
      return fork;
    }
  }
  return std::nullopt;
}

} // namespace evnova::rez
