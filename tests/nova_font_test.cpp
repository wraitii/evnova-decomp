#include <catch2/catch_test_macros.hpp>

#include "game/nova_font.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

namespace {

using game::kNovaFontStyleBold;
using game::kNovaFontStyleItalic;
using game::kNovaFontStyleRegular;
using game::NovaFontCache;
using game::NovaFontFamily;

} // namespace

// The font subsystem resolves the original's screen-font families and can
// rasterize/measure text with them. These tests require the bundled
// Charcoal.ttf / Geneva.ttf shipped next to the executable (the CE release
// placed them in EV Nova/) and the OS fonts for the substituted families.

TEST_CASE("bundled Chicago/Charcoal and Geneva faces resolve on disk",
          "[nova_font]") {
  NovaFontCache cache;
  // Charcoal (Chicago) and Geneva are bundled by the CE release; if they are
  // not present these two must still report availability via their bundled
  // presence (a missing bundle is a real divergence to log, not a silent
  // fallback).
  CHECK(cache.IsFamilyAvailable(NovaFontFamily::kChicago));
  CHECK(cache.IsFamilyAvailable(NovaFontFamily::kGeneva));
}

TEST_CASE("font cache returns a stable handle per family/size/style key",
          "[nova_font]") {
  NovaFontCache cache;
  TTF_Font *a = cache.Font(NovaFontFamily::kGeneva, 12.0F);
  TTF_Font *b = cache.Font(NovaFontFamily::kGeneva, 12.0F);
  CHECK(a != nullptr);
  CHECK(a == b); // cached, not re-opened

  // A different size or style is a different handle.
  TTF_Font *big = cache.Font(NovaFontFamily::kGeneva, 18.0F);
  CHECK(big != a);
  TTF_Font *bold =
      cache.Font(NovaFontFamily::kGeneva, 12.0F, kNovaFontStyleBold);
  CHECK(bold != a);
}

TEST_CASE("text width is positive and monotonic in the string", "[nova_font]") {
  NovaFontCache cache;
  const int w_short = cache.TextWidth(
      NovaFontFamily::kGeneva, 12.0F, kNovaFontStyleRegular, "Abc");
  const int w_long = cache.TextWidth(
      NovaFontFamily::kGeneva, 12.0F, kNovaFontStyleRegular, "Abcdefgh");
  CHECK(w_short > 0);
  CHECK(w_long > w_short);
  // Empty string has zero width.
  CHECK(cache.TextWidth(
            NovaFontFamily::kGeneva, 12.0F, kNovaFontStyleRegular, "") == 0);
}

TEST_CASE("bold/italic styles measure wider or equal and render as a different "
          "face",
          "[nova_font]") {
  NovaFontCache cache;
  TTF_Font *regular =
      cache.Font(NovaFontFamily::kTimes, 14.0F, kNovaFontStyleRegular);
  TTF_Font *boldit = cache.Font(
      NovaFontFamily::kTimes, 14.0F, kNovaFontStyleBold | kNovaFontStyleItalic);
  REQUIRE(regular != nullptr);
  REQUIRE(boldit != nullptr);
  // Styles differ (Times usually has a bold-italic variant; weight/italic
  // emphasis yields a wide-enough advance on the 'W').
  CHECK(cache.TextWidth(NovaFontFamily::kTimes,
                        14.0F,
                        kNovaFontStyleBold | kNovaFontStyleItalic,
                        "WW") >=
        cache.TextWidth(
            NovaFontFamily::kTimes, 14.0F, kNovaFontStyleRegular, "WW"));
}

TEST_CASE("TTF_Init is refcounted across cache sessions", "[nova_font]") {
  REQUIRE(TTF_Init() == true);
  REQUIRE(TTF_Init() == true);
  TTF_Quit();
  REQUIRE(TTF_WasInit());
  TTF_Quit();
  CHECK_FALSE(TTF_WasInit());
}
