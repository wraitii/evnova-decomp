#include "sdl_platform.hpp"
#include "log.hpp"

#include <algorithm>

namespace {
// The original renders onto a fixed 640x480 logical playfield (the full-screen
// space/docked/menu view). See SdlPlatform::ApplyLogicalPresentation for how
// the presentation maps this onto the (larger, resizable) window in the two
// resolution modes.
constexpr int kPlayfieldWidth = 640;
constexpr int kPlayfieldHeight = 480;
} // namespace

void SdlTexture::Deleter::operator()(SDL_Texture *texture) const {
  SDL_DestroyTexture(texture);
}

SdlTexture::SdlTexture(SDL_Texture *texture) : texture_(texture) {}

std::unique_ptr<SdlTexture>
SdlTexture::Create(SDL_Renderer *renderer,
                   int width,
                   int height,
                   std::span<const std::uint8_t> rgba_pixels) {
  SDL_Texture *texture = SDL_CreateTexture(renderer,
                                           SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC,
                                           width,
                                           height);
  if (texture == nullptr ||
      !SDL_UpdateTexture(texture, nullptr, rgba_pixels.data(), width * 4)) {
    SDL_DestroyTexture(texture);
    return nullptr;
  }
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  return std::make_unique<SdlTexture>(texture);
}

SDL_Texture *SdlTexture::get() const { return texture_.get(); }

void SdlPlatform::WindowDeleter::operator()(SDL_Window *window) const {
  SDL_DestroyWindow(window);
}

void SdlPlatform::RendererDeleter::operator()(SDL_Renderer *renderer) const {
  SDL_DestroyRenderer(renderer);
}

SdlPlatform::~SdlPlatform() {
  renderer_.reset();
  window_.reset();
  if (sdl_initialized_) {
    SDL_Quit();
  }
}

bool SdlPlatform::Initialize() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    NovaLog::Error("SDL initialization failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_SetAppMetadata("Escape Velocity Nova", "0.1.0", "com.ambrosiasw.evnova");
  // Default window: start larger than the original's 640x480 so the
  // resolution-extension behaviour is visible (fixed screens kept centred with
  // black borders, the free-flight world extending to show more). Kept
  // resizable; scale_to_window_ is off by default.
  window_.reset(SDL_CreateWindow("Escape Velocity Nova",
                                 960,
                                 720,
                                 SDL_WINDOW_RESIZABLE));
  if (!window_) {
    NovaLog::Error("SDL window creation failed: {}", SDL_GetError());
    return false;
  }

  renderer_.reset(SDL_CreateRenderer(window_.get(), nullptr));
  if (!renderer_) {
    NovaLog::Error("SDL renderer creation failed: {}", SDL_GetError());
    return false;
  }

  SDL_SetRenderVSync(renderer_.get(), 1);
  ApplyLogicalPresentation();
  return true;
}

SDL_Renderer *SdlPlatform::renderer() const { return renderer_.get(); }

void SdlPlatform::ApplyLogicalPresentation() {
  if (!renderer_) {
    return;
  }
  if (scale_to_window_) {
    // DIVERGENCE (documented, toggled with F5): uniformly scale the whole
    // 640x480 playfield to fill the window, letterboxing the aspect.
    SDL_SetRenderLogicalPresentation(renderer_.get(),
                                     kPlayfieldWidth,
                                     kPlayfieldHeight,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    return;
  }
  // Faithful resolution-extension: no logical size, so 1 logical unit = 1
  // physical pixel and the fixed 640x480 playfield gets black borders when the
  // window is larger (fixed screens) or the world extends (see
  // logical_playfield_size).
  SDL_SetRenderLogicalPresentation(renderer_.get(),
                                   0,
                                   0,
                                   SDL_LOGICAL_PRESENTATION_DISABLED);
}

bool SdlPlatform::ToggleScale() {
  scale_to_window_ = !scale_to_window_;
  ApplyLogicalPresentation();
  NovaLog::Info("render scale mode: {}",
                scale_to_window_ ? "scale-to-window (divergence)"
                                 : "resolution-extension (1:1)");
  return scale_to_window_;
}

SDL_FPoint SdlPlatform::logical_playfield_size() const {
  if (scale_to_window_) {
    return {static_cast<float>(kPlayfieldWidth),
            static_cast<float>(kPlayfieldHeight)};
  }
  int w = kPlayfieldWidth;
  int h = kPlayfieldHeight;
  if (renderer_) {
    SDL_GetRenderOutputSize(renderer_.get(), &w, &h);
  }
  return {static_cast<float>(w), static_cast<float>(h)};
}

void SdlPlatform::SetCenteredPlayfield() {
  if (!renderer_) {
    return;
  }
  const auto sz = logical_playfield_size();
  const int ox = std::max(0, static_cast<int>(sz.x) - kPlayfieldWidth) / 2;
  const int oy = std::max(0, static_cast<int>(sz.y) - kPlayfieldHeight) / 2;
  // Clip fixed-screen drawing to the centred 640x480 playfield (a no-op in
  // scale mode where the logical presentation is already 640x480).
  // SDL_RenderCoordinatesFromWindow (used to fill mouse_position_) already
  // subtracts this viewport origin, so hit-tests see playfield coordinates.
  const SDL_Rect viewport{ox, oy, kPlayfieldWidth, kPlayfieldHeight};
  SDL_SetRenderViewport(renderer_.get(), &viewport);
}

void SdlPlatform::SetFullscreenPlayfield() {
  if (!renderer_) {
    return;
  }
  // Full-window viewport (extending world / fullscreen splash): no clipping,
  // mouse coordinates track the whole window 1:1.
  SDL_SetRenderViewport(renderer_.get(), nullptr);
}

std::optional<TextInput> SdlPlatform::PollTextEvent() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      quit_requested_ = true;
      continue;
    }
    if (event.type == SDL_EVENT_WINDOW_RESIZED && !scale_to_window_) {
      // 1:1 extend mode must track the window size so the playfield stays
      // top-left with black borders (fixed screens) / the world extends.
      ApplyLogicalPresentation();
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      SDL_RenderCoordinatesFromWindow(renderer_.get(),
                                      event.motion.x,
                                      event.motion.y,
                                      &mouse_position_.x,
                                      &mouse_position_.y);
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
      SDL_RenderCoordinatesFromWindow(renderer_.get(),
                                      event.button.x,
                                      event.button.y,
                                      &mouse_position_.x,
                                      &mouse_position_.y);
      return TextInput{TextKey::primary};
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
      if (event.key.key == SDLK_F5) {
        ToggleScale();
        continue;
      }
      switch (event.key.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return TextInput{TextKey::enter};
      case SDLK_ESCAPE:
        return TextInput{TextKey::escape};
      case SDLK_BACKSPACE:
        return TextInput{TextKey::backspace};
      default:
        break;
      }
      // Any printable key symbols map to their ASCII value: letters and digits
      // use ASCII syms, while combined punctuation is approximated by its
      // scanned key's symbol (enough for callsign entry).
      const auto sym = static_cast<int>(event.key.key);
      if (sym >= 32 && sym < 127) {
        return TextInput{TextKey::character, static_cast<char>(sym)};
      }
    }
  }
  return std::nullopt;
}

