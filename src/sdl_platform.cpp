#include "sdl_platform.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
// Minimum window size: the user-facing baseline resolution. At this size the
// upscaled fixed screens render their 1024-native art at ~1:1.
constexpr int kMinimumWindowWidth = 1024;
constexpr int kMinimumWindowHeight = 768;
#if EVNOVA_ENABLE_PROBE
// Default port for the external probe harness (override with EVN_PROBE_PORT).
constexpr int kDefaultProbePort = 8190;
#endif

// Convert SDL's physical scancode to the DirectInput-style code stored in the
// original g_player_key_bindings table. Codes through 0x58 mostly retain the
// PC set-1/DIK numbering; navigation and right-side modifiers use the game's
// compact 0x60..0x6f normalized range from g_key_code_display_name_map.
[[nodiscard]] constexpr std::uint16_t OriginalKeyCode(SDL_Scancode scancode) {
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
    return 0x1c;
  case SDL_SCANCODE_LCTRL:
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
    return 0x60;
  case SDL_SCANCODE_UP:
    return 0x61;
  case SDL_SCANCODE_PAGEUP:
    return 0x62;
  case SDL_SCANCODE_LEFT:
    return 0x63;
  case SDL_SCANCODE_RIGHT:
    return 0x64;
  case SDL_SCANCODE_END:
    return 0x65;
  case SDL_SCANCODE_DOWN:
    return 0x66;
  case SDL_SCANCODE_PAGEDOWN:
    return 0x67;
  case SDL_SCANCODE_INSERT:
    return 0x68;
  case SDL_SCANCODE_DELETE:
    return 0x69;
  case SDL_SCANCODE_KP_ENTER:
    return 0x6a;
  case SDL_SCANCODE_RCTRL:
    return 0x6b;
  case SDL_SCANCODE_KP_DIVIDE:
    return 0x6c;
  case SDL_SCANCODE_PRINTSCREEN:
    return 0x6d;
  case SDL_SCANCODE_PAUSE:
    return 0x6e;
  case SDL_SCANCODE_RALT:
    return 0x6f;
  default:
    return 0xffff;
  }
}

static_assert(OriginalKeyCode(SDL_SCANCODE_UP) == 0x61);
static_assert(OriginalKeyCode(SDL_SCANCODE_LEFT) == 0x63);
static_assert(OriginalKeyCode(SDL_SCANCODE_RIGHT) == 0x64);
static_assert(OriginalKeyCode(SDL_SCANCODE_DOWN) == 0x66);
static_assert(OriginalKeyCode(SDL_SCANCODE_KP_ENTER) == 0x6a);
static_assert(OriginalKeyCode(SDL_SCANCODE_RCTRL) == 0x6b);
static_assert(OriginalKeyCode(SDL_SCANCODE_RALT) == 0x6f);
} // namespace

void ApplyPlacementToRenderer(SDL_Renderer *renderer,
                              const Placement &placement,
                              float pixel_density) {
  SDL_SetRenderLogicalPresentation(
      renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
  SDL_SetRenderScale(renderer,
                     placement.scale * pixel_density,
                     placement.scale * pixel_density);
  const SDL_Rect viewport = placement.ToRenderViewport();
  SDL_SetRenderViewport(renderer, &viewport);
}

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
  wall_clock_anchor_ms_ = SDL_GetTicks();
  gameplay_clock_anchor_ms_ = wall_clock_anchor_ms_;

  SDL_SetAppMetadata("Escape Velocity Nova", "0.1.0", "com.ambrosiasw.evnova");
  // Minimum window: 1024x768 (the user-facing baseline resolution, matching
  // the game's native 1024x768 canvas). The window opens at that minimum and
  // stays resizable so larger windows show more of the system in flight. A
  // high-density backing buffer keeps text sharp while each screen picks its
  // own authored placement per frame (see Placement and SetPlacement).
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
  SetPlacement(PlaceWindow(logical_playfield_size()));

  // External probe harness (docs/probe_harness.md): enabled only via the
  // environment; the game runs completely unmodified without it.
  if (std::getenv("EVN_PROBE") != nullptr) {
#if EVNOVA_ENABLE_PROBE
    int port = kDefaultProbePort;
    if (const char *requested = std::getenv("EVN_PROBE_PORT");
        requested != nullptr) {
      port = std::atoi(requested);
    }
    probe_.Start(port);
    probe_.SetQuitLatch([this] { quit_requested_ = true; });
#else
    NovaLog::Warn("EVN_PROBE is set but this build was configured with "
                  "EVNOVA_ENABLE_PROBE=OFF; the probe harness is inactive");
#endif
  }
  return true;
}

