#include "nova_font.hpp"

#include "../log.hpp"
#include "../nova_paths.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

namespace game {

namespace {

constexpr float kDefaultPointSize = 12.0F;

// Apple's MacRoman 0x80..0xFF -> Unicode code points. All values fit in a
// uint16_t (the private-use Apple logo is U+F8FF).
constexpr std::array<std::uint16_t, 128> kMacRomanToUnicode{
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0,
    0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB,
    0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6,
    0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3,
    0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8,
    0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5,
    0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6,
    0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013,
    0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178,
    0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A,
    0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE,
    0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
    0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD,
    0x02DB, 0x02C7,
};

// True when `text` is well-formed UTF-8. The probe harness's JSON output must
// be valid UTF-8, but game resource strings are raw MacRoman while
// probe-authored/user-entered strings are already UTF-8, so the two have to be
// told apart at that boundary.
[[nodiscard]] bool IsValidUtf8(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<std::uint8_t>(text[i]);
    if (lead < 0x80U) {
      ++i;
      continue;
    }
    std::size_t continuation = 0;
    std::uint32_t code_point = 0;
    if ((lead & 0xE0U) == 0xC0U) {
      continuation = 1;
      code_point = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
      continuation = 2;
      code_point = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
      continuation = 3;
      code_point = lead & 0x07U;
    } else {
      return false;
    }
    if (i + continuation >= text.size()) {
      return false;
    }
    for (std::size_t j = 1; j <= continuation; ++j) {
      const auto trail = static_cast<std::uint8_t>(text[i + j]);
      if ((trail & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (trail & 0x3FU);
    }
    const std::uint32_t minimum =
        continuation == 1 ? 0x80U : (continuation == 2 ? 0x800U : 0x10000U);
    if (code_point < minimum || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
      return false;
    }
    i += continuation + 1;
  }
  return true;
}

void AppendUtf8(std::string &out, std::uint32_t code_point) {
  if (code_point < 0x80U) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

// The bundled font filenames (shipped next to the executable by the Windows CE
// release) for the two families that had no OS equivalent on Windows.
constexpr const char *kBundledCharcoal = "Charcoal.ttf";
constexpr const char *kBundledGeneva = "Geneva.ttf";

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

std::string FindBundled(const char *leaf) {
  // The bundled fonts sit in the install root beside Nova.rez / Nova Files.
  const auto install_root = NovaPaths::InstallRoot();
  if (!install_root) {
    return {};
  }
  const auto path = *install_root / leaf;
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && !ec) {
    return path.string();
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

constexpr std::uint32_t Tag(std::string_view tag) {
  return (static_cast<std::uint32_t>(tag[0]) << 24) |
         (static_cast<std::uint32_t>(tag[1]) << 16) |
         (static_cast<std::uint32_t>(tag[2]) << 8) |
         static_cast<std::uint32_t>(tag[3]);
}

std::uint16_t ReadU16(const std::uint8_t *p) {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t ReadU32(const std::uint8_t *p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

void WriteU16(std::uint8_t *p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}

void WriteU32(std::uint8_t *p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}

// TODO(decomp(0x004bc670)) skipped: embedded-bitmap sanitization. The CE
// release's bundled Geneva.ttf is a FontForge conversion whose EBDT/EBLC
// bitmap strikes carry glyph advances inconsistent with the font's own
// outlines (e.g. 'i' at 20 ppem: bitmap advance 8 px vs outline 4.7 px; 'r'
// 12 vs 7.7) -- and FreeType serves a strike whenever the requested pixel
// size matches one exactly, which our 10 px x density raster sizes always
// do. The original's text engines (GDI on CE, classic Mac bitmap Geneva)
// never rasterized these broken strikes, so drawing through them reproduces
// nothing faithful. We rewrite the sfnt table directory in memory with the
// bitmap tables (EBDT/EBLC/EBSC/bdat/bloc) removed, forcing outline
// rendering; the outlines are genuine Geneva (hmtx identical to Apple's
// system face) and render correctly. When `strip_strikes` is false the file
// bytes are returned unmodified (used for the OS-substituted faces, whose
// embedded strikes -- where present -- are legitimate hand-tuned Apple
// bitmaps). Returns the raw file bytes unchanged when the file has no
// strippable tables or does not parse as an sfnt.
std::vector<std::uint8_t> LoadFontFileStrippedOfBitmapStrikes(
    const std::string &path, bool strip_strikes, bool &stripped) {
  stripped = false;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  if (bytes.size() < 12) {
    return bytes;
  }
  if (!strip_strikes) {
    return bytes;
  }
  const std::uint32_t sfnt_version = ReadU32(bytes.data());
  if (sfnt_version != 0x00010000U && sfnt_version != Tag("true") &&
      sfnt_version != Tag("OTTO")) {
    return bytes;
  }
  constexpr std::array<std::uint32_t, 5> kBitmapTags{
      Tag("EBDT"), Tag("EBLC"), Tag("EBSC"), Tag("bdat"), Tag("bloc")};
  const std::uint16_t num_tables = ReadU16(bytes.data() + 4);
  if (bytes.size() < 12U + static_cast<size_t>(num_tables) * 16U) {
    return bytes;
  }
  std::vector<const std::uint8_t *> kept;
  bool has_bitmap_tables = false;
  for (std::uint16_t i = 0; i < num_tables; ++i) {
    const std::uint8_t *entry =
        bytes.data() + 12 + static_cast<size_t>(i) * 16U;
    const std::uint32_t tag = ReadU32(entry);
    if (std::find(kBitmapTags.begin(), kBitmapTags.end(), tag) !=
        kBitmapTags.end()) {
      has_bitmap_tables = true;
      continue;
    }
    kept.push_back(entry);
  }
  if (!has_bitmap_tables) {
    return bytes;
  }

  // Rebuild the file: new directory, then the kept tables at fresh
  // 4-byte-aligned offsets. FreeType does not validate checksums, so the
  // stale head.checkSumAdjustment is left alone.
  std::size_t data_offset = 12 + kept.size() * 16U;
  for (const std::uint8_t *entry : kept) {
    data_offset = (data_offset + ReadU32(entry + 12) + 3U) & ~3U;
  }
  std::vector<std::uint8_t> out(data_offset);
  WriteU32(out.data(), sfnt_version);
  WriteU16(out.data() + 4, static_cast<std::uint16_t>(kept.size()));
  std::size_t write_offset = 12 + kept.size() * 16U;
  for (std::size_t i = 0; i < kept.size(); ++i) {
    const std::uint8_t *entry = kept[i];
    std::uint8_t *out_entry = out.data() + 12 + i * 16U;
    std::memcpy(out_entry, entry, 16);
    WriteU32(out_entry + 8, static_cast<std::uint32_t>(write_offset));
    const std::uint32_t length = ReadU32(entry + 12);
    std::memcpy(
        out.data() + write_offset, bytes.data() + ReadU32(entry + 8), length);
    write_offset = (write_offset + length + 3U) & ~3U;
  }
  stripped = true;
  return out;
}

// Process-wide cache of font file images (raw or sanitized), keyed by path.
// The bytes must outlive every TTF_Font opened through TTF_OpenFontIO, and
// NovaFontCache is constructed fresh for each modal (a dozen-plus sites), so a
// per-instance cache would re-read and re-sanitize -- and re-log -- the same
// file on every dialog. Sharing the image process-wide keeps the backing store
// alive independently of any one cache.
//
// Divergence from the original: the original holds one process-global font
// cache of OS handles (FontCache_InitializeDefaultFamilies, 0x004bc3e0). Our
// port keeps the font *handles* per NovaFontCache instance and only the
// sanitized file *images* process-wide here; the two halves of the original's
// global cache are therefore split, which changes resource lifetime but not
// what is drawn.
//
// Intentionally leaked: SDL_ttf/FreeType teardown can still touch these bytes
// during static destruction, and a handful of font files is negligible.
std::shared_ptr<std::vector<std::uint8_t>> GetFontImage(const std::string &path,
                                                        bool strip_strikes) {
  static auto *cache =
      new std::unordered_map<std::string,
                             std::shared_ptr<std::vector<std::uint8_t>>>();
  const auto existing = cache->find(path);
  if (existing != cache->end()) {
    return existing->second;
  }

  bool stripped = false;
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(
      LoadFontFileStrippedOfBitmapStrikes(path, strip_strikes, stripped));
  if (stripped) {
    NovaLog::Info("font '{}' contains embedded bitmap strikes with advances "
                  "inconsistent with its outlines; stripped them at load time",
                  path);
  }
  cache->emplace(path, bytes);
  return bytes;
}

} // namespace

// Ghidra 0x004bc670 FontCache_GetOrCreateFontHandle.
// DrawContext_SetFontId [0x004b6900] and DrawContext_StoreScaledValue
// [0x004b6920] have no separate port: the "current font/scale" context state
// they mutate is carried by explicit per-call arguments (family, point size,
// QuantizedRasterScale) instead of a global draw context.
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

  // Load (and for the bundled CE faces, sanitize) the file image. Only the
  // bundled CE faces are sanitized: their FontForge-converted embedded strikes
  // are corrupt (see LoadFontFileStrippedOfBitmapStrikes). The image is shared
  // process-wide and must outlive every TTF_Font opened from it.
  const bool bundled_ce_face =
      family == NovaFontFamily::kChicago || family == NovaFontFamily::kGeneva;
  const std::shared_ptr<std::vector<std::uint8_t>> image =
      GetFontImage(file, bundled_ce_face);

  // This size is the concrete raster size requested by the caller. Text draw
  // calls multiply their logical size by the current output density first.
  SDL_IOStream *io =
      SDL_IOFromMem(image->data(), static_cast<std::size_t>(image->size()));
  TTF_Font *font = TTF_OpenFontIO(io, true, point_size);
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

// Decodes MacRoman resource/UI text to UTF-8 for the SDL3_ttf renderer. See
// the header for why this lives at the font boundary.
std::string NovaText_MacRomanToUtf8(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char raw : text) {
    const auto byte = static_cast<std::uint8_t>(raw);
    if (byte < 0x80U) {
      out.push_back(raw);
    } else {
      AppendUtf8(out, kMacRomanToUnicode[byte - 0x80U]);
    }
  }
  return out;
}

// Returns `text` unchanged when it is already valid UTF-8; otherwise decodes
// it as MacRoman. Unlike NovaText_MacRomanToUtf8 this is for data boundaries
// (currently the probe harness's JSON) that must emit valid UTF-8 but may
// receive either raw MacRoman game strings or already-UTF-8 user input.
std::string NovaText_EncodeUtf8(std::string_view text) {
  if (IsValidUtf8(text)) {
    return std::string(text);
  }
  return NovaText_MacRomanToUtf8(text);
}

// Ghidra 0x004bcad0 DrawContext_GetPascalStringWidth.
int NovaFontCache::TextWidth(NovaFontFamily family,
                             float point_size,
                             std::uint16_t style,
                             std::string_view text) {
  TTF_Font *font = Font(family, point_size, style);
  if (font == nullptr || text.empty()) {
    return 0;
  }
  const std::string buf = NovaText_MacRomanToUtf8(text);
  int width = 0;
  if (!TTF_GetStringSize(font, buf.c_str(), buf.size(), &width, nullptr)) {
    return 0;
  }
  return width;
}

// Line pitch of the original's DT_WORDBREAK multi-line draws: the GDI text
// engine advances by tmHeight + tmExternalLeading, which TTF_FontHeight
// mirrors closely enough at Nova's small UI sizes.
int NovaFontCache::LineHeight(NovaFontFamily family,
                              float point_size,
                              std::uint16_t style) {
  TTF_Font *font = Font(family, point_size, style);
  return font != nullptr ? TTF_GetFontHeight(font) : 0;
}

// Ghidra 0x004bca90 DrawContext_DrawPascalString.
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
  const std::string buf = NovaText_MacRomanToUtf8(text);
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

// Ghidra 0x004622f0 DrawContext_DrawCenteredPascalStringInBounds.
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
    const std::string buf = NovaText_MacRomanToUtf8(text);
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
