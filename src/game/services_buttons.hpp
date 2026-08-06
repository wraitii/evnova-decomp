#pragma once

// Clean-room reconstruction of the EV Nova three-state button used by the
// travel/boarding modal windows, mirroring NovaUi_InitThreeStateButtonArt
// (0x004a2f50) + NovaUi_DrawThreeStateButton (0x004a3340).
//
// The original pre-composites the six PICT button slices -- three left-edge
// tiles (ids 0x1d4c top, 0x1d4f mid, 0x1d52 bottom) and three right-edge
// tiles (0x1d4e, 0x1d51, 0x1d54) -- for each of the normal (0x1d4c..) and
// hover (0x1db0..) states, plus a disabled fallback, and draws a button by
// tiling those edges around the (empty) middle and centering the label text.
// This module reconstructs that on SDL: it loads the real slices as textures,
// renders a three-state button body into a target rect, and leaves the label
// glyph to the caller (the original draws the label via the shared text
// engine). Missing slices (the middle column / disabled variants) fall back to
// a solid fill matching the window backdrop, exactly as the game does with a
// 0xc x 0x18 solid rect.

#include <SDL3/SDL.h>

#include "../sdl_platform.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace game {

// A three-state button body. States follow the game's DrawThreeStateButton
// param_4/param_5 conventions: kNormal (idle), kHover (mouse over / focused),
// kDisabled. kPressed is treated as kHover by the original when not disabled.
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

  // Loads the six normal (0x1d4c..) and six hover (0x1db0..) edge PICTs into
  // textures. Slices that fail to decode (or are absent) become a solid fill
  // so the button still renders. Returns true when at least the normal edges
  // are usable.
  [[nodiscard]] bool Initialize(SdlPlatform &platform);

  // Draws a three-state button body into `rect` (logical 640x480 space).
  // `state` picks normal/hover/disabled edge art. The label is drawn by the
  // caller afterwards via the screen-font text engine so it stays selectable/
  // styled independently.
  void Draw(SdlPlatform &platform,
            const SDL_FRect &rect,
            ButtonState state) const;

  [[nodiscard]] bool usable() const { return usable_; }

  // Internal: the three vertical edge segments (0=top, 1=middle, 2=bottom) for
  // each of the left/right edges. Middle segments stretch between the fixed
  // corners; a null texture means "fill with the backdrop".
  struct EdgeTextures {
    std::unique_ptr<SdlTexture> left[3];
    std::unique_ptr<SdlTexture> right[3];
  };

private:
  EdgeTextures normal_;
  EdgeTextures hover_;
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
std::optional<std::uint8_t> ServiceButtonAt(const std::vector<ServiceButton> &buttons,
                                            SDL_FPoint point);

} // namespace game
