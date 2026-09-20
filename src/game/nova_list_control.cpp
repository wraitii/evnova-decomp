#include "nova_list_control.hpp"

#include "../sdl_platform.hpp"
#include "../util/color.hpp"
#include "nova_font.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace game {

namespace {

// Shared Geneva-9 metrics of the screen font (DAT_00735684/86, set in
// Ship_InitGameplayDataTables 0x004b0c20). NovaUi_DrawListRowCallback draws
// the label at row top + the scaled font size with a 4px left inset.
constexpr float kListFontSize = 9.0F;
constexpr float kListTextInset = 4.0F;

} // namespace

// Ghidra 0x004d1fb0 NovaList_ScrollByRows (visible = (bottom-top)/pitch).
int NovaListControl::visible_rows() const {
  if (row_pitch_ <= 0.0F) {
    return 0;
  }
  const auto rows = static_cast<int>(content_.h / row_pitch_);
  return std::max(rows, 1);
}

// Ghidra 0x004d1fb0 NovaList_ScrollByRows (count - visible).
int NovaListControl::max_scroll() const {
  return std::max(0, static_cast<int>(row_count_) - visible_rows());
}

void NovaListControl::clamp_scroll() {
  scroll_ = std::clamp(scroll_, 0, max_scroll());
}

// Ghidra 0x004d1fb0 NovaList_ScrollByRows.
void NovaListControl::ScrollRows(int delta) {
  scroll_ += delta;
  clamp_scroll();
}

void NovaListControl::ScrollTo(int row) { scroll_ = row, clamp_scroll(); }

// Ghidra 0x004d1d40 NovaList_ScrollSelectionIntoView.
void NovaListControl::EnsureVisible(std::size_t index) {
  const int row = static_cast<int>(index);
  if (row < scroll_) {
    scroll_ = row;
  } else if (row >= scroll_ + visible_rows()) {
    scroll_ = row - visible_rows() + 1;
  }
  clamp_scroll();
}

// Ghidra 0x004d1f50 NovaList_GetRowRect.
SDL_FRect NovaListControl::RowRect(std::size_t index) const {
  return SDL_FRect{
      content_.x,
      content_.y + (static_cast<float>(index) - static_cast<float>(scroll_)) *
                       row_pitch_,
      content_.w,
      row_pitch_};
}

// Ghidra 0x004d1db0 NovaList_HitTestPoint (content arm).
std::optional<std::size_t> NovaListControl::RowAt(SDL_FPoint point) const {
  if (point.x < content_.x || point.x >= content_.x + content_.w ||
      point.y < content_.y || point.y >= content_.y + content_.h ||
      row_pitch_ <= 0.0F) {
    return std::nullopt;
  }
  const auto row =
      static_cast<std::size_t>((point.y - content_.y) / row_pitch_) +
      static_cast<std::size_t>(scroll_);
  if (row >= row_count_) {
    return std::nullopt;
  }
  return row;
}

// Ghidra 0x004d1db0 NovaList_HitTestPoint (scrollbar arm).
NovaListScrollbarPart NovaListScrollbarHitTest(const NovaListControl &list,
                                               const SDL_FRect &strip,
                                               SDL_FPoint point) {
  if (!list.scrollable() || strip.w <= 0.0F || strip.h <= 0.0F ||
      point.x < strip.x || point.x >= strip.x + strip.w || point.y < strip.y ||
      point.y >= strip.y + strip.h) {
    return NovaListScrollbarPart::kNone;
  }
  const float arrow_h = std::min(strip.w, strip.h / 2.0F);
  if (point.y < strip.y + arrow_h) {
    return NovaListScrollbarPart::kUp;
  }
  if (point.y >= strip.y + strip.h - arrow_h) {
    return NovaListScrollbarPart::kDown;
  }
  // Thumb geometry mirrors NovaUi_DrawListScrollbar: the track runs between
  // the two arrow caps and the thumb holds max(arrow height, proportional)
  // pixels, positioned by scroll / max_scroll.
  const float track_top = strip.y + arrow_h;
  const float track_h = std::max(1.0F, strip.h - 2.0F * arrow_h);
  const float count = static_cast<float>(list.row_count());
  const float visible = static_cast<float>(list.visible_rows());
  const float thumb_h =
      std::clamp(track_h * visible / std::max(1.0F, count), arrow_h, track_h);
  const float thumb_top =
      track_top + (track_h - thumb_h) * static_cast<float>(list.scroll_rows()) /
                      static_cast<float>(std::max(1, list.max_scroll()));
  if (point.y >= thumb_top && point.y < thumb_top + thumb_h) {
    return NovaListScrollbarPart::kThumb;
  }
  return point.y < thumb_top ? NovaListScrollbarPart::kPageUp
                             : NovaListScrollbarPart::kPageDown;
}

