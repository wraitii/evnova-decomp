#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "pixpat_image.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// The radar interference static is tiled from the ten `ppat` resources
// 128..137 (NovaUi_DrawStellarRadarPanel 0x0045d600 ->
// DrawContext_TileImageInRect 0x004bbdc0). Resource_LoadPixPatAsImage decodes
// them. These tests run against the shipped archives.
//
// ppat 128 is the one duplicated resource key in the shipped data: Nova.rez
// carries a 4-bit grayscale pattern and Nova Graphics 1 an 8-bit palette
// pattern. ResourceDb_RegisterArchive (0x004ff900) prepends each opened
// archive and ResourceDb_FindRecord (0x004cdfa0) walks newest-first, so the
// original resolves 128 to the Nova Graphics 1 pattern; the port matches that.
// The 4-bit decoder path is covered synthetically below.
TEST_CASE("ppat 128 resolves to the newest archive (Nova Graphics 1)",
          "[pixpat][radar]") {
  const auto data = NovaResource_Load(kResourceTypePpat, 128);
  REQUIRE(data.has_value());
  const auto img = Resource_LoadPixPatAsImage(*data);
  REQUIRE(img.has_value());
  CHECK(img->width == 64);
  CHECK(img->height == 64);
  REQUIRE(img->rgba_pixels.size() == static_cast<std::size_t>(64 * 64 * 4));

  auto px = [&](int x, int y) {
    return &img->rgba_pixels[static_cast<std::size_t>(y * 64 + x) * 4U];
  };
  // Nova Graphics 1's 8-bit pattern: a dark red dot at (39, 3), fully opaque.
  CHECK((px(39, 3))[0] == 51);
  CHECK((px(39, 3))[1] == 0);
  CHECK((px(39, 3))[2] == 0);
  CHECK((px(39, 3))[3] == 255);
}

TEST_CASE("ppat is palette-mapped and opaque", "[pixpat][radar]") {
  const auto data = NovaResource_Load(kResourceTypePpat, 129);
  REQUIRE(data.has_value());
  const auto img = Resource_LoadPixPatAsImage(*data);
  REQUIRE(img.has_value());
  CHECK(img->width == 64);
  CHECK(img->height == 64);

  // The 8-bit Nova Graphics 1 pattern uses a dark colour palette (not the
  // grayscale white-noise the port used to synthesize); (1, 0) is (34, 0, 0)
  // from the resource's ColorTable.
  const auto *dot =
      &img->rgba_pixels[static_cast<std::size_t>(0 * 64 + 1) * 4U];
  CHECK(dot[0] == 34);
  CHECK(dot[1] == 0);
  CHECK(dot[2] == 0);
  CHECK(dot[3] == 255);

  // Every decoded pixel is opaque (the pattern is blitted with no mask).
  for (std::size_t i = 3; i < img->rgba_pixels.size(); i += 4) {
    REQUIRE(img->rgba_pixels[i] == 255);
  }
}

TEST_CASE("ppat rejects malformed payloads", "[pixpat][radar]") {
  const auto data = NovaResource_Load(kResourceTypePpat, 128);
  REQUIRE(data.has_value());
  // A truncated payload (and the wrong patType at +0) must decode to nullopt
  // rather than reading out of bounds.
  auto truncated = *data;
  truncated.resize(8);
  CHECK_FALSE(Resource_LoadPixPatAsImage(truncated).has_value());
}

// The shipped interference patterns are 4- and 8-bit; exercise the 1-bit
// unpacking path with a hand-built 4x2 payload so the MSB-first bit extraction
// and the ColorTable low-byte mapping are pinned even without shipped 1-bit
// data.
TEST_CASE("ppat unpacks 1-bit rows", "[pixpat][radar]") {
  std::vector<std::byte> payload(106, std::byte{0});
  const auto write_be16 = [&](std::size_t offset, std::uint16_t value) {
    payload[offset] = static_cast<std::byte>(value >> 8);
    payload[offset + 1] = static_cast<std::byte>(value & 0xffU);
  };
  const auto write_be32 = [&](std::size_t offset, std::uint32_t value) {
    payload[offset] = static_cast<std::byte>(value >> 24);
    payload[offset + 1] = static_cast<std::byte>((value >> 16) & 0xffU);
    payload[offset + 2] = static_cast<std::byte>((value >> 8) & 0xffU);
    payload[offset + 3] = static_cast<std::byte>(value & 0xffU);
  };
  write_be16(0, 1);  // patType
  write_be32(2, 28); // PixMap offset
  write_be32(6, 80); // pixel raster offset
  constexpr std::size_t kMap = 28;
  write_be16(kMap + 4, 0x8001);  // rowBytes flag + 1 byte per row
  write_be16(kMap + 6, 0);       // bounds.top
  write_be16(kMap + 8, 0);       // bounds.left
  write_be16(kMap + 0xa, 2);     // bounds.bottom
  write_be16(kMap + 0xc, 4);     // bounds.right
  write_be16(kMap + 0x20, 1);    // 1 bit per pixel
  write_be32(kMap + 0x2a, 82);   // ColorTable offset
  payload[80] = std::byte{0xa0}; // row 0: 1 0 1 0
  payload[81] = std::byte{0x50}; // row 1: 0 1 0 1
  constexpr std::size_t kTable = 82;
  write_be16(kTable + 6, 2); // entry count
  // Entry 0: index 0 -> red (ColorTable reads the low byte of each component).
  write_be16(kTable + 8, 0);
  write_be16(kTable + 10, 0x00ff);
  // Entry 1: index 1 -> green.
  write_be16(kTable + 16, 1);
  write_be16(kTable + 20, 0x00ff);

  const auto img = Resource_LoadPixPatAsImage(payload);
  REQUIRE(img.has_value());
  CHECK(img->width == 4);
  CHECK(img->height == 2);
  const auto px = [&](int x, int y) {
    return &img->rgba_pixels[static_cast<std::size_t>(y * 4 + x) * 4U];
  };
  CHECK((px(0, 0))[1] == 255); // index 1 -> green
  CHECK((px(1, 0))[0] == 255); // index 0 -> red
  CHECK((px(2, 0))[1] == 255);
  CHECK((px(3, 0))[0] == 255);
  CHECK((px(0, 1))[0] == 255);
  CHECK((px(1, 1))[1] == 255);
  CHECK((px(2, 1))[0] == 255);
  CHECK((px(3, 1))[1] == 255);
}

