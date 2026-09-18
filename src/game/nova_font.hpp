#pragma once

// Clean-room reconstruction of EV Nova's text rendering, mirroring the
// original's font subsystem. The original (Windows CE build, and the Mac
// Carbon build beneath it) draws all UI text through the OS font engine with
// TrueType screen fonts, NOT an embedded bitmap glyph sheet:
//
//   * A DrawContext carries the active font family id, a scaled point size
//     (DrawContext_StoreScaledValue applies the UI-scale factor), font style
//     flags, an RGBA text color (DrawContext_SetRgbColor) and a pair of
//     cursor coordinates used as the text baseline.
//   * FontCache_GetOrCreateFontHandle (0x004bc670) resolves (family, size,
//     style) to a cached OS font handle; FontCache_InitializeDefaultFamilies
//     (0x004bc3e0) registers the classic Mac families Chicago, Times, NewYork,
//     Geneva and Helvetica.
//   * Text is then measured (FUN_004bcb00, via the OS text engine
//     DT_SINGLELINE|DT_NOCLIP|DT_CALCRECT) and drawn (FUN_004bc760) at the
//     baseline.
//
// The Windows CE build substituted each Mac family to a TrueType font and
// shipped Charcoal.ttf and Geneva.ttf beside the executable (see the font
// substitution table around 0x00575d80 / PTR_s_Times_00575d80):
//
//     Chicago   -> Charcoal      (bundled Charcoal.ttf, UI/window title font)
//     Geneva    -> Geneva        (bundled Geneva.ttf, HUD/body font)
//     Times     -> Times New Roman
//     Helvetica -> Arial
//     New York  -> Georgia
//
// This module reconstructs that subsystem on top of SDL3_ttf so the game
// renders with the same screen-font families (and, for the bundled pair, the
// exact shipped TTFs) at the original logical-resolution metrics. It is kept
// free of any one UI screen and is shared by the landed window, HUD and (later)
// every modal.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "../sdl_platform.hpp"

// SDL3_ttf font handle (opaque). Declared here so this header can expose the
// FontCache without pulling SDL_ttf.h into every consumer.
struct TTF_Font;

namespace game {

// The original font family ids in the order FontCache_InitializeDefaultFamilies
// registers them (Chicago first = id 0, the default UI font), matching the
// family-index used by DrawContext_SetFontId.
enum class NovaFontFamily : std::uint16_t {
  kChicago = 0,   // -> bundled Charcoal.ttf (UI / window title text)
  kTimes = 1,     // -> Times New Roman (mission / body serif)
  kNewYork = 2,   // -> Georgia (large serif head)
  kGeneva = 3,    // -> bundled Geneva.ttf (HUD / body sans)
  kHelvetica = 4, // -> Arial (secondary sans)
};

// Mirrors the style bits packed into the DrawContext font-style field
// (field_0x4a) that FontFamily_CreateFontHandle (0x004bc450) consumes: bit 0
// = bold -> font weight 700, bit 1 = italic.
enum NovaFontStyle : std::uint16_t {
  kNovaFontStyleRegular = 0,
  kNovaFontStyleBold = 1 << 0,
  kNovaFontStyleItalic = 1 << 1,
};

// The logical UI resolution the original renders against (the base game
// surface). Text sizes and on-screen box coordinates are chosen in this space.
inline constexpr int kLogicalUiWidth = 640;
inline constexpr int kLogicalUiHeight = 480;

// A resolved, rasterizable font face for one (family, size, style) triple.
// The original caches OS font handles by exactly that key
// (FontFamily_FindHandleBySizeAndStyle / FontFamily_CreateFontHandle).
//
// Divergence from the original: the original keeps a single process-global
// font cache (FontCache_InitializeDefaultFamilies, 0x004bc3e0); our port uses
// short-lived per-screen instances, each holding its own handle map and its
// own TTF_Init/TTF_Quit refcount. Only the sanitized font file *images* are
// shared process-wide (nova_font.cpp GetFontImage), so the original's one
// cache is effectively split in two. This changes resource lifetime/reuse,
// not the text that is measured or drawn.
class NovaFontCache {
public:
  NovaFontCache() = default;
  ~NovaFontCache();

  NovaFontCache(const NovaFontCache &) = delete;
  NovaFontCache &operator=(const NovaFontCache &) = delete;