SDL_Renderer *SdlPlatform::renderer() const { return renderer_.get(); }

bool SdlPlatform::ShowOpenPilotFileDialog() {
  {
    std::scoped_lock lock(open_file_dialog_mutex_);
    if (open_file_dialog_pending_) {
      return false;
    }
    open_file_dialog_pending_ = true;
    open_file_dialog_completed_ = false;
    open_file_dialog_result_ = {};
  }

  static constexpr SDL_DialogFileFilter kPilotFilter{"EV Nova pilot files",
                                                     "plt"};
  SDL_ShowOpenFileDialog(
      [](void *userdata, const char *const *filelist, int) {
        auto &platform = *static_cast<SdlPlatform *>(userdata);
        std::scoped_lock lock(platform.open_file_dialog_mutex_);
        if (filelist == nullptr) {
          platform.open_file_dialog_result_.error = SDL_GetError();
        } else if (*filelist != nullptr) {
          platform.open_file_dialog_result_.path =
              std::filesystem::path(*filelist);
        }
        platform.open_file_dialog_pending_ = false;
        platform.open_file_dialog_completed_ = true;
      },
      this,
      window_.get(),
      &kPilotFilter,
      1,
      nullptr,
      false);
  return true;
}

bool SdlPlatform::ShowOpenInstallFileDialog() {
  {
    std::scoped_lock lock(open_file_dialog_mutex_);
    if (open_file_dialog_pending_) {
      return false;
    }
    open_file_dialog_pending_ = true;
    open_file_dialog_completed_ = false;
    open_file_dialog_result_ = {};
  }
  // No filters: a macOS filter that omits an "all files" entry can grey out
  // the CE .exe. The install root is the selected executable's folder.
  const SDL_PropertiesID props = SDL_CreateProperties();
  if (props == 0) {
    NovaLog::Warn("file chooser: SDL_CreateProperties failed: {}",
                  SDL_GetError());
    std::scoped_lock lock(open_file_dialog_mutex_);
    open_file_dialog_pending_ = false;
    return false;
  }
  SDL_SetStringProperty(
      props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Select EV Nova.exe");
  SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
  SDL_SetPointerProperty(
      props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window_.get());
  SDL_ShowFileDialogWithProperties(
      SDL_FILEDIALOG_OPENFILE,
      [](void *userdata, const char *const *filelist, int) {
        auto &platform = *static_cast<SdlPlatform *>(userdata);
        std::scoped_lock lock(platform.open_file_dialog_mutex_);
        if (filelist == nullptr) {
          platform.open_file_dialog_result_.error = SDL_GetError();
        } else if (*filelist != nullptr) {
          platform.open_file_dialog_result_.path =
              std::filesystem::path(*filelist);
        }
        platform.open_file_dialog_pending_ = false;
        platform.open_file_dialog_completed_ = true;
      },
      this,
      props);
  SDL_DestroyProperties(props);
  return true;
}

std::optional<SdlPlatform::OpenFileDialogResult>
SdlPlatform::PollOpenFileDialogResult() {
  std::scoped_lock lock(open_file_dialog_mutex_);
  if (!open_file_dialog_completed_) {
    return std::nullopt;
  }
  open_file_dialog_completed_ = false;
  // Native dialogs can leave their SDL parent behind another application
  // after the asynchronous callback completes, notably on macOS. This runs
  // on the main thread, where SDL window operations are permitted.
  SDL_RaiseWindow(window_.get());
  return std::move(open_file_dialog_result_);
}

void SdlPlatform::ApplyPlacement() {
  if (!renderer_) {
    return;
  }
  ApplyPlacementToRenderer(renderer_.get(), placement_, WindowPixelDensity());
}

void SdlPlatform::SetPlacement(Placement placement) {
  placement_ = placement.Canonicalized();
  mouse_position_ = placement_.ToAuthored(mouse_window_point_);
  ApplyPlacement();
}

void SdlPlatform::PushPlacement(const Placement &placement) {
  placement_stack_.push_back(placement_);
  placement_ = placement.Canonicalized();
  mouse_position_ = placement_.ToAuthored(mouse_window_point_);
  ApplyPlacement();
}

void SdlPlatform::PopPlacement() {
  if (placement_stack_.empty()) {
    NovaLog::Warn("placement stack underflow");
    return;
  }
  placement_ = placement_stack_.back();
  placement_stack_.pop_back();
  mouse_position_ = placement_.ToAuthored(mouse_window_point_);
  ApplyPlacement();
}

void SdlPlatform::RefreshPlacementAfterResize() {
  const SDL_FPoint window = logical_playfield_size();
  for (Placement &saved : placement_stack_) {
    saved = saved.Reflow(window).Canonicalized();
  }
  placement_ = placement_.Reflow(window).Canonicalized();
  mouse_position_ = placement_.ToAuthored(mouse_window_point_);
  ApplyPlacement();
}

void SdlPlatform::ApplyWindowMode(bool windowed) {
  if (!window_) {
    return;
  }
  SDL_SetWindowFullscreen(window_.get(), !windowed);
}

SDL_FPoint SdlPlatform::logical_playfield_size() const {
  // The extending world tracks the window-coordinate size. The renderer scale
  // maps these coordinates to the high-density backing pixels.
  // Fixed screens use this as the containing window for their authored size.
  int w = kMinimumWindowWidth;
  int h = kMinimumWindowHeight;
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
  return placement_.scale * WindowPixelDensity();
}

// Pushes fresh window geometry to the probe, then services the harness
// (queued jobs, injected events, pause latch).
void SdlPlatform::PumpProbe() {
  int window_width = 0;
  int window_height = 0;
  SDL_GetWindowSize(window_.get(), &window_width, &window_height);
  probe_.SetGeometry(
      {static_cast<float>(window_width), static_cast<float>(window_height)},
      playfield_window_rect());
  const std::uint64_t pump_started_ms = wall_ticks_ms();
  const bool waited_while_paused = probe_.Pump();
  if (waited_while_paused) {
    // Pausing is a probe execution-control operation, not simulated elapsed
    // time. Move the wall anchor forward so resuming cannot produce one large
    // gameplay delta (scaled or otherwise).
    wall_clock_anchor_ms_ += wall_ticks_ms() - pump_started_ms;
  }
  bool enabled = false;
  bool suppress_audio = false;
  std::uint32_t speed_multiplier = 1;
  if (probe_.ConsumeAccelerationRequest(
          enabled, speed_multiplier, suppress_audio)) {
    ApplyProbeExecutionSettings(enabled, speed_multiplier, suppress_audio);
  }
  ServiceX2SpeedDivergence();
}

// DIVERGENCE(original): the original's x2 mode is toggled by the key named in
// ddraw.ini [EV Nova] key_x2mode, default "0x14" = VK_CAPITAL (Caps Lock)
// (Settings_LoadIniAndPrefs 0x00872310; Settings_PollKeyX2Mode 0x00872375
// forwards the code to GetKeyState and the caller tests the low/toggle bit).
// Engaged x2 doubles the per-frame simulation cadence rather than the wall
// clock (docs/frame_timing_and_cadence.md). The port has not reconstructed x2.
// As a temporary testing convenience this maps the same toggle onto the probe
// speed multiplier so a run can be accelerated 2x without the HTTP control
// surface. It is gated on the probe being active so ordinary play is
// unchanged, and it is not x2 fidelity: the whole gameplay clock is scaled,
// including the maintenance work the original leaves at the normal cadence.
// Remove once the real x2 scheduling is ported.
void SdlPlatform::ServiceX2SpeedDivergence() {
  if (!probe_.running()) {
    return;
  }
  const bool engaged = (SDL_GetModState() & SDL_KMOD_CAPS) != 0;
  if (engaged == x2_speed_divergence_active_) {
    return;
  }
  x2_speed_divergence_active_ = engaged;
  NovaLog::Warn("probe: x2 key divergence {} ({}x gameplay clock)",
                engaged ? "engaged" : "released",
                engaged ? 2 : 1);
  ApplyProbeExecutionSettings(engaged, 2, false);
}

std::optional<TextInput> SdlPlatform::PollTextEvent() {
  PumpProbe();
  SDL_Event event;
  const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      quit_requested_ = true;
      continue;
    }
    if (event.type == SDL_EVENT_WINDOW_RESIZED ||
        event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
      RefreshPlacementAfterResize();
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      mouse_window_point_ = {event.motion.x, event.motion.y};
      mouse_position_ = placement_.ToAuthored(mouse_window_point_);
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
      mouse_window_point_ = {event.button.x, event.button.y};
      mouse_position_ = placement_.ToAuthored(mouse_window_point_);
      return TextInput{TextKey::primary, '\0', 0xffff, alt};
    }
    if (event.type == SDL_EVENT_KEY_DOWN) {
      // OS key repeats are surfaced, not dropped: the original's event poll
      // routes Mac autoKey (type 5) through the same handlers as key-down
      // (type 3), e.g. the text-reader/offer scroll arms at 0x00499440 and
      // 0x00447170. `repeat` lets a one-shot caller distinguish if needed.
      const bool repeat = event.key.repeat;
      switch (event.key.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return TextInput{TextKey::enter,
                         '\0',
                         OriginalKeyCode(event.key.scancode),
                         false,
                         repeat};
      case SDLK_ESCAPE:
        return TextInput{TextKey::escape,
                         '\0',
                         OriginalKeyCode(event.key.scancode),
                         false,
                         repeat};
      case SDLK_BACKSPACE:
        return TextInput{TextKey::backspace,
                         '\0',
                         OriginalKeyCode(event.key.scancode),
                         false,
                         repeat};
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
                         OriginalKeyCode(event.key.scancode),
                         alt,
                         repeat};
      }
      if (const auto key_code = OriginalKeyCode(event.key.scancode);
          key_code != 0xffff) {
        return TextInput{TextKey::physical, '\0', key_code, false, repeat};
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
  input.mouse_x = mouse_position_.x;
  input.mouse_y = mouse_position_.y;
  input.window_mouse_x = mouse_window_point_.x;
  input.window_mouse_y = mouse_window_point_.y;
  PumpProbe();
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      quit_requested_ = true;
      continue;
    }
    if (event.type == SDL_EVENT_WINDOW_RESIZED ||
        event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
      RefreshPlacementAfterResize();
      input.mouse_x = mouse_position_.x;
      input.mouse_y = mouse_position_.y;
      input.window_mouse_x = mouse_window_point_.x;
      input.window_mouse_y = mouse_window_point_.y;
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      mouse_window_point_ = {event.motion.x, event.motion.y};
      const SDL_FPoint point = placement_.ToAuthored(mouse_window_point_);
      mouse_position_ = point;
      input.mouse_x = point.x;
      input.mouse_y = point.y;
      input.window_mouse_x = mouse_window_point_.x;
      input.window_mouse_y = mouse_window_point_.y;
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
      mouse_window_point_ = {event.button.x, event.button.y};
      const SDL_FPoint point = placement_.ToAuthored(mouse_window_point_);
      mouse_position_ = point;
      input.mouse_x = point.x;
      input.mouse_y = point.y;
      input.window_mouse_x = mouse_window_point_.x;
      input.window_mouse_y = mouse_window_point_.y;
      input.primary_clicked = true;
      continue;
    }
  }
  const bool *const keys = SDL_GetKeyboardState(nullptr);
  // Virtual held keys from the probe harness (SDL_GetKeyboardState cannot see
  // injected events), merged into the same channel as the physical keys.
  const auto pressed = [&](SDL_Scancode scancode) {
    return keys[scancode] != 0 || probe_.VirtualKey(scancode);
  };
  // Only the clean-room fixed keys live here. Every original binding-table
  // command (movement, fire, target, land/dismiss, travel, cycle_ship_target,
  // nearest-target, face-target, eject, ...) is left false and filled by the
  // spaceflight loop from `NovaInput_IsCommandActive`.
  //
  // Tab cycles the in-system stellar target (Shift = backwards).
  input.cycle_target_next = pressed(SDL_SCANCODE_TAB) &&
                            !pressed(SDL_SCANCODE_LSHIFT) &&
                            !pressed(SDL_SCANCODE_RSHIFT);
  input.cycle_target_previous =
      pressed(SDL_SCANCODE_TAB) &&
      (pressed(SDL_SCANCODE_LSHIFT) || pressed(SDL_SCANCODE_RSHIFT));
  // Ship_FindNext/PreviousPlayerCycleTarget reads the raw DIK codes 0x1d/0x6b
  // (Left/Right Ctrl), so holding either Ctrl restricts the ship cycle to the
  // player's own squad/escorts. Do not reuse the 0x38/0x6f Alt arm modifier
  // here: Alt already forces the face-target/stellar channels and would
  // silently flip the cycle into its escort half.
  input.cycle_ship_escorts =
      pressed(SDL_SCANCODE_LCTRL) || pressed(SDL_SCANCODE_RCTRL);
  return input;
}

