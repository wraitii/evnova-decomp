#include "services_buttons.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"

#include <algorithm>
#include <cmath>

#include <optional>
#include <utility>

namespace game {

ServicesButtonArt::~ServicesButtonArt() = default;

float ThreeStateButtonLabelBaseline(const SDL_FRect &rect) {
  return std::floor(rect.y + rect.h / 2.0F) + 5.0F;
}

// @port 0x004B97E0 20% rendering
// Ghidra 0x004b97e0 DrawContext_DrawLineTo.
// The unclipped 45-degree, 2x2-pen slice used by 0x004a3340; the general
// line/clip path remains TODO(decomp). Both button painters call this helper.
void DrawThreeStateButtonArrow(SDL_Renderer *renderer,
                               const SDL_FRect &rect,
                               bool up,
                               const SDL_Color &color) {
  const int s = static_cast<int>(rect.h) / 10;
  const int cx = static_cast<int>(rect.w) / 2;
  const int cy = static_cast<int>(rect.h) / 2;
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  // The line rasterizer decrements the larger endpoint on each axis before
  // stamping the pen. Each arm therefore has 2s stamps, not 2s+1; for s=2
  // their union is 9x5 pixels, centred on the button's local integer midpoint.
  for (int step = 0; step < 2 * s; ++step) {
    const int y = cy - s + step;
    const int left = up ? cx - 1 - step : cx - 2 * s + step;
    const int right = up ? cx + step : cx + 2 * s - 1 - step;
    for (const int x : {left, right}) {
      const SDL_FRect stamp{
          rect.x + static_cast<float>(x), rect.y + static_cast<float>(y), 2, 2};
      SDL_RenderFillRect(renderer, &stamp);
    }
  }
}

void DrawThreeStateButtonLabel(SdlPlatform &platform,
                               NovaFontCache &font_cache,
                               const SDL_FRect &rect,
                               std::string_view label,
                               const SDL_Color &color) {
  if (label.empty()) {
    return;
  }
  if (label.front() == '^' || label.front() == '&') {
    DrawThreeStateButtonArrow(
        platform.renderer(), rect, label.front() == '^', color);
    return;
  }
  const float left = rect.x;
  const float top = rect.y;
  const float right = rect.x + rect.w;
  const float bottom = rect.y + rect.h;
  const float center_x = (left + right) / 2.0F;
  const float center_y = (top + bottom) / 2.0F;
  SDL_Renderer *renderer = platform.renderer();
  // The original sets a 2x2 pen (FUN_004ba320(2,2)) for the icon strokes and
  // resets it to 1x1 afterwards; SDL has no pen size, so draw each segment
  // twice with a one-pixel diagonal offset to approximate it.
  const auto stroke = [&](float x1, float y1, float x2, float y2) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderLine(
        renderer, center_x + x1, center_y + y1, center_x + x2, center_y + y2);
    SDL_RenderLine(renderer,
                   center_x + x1 + 1.0F,
                   center_y + y1 + 1.0F,
                   center_x + x2 + 1.0F,
                   center_y + y2 + 1.0F);
  };
  switch (label.front()) {
  case '+': {
    const float s = (right - left) / 8.0F;
    stroke(-s, 0.0F, s, 0.0F);
    stroke(0.0F, -s, 0.0F, s);
    return;
  }
  case '-': {
    const float s = (right - left) / 9.0F;
    stroke(-s, 0.0F, s, 0.0F);
    return;
  }
  default:
    break;
  }
  NovaText_DrawCentered(platform,
                        font_cache,
                        kThreeStateButtonFontFamily,
                        kThreeStateButtonFontSize,
                        kNovaFontStyleRegular,
                        color,
                        left,
                        right,
                        ThreeStateButtonLabelBaseline(rect),
                        label);
}

