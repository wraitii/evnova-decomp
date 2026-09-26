#include "intro_cinematic.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_audio.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "mission.hpp"
#include "preferences.hpp"
#include "selection_text_dialog.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace game {
namespace {

// The original waits duration_60h_ticks[i] * 0x3c ms (Ghidra IntroCinematic_Run
// compares a millisecond clock against ticks * 60), i.e. each "60h tick" unit
// is 60 ms of wall time. Kept verbatim so the default single-frame intro holds
// for the same 600 ms it does in the original.
constexpr std::uint64_t kMsPer60Tick = 60;
// Ghidra IntroCinematic_Run plays snd 0x7533 alongside each arted frame
// (loaded via NovaSound_LoadDecodedById 0x004bc2a0, which loads the `snd `
// resource and decodes it; NovaAudio_QueueCenteredSound ->
// Audio_AllocateVoiceSlot queues it with a stereo-width of 10 and a centered
// pan pair DAT_007353f2/DAT_007353f2, and the decoded payload is freed after
// the frame's wait). Stock Nova ships no snd 0x7533, so the intro is silent
// there; a scenario that provides it gets it back. Stereo width/pan are not
// modelled by SdlAudio::Play, so the cue plays at unity gain.
constexpr std::uint16_t kIntroSoundId = 0x7533;

// The original centers each intro PICT at native size on the 1024x768 offscreen
// surface and clips overflow. The surrounding placement contains that authored
// surface in the window and caps it at 1x. Runs inline in IntroCinematic_Run.
void PresentPict(SDL_Renderer *renderer, const PictImage &pict) {
  auto texture =
      SdlTexture::Create(renderer, pict.width, pict.height, pict.rgba_pixels);
  if (!texture) {
    return;
  }
  float width = 0.0F;
  float height = 0.0F;
  SDL_GetTextureSize(texture->get(), &width, &height);
  const SDL_FRect destination{
      (1024.0F - width) / 2.0F, (768.0F - height) / 2.0F, width, height};
  SDL_RenderTexture(renderer, texture->get(), nullptr, &destination);
}

// Ghidra IntroCinematic_Run's epilogue (0x0048adc0): when not skipped and
// intro_text_desc_id != -1, loads the desc (0x004c6d50), runs the placeholder
// and wildcard passes, and opens the generic text reader (0x004982a0). Empty
// text opens no dialog, matching the reader's empty-text arm. Stock .Trader
// carries -1; the no-save fallback 0x7ffd is not a valid desc.
void RunPostIntroTextReader(SdlPlatform &platform,
                            GameState &state,
                            const std::function<void()> &render_background) {
  if (!state.intro_cinematic.should_open_intro_text_dialog()) {
    return;
  }
  std::string text;
  std::int16_t dialog_variant = 0;
  if (const auto desc = NovaResource_LoadDescription(static_cast<std::uint16_t>(
          state.intro_cinematic.intro_text_desc_id))) {
    text = desc->text;
    dialog_variant = desc->dialog_variant;
    // Ui_LoadSelectionDialogResource's placeholder pass runs at load time,
    // before the wildcard pass (0x004c6d50 -> 0x0044a4d0 -> 0x004444f0).
    Mission_ExpandStringPlaceholders(state, text);
    text = Mission_ExpandMissionWildcards(state, text, false, -1);
  }
  if (text.empty()) {
    NovaLog::Info("intro text desc {} produced empty text; reader stays "
                  "closed (original's empty-text arm)",
                  state.intro_cinematic.intro_text_desc_id);
    return;
  }
  // g_selection_dialog_over_static_surface = 1 in the original: the reader
  // redraws the dark space background rather than the live frame.
  NovaUi_RunTextReaderDialog(
      platform, state, text, false, render_background, dialog_variant);
}

// Per-iteration input poll of the intro wait loop. Mirrors the original's
// split:
//   - Enter (0x1c), Space (0x39) and an in-rect left click (the original's
//     local_19 latch, reachable while the button is held inside the
//     render-owner rect) end only the current frame's wait (the outer loop
//     then presents the next configured PICT);
//   - the skip command (g_nova_control_bits[0x48] == g_player_key_bindings
//     [0x17], default 0x01 = PC scancode 1 = Escape; NovaPrefs_ResetKeyBindings
//     0x004b4400) latches bVar9, which breaks out of the wait AND the frame
//     loop, skipping all remaining frames.
// Input_PumpAndTestCommand polls a key-state table (FUN_004f1900:
// g_key_state_snapshot[code] & 1) that only keyboard scancodes feed
// (FUN_004d7330 case 0x100); the mouse button only raises the platform ready
// flag (DAT_008701a0), which is what drives the in-rect frame advance. So a
// single click advances exactly one slide, and the bound skip command
// (binding slot 0x17, default Escape) skips the whole intro.
struct IntroInput {
  bool frame_done = false;
  bool skip_all = false;
};

IntroInput PollIntroInput(SdlPlatform &platform, std::uint16_t skip_key) {
  IntroInput result;
  for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
    const SDL_FPoint mouse = platform.mouse_position();
    const bool mouse_in_intro_surface = mouse.x >= 0.0F && mouse.x < 1024.0F &&
                                        mouse.y >= 0.0F && mouse.y < 768.0F;
    if (input->key == TextKey::enter ||
        (input->key == TextKey::character && input->character == ' ') ||
        (input->key == TextKey::primary && mouse_in_intro_surface)) {
      result.frame_done = true;
    }
    if (skip_key != 0xffff && input->key_code == skip_key) {
      result.skip_all = true;
    }
  }
  return result;
}

} // namespace

