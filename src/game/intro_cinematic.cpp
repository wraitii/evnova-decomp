#include "intro_cinematic.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace game {
namespace {

// The original waits duration_60h_ticks[i] * 0x3c ms (Ghidra IntroCinematic_Run
// compares a millisecond clock against ticks * 60), i.e. each "60h tick" unit
// is 60 ms of wall time. Kept verbatim so the default single-frame intro holds
// for the same 600 ms it does in the original.
constexpr std::uint64_t kMsPer60Tick = 60;
constexpr std::string_view kFirstFlightHint =
    "A NEW LIFE BEGINS ... press ENTER or SPACE to continue";

// Mirrors the splash presentation path in nova_app.cpp: the original centers
// each intro PICT at native size on the offscreen surface and clips overflow;
// here it is uniformly scaled to fit the 640x480 viewport. The visual is
// equivalent for the intro frame art and keeps a single shared draw path.
void PresentPict(SDL_Renderer *renderer, const PictImage &pict) {
  auto texture = SdlTexture::Create(renderer, pict.width, pict.height,
                                    pict.rgba_pixels);
  if (!texture) {
    return;
  }
  float width = 0.0F;
  float height = 0.0F;
  SDL_GetTextureSize(texture->get(), &width, &height);
  const auto scale = std::min(640.0F / width, 480.0F / height);
  const SDL_FRect destination{(640.0F - width * scale) / 2.0F,
                              (480.0F - height * scale) / 2.0F, width * scale,
                              height * scale};
  SDL_RenderTexture(renderer, texture->get(), nullptr, &destination);
}

// Drains any queued input events the modal wait loop did not consume (the
// original calls NovaInputQueue_FlushAllCommands several times on exit).
void DrainInput(SdlPlatform &platform) {
  while (platform.PollTextEvent()) {
  }
}

} // namespace

bool NovaIntroCinematic_Run(SdlPlatform &platform, GameState &state) {
  SDL_Renderer *const renderer = platform.renderer();
  const auto &cinematic = state.intro_cinematic;
  bool skipped = false;

  for (std::size_t frame_index = 0;
       frame_index < cinematic.source_pict_ids.size(); ++frame_index) {
    const auto pict_id = cinematic.source_pict_ids[frame_index];
    if (pict_id < 1) {
      // Ghidra: source_pict_ids[i] < 1 means no art this frame. Skip it.
      continue;
    }

    // Load and present this frame's art. Ghidra: Resource_LoadPictAsImage on
    // source_pict_ids[i], center + blit onto the offscreen surface, draw it to
    // the render-owner rect with the first-flight hint overlaid.
    std::optional<PictImage> pict = std::nullopt;
    if (const auto pict_data = NovaResource_LoadPictData(
            static_cast<std::uint16_t>(pict_id))) {
      pict = Resource_LoadPictAsImage(*pict_data);
    }
    if (!pict) {
      NovaLog::Todo("intro frame PICT 0x{:04x} could not be decoded; showing "
                    "blank frame",
                    pict_id);
    }

    const auto start_ms = platform.ticks_ms();
    const auto duration_ms = static_cast<std::uint64_t>(
                                 std::max<int>(0,
                                               cinematic.duration_60h_ticks
                                                   [frame_index])) *
                             kMsPer60Tick;
    while (!platform.quit_requested()) {
      SDL_SetRenderDrawColor(renderer, 1, 4, 12, SDL_ALPHA_OPAQUE);
      SDL_RenderClear(renderer);
      if (pict) {
        PresentPict(renderer, *pict);
      }
      SDL_SetRenderDrawColor(renderer, 202, 224, 255, SDL_ALPHA_OPAQUE);
      const auto hint_width =
          static_cast<float>(std::strlen(kFirstFlightHint.data())) * 8.0F;
      SDL_RenderDebugText(renderer, (640.0F - hint_width) / 2.0F, 452.0F,
                          kFirstFlightHint.data());
      SDL_RenderPresent(renderer);

      // Wait out the per-frame duration with input-to-skip. Ghidra ends the
      // frame early on Enter (0x1c) or Space (0x39); the primary mouse command
      // (_DAT_00591514) also skips but takes the PollCommandEvent channel,
      // which this loop does not read, so only the keyboard skip keys are
      // honored (a documented divergence). Skipping suppresses the post-intro
      // travel-selection dialog (returned to the caller as `skipped`).
      for (std::optional<TextInput> input;
           (input = platform.PollTextEvent());) {
        if (input->key == TextKey::enter || input->key == TextKey::escape ||
            (input->key == TextKey::character && input->character == ' ')) {
          skipped = true;
          break;
        }
      }
      if (skipped || platform.quit_requested()) {
        break;
      }
      if (platform.ticks_ms() - start_ms >= duration_ms) {
        break;
      }
      SDL_Delay(16);
    }
    if (skipped || platform.quit_requested()) {
      break;
    }
  }

  DrainInput(platform);
  if (skipped) {
    NovaLog::Info("intro cinematic skipped by the player");
  } else {
    NovaLog::Info("intro cinematic finished");
  }
  state.intro_played = true;
  return !skipped;
}

} // namespace game
