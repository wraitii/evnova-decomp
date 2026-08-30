#include "ui_dialog.hpp"

#include "../log.hpp"
#include "../pict_image.hpp"
#include "nova_font.hpp"

#include <SDL3/SDL_render.h>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace game {
namespace {

// The four grayscale RGBColor triples UiWindow_Draw uses for control bevels
// (0x0056f118 / 0x0056f11e / 0x0056f124 / 0x0056f12a; 16-bit channels stored
// little-endian in this binary). The startup window fill/frame globals are
// white / black (g_hud_overlay_text_color / DAT_00733b74).
constexpr SDL_Color kWindowFrame{0, 0, 0, 255};
constexpr SDL_Color kWindowFill{255, 255, 255, 255};
constexpr SDL_Color kBevelHighlight{195, 195, 195, 255}; // 0x0056f118
constexpr SDL_Color kBevelFill{136, 136, 136, 255};      // 0x0056f11e
constexpr SDL_Color kBevelPressedFill{97, 97, 97, 255};  // 0x0056f124
constexpr SDL_Color kBevelShadow{58, 58, 58, 255};       // 0x0056f12a
constexpr SDL_Color kControlText{0, 0, 0, 255};
// Characters inside an edit-text selection invert to white on the highlight.
constexpr SDL_Color kSelectionText{255, 255, 255, 255};

constexpr float kDialogFontSize = 12.0F;
// Popup entry height the original's popup drawer uses (0x14 units).
constexpr float kPopupRowHeight = 20.0F;

[[nodiscard]] SDL_FRect ItemRect(const NovaDialogItem &item,
                                 const SDL_FRect &window) {
  return SDL_FRect{window.x + static_cast<float>(item.left),
                   window.y + static_cast<float>(item.top),
                   static_cast<float>(item.right - item.left),
                   static_cast<float>(item.bottom - item.top)};
}

[[nodiscard]] bool Contains(const SDL_FRect &r, float x, float y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Centres a win_w x win_h dialog inside the playfield's on-screen rect
// (window points), truncating the half-offsets like Dialog_CreateFromDlog.
[[nodiscard]] SDL_FRect
CenterDialogInPlayfield(const SDL_FRect &playfield, float win_w, float win_h) {
  return SDL_FRect{playfield.x + std::truncf((playfield.w - win_w) * 0.5F),
                   playfield.y + std::truncf((playfield.h - win_h) * 0.5F),
                   win_w,
                   win_h};
}

// FUN_004d0a50: fills the rect with `fill`, bevels the top/left edges with
// `highlight` and the bottom/right edges with `shadow`, then the caller frames
// the rect in the window colour.
void DrawBevel(SDL_Renderer *renderer,
               const SDL_FRect &box,
               const SDL_Color &fill,
               const SDL_Color &highlight,
               const SDL_Color &shadow) {
  SDL_SetRenderDrawColor(
      renderer, fill.r, fill.g, fill.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &box);
  SDL_SetRenderDrawColor(renderer,
                         highlight.r,
                         highlight.g,
                         highlight.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderLine(
      renderer, box.x + 1.0F, box.y + 1.0F, box.x + box.w - 1.0F, box.y + 1.0F);
  SDL_RenderLine(
      renderer, box.x + 1.0F, box.y + 1.0F, box.x + 1.0F, box.y + box.h - 1.0F);
  SDL_SetRenderDrawColor(
      renderer, shadow.r, shadow.g, shadow.b, SDL_ALPHA_OPAQUE);
  SDL_RenderLine(renderer,
                 box.x + box.w - 2.0F,
                 box.y + box.h - 2.0F,
                 box.x + 1.0F,
                 box.y + box.h - 1.0F);
  SDL_RenderLine(renderer,
                 box.x + box.w - 2.0F,
                 box.y + box.h - 2.0F,
                 box.x + box.w - 1.0F,
                 box.y + 1.0F);
}

void FrameRect(SDL_Renderer *renderer, const SDL_FRect &box) {
  SDL_SetRenderDrawColor(renderer,
                         kWindowFrame.r,
                         kWindowFrame.g,
                         kWindowFrame.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &box);
}

// FUN_004d0bd0: the focus ring drawn just inside the focused control.
void DrawFocusRing(SDL_Renderer *renderer, const SDL_FRect &box) {
  SDL_FRect ring{box.x + 3.0F, box.y + 3.0F, box.w - 6.0F, box.h - 6.0F};
  ring.w -= 1.0F;
  ring.h -= 1.0F;
  FrameRect(renderer, ring);
}

// FUN_004bcb00: string width in logical pixels, used for caret/selection
// placement and popup label alignment (measured with the same face the draw
// path uses).
[[nodiscard]] float TextWidth(NovaFontCache &font_cache, std::string_view text) {
  return static_cast<float>(font_cache.TextWidth(NovaFontFamily::kGeneva,
                                                 kDialogFontSize,
                                                 kNovaFontStyleRegular,
                                                 text));
}

} // namespace

UiDialogWindow::ItemState *UiDialogWindow::Entry(std::size_t row_1based) {
  if (row_1based == 0 || row_1based > state.size()) {
    return nullptr;
  }
  return &state[row_1based - 1];
}

const UiDialogWindow::ItemState *
UiDialogWindow::Entry(std::size_t row_1based) const {
  if (row_1based == 0 || row_1based > state.size()) {
    return nullptr;
  }
  return &state[row_1based - 1];
}

const NovaDialogItem *UiDialogWindow::Item(std::size_t row_1based) const {
  if (row_1based == 0 || row_1based > items.size()) {
    return nullptr;
  }
  return &items[row_1based - 1];
}

std::optional<UiDialogWindow>
UiWindow_CreateFromDialogResource(SdlPlatform &platform,
                                  std::uint16_t dialog_id) {
  (void)platform;
  const auto definition = NovaResource_LoadDialogDefinition(dialog_id);
  if (!definition) {
    NovaLog::Todo("DLOG {:#06x} could not be located", dialog_id);
    return std::nullopt;
  }
  auto items = NovaResource_LoadDialogItems(definition->dialog_item_list_id);
  if (!items) {
    NovaLog::Todo("DITL {:#06x} (DLOG {:#06x}) could not be located",
                  definition->dialog_item_list_id,
                  dialog_id);
    return std::nullopt;
  }

  UiDialogWindow window;
  window.definition = *definition;
  window.items = std::move(*items);
  window.state.resize(window.items.size());

  // DIVERGENCE: the original switches to the dialog's owner draw context and
  // blits a dedicated window surface over the playfield
  // (DrawContext_SaveAndSetOwnerContext / DrawContext_BlitClippedRect); the
  // background under a modal is whatever that surface machinery last left.
  // The port re-renders the active screen every frame via the run loop's
  // render_background callback and draws the dialog straight onto the renderer
  // at 1:1 window scale centred inside the playfield's on-screen rect (the
  // active screen's logical presentation would upscale the dialog ~2x and
  // smear the 1px control frames). The active screen re-asserts its own
  // presentation on its next frame.
  platform.ApplyWindowPointDrawing();

  // Dialog_CreateFromDlog 0x008730a1: centre on the 640x480 logical playfield,
  // truncating the half-offsets (here: inside the playfield's on-screen rect,
  // window points).
  const float win_w = static_cast<float>(definition->right - definition->left);
  const float win_h = static_cast<float>(definition->bottom - definition->top);
  window.window_rect = CenterDialogInPlayfield(platform.playfield_window_rect(),
                                               win_w, win_h);

  // Type-7 popups seed their entries from the MENU resource named in the DITL
  // tail; runtime-filled popups (e.g. MENU 0x1f5 "Character") arrive empty and
  // are populated through UiPanel_SetEntryListItems.
  for (std::size_t i = 0; i < window.items.size(); ++i) {
    const auto &item = window.items[i];
    if (item.type == 7 && item.refcon != 0) {
      if (const auto menu = NovaResource_LoadMenuDefinition(item.refcon)) {
        window.state[i].popup_title = menu->title;
        window.state[i].popup_entries = menu->entries;
      }
    }
  }
  return window;
}

void UiControl_SetValue(UiDialogWindow &window,
                        std::size_t row_1based,
                        std::int32_t value) {
  if (auto *entry = window.Entry(row_1based)) {
    entry->value = value;
  }
}

std::int32_t UiControl_GetValue(const UiDialogWindow &window,
                                std::size_t row_1based) {
  const auto *entry = window.Entry(row_1based);
  return entry != nullptr ? entry->value : 0;
}

void UiPanel_SetEntryTextPascal(UiDialogWindow &window,
                                std::size_t row_1based,
                                std::string_view text) {
  if (auto *entry = window.Entry(row_1based)) {
    entry->text.assign(text);
  }
}

std::string UiPanel_GetEntryTextPascal(const UiDialogWindow &window,
                                       std::size_t row_1based) {
  const auto *entry = window.Entry(row_1based);
  return entry != nullptr ? entry->text : std::string{};
}

std::optional<std::string>
UiPanel_GetEntryTextPascalIndexed(const UiDialogWindow &window,
                                  std::size_t row_1based,
                                  std::int32_t value_1based) {
  const auto *entry = window.Entry(row_1based);
  if (entry == nullptr || value_1based < 1 ||
      static_cast<std::size_t>(value_1based) > entry->popup_entries.size()) {
    return std::nullopt;
  }
  return entry->popup_entries[static_cast<std::size_t>(value_1based) - 1];
}

void UiPanel_SetTextEntrySelectionRange(UiDialogWindow &window,
                                        std::size_t row_1based,
                                        std::int32_t start,
                                        std::int32_t end) {
  auto *entry = window.Entry(row_1based);
  if (entry == nullptr) {
    return;
  }
  entry->selection_start = std::max(0, start);
  // The dialog call sites pass 0xfe for "to the end of the text".
  if (end >= 0xfe) {
    end = static_cast<std::int32_t>(entry->text.size());
  }
  entry->selection_end = std::max(entry->selection_start, end);
  window.focused_row = row_1based;
}

void UiPanel_SetEntryListItems(UiDialogWindow &window,
                               std::size_t row_1based,
                               std::string_view title,
                               std::vector<std::string> entries) {
  auto *entry = window.Entry(row_1based);
  if (entry == nullptr) {
    return;
  }
  entry->popup_title.assign(title);
  entry->popup_entries = std::move(entries);
  entry->value = std::min<std::int32_t>(
      entry->value, static_cast<std::int32_t>(entry->popup_entries.size()));
}

void UiWindow_Draw(SdlPlatform &platform,
                   NovaFontCache &font_cache,
                   UiDialogWindow &window) {
  SDL_Renderer *renderer = platform.renderer();

  // Window surface: current fill colour + current RGB frame (white / black).
  SDL_SetRenderDrawColor(renderer,
                         kWindowFill.r,
                         kWindowFill.g,
                         kWindowFill.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &window.window_rect);
  FrameRect(renderer, window.window_rect);

  // The original draws items into the dialog's own window surface, so the
  // window edge clips them (DITL 0xc1e carries the Character popup at y=277
  // and vestigial "name3:" items at y=357 in a 213-tall window; those must
  // stay invisible). The port composites straight onto the renderer, so clip
  // the item pass to the window rect instead.
  const SDL_Rect clip{static_cast<int>(window.window_rect.x),
                      static_cast<int>(window.window_rect.y),
                      static_cast<int>(window.window_rect.w),
                      static_cast<int>(window.window_rect.h)};
  SDL_SetRenderClipRect(renderer, &clip);
  for (std::size_t row = 1; row <= window.items.size(); ++row) {
    const auto &item = window.items[row - 1];
    auto &entry = window.state[row - 1];
    // UiWindow_Draw draws every item whose flag byte is nonzero; it does not
    // honour the classic DITL bit-7 enable bit as a draw gate (DITL 0xc1d
    // ships all interactive controls with bit 7 clear and they render in the
    // real game), so neither do we.
    const SDL_FRect box = ItemRect(item, window.window_rect);
    const bool focused = window.focused_row == row;

    switch (item.type) {
    case 4: { // Button
      const bool pressed = entry.value != 0;
      DrawBevel(renderer,
                box,
                pressed ? kBevelPressedFill : kBevelFill,
                pressed ? kBevelShadow : kBevelHighlight,
                pressed ? kBevelHighlight : kBevelShadow);
      FrameRect(renderer, box);
      NovaText_DrawCentered(platform,
                            font_cache,
                            NovaFontFamily::kGeneva,
                            kDialogFontSize,
                            kNovaFontStyleRegular,
                            kControlText,
                            box.x + 4.0F,
                            box.x + box.w - 4.0F,
                            box.y + box.h / 2.0F + 4.0F,
                            item.title);
      if (focused) {
        DrawFocusRing(renderer, box);
      }
      break;
    }
    case 5:
    case 6: { // Checkbox / radio: 17x17 glyph at the rect's top-left.
      const SDL_FRect glyph{box.x, box.y, 17.0F, 17.0F};
      DrawBevel(renderer,
                glyph,
                kBevelFill,
                entry.value != 0 ? kBevelShadow : kBevelHighlight,
                entry.value != 0 ? kBevelHighlight : kBevelShadow);
      FrameRect(renderer, glyph);
      if (entry.value != 0) {
        SDL_SetRenderDrawColor(renderer,
                               kWindowFill.r,
                               kWindowFill.g,
                               kWindowFill.b,
                               SDL_ALPHA_OPAQUE);
        SDL_RenderLine(renderer,
                       glyph.x + 3.0F,
                       glyph.y + 6.0F,
                       glyph.x + 6.0F,
                       glyph.y + 9.0F);
        SDL_RenderLine(renderer,
                       glyph.x + 6.0F,
                       glyph.y + 9.0F,
                       glyph.x + 13.0F,
                       glyph.y + 3.0F);
      }
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    kDialogFontSize,
                    kNovaFontStyleRegular,
                    kControlText,
                    box.x + 20.0F,
                    box.y + box.h / 2.0F + 5.0F,
                    item.title);
      break;
    }
    case 8: { // Static text
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    kDialogFontSize,
                    kNovaFontStyleRegular,
                    kControlText,
                    box.x,
                    box.y + box.h / 2.0F + 5.0F,
                    entry.text.empty() ? item.title : entry.text);
      break;
    }
    case 0x10: { // Edit text
      SDL_FRect inset{box.x - 1.0F, box.y - 1.0F, box.w + 2.0F, box.h + 2.0F};
      DrawBevel(renderer,
                inset,
                kWindowFill,
                kBevelHighlight,
                kWindowFill);
      FrameRect(renderer, box);
      const float text_left = box.x + 3.0F;
      const float baseline = box.y + box.h - 3.0F;
      // Inverted selection between the two character offsets: the highlight
      // backdrop goes down first and the selected characters draw in white
      // through it (drawing the field black first painted them out).
      const bool has_selection =
          focused && entry.selection_start < entry.selection_end &&
          entry.selection_end <= static_cast<std::int32_t>(entry.text.size());
      if (has_selection) {
        const auto start = static_cast<std::size_t>(entry.selection_start);
        const auto end = static_cast<std::size_t>(entry.selection_end);
        const float x0 =
            text_left + TextWidth(font_cache, entry.text.substr(0, start));
        const float x1 =
            text_left + TextWidth(font_cache, entry.text.substr(0, end));
        SDL_SetRenderDrawColor(renderer,
                               kControlText.r,
                               kControlText.g,
                               kControlText.b,
                               SDL_ALPHA_OPAQUE);
        const SDL_FRect sel{x0, box.y + 1.0F, std::max(1.0F, x1 - x0), box.h - 2.0F};
        SDL_RenderFillRect(renderer, &sel);
        const auto draw_span = [&](std::size_t from,
                                   std::size_t to,
                                   float x,
                                   const SDL_Color &color) {
          if (to > from) {
            NovaText_Draw(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          kDialogFontSize,
                          kNovaFontStyleRegular,
                          color,
                          x,
                          baseline,
                          entry.text.substr(from, to - from));
          }
        };
        draw_span(0, start, text_left, kControlText);
        draw_span(start, end, x0, kSelectionText);
        draw_span(end, entry.text.size(), x1, kControlText);
      } else {
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      kDialogFontSize,
                      kNovaFontStyleRegular,
                      kControlText,
                      text_left,
                      baseline,
                      entry.text);
        if (focused) {
          // Blinking caret line (the original toggles its latch every 15 ms).
          const float caret_x =
              text_left +
              TextWidth(font_cache, entry.text.substr(
                  0, static_cast<std::size_t>(entry.selection_start)));
          if ((SDL_GetTicks() / 530U) % 2U == 0U) {
            SDL_SetRenderDrawColor(renderer,
                                   kControlText.r,
                                   kControlText.g,
                                   kControlText.b,
                                   SDL_ALPHA_OPAQUE);
            SDL_RenderLine(renderer,
                           caret_x,
                           box.y + 3.0F,
                           caret_x,
                           box.y + box.h - 3.0F);
          }
        }
      }
      break;
    }
    case 7: { // Popup (drawn by the stock dialog callback in the original)
      DrawBevel(renderer, box, kBevelFill, kBevelHighlight, kBevelShadow);
      FrameRect(renderer, box);
      // Title on the left, current selection right-aligned, arrow at the edge.
      if (!entry.popup_title.empty()) {
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      kDialogFontSize,
                      kNovaFontStyleRegular,
                      kControlText,
                      box.x + 4.0F,
                      box.y + box.h / 2.0F + 4.0F,
                      entry.popup_title);
      }
      std::string selected;
      if (entry.value >= 1 &&
          static_cast<std::size_t>(entry.value) <= entry.popup_entries.size()) {
        selected = entry.popup_entries[static_cast<std::size_t>(entry.value) - 1];
      }
      const float arrow_w = 10.0F;
      if (!selected.empty()) {
        const float width = TextWidth(font_cache, selected);
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      kDialogFontSize,
                      kNovaFontStyleRegular,
                      kControlText,
                      box.x + box.w - arrow_w - 6.0F - width,
                      box.y + box.h / 2.0F + 4.0F,
                      selected);
      }
      SDL_SetRenderDrawColor(renderer,
                             kControlText.r,
                             kControlText.g,
                             kControlText.b,
                             SDL_ALPHA_OPAQUE);
      for (int i = 0; i < 4; ++i) {
        SDL_RenderLine(renderer,
                       box.x + box.w - arrow_w + static_cast<float>(i),
                       box.y + box.h / 2.0F - 2.0F + static_cast<float>(i),
                       box.x + box.w - static_cast<float>(i) - 1.0F,
                       box.y + box.h / 2.0F - 2.0F + static_cast<float>(i));
      }
      if (entry.popup_expanded && !entry.popup_entries.empty()) {
        // Dropdown below the control: one kPopupRowHeight row per entry,
        // selected row inverted.
        SDL_FRect list{box.x, box.y + box.h, box.w,
                       kPopupRowHeight * static_cast<float>(entry.popup_entries.size())};
        SDL_SetRenderDrawColor(renderer,
                               kWindowFill.r,
                               kWindowFill.g,
                               kWindowFill.b,
                               SDL_ALPHA_OPAQUE);
        SDL_RenderFillRect(renderer, &list);
        FrameRect(renderer, list);
        for (std::size_t i = 0; i < entry.popup_entries.size(); ++i) {
          SDL_FRect row_rect{list.x, list.y + kPopupRowHeight * static_cast<float>(i),
                             list.w, kPopupRowHeight};
          if (static_cast<std::int32_t>(i) + 1 == entry.value) {
            SDL_SetRenderDrawColor(renderer,
                                   kControlText.r,
                                   kControlText.g,
                                   kControlText.b,
                                   SDL_ALPHA_OPAQUE);
            SDL_RenderFillRect(renderer, &row_rect);
          }
          NovaText_Draw(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        kDialogFontSize,
                        kNovaFontStyleRegular,
                        static_cast<std::int32_t>(i) + 1 == entry.value
                            ? SDL_Color{255, 255, 255, 255}
                            : kControlText,
                        row_rect.x + 4.0F,
                        row_rect.y + 14.0F,
                        entry.popup_entries[i]);
        }
      }
      break;
    }
    case 0x40: {
      // UiWindow_Draw blits the control record's image into the item rect;
      // the stock dialog callback fills that handle from the item's refcon
      // PICT (DITL 0xc1d: item 2 -> PICT 129 "Strict Play" note art, item 13
      // -> PICT 130 pilot icon). Gauge payloads (0x20) remain a plate.
      if (!entry.image && item.refcon != 0) {
        if (auto pict = NovaResource_LoadPictData(item.refcon)) {
          if (auto image = Resource_LoadPictAsImage(*pict)) {
            entry.image = SdlTexture::Create(platform.renderer(),
                                             image->width,
                                             image->height,
                                             image->rgba_pixels);
          }
        }
        if (!entry.image) {
          NovaLog::Todo("dialog 0x{:04x} item {} PICT {:#06x} unavailable",
                        window.definition.dialog_item_list_id,
                        row,
                        item.refcon);
        }
      }
      if (entry.image) {
        SDL_RenderTexture(renderer,
                          entry.image->get(),
                          nullptr,
                          &box);
      } else {
        SDL_SetRenderDrawColor(renderer,
                               kBevelHighlight.r,
                               kBevelHighlight.g,
                               kBevelHighlight.b,
                               SDL_ALPHA_OPAQUE);
        SDL_RenderFillRect(renderer, &box);
        FrameRect(renderer, box);
      }
      break;
    }
    case 0x20:
      // TODO(decomp(0x004d0d00)) skipped: gauge payloads are not reconstructed;
      // draw a plain plate.
      SDL_SetRenderDrawColor(renderer,
                             kBevelHighlight.r,
                             kBevelHighlight.g,
                             kBevelHighlight.b,
                             SDL_ALPHA_OPAQUE);
      SDL_RenderFillRect(renderer, &box);
      FrameRect(renderer, box);
      break;
    default:
      break;
    }
  }
  SDL_SetRenderClipRect(renderer, nullptr);
}

