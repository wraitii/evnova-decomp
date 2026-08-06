#include "services_buttons.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"

#include <optional>
#include <utility>

namespace game {

ServicesButtonArt::~ServicesButtonArt() = default;

namespace {

// The six live edge slices for one state, in (left/right, row) order matching
// how NovaUi_InitThreeStateButtonArt lays out slices 0..8 in a 3x3 grid where
// only the left and right columns carry tiles:
//   left  row0 = 0x1d4c, row1 = 0x1d4f, row2 = 0x1d52
//   right row0 = 0x1d4e, row1 = 0x1d51, row2 = 0x1d54
// The hover/disabled source set begins at 0x1db0 with the same row offsets.
constexpr std::uint16_t kNormalEdgeStart = 0x1d4c;
constexpr std::uint16_t kHoverEdgeStart = 0x1db0;

void LoadEdgeSetImpl(SdlPlatform &platform,
                 std::uint16_t base_id,
                 ServicesButtonArt::EdgeTextures &out,
                 bool &any_loaded) {
  static constexpr std::uint16_t kLeftOffsets[3] = {0x0000, 0x0003, 0x0006};
  static constexpr std::uint16_t kRightOffsets[3] = {0x0002, 0x0005, 0x0008};
  for (int row = 0; row < 3; ++row) {
    for (int edge = 0; edge < 2; ++edge) {
      const std::uint16_t id =
          static_cast<std::uint16_t>(
              base_id + (edge == 0 ? kLeftOffsets[row] : kRightOffsets[row]));
      auto &slot = edge == 0 ? out.left[row] : out.right[row];
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
          NovaLog::Error("button slice 0x{:04x} decoded but upload failed", id);
        } else {
          NovaLog::Todo("button slice PICT 0x{:04x} failed to decode", id);
        }
      }
      // Missing/undecodable slice -> leave slot null; Draw() fills it with the
      // window backdrop so the button still renders, matching the game's
      // solid 0xc x 0x18 fallback rect.
      slot.reset();
    }
  }
}

// Draws one edge segment into the target: if the source slice is present it is
// drawn at its native width across a vertically-stretched band (the middle
// segment stretches between the fixed corners); otherwise the band is filled
// with `fill` (the window backdrop colour).
void DrawEdgeSegment(SDL_Renderer *renderer,
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
  LoadEdgeSetImpl(platform, kNormalEdgeStart, normal_, any);
  LoadEdgeSetImpl(platform, kHoverEdgeStart, hover_, any);
  usable_ = any;
  if (!usable_) {
    NovaLog::Warn("no three-state button slices loaded; buttons will render "
                  "as flat fills");
  }
  return usable_;
}

void ServicesButtonArt::Draw(SdlPlatform &platform,
                             const SDL_FRect &rect,
                             ButtonState state) const {
  SDL_Renderer *renderer = platform.renderer();
  auto &edges = state == ButtonState::kHover ? hover_ : normal_;
  // Backdrop colour used for missing slices / the button body.
  const SDL_Color kBackdrop{1, 4, 12, 255};

  // The three source rows: fixed 25px corners + a vertically-stretched middle.
  constexpr float kCornerHeight = 25.0F;
  const float x0 = rect.x;
  const float x1 = rect.x + 13.0F;
  const float x2 = rect.x + rect.w - 13.0F;
  const float middle_bottom = rect.y + rect.h - kCornerHeight;
  const float y_top = rect.y;
  const float mid_top = rect.y + kCornerHeight;
  const float mid_bottom = rect.y + rect.h - kCornerHeight;

  // Left edge (three segments into the left 13px band).
  const float le = 13.0F;
  DrawEdgeSegment(renderer,
                  edges.left[0] ? edges.left[0]->get() : nullptr,
                  kBackdrop,
                  {x0, y_top, le, kCornerHeight});
  DrawEdgeSegment(renderer,
                  edges.left[1] ? edges.left[1]->get() : nullptr,
                  kBackdrop,
                  {x0, mid_top, le, mid_bottom - mid_top});
  DrawEdgeSegment(renderer,
                  edges.left[2] ? edges.left[2]->get() : nullptr,
                  kBackdrop,
                  {x0, middle_bottom, le, kCornerHeight});

  // Right edge.
  const float re = 13.0F;
  DrawEdgeSegment(renderer,
                  edges.right[0] ? edges.right[0]->get() : nullptr,
                  kBackdrop,
                  {x2, y_top, re, kCornerHeight});
  DrawEdgeSegment(renderer,
                  edges.right[1] ? edges.right[1]->get() : nullptr,
                  kBackdrop,
                  {x2, mid_top, re, mid_bottom - mid_top});
  DrawEdgeSegment(renderer,
                  edges.right[2] ? edges.right[2]->get() : nullptr,
                  kBackdrop,
                  {x2, middle_bottom, re, kCornerHeight});

  // Body: the middle band is the window backdrop so the bevel reads as a solid
  // button.
  SDL_SetRenderDrawColor(renderer, kBackdrop.r, kBackdrop.g, kBackdrop.b,
                         kBackdrop.a);
  const SDL_FRect body{x1, rect.y, x2 - x1, rect.h};
  SDL_RenderFillRect(renderer, &body);
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
