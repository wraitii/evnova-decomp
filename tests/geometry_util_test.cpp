#include "util/geometry.hpp"

#include <catch2/catch_test_macros.hpp>

using evnova::util::InsetRect;
using evnova::util::IntersectRect;

TEST_CASE("InsetRect shrinks a rect by axis deltas") {
  const SDL_Rect rect{0, 0, 640, 400};
  const SDL_Rect inset = InsetRect(rect, 16, 8);
  CHECK(inset.x == 16);
  CHECK(inset.y == 8);
  CHECK(inset.w == 640 - 32);
  CHECK(inset.h == 400 - 16);
}

TEST_CASE("IntersectRect mirrors QuickDraw SectRect") {
  const SDL_Rect a{0, 0, 100, 100};

  const auto inner = IntersectRect(a, SDL_Rect{10, 10, 20, 20});
  REQUIRE(inner.has_value());
  CHECK(inner->x == 10);
  CHECK(inner->y == 10);
  CHECK(inner->w == 20);
  CHECK(inner->h == 20);

  const auto corner = IntersectRect(a, SDL_Rect{50, 50, 100, 100});
  REQUIRE(corner.has_value());
  CHECK(corner->x == 50);
  CHECK(corner->y == 50);
  CHECK(corner->w == 50);
  CHECK(corner->h == 50);

  // Edges that merely touch are empty, as are disjoint rects.
  CHECK_FALSE(IntersectRect(a, SDL_Rect{100, 0, 10, 10}).has_value());
  CHECK_FALSE(IntersectRect(a, SDL_Rect{0, 100, 10, 10}).has_value());
  CHECK_FALSE(IntersectRect(a, SDL_Rect{200, 0, 10, 10}).has_value());
}
