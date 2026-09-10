#include "brgr_archive.hpp"
#include "log.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
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

// c\x9alr (and the other color-table resources) store each color as a
// big-endian 32-bit 0x00RRGGBB long, so the leading byte at `offset` is the
// unused high byte and the channels are offset+1/+2/+3. Reading
// offset/offset+1/offset+2 (the raw little-endian byte window) byte-shifts
// every non-symmetric color; the store grid colors below make that visible
// (gray/red become olive/green).
[[nodiscard]] NovaRgbColor ReadRgb(std::span<const std::byte> bytes,
                                   std::size_t offset) {
  return NovaRgbColor{
      .red = std::to_integer<std::uint8_t>(bytes[offset + 1]),
      .green = std::to_integer<std::uint8_t>(bytes[offset + 2]),
      .blue = std::to_integer<std::uint8_t>(bytes[offset + 3]),
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
  // Record display name from resource.map (`name` field of the 0x10a-byte
  // record, written at [0x0a]). For resource families that key off the record
  // name rather than a numeric field (ships, outfits, stellars, systems) this
  // is what the loader surfaces as the class/outfit/object display name.
  std::string name;
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
          std::array{std::byte{0x42},
                     std::byte{0x52},
                     std::byte{0x47},
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
  if (entry_count == 0 || entry_count > (blob_size - 12) / kBrgrEntrySize) {
    NovaLog::Warn("BRGR archive has an invalid entry count: {}", path.string());
    return std::nullopt;
  }
  for (std::size_t index = 0; index < entry_count; ++index) {
    const auto entry_offset = entries_begin + index * kBrgrEntrySize;
    const auto offset =
        static_cast<std::size_t>(ReadLe32(archive.bytes, entry_offset));
    const auto size =
        static_cast<std::size_t>(ReadLe32(archive.bytes, entry_offset + 4));
    if (offset > archive.bytes.size() || size > archive.bytes.size() - offset) {
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
    if (type_count == 0 || type_count > 500 || type_dir_offset > entry.size ||
        type_dir_offset + type_count * kMapTypeEntrySize > entry.size) {
      continue;
    }
    // Some containers carry multiple regions whose first bytes happen to read
    // as a plausible map header (a small type_dir_offset and a modest type
    // count) but whose "records" are not a coherent record table. Only trust a
    // region when every type entry's record table lies entirely inside it; a
    // genuine resource.map is one big packed block of fixed-size records, so an
    // overflowing entry means this region is not the map. This is what earlier
    // caused Nova Data 4's o\x9ftf / w\x91ap records to be missed: a bogus
    // region was accepted before the real one.
    bool table_in_bounds = true;
    for (std::size_t type = 0; type < type_count; ++type) {
      const auto type_entry_offset =
          entry.offset + type_dir_offset + type * kMapTypeEntrySize;
      const auto records_offset = static_cast<std::size_t>(
          ReadBe32(archive.bytes, type_entry_offset + 4));
      const auto record_count = static_cast<std::size_t>(
          ReadBe32(archive.bytes, type_entry_offset + 8));
      const auto table_end =
          records_offset +
          record_count * static_cast<std::size_t>(kMapRecordSize);
      if (records_offset > entry.size || table_end > entry.size) {
        table_in_bounds = false;
        break;
      }
    }
    if (!table_in_bounds) {
      continue;
    }
    for (std::size_t type = 0; type < type_count; ++type) {
      const auto type_entry_offset =
          entry.offset + type_dir_offset + type * kMapTypeEntrySize;
      const auto type_code = ReadBe32(archive.bytes, type_entry_offset);
      const auto records_offset = static_cast<std::size_t>(
          ReadBe32(archive.bytes, type_entry_offset + 4));
      const auto record_count = static_cast<std::size_t>(
          ReadBe32(archive.bytes, type_entry_offset + 8));
      for (std::size_t record = 0; record < record_count; ++record) {
        const auto record_offset =
            entry.offset + records_offset + record * kMapRecordSize;
        const auto index =
            static_cast<std::size_t>(ReadBe32(archive.bytes, record_offset));
        const auto resource_id = ReadBe16(archive.bytes, record_offset + 8);
        if (index == 0 || index - 1 >= archive.entries.size()) {
          continue;
        }
        // Record name is a NUL-terminated C string at [0x0a] of the 0x10a-byte
        // record, e.g. "Marauder" or "Light Blaster". Bounded by the record's
        // fixed width (and by the end of file if truncated).
        std::string record_name;
        const std::size_t name_capacity =
            (record_offset + 10 >= archive.bytes.size())
                ? 0
                : std::min(kMapRecordSize - 10,
                           archive.bytes.size() - (record_offset + 10));
        if (name_capacity > 0) {
          record_name.append(reinterpret_cast<const char *>(
                                 archive.bytes.data() + record_offset + 10),
                             name_capacity);
          if (const auto nul = record_name.find('\0');
              nul != std::string::npos) {
            record_name.resize(nul);
          }
        }
        archive.records.push_back(
            {type_code, resource_id, index - 1, std::move(record_name)});
      }
    }
    return archive;
  }

  NovaLog::Warn("BRGR archive has no parseable resource.map: {}",
                path.string());
  return std::nullopt;
}

// Archives needed by the recompiled path. Graphics 3 holds the sp\x95n,
// c\x9alr and rl\x91D menu assets; Nova Titles 1 holds the splash PICTs
// (0x1fa4 loading, 0x83 startup); Nova Sounds holds the "snd " audio family
// used for menu feedback and the intro/travel/loading sounds (ids 600..603).
//
// The scenario data archives hold the runtime resource tables that
// NovaData_LoadScenarioResourceTables (0x004bd3c0) rebuilds: ships (sh\x95p)
// and the default character (ch\x9ar) live in Nova Data 1; stellars (sp\x9ab)
// and systems (s\xd8st) in Nova Data 2; descriptions (d\x91sc) span several;
// outfits (o\x9ftf) and weapons (w\x91ap) are in Nova Data 4. The remaining
// Data archives are loaded so plugins/overrides resolve the same way the
// original scans them; more are added as graphics/ships subsystems as they are
// reconstructed.
//
// The ship ship-animation descriptor (sh\x8an) and its 16-bit sprite sheets
// (rl\x91D) live in the Nova Ships archives; they are needed so the flight
// rendering path can draw a ship by heading (ShipClass_LoadShipClassVisual-
// AndLaunchData reads sh\x8an BaseImageID -> rl\x91D).
//  0x1F8 space color). The ship ship-animation descriptor (sh\x8an) and its
//  16-bit sprite sheets (rl\x91D) live in the Nova Ships archives; the stellar
//  (planet) spin sprites (sp\x9an ids 1000-1255 + their rl\x91D sheets) are in
//  Nova Graphics 2; both are needed for the flight rendering path.
constexpr std::array kArchiveFileNames{
    // The core UI archive lives directly in EV Nova/ (not under Nova Files/):
    // it carries the DLOG/DITL/MENU/ALRT/CNTL dialog-window resources that the
    // engine's dialog system (UiWindow_CreateFromDialogResource -> DLOG
    // 0x444c4f47
    // / DITL 0x4449544c) builds every in-game window from, plus a handful of
    // PICT/STR#. Without it the .rez set is incomplete and dialog data is
    // missing (this is what the landed/services window rects come from).
    "Nova.rez",
    "Nova Graphics 3.rez",
    "Nova Titles 1.rez",
    "Nova Titles 2.rez",
    "Nova Titles 3.rez",
    "Nova Titles 4.rez",
    "Nova Sounds.rez",
    "Nova Ships 1.rez",
    "Nova Ships 2.rez",
    "Nova Ships 3.rez",
    "Nova Ships 4.rez",
    "Nova Ships 5.rez",
    "Nova Ships 6.rez",
    "Nova Ships 7.rez",
    "Nova Graphics 1.rez",
    "Nova Graphics 2.rez",
    "Nova Data 1.rez",
    "Nova Data 2.rez",
    "Nova Data 3.rez",
    "Nova Data 4.rez",
    "Nova Data 5.rez",
    "Nova Data 6.rez",
};
constexpr std::array kNovaFilesRoots{
    // Nova.rez sits one level above the Nova Files/ subfolder, so search both.
    "EV Nova/",
    "EV Nova/Nova Files/",
    "../../../EV Nova/",
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

  // Like Load plus the record's resource.map display name.
  [[nodiscard]] std::optional<NovaResource>
  LoadNamed(std::uint32_t type_code, std::uint16_t resource_id) {
    EnsureLoaded();
    for (const auto &archive : archives_) {
      for (const auto &record : archive.records) {
        if (record.type_code != type_code ||
            record.resource_id != resource_id) {
          continue;
        }
        return NovaResource{.bytes = Region(archive, record),
                            .name = record.name};
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

  // Diagnostics: every distinct (type_code, resource_id) pair across all
  // archives, used to confirm which resource families the BRGR maps carry.
  [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint16_t>> AllKeys() {
    EnsureLoaded();
    std::vector<std::pair<std::uint32_t, std::uint16_t>> keys;
    for (const auto &archive : archives_) {
      for (const auto &record : archive.records) {
        keys.emplace_back(record.type_code, record.resource_id);
      }
    }
    return keys;
  }

private:
  [[nodiscard]] std::vector<std::byte>
  Region(const LoadedArchive &archive, const ResourceRecord &record) const {
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

std::optional<NovaResource> NovaResource_LoadNamed(std::uint32_t type_code,
                                                   std::uint16_t resource_id) {
  return NovaResourceDb::Instance().LoadNamed(type_code, resource_id);
}

std::optional<std::vector<std::byte>>
NovaResource_LoadNthOfType(std::uint32_t type_code, std::size_t ordinal) {
  return NovaResourceDb::Instance().LoadNthOfType(type_code, ordinal);
}

std::vector<std::pair<std::uint32_t, std::uint16_t>> NovaResource_AllKeys() {
  return NovaResourceDb::Instance().AllKeys();
}

std::optional<std::filesystem::path>
NovaResource_LocateFile(const std::string &file_name) {
  for (const auto root : kNovaFilesRoots) {
    const auto path = std::filesystem::path{root} / file_name;
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  NovaLog::Todo("Nova Files asset '{}' was not found", file_name);
  return std::nullopt;
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
  constexpr std::size_t kGridBrightOffset = 0x56; // selection square
  constexpr std::size_t kGridDimOffset = 0x5a;    // normal grid frame
  constexpr std::size_t kFloatingMapOffset = 0x8a;
  constexpr std::size_t kListTextOffset = 0x8e;
  constexpr std::size_t kListBackgroundOffset = 0x92;
  constexpr std::size_t kListHiliteOffset = 0x96;
  constexpr std::size_t kEscortHiliteOffset = 0x9a;
  constexpr std::size_t kPaletteEnd = kEscortHiliteOffset + 4;
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
      .grid_bright = ReadRgb(resource_data, kGridBrightOffset),
      .grid_dim = ReadRgb(resource_data, kGridDimOffset),
      .menu_font_size = ReadBe16(resource_data, kMenuFontSizeOffset),
  };
  for (std::size_t index = 0; index < style.button_origins.size(); ++index) {
    const auto offset = kButtonOriginsOffset + index * kButtonOriginSize;
    style.button_origins[index] = NovaMenuPoint{
        .x = ReadBeI16(resource_data, offset),
        .y = ReadBeI16(resource_data, offset + 2),
    };
  }
  // Ghidra: NovaData_LoadScenarioResourceTables reads c\x9alr +0xe0 into
  // DAT_007d2524/26, the main-screen logo anchor.
  if (resource_data.size() >= 0xe4) {
    style.logo_origin = NovaMenuPoint{
        .x = ReadBeI16(resource_data, 0xe0),
        .y = ReadBeI16(resource_data, 0xe2),
    };
  }
  if (resource_data.size() >= 0xe8) {
    style.center_preview_origin = NovaMenuPoint{
        .x = ReadBeI16(resource_data, 0xe4),
        .y = ReadBeI16(resource_data, 0xe6),
    };
  }
  if (resource_data.size() >= 0xf4) {
    for (std::size_t index = 0; index < style.row_reveal_origins.size();
         ++index) {
      const auto offset = 0xe8 + index * 4;
      style.row_reveal_origins[index] = NovaMenuPoint{
          .x = ReadBeI16(resource_data, offset),
          .y = ReadBeI16(resource_data, offset + 2),
      };
    }
  }
  // List/map palette (guarded: a truncated c\x9alr keeps the black defaults).
  if (resource_data.size() >= kPaletteEnd) {
    style.floating_map = ReadRgb(resource_data, kFloatingMapOffset);
    style.list_text = ReadRgb(resource_data, kListTextOffset);
    style.list_background = ReadRgb(resource_data, kListBackgroundOffset);
    style.list_hilite = ReadRgb(resource_data, kListHiliteOffset);
    style.escort_hilite = ReadRgb(resource_data, kEscortHiliteOffset);
  }
  return style;
}

std::optional<NovaMainMenuStyle> NovaResource_LoadMainMenuStyle() {
  // The game reads the first c\x9alr record (FUN_004ce2a0(0x639a6c72, 1)); in
  // Nova Graphics 3 that record's resource id is 0x80.
  const auto resource_data = NovaResource_LoadNthOfType(kResourceTypeColors, 1);
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
NovaResource_LoadSndData(std::uint16_t resource_id) {
  // Ghidra 0x004BC2A0 NovaSound_LoadDecodedById. Resource acquisition is here;
  // its decode tail runs in NovaSound_Decode below.
  // Silent on miss: the original probes whole contiguous ranges (the 200..455
  // gameplay table and 300..363 impact table in FUN_004b0740) and tolerates
  // the many ids that do not exist in Nova Sounds.rez, so a miss is normal.
  // Callers that care about a specific sound log their own diagnostic.
  return NovaResource_Load(kResourceTypeSnd, resource_id);
}

std::optional<NovaSoundData>
NovaSound_Decode(std::span<const std::byte> resource_data) {
  // FUN_004d6e60 accepts the format-1/2/3 'snd' payload layouts Ghidra's
  // FUN_004d6e60 dispatches on: format-2 'NONE' headers (menu focus ticks
  // 600/601), format-1 extended headers containing Apple IMA4 packets (the
  // row-reveal effects 602/603 and the IMA4 weapon sounds), and format-1
  // 'NONE' 8-bit mono payloads (the bulk of the weapon fire sounds, e.g.
  // Light Blaster). Weapon fire sounds live in snd ids 200..235 (see
  // NovaWeapon_FireSound slot -> resource mapping in weapon.cpp).
  const auto read_be16 = [resource_data](std::size_t offset) {
    if (offset + 2 > resource_data.size()) {
      return std::uint16_t{0};
    }
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint8_t>(resource_data[offset]) << 8U) |
        std::to_integer<std::uint8_t>(resource_data[offset + 1]));
  };
  const auto read_be32 = [resource_data](std::size_t offset) {
    if (offset + 4 > resource_data.size()) {
      return std::uint32_t{0};
    }
    return (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(resource_data[offset]))
            << 24U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(resource_data[offset + 1]))
            << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(resource_data[offset + 2]))
            << 8U) |
           static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(resource_data[offset + 3]));
  };
  if (resource_data.size() < 0x22) {
    return std::nullopt;
  }

  if (read_be16(0) == 1U) {
    // Format-1 'snd' payloads (the version word at +0 in {1,2,3} routes through
    // the same Ghidra command-list scan: FUN_004d6e60 -> the format handler)
    // place a single 0x8051 data command whose offset field is 0x14 for our
    // shipped set, so data_offset = 0x14. The byte at data_offset + 0x14 is
    // the sub-format discriminator Ghidra switches on (FUN_004d6e60's cVar1):
    //   0x00 -> 'NONE' 8-bit mono uncompressed (weapon fire/beam sounds, e.g.
    //            Light Blaster 208 @ 11127 Hz). Sample data at +0x16.
    //   0xfe -> extended SoundHeader carrying Apple IMA4 packets (menu row
    //            reveals 602/603 and the IMA4 weapon sounds). Sample data
    //            begins 0x40 bytes later; Ghidra leaves the codec conversion
    //            to the platform audio backend, and SDL consumes PCM, so the
    //            34-byte IMA4 packets are decoded here.
    constexpr std::size_t kHeaderOffset = 0x14;
    const auto discriminator =
        std::to_integer<std::uint8_t>(resource_data[kHeaderOffset + 0x14]);

    if (discriminator == 0x00) {
      // 'NONE' 8-bit mono, the same decode as the format-2 path but with the
      // fixed 0x14 data offset. SoundHeader at +0x14 carries the 16.16
      // fixed-point sample rate; each byte is biased 8-bit signed-mapped to
      // 16-bit exactly as FUN_004d6900 does ((v + 0x80) | (v + 0x80) << 8).
      constexpr std::size_t kNonesOffset = kHeaderOffset + 0x16;
      if (resource_data.size() <= kNonesOffset) {
        NovaLog::Todo("format-1 'NONE' snd resource has no sample data");
        return std::nullopt;
      }
      NovaSoundData sound{};
      sound.channel_count = 1;
      const auto fixed_sample_rate = read_be32(kHeaderOffset + 8);
      sound.sample_rate =
          static_cast<int>((fixed_sample_rate + 0x8000U) >> 16U);
      if (sound.sample_rate <= 0) {
        return std::nullopt;
      }
      sound.samples.reserve(resource_data.size() - kNonesOffset);
      for (std::size_t index = kNonesOffset; index < resource_data.size();
           ++index) {
        const auto biased = std::to_integer<int>(resource_data[index]) + 0x80;
        const auto value = static_cast<std::uint16_t>(biased | (biased << 8));
        sound.samples.push_back(static_cast<std::int16_t>(value));
      }
      NovaLog::Debug("decoded format-1 'NONE' 8-bit snd \x20with {} samples "
                     "at {} Hz",
                     sound.samples.size(),
                     sound.sample_rate);
      return sound;
    }

    constexpr std::size_t kSamplesOffset = kHeaderOffset + 0x40;
    constexpr std::size_t kPacketBytes = 34;
    constexpr std::size_t kSamplesPerPacket = 64;
    constexpr std::uint32_t kIma4 = 0x696d6134;
    if (discriminator != 0xfeU || resource_data.size() < kSamplesOffset ||
        read_be32(kHeaderOffset + 4) != 1U ||
        read_be32(kHeaderOffset + 0x28) != kIma4 ||
        read_be16(kHeaderOffset + 0x3e) != 16U ||
        (resource_data.size() - kSamplesOffset) % kPacketBytes != 0U) {
      NovaLog::Todo("unsupported extended snd resource layout");
      return std::nullopt;
    }

    constexpr std::array<int, 89> kStepTable{
        7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
        19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
        50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
        130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
        337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
        876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
        2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
        5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    constexpr std::array<int, 16> kIndexAdjust{
        -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

    NovaSoundData sound{};
    sound.channel_count = 1;
    sound.sample_rate = static_cast<int>(read_be32(kHeaderOffset + 8) >> 16U);
    if (sound.sample_rate <= 0) {
      return std::nullopt;
    }
    const auto packet_count =
        (resource_data.size() - kSamplesOffset) / kPacketBytes;
    sound.samples.reserve(packet_count * kSamplesPerPacket);
    int predictor = 0;
    for (std::size_t packet = 0; packet < packet_count; ++packet) {
      const auto packet_offset = kSamplesOffset + packet * kPacketBytes;
      const auto preamble = read_be16(packet_offset);
      // QuickTime replaces the predictor's high nine bits from each packet
      // preamble while preserving the prior packet's low seven bits. Those
      // seven preamble bits separately select the new step-table index.
      predictor = static_cast<std::int16_t>((preamble & 0xff80U) |
                                            (predictor & 0x007f));
      int step_index = std::min<int>(preamble & 0x7fU, 88);
      for (std::size_t byte_index = 2; byte_index < kPacketBytes;
           ++byte_index) {
        const auto packed = std::to_integer<std::uint8_t>(
            resource_data[packet_offset + byte_index]);
        for (const int shift : {0, 4}) {
          const int nibble = (packed >> shift) & 0x0f;
          const int step = kStepTable[static_cast<std::size_t>(step_index)];
          int difference = step >> 3;
          if ((nibble & 1) != 0) {
            difference += step >> 2;
          }
          if ((nibble & 2) != 0) {
            difference += step >> 1;
          }
          if ((nibble & 4) != 0) {
            difference += step;
          }
          predictor += (nibble & 8) != 0 ? -difference : difference;
          predictor = std::clamp(predictor, -32768, 32767);
          step_index = std::clamp(
              step_index + kIndexAdjust[static_cast<std::size_t>(nibble)],
              0,
              88);
          sound.samples.push_back(static_cast<std::int16_t>(predictor));
        }
      }
    }
    NovaLog::Debug("decoded Apple IMA4 snd with {} samples at {} Hz",
                   sound.samples.size(),
                   sound.sample_rate);
    return sound;
  }

  if (read_be16(0) != 2U) {
    NovaLog::Todo("unsupported snd resource container version");
    return std::nullopt;
  }

  // FUN_004d6d30 format-2 table scan -> sub-structure offset.
  const auto entry_count = read_be16(4);
  std::size_t data_offset = 0;
  for (std::size_t entry = 0; entry < static_cast<std::size_t>(entry_count);
       ++entry) {
    const auto table_pos = 6 + entry * 8;
    if (table_pos + 8 > resource_data.size()) {
      break;
    }
    const auto marker = read_be16(table_pos);
    if (marker == 0x8051 || marker == 0x8050) {
      data_offset = read_be32(table_pos + 4);
      break;
    }
  }

  // cVar1 == 0 selects the 'NONE' 8-bit mono path (FUN_004d6e60).
  if (data_offset + 0x14 >= resource_data.size() ||
      std::to_integer<std::uint8_t>(resource_data[data_offset + 0x14]) !=
          0x00) {
    NovaLog::Todo(
        "snd \x20sub-formats other than 'NONE' 8-bit are not decoded yet");
    return std::nullopt;
  }

  const std::size_t data_start = data_offset + 0x16;
  NovaSoundData sound{};
  sound.channel_count = 1;
  // Standard SoundHeader.sampleRate at +8 is an unsigned 16.16 fixed-point
  // value. The shipped focus ticks use the classic Macintosh rate
  // 0x2b7745d1 (about 11127.27 Hz); forcing them to the 44.1 kHz output rate
  // makes them several times too short and pitches them up.
  const auto fixed_sample_rate = read_be32(data_offset + 8);
  sound.sample_rate = static_cast<int>((fixed_sample_rate + 0x8000U) >> 16U);
  if (sound.sample_rate <= 0) {
    return std::nullopt;
  }
  sound.samples.reserve(resource_data.size() - data_start);
  // Ghidra FUN_004d6900: *out = (v + 0x80) | (v + 0x80) << 8, truncated to
  // 16 bits. Reconstruct the same bit pattern exactly (full-width biased
  // value before the 16-bit store).
  for (std::size_t index = data_start; index < resource_data.size(); ++index) {
    const auto biased = std::to_integer<int>(resource_data[index]) + 0x80;
    const auto value = static_cast<std::uint16_t>(biased | (biased << 8));
    sound.samples.push_back(static_cast<std::int16_t>(value));
  }
  NovaLog::Debug("decoded 'NONE' 8-bit snd \x20with {} samples from offset {}",
                 sound.samples.size(),
                 data_start);
  return sound;
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

std::optional<std::vector<std::byte>> NovaResource_LoadMainMenuBackdropData() {
  return NovaResource_LoadPictData(0x1f40);
}

std::optional<std::vector<std::byte>> NovaResource_LoadMainMenuLogoData() {
  return NovaResource_LoadPictData(0x1f4a);
}

namespace {

// Big-endian 16-bit read helper for DITL field parsing.
[[nodiscard]] std::uint16_t DialogReadBe16(std::span<const std::byte> data,
                                           std::size_t offset) {
  return std::to_integer<std::uint16_t>(data[offset]) << 8U |
         std::to_integer<std::uint16_t>(data[offset + 1]);
}

} // namespace

// Ghidra 0x004cef50 Dialog_ParseItemList.
std::optional<std::vector<NovaDialogItem>>
NovaResource_LoadDialogItems(std::uint16_t dialog_item_list_id) {
  // The DITL payload is the raw resource (FUN_004cef50 receives the same via
  // *handle). Its first big-endian short is the item count; the parser then
  // walks count+1 entries (the trailing entry is a zero-value terminator).
  const auto data =
      NovaResource_Load(kResourceTypeDialogItemList, dialog_item_list_id);
  if (!data) {
    NovaLog::Todo("DITL 0x{:04x} could not be located", dialog_item_list_id);
    return std::nullopt;
  }
  const std::size_t size = data->size();
  if (size < 2) {
    return std::nullopt;
  }
  const std::size_t count = DialogReadBe16(*data, 0);

  std::vector<NovaDialogItem> items;
  // First item follows the 2-byte count field.
  std::size_t pos = 2;
  for (std::size_t entry = 0; entry <= count; ++entry) {
    // Faithful bounds check: the item header (rect + type) is 14 bytes
    // (FUN_004cef50 reads through +0xd).
    if (pos + 14 > size) {
      NovaLog::Todo("DITL 0x{:04x}: item {} at {} out of bounds ({})",
                    dialog_item_list_id,
                    entry,
                    pos,
                    size);
      return std::nullopt;
    }
    NovaDialogItem item;
    item.index = entry;
    // Rect is two BE shorts ordered (top, left, bottom, right)
    // at item+4..+11; the type byte (bit 7 = enabled) at item+12.
    item.top = static_cast<std::int16_t>(DialogReadBe16(*data, pos + 4));
    item.left = static_cast<std::int16_t>(DialogReadBe16(*data, pos + 6));
    item.bottom = static_cast<std::int16_t>(DialogReadBe16(*data, pos + 8));
    item.right = static_cast<std::int16_t>(DialogReadBe16(*data, pos + 10));
    const auto type_byte = std::to_integer<std::uint8_t>((*data)[pos + 12]);
    item.enabled = (type_byte & 0x80U) != 0;
    item.type = type_byte & 0x7fU;

    // Advance past the item's variable tail, mirroring FUN_004cef50 (this
    // also fills item.title for text-like types, so the item is pushed after
    // the tail is parsed):
    //  - text-like types (4,5,6,8,0x10) carry a pascal string starting at
    //    +13; skip its length byte + characters.
    //  - icon/pict/control types (7,0x20,0x40) carry an extra refcon short.
    //  - everything else (buttons/plain) skips a 14-byte fixed record.
    std::size_t next;
    switch (item.type) {
    case 4:
    case 5:
    case 6:
    case 8:
    case 0x10: {
      const std::size_t title_len = static_cast<std::size_t>(
          std::to_integer<std::uint8_t>((*data)[pos + 13]));
      // Capture the item's caption so callers can draw label text without a
      // separate string lookup (preferences checkboxes read their title).
      if (pos + 13 + 1 + title_len <= size) {
        const auto first = data->begin() + pos + 14;
        item.title.assign(std::string_view(
            reinterpret_cast<const char *>(std::to_address(first)), title_len));
      }
      next = pos + 13 + title_len + 1;
      break;
    }
    case 7:
    case 0x20:
    case 0x40:
      next = pos + 16; // +8 ushorts
      // The control tail is [subtype byte][BE u16 refcon]: type-7 popups load
      // the MENU named by the refcon, 0x40 image items blit the PICT (verified
      // on DITL 0xc1d: item 2 -> PICT 129 (Strict Play note art), item 10 ->
      // MENU 0x1f4 "Gender", item 12 -> MENU 0x1f5 "Character", item 13 ->
      // PICT 130 (pilot icon)).
      if (pos + 16 <= size) {
        item.popup_subtype = std::to_integer<std::uint8_t>((*data)[pos + 13]);
        item.refcon = static_cast<std::uint16_t>(
            (std::to_integer<unsigned>((*data)[pos + 14]) << 8U) |
            std::to_integer<unsigned>((*data)[pos + 15]));
      }
      break;
    default:
      next = pos + 14; // +7 ushorts
      break;
    }
    items.push_back(item);
    // Re-align to an even offset before the next item.
    pos = next & ~std::size_t{1};
    if ((next & std::size_t{1}) != 0) {
      pos = next + 1;
    }
  }
  return items;
}

std::optional<NovaDialogDefinition>
NovaResource_LoadDialogDefinition(std::uint16_t dialog_id) {
  const auto data = NovaResource_Load(kResourceTypeDialog, dialog_id);
  if (!data) {
    NovaLog::Todo("DLOG 0x{:04x} could not be located", dialog_id);
    return std::nullopt;
  }
  // The DLOG is at least 20 bytes: bounds (off 0..7), procID/flags
  // (off 8..17) and the linked DITL id (off 18..19).
  if (data->size() < 20) {
    return std::nullopt;
  }
  NovaDialogDefinition def;
  def.top = static_cast<std::int16_t>(DialogReadBe16(*data, 0));
  def.left = static_cast<std::int16_t>(DialogReadBe16(*data, 2));
  def.bottom = static_cast<std::int16_t>(DialogReadBe16(*data, 4));
  def.right = static_cast<std::int16_t>(DialogReadBe16(*data, 6));
  def.dialog_item_list_id = DialogReadBe16(*data, 18);
  return def;
}

std::optional<NovaResource>
NovaResource_AccessCharacterBlockByKey(std::string_view key) {
  // Ghidra 0x004ce300 ResourceData_AccessByKey -> ResourceData_FindByKey:
  // linear scan of the family's registered entries, matching the metadata
  // name (block+0xe). The archive world enumerates the ch\x9ar resources
  // loaded from the .rez files; the in-memory registry slice is not
  // reconstructed (see header TODO).
  for (const auto &[type_code, id] : NovaResource_AllKeys()) {
    if (type_code != kResourceTypeCharacter) {
      continue;
    }
    auto entry = NovaResource_LoadNamed(kResourceTypeCharacter, id);
    if (entry && entry->name == key) {
      return entry;
    }
  }
  return std::nullopt;
}

std::optional<NovaMenuDefinition>
NovaResource_LoadMenuDefinition(std::uint16_t menu_id) {
  const auto data = NovaResource_Load(kResourceTypeMenu, menu_id);
  if (!data || data->size() < 0x10) {
    NovaLog::Todo("MENU {:#06x} absent or truncated", menu_id);
    return std::nullopt;
  }
  const auto bytes = std::span{*data};
  NovaMenuDefinition out;
  std::size_t pos =
      0x0e; // classic menu header is 0x10 bytes; title len at 0x0e
  const std::size_t title_len = std::to_integer<std::size_t>(bytes[pos]);
  ++pos;
  if (pos + title_len > bytes.size()) {
    return std::nullopt;
  }
  out.title.assign(reinterpret_cast<const char *>(bytes.data()) + pos,
                   title_len);
  pos += title_len;
  // Entries: [pascal string][u16 cmd][u16 glyph], trailing 0x00 terminator.
  while (pos < bytes.size() && std::to_integer<unsigned>(bytes[pos]) != 0) {
    const std::size_t len = std::to_integer<std::size_t>(bytes[pos]);
    ++pos;
    if (pos + len > bytes.size()) {
      break;
    }
    out.entries.emplace_back(reinterpret_cast<const char *>(bytes.data()) + pos,
                             len);
    pos += len + 4; // cmd + glyph shorts
  }
  return out;
}

std::optional<NovaStellarDescription>
NovaResource_LoadDescription(std::uint16_t resource_id) {
  const auto data = NovaResource_Load(kResourceTypeDescription, resource_id);
  if (!data || data->size() < 2) {
    NovaLog::Todo("description desc {} absent or truncated", resource_id);
    return std::nullopt;
  }
  const auto bytes = std::span{*data};
  const std::size_t size = bytes.size();

  NovaStellarDescription out;
  auto *raw = reinterpret_cast<const char *>(bytes.data());

  // Leading NUL-terminated C-string: the landing description text.
  {
    const void *end = std::memchr(raw, '\0', size);
    const std::size_t len =
        end == nullptr
            ? size
            : static_cast<std::size_t>(static_cast<const char *>(end) - raw);
    out.text.assign(raw, len);

    // After the text NUL, Ui_LoadSelectionDialogResource reads the 2-byte BE
    // selection-dialog variant/picture id at text_len+1..+2, then the trailing
    // status C-string (<=0x20 chars) at text_len+3.
    if (len + 3 <= size) {
      out.dialog_variant = ReadBeI16(bytes, len + 1);
      const std::size_t status_rem = size - (len + 3);
      const void *status_end = std::memchr(raw + len + 3, '\0', status_rem);
      std::size_t status_len =
          status_end == nullptr
              ? status_rem
              : static_cast<std::size_t>(static_cast<const char *>(status_end) -
                                         (raw + len + 3));
      status_len = std::min(status_len, std::size_t{0x20});
      out.status.assign(raw + len + 3, status_len);
    }
  }
  return out;
}

std::optional<NovaStellarDescription>
NovaResource_LoadStellarDescription(std::int16_t stellar_id) {
  // Prompt text is keyed by the stellar's raw resource id (>= 0x80): the main
  // docked window calls Ui_LoadSelectionDialogResource(stellar_id + 0x80)
  // where its arg is the stellar's 0-based slot, i.e. the desc id = the raw
  // stellar id. Resources that carry a block for a stellar are validated
  // against the resource id clamp the loader uses (0x80.. *).
  if (stellar_id < 0x80) {
    return std::nullopt;
  }
  return NovaResource_LoadDescription(static_cast<std::uint16_t>(stellar_id));
}
