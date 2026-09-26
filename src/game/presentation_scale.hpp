#pragma once

// Port-only presentation multipliers. The original game has no user-facing
// scale controls; these let the SDL port scale UI,
// the flight scene and mission dialogs independently. They are presentation
// choices, not game-logic changes, so they are not gated through the original
// bug-fix compatibility switch.

#include <cmath>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace game {

// Supported range for a single multiplier. Values outside this range, or
// malformed ones, fall back to 1.0 at the call site.
inline constexpr float kPresentationScaleMin = 0.5F;
inline constexpr float kPresentationScaleMax = 4.0F;

// Parses one multiplier. Expects already-trimmed input and accepts only a
// complete, finite decimal in [kPresentationScaleMin, kPresentationScaleMax];
// rejects surrounding whitespace, trailing junk, NaN,
// infinity, zero, negatives and out-of-range values. Returns nullopt on
// rejection so the caller can warn and fall back. Uses the classic locale so
// the decimal separator never depends on the host locale (Apple libc++ does
// not yet provide floating-point std::from_chars).
[[nodiscard]] inline std::optional<float>
PresentationScale_Parse(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  // Callers trim, so require an already-trimmed value rather than relying on
  // the stream's leading-whitespace skip.
  constexpr std::string_view kWhitespace = " \t\r\n";
  if (kWhitespace.find(text.front()) != std::string_view::npos ||
      kWhitespace.find(text.back()) != std::string_view::npos) {
    return std::nullopt;
  }
  std::istringstream in{std::string{text}};
  in.imbue(std::locale::classic());
  float value = 0.0F;
  in >> value;
  if (in.fail() || !in.eof()) {
    return std::nullopt;
  }
  if (!std::isfinite(value) || value < kPresentationScaleMin ||
      value > kPresentationScaleMax) {
    return std::nullopt;
  }
  return value;
}

// The three independent multipliers. All default to 1.0 (native geometry).
struct PresentationScale {
  // General UI scale: menus, fixed screens, dialogs, HUD chrome, overlays.
  float ui = 1.0F;
  // Flight-scene scale: world rendering only; UI is unaffected.
  float flight_scene = 1.0F;
  // Mission-dialog multiplier applied on top of `ui` at the two mission
  // entrypoints only.
  float mission = 1.0F;

  // Composed request for the mission offer/variant and mission info dialogs:
  // authored UI (so they follow `ui`) with the mission multiplier on top,
  // composed before the single fit clamp in PlaceContained/PlaceCenteredIn.
  [[nodiscard]] float mission_dialog() const { return ui * mission; }
};

} // namespace game
