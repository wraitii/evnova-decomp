#include "brgr_archive.hpp"
#include "log.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {

[[nodiscard]] std::uint32_t ReadLe32(const std::vector<std::byte> &bytes,
                                     std::size_t offset) {
  if (offset + 4 > bytes.size()) {
    return 0;
  }
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U |
         std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U |
         std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U;
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

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) << 8U |
      std::to_integer<std::uint8_t>(bytes[offset + 1]));
}

[[nodiscard]] std::int16_t ReadBeI16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::int16_t>(ReadBe16(bytes, offset));
}

[[nodiscard]] NovaRgbColor ReadRgb(std::span<const std::byte> bytes,
                                   std::size_t offset) {
  return NovaRgbColor{
      .red = std::to_integer<std::uint8_t>(bytes[offset]),
      .green = std::to_integer<std::uint8_t>(bytes[offset + 1]),
      .blue = std::to_integer<std::uint8_t>(bytes[offset + 2]),
  };
}

// ---------------------------------------------------------------------------
// BRGR container + resource.map parsing (clean-room reconstruction of
// ResourceArchive_OpenRez / BrgrResource_CloneAndRelocateTable).
//
// Container layout (little-endian):
//   [0x00] 'BRGR'
//   [0x08] descriptor-blob size
//   [0x0c] blob: [u32 1][u32 1][u32 entry_count]
//          then entry_count * 12-byte entries: [u32 offset][u32 size][u32 name]
//          then the names area ("resource.map" lives here for every archive).
// Each entry is a data region inside the archive file.
//
// resource.map (big-endian, one of the container regions):
//   [u32 type_dir_offset][u32 type_count]
//   type entries (12 bytes): [u32 type_code][u32 records_offset][u32 count]
//   records (0x10a bytes): [u32 index][u32 type_code][u16 res_id][name...]
// A record's index is 1-based into the container entry table, so its payload
// is entries[index - 1].
// ---------------------------------------------------------------------------

constexpr std::size_t kBrgrBlobOffset = 0x0c;
constexpr std::size_t kBrgrEntrySize = 12;
constexpr std::size_t kMapTypeEntrySize = 12;
constexpr std::size_t kMapRecordSize = 0x10a;

struct ArchiveEntry {
  std::size_t offset = 0;
  std::size_t size = 0;
};

struct ResourceRecord {
  std::uint32_t type_code = 0;
  std::uint16_t resource_id = 0;
  std::size_t entry_index = 0;
};

struct LoadedArchive {
  std::vector<std::byte> bytes;
  std::vector<ArchiveEntry> entries;
  std::vector<ResourceRecord> records;
};

[[nodiscard]] std::optional<LoadedArchive>
ParseArchive(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  const auto archive_size = input ? static_cast<std::size_t>(input.tellg()) : 0;
  std::vector<std::byte> bytes(archive_size);
  input.seekg(0);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (bytes.size() < 0x1c ||
      std::array{bytes[0], bytes[1], bytes[2], bytes[3]} !=
          std::array{std::byte{0x42}, std::byte{0x52}, std::byte{0x47},
                     std::byte{0x52}}) {
    NovaLog::Warn("BRGR archive unavailable or malformed: {}", path.string());
    return std::nullopt;
  }

  LoadedArchive archive;
  archive.bytes = std::move(bytes);

  const auto blob_size = static_cast<std::size_t>(ReadLe32(archive.bytes, 8));
  if (blob_size < 12 || kBrgrBlobOffset + blob_size > archive.bytes.size()) {
    NovaLog::Warn("BRGR archive has an invalid descriptor blob: {}",
                  path.string());
    return std::nullopt;
  }
  const auto entry_count =
      static_cast<std::size_t>(ReadLe32(archive.bytes, kBrgrBlobOffset + 8));
  const auto entries_begin = kBrgrBlobOffset + 12;
  if (entry_count == 0 ||
      entry_count > (blob_size - 12) / kBrgrEntrySize) {
    NovaLog::Warn("BRGR archive has an invalid entry count: {}", path.string());
    return std::nullopt;
  }
  for (std::size_t index = 0; index < entry_count; ++index) {
    const auto entry_offset = entries_begin + index * kBrgrEntrySize;
    const auto offset =
        static_cast<std::size_t>(ReadLe32(archive.bytes, entry_offset));
    const auto size =
        static_cast<std::size_t>(ReadLe32(archive.bytes, entry_offset + 4));
    if (offset > archive.bytes.size() ||
        size > archive.bytes.size() - offset) {
      NovaLog::Warn("BRGR archive has an out-of-bounds region: {}",
                    path.string());
      return std::nullopt;
    }
    archive.entries.push_back({offset, size});
  }

  // Locate resource.map: the container region whose payload parses as a map
  // header ([type_dir_offset][type_count]). The game finds it by name through
  // the entry table; scanning is deterministic and avoids the name relocation.
  for (std::size_t map_index = 0; map_index < archive.entries.size();
       ++map_index) {
    const auto &entry = archive.entries[map_index];
    if (entry.size < 8) {
      continue;
    }
    const auto type_dir_offset =
        static_cast<std::size_t>(ReadBe32(archive.bytes, entry.offset));
    const auto type_count =
        static_cast<std::size_t>(ReadBe32(archive.bytes, entry.offset + 4));
    if (type_count == 0 || type_count > 500 ||
        type_dir_offset > entry.size ||
        type_dir_offset + type_count * kMapTypeEntrySize > entry.size) {
      continue;
    }
    for (std::size_t type = 0; type < type_count; ++type) {
      const auto type_entry_offset =
          entry.offset + type_dir_offset + type * kMapTypeEntrySize;
      const auto type_code = ReadBe32(archive.bytes, type_entry_offset);
      const auto records_offset =
          static_cast<std::size_t>(
              ReadBe32(archive.bytes, type_entry_offset + 4));
      const auto record_count =
          static_cast<std::size_t>(
              ReadBe32(archive.bytes, type_entry_offset + 8));
      for (std::size_t record = 0; record < record_count; ++record) {
        const auto record_offset = entry.offset + records_offset +
                                   record * kMapRecordSize;
        if (record_offset + 0x0e > entry.offset + entry.size) {
          break;
        }
        const auto index =
            static_cast<std::size_t>(ReadBe32(archive.bytes, record_offset));
        const auto resource_id = ReadBe16(archive.bytes, record_offset + 8);
        if (index == 0 || index - 1 >= archive.entries.size()) {
          continue;
        }
        archive.records.push_back(
            {type_code, resource_id, index - 1});
      }
    }
    return archive;
  }

  NovaLog::Warn("BRGR archive has no parseable resource.map: {}",
                path.string());
  return std::nullopt;
}

