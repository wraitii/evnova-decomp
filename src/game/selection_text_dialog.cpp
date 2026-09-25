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
constexpr std::uint16_t kReaderArtDialogId = 0xbbc;
constexpr std::uint16_t kReaderArtBackdropPict = 0x214f;
constexpr std::int16_t kArtVariantThreshold = 0x80;

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
// Breathing room below the last line once the text overflows. Applied only to
// the scrollable case (see UpdateMaxScroll) so a fitted view never gains
// phantom scroll just to show the margin.
constexpr float kBottomMargin = 2.0F;

} // namespace

// @port 0x004BCD90 80% ui,rendering
// Clean-room NovaTextScrollView ctor: stores the view/display rect and wraps
// the text at the full DITL view width without an extra horizontal inset with
// the shared Geneva-9 face and 11pt leading (NovaTextView_Create 0x004bcd90 +
// NovaTextView_SetText). Gaps: original +0xc/+0x14 rect-field split and the
// optional draw-context handle are structural.
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
  // The original top-aligns the run inside the content rect (first baseline at
  // content.top + ascent) and slides the view over it. The per-line draw here
  // anchors the run to the view at kFirstLineOffset, which is lower by
  // (kFirstLineOffset - ascent); carry that into the clamp so the final line's
  // descenders can still be scrolled fully into the view, plus a small gap.
  bottom_correction_ =
      std::max(0.0F,
               kFirstLineOffset -
                   static_cast<float>(fonts.Ascent(NovaFontFamily::kGeneva,
                                                   kTextSize,
                                                   kNovaFontStyleRegular))) +
      kBottomMargin;
  UpdateMaxScroll();
}

// @port 0x004BCE10 80% ui
// @port 0x004BCE90 75% ui
// NovaTextView_ScrollBy (0x004bce90) clamps the scroll against the content
// height reported by NovaTextView_UpdateContentHeight (0x004bce10) =
// NovaText_MeasureWrappedTextHeight, i.e. the wrapped text height only. The
// first-line/bottom insets in content_height_ are draw-only and must not
// inflate the scroll extent, or a fitted view claims scroll and the arrow
// buttons light up with nothing to scroll to. Once the text does overflow,
// however, the run's bottom edge (not its last baseline) has to reach the view
// bottom, so add back bottom_correction_. The guard keeps a view that only
// just fits (or was auto-shrunk to fit) at zero extent.
void NovaTextScrollView::UpdateMaxScroll() {
  max_scroll_ = std::max(0.0F, text_height_ - view_rect_.h);
  if (max_scroll_ > 0.0F) {
    max_scroll_ += bottom_correction_;
  }
}

// @port 0x00499270 85% correctness
// The smooth param_5==0 elapsed-tick path (px_per_second * ticks) is skipped:
// TODO(decomp(0x00499270)) skipped -- dead in the shipped binary, every call
// site passes the immediate flag. Ghidra 0x00499270 NovaUi_ScrollSelectionText,
// immediate arm. Its param_5 == 0 "smooth" arm would scale param_4 as px/sec by
// the elapsed 60Hz ticks (x the 1/60 double at 0x005759b0);
// TODO(decomp(0x00499270)) skipped: it is dead in the shipped binary -- every
// call site in the mission-offer poll/run and the text-reader run/callback
// passes the immediate flag -- so the port models only the +/-px step.
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