  // Returns the loaded TTF_Font for (family, point_size, style), loading it
  // (and caching the handle) on first use; null when the face could not be
  // resolved. point_size is in the logical 640x480 space (the original's
  // scaled_value before UI scale is applied -- SDL_ttf is given the logical
  // size. NovaText_Draw opens a density-scaled cached face for rasterization,
  // then draws it at these unchanged logical dimensions.
  [[nodiscard]] TTF_Font *Font(NovaFontFamily family,
                               float point_size,
                               std::uint16_t style = kNovaFontStyleRegular);

  // Number of logical pixels the given string occupies at (family, size,
  // style) using the same face the game would measure with (draw path).
  [[nodiscard]] int TextWidth(NovaFontFamily family,
                              float point_size,
                              std::uint16_t style,
                              std::string_view text);

  // Line pitch (ascent + descent + external leading) the original's
  // multi-line DT_WORDBREAK draws advance by (NovaText_DrawText's
  // filled-rect mode, e.g. the mission-info description panel).
  [[nodiscard]] int LineHeight(NovaFontFamily family,
                               float point_size,
                               std::uint16_t style = kNovaFontStyleRegular);

  // True when a face for `family` could be located through the tiered
  // resolver (bundled CE TTF, native macOS family, or the CE substitution /
  // Linux generic), i.e. text will render for it.
  [[nodiscard]] bool IsFamilyAvailable(NovaFontFamily family) const;

  // Releases all cached font handles (called at shutdown).
  void Clear();

private:
  struct FontKey {
    std::uint16_t family;
    float size;
    std::uint16_t style;

    bool operator==(const FontKey &) const = default;
  };

  struct FontKeyHash {
    std::size_t operator()(const FontKey &k) const noexcept {
      std::size_t h = static_cast<std::size_t>(k.family);
      h = h * 31 + static_cast<std::size_t>(k.size * 64.0F);
      h = h * 31 + static_cast<std::size_t>(k.style);
      return h;
    }
  };

  // Resolves a family through the tiers: bundled CE TTF, native macOS face,
  // CE GDI substitution / Linux generic, then bundled Geneva.ttf as the last
  // resort (logged once per family). Empty when nothing resolves.
  [[nodiscard]] std::string ResolveFontFile(NovaFontFamily family) const;

  std::unordered_map<FontKey, TTF_Font *, FontKeyHash> fonts_;
  // Whether the (refcounted) SDL_ttf library is currently held by this cache;
  // set lazily on first font open and torn down by the destructor. SDL_ttf's
  // refcount makes nested caches safe.
  bool ttf_initialized_ = false;
};

// Decodes the classic Mac OS "MacRoman" text that every game resource string
// (STR# pools, resource names, descriptions) is encoded in, into UTF-8 for
// SDL3_ttf. ASCII passes through unchanged; 0x80..0xFF map through Apple's
// standard MacRoman table (0xAA -> U+2122 trade mark, 0xA1 -> U+00B0 degree).
// The game deliberately keeps MacRoman as its internal representation, so this
// conversion belongs at the font boundary and nowhere else.
[[nodiscard]] std::string NovaText_MacRomanToUtf8(std::string_view text);

// Returns `text` unchanged when it is already valid UTF-8; otherwise decodes it
// as MacRoman. For data boundaries that must emit valid UTF-8 (currently the
// probe harness's JSON) but may carry either raw MacRoman game strings or
// already-UTF-8 user input.
[[nodiscard]] std::string NovaText_EncodeUtf8(std::string_view text);

// Draws `text` with family/size/style at the given logical coordinates with
// `color`, treating (x, y) as the *baseline* exactly as the original's cursor
// does (FUN_004bc760 places the glyph rect top at baseline - fontsize and the
// OS engine's ascent positions the ink above the baseline). The glyphs are
// antialiased by SDL3_ttf at the current physical output density, matching the
// scale-then-rasterize transparent-background DrawTextW path in the original,
// then drawn at their unchanged logical size. The font cache member must
// outlive the call; the platform owns the renderer.
void NovaText_Draw(SdlPlatform &platform,
                   NovaFontCache &cache,
                   NovaFontFamily family,
                   float point_size,
                   std::uint16_t style,
                   const SDL_Color &color,
                   float baseline_x,
                   float baseline_y,
                   std::string_view text);

// Draws `text` centered horizontally between x_left and x_right at the given
// baseline y. Mirrors DrawContext_DrawCenteredPascalStringInBounds
// (0x004622f0): measures the width, centers it over the bounds, then draws.
void NovaText_DrawCentered(SdlPlatform &platform,
                           NovaFontCache &cache,
                           NovaFontFamily family,
                           float point_size,
                           std::uint16_t style,
                           const SDL_Color &color,
                           float x_left,
                           float x_right,
                           float baseline_y,
                           std::string_view text);

} // namespace game
