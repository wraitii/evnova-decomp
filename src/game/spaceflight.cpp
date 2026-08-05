#include "spaceflight.hpp"

#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "intro_cinematic.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace game {
namespace {

// Draw the placeholder in-system view. The original renders a starfield,
// planets, the pilot's ship sprite, and the full HUD chrome on the gameplay
// surface. None of that is reconstructed yet, so this is a recognized stand-in:
// a starfield, a HUD frame with the pilot's basics, and a "SIMULATION NOT
// RECONSTRUCTED" banner (mirroring the placeholder HUD the menu already draws).
void DrawInGameFrame(SdlPlatform &platform, const GameState &state) {
  SDL_Renderer *const renderer = platform.renderer();
  SDL_SetRenderDrawColor(renderer, 0, 0, 6, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);

  // Sparse deterministic starfield; the original uses a per-system ambient
  // particle system (NovaEffects_QueuedAmbientStarParticles).
  SDL_SetRenderDrawColor(renderer, 140, 190, 255, SDL_ALPHA_OPAQUE);
  constexpr std::size_t kStarCount = 90;
  for (std::size_t i = 0; i < kStarCount; ++i) {
    const auto x = static_cast<float>((i * 61 + 13) % 640);
    const auto y = static_cast<float>((i * 97 + 41) % 400);
    SDL_RenderPoint(renderer, x, y);
  }

  // HUD frame + debug readouts (placeholder).
  SDL_SetRenderDrawColor(renderer, 51, 113, 171, SDL_ALPHA_OPAQUE);
  const SDL_FRect outer{18.0F, 400.0F, 604.0F, 62.0F};
  SDL_RenderRect(renderer, &outer);
  SDL_SetRenderDrawColor(renderer, 202, 224, 255, SDL_ALPHA_OPAQUE);
  const std::string callsign = "PLT " + state.pilot.first_name;
  SDL_RenderDebugText(renderer, 30.0F, 410.0F, callsign.c_str());
  const std::string hud =
      "SHLD " + std::to_string(static_cast<int>(state.player.shield_points)) +
      "   ARM " + std::to_string(static_cast<int>(state.player.armor_points)) +
      "   FUEL " + std::to_string(static_cast<int>(state.player.fuel_points)) +
      "   CR " + std::to_string(state.player.credits);
  SDL_RenderDebugText(renderer, 30.0F, 426.0F, hud.c_str());
  SDL_SetRenderDrawColor(renderer, 240, 120, 90, SDL_ALPHA_OPAQUE);
  SDL_RenderDebugText(renderer, 30.0F, 444.0F,
                      "SPACEFLIGHT SIMULATION NOT RECONSTRUCTED  [ESC] menu");
}

} // namespace

void NovaSpaceflight_Run(SdlPlatform &platform, GameState &state) {
  NovaLog::Info("entering spaceflight mode");

  // Preflight: the new-game intro cinematic plays on the pilot's first entry
  // (Ghidra DAT_00596d35 == 0 in Ship_RunSpaceflightMode). We set the
  // intro_played latch *after* the intro returns, exactly as the original sets
  // DAT_00596d35 = 0x01 immediately after IntroCinematic_Run(). The intro's
  // skip result already gates (a stub of) the post-intro travel-selection
  // dialog internally, so its return value needs no action here.
  if (!state.intro_played) {
    (void)NovaIntroCinematic_Run(platform, state);
    // Ghidra: DAT_00596d35 = 0x01, the latch IntroCinematic_SetupFrames/
    // Game_ResetNewGameState clear on a new pilot (see new_pilot_flow.cpp).
    state.intro_played = true;
  }

  // In-space main loop. Frame_SpaceflightLoop is a per-frame update/present
  // loop that handles movement, targeting, combat, missions and the pause menu
  // until the player exits; here the replaceable stub is the whole frame.
  // TODO(decomp): implement the real spaceflight loop.
  bool returning_to_menu = false;
  while (!platform.quit_requested() && !returning_to_menu) {
    // Exit-to-menu: the original enters the pause menu via Escape; that flow
    // (Ui_RunPauseMenu / Menu_Open...) is not reconstructed, so Escape returns
    // directly to the menu shell and is logged as a divergence.
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      if (input->key == TextKey::escape ||
          (input->key == TextKey::character && input->character == 'q')) {
        returning_to_menu = true;
        break;
      }
    }

    DrawInGameFrame(platform, state);
    SDL_RenderPresent(platform.renderer());
    SDL_Delay(16);
  }

  NovaLog::Info("leaving spaceflight mode to the main menu");
}

} // namespace game
