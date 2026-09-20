#pragma once

// Clean-room counterpart of the game's reusable Mac list control used by the
// mission BBS and mission computer (and by the other native lists), modelled
// from its helper family:
//   NovaList_Create  create (rect, row pitch, row count, draw callback,
//   scrollbar) NovaList_GetRowRect  row rect (list.top + (row - scroll) *
//   pitch) NovaList_HitTestPoint  point -> row (accounting for the scroll
//   offset) NovaList_ScrollByRows  scroll by rows, clamped to [0, count -
//   visible] NovaList_ScrollSelectionIntoView  scroll the selected row into
//   view NovaList_SetSelection  set the selected row (redraws) NovaList_Draw
//   redraw rows + the native vertical scrollbar
//
// The original stores the scroll offset in ROWS (+0x24) and derives the
// visible row count as floor(list_height / row_pitch); this mirrors that so a
// window only has to keep the selection index and hand it to EnsureVisible.
// Scrollbars are drawn into a caller-supplied 15px strip because the dialogs
// differ: the BBS owns a separate DITL scrollbar entry (DLOG 0x3ee, UiPanel
// entry 3), while the mission computer's rebuild narrows the list by 0xf and
// paints the strip along the list's right edge.

#include <cstddef>
#include <optional>
#include <string_view>

#include <SDL3/SDL_rect.h>

#include "../util/color.hpp"

class SdlPlatform;

namespace game {

class NovaFontCache;

// Scroll/geometry model for one native list. Selection is owned by the caller
// (the dialogs already track it) so this stays reusable and testable.
class NovaListControl {
public:
  NovaListControl() = default;

  // Ghidra 0x004d1a60 NovaList_Create.
  NovaListControl(const SDL_FRect &content,
                  float row_pitch,
                  std::size_t row_count)
      : content_(content), row_pitch_(row_pitch), row_count_(row_count) {}

  void SetContentRect(const SDL_FRect &content) { content_ = content; }

  // NovaList_Create/list sizing: the rebuild resizes the control when the row
  // set changes, so the scroll offset is re-clamped against the new extent.
  void SetRowCount(std::size_t row_count) {
    row_count_ = row_count;
    clamp_scroll();
  }

  void SetRowPitch(float row_pitch) {
    row_pitch_ = row_pitch;
    clamp_scroll();
  }

  [[nodiscard]] const SDL_FRect &content_rect() const { return content_; }

  [[nodiscard]] float row_pitch() const { return row_pitch_; }

  [[nodiscard]] std::size_t row_count() const { return row_count_; }

  // floor(content height / pitch), never below one row: the original divides
  // the integer heights and only guards the >visible case for the scrollbar.
  [[nodiscard]] int visible_rows() const;
  [[nodiscard]] int max_scroll() const;

  [[nodiscard]] int scroll_rows() const { return scroll_; }

  [[nodiscard]] bool scrollable() const { return max_scroll() > 0; }

  // NovaList_ScrollByRows: scroll by whole rows with the original clamp.
  void ScrollRows(int delta);
  void ScrollTo(int row);
  // NovaList_ScrollSelectionIntoView: move the scroll offset just enough to
  // reveal `index`.
  void EnsureVisible(std::size_t index);

  // NovaList_GetRowRect: window-local rect of `index`, offset by the scroll
  // position. Rows scrolled out of view return a rect outside content_ (callers
  // clip).
  [[nodiscard]] SDL_FRect RowRect(std::size_t index) const;
  // NovaList_HitTestPoint (content arm): row under a point, accounting for the
  // scroll offset; nullopt outside the content band or past the last row.
  [[nodiscard]] std::optional<std::size_t> RowAt(SDL_FPoint point) const;

private:
  void clamp_scroll();

  SDL_FRect content_{};
  float row_pitch_ = 12.0F;
  std::size_t row_count_ = 0;
  int scroll_ = 0;
};

// NovaList_HitTestPoint (scrollbar arm) hit classification for a click in the
// strip. kThumb is a click landing on the thumb body itself; the original
// leaves that case unhandled (no drag tracking), so callers treat it as a
// no-op alongside kNone.
enum class NovaListScrollbarPart {
  kNone,
  kUp,
  kDown,
  kPageUp,
  kPageDown,
  kThumb,
};

// `strip` is the 15px vertical scrollbar band. When the list fits (no
// scrollbar) the result is always kNone.
[[nodiscard]] NovaListScrollbarPart NovaListScrollbarHitTest(
    const NovaListControl &list, const SDL_FRect &strip, SDL_FPoint point);

// Ghidra 0x00448a30 NovaUi_DrawListRowCallback: fills the row from the c.lr
// list palette (background / hilite when selected) and draws the label with
// the shared Geneva-9 screen font (DAT_00735684/86), text inset 4px and the
// baseline at row top + 9. Extracted so the BBS and mission computer share the
// exact row painting.
void NovaUi_DrawListRow(SdlPlatform &platform,
                        NovaFontCache &font_cache,
                        const SDL_FRect &row,
                        std::string_view text,
                        bool selected,
                        const NovaRgbColor &text_color,
                        const NovaRgbColor &background,
                        const NovaRgbColor &hilite);

// Approximation of the native vertical scrollbar (NovaList_Draw): 15px trough
// with a frame, up/down arrow chevrons and a proportional thumb. `strip` is
// the band returned by the list layout.
void NovaUi_DrawListScrollbar(SdlPlatform &platform,
                              const NovaListControl &list,
                              const SDL_FRect &strip);

} // namespace game