FlightInput SdlPlatform::PollFlightInput() {
  // Drain queued events first so the window stays responsive and the keyboard
  // state reflects the latest presses/releases. Then read the live key state
  // for the flight controls (edge-agnostic, so holding a key steers).
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      quit_requested_ = true;
    }
    if (event.type == SDL_EVENT_WINDOW_RESIZED && !scale_to_window_) {
      ApplyLogicalPresentation();
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        event.key.key == SDLK_F5) {
      ToggleScale();
    }
  }
  const bool *const keys = SDL_GetKeyboardState(nullptr);
  FlightInput input;
  const auto pressed = [&](SDL_Scancode scancode) {
    return keys[scancode] != 0;
  };
  input.turn_left = pressed(SDL_SCANCODE_LEFT) || pressed(SDL_SCANCODE_A);
  input.turn_right = pressed(SDL_SCANCODE_RIGHT) || pressed(SDL_SCANCODE_D);
  input.thrust = pressed(SDL_SCANCODE_UP) || pressed(SDL_SCANCODE_W);
  input.brake = pressed(SDL_SCANCODE_DOWN) || pressed(SDL_SCANCODE_S);
  input.travel = pressed(SDL_SCANCODE_J);
  input.target_action = pressed(SDL_SCANCODE_E);
  // Primary fire (held): space. See FlightInput::fire for the mapping note.
  input.fire = pressed(SDL_SCANCODE_SPACE);
  return input;
}

std::optional<char> SdlPlatform::PollCommandEvent() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      quit_requested_ = true;
      continue;
    }
    if (event.type == SDL_EVENT_WINDOW_RESIZED && !scale_to_window_) {
      ApplyLogicalPresentation();
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      SDL_RenderCoordinatesFromWindow(renderer_.get(),
                                      event.motion.x,
                                      event.motion.y,
                                      &mouse_position_.x,
                                      &mouse_position_.y);
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
      SDL_RenderCoordinatesFromWindow(renderer_.get(),
                                      event.button.x,
                                      event.button.y,
                                      &mouse_position_.x,
                                      &mouse_position_.y);
      return 'm';
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
      if (event.key.key == SDLK_F5) {
        ToggleScale();
        continue;
      }
      switch (event.key.key) {
      case SDLK_N:
        return 'n';
      case SDLK_O:
        return 'o';
      case SDLK_P:
        return 'p';
      case SDLK_A:
        return 'a';
      case SDLK_Q:
      case SDLK_ESCAPE:
        return 'q';
      default:
        break;
      }
    }
  }
  return std::nullopt;
}

bool SdlPlatform::quit_requested() const { return quit_requested_; }

std::uint64_t SdlPlatform::ticks_ms() const { return SDL_GetTicks(); }

// mouse_position_ is already in render (viewport-relative) coordinates: it is
// filled via SDL_RenderCoordinatesFromWindow, which subtracts the current
// renderer viewport origin (the centring offset on fixed screens) whereas a
// letterboxed scale-mode presentation maps through SDL's logical src/dst rects.
// So it is returned unchanged for hit-testing.
SDL_FPoint SdlPlatform::mouse_position() const { return mouse_position_; }
