#include "sdl_platform.hpp"
#include "log.hpp"

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
  window_.reset(
      SDL_CreateWindow("Escape Velocity Nova", 960, 720, SDL_WINDOW_RESIZABLE));
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
  SDL_SetRenderLogicalPresentation(
      renderer_.get(), 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
  return true;
}

SDL_Renderer *SdlPlatform::renderer() const { return renderer_.get(); }

std::optional<TextInput> SdlPlatform::PollTextEvent() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      quit_requested_ = true;
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
  return input;
}

std::optional<char> SdlPlatform::PollCommandEvent() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      quit_requested_ = true;
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

SDL_FPoint SdlPlatform::mouse_position() const { return mouse_position_; }
