#include "services_buttons.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"

#include <algorithm>

#include <optional>
#include <utility>

namespace game {

ServicesButtonArt::~ServicesButtonArt() = default;

namespace {

// The three button strips, in the same order NovaUi_InitThreeStateButtonArt
// (0x004a2f50) loads them: nine consecutive PICTs from 0x1d4c (normal left) to
// 0x1d54 (grey right), i.e. 0x1d4c + state*3 + piece:
//   normal  left/middle/right = 0x1d4c, 0x1d4d, 0x1d4e  ("Button ... Bright")
//   pressed left/middle/right = 0x1d4f, 0x1d50, 0x1d51  ("click ...")
//   grey    left/middle/right = 0x1d52, 0x1d53, 0x1d54  ("grey ...")
constexpr std::uint16_t kStripBase = 0x1d4c;
constexpr std::uint16_t kPiecesPerStrip = 3; // left, middle, right

// Loads one strip's three pieces (left cap, stretchable middle tile, right
// cap) from Nova Graphics 3 PICTs `base`, ... `base+2`.
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
        slot = SdlTexture::Create(platform.renderer(),
                                  pict->width,
                                  pict->height,
                                  pict->rgba_pixels);
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

  // The left/right caps are fixed 13px wide; the 2px middle tile stretches
  // across the body between them. For a button narrower than two caps
  // (2*13 = 26px) clamp so it still draws without a negative middle band.
  constexpr float kCapWidth = 13.0F;
  const float left_w = std::min(kCapWidth, rect.w / 2.0F);
  const float right_w = std::min(kCapWidth, rect.w - left_w);
  const float mid_x = rect.x + left_w;
  const float mid_w = std::max(0.0F, rect.w - left_w - right_w);

  // Left cap.
  DrawPiece(renderer, strip->left ? strip->left->get() : nullptr, kBackdrop,
            {rect.x, rect.y, left_w, rect.h});
  // Stretched middle tile.
  DrawPiece(renderer, strip->middle ? strip->middle->get() : nullptr, kBackdrop,
            {mid_x, rect.y, mid_w, rect.h});
  // Right cap.
  DrawPiece(renderer, strip->right ? strip->right->get() : nullptr, kBackdrop,
            {rect.x + rect.w - right_w, rect.y, right_w, rect.h});
}

std::optional<std::uint8_t> ServiceButtonAt(
    const std::vector<ServiceButton> &buttons,
    SDL_FPoint point) {
  for (const auto &b : buttons) {
    if (point.x >= b.rect.x && point.x <= b.rect.x + b.rect.w &&
        point.y >= b.rect.y && point.y <= b.rect.y + b.rect.h) {
      return b.slot;
    }
  }
  return std::nullopt;
}

} // namespace game
