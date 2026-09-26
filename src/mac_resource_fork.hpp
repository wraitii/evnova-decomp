#pragma once

// Classic Macintosh resource-fork reader.
//
// EV Nova originated on the Mac, where every game-data file and plug-in stored
// its resources in the HFS resource fork. The Windows/CE build that this
// reimplementation follows (via Ghidra) rewrote them into the flat 'BRGR'
// container that src/brgr_archive.cpp parses, and the stock CE executable has
// no fallback for the Mac form (its 'Convert Plug-ins.exe' did the conversion
// offline). This module reads the original fork directly so the port can
// consume Mac-era data and plug-ins unchanged.
//
// Layout (all big-endian; see Inside Macintosh: More Macintosh Toolbox, and
// docs/assets/ResForge/ResForge/Formats/ClassicFormat.swift for a reference
// implementation):
//
//   header (16 bytes): [u32 data offset][u32 map offset][u32 data length]
//                      [u32 map length]
//   data area: one per resource, [u32 payload length][payload]
//   map: 16-byte copy of the header, 4/2/2 bytes reserved, then
//        [u16 type-list offset][u16 name-list offset] (relative to map start)
//   type list: [u16 type count - 1], then 8-byte entries
//        [u32 type code][u16 resource count - 1][u16 ref-list offset]
//   ref lists: 12-byte entries
//        [u16 id][u16 name offset][u8 attributes][u24 data offset][4 reserved]
//        (name offset 0xffff means unnamed; data offset is relative to the data
//        area start and points at the 4-byte payload length)
//   name list: Pascal strings
//
// A resource whose attribute bit 0x01 is set is stored compressed with a
// 'dcmp' decompressor; this reader reports the attribute but does not
// decompress (the caller decides how to handle it).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace evnova::rez {

// Attribute bit for a 'dcmp'-compressed payload (resCompressed).
constexpr std::uint8_t kResourceAttributeCompressed = 0x01;

struct ResourceForkResource {
  std::uint32_t type_code = 0;
  std::uint16_t resource_id = 0;
  std::uint8_t attributes = 0;
  std::string name;
  // Payload slice into ResourceFork::bytes (length prefix already stripped).
  std::size_t offset = 0;
  std::size_t size = 0;
};

struct ResourceFork {
  // Owns the fork image; every resource's offset/size indexes into this.
  std::vector<std::byte> bytes;
  std::vector<ResourceForkResource> resources;
};

// Reads a whole file into memory. Returns nullopt when the file is missing,
// unreadable, or empty.
[[nodiscard]] std::optional<std::vector<std::byte>>
ReadFileBytes(const std::filesystem::path &path);

// Reads at most `max_bytes` from the start of `path` (a short file yields what
// it has). Used to sniff a container before committing to a full read.
[[nodiscard]] std::optional<std::vector<std::byte>>
ReadFilePrefix(const std::filesystem::path &path, std::size_t max_bytes);

// Cheap pre-filters for the sniff in ParseArchive: true when `header` looks
// like a classic resource-fork header for a file of `file_size` bytes, or
// starts with an AppleSingle/AppleDouble signature. A true result still needs
// ParseResourceFork to validate the full map.
[[nodiscard]] bool IsResourceForkHeader(std::span<const std::byte> header,
                                        std::size_t file_size);
[[nodiscard]] bool
IsAppleSingleOrDoubleHeader(std::span<const std::byte> header);

// Parses a complete resource-fork image. Returns nullopt when the header or
// map is malformed; individual out-of-bounds resources are skipped.
[[nodiscard]] std::optional<ResourceFork>
ParseResourceFork(std::span<const std::byte> fork_bytes);

// Parses `data` as a resource fork, unwrapping an AppleSingle (magic
// 0x00051600) or AppleDouble (0x00051607) image first when present. This is
// how the fork survives on hosts and archives that keep it as a data fork.
[[nodiscard]] std::optional<ResourceFork>
ParseResourceForkOrAppleDouble(std::span<const std::byte> data);

// Loads the resource fork stored alongside `path`: the macOS
// "<path>/..namedfork/rsrc" pseudo-file first, then an AppleDouble "._name"
// sidecar so unpacked archives work on other hosts. Returns nullopt when
// neither yields a parseable fork.
[[nodiscard]] std::optional<ResourceFork>
LoadResourceFork(const std::filesystem::path &path);

} // namespace evnova::rez
