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
enum class TextKey { none, character, enter, escape, backspace };

struct TextInput {
  TextKey key = TextKey::none;
  char character = '\0'; // valid when key == TextKey::character
};

class SdlTexture {
public:
  [[nodiscard]] static std::unique_ptr<SdlTexture>
  Create(SDL_Renderer *renderer, int width, int height,
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
