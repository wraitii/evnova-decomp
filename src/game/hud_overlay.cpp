#include "hud_overlay.hpp"

#include "../brgr_archive.hpp"
#include "mission.hpp"

#include <SDL3/SDL.h>

#include <random>

namespace game {

// Ghidra 0x0047e2d0 NovaHud_ShowOverlayMessage. The original stores param_2
// (a tick countdown) in g_hud_overlay_msg_color, which
// Frame_TickHudOverlayAndRouteMapTimers (0x0042f1b0) decrements it once per
// raw Frame_TickSystems call, clearing the message when it reaches zero. The
// original flight loop admits one such call per 21 ms at its maximum cadence;
// the port uses that reference duration while keeping expiry independent of
// the display refresh rate.
constexpr std::uint64_t kOverlayTickMs = 21U;

void NovaHud_ShowOverlayMessage(GameState &state,
                                std::string message,
                                std::uint8_t red,
                                std::uint8_t green,
                                std::uint8_t blue,
                                std::uint64_t duration_frames) {
  state.hud_overlay.active = true;
  state.hud_overlay.message = std::move(message);
  state.hud_overlay.red = red;
  state.hud_overlay.green = green;
  state.hud_overlay.blue = blue;
  state.hud_overlay.expiry_ms =
      state.gameplay_now_ms + duration_frames * kOverlayTickMs;
}

// Duration-only overload: see hud_overlay.hpp.
void NovaHud_ShowOverlayMessage(GameState &state,
                                std::string message,
                                std::uint64_t duration_frames) {
  NovaHud_ShowOverlayMessage(
      state, std::move(message), 0xe0, 0xe0, 0xe0, duration_frames);
}

// Ghidra 0x0047e430 NovaHud_ShowCachedOverlayMessage.
void NovaHud_ShowCachedOverlayMessage(GameState &state, bool extend) {
  if (!state.hud_overlay.active || state.hud_overlay.message.empty()) {
    return;
  }
  // A cached re-show just keeps a live message on screen a moment longer (the
  // original replays the same buffered text after a refresh pass).
  if (extend) {
    const std::uint64_t lifetime =
        state.hud_overlay.expiry_ms > state.gameplay_now_ms
            ? state.hud_overlay.expiry_ms - state.gameplay_now_ms
            : 250U * kOverlayTickMs;
    state.hud_overlay.expiry_ms = state.gameplay_now_ms + lifetime;
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
      state.gameplay_now_ms >= state.hud_overlay.expiry_ms) {
    NovaHud_ClearOverlayMessage(state);
  }
}

std::optional<std::string>
NovaHud_DecodeStringEntry(std::span<const std::byte> pool,
                          std::uint16_t entry) {
  if (pool.size() < 2) {
    return std::nullopt;
  }
  const std::uint16_t count = (static_cast<std::uint16_t>(pool[0]) << 8) |
                              static_cast<std::uint16_t>(pool[1]);
  if (entry == 0 || entry > count) {
    return std::nullopt;
  }
  std::size_t p = 2;
  for (std::uint16_t i = 1; i < entry; ++i) {
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
                                                   std::uint16_t entry) {
  const auto bytes = NovaResource_Load(kResourceTypeStringTable, resource_id);
  if (!bytes) {
    return std::nullopt;
  }
  return NovaHud_DecodeStringEntry(*bytes, entry);
}

std::uint16_t NovaHud_StringPoolEntryCount(std::uint16_t resource_id) {
  const auto bytes = NovaResource_Load(kResourceTypeStringTable, resource_id);
  if (!bytes || bytes->size() < 2) {
    return 0;
  }
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint8_t>((*bytes)[0]) << 8U) |
      std::to_integer<std::uint8_t>((*bytes)[1]));
}

std::optional<std::string>
NovaResources_DecodeStringResource(std::span<const std::byte> resource) {
  if (resource.empty()) {
    return std::nullopt;
  }
  const auto length = static_cast<std::size_t>(resource.front());
  if (length > resource.size() - 1U) {
    return std::nullopt;
  }

  std::string out(length, '\0');
  for (std::size_t i = 0; i < length; ++i) {
    out[i] = static_cast<char>(resource[i + 1U]);
  }
  return out;
}

// Ghidra 0x004c73b0 NovaResources_CopyStringResource.
std::optional<std::string>
NovaResources_LoadStringResource(std::uint16_t resource_id) {
  const auto bytes = NovaResource_Load(kResourceTypeString, resource_id);
  if (!bytes) {
    return std::nullopt;
  }
  return NovaResources_DecodeStringResource(*bytes);
}

std::optional<std::string>
NovaResources_LoadPatchedStringEntry(std::uint16_t fallback_pool,
                                     std::uint16_t entry,
                                     std::uint16_t override_base) {
  if (entry == 0) {
    return std::nullopt;
  }
  if (auto patched =
          NovaResources_LoadStringResource(static_cast<std::uint16_t>(
              override_base + static_cast<std::uint16_t>(entry - 1U)))) {
    return patched;
  }
  return NovaHud_LoadStringEntry(fallback_pool, entry);
}