void NovaUi_DrawListRow(SdlPlatform &platform,
                        NovaFontCache &font_cache,
                        const SDL_FRect &row,
                        std::string_view text,
                        bool selected,
                        const NovaRgbColor &text_color,
                        const NovaRgbColor &background,
                        const NovaRgbColor &hilite) {
  SDL_Renderer *renderer = platform.renderer();
  const SDL_Color fill =
      evnova::util::ToSdlColor(selected ? hilite : background);
  SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
  SDL_RenderFillRect(renderer, &row);
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                kListFontSize,
                kNovaFontStyleRegular,
                evnova::util::ToSdlColor(text_color),
                row.x + kListTextInset,
                row.y + kListFontSize,
                text);
}

// Ghidra 0x004d2010 NovaList_Draw.
void NovaUi_DrawListScrollbar(SdlPlatform &platform,
                              const NovaListControl &list,
                              const SDL_FRect &strip) {
  if (!list.scrollable() || strip.w <= 0.0F || strip.h <= 0.0F) {
    return;
  }
  SDL_Renderer *renderer = platform.renderer();
  const float arrow_h = std::min(strip.w, strip.h / 2.0F);
  // Trough + frame, matching the native border colours (DAT_00575adc frame on
  // the 0x808080/PTr_DAT_00575acc surface). The port uses flat greys.
  const SDL_Color frame{0xc0, 0xc0, 0xc0, SDL_ALPHA_OPAQUE};
  const SDL_Color trough{0x30, 0x30, 0x30, SDL_ALPHA_OPAQUE};
  const SDL_Color thumb{0x80, 0x80, 0x80, SDL_ALPHA_OPAQUE};
  SDL_SetRenderDrawColor(renderer, trough.r, trough.g, trough.b, trough.a);
  SDL_RenderFillRect(renderer, &strip);
  SDL_SetRenderDrawColor(renderer, frame.r, frame.g, frame.b, frame.a);
  SDL_RenderRect(renderer, &strip);

  // Proportional thumb (NovaList_Draw positions it by scroll / usable track).
  const float track_top = strip.y + arrow_h;
  const float track_h = std::max(1.0F, strip.h - 2.0F * arrow_h);
  const float count = static_cast<float>(list.row_count());
  const float visible = static_cast<float>(list.visible_rows());
  const float thumb_h =
      std::clamp(track_h * visible / std::max(1.0F, count), arrow_h, track_h);
  const float thumb_top =
      track_top + (track_h - thumb_h) * static_cast<float>(list.scroll_rows()) /
                      static_cast<float>(std::max(1, list.max_scroll()));
  const SDL_FRect thumb_rect{
      strip.x + 1.0F, thumb_top, strip.w - 2.0F, thumb_h};
  SDL_SetRenderDrawColor(renderer, thumb.r, thumb.g, thumb.b, thumb.a);
  SDL_RenderFillRect(renderer, &thumb_rect);
  SDL_SetRenderDrawColor(renderer, frame.r, frame.g, frame.b, frame.a);
  SDL_RenderRect(renderer, &thumb_rect);

  // Solid up/down chevrons, sized to the arrow cap. The native painter writes
  // them with a 2x2 pen (FUN_004ba320(2,2)); SDL approximates with two-pixel
  // scanline steps.
  const auto triangle = [&](bool up) {
    const float cy =
        up ? strip.y + arrow_h / 2.0F : strip.y + strip.h - arrow_h / 2.0F;
    const float half = std::max(2.0F, arrow_h / 4.0F);
    const float cx = strip.x + strip.w / 2.0F;
    SDL_SetRenderDrawColor(renderer, frame.r, frame.g, frame.b, frame.a);
    for (int step = 0; step < static_cast<int>(half); ++step) {
      const float y = up ? cy - half + static_cast<float>(step)
                         : cy + half - static_cast<float>(step);
      const float w = static_cast<float>(step + 1);
      const SDL_FRect line{cx - w, y, 2.0F * w, 1.0F};
      SDL_RenderFillRect(renderer, &line);
    }
  };
  triangle(true);
  triangle(false);
}

} // namespace game
