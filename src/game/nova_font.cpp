#include "nova_font.hpp"

#include "../log.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>

namespace game {

namespace {

constexpr float kDefaultPointSize = 12.0F;

// The bundled font filenames (shipped next to the executable by the Windows CE
// release) for the two families that had no OS equivalent on Windows.
constexpr const char *kBundledCharcoal = "Charcoal.ttf";
constexpr const char *kBundledGeneva = "Geneva.ttf";

// Candidate root directories (same convention as NovaResourceDb in
// brgr_archive.cpp, which searches "EV Nova/" relative to the CWD and build
// tree). The fonts sit one level up from "Nova Files".
constexpr std::array<const char *, 2> kAssetRoots{
    "EV Nova/",
    "../../../EV Nova/",
};

// Well-known per-OS path candidates for the three non-bundled families. Each
// family is tried in order; the first existing file wins. Empty paths are
// skipped.
// Full candidate lists per family as file paths (a resolved face), matching
// the original's Times->Times New Roman, Helvetica->Arial, New York->Georgia
// substitution.
const char *TimesFaceCandidates[] = {
    "/System/Library/Fonts/Times.ttc",
    "/System/Library/Fonts/Supplemental/Times New Roman.ttf",
    "/Library/Fonts/Times New Roman.ttf",
    "C:\\Windows\\Fonts\\times.ttf",
    nullptr,
};

const char *ArialFaceCandidates[] = {
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Arial.ttf",
    "C:\\Windows\\Fonts\\arial.ttf",
    nullptr,
};

const char *GeorgiaFaceCandidates[] = {
    "/System/Library/Fonts/Supplemental/Georgia.ttf",
    "C:\\Windows\\Fonts\\georgia.ttf",
    nullptr,
};

std::string JoinPath(std::string dir, const char *leaf) {
  if (!dir.empty() && dir.back() == '/') {
    dir.pop_back();
  }
  return dir + '/' + leaf;
}

std::string FindBundled(const char *leaf) {
  for (const char *root : kAssetRoots) {
    const std::string path = JoinPath(root, leaf);
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
      return path;
    }
  }
  return {};
}

std::string FindFirstExisting(const char *const *candidates) {
  for (const char *const *p = candidates; *p != nullptr; ++p) {
    std::error_code ec;
    if (std::filesystem::exists(*p, ec) && !ec) {
      return *p;
    }
  }
  return {};
}

// Keep the cache key stable while allowing fractional logical-presentation
// scales. One 64th of a point is far below a physical pixel at UI sizes, but
// prevents every tiny resize delta from creating a distinct font handle.
float QuantizedRasterScale(const SdlPlatform &platform, float point_size) {
  const float logical_size = point_size > 0.0F ? point_size : kDefaultPointSize;
  const float requested_size =
      logical_size * std::max(1.0F, platform.text_raster_scale());
  const float raster_size = std::round(requested_size * 64.0F) / 64.0F;
  return raster_size / logical_size;
}

} // namespace

// ---------------------------------------------------------------------------
// NovaFontCache
// ---------------------------------------------------------------------------
NovaFontCache::~NovaFontCache() {
  Clear();
  // Pair the lazy TTF_Init we performed on first use.
  if (ttf_initialized_) {
    TTF_Quit();
  }
}

void NovaFontCache::Clear() {
  for (auto &[key, font] : fonts_) {
    if (font != nullptr) {
      TTF_CloseFont(font);
    }
  }
  fonts_.clear();
}

std::string NovaFontCache::ResolveFontFile(NovaFontFamily family) const {
  switch (family) {
  case NovaFontFamily::kChicago:
    return FindBundled(kBundledCharcoal);
  case NovaFontFamily::kGeneva:
    return FindBundled(kBundledGeneva);
  case NovaFontFamily::kTimes:
    return FindFirstExisting(TimesFaceCandidates);
  case NovaFontFamily::kHelvetica:
    return FindFirstExisting(ArialFaceCandidates);
  case NovaFontFamily::kNewYork:
    return FindFirstExisting(GeorgiaFaceCandidates);
  }
  return {};
}

bool NovaFontCache::IsFamilyAvailable(NovaFontFamily family) const {
  return !ResolveFontFile(family).empty();
}

