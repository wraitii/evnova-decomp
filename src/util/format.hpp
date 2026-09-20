#pragma once

// Shared text formatting helpers.

#include <cstdint>
#include <string>
#include <string_view>

namespace evnova::util {
namespace detail {

// Inserts a comma every three digits, walking the string from the left. The
// sign character (if any) participates in the grouping exactly as the
// original DrawContext_DrawGroupedUInt does.
[[nodiscard]] inline std::string GroupDigits(std::string digits) {
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) {
      out.push_back(',');
    }
    out.push_back(digits[i]);
  }
  return out;
}

} // namespace detail

// Ghidra DrawContext_DrawGroupedUInt: decimal digits grouped in threes with
// commas, e.g. 26600 -> "26,600".
[[nodiscard]] inline std::string GroupThousands(std::int32_t value) {
  return detail::GroupDigits(std::to_string(value));
}

[[nodiscard]] inline std::string GroupThousands(std::uint32_t value) {
  return detail::GroupDigits(std::to_string(value));
}

// Capitalizes a leading ASCII letter, matching the MetroWerks C-locale
// MWRuntime_ToUpper (Ghidra 0x004d6260): only 'a'..'z' change and every other
// byte, including the >= 0x80 range, is left alone. Several original UI paths
// run the first character of a label or count word through that runtime call.
[[nodiscard]] inline std::string UpperFirstAscii(std::string text) {
  if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') {
    text[0] = static_cast<char>(text[0] - 'a' + 'A');
  }
  return text;
}

} // namespace evnova::util
