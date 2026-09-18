#pragma once

// Clean-room name-display helpers shared by the scenario loader and the
// new-game pilot naming flow. These mirror the original Pascal-string name
// normalizers called on every loaded display name.

#include <string>
#include <string_view>

namespace game {

// Ghidra 0x004cd230 NameString_StripSubtitleSuffix. Finds the last ';' (0x3b),
// then scans left. A space lowers the cut point; a further ';' is skipped
// without moving it; the first other character ends the cut just after it. If
// no other character exists, the cut is the leftmost space's index (or the
// last ';' index when there is no space).
//   "Base Name;Subtitle " -> "Base Name"
//   "A ; ;X" -> "A",  "; ;X" -> ";",  ";;X" -> ";",  "Name;" -> "Name",
//   "   ;Sub" -> "",  "NoSemi  " -> "NoSemi  "
[[nodiscard]] inline std::string
NovaText_StripSubtitleSuffix(std::string_view name) {
  const auto semicolon = name.rfind(';');
  if (semicolon == std::string_view::npos) {
    return std::string{name};
  }
  std::size_t leftmost_space = std::string_view::npos;
  for (std::size_t i = semicolon; i-- > 0;) {
    const char c = name[i];
    if (c != ' ' && c != ';') {
      return std::string{name.substr(0, i + 1)};
    }
    if (c == ' ') {
      leftmost_space = i;
    }
  }
  const std::size_t length =
      leftmost_space == std::string_view::npos ? semicolon : leftmost_space;
  return std::string{name.substr(0, length)};
}

} // namespace game
