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
// Ghidra IntroCinematic_Run queues hint string `LoadStringResourceCopyById
// (0x7533)` centered over the frame. The STR# pool 0x7533 is not reconstructed
// (the string-table loader is not implemented), so the wording is sourced
// inline and the divergence logged; functionally it is the same one-line
// first-flight hint.
constexpr std::string_view kFirstFlightHint =
    "A NEW LIFE BEGINS ... press ENTER or SPACE to continue";

// Mirrors the splash presentation path in nova_app.cpp: the original centers
// each intro PICT at native size on the offscreen surface and clips overflow;
// here it is uniformly scaled to fit the 640x480 viewport. The visual is
// equivalent for the intro frame art and keeps a single shared draw path.
void PresentPict(SDL_Renderer *renderer, const PictImage &pict) {
  auto texture =
      SdlTexture::Create(renderer, pict.width, pict.height, pict.rgba_pixels);
  if (!texture) {
    return;
  }
  float width = 0.0F;
  float height = 0.0F;
  SDL_GetTextureSize(texture->get(), &width, &height);
  const auto scale = std::min(640.0F / width, 480.0F / height);
  const SDL_FRect destination{(640.0F - width * scale) / 2.0F,
                              (480.0F - height * scale) / 2.0F,
                              width * scale,
                              height * scale};
  SDL_RenderTexture(renderer, texture->get(), nullptr, &destination);
}

// Drains any queued input events the modal wait loop did not consume (the
// original calls NovaInputQueue_FlushAllCommands several times on exit).
void DrainInput(SdlPlatform &platform) {
  while (platform.PollTextEvent()) {
  }
}

// Ghidra IntroCinematic_Run's post-intro epilogue: after the last frame, when
// g_intro_cinematic.post_intro_dest_id != -1 it opens the travel-selection
// dialog for that destination
// (Ui_RunTravelSelectionDialog, gating DAT_007d1fa6 = 1). The full systems
// dialog is not reconstructed, so this reproduces the *gating* faithfully and
// logs what is skipped. With the new-game default post_intro_dest_id = 0x7ffd
// the gate always opens, mirroring the original's expectation that the pilot
// picks a first destination after the intro.
void RunPostIntroDestinationStub(const GameState &state) {
  if (!state.intro_cinematic.should_open_post_intro_dialog()) {
    return;
  }
  NovaLog::Todo(
      "post-intro travel-selection dialog (Ui_RunTravelSelectionDialog) is "
      "not reconstructed: the intro gated on post_intro_dest_id {} but no "
      "destination dialog/systems table is available",
      state.intro_cinematic.post_intro_dest_id);
}

// A pointer press inside the render rect makes IntroCinematic_Run leave its
// current frame wait (`local_19`); it does not set the routine's separate
// command latch (`bVar9`) and therefore does not discard the remaining frames.
// Enter (0x1c) and Space (0x39) have the same per-frame effect. SDL exposes
// only the pointer press here, so it maps to that mouse path. Escape is not a
// cinematic skip key in the original.
struct SkipState {
  bool frame_done = false;
};

SkipState PollSkip(SdlPlatform &platform) {
  SkipState result;
  for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
    if (input->key == TextKey::enter ||
        (input->key == TextKey::character && input->character == ' ') ||
        input->key == TextKey::primary) {
      result.frame_done = true;
    }
  }
  return result;
}

} // namespace

// Ghidra 0x0048adc0 IntroCinematic_Run.
bool NovaIntroCinematic_Run(SdlPlatform &platform, GameState &state) {
  SDL_Renderer *const renderer = platform.renderer();
  const auto &cinematic = state.intro_cinematic;

  // A frame-specific advance only ends that frame's wait. In particular, a
  // click during the first stock frame advances to the second stock frame.
  for (std::size_t frame_index = 0;
       frame_index < cinematic.source_pict_ids.size();
       ++frame_index) {
    const auto pict_id = cinematic.source_pict_ids[frame_index];
    if (pict_id < 1) {
      // Ghidra: source_pict_ids[i] < 1 means no art this frame. Only frames
      // with art enter the wait loop; the loop nevertheless walks all four
      // slots.
      if (platform.quit_requested()) {
        break;
      }
      continue;
    }

    // Load and present this frame's art. Ghidra: Resource_LoadPictAsImage on
    // source_pict_ids[i], center + blit onto the offscreen surface, draw it to
    // the render-owner rect with the first-flight hint overlaid.
    std::optional<PictImage> pict = std::nullopt;
    if (const auto pict_data =
            NovaResource_LoadPictData(static_cast<std::uint16_t>(pict_id))) {
      pict = Resource_LoadPictAsImage(*pict_data);
    }
    if (!pict) {
      NovaLog::Todo("intro frame PICT 0x{:04x} could not be decoded; showing "
                    "blank frame",
                    pict_id);
    }

    const auto start_ms = platform.ticks_ms();
    const auto duration_ms =
        static_cast<std::uint64_t>(
            std::max<int>(0, cinematic.duration_60h_ticks[frame_index])) *
        kMsPer60Tick;
    while (!platform.quit_requested()) {
      SDL_SetRenderDrawColor(renderer, 1, 4, 12, SDL_ALPHA_OPAQUE);
      SDL_RenderClear(renderer);
      // The intro cinematic renders as a fixed screen, upscaled to the window
      // like the menu/splash (same scaled 640x480 logical presentation).
      platform.SetScaledPlayfield();
      if (pict) {
        PresentPict(renderer, *pict);
      }
      SDL_SetRenderDrawColor(renderer, 202, 224, 255, SDL_ALPHA_OPAQUE);
      const auto hint_width =
          static_cast<float>(std::strlen(kFirstFlightHint.data())) * 8.0F;
      SDL_RenderDebugText(renderer,
                          (640.0F - hint_width) / 2.0F,
                          452.0F,
                          kFirstFlightHint.data());
      SDL_RenderPresent(renderer);

      // Wait out the per-frame duration. Keyboard and pointer input advance
      // one frame only; the outer loop then presents the next configured PICT.
      const auto skip = PollSkip(platform);
      if (platform.quit_requested()) {
        break;
      }
      if (skip.frame_done) {
        break;
      }
      if (platform.ticks_ms() - start_ms >= duration_ms) {
        break;
      }
      SDL_Delay(16);
    }
    if (platform.quit_requested()) {
      break;
    }
  }

  DrainInput(platform);

  // Ghidra: after the sequence, if post_intro_dest_id != -1, open the
  // travel-selection dialog. `intro_played`
  // is *not* set here: Ship_RunSpaceflightMode sets DAT_00596d35 after
  // IntroCinematic_Run returns (see spaceflight.cpp).
  if (!platform.quit_requested()) {
    NovaLog::Info("intro cinematic finished (input may have advanced "
                  "individual frames)");
    RunPostIntroDestinationStub(state);
  }
  return !platform.quit_requested();
}

} // namespace game
