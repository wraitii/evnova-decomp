#include "selection_text_dialog.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "../util/geometry.hpp"
#include "command_input.hpp"
#include "game_state.hpp"
#include "hud_overlay.hpp"
#include "landed_window.hpp"
#include "nova_font.hpp"
#include "services_buttons.hpp"
#include "starmap.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace game {

using evnova::util::Contains;

namespace {

constexpr std::uint16_t kReaderDialogId = 0xbbb;

// Command slot 9: the one rebindable galaxy-map command (default M), the same
// slot the flight loop reads. The original reader callback (0x00499440) raises
// action 4 from it.
constexpr std::size_t kStarmapCommand = 0x09;

// Shared selection-dialog text metrics: the reader's text view uses the
// shared Geneva-9 screen font (DAT_00735684/86, set in 0x004b0c20) with the
// 11pt leading the landed-description/about texts use.
constexpr float kTextSize = 9.0F;
constexpr float kLineHeight = 11.0F;
constexpr float kFirstLineOffset = 11.0F;

} // namespace

NovaTextScrollView::NovaTextScrollView(NovaFontCache &fonts,
                                       std::string_view text,
                                       const SDL_FRect &view_rect)
    : view_rect_(view_rect), fonts_(&fonts) {
  // 0x004bc760 wraps against the supplied rectangle's left/right edges;
  // the DITL text rectangle already supplies the dialog's outer margins.
  const float text_w = std::max(1.0F, view_rect.w);
  lines_ = WrapDescriptionLines(
      text, static_cast<int>(text_w), [&](std::string_view line) {
        return fonts.TextWidth(
            NovaFontFamily::kGeneva, kTextSize, kNovaFontStyleRegular, line);
      });
  const float wrapped_height = static_cast<float>(lines_.size()) * kLineHeight;
  text_height_ = wrapped_height;
  content_height_ = kFirstLineOffset + wrapped_height + 6.0F;
  // NovaTextView_ScrollBy (0x004bce90) clamps the scroll against the content
  // height reported by NovaTextView_UpdateContentHeight (0x004bce10) =
  // NovaText_MeasureWrappedTextHeight, i.e. the wrapped text height only.
  // The first-line/bottom insets in content_height_ are draw-only and must not
  // inflate the scroll extent, or a fitted view still claims ~17px of scroll
  // and the arrow buttons light up with nothing to scroll to.
  max_scroll_ = std::max(0.0F, text_height_ - view_rect.h);
}

// Ghidra 0x00499270 NovaUi_ScrollSelectionText, immediate arm. Its
// param_5 == 0 "smooth" arm would scale param_4 as px/sec by the elapsed 60Hz
// ticks (x the 1/60 double at 0x005759b0); TODO(decomp(0x00499270)) skipped:
// it is dead in the shipped binary -- every call site in the mission-offer
// poll/run and the text-reader run/callback passes the immediate flag -- so
// the port models only the +/-px step.
void NovaTextScrollView::ScrollBy(float delta) {
  scroll_offset_ = std::clamp(scroll_offset_ + delta, 0.0F, max_scroll_);
}

TextScrollKey MapTextScrollKey(std::uint16_t key_code) {
  switch (key_code) {
  case 0x61: // normalized Up
    return TextScrollKey::kLineUp;
  case 0x66: // normalized Down
    return TextScrollKey::kLineDown;
  case 0x60: // normalized Home
    return TextScrollKey::kHome;
  case 0x62: // normalized PageUp
    return TextScrollKey::kPageUp;
  case 0x65: // normalized End
    return TextScrollKey::kEnd;
  case 0x67: // normalized PageDown
    return TextScrollKey::kPageDown;
  default:
    return TextScrollKey::kNone;
  }
}

