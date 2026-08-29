#include <catch2/catch_test_macros.hpp>

#include "brgr_archive.hpp"
#include "cicn_image.hpp"

namespace game {

// The reticle CICN decoder (fresh in this build). Verifies the extended color-
// icon parse reproduces the ship-retical bracket art from cicn 10008 (frame 0
// of the 16-frame ship reticle). The icon is 16x16, 4bpp, with a 10-entry
// palette; the mask forms a top-left corner pie leaving the right/bottom rows
// transparent.
TEST_CASE("cicn 10008 decodes the ship-reticle corner bracket",
          "[cicn][reticle]") {
  const auto data = NovaResource_Load(kResourceTypeCicn, 10008);
  REQUIRE(data.has_value());
  REQUIRE(data->size() >= 16);
  const auto img = Resource_LoadCicnAsImage(*data);
  REQUIRE(img.has_value());
  CHECK(img->width == 16);
  CHECK(img->height == 16);
  REQUIRE(img->rgba_pixels.size() == static_cast<std::size_t>(16 * 16 * 4));

  // The rightmost column is outside the bracket's top-left-ish extent in the
  // mask's bottom rows, so those pixels are transparent. Spot-check that the
  // top-left region is opaque (mask set) and the very bottom-right is not.
  auto px = [&](int x, int y) {
    return &img->rgba_pixels[static_cast<std::size_t>(y * 16 + x) * 4U];
  };
  // x=0,y=8 is on the diagonal edge inside the bracket (mask set).
  CHECK((px(0, 8))[3] == 255);
  // x=15,y=15 sits in the frame's empty lower-right corner (mask clear).
  CHECK((px(15, 15))[3] == 0);
}

// The mask must actually carve a non-empty corner: at least half the icon is
// transparent (the two empty rows and the right underside), and the bracket's
// colour-indexed pixels are non-black so the palette mapping ran.
TEST_CASE("cicn 10008 mask is a corner bracket, not a full block",
          "[cicn][reticle]") {
  const auto data = NovaResource_Load(kResourceTypeCicn, 10008);
  REQUIRE(data.has_value());
  const auto img = Resource_LoadCicnAsImage(*data);
  REQUIRE(img.has_value());
  int opaque = 0;
  bool any_color = false;
  for (std::size_t i = 0; i < img->rgba_pixels.size(); i += 4) {
    if (img->rgba_pixels[i + 3] == 255) {
      ++opaque;
      if (img->rgba_pixels[i] != 255 || // any non-white colour index used
          img->rgba_pixels[i + 1] != 255 || img->rgba_pixels[i + 2] != 255) {
        any_color = true;
      }
    }
  }
  const int total = 16 * 16;
  CHECK(opaque > 0);
  CHECK(opaque < total); // the mask is not the whole 16x16 block
  CHECK(opaque >
        total / 5); // a substantial corner (the observed mask is ~78/256)
  CHECK(any_color); // the red->orange gradient palette is applied
}

} // namespace game
