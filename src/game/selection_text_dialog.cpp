#include "selection_text_dialog.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "landed_window.hpp"
#include "nova_font.hpp"
#include "services_buttons.hpp"
#include "starmap.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace game {
namespace {

constexpr std::uint16_t kReaderDialogId = 0xbbb;

// Shared selection-dialog text metrics: the 12pt Geneva selection font and
// the wrap/leading the view applies (NovaTextView_SetText + UpdateContent-
// Height measure with the same face).
constexpr float kTextSize = 12.0F;
constexpr float kLineHeight = 14.0F;
constexpr float kFirstLineOffset = 15.0F;
constexpr float kTextInset = 6.0F;

[[nodiscard]] bool Contains(const SDL_FRect &rect, SDL_FPoint point) {
  return point.x >= rect.x && point.x < rect.x + rect.w && point.y >= rect.y &&
         point.y < rect.y + rect.h;
}

} // namespace

NovaTextScrollView::NovaTextScrollView(NovaFontCache &fonts,
                                       std::string_view text,
                                       const SDL_FRect &view_rect)
    : view_rect_(view_rect), fonts_(&fonts) {
  const float text_w = std::max(1.0F, view_rect.w - 2.0F * kTextInset);
  lines_ = WrapDescriptionLines(
      text, static_cast<int>(text_w), [&](std::string_view line) {
        return fonts.TextWidth(
            NovaFontFamily::kGeneva, kTextSize, kNovaFontStyleRegular, line);
      });
  const float content_height =
      kFirstLineOffset + static_cast<float>(lines_.size()) * kLineHeight + 6.0F;
  content_height_ = content_height;
  max_scroll_ = std::max(0.0F, content_height - view_rect.h);
}

void NovaTextScrollView::ScrollBy(float delta) {
  scroll_offset_ = std::clamp(scroll_offset_ + delta, 0.0F, max_scroll_);
}

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
                    view_rect_.x + kTextInset,
                    baseline,
                    line);
    }
    baseline += kLineHeight;
  }
  SDL_SetRenderClipRect(renderer, nullptr);
}

void NovaUi_DrawScrollArrow(SdlPlatform &platform,
                            const SDL_FRect &rect,
                            bool up,
                            bool enabled) {
  SDL_Renderer *renderer = platform.renderer();
  constexpr SDL_Color kFill{16, 40, 72, 255};
  constexpr SDL_Color kFrame{80, 140, 190, 255};
  const SDL_Color &glyph =
      enabled ? SDL_Color{255, 255, 255, 255} : SDL_Color{128, 128, 128, 255};
  SDL_SetRenderDrawColor(renderer, kFill.r, kFill.g, kFill.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &rect);
  SDL_SetRenderDrawColor(
      renderer, kFrame.r, kFrame.g, kFrame.b, SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &rect);
  const float cx = rect.x + rect.w / 2.0F;
  const float cy = rect.y + rect.h / 2.0F;
  SDL_SetRenderDrawColor(renderer, glyph.r, glyph.g, glyph.b, SDL_ALPHA_OPAQUE);
  for (int i = 0; i < 5; ++i) {
    const float y = up ? cy - 2.0F + static_cast<float>(i)
                       : cy + 2.0F - static_cast<float>(i);
    SDL_RenderLine(renderer,
                   cx - static_cast<float>(i) - 1.0F,
                   y,
                   cx + static_cast<float>(i) + 1.0F,
                   y);
  }
}

namespace {

struct ReaderLayout {
  SDL_FRect window{};
  SDL_FRect done_button{}; // UiPanel entry 1
  SDL_FRect text_area{};   // UiPanel entry 3
  SDL_FRect arrow_up{};    // UiPanel entry 5
  SDL_FRect arrow_down{};  // UiPanel entry 6
  std::string done_caption = "Done";
};

// Window bounds + control rects from the real DLOG/DITL, centred on the
// fullscreen window-coordinate space the docked menu and store windows use
// (Dialog_CreateFromDlog 0x008730a1 centred on the original's fixed 640x480
// canvas; the port's docked screen spans the whole window, so centring on
// logical_playfield_size keeps the reader over the dock's centre).
[[nodiscard]] ReaderLayout LoadReaderLayout(const SdlPlatform &platform) {
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
  const SDL_FPoint output = platform.logical_playfield_size();
  const SDL_FPoint origin{(output.x - win_w) / 2.0F, (output.y - win_h) / 2.0F};
  layout.window = {origin.x, origin.y, win_w, win_h};
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
  // The Done caption lives in the DITL item's own Pascal string when the
  // resource carries one.
  for (const auto &item : *items) {
    if (item.index == 0 && !item.title.empty()) {
      layout.done_caption = item.title;
      break;
    }
  }
  return layout;
}

// Backdrop strip: PICT 0x214c/0x214d/0x214e blitted main-art top-anchored
// with the top and bottom strips over it (NovaUi_DrawSelectionDialogContent
// 0x00499870). Returns false when any piece is missing.
[[nodiscard]] bool DrawBackdropStrip(SdlPlatform &platform,
                                     const SDL_FRect &window) {
  const auto load = [&](std::uint16_t id) {
    const auto data = NovaResource_LoadPictData(id);
    const auto image = data ? Resource_LoadPictAsImage(*data) : std::nullopt;
    if (!data || !image) {
      return std::unique_ptr<SdlTexture>{};
    }
    return SdlTexture::Create(
        platform.renderer(), image->width, image->height, image->rgba_pixels);
  };
  auto main = load(0x214d);
  auto top = load(0x214c);
  auto bottom = load(0x214e);
  if (main == nullptr || top == nullptr || bottom == nullptr) {
    return false;
  }
  const auto blit = [&](SdlTexture &texture, const SDL_FRect &dst) {
    SDL_RenderTexture(platform.renderer(), texture.get(), nullptr, &dst);
  };
  float w = 0.0F;
  float h = 0.0F;
  SDL_GetTextureSize(main->get(), &w, &h);
  blit(*main,
       {window.x, window.y, std::min(w, window.w), std::min(h, window.h)});
  SDL_GetTextureSize(top->get(), &w, &h);
  blit(*top,
       {window.x, window.y, std::min(w, window.w), std::min(h, window.h)});
  SDL_GetTextureSize(bottom->get(), &w, &h);
  blit(*bottom,
       {window.x,
        window.y + window.h - std::min(h, window.h),
        std::min(w, window.w),
        std::min(h, window.h)});
  return true;
}

} // namespace