// Ghidra 0x00499440 / 0x00447170 key arms, via NovaUi_ScrollSelectionText
// (0x00499270): the line and page steps go through the +/-px path, Home/End
// ask for an oversized step that the view clamp turns into a jump to the end.
// The original clears the exhausted latch explicitly; the live can_*()
// predicate makes that automatic.
bool NovaTextScrollView::ApplyScrollKey(TextScrollKey key) {
  const float before = scroll_offset_;
  switch (key) {
  case TextScrollKey::kLineUp:
    if (can_scroll_up()) {
      ScrollBy(-10.0F);
    }
    break;
  case TextScrollKey::kLineDown:
    if (can_scroll_down()) {
      ScrollBy(10.0F);
    }
    break;
  case TextScrollKey::kHome:
    if (can_scroll_up()) {
      scroll_offset_ = 0.0F;
    }
    break;
  case TextScrollKey::kPageUp:
    if (can_scroll_up()) {
      ScrollBy(-250.0F);
    }
    break;
  case TextScrollKey::kEnd:
    if (can_scroll_down()) {
      scroll_offset_ = max_scroll_;
    }
    break;
  case TextScrollKey::kPageDown:
    if (can_scroll_down()) {
      ScrollBy(250.0F);
    }
    break;
  case TextScrollKey::kNone:
    break;
  }
  return scroll_offset_ != before;
}

void NovaTextScrollHold::Press(NovaTextScrollView &view,
                               bool up,
                               std::uint64_t now_ms) {
  held_ = true;
  up_ = up;
  last_tick_ = (now_ms * 60U) / 1000U;
  // The original's hold loop emits one step before it starts waiting on the
  // 60Hz tick, so a tap shorter than a tick still moves one pixel.
  if (up_ ? view.can_scroll_up() : view.can_scroll_down()) {
    view.ScrollBy(up_ ? -1.0F : 1.0F);
  }
}

bool NovaTextScrollHold::Update(NovaTextScrollView &view,
                                bool button_down,
                                std::uint64_t now_ms) {
  if (!held_) {
    return false;
  }
  if (!button_down) {
    held_ = false;
    return false;
  }
  const std::uint64_t tick = (now_ms * 60U) / 1000U;
  if (tick <= last_tick_) {
    return false;
  }
  const float before = view.scroll_offset();
  bool more = true;
  for (std::uint64_t step = last_tick_; step < tick && more; ++step) {
    if (up_ ? view.can_scroll_up() : view.can_scroll_down()) {
      view.ScrollBy(up_ ? -1.0F : 1.0F);
    } else {
      // Ran into the end of the content; stop repeating.
      held_ = false;
      more = false;
    }
  }
  last_tick_ = tick;
  return view.scroll_offset() != before;
}

// Ghidra 0x004bcf30 NovaTextView redraw path: the wrapped text is clipped to
// the view rect and offset by the scroll position.
void NovaTextScrollView::Draw(SdlPlatform &platform) const {
  SDL_Renderer *renderer = platform.renderer();
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &view_rect_);
  const SDL_Rect clip{static_cast<int>(view_rect_.x),
                      static_cast<int>(view_rect_.y),
                      static_cast<int>(view_rect_.w),
                      static_cast<int>(view_rect_.h)};
  SDL_SetRenderClipRect(renderer, &clip);
  float baseline = view_rect_.y + kFirstLineOffset - scroll_offset_;
  for (const auto &line : lines_) {
    if (baseline > view_rect_.y + view_rect_.h) {
      break;
    }
    if (baseline >= view_rect_.y) {
      NovaText_Draw(platform,
                    *fonts_,
                    NovaFontFamily::kGeneva,
                    kTextSize,
                    kNovaFontStyleRegular,
                    SDL_Color{255, 255, 255, 255},
                    view_rect_.x,
                    baseline,
                    line);
    }
    baseline += kLineHeight;
  }
  SDL_SetRenderClipRect(renderer, nullptr);
}

void NovaUi_DrawScrollArrow(SdlPlatform &platform,
                            const ServicesButtonArt &button_art,
                            const SDL_FRect &rect,
                            bool up,
                            bool enabled) {
  button_art.Draw(
      platform, rect, enabled ? ButtonState::kNormal : ButtonState::kDisabled);
  DrawThreeStateButtonArrow(
      platform.renderer(),
      rect,
      up,
      enabled ? SDL_Color{255, 255, 255, SDL_ALPHA_OPAQUE}
              : SDL_Color{0x26, 0x26, 0x26, SDL_ALPHA_OPAQUE});
}

