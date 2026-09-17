#pragma once

// Button captions come from the shared game-strings pool STR# 0x96; several
// dialogs had grown their own copy of this loader with different fallback
// spellings. One helper, defaulting to "?".

#include "hud_overlay.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace game {

inline constexpr std::uint16_t kButtonLabelStr = 0x96;

[[nodiscard]] inline std::string
LoadButtonLabel(std::uint16_t entry, std::string_view fallback = "?") {
  return NovaHud_LoadStringEntry(kButtonLabelStr, entry)
      .value_or(std::string{fallback});
}

} // namespace game