bool SdlPlatform::IsOriginalKeyCodeHeld(std::uint16_t key_code) {
  const bool *const keys = SDL_GetKeyboardState(nullptr);
  const auto held = [&](SDL_Scancode scancode) {
    return keys[scancode] != 0 || probe_.VirtualKey(scancode);
  };
  // SDL's enum leaves 1..3 unnamed between SDL_SCANCODE_UNKNOWN and
  // SDL_SCANCODE_A; those gaps have no OriginalKeyCode mapping (the switch
  // returns 0xffff), so start at the first named scancode and avoid the
  // out-of-range cast.
  for (int value = SDL_SCANCODE_A; value < SDL_SCANCODE_COUNT; ++value) {
    const auto scancode = static_cast<SDL_Scancode>(value);
    if (OriginalKeyCode(scancode) == key_code) {
      return held(scancode);
    }
  }
  return false;
}

void SdlPlatform::Present() {
  // Capture while the frame about to be swapped in is still the active render
  // target; SDL_RenderReadPixels after SDL_RenderPresent reads an undefined
  // backbuffer under the GPU backends.
  probe_.OnPresent(renderer_.get());
  SDL_RenderPresent(renderer_.get());
}

std::optional<char> SdlPlatform::PollCommandEvent() {
  PumpProbe();
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) {
      quit_requested_ = true;
      continue;
    }
    if (event.type == SDL_EVENT_WINDOW_RESIZED ||
        event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
      RefreshPlacementAfterResize();
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      mouse_window_point_ = {event.motion.x, event.motion.y};
      mouse_position_ = placement_.ToAuthored(mouse_window_point_);
      continue;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
      mouse_window_point_ = {event.button.x, event.button.y};
      mouse_position_ = placement_.ToAuthored(mouse_window_point_);
      return 'm';
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
      switch (event.key.key) {
      case SDLK_N:
        return 'n';
      case SDLK_O:
        return 'o';
      case SDLK_E:
        return 'e';
      case SDLK_P:
        return 'p';
      case SDLK_A:
        return 'a';
      // Ghidra 0x004872a0 'x': ACKNOWLEDGEMENTS reader (d\x91sc 0x7ffe).
      case SDLK_X:
        return 'x';
      case SDLK_Q:
        return 'q';
      default:
        break;
      }
    }
  }
  return std::nullopt;
}

