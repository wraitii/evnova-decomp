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

private:
  struct WindowDeleter {
    void operator()(SDL_Window *window) const;
  };

  struct RendererDeleter {
    void operator()(SDL_Renderer *renderer) const;
  };

  bool sdl_initialized_ = false;
  bool quit_requested_ = false;
  SDL_FPoint mouse_position_{};
  std::unique_ptr<SDL_Window, WindowDeleter> window_;
  std::unique_ptr<SDL_Renderer, RendererDeleter> renderer_;
};