// Exercise the 4-bit unpacking path (two pixels per byte, 4-bit shift step)
// with a hand-built 4x2 payload, mirroring the 1-bit case above.
TEST_CASE("ppat unpacks 4-bit rows", "[pixpat][radar]") {
  std::vector<std::byte> payload(0xa0, std::byte{0});
  const auto write_be16 = [&](std::size_t offset, std::uint16_t value) {
    payload[offset] = static_cast<std::byte>(value >> 8);
    payload[offset + 1] = static_cast<std::byte>(value & 0xffU);
  };
  const auto write_be32 = [&](std::size_t offset, std::uint32_t value) {
    payload[offset] = static_cast<std::byte>(value >> 24);
    payload[offset + 1] = static_cast<std::byte>((value >> 16) & 0xffU);
    payload[offset + 2] = static_cast<std::byte>((value >> 8) & 0xffU);
    payload[offset + 3] = static_cast<std::byte>(value & 0xffU);
  };
  write_be16(0, 1);  // patType
  write_be32(2, 28); // PixMap offset
  write_be32(6, 80); // pixel raster offset
  constexpr std::size_t kMap = 28;
  write_be16(kMap + 4, 0x8002);  // rowBytes flag + 2 bytes per row
  write_be16(kMap + 6, 0);       // bounds.top
  write_be16(kMap + 8, 0);       // bounds.left
  write_be16(kMap + 0xa, 2);     // bounds.bottom
  write_be16(kMap + 0xc, 4);     // bounds.right
  write_be16(kMap + 0x20, 4);    // 4 bits per pixel
  write_be32(kMap + 0x2a, 84);   // ColorTable offset
  payload[80] = std::byte{0x01}; // row 0: indices 0 1 2 3
  payload[81] = std::byte{0x23};
  payload[82] = std::byte{0x45}; // row 1: indices 4 5 6 7
  payload[83] = std::byte{0x67};
  constexpr std::size_t kTable = 84;
  write_be16(kTable + 6, 8); // entry count
  const auto entry = [&](std::size_t index,
                         std::uint8_t red,
                         std::uint8_t green,
                         std::uint8_t blue) {
    const std::size_t base = kTable + 8 + index * 8;
    write_be16(base, static_cast<std::uint16_t>(index));
    write_be16(base + 2, static_cast<std::uint16_t>(red));
    write_be16(base + 4, static_cast<std::uint16_t>(green));
    write_be16(base + 6, static_cast<std::uint16_t>(blue));
  };
  entry(0, 0xff, 0, 0);
  entry(1, 0, 0xff, 0);
  entry(2, 0, 0, 0xff);
  entry(3, 0xff, 0xff, 0xff);
  entry(4, 0x11, 0, 0);
  entry(5, 0, 0x22, 0);
  entry(6, 0, 0, 0x33);
  entry(7, 0x44, 0x55, 0x66);

  const auto img = Resource_LoadPixPatAsImage(payload);
  REQUIRE(img.has_value());
  CHECK(img->width == 4);
  CHECK(img->height == 2);
  const auto px = [&](int x, int y) {
    return &img->rgba_pixels[static_cast<std::size_t>(y * 4 + x) * 4U];
  };
  CHECK((px(0, 0))[0] == 0xff); // index 0 -> red
  CHECK((px(1, 0))[1] == 0xff); // index 1 -> green
  CHECK((px(2, 0))[2] == 0xff); // index 2 -> blue
  CHECK((px(3, 0))[0] == 0xff); // index 3 -> white
  CHECK((px(3, 0))[1] == 0xff);
  CHECK((px(3, 0))[2] == 0xff);
  CHECK((px(0, 1))[0] == 0x11); // index 4
  CHECK((px(1, 1))[1] == 0x22); // index 5
  CHECK((px(2, 1))[2] == 0x33); // index 6
  CHECK((px(3, 1))[0] == 0x44); // index 7
  CHECK((px(3, 1))[1] == 0x55);
  CHECK((px(3, 1))[2] == 0x66);
}