// @port 0x004CD3B0 95% ui
// Ghidra 0x004cd3b0 IntroCinematic_SetupFrames: keyed block lookup by
// character-template name (ResourceData_AccessByKey); fills source_pict_ids
// block+0x20, duration_60h_ticks block+0x28 (clamped [0,300]), and
// intro_text_desc_id block+0x30 (Bible char IntroTextID). Absent block -> PICT
// 0x2008 / 10 ticks / desc 0x7ffd. See docs/char_resource_format.md.
// TODO(decomp(0x004cd3b0)): verify the frame/duration decode (block +0x20 and
// +0x28 reads, the [0,300] clamp, and the absent-block fallbacks) against
// Ghidra; no known behavioral gap.
void NovaIntroCinematic_SetupFrames(GameState &state,
                                    std::string_view block_key) {
  auto &cinematic = state.intro_cinematic;

  // Ghidra: DAT_00863d60 = ResourceData_AccessByKey(0x63688a72, pilot_data).
  const auto block = NovaResource_AccessCharacterBlockByKey(block_key);
  if (!block) {
    // No-save default: one PICT 0x2008 for 10 ticks, intro text 0x7ffd
    // (not a valid desc, but != -1 so the reader still opens).
    cinematic.intro_text_desc_id = 0x7ffd;
    cinematic.source_pict_ids = {0x2008, -1, -1, -1};
    cinematic.duration_60h_ticks = {10, 0, 0, 0};
    NovaLog::Info("intro cinematic: no pilot block for '{}'; using the "
                  "no-save default frame",
                  block_key);
    return;
  }

  const auto bytes = std::span{block->bytes};
  const auto read_i16 = [&bytes](std::size_t offset) {
    if (offset + 2 > bytes.size()) {
      // The original grows short blocks to 0x16a (ResourceData_EnsureBlockSize)
      // before reading; absent that, reads past the block stay zero.
      return static_cast<std::int16_t>(0);
    }
    return static_cast<std::int16_t>(
        (std::to_integer<unsigned>(bytes[offset]) << 8U) |
        std::to_integer<unsigned>(bytes[offset + 1]));
  };

  cinematic.intro_text_desc_id = read_i16(0x30);
  for (std::size_t i = 0; i < cinematic.source_pict_ids.size(); ++i) {
    auto frame_id = read_i16(0x20 + i * 2);
    auto duration = read_i16(0x28 + i * 2);
    // Frame ids below 0x80 are not PICTs; the original rewrites them to the
    // "no art" pair. Valid frames clamp the duration to [0, 300] ticks.
    if (frame_id < 0x80) {
      frame_id = -1;
      duration = 0;
    } else {
      duration = std::clamp<std::int16_t>(duration, 0, 300);
    }
    cinematic.source_pict_ids[i] = frame_id;
    cinematic.duration_60h_ticks[i] = duration;
  }
  NovaLog::Info("intro cinematic configured from block '{}': frames {} {} {} "
                "{} for {} {} {} {} ticks, intro text desc {}",
                block->name,
                cinematic.source_pict_ids[0],
                cinematic.source_pict_ids[1],
                cinematic.source_pict_ids[2],
                cinematic.source_pict_ids[3],
                cinematic.duration_60h_ticks[0],
                cinematic.duration_60h_ticks[1],
                cinematic.duration_60h_ticks[2],
                cinematic.duration_60h_ticks[3],
                cinematic.intro_text_desc_id);
}

