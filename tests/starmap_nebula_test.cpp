#include <catch2/catch_test_macros.hpp>

#include "game/starmap_internal.hpp"

#include <array>

using game::starmap_detail::ChooseNebulaTier;

namespace {

// Nebula PICT tiers 0..6 in ascending-size order; 0 counts as absent.
constexpr std::array<float, 7> kTierW{
    100.0F, 200.0F, 300.0F, 400.0F, 500.0F, 600.0F, 700.0F};
constexpr std::array<float, 7> kTierH{
    100.0F, 200.0F, 300.0F, 400.0F, 500.0F, 600.0F, 700.0F};

} // namespace

// Ghidra 0x004a5560: the original accepts the first tier that covers the
// destination in either dimension, so a 250x150 rect takes the 200x200 image
// (too narrow) and stretches it up.
TEST_CASE("nebula tier selection reproduces the original single-dimension fit",
          "[starmap][nebula]") {
  CHECK(ChooseNebulaTier(kTierW, kTierH, 250.0F, 150.0F, false) == 1);
}

// BUGFIX(original) under BugFixPolicy::safe: require coverage in both
// dimensions so the slightly-oversized image is scaled down.
TEST_CASE("nebula tier selection scales an oversized image down when safe",
          "[starmap][nebula]") {
  CHECK(ChooseNebulaTier(kTierW, kTierH, 250.0F, 150.0F, true) == 2);
  CHECK(ChooseNebulaTier(kTierW, kTierH, 99.0F, 99.0F, true) == 0);
  CHECK(ChooseNebulaTier(kTierW, kTierH, 300.0F, 300.0F, true) == 2);
}

// With no tier large enough both policies fall back to the largest image.
TEST_CASE("nebula tier selection falls back to the largest when nothing covers",
          "[starmap][nebula]") {
  CHECK(ChooseNebulaTier(kTierW, kTierH, 900.0F, 900.0F, false) == 6);
  CHECK(ChooseNebulaTier(kTierW, kTierH, 900.0F, 900.0F, true) == 6);
}

TEST_CASE("nebula tier selection ignores absent tiers", "[starmap][nebula]") {
  const std::array<float, 7> w{0.0F, 0.0F, 300.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  const std::array<float, 7> h{0.0F, 0.0F, 300.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  CHECK(ChooseNebulaTier(w, h, 250.0F, 250.0F, false) == 2);
  CHECK(ChooseNebulaTier(w, h, 250.0F, 250.0F, true) == 2);
  const std::array<float, 7> none{};
  CHECK(ChooseNebulaTier(none, none, 10.0F, 10.0F, false) == -1);
  CHECK(ChooseNebulaTier(none, none, 10.0F, 10.0F, true) == -1);
}