TTF_Font *NovaFontCache::Font(NovaFontFamily family,
                              float point_size,
                              std::uint16_t style) {
  if (point_size <= 0.0F) {
    point_size = kDefaultPointSize;
  }
  const FontKey key{static_cast<std::uint16_t>(family), point_size, style};
  const auto it = fonts_.find(key);
  if (it != fonts_.end()) {
    return it->second;
  }

  // Lazily initialise the SDL3_ttf library on first use (the original's font
  // cache manages its own platform-text lifetime; we pair this with TTF_Quit
  // in the destructor).
  if (!ttf_initialized_) {
    if (!TTF_Init()) {
      NovaLog::Error("SDL_ttf failed to initialize: {}", SDL_GetError());
      return nullptr;
    }
    ttf_initialized_ = true;
  }

  std::string file = ResolveFontFile(family);
  if (file.empty()) {
    NovaLog::Warn(
        "font family {} (style {:#x}) unavailable: no font file resolved; "
        "text will not render for this face",
        static_cast<unsigned>(family),
        style);
    // Cache a null so we don't re-probe every frame.
    fonts_[key] = nullptr;
    return nullptr;
  }

  // This size is the concrete raster size requested by the caller. Text draw
  // calls multiply their logical size by the current output density first.
  TTF_Font *font = TTF_OpenFont(file.c_str(), point_size);
  if (font == nullptr) {
    NovaLog::Warn("font family {} failed to open '{}': {}",
                  static_cast<unsigned>(family),
                  file,
                  SDL_GetError());
    fonts_[key] = nullptr;
    return nullptr;
  }

  // Style: bold (bit 0) and italic (bit 1), mirroring
  // FontFamily_CreateFontHandle. SDL3_ttf synthesizes bold when the face lacks
  // a bold weight; this version has no explicit post-open weight setter, so we
  // drive it through the style flags (the bundled Charcoal/Geneva faces have no
  // true bold variant anyway).
  TTF_SetFontStyle(
      font,
      ((style & kNovaFontStyleBold) ? TTF_STYLE_BOLD : TTF_STYLE_NORMAL) |
          ((style & kNovaFontStyleItalic) ? TTF_STYLE_ITALIC : 0U));
  // The original delegates grid fitting to the platform text engine. Light
  // FreeType hinting most closely matches the comparatively restrained GDI /
  // classic Mac screen-font coverage at Nova's small UI sizes.
  TTF_SetFontHinting(font, TTF_HINTING_LIGHT);

  fonts_[key] = font;
  return font;
}

int NovaFontCache::TextWidth(NovaFontFamily family,
                             float point_size,
                             std::uint16_t style,
                             std::string_view text) {
  TTF_Font *font = Font(family, point_size, style);
  if (font == nullptr || text.empty()) {
    return 0;
  }
  std::string buf(text);
  int width = 0;
  if (!TTF_GetStringSize(font, buf.c_str(), buf.size(), &width, nullptr)) {
    return 0;
  }
  return width;
}

// ---------------------------------------------------------------------------
// Text drawing
// ---------------------------------------------------------------------------
// Draws one string: rasterise the glyphs with SDL3_ttf, upload the resulting
// surface to a texture, and blit it so the baseline lands at the requested
// logical y. The original uses DrawTextW with a transparent background, whose
// antialiased coverage is matched by SDL_ttf's blended renderer. This mirrors
// FUN_004bc760's baseline model.
void NovaText_Draw(SdlPlatform &platform,
                   NovaFontCache &cache,
                   NovaFontFamily family,
                   float point_size,
                   std::uint16_t style,
                   const SDL_Color &color,
                   float baseline_x,
                   float baseline_y,
                   std::string_view text) {
  if (text.empty()) {
    return;
  }
  const float logical_size = point_size > 0.0F ? point_size : kDefaultPointSize;
  const float raster_scale = QuantizedRasterScale(platform, point_size);
  TTF_Font *font = cache.Font(family, logical_size * raster_scale, style);
  if (font == nullptr) {
    return;
  }
  std::string buf(text);
  SDL_Surface *surface =
      TTF_RenderText_Blended(font, buf.c_str(), buf.size(), color);
  if (surface == nullptr) {
    NovaLog::Warn("text '{}' failed to render: {}", buf, SDL_GetError());
    return;
  }
  SDL_Texture *texture =
      SDL_CreateTextureFromSurface(platform.renderer(), surface);
  SDL_DestroySurface(surface);
  if (texture == nullptr) {
    NovaLog::Warn("text texture upload failed: {}", SDL_GetError());
    return;
  }
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

  float w = 0, h = 0;
  SDL_GetTextureSize(texture, &w, &h);
  // Place the surface so its baseline (ascent from the top of the glyph
  // surface) sits at baseline_y, matching the original's cursor-as-baseline.
  const float ascent = static_cast<float>(TTF_GetFontAscent(font));
  const SDL_FRect dst{baseline_x,
                      baseline_y - ascent / raster_scale,
                      w / raster_scale,
                      h / raster_scale};
  SDL_RenderTexture(platform.renderer(), texture, nullptr, &dst);
  SDL_DestroyTexture(texture);
}

void NovaText_DrawCentered(SdlPlatform &platform,
                           NovaFontCache &cache,
                           NovaFontFamily family,
                           float point_size,
                           std::uint16_t style,
                           const SDL_Color &color,
                           float x_left,
                           float x_right,
                           float baseline_y,
                           std::string_view text) {
  const float raster_scale = QuantizedRasterScale(platform, point_size);
  const float logical_size = point_size > 0.0F ? point_size : kDefaultPointSize;
  TTF_Font *font = cache.Font(family, logical_size * raster_scale, style);
  int raster_width = 0;
  if (font != nullptr && !text.empty()) {
    const std::string buf(text);
    (void)TTF_GetStringSize(
        font, buf.c_str(), buf.size(), &raster_width, nullptr);
  }
  const float width = static_cast<float>(raster_width) / raster_scale;
  const float center = (x_left + x_right) / 2.0F;
  NovaText_Draw(platform,
                cache,
                family,
                point_size,
                style,
                color,
                center - width / 2.0F,
                baseline_y,
                text);
}

} // namespace game