// @port 0x0048adc0 98% ui
// Ghidra 0x0048adc0 IntroCinematic_Run.
bool NovaIntroCinematic_Run(SdlPlatform &platform,
                            SdlAudio &audio,
                            GameState &state,
                            const NovaPreferences &prefs) {
  SDL_Renderer *const renderer = platform.renderer();
  const auto &cinematic = state.intro_cinematic;
  const std::uint16_t skip_key = prefs.bindings.cmd_to_key[0x17];

  // The skip latch is polled at entry too: the bound skip key already held when
  // the cinematic starts skips straight to the epilogue gate (the original
  // tests g_player_key_bindings[0x17] once before the frame loop).
  IntroInput input_state;
  if (skip_key != 0xffff && platform.IsOriginalKeyCodeHeld(skip_key)) {
    input_state.skip_all = true;
  }

  // Decode the intro sound once for the run (the original decodes the snd
  // resource per arted frame and frees it after the frame's wait).
  std::optional<NovaSoundData> intro_sound;
  if (const auto snd = NovaResource_LoadSndData(kIntroSoundId)) {
    intro_sound = NovaSound_Decode(*snd);
    if (!intro_sound) {
      NovaLog::Todo("intro sound snd 0x{:04x} could not be decoded",
                    kIntroSoundId);
    }
  }

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
    // source_pict_ids[i], center + blit onto the offscreen surface, commit
    // the frame to the render-owner rect.
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
    if (intro_sound) {
      audio.Play(*intro_sound,
                 1.0F,
                 1.0F,
                 /*sound_key=*/-1,
                 /*priority_width=*/10);
    }

    const auto start_ms = platform.gameplay_ticks_ms();
    const auto duration_ms =
        static_cast<std::uint64_t>(
            std::max<int>(0, cinematic.duration_60h_ticks[frame_index])) *
        kMsPer60Tick;
    while (!platform.quit_requested() && !input_state.skip_all) {
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
      SDL_RenderClear(renderer);
      // The intro cinematic uses the native 1024x768 authored composition and
      // is deliberately excluded from the UI scale: it is timed media, not an
      // interactive composition.
      SdlPlatform::ScopedPlacement placement(
          platform,
          PlaceContained({1024.0F, 768.0F}, platform.logical_playfield_size()));
      if (pict) {
        PresentPict(renderer, *pict);
      }
      platform.Present();

      // Wait out the per-frame duration; Enter/Space/in-rect click advance one
      // frame, Escape skips the rest.
      const auto polled = PollIntroInput(platform, skip_key);
      input_state.skip_all = input_state.skip_all || polled.skip_all;
      if (platform.quit_requested() || input_state.skip_all ||
          polled.frame_done ||
          platform.gameplay_ticks_ms() - start_ms >= duration_ms) {
        break;
      }
      platform.PaceFrame();
    }
    if (platform.quit_requested() || input_state.skip_all) {
      break;
    }
  }

  // Ghidra: NovaInputQueue_FlushAllCommands x4 on exit.
  while (platform.PollTextEvent()) {
  }

  // Ghidra: after the sequence, if not skipped and intro_text_desc_id != -1,
  // show the intro text reader. `intro_played` is *not* set here:
  // Ship_RunSpaceflightMode sets g_intro_played after IntroCinematic_Run
  // returns (see spaceflight.cpp).
  if (!platform.quit_requested() && !input_state.skip_all) {
    NovaLog::Info("intro cinematic finished (input may have advanced "
                  "individual frames)");
    // over_static_surface variant: fill the dark space background (PTR_DAT_
    // 00575acc equivalent) rather than the live frame.
    RunPostIntroTextReader(platform, state, [renderer]() {
      SDL_SetRenderDrawColor(renderer, 1, 4, 12, SDL_ALPHA_OPAQUE);
      SDL_RenderClear(renderer);
    });
  }
  // Return the bVar9 mirror (see intro_cinematic.hpp).
  return !input_state.skip_all;
}

} // namespace game