// @port 0x004BCF30 75% ui
// NovaTextScrollView::Draw: clipped wrapped redraw at the view rect using the
// scroll offset, with text starting at rect.left and wrapping to rect.right
// (no extra 6px inset). The incoming bottom line is submitted as soon as its
// line box enters the clip (not only once its baseline is inside), so its
// upper half shows while scrolling in, matching the original's single clipped
// DrawText over the whole run. Gaps: draw-context/owner-surface bookkeeping
// and the per-button flash are approximated.
// Ghidra 0x004bcf30 NovaTextView_Draw: the wrapped text is clipped to
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
  // The original hands the whole wrapped run to one clipped DrawText
  // (0x004bcf30 -> NovaText_DrawText), so a line whose baseline is still below
  // the view bottom already shows its upper half as it scrolls in. Per-line
  // drawing must test the line box top rather than the baseline, then let the
  // clip rect trim the overhang; breaking on the baseline dropped the incoming
  // line until it was fully in view.
  float baseline = view_rect_.y + kFirstLineOffset - scroll_offset_;
  const float view_bottom = view_rect_.y + view_rect_.h;
  for (const auto &line : lines_) {
    if (baseline - kLineHeight >= view_bottom) {
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
  SDL_FRect art_entry{};   // UiPanel entry 2: variant PICT target (art arm)
  SDL_FRect text_area{};   // UiPanel entry 3
  SDL_FRect arrow_up{};    // UiPanel entry 5
  SDL_FRect arrow_down{};  // UiPanel entry 6
  std::string done_caption = "Okay";
  bool art_variant = false;
};

// Window bounds + control rects from the real DLOG/DITL in authored local
// coordinates. The platform contains this composition and maps input back to
// the same local space.
[[nodiscard]] ReaderLayout LoadReaderLayout(std::int16_t dialog_variant) {
  ReaderLayout layout;
  layout.art_variant =
      static_cast<std::uint16_t>(dialog_variant) >= kArtVariantThreshold;
  const auto definition = NovaResource_LoadDialogDefinition(
      layout.art_variant ? kReaderArtDialogId : kReaderDialogId);
  const auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (!definition || !items) {
    NovaLog::Todo("text-reader DLOG/DITL {:#x} unavailable; refusing a "
                  "synthetic layout",
                  layout.art_variant ? kReaderArtDialogId : kReaderDialogId);
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
  layout.art_entry = rect(1);   // UiPanel entry 2 (art arm only)
  layout.text_area = rect(2);   // UiPanel entry 3
  layout.arrow_up = rect(4);    // UiPanel entry 5
  layout.arrow_down = rect(5);  // UiPanel entry 6
  // The Okay caption is STR# 0x96 entry 0x1b (0x004a2ac0).
  if (const auto caption = NovaHud_LoadStringEntry(0x96, 0x1b)) {
    layout.done_caption = *caption;
  }
  return layout;
}

struct ReaderBackdrop {
  std::unique_ptr<SdlTexture> top;    // 0x214c (variant < 0x80)
  std::unique_ptr<SdlTexture> body;   // 0x214d (variant < 0x80)
  std::unique_ptr<SdlTexture> bottom; // 0x214e (variant < 0x80)
  std::unique_ptr<SdlTexture> art;    // 0x214f (variant >= 0x80)
  // The dësc variant itself, blitted into DITL entry 2 in the art arm.
  std::unique_ptr<SdlTexture> variant_pict;
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

[[nodiscard]] ReaderBackdrop LoadReaderBackdrop(SdlPlatform &platform,
                                                std::int16_t dialog_variant) {
  ReaderBackdrop backdrop;
  if (static_cast<std::uint16_t>(dialog_variant) >= kArtVariantThreshold) {
    backdrop.art = LoadBackdropPict(platform, kReaderArtBackdropPict);
    backdrop.variant_pict =
        LoadBackdropPict(platform, static_cast<std::uint16_t>(dialog_variant));
    if (!backdrop.art) {
      NovaLog::Todo("text-reader art backdrop PICT 0x214f missing; window "
                    "stays a flat fill");
    }
    if (!backdrop.variant_pict) {
      NovaLog::Todo("text-reader variant PICT {:#x} missing; entry 2 stays "
                    "unpainted",
                    static_cast<std::uint16_t>(dialog_variant));
    }
    return backdrop;
  }
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

// @port 0x004982a0 88% ui
// TODO(decomp): desc status-string display (Ui_PlayMovieFileModal, a QuickTime
// platform replacement), save/restore player
// ai_secondary_target_slot/travel_transfer_mode around the map,
// over-static-surface redraw variants, pressed-button flash during click
// tracking. Background re-renders the live docked menu / flight view per frame
// rather than replaying a captured snapshot (deliberate divergence; Custom-art
// arm (Ghidra 0x004982a0): variant >= 0x80 runs in DLOG 0xbbc with the single
// backdrop PICT 0x214f and blits the variant PICT into entry 2.
// @port 0x00499870 60% rendering
// @port 0x004A2AC0 40% ui,rendering
// Ghidra 0x004a2ac0 OutfitterMenu_FUN_004a2ac0 reader draw slice: the Okay
// button and DITL entry 5/6 scroll arrows are drawn inline in
// NovaUi_RunTextReaderDialog below; the -3 caller-supplied-image arm and
// plus/minus pen raster remain approximate.
// TODO(decomp): the offscreen-surface owner switching and
// DrawContext_BlitClippedRect composite are replaced by direct rendering to the
// platform surface. Backdrop for the < 0x80 variant: three PICTs composed by
// NovaUi_DrawSelectionDialogContent 0x00499870 in order body -> top ->
// bottom: 0x214d (441x365) at window.y + height(0x214c), 0x214c (441x9) at
// the window top, 0x214e (441x40) anchored to the window bottom (it wins
// where the over-tall body overlaps). DAT_007d4bf0/4bf4 are overlapping
// aliases of the image-slot array's entries 1/2 and stay set for the reader
// -- the >= 0x80 art variant is the only path that zeroes them. Blits clip
// to the window rect (the painter draws inside the modal window's context).
// Loaded once per dialog, not per frame.
void NovaUi_RunTextReaderDialog(SdlPlatform &platform,
                                GameState &state,
                                const std::string &text,
                                bool allow_starmap,
                                const std::function<void()> &render_background,
                                std::int16_t dialog_variant) {
  const SdlPlatform::ScopedPlacement restore_placement(
      platform, platform.current_placement());
  ReaderLayout layout = LoadReaderLayout(dialog_variant);
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
  if (!layout.art_variant && view.text_height() < layout.text_area.h) {
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

  const ReaderBackdrop backdrop = LoadReaderBackdrop(platform, dialog_variant);

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
    // Backdrop composite (0x00499870). Variant < 0x80: body 0x214d first,
    // then the top strip 0x214c, then the bottom strip 0x214e -- the bottom
    // strip paints over the body's overhang. Variant >= 0x80: the single art
    // PICT 0x214f fills the window, then the dësc variant PICT is blitted
    // into entry 2. All blits clip to the window rect.
    const auto blit = [&](const std::unique_ptr<SdlTexture> &texture,
                          const SDL_FRect &dst) {
      if (texture) {
        SDL_RenderTexture(platform.renderer(), texture->get(), nullptr, &dst);
      }
    };
    const auto blit_native =
        [&](const std::unique_ptr<SdlTexture> &texture, float x, float y) {
          if (!texture) {
            return;
          }
          float w = 0.0F;
          float h = 0.0F;
          SDL_GetTextureSize(texture->get(), &w, &h);
          blit(texture, SDL_FRect{x, y, w, h});
        };
    if (backdrop.art || backdrop.variant_pict || backdrop.top ||
        backdrop.body || backdrop.bottom) {
      const SDL_Rect window_clip{static_cast<int>(layout.window.x),
                                 static_cast<int>(layout.window.y),
                                 static_cast<int>(layout.window.w),
                                 static_cast<int>(layout.window.h)};
      SDL_SetRenderClipRect(platform.renderer(), &window_clip);
      if (layout.art_variant) {
        blit(backdrop.art, layout.window);
        blit(backdrop.variant_pict, layout.art_entry);
      } else {
        float top_h = 0.0F;
        if (backdrop.top) {
          SDL_GetTextureSize(backdrop.top->get(), nullptr, &top_h);
        }
        blit_native(backdrop.body, layout.window.x, layout.window.y + top_h);
        blit_native(backdrop.top, layout.window.x, layout.window.y);
        float bottom_h = 0.0F;
        if (backdrop.bottom) {
          SDL_GetTextureSize(backdrop.bottom->get(), nullptr, &bottom_h);
        }
        blit_native(backdrop.bottom,
                    layout.window.x,
                    layout.window.y + layout.window.h - bottom_h);
      }
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