void UiWindow_RunInteractionLoop(
    SdlPlatform &platform,
    NovaFontCache &font_cache,
    UiDialogWindow &window,
    short *code_out,
    const std::function<void()> &render_background) {
  *code_out = -1;
  // Keep the screen under the dialog alive: the callback redraws the active
  // screen under its own presentation, then we switch to the 1:1 compositing
  // state on top (re-asserted every frame; a window resize makes the platform
  // re-apply the underlying screen's presentation) and keep the dialog
  // centred in the playfield's on-screen rect.
  if (render_background) {
    render_background();
  }
  platform.ApplyWindowPointDrawing();
  window.window_rect = CenterDialogInPlayfield(
      platform.playfield_window_rect(),
      static_cast<float>(window.definition.right - window.definition.left),
      static_cast<float>(window.definition.bottom - window.definition.top));
  const SDL_FPoint mouse = platform.mouse_window_point();
  auto activate = [&](std::size_t row) {
    *code_out = static_cast<short>(row);
  };

  // A click outside an expanded popup collapses it without activating.
  auto expanded_row = [&]() -> std::size_t {
    for (std::size_t row = 1; row <= window.items.size(); ++row) {
      if (window.state[row - 1].popup_expanded) {
        return row;
      }
    }
    return 0;
  };

  for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
    switch (in->key) {
    case TextKey::primary: {
      // An expanded popup consumes the whole click: selecting an entry,
      // re-clicking the control, or clicking anywhere else all just close it.
      // (Without this, clicks landing on the dropdown area would fall through
      // to whatever earlier DITL row overlaps it.)
      const std::size_t expanding = expanded_row();
      if (expanding != 0) {
        auto &entry = *window.Entry(expanding);
        const SDL_FRect box =
            ItemRect(*window.Item(expanding), window.window_rect);
        const SDL_FRect list{
            box.x, box.y + box.h, box.w,
            kPopupRowHeight * static_cast<float>(entry.popup_entries.size())};
        if (Contains(list, mouse.x, mouse.y)) {
          entry.value = std::clamp(
              static_cast<int>((mouse.y - list.y) / kPopupRowHeight) + 1,
              1,
              static_cast<int>(entry.popup_entries.size()));
        }
        entry.popup_expanded = false;
        break;
      }
      for (std::size_t row = 1; row <= window.items.size(); ++row) {
        const auto &item = window.items[row - 1];
        auto &entry = window.state[row - 1];
        const SDL_FRect box = ItemRect(item, window.window_rect);
        if (!Contains(box, mouse.x, mouse.y)) {
          continue;
        }
        switch (item.type) {
        case 4: // Button: activation code = the 1-based ordinal.
          activate(row);
          break;
        case 5:
        case 6: // Checkbox: report the ordinal (code 4 in 0xc1d). The dialog
                // code performs the toggle itself via UiControl_SetValue,
                // mirroring the original's data flow.
          activate(row);
          break;
        case 7:
          if (!entry.popup_entries.empty()) {
            entry.popup_expanded = true;
          }
          break;
        case 0x10: // Edit text: focus and place the caret at the end.
          window.focused_row = row;
          entry.selection_start = entry.selection_end =
              static_cast<std::int32_t>(entry.text.size());
          break;
        default:
          break;
        }
        if (*code_out != -1) {
          break;
        }
      }
      break;
    }
    case TextKey::character:
    case TextKey::backspace: {
      auto *entry = window.Entry(window.focused_row);
      const auto *item = window.Item(window.focused_row);
      if (entry == nullptr || item == nullptr || item->type != 0x10) {
        break;
      }
      if (in->key == TextKey::backspace) {
        if (entry->selection_start == entry->selection_end &&
            entry->selection_start > 0) {
          entry->text.erase(static_cast<std::size_t>(entry->selection_start) - 1U,
                            1);
          entry->selection_start = entry->selection_end =
              entry->selection_start - 1;
        } else if (entry->selection_start < entry->selection_end) {
          entry->text.erase(static_cast<std::size_t>(entry->selection_start),
                            static_cast<std::size_t>(entry->selection_end -
                                                     entry->selection_start));
          entry->selection_end = entry->selection_start;
        }
      } else {
        if (entry->selection_start < entry->selection_end) {
          entry->text.erase(static_cast<std::size_t>(entry->selection_start),
                            static_cast<std::size_t>(entry->selection_end -
                                                     entry->selection_start));
          entry->selection_end = entry->selection_start;
        }
        entry->text.insert(
            static_cast<std::size_t>(entry->selection_start), 1, in->character);
        entry->selection_start = entry->selection_end = entry->selection_start + 1;
      }
      break;
    }
    case TextKey::enter: {
      // Enter activates the first button.
      for (std::size_t row = 1; row <= window.items.size(); ++row) {
        if (window.items[row - 1].type == 4) {
          activate(row);
          break;
        }
      }
      break;
    }
    case TextKey::escape: {
      // TODO(decomp(0x004cfdd0)) skipped: the original's exact escape-path key
      // set is not reconstructed; activating the "Cancel" button matches every
      // dialog shipped with the game.
      for (std::size_t row = 1; row <= window.items.size(); ++row) {
        if (window.items[row - 1].type == 4 &&
            window.items[row - 1].title == "Cancel") {
          activate(row);
          break;
        }
      }
      break;
    }
    default:
      break;
    }
    if (*code_out != -1) {
      break;
    }
  }

  UiWindow_Draw(platform, font_cache, window);
  SDL_RenderPresent(platform.renderer());
  // Modal-loop cadence: the original yields through its frame pump
  // (NovaPlatform_PumpWindowEventsThrottled); other ported dialogs use 16 ms.
  SDL_Delay(16);
}

} // namespace game
