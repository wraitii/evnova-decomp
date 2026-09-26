#pragma once

// Shared drawing primitives for the port's preference dialogs. These are the
// clean-room reimplementation of the original UiWindow_Draw chrome that the
// Settings dialog (DLOG 0xfa3) needs and that the port-only Extra Prefs modal
// in preferences_extra.cpp reuses. They are small and header-inline so both
// translation units share one definition; LoadSettingsArtwork stays in
// preferences.cpp because it loads resource PICTs.

#include "../sdl_platform.hpp"
#include "control_bevel.hpp"
#include "nova_font.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace game::preferences_detail {

// The Settings dialog resource id (DLOG/DITL 0xfa3). Also used for the Extra
// Prefs modal's reused window chrome.
inline constexpr std::uint16_t kSettingsDialogId = 0xfa3;

// Window/control colours. The original fills the dialog with the current fill
// colour and frames it with the current RGB colour; at startup those globals
// are white (g_hud_overlay_text_color) and black (DAT_00733b74). The bevel
// triples are the RGBColor constants at 0x0056f118..0x0056f12a.
inline constexpr SDL_Color kWindowFrame{0, 0, 0, 255};
inline constexpr SDL_Color kWindowFill{255, 255, 255, 255};
inline constexpr SDL_Color kControlHighlight{195, 195, 195, 255};
inline constexpr SDL_Color kControlFill{136, 136, 136, 255};
inline constexpr SDL_Color kControlShadow{58, 58, 58, 255};
inline constexpr SDL_Color kControlText{0, 0, 0, 255};
inline constexpr SDL_Color kControlSelectedText{255, 255, 255, 255};
// Locked controls are drawn light grey (a port addition: the original has no
// disabled state because these preferences were live toggles).
inline constexpr SDL_Color kControlDisabledFill{205, 205, 205, 255};
inline constexpr SDL_Color kControlDisabledHighlight{222, 222, 222, 255};
inline constexpr SDL_Color kControlDisabledShadow{150, 150, 150, 255};
inline constexpr SDL_Color kControlDisabledText{160, 160, 160, 255};
// Tint applied to the PICT slider arrows when their control is locked.
inline constexpr std::uint8_t kDisabledArrowTint = 150;

// Native slider-arrow PICTs (STR# 0x86/0x87).
inline constexpr std::uint16_t kSoundArrowUpPict = 0x0086;
inline constexpr std::uint16_t kSoundArrowDownPict = 0x0087;

// Redraws the owning screen before a modal is composited. With no callback it
// clears to black (the port's stand-in for the retained owner surface).
// DIVERGENCE: the original retains the owner surface and blits every modal
// surface bottom-to-top; the port redraws the owner each frame instead.
inline void DrawOwningScreen(SdlPlatform &platform,
                             const std::function<void()> &render_background) {
  SDL_Renderer *const renderer = platform.renderer();
  if (render_background) {
    render_background();
  } else {
    platform.SetPlacement(PlaceWindow(platform.logical_playfield_size()));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
  }
}

// Centres a window box (def->right-left x def->bottom-top) on the playfield,
// truncating the half-offset toward zero to match Dialog_CreateFromDlog
// (FUN_008730a1) integer arithmetic.
[[nodiscard]] inline SDL_FRect
CenterWindowOnPanel(const SDL_FRect &panel, float win_w, float win_h) {
  (void)panel;
  return SDL_FRect{0.0F, 0.0F, win_w, win_h};
}