bool SdlPlatform::quit_requested() const { return quit_requested_; }

std::uint64_t SdlPlatform::wall_ticks_ms() const { return SDL_GetTicks(); }

std::uint64_t SdlPlatform::gameplay_ticks_ms() const {
  const std::uint64_t wall_elapsed = wall_ticks_ms() - wall_clock_anchor_ms_;
  return gameplay_clock_anchor_ms_ + wall_elapsed * speed_multiplier_;
}

void SdlPlatform::PaceFrame() {
  if (!accelerated_) {
    SDL_Delay(16);
  }
}

void SdlPlatform::ApplyProbeExecutionSettings(bool enabled,
                                              std::uint32_t speed_multiplier,
                                              bool suppress_audio) {
  // Re-anchor before changing scale so gameplay time stays monotonic across
  // enable, multiplier-change, and disable requests.
  const std::uint64_t gameplay_now = gameplay_ticks_ms();
  const std::uint64_t wall_now = wall_ticks_ms();
  gameplay_clock_anchor_ms_ = gameplay_now;
  wall_clock_anchor_ms_ = wall_now;
  accelerated_ = enabled;
  speed_multiplier_ =
      enabled ? std::max<std::uint32_t>(1, speed_multiplier) : 1;
  if (probe_audio_suppression_handler_) {
    probe_audio_suppression_handler_(enabled && suppress_audio);
  }
  if (accelerated_) {
    SDL_SetRenderVSync(renderer_.get(), 0);
    NovaLog::Info("probe: accelerated mode enabled ({}x)", speed_multiplier_);
  } else {
    SDL_SetRenderVSync(renderer_.get(), 1);
    NovaLog::Info("probe: accelerated mode disabled");
  }
}

// mouse_position_ is authored-space coordinates derived from the cached raw
// window point and the current placement. Recomputing it on placement changes
// keeps hit-testing correct even when the cursor does not move.
SDL_FPoint SdlPlatform::mouse_position() const { return mouse_position_; }

bool SdlPlatform::PrimaryMouseDown() const {
  return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
}

SDL_FPoint SdlPlatform::mouse_window_point() const {
  return mouse_window_point_;
}

SDL_FRect SdlPlatform::playfield_window_rect() const { return placement_.dst; }