namespace {
// STR# 0x7d2 (landing/docking feedback) entry numbers, exactly as the original
// passes them to Resource_LoadStringEntry (1-based;
// Stellar_HandleStellarEntryAndExit 0x00457580). pool content: 0x3c "You don't
// have enough", 0x3e "to pay the docking fee.", 0x3f "to pay the landing fee.",
// 0x42/0x43 too-far station/planet, 0x46/0x47 too-fast station/planet, 0x56
// "dock at ", 0x57 "land on ".
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
    // TODO(decomp): the original composes the 0x54 lead-in ("Your ship is
    // unable to") with the 0x55/0x56 wormhole/hypergate variants too; the port
    // shows only the dock/land fragment.
    text = NovaHud_LoadStringEntry(
        kStrId,
        is_station ? static_cast<std::uint16_t>(0x57) // "dock at "
                   : static_cast<std::uint16_t>(0x58) // "land on "
    );
    break;
  case LandedDenial::kUnauthorized:
    // Stellar_HandleStellarEntryAndExit: STR# 0x7d2 entry 0x52 for stations,
    // 0x53 for planets (0x51 is reserved for denied hypergates).
    text =
        NovaHud_LoadStringEntry(kStrId,
                                is_station ? static_cast<std::uint16_t>(0x52)
                                           : static_cast<std::uint16_t>(0x53));
    break;
  case LandedDenial::kCloaked:
    // Stellar_HandleStellarEntryAndExit 0x004587xx: a ship at/inside the cloak
    // visibility threshold cannot land; entry 0x49 is "Disengage cloaking
    // device first."
    text = NovaHud_LoadStringEntry(kStrId, 0x49)
               .value_or("Disengage cloaking device first.");
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
  // 0x168 raw flight calls, the original's recorded overlay duration for
  // landing
  // feedback (Stellar_HandleStellarEntryAndExit).
  NovaHud_ShowOverlayMessage(state, *text, 0xe0, 0xe0, 0xe0, 0x168U);
}

// Ghidra Stellar_RunDockAndLaunchSequence tail block (0x00455e10, 0x00456323):
namespace {
// STR# 0x7d2 launch-departure pool: 0x37..0x3b are the five "leaving" lead
// variants, 0x3c is the "on" connector before the date.
inline constexpr std::uint16_t kLaunchMsgStrId = 0x7d2;
inline constexpr std::uint16_t kLaunchOn = 0x3c;
} // namespace

// <variant> <stellar> on <date>." runs after the destination-interaction
// loop returns, i.e. as the player launches. The gate mirrors DAT_007cab1c:
// below 3 (fresh-pilot flight-tutorial hints still active) the message is
// skipped and the state resets to -1; at 0x7fff (latched by the jump
// hold-begin 0x0044c561, hyperspace arrival 0x0044f83f and ship resets) the
// message shows every launch and the state is left latched (the original's
// JMP LAB_00456158 skips the reset). When a pending overlay message was staged
// at landing, that buffer is shown instead (0x1f4 ticks) and the travel-hint
// state is left untouched.
void NovaHud_ShowLaunchDepartureMessage(GameState &state,
                                        std::int16_t stellar_id) {
  // Staged mission-script message wins over any departure overlay. The
  // original checks this buffer first (0x00456134), shows it for 0x1f4 ticks,
  // replays it cached, clears the first byte, and jumps straight to the launch
  // tail epilogue (0x00456158), so the travel-hint state is NOT reset.
  if (!state.pending_overlay_message.empty()) {
    NovaHud_ShowOverlayMessage(state,
                               state.pending_overlay_message,
                               static_cast<std::uint64_t>(0x1f4U));
    NovaHud_ShowCachedOverlayMessage(state, /*extend=*/true);
    state.pending_overlay_message.clear();
    return;
  }
  if (state.travel.travel_hint_state < 3) {
    state.travel.travel_hint_state = -1;
    return;
  }
  const std::uint16_t variant =
      0x37 + static_cast<std::uint16_t>(
                 std::uniform_int_distribution<int>{0, 4}(state.rng));
  std::string msg;
  if (const auto text = NovaHud_LoadStringEntry(kLaunchMsgStrId, variant)) {
    msg = *text;
  }
  msg += " ";
  if (const auto *stellar = state.scenario.Stellar(stellar_id)) {
    // StellarDef +0x48 display name, copied bounded to 0x3f bytes.
    msg += stellar->name.substr(0, 0x3f);
  }
  msg += " ";
  if (const auto text = NovaHud_LoadStringEntry(kLaunchMsgStrId, kLaunchOn)) {
    msg += *text;
  }
  msg += " ";
  // Stellar_FormatElapsedTravelTime shape: full month names (STR# 0x89
  // entries 1-12). The trailing chär/save-carried date suffix
  // (g_date_suffix, save block +0x5eee) is empty for stock pilots.
  msg += NovaText_FormatDateString(
      state.date, false, state.date_prefix, state.date_suffix);
  msg += ".";
  NovaHud_ShowOverlayMessage(state, msg, static_cast<std::uint64_t>(0xf0U));
  // The original re-arms the cached overlay (0x0045646e).
  NovaHud_ShowCachedOverlayMessage(state, /*extend=*/true);
}

// Ghidra 0x00467cf0 System_ShowSystemEventMessage.
void System_ShowSystemEventMessage(GameState &state, std::int16_t message_id) {
  if (message_id < 0) {
    return;
  }
  const auto entry = static_cast<std::uint16_t>(message_id);
  // Sparse `STR ` id message_id + 999 first, then STR# 1000 entry message_id
  // (see NovaResources_LoadPatchedStringEntry: override_base is the sparse id
  // for slot 1). The original's overlay duration is 0x1e0; the shared
  // g_hud_overlay_text_color tint maps to the port's default overlay colour.
  const auto text = NovaResources_LoadPatchedStringEntry(1000, entry, 1000);
  NovaHud_ShowOverlayMessage(
      state, text.value_or(std::string{}), static_cast<std::uint64_t>(0x1e0U));
}

} // namespace game
