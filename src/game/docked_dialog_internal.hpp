#pragma once

// Shared internals for the docked sub-window dialogs. docked_dialog.cpp was
// split into one translation unit per service window (mission BBS, store,
// bar, trade center) plus the generic frame/orchestrator core; the small set
// of helpers and per-service entry points they share live here. This is a
// private implementation header, not part of the public game API.

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "../util/geometry.hpp"
#include "docked_dialog.hpp"
#include "pict_texture.hpp"

namespace game {

// The full-width Spaceport backdrop PICT (618x517) re-layered behind every
// sub-window dialog. See landed_window.cpp for the 0x2137 fallback when
// 0x2134 is missing.
inline constexpr std::uint16_t kDockedBackdropPict = 0x2134;

// Stellar / disaster resource ids are 0x80-based; the runtime tables are
// indexed from zero.
inline constexpr std::int16_t kResourceIdBase = 0x80;

// Dim body text shared by the generic sub-window hint and the trade-center
// rows.
inline constexpr SDL_Color kDim{192, 192, 192, 255};

// Shared rect helpers (evnova::util); every docked sub-window uses these
// unqualified.
using evnova::util::Contains;
using evnova::util::OffsetRect;

// Label/unit text from the game-strings pool STR# 0x7d2 (see the definition in
// docked_store_dialog.cpp for the 0-based pool index convention).
[[nodiscard]] std::string InfoString(std::uint16_t pool_index);

// Commodity name for the disaster report / trade-center banner, from STR# 0xfa1
// (defined in docked_bar_dialog.cpp).
[[nodiscard]] std::string BarCommodityName(std::int16_t commodity);

// The landed-store quantity prompt (DLOG 0x3eb); defined in
// docked_store_dialog.cpp and reused by the trade center.
[[nodiscard]] std::int16_t
RunStoreQuantityPrompt(SdlPlatform &platform,
                       std::int16_t max_quantity,
                       const std::function<void()> &render_background);

// The generic Outfitter/Shipyard store modal; defined in
// docked_store_dialog.cpp. Also entered by the Bar's Hire Escort action with
// hire_mode set.
[[nodiscard]] LandedExit
RunStoreDialog(SdlPlatform &platform,
               SdlAudio &audio,
               GameState &state,
               LandedService service,
               std::int16_t stellar_id,
               const std::function<void()> &render_background,
               bool hire_mode = false);

// Per-service modals orchestrated by NovaLanded_RunSubWindowDialog. Each takes
// the shared SdlAudio so a nested mission-computer window can play its cues.
[[nodiscard]] LandedExit
RunMissionBbsWindow(SdlPlatform &platform,
                    SdlAudio &audio,
                    GameState &state,
                    std::int16_t stellar_id,
                    const std::function<void()> &render_background);

[[nodiscard]] LandedExit
RunBarDialog(SdlPlatform &platform,
             SdlAudio &audio,
             GameState &state,
             std::int16_t stellar_id,
             const std::function<void()> &render_background);

[[nodiscard]] LandedExit
RunTradeCenterDialog(SdlPlatform &platform,
                     SdlAudio &audio,
                     GameState &state,
                     std::int16_t stellar_id,
                     const std::function<void()> &render_background);

} // namespace game
