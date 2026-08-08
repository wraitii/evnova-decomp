#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

// Key a modal text/input dialog can act on. The menu's command channel only
// reports a fixed action-key set, so dialogs read raw editable keys through a
// separate channel.
enum class TextKey {
  none,
  character,
  enter,
  escape,
  backspace,
  // Left mouse button-press. Delivered through the same raw channel as the
  // keyboard so modal loops (e.g. the intro cinematic) that only read
  // PollTextEvent can also honor the game's primary-click command, mirroring
  // Ghidra IntroCinematic_Run polling _DAT_00591514. The menu's
  // PollCommandEvent channel reports the same press as 'm'.
  primary,
};

struct TextInput {
  TextKey key = TextKey::none;
  char character = '\0'; // valid when key == TextKey::character
};

// Continuous flight-input snapshot polled once per frame from the live
// keyboard state (not edge events, so holding a key steers continuously).
// Mirrors the player-control channel the original reads through the primary
// input driver in Frame_SpaceflightLoop scope 3 (Ship_HandlePlayerShipCore).
struct FlightInput {
  bool turn_left = false;  // left / 'a'
  bool turn_right = false; // right / 'd'
  bool thrust = false;     // up / 'w' (accelerate toward heading)
  bool brake = false;      // down / 's' (decelerate)
  // Primary fire: space (hold to keep firing the player's main weapon bank).
  // Stand-in for the original's primary-fire input command; the Ghost map is
  // not reconstructed, so this build binds the logical primary-fire command to
  // the space bar (documented divergence).
  bool fire = false;
  // Edge-triggered travel engage: 'j' (hyperspace jump toward the nearest
  // available travel point). The original uses a separate travel command
  // channel; this build maps it to a dedicated key so it is distinct from the
  // continuous steer inputs.
  bool travel = false;
  // Edge-triggered target-action command: 'e' (land on / interact with the
  // currently targeted stellar). Stand-in for the original's target-action
  // command channel (Ship_HandlePlayerTargetActionCommand); opens the landing
  // interaction for a landable target in range.
  bool target_action = false;
  // Cycle the stellar target. This is a clean-room binding for the original's
  // target-selection command channel; Tab advances through the current
  // system's eligible stellars and Shift+Tab goes backwards.
  bool cycle_target_next = false;
  bool cycle_target_previous = false;
};

class SdlTexture {
public:
  [[nodiscard]] static std::unique_ptr<SdlTexture>
  Create(SDL_Renderer *renderer,
         int width,
         int height,
         std::span<const std::uint8_t> rgba_pixels);
  [[nodiscard]] SDL_Texture *get() const;
  explicit SdlTexture(SDL_Texture *texture);

private:
  struct Deleter {
    void operator()(SDL_Texture *texture) const;
  };

  std::unique_ptr<SDL_Texture, Deleter> texture_;
};

class SdlPlatform {
public:
  SdlPlatform() = default;
  ~SdlPlatform();

  SdlPlatform(const SdlPlatform &) = delete;
  SdlPlatform &operator=(const SdlPlatform &) = delete;
  SdlPlatform(SdlPlatform &&) = delete;
  SdlPlatform &operator=(SdlPlatform &&) = delete;

  [[nodiscard]] bool Initialize();
  [[nodiscard]] SDL_Renderer *renderer() const;
  [[nodiscard]] std::optional<char> PollCommandEvent();
  // Raw editable-key event for modal dialogs. Enter/Escape/Backspace are
  // returned as distinct TextKey values; otherwise returns the translated
  // printable ASCII character (shifted key case). Quit still sets
  // quit_requested_.
  [[nodiscard]] std::optional<TextInput> PollTextEvent();
  // Live keyboard-state flight control snapshot (held-key steering).
  [[nodiscard]] FlightInput PollFlightInput();
  [[nodiscard]] bool quit_requested() const;
  [[nodiscard]] std::uint64_t ticks_ms() const;
  [[nodiscard]] SDL_FPoint mouse_position() const;

  // The current window->content presentation policy for this frame. See
  // SetCenteredPlayfield / SetScaledPlayfield / SetFullscreenPlayfield.
  enum class Presentation { kCentered, kScaled, kFullscreen };

  // The full window pixel size in the current presentation (logical draw
  // coordinates). World/spaceflight drawing queries this to extend to the
  // (possibly larger) window; it is independent of whether fixed screens are
  // being upscaled or bordered.
  [[nodiscard]] SDL_FPoint logical_playfield_size() const;

  // Resolution-extension helpers. The game renders onto a logical 640x480
  // content canvas (the original's 1024x768 surface scaled to its window); the
  // window itself has a 1024x768 minimum. Each presentation policy maps that
  // content to the window differently:
  //
  //  * SetScaledPlayfield()  -- fixed screens (main menu, splash, intro) are
  //    uniformly upscaled to fill the window (letterbox for aspect) via SDL's
  //    logical presentation. This is the "scale a few things up" default; at
  //    the 1024x768 minimum it is ~1:1 with the 1024-native art.
  //  * SetCenteredPlayfield()-- the docked/landed screen stays at native 1:1
  //    size, centred in the window with black bars on every side (never
  //    upscaled). Clipped via SDL_SetRenderViewport.
  //  * SetFullscreenPlayfield() -- the free-flight world spans the whole
  //    window 1:1 (no clipping / logical size) so larger windows show more of
  //    the system; HUD chrome stays at fixed, unscaled logical coordinates.
  //
  // SDL_RenderCoordinatesFromWindow reports the mouse in content coordinates
  // (viewport-relative / through the logical rect) when the corresponding
  // presentation is active, so hit-tests stay correct in all three modes.
  void SetCenteredPlayfield();
  void SetScaledPlayfield();
  void SetFullscreenPlayfield();

private:
  // The 640x480 logical content canvas shared by the fixed screens. This is
  // scaled up to the window in kScaled presentation and clipped centred in
  // kCentered; the extending world ignores it and tracks the window size.
  void ApplyCenteredPresentation();
  void ApplyScaledPresentation();
  void ApplyFullscreenPresentation();

  struct WindowDeleter {
    void operator()(SDL_Window *window) const;
  };

  struct RendererDeleter {
    void operator()(SDL_Renderer *renderer) const;
  };

  bool sdl_initialized_ = false;
  bool quit_requested_ = false;
  Presentation presentation_ = Presentation::kFullscreen;
  SDL_FPoint mouse_position_{};
  std::unique_ptr<SDL_Window, WindowDeleter> window_;
  std::unique_ptr<SDL_Renderer, RendererDeleter> renderer_;
};