namespace {

// The three button strips, in the same order NovaUi_InitThreeStateButtonArt
// (0x004a2f50) loads them: nine consecutive PICTs from 0x1d4c (normal left) to
// 0x1d54 (grey right), i.e. 0x1d4c + state*3 + piece:
//   normal  left/middle/right = 0x1d4c, 0x1d4d, 0x1d4e  ("Button ... Bright")
//   pressed left/middle/right = 0x1d4f, 0x1d50, 0x1d51  ("click ...")
//   grey    left/middle/right = 0x1d52, 0x1d53, 0x1d54  ("grey ...")
constexpr std::uint16_t kStripBase = 0x1d4c;
constexpr std::uint16_t kPiecesPerStrip = 3; // left, middle, right

// Each strip's left/right caps are carved by a 1-bit mask PICT 0x64 above the
// strip's art ids (NovaUi_InitThreeStateButtonArt loads 0x1d4c..0x1d54 AND
// 0x1db0..0x1db8):
//   normal  masks 0x1db0 ("unclick mask left") / 0x1db2 ("unclick mask right")
//   pressed masks 0x1db3 ("click mask left")    / 0x1db5 ("click mask right")
//   grey    masks 0x1db6 ("grey mask left")     / 0x1db8 ("grey mask right")
// The 2px middle tile has no mask (it is fully opaque). Mask semantics follow
// the game's mask compositing (FUN_004b9410): white pixels are masked out and
// stay transparent, black pixels show the art.
constexpr std::uint16_t kMaskOffsetFromArt = 0x64;

// Applies the cap's 1-bit mask PICT (white = transparent) to the cap's RGBA
// pixels so the outside of the rounded corners is fully transparent, as in the
// original (which blits each cap through its mask; without this the cap art's
// opaque corner-cutout pixels show as dark smudges). A missing/undecodable or
// size-mismatched mask leaves the cap fully opaque.
void ApplyCapMask(std::uint16_t art_base,
                  std::size_t piece,
                  const PictImage &cap,
                  std::vector<std::uint8_t> &rgba) {
  const std::uint16_t mask_id = static_cast<std::uint16_t>(
      art_base + kMaskOffsetFromArt + (piece == 2 ? 2 : 0));
  const auto mask_data = NovaResource_LoadPictData(mask_id);
  if (!mask_data) {
    return;
  }
  const auto mask = Resource_LoadPictAsImage(*mask_data);
  if (!mask || mask->width != cap.width || mask->height != cap.height) {
    NovaLog::Todo("button cap mask 0x{:04x} missing or mismatched", mask_id);
    return;
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(cap.width) *
                                  static_cast<std::size_t>(cap.height);
       ++i) {
    // Mask PICTs are 1-bit black/white; white marks the transparent corner
    // cutouts.
    if (mask->rgba_pixels[i * 4] > 127) {
      rgba[i * 4 + 3] = 0;
    }
  }
}

// Loads one strip's three pieces (left cap, stretchable middle tile, right
// cap) from Nova Graphics 3 PICTs `base`, ... `base+2`, applying each cap's
// mask (see ApplyCapMask) so the rounded corners stay transparent.
void LoadStripImpl(SdlPlatform &platform,
                   std::uint16_t base,
                   ServicesButtonArt::StripPieces &out,
                   bool &any_loaded) {
  std::unique_ptr<SdlTexture> *slots[3] = {&out.left, &out.middle, &out.right};
  for (std::size_t piece = 0; piece < kPiecesPerStrip; ++piece) {
    const std::uint16_t id = static_cast<std::uint16_t>(base + piece);
    auto &slot = *slots[piece];
    if (const auto data = NovaResource_LoadPictData(id)) {
      if (const auto pict = Resource_LoadPictAsImage(*data)) {
        std::vector<std::uint8_t> rgba = pict->rgba_pixels;
        if (piece != 1) {
          ApplyCapMask(base, piece, *pict, rgba);
        }
        slot = SdlTexture::Create(
            platform.renderer(), pict->width, pict->height, rgba);
        if (slot) {
          any_loaded = true;
          continue;
        }
        NovaLog::Error("button strip piece 0x{:04x} decoded but upload failed",
                       id);
      } else {
        NovaLog::Todo("button strip PICT 0x{:04x} failed to decode", id);
      }
    }
    // Missing/undecodable piece -> leave slot null; Draw() fills it with the
    // window backdrop so the button still renders.
    slot.reset();
  }
}

// Draws one button piece into the target rect: the source slice is stretched to
// fill `dest` (the 2px middle tile is stretched horizontally across the body);
// if the slice is absent the rect is filled with `fill` (the window backdrop
// colour).
void DrawPiece(SDL_Renderer *renderer,
               SDL_Texture *slice,
               const SDL_Color &fill,
               SDL_FRect dest) {
  if (slice != nullptr) {
    SDL_RenderTexture(renderer, slice, nullptr, &dest);
  } else {
    SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(renderer, &dest);
  }
}

} // namespace