void NovaUi_RunTextReaderDialog(
    SdlPlatform &platform,
    GameState &state,
    const std::string &text,
    bool allow_starmap,
    const std::function<void()> &render_background) {
  ReaderLayout layout = LoadReaderLayout(platform);
  if (layout.window.w <= 0.0F) {
    return;
  }

  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);
  NovaFontCache font_cache;
  NovaTextScrollView view(font_cache, text, layout.text_area);

  // Auto-size arm (0x004982a0): when the text is shorter than the view the
  // window shrinks (content clamped to a minimum of 0x30) and the controls
  // below the text area (entries 1/5/6) shift up to hug it. The original
  // re-centres the moved window; the port matches that with rounded half
  // shifts.
  if (view.max_scroll() <= 0.0F && view.content_height() < layout.text_area.h) {
    const float shortfall = layout.text_area.h - view.content_height();
    const float shrink = shortfall + 16.0F; // the -0x10 adjustment
    const auto shift_up = [&](SDL_FRect &rect) { rect.y -= shrink; };
    shift_up(layout.done_button);
    shift_up(layout.arrow_up);
    shift_up(layout.arrow_down);
    layout.text_area.h -= shrink;
    layout.window.h -= shrink;
    layout.window.y += std::round(shrink / 2.0F);
  }

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
    // The reader's DLOG coordinates are window-point space.
    platform.SetFullscreenPlayfield();
    SDL_SetRenderDrawColor(platform.renderer(), 16, 40, 72, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(platform.renderer(), &layout.window);
    if (!DrawBackdropStrip(platform, layout.window)) {
      NovaLog::Todo("text-reader backdrop PICTs 0x214c/0x214d/0x214e "
                    "incomplete; using a flat window fill");
    }

    view.Draw(platform);
    NovaUi_DrawScrollArrow(
        platform, layout.arrow_up, true, view.scroll_offset() > 0.0F);
    NovaUi_DrawScrollArrow(platform,
                           layout.arrow_down,
                           false,
                           view.scroll_offset() < view.max_scroll());

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
    SDL_RenderPresent(platform.renderer());
  };

  draw_frame();
  bool starmap_requested = false;
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
        // Scroll arrows (actions 5/6, +/-10 px per NovaUi_ScrollSelectionText
        // in the modal loop).
        if (Contains(layout.arrow_down, point)) {
          view.ScrollBy(10.0F);
        } else if (Contains(layout.arrow_up, point)) {
          view.ScrollBy(-10.0F);
        }
        continue;
      }
      if (input->key == TextKey::character) {
        // Port stand-in for the original's starmap key binding (binding 9 ->
        // action 4 in NovaUi_PollTravelScriptAction).
        const char key = static_cast<char>(
            std::tolower(static_cast<unsigned char>(input->character)));
        if (key == 'm') {
          starmap_requested = true;
        }
      }
      if (input->key == TextKey::physical) {
        // Port convenience: DIK arrows scroll (the original scrolls only via
        // the arrow buttons).
        if (input->key_code == 0xc8) { // DIK_UP
          view.ScrollBy(-10.0F);
        } else if (input->key_code == 0xd0) { // DIK_DOWN
          view.ScrollBy(10.0F);
        }
      }
    }
    // Action 4: the starmap window when allow_starmap is set (the mission
    // brief passes it). The original also preselects the mission's
    // destination system when flags carry 0x100 and restores the flight-scene
    // travel state afterwards; TODO(decomp) skipped for the preselect.
    if (allow_starmap && starmap_requested) {
      (void)NovaStarmap_RunWindow(platform, state);
      starmap_requested = false;
    }
    draw_frame();
    SDL_Delay(16);
  }
}

} // namespace game
