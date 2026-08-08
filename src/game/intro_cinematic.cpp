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
// the intro was not skipped and g_intro_cinematic.post_intro_dest_id != -1 it
// opens the travel-selection dialog for that destination
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

// Mirrors the original's two distinct skip mechanisms from IntroCinematic_Run:
//  * Enter (0x1c) / Space (0x39) ended only the *current* frame's wait.
//  * The primary mouse command (_DAT_00591514) set the persistent latch `bVar9`
//    that carries across all remaining frames and is also latched once at
//    entry.
// Note Escape is *not* a skip key here: Ghidra IntroCinematic_Run reads only
// 0x1c (Enter), 0x39 (Space) and the primary mouse command. Returns whether the
// frame's wait should end (Enter/Space) and whether the primary latch fired.
struct SkipState {
  bool frame_done = false;    // Enter/Space: end this frame's wait
  bool primary_latch = false; // primary mouse: skip the whole sequence (bVar9)
};

SkipState PollSkip(SdlPlatform &platform, bool primary_latch) {
  SkipState result;
  result.primary_latch = primary_latch;
  for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
    if (input->key == TextKey::enter ||
        (input->key == TextKey::character && input->character == ' ')) {
      result.frame_done = true;
    } else if (input->key == TextKey::primary) {
      result.primary_latch = true;
    }
  }
  return result;
}

} // namespace

bool NovaIntroCinematic_Run(SdlPlatform &platform, GameState &state) {
  SDL_Renderer *const renderer = platform.renderer();
  const auto &cinematic = state.intro_cinematic;

  // Ghidra latched the primary mouse command once at entry into loop-local
  // `bVar9`, so if the player was already pressing the primary button the
  // whole sequence is skipped immediately. Enter/Space only fast-forward the
  // current frame, so they interact with the latch per frame below.
  bool primary_latch = PollSkip(platform, false).primary_latch;

  // Ghidra's per-frame quick-break on the frame loop is driven by bVar9 (the
  // primary latch); an Enter/Space fast-forward only moves to the next frame
  // slot, of which the default config has a single PICT then terminators.
  for (std::size_t frame_index = 0;
       frame_index < cinematic.source_pict_ids.size();
       ++frame_index) {
    const auto pict_id = cinematic.source_pict_ids[frame_index];
    if (pict_id < 1) {
      // Ghidra: source_pict_ids[i] < 1 means no art this frame. Only frames
      // with art enter the wait loop; the loop nevertheless walks all four
      // slots. A primary latch set on an earlier frame ends the walk.
      if (primary_latch || platform.quit_requested()) {
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

      // Wait out the per-frame duration. Enter/Space end this frame's wait
      // (frame_done); the primary mouse command sets the persistent latch,
      // which short-circuits every remaining wait immediately.
      const auto skip = PollSkip(platform, primary_latch);
      primary_latch = skip.primary_latch;
      if (primary_latch || platform.quit_requested()) {
        break; // primary latch ends the whole walk (see outer loop)
      }
      if (skip.frame_done) {
        break; // Enter/Space: advance to the next frame slot only
      }
      if (platform.ticks_ms() - start_ms >= duration_ms) {
        break;
      }
      SDL_Delay(16);
    }
    if (primary_latch || platform.quit_requested()) {
      break;
    }
  }

  DrainInput(platform);

  // Ghidra: after the sequence, if the primary latch was NOT set and
  // post_intro_dest_id != -1, open the travel-selection dialog. `intro_played`
  // is *not* set here: Ship_RunSpaceflightMode sets DAT_00596d35 after
  // IntroCinematic_Run returns (see spaceflight.cpp).
  if (!primary_latch && !platform.quit_requested()) {
    NovaLog::Info("intro cinematic finished (Enter/Space may have advanced "
                  "the single frame)");
    RunPostIntroDestinationStub(state);
  } else {
    NovaLog::Info("intro cinematic skipped by the player");
  }
  // Mirrors bVar9: the post-intro destination dialog is suppressed only by the
  // primary mouse command, matching the original's gate (`!bVar9 &&
  // post_intro_dest_id != -1`).
  return !primary_latch;
}

} // namespace game