// Archives needed by the menu/splash path. Graphics 3 holds the sp\x95n,
// c\x9alr and rl\x91D menu assets; Nova Titles 1 holds the splash PICTs
// (0x1fa4 loading, 0x83 startup). Extend as more subsystems are reconstructed.
constexpr std::array kArchiveFileNames{
    "Nova Graphics 3.rez",
    "Nova Titles 1.rez",
};
constexpr std::array kNovaFilesRoots{
    "EV Nova/Nova Files/",
    "../../../EV Nova/Nova Files/",
};

class NovaResourceDb {
public:
  [[nodiscard]] static NovaResourceDb &Instance() {
    static NovaResourceDb db;
    return db;
  }

  [[nodiscard]] std::optional<std::vector<std::byte>>
  Load(std::uint32_t type_code, std::uint16_t resource_id) {
    EnsureLoaded();
    for (const auto &archive : archives_) {
      for (const auto &record : archive.records) {
        if (record.type_code != type_code ||
            record.resource_id != resource_id) {
          continue;
        }
        return Region(archive, record);
      }
    }
    return std::nullopt;
  }

  // n-th record of a type across all archives (1-based), mirroring the game's
  // FUN_004ce2a0/FUN_004ce030 walk.
  [[nodiscard]] std::optional<std::vector<std::byte>>
  LoadNthOfType(std::uint32_t type_code, std::size_t ordinal) {
    EnsureLoaded();
    if (ordinal == 0) {
      return std::nullopt;
    }
    std::size_t remaining = ordinal - 1;
    for (const auto &archive : archives_) {
      for (const auto &record : archive.records) {
        if (record.type_code != type_code) {
          continue;
        }
        if (remaining == 0) {
          return Region(archive, record);
        }
        --remaining;
      }
    }
    return std::nullopt;
  }

private:
  [[nodiscard]] std::vector<std::byte> Region(const LoadedArchive &archive,
                                              const ResourceRecord &record) const {
    const auto &entry = archive.entries[record.entry_index];
    return std::vector<std::byte>{
        archive.bytes.begin() + static_cast<std::ptrdiff_t>(entry.offset),
        archive.bytes.begin() +
            static_cast<std::ptrdiff_t>(entry.offset + entry.size)};
  }

  void EnsureLoaded() {
    if (loaded_) {
      return;
    }
    loaded_ = true;
    for (const auto root : kNovaFilesRoots) {
      for (const auto file_name : kArchiveFileNames) {
        const auto path = std::filesystem::path{root} / file_name;
        if (!std::filesystem::exists(path)) {
          continue;
        }
        if (auto archive = ParseArchive(path)) {
          archives_.push_back(std::move(*archive));
          NovaLog::Info("opened BRGR archive {}", path.string());
        }
      }
    }
    if (archives_.empty()) {
      NovaLog::Todo("no Nova .rez archives were found relative to the working "
                    "directory");
    }
  }

