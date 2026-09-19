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

TEST_CASE("STR resource decodes one Pascal string and rejects truncation",
          "[hud_overlay]") {
  const std::vector<std::byte> resource{std::byte{4},
                                        std::byte{'H'},
                                        std::byte{'a'},
                                        std::byte{'i'},
                                        std::byte{'l'},
                                        std::byte{0x7f}};
  CHECK(NovaResources_DecodeStringResource(resource) ==
        std::optional<std::string>("Hail"));

  const std::vector<std::byte> empty_string{std::byte{0}};
  CHECK(NovaResources_DecodeStringResource(empty_string) ==
        std::optional<std::string>(""));
  CHECK(NovaResources_DecodeStringResource({}) == std::nullopt);

  const std::vector<std::byte> truncated{std::byte{2}, std::byte{'x'}};
  CHECK(NovaResources_DecodeStringResource(truncated) == std::nullopt);
}

TEST_CASE("HUD overlay deadlines use the gameplay clock snapshot",
          "[hud_overlay][clock]") {
  GameState state;
  state.gameplay_now_ms = 1'000;
  NovaHud_ShowOverlayMessage(state, "test", std::uint64_t{10});
  CHECK(state.hud_overlay.active);
  CHECK(state.hud_overlay.expiry_ms == 1'210);

  state.gameplay_now_ms = 1'209;
  NovaHud_TickOverlay(state);
  CHECK(state.hud_overlay.active);
  state.gameplay_now_ms = 1'210;
  NovaHud_TickOverlay(state);
  CHECK_FALSE(state.hud_overlay.active);
}

TEST_CASE("system event message shows the STR# 1000 fallback",
          "[hud_overlay]") {
  // System_ShowSystemEventMessage (0x00467cf0) looks up sparse `STR ` id
  // message_id + 999, then falls back to STR# 1000 entry message_id (1-based).
  // The shipped pool's entry 1 is the Auroran message-buoy warning.
  GameState state;
  System_ShowSystemEventMessage(state, 1);
  REQUIRE(state.hud_overlay.active);
  CHECK(state.hud_overlay.message.find("Message Buoy") != std::string::npos);
  CHECK(state.hud_overlay.expiry_ms == 0x1e0ULL * 21ULL);

  // -1 (no Message field) shows nothing, mirroring the arrival branch.
  GameState no_message;
  System_ShowSystemEventMessage(no_message, -1);
  CHECK_FALSE(no_message.hud_overlay.active);
}

TEST_CASE("staged mission message takes priority over the launch departure",
          "[hud_overlay]") {
  GameState state;
  state.pending_overlay_message = "staged text";
  state.travel.travel_hint_state = -1;
  // stellar_id is unused on the pending path.
  NovaHud_ShowLaunchDepartureMessage(state, -1);
  CHECK(state.hud_overlay.active);
  CHECK(state.hud_overlay.message == "staged text");
  CHECK(state.hud_overlay.expiry_ms == 0x1f4ULL * 21ULL);
  CHECK(state.pending_overlay_message.empty());
  // The original jumps to the launch-tail epilogue, so the hint state is left
  // untouched instead of being reset to -1.
  CHECK(state.travel.travel_hint_state == -1);
}

} // namespace
} // namespace game
