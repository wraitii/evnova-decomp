#include "game/selection_text_dialog.hpp"

#include "game/nova_font.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace game {

namespace {

// A view with a known scroll extent that does not depend on font metrics:
// each '\r' forces a line, and the view height leaves a deterministic
// max_scroll. Font loading may fail in the test environment (TextWidth then
// returns 0), which only collapses word wrapping -- explicit line breaks still
// produce one line each.
NovaTextScrollView MakeTallView(NovaFontCache &fonts) {
  std::string text;
  for (int i = 0; i < 100; ++i) {
    text += "line";
    if (i != 99) {
      text += '\r';
    }
  }
  return NovaTextScrollView(fonts, text, SDL_FRect{0.0F, 0.0F, 200.0F, 100.0F});
}

} // namespace

TEST_CASE("scroll keys map the port-normalized navigation codes",
          "[selection-text]") {
  CHECK(MapTextScrollKey(0x61) == TextScrollKey::kLineUp);
  CHECK(MapTextScrollKey(0x66) == TextScrollKey::kLineDown);
  CHECK(MapTextScrollKey(0x60) == TextScrollKey::kHome);
  CHECK(MapTextScrollKey(0x62) == TextScrollKey::kPageUp);
  CHECK(MapTextScrollKey(0x65) == TextScrollKey::kEnd);
  CHECK(MapTextScrollKey(0x67) == TextScrollKey::kPageDown);
  CHECK(MapTextScrollKey(0x00) == TextScrollKey::kNone);
  CHECK(MapTextScrollKey(0xffff) == TextScrollKey::kNone);
}

TEST_CASE("scroll keys step, page, and jump to the ends", "[selection-text]") {
  NovaFontCache fonts;
  NovaTextScrollView view = MakeTallView(fonts);
  REQUIRE(view.max_scroll() > 250.0F);
  REQUIRE_FALSE(view.can_scroll_up());
  REQUIRE(view.can_scroll_down());

  // Line steps are +/-10px, down first.
  CHECK(view.ApplyScrollKey(TextScrollKey::kLineDown));
  CHECK(view.scroll_offset() == 10.0F);
  CHECK(view.ApplyScrollKey(TextScrollKey::kLineUp));
  CHECK(view.scroll_offset() == 0.0F);
  // At the top the up action is gated and changes nothing.
  CHECK_FALSE(view.ApplyScrollKey(TextScrollKey::kLineUp));

  // PageDown is 0xfa (250)px, PageUp the same back up.
  CHECK(view.ApplyScrollKey(TextScrollKey::kPageDown));
  CHECK(view.scroll_offset() == 250.0F);
  CHECK(view.ApplyScrollKey(TextScrollKey::kPageUp));
  CHECK(view.scroll_offset() == 0.0F);

  // End jumps to the bottom; a second End is gated.
  CHECK(view.ApplyScrollKey(TextScrollKey::kEnd));
  CHECK(view.scroll_offset() == view.max_scroll());
  CHECK_FALSE(view.ApplyScrollKey(TextScrollKey::kEnd));
  CHECK_FALSE(view.can_scroll_down());

  // Home jumps back to the top; a second Home is gated.
  CHECK(view.ApplyScrollKey(TextScrollKey::kHome));
  CHECK(view.scroll_offset() == 0.0F);
  CHECK_FALSE(view.ApplyScrollKey(TextScrollKey::kHome));
  CHECK_FALSE(view.can_scroll_up());
}

TEST_CASE("arrow hold scrolls one pixel per 60Hz tick", "[selection-text]") {
  NovaFontCache fonts;
  NovaTextScrollView view = MakeTallView(fonts);
  NovaTextScrollHold hold;

  // Tap shorter than a tick still emits the first step.
  hold.Press(view, /*up=*/false, 0);
  CHECK(hold.held());
  CHECK(view.scroll_offset() == 1.0F);
  // Same tick: no additional step.
  CHECK_FALSE(hold.Update(view, /*button_down=*/true, 0));
  CHECK(view.scroll_offset() == 1.0F);
  // Next 60Hz tick: one more pixel.
  CHECK(hold.Update(view, /*button_down=*/true, 17));
  CHECK(view.scroll_offset() == 2.0F);
  // A three-tick gap catches up.
  CHECK(hold.Update(view, /*button_down=*/true, 67));
  CHECK(view.scroll_offset() == 5.0F);
  // Release stops the repeat.
  CHECK_FALSE(hold.Update(view, /*button_down=*/false, 84));
  CHECK_FALSE(hold.held());

  // Probe-style click: the press emits one step, the next frame reports the
  // button up (injected clicks never update SDL's live state), so it stops.
  hold.Press(view, /*up=*/false, 100);
  CHECK(view.scroll_offset() == 6.0F);
  CHECK_FALSE(hold.Update(view, /*button_down=*/false, 117));
  CHECK_FALSE(hold.held());

  // Holding up walks back toward the top and stops at the boundary.
  hold.Press(view, /*up=*/true, 200);
  CHECK(view.scroll_offset() == 5.0F);
  for (std::uint64_t ms = 217; ms <= 1000; ms += 17) {
    (void)hold.Update(view, /*button_down=*/true, ms);
  }
  CHECK(view.scroll_offset() == 0.0F);
  CHECK_FALSE(hold.held());
}

} // namespace game