namespace {

struct ReaderLayout {
  SDL_FRect window{};
  SDL_FRect done_button{}; // UiPanel entry 1
  SDL_FRect text_area{};   // UiPanel entry 3
  SDL_FRect arrow_up{};    // UiPanel entry 5
  SDL_FRect arrow_down{};  // UiPanel entry 6
  std::string done_caption = "Okay";
};

// Window bounds + control rects from the real DLOG/DITL in authored local
// coordinates. The platform contains this composition and maps input back to
// the same local space.
[[nodiscard]] ReaderLayout LoadReaderLayout() {
  ReaderLayout layout;
  const auto definition = NovaResource_LoadDialogDefinition(kReaderDialogId);
  const auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (!definition || !items) {
    NovaLog::Todo("text-reader DLOG/DITL 0xbbb unavailable; refusing a "
                  "synthetic layout");
    return layout;
  }
  const float win_w = static_cast<float>(definition->right - definition->left);
  const float win_h = static_cast<float>(definition->bottom - definition->top);
  const SDL_FPoint origin{0.0F, 0.0F};
  layout.window = {0.0F, 0.0F, win_w, win_h};
  const auto rect = [&](std::size_t index) {
    for (const auto &item : *items) {
      if (item.index == index) {
        return SDL_FRect{origin.x + static_cast<float>(item.left),
                         origin.y + static_cast<float>(item.top),
                         static_cast<float>(item.right - item.left),
                         static_cast<float>(item.bottom - item.top)};
      }
    }
    return SDL_FRect{};
  };
  layout.done_button = rect(0); // UiPanel entry 1
  layout.text_area = rect(2);   // UiPanel entry 3
  layout.arrow_up = rect(4);    // UiPanel entry 5
  layout.arrow_down = rect(5);  // UiPanel entry 6
  // The button caption is not the DITL title (entry 1 is a title-less
  // userItem): the 0x004a2ac0 painter labels it from STR# 0x96 slot 0x1a
  // (1-based entry 0x1b, "Okay"), like every selection dialog.
  if (const auto caption = NovaHud_LoadStringEntry(0x96, 0x1b)) {
    layout.done_caption = *caption;
  }
  return layout;
}

// Backdrop for the < 0x80 variant: three PICTs composed by
// NovaUi_DrawSelectionDialogContent 0x00499870 in order body -> top ->
// bottom: 0x214d (441x365) at window.y + height(0x214c), 0x214c (441x9) at
// the window top, 0x214e (441x40) anchored to the window bottom (it wins
// where the over-tall body overlaps). DAT_007d4bf0/4bf4 are overlapping
// aliases of the image-slot array's entries 1/2 and stay set for the reader
// -- the >= 0x80 art variant is the only path that zeroes them. Blits clip
// to the window rect (the painter draws inside the modal window's context).
// Loaded once per dialog, not per frame.
struct ReaderBackdrop {
  std::unique_ptr<SdlTexture> top;    // 0x214c
  std::unique_ptr<SdlTexture> body;   // 0x214d
  std::unique_ptr<SdlTexture> bottom; // 0x214e
};

[[nodiscard]] std::unique_ptr<SdlTexture>
LoadBackdropPict(SdlPlatform &platform, std::uint16_t pict_id) {
  const auto data = NovaResource_LoadPictData(pict_id);
  const auto image = data ? Resource_LoadPictAsImage(*data) : std::nullopt;
  if (!data || !image) {
    return nullptr;
  }
  return SdlTexture::Create(
      platform.renderer(), image->width, image->height, image->rgba_pixels);
}

[[nodiscard]] ReaderBackdrop LoadReaderBackdrop(SdlPlatform &platform) {
  ReaderBackdrop backdrop;
  backdrop.top = LoadBackdropPict(platform, 0x214c);
  backdrop.body = LoadBackdropPict(platform, 0x214d);
  backdrop.bottom = LoadBackdropPict(platform, 0x214e);
  if (!backdrop.top || !backdrop.body || !backdrop.bottom) {
    NovaLog::Todo("text-reader backdrop PICT 0x214c/0x214d/0x214e missing; "
                  "window stays a flat fill");
  }
  return backdrop;
}

} // namespace

