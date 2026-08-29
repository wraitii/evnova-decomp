#pragma once

// Clean-room reconstruction of the transient on-screen HUD overlay message
// (NovaHud_ShowOverlayMessage 0x0047e2d0 / NovaHud_ShowCachedOverlayMessage
// 0x0047e430). The original keeps the last message text in
// g_hud_overlay_msg_buffer, marks it present by g_hud_overlay_msg_color > 0,
// draws it into the fixed message rect (g_hud_overlay_message_rect) over the
// flight viewport, and replays it after refresh passes. This module is pure
// game logic: it only arms GameState::hud_overlay, and the HudRenderer paints
// it. It is used for the flight/travel/landing feedback text (STR# 0x7d2) and
// the destination-bribe confirmation toast.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "game_state.hpp"
#include "landed_window.hpp"

namespace game {

// Mirrors NovaHud_ShowOverlayMessage: arms the transient HUD overlay message.
// Replaces any current message. `duration_frames` is the message lifetime in
// rendered frames: the original stores param_2 in g_hud_overlay_msg_color,
// which doubles as the countdown (Frame_UpdateScreenFlashTimers 0x0042f1b0
// decrements it once per frame and clears the message at zero). The port
// converts frames at the original's ~60 Hz render cadence. Board denials pass
// 0x168 and the boarding/plunder window's loot overlays pass 0xf0; landing
// feedback passes 0xfa. The text is drawn at the bottom of the flight
// viewport. The `color` is the on-screen RGB tint of the message text.
void NovaHud_ShowOverlayMessage(GameState &state,
                                std::string message,
                                std::uint8_t red = 0xe0,
                                std::uint8_t green = 0xe0,
                                std::uint8_t blue = 0xe0,
                                std::uint64_t duration_frames = 250);

// Mirrors NovaHud_ShowCachedOverlayMessage: re-arms the last shown message for
// repaint (the original re-displays g_hud_overlay_msg_buffer after a
// refresh/system pass). Does nothing when no message is cached/active. When
// `extend` is true the message's expiry is pushed out by its original
// duration (used by the dialog loop to keep the confirmation toast on screen).
void NovaHud_ShowCachedOverlayMessage(GameState &state, bool extend);

// Convenience: any currently-showing overlay message is cleared immediately.
void NovaHud_ClearOverlayMessage(GameState &state);

// Per-frame tick: clears any overlay message whose wall-clock expiry has
// passed (called once per spaceflight frame so the draw path can stay const
// over GameState).
void NovaHud_TickOverlay(GameState &state);

// Loads one entry from a STR# string pool (Ghidra Resource_LoadStringEntry
// 0x004b8ca0). The pool is fetched from the archives via
// NovaResource_Load(0x53545223, resource_id) and entry `index` (0-based) is
// decoded by the STR# format (big-endian u16 count, then length-prefixed
// strings). Returns the entry, or std::nullopt when the pool/index is missing.
// Used to build the landing/negotiation feedback text (STR# 0x7d2).
[[nodiscard]] std::optional<std::string>
NovaHud_LoadStringEntry(std::uint16_t resource_id, std::uint16_t index);

// Pure STR# pool decoder (exposed for the data-loading unit test): given the
// raw pool payload (big-endian u16 string count, then per-entry 1-byte length
// + bytes), returns entry `index` (0-based) or std::nullopt when out of range
// or malformed.
[[nodiscard]] std::optional<std::string>
NovaHud_DecodeStringEntry(std::span<const std::byte> pool, std::uint16_t index);

// Composes and shows the on-screen HUD overlay for a denied landing request,
// mirroring the Stellar_ProcessTravelAndLanding feedback cases (STR# 0x7d2).
// `denial` is the reason NovaLanding_EnterDocked reported; `is_station` picks
// the dock-vs-land phrasing (travel_flags & 0x10 on the target stellar).
void NovaHud_ShowLandingDenial(GameState &state,
                               LandedDenial denial,
                               bool is_station);

} // namespace game
