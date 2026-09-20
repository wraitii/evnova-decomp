#include "game/nova_list_control.hpp"

#include <catch2/catch_test_macros.hpp>

namespace game {

namespace {

// A 144px-tall list at the BBS DITL 0x3ee position, 12px native pitch.
NovaListControl MakeList(std::size_t rows) {
  return NovaListControl(SDL_FRect{10.0F, 30.0F, 195.0F, 144.0F}, 12.0F, rows);
}

} // namespace

// The visible row count is floor(list height / pitch) and the scroll extent is
// count - visible, exactly as FUN_004d1fb0's integer clamp derives it.
TEST_CASE("native list control derives visible rows and scroll extent",
          "[mission][list]") {
  NovaListControl list = MakeList(20);
  CHECK(list.visible_rows() == 12);
  CHECK(list.max_scroll() == 8);
  CHECK(list.scrollable());

  list = MakeList(12);
  CHECK(list.visible_rows() == 12);
  CHECK(list.max_scroll() == 0);
  CHECK_FALSE(list.scrollable());

  list = MakeList(5);
  CHECK(list.max_scroll() == 0);
  CHECK_FALSE(list.scrollable());
}

// FUN_004d1fb0 clamps the scroll offset into [0, count - visible].
TEST_CASE("native list control clamps scrolling to the row extent",
          "[mission][list]") {
  NovaListControl list = MakeList(20);
  list.ScrollRows(5);
  CHECK(list.scroll_rows() == 5);
  list.ScrollRows(100);
  CHECK(list.scroll_rows() == 8);
  list.ScrollRows(-100);
  CHECK(list.scroll_rows() == 0);
  list.ScrollTo(7);
  CHECK(list.scroll_rows() == 7);
  list.ScrollTo(-1);
  CHECK(list.scroll_rows() == 0);

  // Shrinking the row set re-clamps the offset.
  list.ScrollTo(8);
  list.SetRowCount(4);
  CHECK(list.scroll_rows() == 0);
}

// FUN_004d1d40 scrolls just enough to reveal the selected row.
TEST_CASE("native list control scrolls the selection into view",
          "[mission][list]") {
  NovaListControl list = MakeList(20);
  list.EnsureVisible(0);
  CHECK(list.scroll_rows() == 0);
  list.EnsureVisible(11);
  CHECK(list.scroll_rows() == 0);
  list.EnsureVisible(12);
  CHECK(list.scroll_rows() == 1);
  list.EnsureVisible(19);
  CHECK(list.scroll_rows() == 8);
  list.EnsureVisible(0);
  CHECK(list.scroll_rows() == 0);
}

// FUN_004d1f50 places row i at list.top + (i - scroll) * pitch.
TEST_CASE("native list control row rects follow the scroll offset",
          "[mission][list]") {
  NovaListControl list = MakeList(20);
  SDL_FRect rect = list.RowRect(0);
  CHECK(rect.x == 10.0F);
  CHECK(rect.y == 30.0F);
  CHECK(rect.w == 195.0F);
  CHECK(rect.h == 12.0F);
  list.ScrollRows(2);
  rect = list.RowRect(2);
  CHECK(rect.y == 30.0F);
  rect = list.RowRect(0);
  CHECK(rect.y == 6.0F);
}

// FUN_004d1db0 maps a content click to (y - list.top) / pitch + scroll; points
// outside the band or past the last row find no row.
TEST_CASE("native list control maps points to scrolled rows",
          "[mission][list]") {
  NovaListControl list = MakeList(20);
  list.ScrollRows(3);
  CHECK(list.RowAt(SDL_FPoint{10.0F, 30.0F}) == std::optional<std::size_t>{3});
  CHECK(list.RowAt(SDL_FPoint{10.0F, 41.0F}) == std::optional<std::size_t>{3});
  CHECK(list.RowAt(SDL_FPoint{10.0F, 42.0F}) == std::optional<std::size_t>{4});
  CHECK_FALSE(list.RowAt(SDL_FPoint{10.0F, 29.0F}).has_value());
  CHECK_FALSE(list.RowAt(SDL_FPoint{10.0F, 174.0F}).has_value());

  // Past the final row: 20 rows starting at scroll 3 occupy the first 17 slots.
  NovaListControl shallow = MakeList(4);
  CHECK(shallow.RowAt(SDL_FPoint{10.0F, 30.0F + 4.0F * 12.0F}) == std::nullopt);
}

// The scrollbar is drawn into a caller-supplied strip: arrow caps step one
// row, the track pages by the visible row count.
TEST_CASE("native list scrollbar hit-tests the arrows and trough",
          "[mission][list]") {
  NovaListControl list = MakeList(24);
  const SDL_FRect strip{205.0F, 30.0F, 15.0F, 144.0F};
  // Arrow caps are 15px tall at the top and bottom.
  CHECK(NovaListScrollbarHitTest(list, strip, SDL_FPoint{210.0F, 32.0F}) ==
        NovaListScrollbarPart::kUp);
  CHECK(NovaListScrollbarHitTest(list, strip, SDL_FPoint{210.0F, 170.0F}) ==
        NovaListScrollbarPart::kDown);
  // The thumb sits near the top at scroll 0; a click further down the track
  // pages down.
  CHECK(NovaListScrollbarHitTest(list, strip, SDL_FPoint{210.0F, 60.0F}) ==
        NovaListScrollbarPart::kThumb);
  CHECK(NovaListScrollbarHitTest(list, strip, SDL_FPoint{210.0F, 130.0F}) ==
        NovaListScrollbarPart::kPageDown);
  // Outside the strip / when the list fits, nothing is hit.
  CHECK(NovaListScrollbarHitTest(list, strip, SDL_FPoint{100.0F, 90.0F}) ==
        NovaListScrollbarPart::kNone);
  NovaListControl fitted = MakeList(4);
  CHECK(NovaListScrollbarHitTest(fitted, strip, SDL_FPoint{210.0F, 32.0F}) ==
        NovaListScrollbarPart::kNone);
}

} // namespace game
