#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_rect.h>

#include "services_buttons.hpp"

class SdlPlatform;
class SdlTexture;
struct SDL_Texture;

namespace game {

struct GameState;
class NovaFontCache;

// Clean-room counterpart of the game's shared read-only scrolling text view:
// NovaTextView_Create (0x004bcd90), NovaTextView_UpdateContentHeight
// (0x004bce10) and NovaTextView_ScrollBy (0x004bce90). Every selection-dialog
// window hosts one of these over its DITL text-view entry (the mission-offer
// window's entry 3, the generic reader's entry 3, ...): the text is wrapped at
// the view width with the shared Geneva-9 screen font (DAT_00735684/86), the
// wrapped height is the content height, and scrolling offsets the view within
// that content, clamped at both ends.
class NovaTextScrollView {
public:
  NovaTextScrollView(NovaFontCache &fonts,
                     std::string_view text,
                     const SDL_FRect &view_rect);

  // NovaUi_ScrollSelectionText steps +/-10 px per action; the clamp mirrors
  // the original's maxed/scrolled latch gating.
  void ScrollBy(float delta);

  [[nodiscard]] float max_scroll() const { return max_scroll_; }

  [[nodiscard]] float scroll_offset() const { return scroll_offset_; }

  [[nodiscard]] float content_height() const { return content_height_; }

  // Pure wrapped text height without the view's leading/trailing insets —
  // the quantity NovaText_MeasureWrappedTextHeight (0x004bcc70, DrawText
  // DT_CALCRECT) reports and the reader's auto-size arm consumes.
  [[nodiscard]] float text_height() const { return text_height_; }

  [[nodiscard]] const SDL_FRect &view_rect() const { return view_rect_; }

  // Auto-size support (0x004982a0): after shrinking the window, the original
  // republishes the new entry-3 rect into the text view (editor +0xc/+0x14)
  // and re-measures. The wrap width is unchanged, so only the clip/fill rect
  // and the scroll clamp move.
  void SetViewRect(const SDL_FRect &rect) {
    view_rect_ = rect;
    max_scroll_ = std::max(0.0F, content_height_ - view_rect_.h);
    scroll_offset_ = std::clamp(scroll_offset_, 0.0F, max_scroll_);
  }

  // Clipped wrapped draw at the view rect (NovaTextView's redraw path).
  void Draw(SdlPlatform &platform) const;

private:
  SDL_FRect view_rect_{};
  std::vector<std::string> lines_;
  float content_height_ = 0.0F;
  float text_height_ = 0.0F;
  float max_scroll_ = 0.0F;
  float scroll_offset_ = 0.0F;
  NovaFontCache *fonts_ = nullptr;
};

// Ghidra 0x004a3340 NovaUi_DrawThreeStateButton via the selection-dialog
// painters (0x004a2ac0/0x004a1820): the scroll arrows are three-state button
// bodies whose glyphs are vector chevrons keyed off the label's second byte
// ('^' up / '&' down, STR# 0x96 slots 0x12/0x13) -- not text. `up` selects
// the chevron shape; `enabled` selects the normal/grey strip and the
// white/dark-grey glyph (câlr record bytes ffffff / 262626). Chevron
// geometry is integer: s = width/10, apex at the rounded midpoint, stroked
// with the original's 2x2 pen.
void NovaUi_DrawScrollArrow(SdlPlatform &platform,
                            const ServicesButtonArt &button_art,
                            const SDL_FRect &rect,
                            bool up,
                            bool enabled);

// Ghidra 0x004982a0 Ui_RunTravelSelectionDialog (partial port): the game's
// generic scrolling text-reader modal (mission briefings/debriefs, landing
// text, About Nova, ...). Window DLOG 0xbbb with DITL entry 1 = Okay button
// (STR# 0x96 slot 0x1a caption over a three-state body), entry 3 = read-only
// text view, entries 5/6 = scroll arrows (three-state bodies with vector
// ^/& chevrons); backdrop PICT strips 0x214c (top) + 0x214d (body) + 0x214e
// (bottom) over a black fill; auto-shrinks
// to the measured text height (min 0x30) when the text is shorter than the
// view, shifting the controls below the text area up. `text` arrives already
// expanded (callers fill it the way the original fills g_selection_dialog_text
// via Ui_LoadSelectionDialog- Resource 0x004c6d50 + the placeholder/wildcard
// passes).
//
// Modal actions: Done closes (the original's action 1); 5/6 scroll the view
// +/-10 px; action 4 opens the starmap when `allow_starmap` is set (the port
// keys it to 'm' in place of the original's key binding); Esc/Enter also
// close (port divergence, the original exits only via Done).
// `render_background` re-renders the preserved underlying screen (docked
// menu, flight view, ...) each frame so the modal just layers its window on
// top; without it the screen is cleared to black.
// TODO(decomp) skipped: the variant >= 0x80 DLOG 0xbbc + PICT 0x214f art
// path, the desc status-string display, and g_selection_dialog_over_static-
// _surface redraw variants.
void NovaUi_RunTextReaderDialog(
    SdlPlatform &platform,
    GameState &state,
    const std::string &text,
    bool allow_starmap,
    const std::function<void()> &render_background = {});

} // namespace game
