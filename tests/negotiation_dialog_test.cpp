// Unit tests for the bribe-cost computation of the destination-interaction
// dialog (src/game/negotiation_dialog.cpp). The cost is a function of the
// player's credits, the government flag gate and the GameState PRNG (so a
// freshly-seeded RNG makes the bounds testable without any SDL/runtime
// dependency).

#include "../src/game/negotiation_dialog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>

namespace game {
namespace {

TEST_CASE("bribe cost: always within the [1000, 900000] clamp",
          "[negotiation]") {
  // A generous credit balance yields a large random upper bound; the cap must
  // still hold regardless of the RNG draw.
  std::mt19937 rng{42};
  for (int i = 0; i < 50; ++i) {
    const std::int32_t huge =
        NovaNegotiation_ComputeBribeCost(rng, 50'000'000, -1);
    CHECK(huge >= 1000);
    CHECK(huge <= 900000);
  }

  // Nearly zero credits are clamped up to the floor.
  const std::int32_t poor = NovaNegotiation_ComputeBribeCost(rng, 500, -1);
  CHECK(poor >= 1000);
  CHECK(poor <= 900000);
}

TEST_CASE("bribe cost: zero / tiny credits never go below the floor",
          "[negotiation]") {
  std::mt19937 rng{42};
  CHECK(NovaNegotiation_ComputeBribeCost(rng, 0, -1) >= 1000);
  CHECK(NovaNegotiation_ComputeBribeCost(rng, 1, -1) >= 1000);
  CHECK(NovaNegotiation_ComputeBribeCost(rng, 1000, -1) >= 1000);
}

TEST_CASE("bribe cost: government id does not affect the base",
          "[negotiation]") {
  // The 1.5x government scale lives in the caller (which looks up the
  // government table); the pure helper ignores the id, so a valid id and an
  // invalid id give the same draw for the same credits + RNG.
  std::mt19937 rng_a{7}, rng_b{7};
  const std::int32_t with_gov =
      NovaNegotiation_ComputeBribeCost(rng_a, 1'000'000, 5);
  const std::int32_t with_none =
      NovaNegotiation_ComputeBribeCost(rng_b, 1'000'000, -1);
  CHECK(with_gov == with_none);
}

} // namespace
} // namespace game