// Draws the 17x17 checkbox and label used by UiWindow_Draw. The original uses
// the same four grayscale RGBColor triples as its push buttons.
inline void DrawCheckBox(SdlPlatform &platform,
                         NovaFontCache &font_cache,
                         const SDL_FRect &box,
                         std::string_view label,
                         bool checked,
                         bool disabled) {
  SDL_Renderer *renderer = platform.renderer();
  const SDL_Color fill = disabled ? kControlDisabledFill : kControlFill;
  const SDL_Color highlight =
      disabled ? kControlDisabledHighlight : kControlHighlight;
  const SDL_Color shadow = disabled ? kControlDisabledShadow : kControlShadow;
  const SDL_Color text = disabled ? kControlDisabledText : kControlText;
  const SDL_Color check = disabled ? kControlDisabledText : kWindowFill;
  const SDL_FRect glyph{box.x, box.y, 17.0F, 17.0F};
  // Checked state inverts the bevel (dark top/left, light bottom/right),
  // exactly as UiWindow_Draw 0x004d0d00 does for DITL control types 5/6.
  const SDL_Color bevel_highlight = checked ? shadow : highlight;
  const SDL_Color bevel_shadow = checked ? highlight : shadow;
  DrawControlBevel(renderer, glyph, fill, bevel_highlight, bevel_shadow);
  SDL_SetRenderDrawColor(renderer,
                         kWindowFrame.r,
                         kWindowFrame.g,
                         kWindowFrame.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &glyph);
  if (checked) {
    // Checked mark is an X across the glyph square, inset by 3 on each side
    // (both diagonals; UiWindow_Draw 0x004d0d00).
    SDL_SetRenderDrawColor(
        renderer, check.r, check.g, check.b, SDL_ALPHA_OPAQUE);
    SDL_RenderLine(renderer,
                   glyph.x + 3.0F,
                   glyph.y + 3.0F,
                   glyph.x + 14.0F,
                   glyph.y + 14.0F);
    SDL_RenderLine(renderer,
                   glyph.x + 14.0F,
                   glyph.y + 3.0F,
                   glyph.x + 3.0F,
                   glyph.y + 14.0F);
  }
  const float baseline = box.y + box.h / 2.0F + 5.0F;
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                text,
                box.x + 20.0F,
                baseline,
                label);
}

// Draws one of the small slider arrows (pointing up in the upper cell, down in
// the lower), as a small filled triangle to keep it font-independent.
inline void DrawSliderArrow(SdlPlatform &platform,
                            NovaFontCache &,
                            const SDL_FRect &box,
                            bool up,
                            bool disabled) {
  const float cx = box.x + box.w / 2.0F;
  const float cy = box.y + box.h / 2.0F;
  const float base = 4.0F; // halfwidth of the triangle base
  const float h = 6.0F;    // triangle height
  const float top_y = up ? cy - h / 2.0F : cy + h / 2.0F;
  // Fill the triangle as horizontal strips, thinnest at the tip, widest at the
  // base.
  const SDL_Color arrow = disabled ? kControlDisabledText : kControlText;
  SDL_SetRenderDrawColor(
      platform.renderer(), arrow.r, arrow.g, arrow.b, SDL_ALPHA_OPAQUE);
  const int rows = 4;
  for (int i = 0; i < rows; ++i) {
    const float t =
        (up ? static_cast<float>(i) : static_cast<float>(rows - 1 - i)) /
        static_cast<float>(rows - 1);
    const float yy =
        up ? top_y + static_cast<float>(i) : top_y - static_cast<float>(i);
    const float half = base * t;
    const float x0 = std::max(cx - half, box.x);
    const float x1 = std::min(cx + half, box.x + box.w);
    if (x1 > x0) {
      const SDL_FRect strip{x0, yy, x1 - x0, 1.0F};
      SDL_RenderFillRect(platform.renderer(), &strip);
    }
  }
}

// Draws a bordered push button with its centred label.
inline void DrawButton(SdlPlatform &platform,
                       NovaFontCache &font_cache,
                       const SDL_FRect &box,
                       std::string_view label,
                       bool highlighted) {
  SDL_Renderer *renderer = platform.renderer();
  const SDL_Color highlight = highlighted ? kControlShadow : kControlHighlight;
  const SDL_Color shadow = highlighted ? kControlHighlight : kControlShadow;
  DrawControlBevel(renderer, box, kControlFill, highlight, shadow);
  SDL_SetRenderDrawColor(renderer,
                         kWindowFrame.r,
                         kWindowFrame.g,
                         kWindowFrame.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &box);
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        12.0F,
                        kNovaFontStyleRegular,
                        highlighted ? kControlSelectedText : kControlText,
                        box.x + 4.0F,
                        box.x + box.w - 4.0F,
                        box.y + box.h / 2.0F + 4.0F,
                        label);
}

struct SettingsArtwork {
  std::unique_ptr<SdlTexture> arrow_up;
  std::unique_ptr<SdlTexture> arrow_down;
};

// Loads the native slider-arrow PICTs (defined in preferences.cpp).
[[nodiscard]] SettingsArtwork LoadSettingsArtwork(SdlPlatform &platform);

} // namespace game::preferences_detail
