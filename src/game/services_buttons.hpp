#pragma once

// Clean-room reconstruction of the EV Nova three-state button used by the
// travel/boarding modal windows, mirroring NovaUi_InitThreeStateButtonArt
// (0x004a2f50) + NovaUi_DrawThreeStateButton (0x004a3340).
//
// The button is a 3-piece horizontal strip: a fixed left cap (13px), a 2px-wide
// middle tile that stretches horizontally to fill the body, and a fixed right
// cap (13px), all 25px tall. Each of the three visual states has its own strip:
//
//    state   PICT ids (left, middle, right)   resource-map display name
//    normal  0x1d4c, 0x1d4d, 0x1d4e          "Button Left/Middle/Right Bright"
//    pressed 0x1d4f, 0x1d50, 0x1d51          "click left/middle/right"
//    grey    0x1d52, 0x1d53, 0x1d54          "grey left/middle/right"
//
//  0x1d4c..0x1d54 and the mask set 0x1db0.. come straight from Nova Graphics
//  3.rez and the nine consecutive PICT loads in NovaUi_InitThreeStateButtonArt
//  (0x1d4c + 0..8). The down/disabled variant ids are *not* vertical slices of
//  one button: 0x1d4f ("click left") and 0x1d52 ("grey left") begin separate
//  state strips. NovaUi_DrawThreeStateButton selects one whole strip by state
//  index (param_4/param_5: 0,0 -> normal; 0,!0 -> pressed; !0 -> grey) and
//  stretches that state's 2px middle across the body. Each cap uses its 1-bit
//  mask PICT (white = transparent), so the outside of the rounded corners
//  is fully transparent; the middle tile is unmasked/opaque. This module loads
//  the real strips, draws the body by left cap + stretched middle + right cap,
//  applies the cap masks, and leaves the label glyph to the caller. Missing
//  strips fall back to a solid fill matching the window backdrop, as the game
//  does with its 0xc x 0x18 solid rect.

#include <SDL3/SDL.h>

#include "../sdl_platform.hpp"
#include "nova_font.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace game {

// Nova Graphics 3's first c\x9alr record configures the shared button labels
// as Charcoal 12 (record offsets +0x9e and +0xde). In the reconstructed font
// family table, the bundled Charcoal.ttf is the Chicago-family screen face.
inline constexpr NovaFontFamily kThreeStateButtonFontFamily =
    NovaFontFamily::kChicago;
inline constexpr float kThreeStateButtonFontSize = 12.0F;

// NovaUi_DrawThreeStateButton (0x004a3340) places the label baseline at the
// integer vertical midpoint of the button rect plus five logical pixels.
[[nodiscard]] float ThreeStateButtonLabelBaseline(const SDL_FRect &rect);

// Draws a three-state button's caption exactly as NovaUi_DrawThreeStateButton
// (0x004a3340) does: a leading '^', '&', '+' or '-' selects a 2px vector icon
// (down chevron, up chevron, plus, minus); anything else draws the whole
// string centred on the baseline. The string is the resolved STR# 0x96
// caption, so callers should not special-case those bytes themselves.
void DrawThreeStateButtonLabel(SdlPlatform &platform,
                               NovaFontCache &font_cache,
                               const SDL_FRect &rect,
                               std::string_view label,
                               const SDL_Color &color);

// A three-state button body. States follow the game's DrawThreeStateButton
// param_4/param_5 conventions: kNormal (idle), kHover (mouse over / focused;
// uses the "click"/pressed art), kDisabled (uses the "grey" art).
enum class ButtonState : std::uint8_t { kNormal, kHover, kDisabled };

// Loads and caches the three-state button edge slices. Kept on a per-window
// basis so repeated dockings do not re-decode; resources live in the Nova
// Graphics archives.
class ServicesButtonArt {
public:
  ServicesButtonArt() = default;
  ~ServicesButtonArt();

  ServicesButtonArt(const ServicesButtonArt &) = delete;
  ServicesButtonArt &operator=(const ServicesButtonArt &) = delete;

  // Loads the three strips' PICTs into textures: normal (0x1d4c..0x1d4e),
  // pressed/hover (0x1d4f..0x1d51) and grey/disabled (0x1d52..0x1d54), each
  // with its left 13px cap, 2px middle tile and right 13px cap. Pieces that
  // fail to decode (or are absent) become a solid fill so the button still
  // renders. Returns true when at least the normal strip is usable.
  [[nodiscard]] bool Initialize(SdlPlatform &platform);

  // Draws a three-state button body into `rect` (logical 640x480 space).
  // `state` picks normal/hover/disabled strip art. The body is drawn as the
  // left cap, the 2px middle tile stretched across the remaining width, and
  // the right cap (native 25px corner height). The label is drawn by the
  // caller afterwards via the screen-font text engine.
  void
  Draw(SdlPlatform &platform, const SDL_FRect &rect, ButtonState state) const;

  [[nodiscard]] bool usable() const { return usable_; }

  // The three horizontal pieces (0=left cap, 1=middle tile, 2=right cap) of one
  // button strip. A null texture means "fill with the backdrop".
  struct StripPieces {
    std::unique_ptr<SdlTexture> left;
    std::unique_ptr<SdlTexture> middle;
    std::unique_ptr<SdlTexture> right;
  };

private:
  StripPieces normal_;
  StripPieces pressed_;
  StripPieces grey_;
  bool usable_ = false;
};

// A labelled desk button: a body rect (identical for every state so the hit
// test is stable) plus the accessible service it triggers.
struct ServiceButton {
  SDL_FRect rect;
  // 0-based service slot (index into the window's service list).
  std::uint8_t slot = 0;
};

// Returns the service button whose rect contains the logical point, or
// std::nullopt. Mirrors the original hit-testing a clicked service button.
std::optional<std::uint8_t>
ServiceButtonAt(const std::vector<ServiceButton> &buttons, SDL_FPoint point);

} // namespace game
