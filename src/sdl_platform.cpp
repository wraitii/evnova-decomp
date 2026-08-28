#include "sdl_platform.hpp"
#include "log.hpp"

#include <algorithm>

namespace {
// The original renders onto a 1024x768 internal canvas which it then scales to
// its 640x480 window. We keep the same 4:3 content as a 640x480 logical
// playfield (see SdlPlatform::Apply*Presentation) and present a window with a
// 1024x768 minimum, so the fixed screens (which are upscaled, or kept centred
// with bars for the docked window) and the extending free-flight world all
// resolve at least as crisply as the original's native canvas.
constexpr int kPlayfieldWidth = 640;
constexpr int kPlayfieldHeight = 480;
// Minimum window size: the user-facing baseline resolution. At this size the
// upscaled fixed screens render their 1024-native art at ~1:1.
constexpr int kMinimumWindowWidth = 1024;
constexpr int kMinimumWindowHeight = 768;

// Convert SDL's physical scancode to the DirectInput-style code stored in the
// original g_player_key_bindings table. The Windows CE build used a small
// mixture of these scan codes and ASCII letters; keeping the physical mapping
// here gives the key-settings dialog one stable code path for both printable
// and non-printable keys.
[[nodiscard]] std::uint16_t OriginalKeyCode(SDL_Scancode scancode) {
  switch (scancode) {
  case SDL_SCANCODE_ESCAPE:
    return 0x01;
  case SDL_SCANCODE_1:
    return 0x02;
  case SDL_SCANCODE_2:
    return 0x03;
  case SDL_SCANCODE_3:
    return 0x04;
  case SDL_SCANCODE_4:
    return 0x05;
  case SDL_SCANCODE_5:
    return 0x06;
  case SDL_SCANCODE_6:
    return 0x07;
  case SDL_SCANCODE_7:
    return 0x08;
  case SDL_SCANCODE_8:
    return 0x09;
  case SDL_SCANCODE_9:
    return 0x0a;
  case SDL_SCANCODE_0:
    return 0x0b;
  case SDL_SCANCODE_MINUS:
    return 0x0c;
  case SDL_SCANCODE_EQUALS:
    return 0x0d;
  case SDL_SCANCODE_BACKSPACE:
    return 0x0e;
  case SDL_SCANCODE_TAB:
    return 0x0f;
  case SDL_SCANCODE_Q:
    return 0x10;
  case SDL_SCANCODE_W:
    return 0x11;
  case SDL_SCANCODE_E:
    return 0x12;
  case SDL_SCANCODE_R:
    return 0x13;
  case SDL_SCANCODE_T:
    return 0x14;
  case SDL_SCANCODE_Y:
    return 0x15;
  case SDL_SCANCODE_U:
    return 0x16;
  case SDL_SCANCODE_I:
    return 0x17;
  case SDL_SCANCODE_O:
    return 0x18;
  case SDL_SCANCODE_P:
    return 0x19;
  case SDL_SCANCODE_LEFTBRACKET:
    return 0x1a;
  case SDL_SCANCODE_RIGHTBRACKET:
    return 0x1b;
  case SDL_SCANCODE_RETURN:
  case SDL_SCANCODE_KP_ENTER:
    return 0x1c;
  case SDL_SCANCODE_LCTRL:
  case SDL_SCANCODE_RCTRL:
    return 0x1d;
  case SDL_SCANCODE_A:
    return 0x1e;
  case SDL_SCANCODE_S:
    return 0x1f;
  case SDL_SCANCODE_D:
    return 0x20;
  case SDL_SCANCODE_F:
    return 0x21;
  case SDL_SCANCODE_G:
    return 0x22;
  case SDL_SCANCODE_H:
    return 0x23;
  case SDL_SCANCODE_J:
    return 0x24;
  case SDL_SCANCODE_K:
    return 0x25;
  case SDL_SCANCODE_L:
    return 0x26;
  case SDL_SCANCODE_SEMICOLON:
    return 0x27;
  case SDL_SCANCODE_APOSTROPHE:
    return 0x28;
  case SDL_SCANCODE_GRAVE:
    return 0x29;
  case SDL_SCANCODE_LSHIFT:
    return 0x2a;
  case SDL_SCANCODE_BACKSLASH:
    return 0x2b;
  case SDL_SCANCODE_Z:
    return 0x2c;
  case SDL_SCANCODE_X:
    return 0x2d;
  case SDL_SCANCODE_C:
    return 0x2e;
  case SDL_SCANCODE_V:
    return 0x2f;
  case SDL_SCANCODE_B:
    return 0x30;
  case SDL_SCANCODE_N:
    return 0x31;
  case SDL_SCANCODE_M:
    return 0x32;
  case SDL_SCANCODE_COMMA:
    return 0x33;
  case SDL_SCANCODE_PERIOD:
    return 0x34;
  case SDL_SCANCODE_SLASH:
    return 0x35;
  case SDL_SCANCODE_RSHIFT:
    return 0x36;
  case SDL_SCANCODE_KP_MULTIPLY:
    return 0x37;
  case SDL_SCANCODE_LALT:
  case SDL_SCANCODE_RALT:
    return 0x38;
  case SDL_SCANCODE_SPACE:
    return 0x39;
  case SDL_SCANCODE_CAPSLOCK:
    return 0x3a;
  case SDL_SCANCODE_F1:
    return 0x3b;
  case SDL_SCANCODE_F2:
    return 0x3c;
  case SDL_SCANCODE_F3:
    return 0x3d;
  case SDL_SCANCODE_F4:
    return 0x3e;
  case SDL_SCANCODE_F5:
    return 0x3f;
  case SDL_SCANCODE_F6:
    return 0x40;
  case SDL_SCANCODE_F7:
    return 0x41;
  case SDL_SCANCODE_F8:
    return 0x42;
  case SDL_SCANCODE_F9:
    return 0x43;
  case SDL_SCANCODE_F10:
    return 0x44;
  case SDL_SCANCODE_NUMLOCKCLEAR:
    return 0x45;
  case SDL_SCANCODE_SCROLLLOCK:
    return 0x46;
  case SDL_SCANCODE_KP_7:
    return 0x47;
  case SDL_SCANCODE_KP_8:
    return 0x48;
  case SDL_SCANCODE_KP_9:
    return 0x49;
  case SDL_SCANCODE_KP_MINUS:
    return 0x4a;
  case SDL_SCANCODE_KP_4:
    return 0x4b;
  case SDL_SCANCODE_KP_5:
    return 0x4c;
  case SDL_SCANCODE_KP_6:
    return 0x4d;
  case SDL_SCANCODE_KP_PLUS:
    return 0x4e;
  case SDL_SCANCODE_KP_1:
    return 0x4f;
  case SDL_SCANCODE_KP_2:
    return 0x50;
  case SDL_SCANCODE_KP_3:
    return 0x51;
  case SDL_SCANCODE_KP_0:
    return 0x52;
  case SDL_SCANCODE_KP_PERIOD:
    return 0x53;
  case SDL_SCANCODE_F11:
    return 0x57;
  case SDL_SCANCODE_F12:
    return 0x58;
  case SDL_SCANCODE_HOME:
    return 0xc7;
  case SDL_SCANCODE_UP:
    return 0xc8;
  case SDL_SCANCODE_PAGEUP:
    return 0xc9;
  case SDL_SCANCODE_LEFT:
    return 0xcb;
  case SDL_SCANCODE_RIGHT:
    return 0xcd;
  case SDL_SCANCODE_END:
    return 0xcf;
  case SDL_SCANCODE_DOWN:
    return 0xd0;
  case SDL_SCANCODE_PAGEDOWN:
    return 0xd1;
  case SDL_SCANCODE_INSERT:
    return 0xd2;
  case SDL_SCANCODE_DELETE:
    return 0xd3;
  default:
    return 0xffff;
  }
}
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
  // Minimum window: 1024x768 (the user-facing baseline resolution, matching
  // the game's native 1024x768 canvas). The window opens at that minimum and
  // stays resizable so larger windows show more of the system in flight. A
  // high-density backing buffer keeps text sharp while each screen picks its
  // own presentation policy per frame (see the *Presentation helpers).
  window_.reset(
      SDL_CreateWindow("Escape Velocity Nova",
                       kMinimumWindowWidth,
                       kMinimumWindowHeight,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
  if (!window_) {
    NovaLog::Error("SDL window creation failed: {}", SDL_GetError());
    return false;
  }
  // Users may not shrink the game below its baseline resolution.
  SDL_SetWindowMinimumSize(
      window_.get(), kMinimumWindowWidth, kMinimumWindowHeight);

  renderer_.reset(SDL_CreateRenderer(window_.get(), nullptr));
  if (!renderer_) {
    NovaLog::Error("SDL renderer creation failed: {}", SDL_GetError());
    return false;
  }

  SDL_SetRenderVSync(renderer_.get(), 1);
  SetFullscreenPlayfield();
  return true;
}

SDL_Renderer *SdlPlatform::renderer() const { return renderer_.get(); }

void SdlPlatform::ApplyFullscreenPresentation() {
  // Extending free-flight world / fullscreen splash: 1 logical unit = 1
  // window-coordinate point, no clipping. On a high-density display the
  // renderer scale maps that unit to the backing pixels without changing how
  // much world fits in the window.
  presentation_ = Presentation::kFullscreen;
  SDL_SetRenderLogicalPresentation(
      renderer_.get(), 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
  SDL_SetRenderViewport(renderer_.get(), nullptr);
  const float density = WindowPixelDensity();
  SDL_SetRenderScale(renderer_.get(), density, density);
}

void SdlPlatform::ApplyScaledPresentation() {
  // Fixed screens (menu / splash / intro): uniformly upscale the 640x480
  // content canvas to fill the window (letterboxing the 4:3 aspect). At the
  // 1024x768 minimum this reads back at the native art resolution.
  presentation_ = Presentation::kScaled;
  SDL_SetRenderScale(renderer_.get(), 1.0F, 1.0F);
  SDL_SetRenderViewport(renderer_.get(), nullptr);
  SDL_SetRenderLogicalPresentation(renderer_.get(),
                                   kPlayfieldWidth,
                                   kPlayfieldHeight,
                                   SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

void SdlPlatform::ApplyCenteredPresentation() {
  // Docked/landed screen: native 1:1 window-coordinate size, centred in the
  // window with black bars on every side. A high-density backing buffer gives
  // each logical unit multiple physical pixels without making the panel
  // physically larger.
  presentation_ = Presentation::kCentered;
  SDL_SetRenderLogicalPresentation(
      renderer_.get(), 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
  const float density = WindowPixelDensity();
  SDL_SetRenderScale(renderer_.get(), density, density);
  int w = kPlayfieldWidth;
  int h = kPlayfieldHeight;
  SDL_GetWindowSize(window_.get(), &w, &h);
  // SDL applies the render scale to the viewport as well as draw coordinates,
  // so the viewport must stay in logical/window units. Supplying backing-pixel
  // dimensions here would multiply both its offset and extent by density a
  // second time.
  const int ox = std::max(0, w - kPlayfieldWidth) / 2;
  const int oy = std::max(0, h - kPlayfieldHeight) / 2;
  const SDL_Rect viewport{ox, oy, kPlayfieldWidth, kPlayfieldHeight};
  SDL_SetRenderViewport(renderer_.get(), &viewport);
}

void SdlPlatform::SetFullscreenPlayfield() { ApplyFullscreenPresentation(); }

void SdlPlatform::SetScaledPlayfield() { ApplyScaledPresentation(); }

void SdlPlatform::SetCenteredPlayfield() { ApplyCenteredPresentation(); }

SDL_FPoint SdlPlatform::logical_playfield_size() const {
  // The extending world tracks the window-coordinate size. The renderer scale
  // maps these coordinates to the high-density backing pixels.
  // (Fixed screens do not query this; they draw in the 640x480 content canvas
  // through their own presentation.)
  int w = kPlayfieldWidth;
  int h = kPlayfieldHeight;
  if (window_) {
    SDL_GetWindowSize(window_.get(), &w, &h);
  }
  return {static_cast<float>(w), static_cast<float>(h)};
}

float SdlPlatform::WindowPixelDensity() const {
  if (!window_) {
    return 1.0F;
  }
  const float density = SDL_GetWindowPixelDensity(window_.get());
  return density > 0.0F ? density : 1.0F;
}

float SdlPlatform::text_raster_scale() const {
  if (!renderer_) {
    return 1.0F;
  }
  if (presentation_ != Presentation::kScaled) {
    return WindowPixelDensity();
  }

  int output_width = kPlayfieldWidth;
  int output_height = kPlayfieldHeight;
  if (!SDL_GetRenderOutputSize(
          renderer_.get(), &output_width, &output_height)) {
    return WindowPixelDensity();
  }
  return std::max(
      1.0F,
      std::min(static_cast<float>(output_width) / kPlayfieldWidth,
               static_cast<float>(output_height) / kPlayfieldHeight));
}

std::optional<TextInput> SdlPlatform::PollTextEvent() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      quit_requested_ = true;
      continue;
    }
    if (event.type == SDL_EVENT_WINDOW_RESIZED) {
      // The presentation is chosen per-frame by the active screen (centred /
      // scaled / fullscreen); a resize just needs the menu/splash upscaled and
      // the docked-centric offset recomputed, which the next Set*Playfield()
      // call does. Nothing to do here beyond refreshing the logical rect for
      // the current presentation.
      if (presentation_ == Presentation::kScaled) {
        ApplyScaledPresentation();
      } else if (presentation_ == Presentation::kCentered) {
        ApplyCenteredPresentation();
      }
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
        return TextInput{
            TextKey::enter, '\0', OriginalKeyCode(event.key.scancode)};
      case SDLK_ESCAPE:
        return TextInput{
            TextKey::escape, '\0', OriginalKeyCode(event.key.scancode)};
      case SDLK_BACKSPACE:
        return TextInput{
            TextKey::backspace, '\0', OriginalKeyCode(event.key.scancode)};
      default:
        break;
      }
      // Any printable key symbols map to their ASCII value: letters and digits
      // use ASCII syms, while combined punctuation is approximated by its
      // scanned key's symbol (enough for callsign entry).
      const auto sym = static_cast<int>(event.key.key);
      if (sym >= 32 && sym < 127) {
        return TextInput{TextKey::character,
                         static_cast<char>(sym),
                         OriginalKeyCode(event.key.scancode)};
      }
      if (const auto key_code = OriginalKeyCode(event.key.scancode);
          key_code != 0xffff) {
        return TextInput{TextKey::physical, '\0', key_code};
      }
    }
  }
  return std::nullopt;
}

FlightInput SdlPlatform::PollFlightInput() {
  // Drain queued events first so the window stays responsive and the keyboard
  // state reflects the latest presses/releases. Then read the live key state
  // for the flight controls (edge-agnostic, so holding a key steers).
  FlightInput input;
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
                                      &input.mouse_x,
                                      &input.mouse_y);
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
      SDL_RenderCoordinatesFromWindow(renderer_.get(),
                                      event.button.x,
                                      event.button.y,
                                      &input.mouse_x,
                                      &input.mouse_y);
      input.primary_clicked = true;
      continue;
    }
    // Escape/'q' exit the flight loop; captured here because this function
    // drains the queue the old PollTextEvent-based check relied on.
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q)) {
      input.escape_pressed = true;
      continue;
    }
  }
  const bool *const keys = SDL_GetKeyboardState(nullptr);
  const auto pressed = [&](SDL_Scancode scancode) {
    return keys[scancode] != 0;
  };
  input.turn_left = pressed(SDL_SCANCODE_LEFT) || pressed(SDL_SCANCODE_A);
  input.turn_right = pressed(SDL_SCANCODE_RIGHT) || pressed(SDL_SCANCODE_D);
  input.thrust = pressed(SDL_SCANCODE_UP) || pressed(SDL_SCANCODE_W);
  input.reverse = pressed(SDL_SCANCODE_DOWN) || pressed(SDL_SCANCODE_S);
  input.afterburner =
      pressed(SDL_SCANCODE_LCTRL) || pressed(SDL_SCANCODE_RCTRL);
  input.travel = pressed(SDL_SCANCODE_J);
  input.starmap = pressed(SDL_SCANCODE_M);
  input.land = pressed(SDL_SCANCODE_RETURN) || pressed(SDL_SCANCODE_KP_ENTER);
  input.target_action = pressed(SDL_SCANCODE_E);
  input.board = pressed(SDL_SCANCODE_B);
  input.cycle_target_next = pressed(SDL_SCANCODE_TAB) &&
                            !pressed(SDL_SCANCODE_LSHIFT) &&
                            !pressed(SDL_SCANCODE_RSHIFT);
  input.cycle_target_previous =
      pressed(SDL_SCANCODE_TAB) &&
      (pressed(SDL_SCANCODE_LSHIFT) || pressed(SDL_SCANCODE_RSHIFT));
  // Shift is the shared direction modifier for the backward cycling commands.
  const bool shift_held =
      pressed(SDL_SCANCODE_LSHIFT) || pressed(SDL_SCANCODE_RSHIFT);
  const bool alt_held =
      pressed(SDL_SCANCODE_LALT) || pressed(SDL_SCANCODE_RALT);
  // Destination-SYSTEM cycling (the next-jump system): Backslash forwards,
  // Shift+Backslash backwards. This is the command the EV Nova manual binds
  // to Backslash for choosing the hyperspace destination system, distinct
  // from Tab's stellar cycle above.
  const bool backslash = pressed(SDL_SCANCODE_BACKSLASH);
  input.cycle_destination_next = backslash && !shift_held;
  input.cycle_destination_previous = backslash && shift_held;
  input.hyperspace_mode = pressed(SDL_SCANCODE_H);
  // Ship-target cycling: backquote (`) next, Shift+backquote backwards (the
  // original's direction modifiers are Left/Right Shift, 0x2a/0x36). Alt (or
  // the original's 'k', 0x6b) restricts the cycle to combat-relevant ships.
  input.cycle_ship_include_combat = alt_held || pressed(SDL_SCANCODE_K);
  input.cycle_ship_target_next = pressed(SDL_SCANCODE_GRAVE) && !shift_held;
  input.cycle_ship_target_previous = pressed(SDL_SCANCODE_GRAVE) && shift_held;
  // Nearest hostile/engaged target selection.
  input.select_nearest_hostile = pressed(SDL_SCANCODE_O) && !alt_held;
  input.select_nearest_engaged = pressed(SDL_SCANCODE_O) && alt_held;
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

// mouse_position_ is already in render (viewport-relative) coordinates: it is
// filled via SDL_RenderCoordinatesFromWindow, which subtracts the current
// renderer viewport origin (the centring offset on fixed screens) whereas a
// letterboxed scale-mode presentation maps through SDL's logical src/dst rects.
// So it is returned unchanged for hit-testing.
SDL_FPoint SdlPlatform::mouse_position() const { return mouse_position_; }