  bool loaded_ = false;
  std::vector<LoadedArchive> archives_;
};

} // namespace

std::optional<std::vector<std::byte>>
NovaResource_Load(std::uint32_t type_code, std::uint16_t resource_id) {
  return NovaResourceDb::Instance().Load(type_code, resource_id);
}

std::optional<std::vector<std::byte>>
NovaResource_LoadNthOfType(std::uint32_t type_code, std::size_t ordinal) {
  return NovaResourceDb::Instance().LoadNthOfType(type_code, ordinal);
}

std::optional<NovaSpriteDefinition>
NovaSpriteDefinition_Parse(std::span<const std::byte> resource_data) {
  if (resource_data.size() < 12) {
    return std::nullopt;
  }
  NovaSpriteDefinition definition{
      .sprites_resource_id = ReadBe16(resource_data, 0),
      .mask_resource_id = ReadBe16(resource_data, 2),
      .tile_width = ReadBe16(resource_data, 4),
      .tile_height = ReadBe16(resource_data, 6),
      .tiles_x = ReadBe16(resource_data, 8),
      .tiles_y = ReadBe16(resource_data, 10),
  };
  if (definition.tile_width == 0 || definition.tile_height == 0 ||
      definition.tiles_x == 0 || definition.tiles_y == 0 ||
      definition.tile_width > 1024 || definition.tile_height > 1024 ||
      definition.tiles_x > 64 || definition.tiles_y > 64) {
    return std::nullopt;
  }
  return definition;
}

std::optional<NovaSpriteDefinition>
NovaResource_LoadMainMenuSpriteDefinition(std::uint16_t sprite_id) {
  const auto resource_data = NovaResource_Load(kResourceTypeSprites, sprite_id);
  if (!resource_data) {
    NovaLog::Todo("sp\\x95n resource {} could not be located", sprite_id);
    return std::nullopt;
  }
  const auto definition = NovaSpriteDefinition_Parse(*resource_data);
  if (!definition) {
    NovaLog::Warn("sp\\x95n resource {} is malformed", sprite_id);
  }
  return definition;
}

std::optional<NovaMainMenuStyle>
NovaMainMenuStyle_Parse(std::span<const std::byte> resource_data) {
  constexpr std::size_t kMenuFontSizeOffset = 0x4c;
  constexpr std::size_t kMenuBrightOffset = 0x4e;
  constexpr std::size_t kMenuDimOffset = 0x52;
  constexpr std::size_t kButtonOriginsOffset = 0x72;
  constexpr std::size_t kButtonOriginSize = 4;
  constexpr std::size_t kRequiredSize =
      kButtonOriginsOffset + kButtonOriginSize * 6;
  if (resource_data.size() < kRequiredSize) {
    return std::nullopt;
  }

  NovaMainMenuStyle style{
      .menu_bright = ReadRgb(resource_data, kMenuBrightOffset),
      .menu_dim = ReadRgb(resource_data, kMenuDimOffset),
      .menu_font_size = ReadBe16(resource_data, kMenuFontSizeOffset),
  };
  for (std::size_t index = 0; index < style.button_origins.size(); ++index) {
    const auto offset = kButtonOriginsOffset + index * kButtonOriginSize;
    style.button_origins[index] = NovaMenuPoint{
        .x = ReadBeI16(resource_data, offset),
        .y = ReadBeI16(resource_data, offset + 2),
    };
  }
  return style;
}

std::optional<NovaMainMenuStyle> NovaResource_LoadMainMenuStyle() {
  // The game reads the first c\x9alr record (FUN_004ce2a0(0x639a6c72, 1)); in
  // Nova Graphics 3 that record's resource id is 0x80.
  const auto resource_data =
      NovaResource_LoadNthOfType(kResourceTypeColors, 1);
  if (!resource_data) {
    NovaLog::Todo("main-menu c\\x9alr resource could not be located");
    return std::nullopt;
  }
  const auto style = NovaMainMenuStyle_Parse(*resource_data);
  if (!style) {
    NovaLog::Warn("c\\x9alr resource is malformed");
  }
  return style;
}

std::optional<std::vector<std::byte>>
NovaResource_LoadPictData(std::uint16_t resource_id) {
  const auto resource_data = NovaResource_Load(kResourceTypePict, resource_id);
  if (!resource_data) {
    NovaLog::Todo("PICT resource 0x{:04x} could not be located", resource_id);
    return std::nullopt;
  }
  return resource_data;
}
