#pragma once

#include <algorithm>
#include <cstdint>
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

// Port-normalized physical key codes the scroll actions consume (see
// sdl_platform OriginalKeyCode): Home 0x60, Up 0x61, PageUp 0x62, End 0x65,
// Down 0x66, PageDown 0x67. The original EV Nova codes are Home 0x91, Up 0x0b,
// PageUp 0x92, End 0x93, Down 0x0a, PageDown 0x94.
enum class TextScrollKey : std::uint8_t {
  kNone,
  kLineUp,
  kLineDown,
  kHome,
  kPageUp,
  kEnd,
  kPageDown,
};

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

  // Can the view still scroll in either direction? These are the original's
  // g_selection_editor_maxed (can scroll up) / g_selection_editor_active (can
  // scroll down) latches, which NovaUi_ScrollSelectionText (0x00499270)
  // recomputes from the view/content rects.
  [[nodiscard]] bool can_scroll_up() const { return scroll_offset_ > 0.0F; }

  [[nodiscard]] bool can_scroll_down() const {
    return scroll_offset_ < max_scroll_;
  }

  // Applies one original scroll action (Ghidra 0x00499440 reader callback /
  // 0x00447170 offer poll, both through NovaUi_ScrollSelectionText): line
  // actions move +/-10px, Home/End jump to an end, PageUp/PageDown move
  // 0xfa (250)px. Every action is gated on the maxed/active latch it
  // consumes. Returns true when the offset changed.
  bool ApplyScrollKey(TextScrollKey key);

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
    UpdateMaxScroll();
    scroll_offset_ = std::clamp(scroll_offset_, 0.0F, max_scroll_);
  }

  // Clipped wrapped draw at the view rect (NovaTextView's redraw path).
  void Draw(SdlPlatform &platform) const;

private:
  // Clamped scroll extent for the current view height and line metrics.
  void UpdateMaxScroll();

  SDL_FRect view_rect_{};
  std::vector<std::string> lines_;
  float content_height_ = 0.0F;
  float text_height_ = 0.0F;
  float max_scroll_ = 0.0F;
  float scroll_offset_ = 0.0F;
  // Baseline distance the draw model adds below the wrapped text height (the
  // first-line offset minus the font ascent); see UpdateMaxScroll.
  float bottom_correction_ = 0.0F;
  NovaFontCache *fonts_ = nullptr;
};

[[nodiscard]] TextScrollKey MapTextScrollKey(std::uint16_t key_code);

// Hold-to-repeat state for the text-view arrow buttons (Ghidra 0x00447170 and
// 0x00499440): a press that lands on an arrow enters a loop that scrolls 1px
// per 60Hz tick while the button stays down, gated on the maxed/active
// latches. A tap shorter than a tick still emits the first 1px step, which
// keeps /probe/click usable because injected clicks never report as held.
class NovaTextScrollHold {
public:
  // Primary press on the up (true) or down (false) arrow; emits the first
  // step immediately.
  void Press(NovaTextScrollView &view, bool up, std::uint64_t now_ms);

  // Once per frame with the live button state; emits one step per elapsed
  // 60Hz tick while held. Returns true when the offset changed.
  bool Update(NovaTextScrollView &view, bool button_down, std::uint64_t now_ms);

  [[nodiscard]] bool held() const { return held_; }

  [[nodiscard]] bool up() const { return up_; }

private:
  bool held_ = false;
  bool up_ = false;
  std::uint64_t last_tick_ = 0;
};

// Ghidra 0x004a3340 NovaUi_DrawThreeStateButton via the selection-dialog
// painters (0x004a2ac0/0x004a1820): the scroll arrows are three-state button
// bodies whose glyphs are vector chevrons keyed off the label's second byte
// ('^' up / '&' down, STR# 0x96 slots 0x12/0x13) -- not text. `up` selects
// the chevron shape; `enabled` selects the normal/grey strip and the
// white/dark-grey glyph (câlr record bytes ffffff / 262626). The original
// draws the glyph into the button-strip surface at the local integer centre
// (floor(w/2), floor(h/2)) with s = floor(h/10), then blits it to the rect.
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
// +/-10 px; action 4 opens the starmap when `allow_starmap` is set, polled
// from the rebindable map command (slot 9, default M) the original's callback
// (0x00499440) tests. Esc/Enter also
// close (port divergence, the original exits only via Done).
// `render_background` re-renders the preserved underlying screen (docked
// menu, flight view, ...) each frame so the modal just layers its window on
// top; without it the screen is cleared to black.
//
// `dialog_variant` is the dësc's trailing variant field. When it is a PICT id
// >= 0x80 AND the original's caller passed show_art_and_status, the reader
// runs the custom-art arm: DLOG 0xbbc with the single backdrop PICT 0x214f
// and the variant PICT blitted into DITL entry 2 (Ghidra 0x004982a0 /
// NovaUi_DrawSelectionDialogContent 0x00499870). A variant < 0x80 keeps the
// 0xbbb strip. Callers that only have a bare string pass 0. The auto-size arm
// is disabled in the art arm, as in the original.
// TODO(decomp) skipped: the desc status-string movie (Ui_PlayMovieFileModal
// 0x0049db00, recorded as a `qt` platform skip in decomp-skipped.tsv) and
// g_selection_dialog_over_static_surface redraw variants.
//
// `mission_dialog` selects the requested presentation scale: mission desc
// readers (Brief/QuickBrief/LoadCarg/DumpCargo/Comp/Fail/ShipDone and the
// mission-cargo denial texts) request `mission_dialog_scale()` (`U * M`);
// every other reader (about, intro, store/bar notices, escort payroll) uses
// `ui_scale()` (`U`). The DLOG is shared, so the flag is the only difference.
void NovaUi_RunTextReaderDialog(
    SdlPlatform &platform,
    GameState &state,
    const std::string &text,
    bool allow_starmap,
    const std::function<void()> &render_background = {},
    std::int16_t dialog_variant = 0,
    bool mission_dialog = false);

} // namespace game
