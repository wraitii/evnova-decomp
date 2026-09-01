#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_rect.h>

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
// the view width with the shared 12pt selection-dialog font, the wrapped
// height is the content height, and scrolling offsets the view within that
// content, clamped at both ends.
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

  [[nodiscard]] const SDL_FRect &view_rect() const { return view_rect_; }

  // Clipped wrapped draw at the view rect (NovaTextView's redraw path).
  void Draw(SdlPlatform &platform) const;

private:
  SDL_FRect view_rect_{};
  std::vector<std::string> lines_;
  float content_height_ = 0.0F;
  float max_scroll_ = 0.0F;
  float scroll_offset_ = 0.0F;
  NovaFontCache *fonts_ = nullptr;
};

// The three-state scroll-arrow buttons the window draw callbacks paint over
// the backdrop art (TravelOutfitMenu_FUN_004a1820 / FUN_004a2ac0: entry
// states 0 = enabled, 0xffff = disabled, resolved from the view's maxed/
// scrolled latches). `up` selects the arrow glyph.
void NovaUi_DrawScrollArrow(SdlPlatform &platform,
                            const SDL_FRect &rect,
                            bool up,
                            bool enabled);

// Ghidra 0x004982a0 Ui_RunTravelSelectionDialog (partial port): the game's
// generic scrolling text-reader modal (mission briefings/debriefs, landing
// text, About Nova, ...). Window DLOG 0xbbb with DITL entry 1 = Done button,
// entry 3 = read-only text view, entries 5/6 = scroll arrows; backdrop strip
// PICTs 0x214c/0x214d/0x214e; auto-shrinks to the measured text height (min
// 0x30) when the text is shorter than the view, shifting the controls below
// the text area up. `text` arrives already expanded (callers fill it the way
// the original fills g_selection_dialog_text via Ui_LoadSelectionDialog-
// Resource 0x004c6d50 + the placeholder/wildcard passes).
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
