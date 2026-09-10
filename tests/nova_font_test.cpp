#include "game/nova_font.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace game {

TEST_CASE("MacRoman text decodes to UTF-8 for the font renderer",
          "[font][encoding]") {
  CHECK(NovaText_MacRomanToUtf8("") == "");
  CHECK(NovaText_MacRomanToUtf8("plain ASCII") == "plain ASCII");

  // 0xA1 is the degree sign (U+00B0, two-byte UTF-8).
  const std::string degree = "\xC2\xB0";
  CHECK(NovaText_MacRomanToUtf8("\xa1") == degree);
  // 0x8E is e-acute (U+00E9).
  CHECK(NovaText_MacRomanToUtf8("\x8e") == "\xC3\xA9");
  // 0xAA is trade mark (U+2122, three-byte UTF-8).
  CHECK(NovaText_MacRomanToUtf8("\xaa") == "\xE2\x84\xA2");
  // 0xF0 is the Apple logo (U+F8FF).
  CHECK(NovaText_MacRomanToUtf8("\xf0") == "\xEF\xA3\xBF");

  // The j\x9fnk record name that previously rendered a replacement glyph.
  CHECK(NovaText_MacRomanToUtf8("Xtreem\xaa Rocket-Boards") ==
        "Xtreem\xE2\x84\xA2 Rocket-Boards");
}

} // namespace game
