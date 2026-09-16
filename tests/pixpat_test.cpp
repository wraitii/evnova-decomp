#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "pixpat_image.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// The radar interference static is tiled from the ten `ppat` resources
// 128..137 (NovaUi_DrawStellarRadarPanel 0x0045d600 ->
// DrawContext_TileImageInRect 0x004bbdc0). Resource_LoadPixPatAsImage decodes
// them. These tests run against the shipped archives: ppat 128 is the 4-bit
// Nova.rez pattern (the port's first-match archive order resolves it there; the
// original's newest-first order would pick Nova Graphics 1 instead -- see
// hud_renderer.hpp), and ppat 129 is an 8-bit Nova Graphics 1 pattern with a
// colour palette.
TEST_CASE("ppat 128 decodes the 4-bit grayscale radar pattern",
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
  // The pattern is mostly black with sparse white dots; the first dot sits at
  // (39, 3) in the decoded raster.
  CHECK((px(0, 0))[0] == 0);
  CHECK((px(0, 0))[3] == 255);
  CHECK((px(39, 3))[0] == 255);
  CHECK((px(39, 3))[1] == 255);
  CHECK((px(39, 3))[2] == 255);
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
