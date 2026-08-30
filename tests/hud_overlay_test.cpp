// Unit tests for the STR# string-pool decoder backing the HUD overlay /
// landing-feedback text (src/game/hud_overlay.cpp). The decode is a pure
// data-loading routine, so its format handling is tested here with known-final
// fixtures (big-endian u16 count, then length-prefixed entries), matching the
// EV Nova STR# pool layout parsed by the archival tooling.

#include "../src/game/hud_overlay.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

namespace game {
namespace {

// Builds a raw STR# pool payload from a list of string literals.
std::vector<std::byte> MakePool(std::initializer_list<const char *> items) {
  std::vector<std::byte> out;
  // Big-endian u16 count.
  const std::uint16_t count = static_cast<std::uint16_t>(items.size());
  out.push_back(static_cast<std::byte>((count >> 8) & 0xff));
  out.push_back(static_cast<std::byte>(count & 0xff));
  for (const char *s : items) {
    std::size_t len = 0;
    while (s[len] != '\0') {
      ++len;
    }
    out.push_back(static_cast<std::byte>(len));
    for (std::size_t i = 0; i < len; ++i) {
      out.push_back(static_cast<std::byte>(s[i]));
    }
  }
  return out;
}

TEST_CASE("STR# pool decodes length-prefixed entries and rejects malformed "
          "input",
          "[hud_overlay]") {
  // Happy path: big-endian count, then length-prefixed entries. Entry numbers
  // are 1-based, matching Resource_LoadStringEntry (0x004b8ca0).
  const auto pool =
      MakePool({"land on Planet", "dock at Station", "too far", "too fast"});
  REQUIRE(NovaHud_DecodeStringEntry(pool, 1) ==
          std::optional<std::string>("land on Planet"));
  REQUIRE(NovaHud_DecodeStringEntry(pool, 2) ==
          std::optional<std::string>("dock at Station"));
  REQUIRE(NovaHud_DecodeStringEntry(pool, 3) ==
          std::optional<std::string>("too far"));
  REQUIRE(NovaHud_DecodeStringEntry(pool, 4) ==
          std::optional<std::string>("too fast"));

  // Entry 0 (rejected by the original too) and out-of-range entries.
  CHECK(NovaHud_DecodeStringEntry(pool, 0) == std::nullopt);
  CHECK(NovaHud_DecodeStringEntry(pool, 5) == std::nullopt);
  CHECK(NovaHud_DecodeStringEntry(pool, 0xffff) == std::nullopt);

  // Empty pool (count 0) -> entry 1 rejected.
  std::vector<std::byte> empty{std::byte{0}, std::byte{0}};
  CHECK(NovaHud_DecodeStringEntry(empty, 1) == std::nullopt);

  // Truncated pool (no count) -> rejected.
  std::vector<std::byte> truncated{std::byte{'a'}};
  CHECK(NovaHud_DecodeStringEntry(truncated, 1) == std::nullopt);
}

} // namespace
} // namespace game