void NovaUi_RunTextReaderDialog(
    SdlPlatform &platform,
    GameState &state,
    const std::string &text,
    bool allow_starmap,
    const std::function<void()> &render_background) {
  const SdlPlatform::ScopedPlacement restore_placement(
      platform, platform.current_placement());
  ReaderLayout layout = LoadReaderLayout();
  if (layout.window.w <= 0.0F) {
    return;
  }
  // Keep the original DLOG canvas: the auto-size arm moves its shortened
  // window down by 0.3 * shrink, rather than re-centring by half the shrink.
  const SDL_FPoint authored_size{layout.window.w, layout.window.h};

  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  // Ui_RunTravelSelectionDialog appends "\r" to the selection text before
  // building the view (a no-op for empty text, which shows no dialog); the
  // wrapper treats it as a plain line break.
  const std::string shown_text = text.empty() ? text : text + "\r";
  NovaTextScrollView view(font_cache, shown_text, layout.text_area);

  // Auto-size arm (0x004982a0): when the measured wrapped text height is
  // shorter than the view the window shrinks. The measurement (pure text
  // height, no insets, NovaText_MeasureWrappedTextHeight 0x004bcc70) is
  // clamped to a 0x30 minimum, shrink = view height - measured - 0x10;
  // entries 1/5/6 shift up by the shrink and entry 3's bottom rises. The
  // window then resizes and drops by round(shrink * 0.3) (DAT_005759a8,
  // 0.3f) to re-balance it. The original does this in window-local
  // coordinates, so the drop carries the child items with it; the port's
  // rects are absolute, so every rect shifts down by the same amount.
  if (view.text_height() < layout.text_area.h) {
    const float measured = std::max(view.text_height(), 48.0F);
    const float shrink = layout.text_area.h - measured - 16.0F;
    const auto shift_up = [&](SDL_FRect &rect) { rect.y -= shrink; };
    shift_up(layout.done_button);
    shift_up(layout.arrow_up);
    shift_up(layout.arrow_down);
    layout.text_area.h -= shrink;
    layout.window.h -= shrink;
    const float drop = std::round(shrink * 0.3F);
    const auto drop_rect = [&](SDL_FRect &rect) { rect.y += drop; };
    drop_rect(layout.window);
    drop_rect(layout.done_button);
    drop_rect(layout.arrow_up);
    drop_rect(layout.arrow_down);
    // Entry 3's top stays fixed relative to the window; only its bottom
    // rises with the shrink, so it takes the drop but not the shift-up.
    drop_rect(layout.text_area);
    // The original republishes the shrunk entry-3 rect into the view; without
    // this the view keeps drawing (and black-filling) the full pre-shrink
    // text area, covering the backdrop strips.
    view.SetViewRect(layout.text_area);
  }

  const ReaderBackdrop backdrop = LoadReaderBackdrop(platform);

  // Publish the final (post-auto-size) control rects to the probe harness.
  ProbeUiAutoClear probe_ui_guard(platform);

  const auto draw_frame = [&]() {
    // Deliberate divergence (see docs/dlog_ditl_dialog_format.md): the
    // background is re-rendered every frame and the modal window is layered
    // on top; the original composites the dialog over the single game
    // surface. Without a background renderer the screen clears to black.
    if (render_background) {
      render_background();
    } else {
      SDL_SetRenderDrawColor(platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
      SDL_RenderClear(platform.renderer());
    }
    platform.SetPlacement(
        PlaceContained(authored_size, platform.logical_playfield_size()));
    const Placement &placement = platform.current_placement();
    platform.PublishProbeUi(
        "text_reader",
        {{"window", placement.ToWindowRect(layout.window)},
         {"done", placement.ToWindowRect(layout.done_button)},
         {"scroll_up", placement.ToWindowRect(layout.arrow_up)},
         {"scroll_down", placement.ToWindowRect(layout.arrow_down)}});
    // Window fill is the black space-background colour (PTR_DAT_00575acc),
    // matching the opaque black text panel below.
    SDL_SetRenderDrawColor(platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(platform.renderer(), &layout.window);
    // Backdrop composite (0x00499870): body 0x214d first, then the top
    // strip 0x214c, then the bottom strip 0x214c-anchored 0x214e -- the
    // bottom strip paints over the body's overhang. All blits clip to the
    // window rect.
    if (backdrop.top || backdrop.body || backdrop.bottom) {
      const SDL_Rect window_clip{static_cast<int>(layout.window.x),
                                 static_cast<int>(layout.window.y),
                                 static_cast<int>(layout.window.w),
                                 static_cast<int>(layout.window.h)};
      SDL_SetRenderClipRect(platform.renderer(), &window_clip);
      const auto blit = [&](const std::unique_ptr<SdlTexture> &texture,
                            float y) {
        if (!texture) {
          return;
        }
        float w = 0.0F;
        float h = 0.0F;
        SDL_GetTextureSize(texture->get(), &w, &h);
        const SDL_FRect dst{layout.window.x, y, w, h};
        SDL_RenderTexture(platform.renderer(), texture->get(), nullptr, &dst);
      };
      float top_h = 0.0F;
      if (backdrop.top) {
        SDL_GetTextureSize(backdrop.top->get(), nullptr, &top_h);
      }
      blit(backdrop.body, layout.window.y + top_h);
      blit(backdrop.top, layout.window.y);
      float bottom_h = 0.0F;
      if (backdrop.bottom) {
        SDL_GetTextureSize(backdrop.bottom->get(), nullptr, &bottom_h);
      }
      blit(backdrop.bottom, layout.window.y + layout.window.h - bottom_h);
      SDL_SetRenderClipRect(platform.renderer(), nullptr);
    }

    view.Draw(platform);
    // Arrow states (0x004a2ac0): scrollable direction -> normal, otherwise
    // disabled; when NEITHER direction can scroll both buttons take state
    // 0xfffe and are not drawn at all (a fitted view shows no arrows).
    if (view.scroll_offset() > 0.0F ||
        view.scroll_offset() < view.max_scroll()) {
      NovaUi_DrawScrollArrow(platform,
                             button_art,
                             layout.arrow_up,
                             true,
                             view.scroll_offset() > 0.0F);
      NovaUi_DrawScrollArrow(platform,
                             button_art,
                             layout.arrow_down,
                             false,
                             view.scroll_offset() < view.max_scroll());
    }

    button_art.Draw(platform, layout.done_button, ButtonState::kNormal);
    NovaText_DrawCentered(platform,
                          font_cache,
                          kThreeStateButtonFontFamily,
                          kThreeStateButtonFontSize,
                          kNovaFontStyleRegular,
                          SDL_Color{255, 255, 255, 255},
                          layout.done_button.x,
                          layout.done_button.x + layout.done_button.w,
                          ThreeStateButtonLabelBaseline(layout.done_button),
                          layout.done_caption);
    platform.Present();
  };

  draw_frame();
  NovaTextScrollHold scroll_hold;
  bool starmap_command_was_held = false;
  while (!platform.quit_requested()) {
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      if (input->key == TextKey::escape || input->key == TextKey::enter) {
        // Divergence: the original closes only via its Done button.
        NovaLog::Todo("text-reader Esc/Enter closes; the original exits only "
                      "via Done (0x004982a0)");
        return;
      }
      if (input->key == TextKey::primary) {
        const SDL_FPoint point = platform.mouse_position();
        if (Contains(layout.done_button, point)) {
          return;
        }
        // Scroll arrows (entries 5/6): Ghidra 0x00499440 enters its
        // hold-to-repeat loop when the press lands on the arrow, gated on the
        // maxed (up) / active (down) latch.
        if (Contains(layout.arrow_up, point)) {
          if (view.can_scroll_up()) {
            scroll_hold.Press(view, true, platform.wall_ticks_ms());
          }
        } else if (Contains(layout.arrow_down, point)) {
          if (view.can_scroll_down()) {
            scroll_hold.Press(view, false, platform.wall_ticks_ms());
          }
        }
        continue;
      }
      if (input->key == TextKey::physical) {
        // Ghidra 0x00499440: Up/Down scroll +/-10px (actions 5/6) and
        // Home/End/PageUp/PageDown jump to an end / move 250px, gated on the
        // maxed/active latches.
        (void)view.ApplyScrollKey(MapTextScrollKey(input->key_code));
      }
    }
    // Hold-to-repeat pump (0x00499440): 1px per 60Hz tick while the button
    // stays down over an arrow.
    (void)scroll_hold.Update(
        view, platform.PrimaryMouseDown(), platform.wall_ticks_ms());
    // Action 4: the map command (slot 9, default M) opens the starmap when
    // allow_starmap is set (the mission brief passes it). Edge-triggered so
    // the map does not reopen while the key stays held. TODO(decomp) skipped:
    // the original saves/restores the player ai_secondary_target_slot and
    // travel_transfer_mode around the map.
    const bool starmap_command_held =
        NovaInput_IsCommandActive(platform, kStarmapCommand);
    if (allow_starmap && starmap_command_held && !starmap_command_was_held) {
      (void)NovaStarmap_RunWindow(platform, state);
    }
    starmap_command_was_held = starmap_command_held;
    draw_frame();
    platform.PaceFrame();
  }
}

} // namespace game
