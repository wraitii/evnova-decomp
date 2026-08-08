#include "hud_overlay.hpp"

#include "../brgr_archive.hpp"

#include <SDL3/SDL.h>

namespace game {
namespace {
// The STR# resource type code (0x53545223, "STR#").
inline constexpr std::uint32_t kStringResourceType = 0x53545223U;
} // namespace

void NovaHud_ShowOverlayMessage(GameState &state,
                                std::string message,
                                std::uint8_t red,
                                std::uint8_t green,
                                std::uint8_t blue,
                                std::uint64_t duration_ms) {
  state.hud_overlay.active = true;
  state.hud_overlay.message = std::move(message);
  state.hud_overlay.red = red;
  state.hud_overlay.green = green;
  state.hud_overlay.blue = blue;
  state.hud_overlay.expiry_ms = SDL_GetTicks() + duration_ms;
}

void NovaHud_ShowCachedOverlayMessage(GameState &state, bool extend) {
  if (!state.hud_overlay.active || state.hud_overlay.message.empty()) {
    return;
  }
  // A cached re-show just keeps a live message on screen a moment longer (the
  // original replays the same buffered text after a refresh pass).
  if (extend) {
    const std::uint64_t lifetime =
        state.hud_overlay.expiry_ms > SDL_GetTicks()
            ? state.hud_overlay.expiry_ms - SDL_GetTicks()
            : 250U;
    state.hud_overlay.expiry_ms = SDL_GetTicks() + lifetime;
  }
}

void NovaHud_ClearOverlayMessage(GameState &state) {
  state.hud_overlay.active = false;
  state.hud_overlay.message.clear();
  state.hud_overlay.red = 0xe0;
  state.hud_overlay.green = 0xe0;
  state.hud_overlay.blue = 0xe0;
  state.hud_overlay.expiry_ms = 0;
}

void NovaHud_TickOverlay(GameState &state) {
  if (state.hud_overlay.active && state.hud_overlay.expiry_ms != 0 &&
      SDL_GetTicks() >= state.hud_overlay.expiry_ms) {
    NovaHud_ClearOverlayMessage(state);
  }
}

std::optional<std::string>
NovaHud_DecodeStringEntry(std::span<const std::byte> pool,
                          std::uint16_t index) {
  if (pool.size() < 2) {
    return std::nullopt;
  }
  const std::uint16_t count = (static_cast<std::uint16_t>(pool[0]) << 8) |
                              static_cast<std::uint16_t>(pool[1]);
  if (index >= count) {
    return std::nullopt;
  }
  std::size_t p = 2;
  for (std::uint16_t i = 0; i < index; ++i) {
    if (p >= pool.size()) {
      return std::nullopt;
    }
    const std::size_t len = static_cast<std::size_t>(pool[p]);
    if (p + 1U + len > pool.size()) {
      return std::nullopt;
    }
    p += 1U + len;
  }
  if (p >= pool.size()) {
    return std::nullopt;
  }
  const std::size_t len = static_cast<std::size_t>(pool[p]);
  p += 1U;
  if (p + len > pool.size()) {
    return std::nullopt;
  }
  std::string out;
  out.resize(len);
  for (std::size_t i = 0; i < len; ++i) {
    out[i] = static_cast<char>(pool[p + i]);
  }
  return out;
}

std::optional<std::string> NovaHud_LoadStringEntry(std::uint16_t resource_id,
                                                   std::uint16_t index) {
  const auto bytes = NovaResource_Load(kStringResourceType, resource_id);
  if (!bytes) {
    return std::nullopt;
  }
  return NovaHud_DecodeStringEntry(*bytes, index);
}

namespace {
// STR# 0x7d2 (landing/docking feedback) entry indices, from
// Stellar_ProcessTravelAndLanding.
inline constexpr std::uint16_t kStrId = 0x7d2;
inline constexpr std::uint16_t kTooFarStation = 0x43;
inline constexpr std::uint16_t kTooFarPlanet = 0x44;
inline constexpr std::uint16_t kTooFastStation = 0x47;
inline constexpr std::uint16_t kTooFastPlanet = 0x48;
inline constexpr std::uint16_t kNoCredits = 0x3d;  // "You don't have enough"
inline constexpr std::uint16_t kPayDockFee = 0x3f; // "to pay the docking fee."
inline constexpr std::uint16_t kPayLandFee = 0x40; // "to pay the landing fee."
} // namespace

void NovaHud_ShowLandingDenial(GameState &state,
                               LandedDenial denial,
                               bool is_station) {
  std::optional<std::string> text;
  switch (denial) {
  case LandedDenial::kNone:
    return;
  case LandedDenial::kUnavailable:
    text = NovaHud_LoadStringEntry(
        kStrId,
        is_station ? static_cast<std::uint16_t>(0x57) // "dock at ..."
                   : static_cast<std::uint16_t>(0x58) // "land on ..."
    );
    break;
  case LandedDenial::kTooFar:
    text = NovaHud_LoadStringEntry(kStrId,
                                   is_station ? kTooFarStation : kTooFarPlanet);
    break;
  case LandedDenial::kTooFast:
    text = NovaHud_LoadStringEntry(
        kStrId, is_station ? kTooFastStation : kTooFastPlanet);
    break;
  case LandedDenial::kTooExpensive:
    text = NovaHud_LoadStringEntry(kStrId, kNoCredits);
    if (text) {
      text->append(" ");
      if (const auto fee = NovaHud_LoadStringEntry(
              kStrId, is_station ? kPayDockFee : kPayLandFee)) {
        text->append(*fee);
      }
    }
    break;
  }
  if (!text || text->empty()) {
    // Fall back to a plain English phrase when the STR# pool is unavailable.
    text = std::string("Unable to land here.");
  }
  NovaHud_ShowOverlayMessage(state, *text, 0xe0, 0xe0, 0xe0, 360U);
}

} // namespace game