// @port 0x004A2F50 60% ui
// Ghidra 0x004a2f50 NovaUi_InitThreeStateButtonArt: the nine-strip art cache is
// loaded by ServicesButtonArt::Initialize (the STR# 0x96 label table it builds
// is consumed as direct NovaHud_LoadStringEntry calls at each call site).
[[nodiscard]] bool ServicesButtonArt::Initialize(SdlPlatform &platform) {
  bool any = false;
  LoadStripImpl(platform, kStripBase + 0, normal_, any);
  LoadStripImpl(platform, kStripBase + 3, pressed_, any);
  LoadStripImpl(platform, kStripBase + 6, grey_, any);
  usable_ = any;
  if (!usable_) {
    NovaLog::Warn("no three-state button strips loaded; buttons will render "
                  "as flat fills");
  }
  return usable_;
}

// @port 0x004A3340 78% rendering,ui
// Three-state body render via ServicesButtonArt::Draw (normal/pressed/disabled
// strip pick) and the caption pass via DrawThreeStateButtonLabel: centred
// text, or the 2px '^' up / '&' down chevron and '+'/'-' plus/minus icons,
// matching the original's leading-byte branches. Store and text-view arrows
// share DrawThreeStateButtonArrow: local integer centre, s=floor(h/10),
// original endpoint adjustment and 2x2 pen stamps (9x5 footprint at both 23px
// and 25px button heights). Gaps: the -3 caller-supplied-image arm and
// plus/minus pen rasterisation remain approximate. Body uses native 13px caps
// with the middle tile stretched between them; sub-26px buttons overlap the
// caps (right cap last) rather than compressing them.
void ServicesButtonArt::Draw(SdlPlatform &platform,
                             const SDL_FRect &rect,
                             ButtonState state) const {
  SDL_Renderer *renderer = platform.renderer();
  const StripPieces *strip = &normal_;
  if (state == ButtonState::kHover) {
    strip = &pressed_;
  } else if (state == ButtonState::kDisabled) {
    strip = &grey_;
  }
  // Backdrop colour used for missing pieces / the button body.
  const SDL_Color kBackdrop{1, 4, 12, 255};

  // The left/right caps are fixed 13px wide and the 2px middle tile stretches
  // across the body between them. The original blits the left cap
  // (FUN_00873279), then the right cap (FUN_00873310), then the middle band;
  // for the 23px scroll arrows the middle band is empty, so only the caps
  // matter and the right cap, drawn after the left, wins the overlap. Do not
  // compress the caps to fit -- the native 13px art carries the rounded
  // corners and shading.
  constexpr float kCapWidth = 13.0F;
  const float left_w = kCapWidth;
  const float right_w = kCapWidth;
  const float mid_x = rect.x + left_w;
  const float mid_w = std::max(0.0F, rect.w - left_w - right_w);

  // Left cap.
  DrawPiece(renderer,
            strip->left ? strip->left->get() : nullptr,
            kBackdrop,
            {rect.x, rect.y, left_w, rect.h});
  // Stretched middle tile.
  DrawPiece(renderer,
            strip->middle ? strip->middle->get() : nullptr,
            kBackdrop,
            {mid_x, rect.y, mid_w, rect.h});
  // Right cap.
  DrawPiece(renderer,
            strip->right ? strip->right->get() : nullptr,
            kBackdrop,
            {rect.x + rect.w - right_w, rect.y, right_w, rect.h});
}

std::optional<std::uint8_t>
ServiceButtonAt(const std::vector<ServiceButton> &buttons, SDL_FPoint point) {
  for (const auto &b : buttons) {
    if (point.x >= b.rect.x && point.x <= b.rect.x + b.rect.w &&
        point.y >= b.rect.y && point.y <= b.rect.y + b.rect.h) {
      return b.slot;
    }
  }
  return std::nullopt;
}

} // namespace game
