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
// simulation ticks: the original stores param_2 in g_hud_overlay_msg_color,
// which doubles as the countdown (Frame_UpdateScreenFlashTimers 0x0042f1b0,
// called from Frame_TickSystems, decrements it once per 30 Hz tick and
// clears the message at zero). The port converts ticks to wall-clock ms at
// the original's 30 Hz cadence (its own loop renders at ~60 Hz but ticks the
// sim on the same 30 Hz basis). Board denials pass 0x168 and the
// boarding/plunder window's loot overlays pass 0xf0; landing feedback passes
// 0xfa. The text is drawn at the bottom of the flight viewport. The `color`
// is the on-screen RGB tint of the message text.
void NovaHud_ShowOverlayMessage(GameState &state,
                                std::string message,
                                std::uint8_t red = 0xe0,
                                std::uint8_t green = 0xe0,
                                std::uint8_t blue = 0xe0,
                                std::uint64_t duration_frames = 250);

// Duration-only form: the message overlays driven by gameplay events
// (mission failure/disable/destruction STR# 0x7d2 0x11c/0x11d/0x11f/0x120)
// pass a duration (0xf0/0x168/5000) and the shared g_hud_overlay_text_color,
// which the port renders with the default tint.
void NovaHud_ShowOverlayMessage(GameState &state,
                                std::string message,
                                std::uint64_t duration_frames);

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
// NovaResource_Load(0x53545223, resource_id) and `entry` is the 1-BASED entry
// number, exactly like the original helper (it walks entry-1 length-prefixed
// strings from the big-endian u16 count header; entry 0 and entries past the
// count select nothing). Returns the entry, or std::nullopt when the
// pool/entry is missing. Used to build the landing/negotiation feedback text
// (STR# 0x7d2).
[[nodiscard]] std::optional<std::string>
NovaHud_LoadStringEntry(std::uint16_t resource_id, std::uint16_t entry);

// Pure STR# pool decoder (exposed for the data-loading unit test): given the
// raw pool payload (big-endian u16 string count, then per-entry 1-byte length
// + bytes), returns the 1-BASED entry `entry` or std::nullopt when the entry
// is 0, out of range, or the pool is malformed (the original yields an empty
// string in those cases; nullopt lets callers keep their fallback text).
[[nodiscard]] std::optional<std::string>
NovaHud_DecodeStringEntry(std::span<const std::byte> pool, std::uint16_t entry);

// Composes and shows the on-screen HUD overlay for a denied landing request,
// mirroring the Stellar_ProcessTravelAndLanding feedback cases (STR# 0x7d2).
// `denial` is the reason NovaLanding_EnterDocked reported; `is_station` picks
// the dock-vs-land phrasing (travel_flags & 0x10 on the target stellar).
void NovaHud_ShowLandingDenial(GameState &state,
                               LandedDenial denial,
                               bool is_station);

} // namespace game
