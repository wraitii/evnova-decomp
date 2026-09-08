#include "about_dialog.hpp"

#include "../brgr_archive.hpp"
#include "nova_font.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>
#include <vector>

namespace game {
namespace {

// Ghidra 0x004bc760: the text engine scales each 5-bit colour component by 8.
constexpr SDL_Color kAboutTextColor{0, 0, 0, SDL_ALPHA_OPAQUE};
constexpr SDL_Color kWindowFill{255, 255, 255, SDL_ALPHA_OPAQUE};
constexpr SDL_Color kWindowFrame{0, 0, 0, SDL_ALPHA_OPAQUE};
constexpr SDL_Color kButtonFill{219, 219, 219, SDL_ALPHA_OPAQUE};
// Description body text (landed descriptions use the same Geneva 9 / 11pt
// leading in dialog space).
constexpr float kAboutFontSize = 9.0F;
constexpr float kAboutLineHeight = 11.0F;
constexpr std::uint16_t kUpArrowKeyCode = 0xc8;   // DIK UP
constexpr std::uint16_t kDownArrowKeyCode = 0xd2; // DIK DOWN

struct AboutLayout {
  SDL_FRect window{};
  SDL_FRect text_area{};
  SDL_FRect ok_button{};
  SDL_FRect arrow_up{};
  SDL_FRect arrow_down{};
};

// DLOG 0xbbb (Nova.rez, 441x313 at 18,24) with DITL rows: 1 = OK button,
// 3 = text area, 5/6 = scroll arrows. The window is centred on the 640x480
// logical playfield (Dialog_CreateFromDlog 0x008730a1). Falls back to those
// authored rects when the resource cannot be parsed.
[[nodiscard]] AboutLayout LoadAboutLayout() {
  AboutLayout layout{};
  layout.window = {99.0F, 83.0F, 441.0F, 313.0F};
  layout.text_area = {110.0F, 93.0F, 417.0F, 262.0F};
  layout.ok_button = {272.0F, 364.0F, 99.0F, 25.0F};
  layout.arrow_up = {429.0F, 365.0F, 23.0F, 23.0F};
  layout.arrow_down = {462.0F, 365.0F, 23.0F, 23.0F};

  const auto definition = NovaResource_LoadDialogDefinition(0xbbb);
  const auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (definition && items) {
    const float win_w =
        static_cast<float>(definition->right - definition->left);
    const float win_h =
        static_cast<float>(definition->bottom - definition->top);
    layout.window = SDL_FRect{
        (640.0F - win_w) / 2.0F, (480.0F - win_h) / 2.0F, win_w, win_h};
    const auto rect = [&](std::size_t row_1based, SDL_FRect fallback) {
      if (row_1based == 0 || row_1based > items->size()) {
        return fallback;
      }
      const auto &item = (*items)[row_1based - 1];
      return SDL_FRect{layout.window.x + static_cast<float>(item.left),
                       layout.window.y + static_cast<float>(item.top),
                       static_cast<float>(item.right - item.left),
                       static_cast<float>(item.bottom - item.top)};
    };
    layout.ok_button = rect(1, layout.ok_button);
    layout.text_area = rect(3, layout.text_area);
    layout.arrow_up = rect(5, layout.arrow_up);
    layout.arrow_down = rect(6, layout.arrow_down);
  }
  return layout;
}

// d\x91sc 0x7fff "About text" (Nova Data 5) split into display lines. The
// <REG> marker is the registration-name slot filled by
// Stellar_BuildTravelDestinationDescription at runtime.
[[nodiscard]] std::vector<std::string> LoadAboutLines() {
  std::string text;
  if (const auto data = NovaResource_Load(kResourceTypeDescription, 0x7fff)) {
    text.reserve(data->size());
    for (const std::byte b : *data) {
      const auto c = std::to_integer<char>(b);
      text.push_back(c == '\r' ? '\n' : c);
    }
  }
  // TODO(decomp(0x0045d100)) skipped: Stellar_BuildTravelDestinationDescription
  // substitutes the registered pilot/company name for <REG>; the port has no
  // registration record yet.
  const std::string marker = "<REG>";
  for (std::size_t pos = text.find(marker); pos != std::string::npos;
       pos = text.find(marker)) {
    text.replace(pos, marker.size(), "Unregistered");
  }

  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto end = text.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

[[nodiscard]] bool Contains(const SDL_FRect &rect, SDL_FPoint point) {
  return point.x >= rect.x && point.x < rect.x + rect.w && point.y >= rect.y &&
         point.y < rect.y + rect.h;
}

void DrawScrollArrow(SDL_Renderer *renderer, const SDL_FRect &rect, bool up) {
  SDL_SetRenderDrawColor(
      renderer, kWindowFill.r, kWindowFill.g, kWindowFill.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &rect);
  SDL_SetRenderDrawColor(renderer,
                         kWindowFrame.r,
                         kWindowFrame.g,
                         kWindowFrame.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &rect);
  const float cx = rect.x + rect.w / 2.0F;
  const float cy = rect.y + rect.h / 2.0F;
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

} // namespace

void NovaMenu_RunAboutDialog(SdlPlatform &platform,
                             NovaFontCache &font_cache,
                             const std::function<void()> &render_background) {
  SDL_Renderer *const renderer = platform.renderer();
  const AboutLayout layout = LoadAboutLayout();
  const std::vector<std::string> lines = LoadAboutLines();

  const int total_height =
      static_cast<int>(lines.size()) * static_cast<int>(kAboutLineHeight);
  const int visible_height = static_cast<int>(layout.text_area.h);
  int scroll_offset = 0;

  platform.SetScaledPlayfield();
  bool done = false;
  while (!done && !platform.quit_requested()) {
    if (render_background) {
      render_background();
    }
    platform.SetScaledPlayfield();

    // Dim the menu behind the modal so it reads as a popped window (the
    // original opens the selection dialog over the live menu frame).
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_FRect full{0.0F, 0.0F, 640.0F, 480.0F};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 130);
    SDL_RenderFillRect(renderer, &full);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    SDL_SetRenderDrawColor(renderer,
                           kWindowFill.r,
                           kWindowFill.g,
                           kWindowFill.b,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &layout.window);
    SDL_SetRenderDrawColor(renderer,
                           kWindowFrame.r,
                           kWindowFrame.g,
                           kWindowFrame.b,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &layout.window);

    // Scrolling text region. TODO(decomp): the original's read-only text view
    // (NovaTextView_Create 0x004bcd90) styles/wraps this region; the port draws
    // the d\x91sc lines verbatim and clips to the DITL rect.
    for (std::size_t i = 0; i < lines.size(); ++i) {
      const float line_top = layout.text_area.y +
                             static_cast<float>(i) * kAboutLineHeight -
                             static_cast<float>(scroll_offset);
      if (line_top + kAboutLineHeight <= layout.text_area.y ||
          line_top >= layout.text_area.y + layout.text_area.h) {
        continue;
      }
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    kAboutFontSize,
                    kNovaFontStyleRegular,
                    kAboutTextColor,
                    layout.text_area.x + 4.0F,
                    line_top + 9.0F,
                    lines[i]);
    }

    DrawScrollArrow(renderer, layout.arrow_up, true);
    DrawScrollArrow(renderer, layout.arrow_down, false);

    // OK button.
    SDL_SetRenderDrawColor(renderer,
                           kButtonFill.r,
                           kButtonFill.g,
                           kButtonFill.b,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &layout.ok_button);
    SDL_SetRenderDrawColor(renderer,
                           kWindowFrame.r,
                           kWindowFrame.g,
                           kWindowFrame.b,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &layout.ok_button);
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          12.0F,
                          kNovaFontStyleRegular,
                          kAboutTextColor,
                          layout.ok_button.x + 4.0F,
                          layout.ok_button.x + layout.ok_button.w - 4.0F,
                          layout.ok_button.y + layout.ok_button.h / 2.0F + 4.0F,
                          "OK");

    platform.Present();

    while (const auto input = platform.PollTextEvent()) {
      switch (input->key) {
      case TextKey::primary: {
        const auto mouse = platform.mouse_position();
        if (Contains(layout.ok_button, mouse)) {
          done = true;
        } else if (Contains(layout.arrow_up, mouse)) {
          scroll_offset = std::max(
              0, scroll_offset - static_cast<int>(kAboutLineHeight * 3.0F));
        } else if (Contains(layout.arrow_down, mouse)) {
          scroll_offset = std::min(
              total_height - visible_height,
              scroll_offset + static_cast<int>(kAboutLineHeight * 3.0F));
        }
        break;
      }
      case TextKey::enter:
      case TextKey::escape:
        done = true;
        break;
      case TextKey::physical:
        if (input->key_code == kUpArrowKeyCode) {
          scroll_offset =
              std::max(0, scroll_offset - static_cast<int>(kAboutLineHeight));
        } else if (input->key_code == kDownArrowKeyCode) {
          scroll_offset =
              std::min(total_height - visible_height,
                       scroll_offset + static_cast<int>(kAboutLineHeight));
        }
        break;
      default:
        break;
      }
    }
  }
  platform.SetScaledPlayfield();
}

} // namespace game
