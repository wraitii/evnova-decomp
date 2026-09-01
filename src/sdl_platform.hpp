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
  // A non-printable physical key (arrows, function keys, modifiers, etc.)
  // carrying only key_code. Text-entry modals ignore this channel; the Key
  // Settings modal uses it to capture the complete physical-key set.
  physical,
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
  // Original EV Nova key code (DIK-style for physical keys, with the four
  // legacy flight letters represented as ASCII by their preference defaults).
  // 0xffff means that this event has no bindable key code.
  std::uint16_t key_code = 0xffff;
};

// Continuous flight-input snapshot polled once per frame from the live
// keyboard state (not edge events, so holding a key steers continuously).
// Mirrors the player-control channel the original reads through the primary
// input driver in Frame_SpaceflightLoop scope 3 (Ship_HandlePlayerShipCore).
struct FlightInput {
  bool turn_left = false;  // left / 'a'
  bool turn_right = false; // right / 'd'
  bool thrust = false;     // up / 'w' (accelerate toward heading)
  bool reverse = false;    // down / 's' (turn ship to fly backward)
  // Held afterburner command. The original maps this through the configurable
  // gameplay-command table; this clean-room binding uses Ctrl so it remains
  // independent of target cycling (Shift+Tab).
  bool afterburner = false;
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
  // Edge-triggered galaxy-map command: 'm' opens the starmap modal. Mirrors
  // the original's map command checked by Ship_HandlePlayerShip (0x0044b120)
  // through NovaInput_IsCommandActiveWithGameplayGuards; this build binds it to
  // a single key distinct from the steer/travel/target inputs.
  bool starmap = false;
  // Edge-triggered active-missions command: 'i' opens the mission-info
  // ("mission computer") window listing the pilot's active missions. Mirrors
  // gameplay command 0x28 in Ship_HandlePlayerShipCore (0x0044aa70), whose
  // default binding is DIK 0x17 = I (NovaPrefs_ResetKeyBindings 0x004b4400).
  bool mission_info = false;
  // Edge-triggered normal arrival command: Return. When the currently selected
  // ordinary stellar is inside the 250-unit arrival envelope, this follows the
  // ticker-text / Spaceport path in Stellar_ProcessTravelAndLanding instead of
  // opening the target-action interaction dialog.
  bool land = false;
  // Edge-triggered target-action command: 'e' opens the destination-
  // interaction window for the currently targeted stellar. It is separate
  // from both physical stellar collision and the later docked UI path.
  bool target_action = false;
  // Edge-triggered board command: 'b' runs Ship_HandlePlayerBoardTargetCommand
  // (0x0045a3d0) against the primary ship target — disabled-ship validation
  // (range / relative velocity / heading / crew) and the boarding-plunder
  // window. Mirrors the original's g_playerBoardTargetCommandLatch channel.
  bool board = false;
  // Cycle the stellar target. This is a clean-room binding for the original's
  // target-selection command channel; Tab advances through the current
  // system's eligible stellars and Shift+Tab goes backwards.
  bool cycle_target_next = false;
  bool cycle_target_previous = false;
  // Cycle the destination SYSTEM for the next jump: Backslash cycles forward
  // through the systems directly linked to (jumpable from) the current system,
  // Shift+Backslash backwards. Mirrors the original's command 0x60
  // (g_playerCycleTravelTargetCommandLatch) read by Ship_HandlePlayerShip,
  // whose default binding the EV Nova manual describes as "press the
  // Backslash key until the name of your desired destination system appears"
  // (after entering hyperspace mode with H). Distinct from cycle_target_next
  // (Tab), which cycles stellars within the system.
  bool cycle_destination_next = false;
  bool cycle_destination_previous = false;
  // Hyperspace-mode toggle (H): arms the in-flight destination-system
  // selection channel so leading Backslash cycles choose a jump system
  // (manual: "press the H key to set your ship's computer to hyperspace
  // mode. Then, press the Backslash key until the desired destination...").
  bool hyperspace_mode = false;
  // Cycle the ship (primary) target: backquote (`), Shift+backquote backwards
  // (the original's default binding per the EV Nova manual: "press the ` key
  // until the desired ship is selected"). When cycle_ship_include_combat is
  // held (Alt or 'k') the cycle restricts itself to combat-relevant ships
  // (ships targeting the player or a player-targeting ship), mirroring the
  // original's modifier commands 0x1d (Left Ctrl) / 0x6b ('k'); the clean-room
  // binding uses Alt to match the manual's description and stay clear of the
  // afterburner Ctrl binding.
  bool cycle_ship_target_next = false;
  bool cycle_ship_target_previous = false;
  bool cycle_ship_include_combat = false;
  // Select the nearest hostile combat target ('o'), or the nearest engaged
  // target (Alt+'o'). Mirrors the original's "target nearest" command whose
  // default arm selects Ship_SelectNearestHostileCombatTarget and whose
  // 0x38/0x6f modifiers select Ship_SelectNearestEngagedTarget.
  bool select_nearest_hostile = false;
  bool select_nearest_engaged = false;
  // Edge latches from drained SDL events the loop otherwise could not see
  // (PollFlightInput owns the event drain). escape_pressed latches Escape or
  // 'q' keydown; primary_clicked latches a left mouse press with the current
  // render-coordinate cursor position, used for click-to-target ship picking.
  bool escape_pressed = false;
  bool primary_clicked = false;
  float mouse_x = 0.0F;
  float mouse_y = 0.0F;
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
  // returned as distinct TextKey values; printable keys return translated
  // ASCII and non-printable physical keys use TextKey::physical. Quit still
  // sets quit_requested_.
  [[nodiscard]] std::optional<TextInput> PollTextEvent();
  // Live keyboard-state flight control snapshot (held-key steering).
  [[nodiscard]] FlightInput PollFlightInput();
  [[nodiscard]] bool quit_requested() const;
  [[nodiscard]] std::uint64_t ticks_ms() const;
  [[nodiscard]] SDL_FPoint mouse_position() const;

  // The current window->content presentation policy for this frame. See
  // SetCenteredPlayfield / SetScaledPlayfield / SetFullscreenPlayfield.
  enum class Presentation { kCentered, kScaled, kFullscreen };

  // The full window size in logical draw coordinates. World/spaceflight
  // drawing queries this to extend to the
  // (possibly larger) window; it is independent of whether fixed screens are
  // being upscaled or bordered.
  [[nodiscard]] SDL_FPoint logical_playfield_size() const;

  // Physical output pixels occupied by one current render-coordinate unit.
  // Text uses this to rasterize at the destination resolution, then draws the
  // resulting texture at its unchanged logical size.
  [[nodiscard]] float text_raster_scale() const;

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
  //    size, centred in the window with black bars on every side. Retina
  //    backing pixels increase detail without changing its physical size.
  //  * SetFullscreenPlayfield() -- the free-flight world spans the whole
  //    window in window-coordinate units (no clipping / fixed logical size),
  //    so larger windows show more of the system; HUD chrome stays fixed.
  //
  // SDL_RenderCoordinatesFromWindow reports the mouse in content coordinates
  // (viewport-relative / through the logical rect) when the corresponding
  // presentation is active, so hit-tests stay correct in all three modes.
  void SetCenteredPlayfield();
  void SetScaledPlayfield();
  void SetFullscreenPlayfield();

  // Raw mouse position in window coordinates (SDL window points), unmapped by
  // any presentation transform. The dialog runtime composites over the last
  // presented frame and draws at 1:1 window scale, so it maps the mouse
  // against the playfield's on-screen rect itself instead of relying on the
  // active presentation's coordinate mapping.
  [[nodiscard]] SDL_FPoint mouse_window_point() const;

  // Where the 640x480 logical content canvas currently sits on the window, in
  // window points (letterboxed dst rect in the scaled presentation, the
  // integer-centred panel in the centred one, the whole window in fullscreen).
  [[nodiscard]] SDL_FRect playfield_window_rect() const;

  // Transient drawing mode for modal windows that composite over the last
  // presented frame: logical presentation disabled, no viewport, 1 drawing
  // unit = 1 window point. Unlike the Set*Playfield modes this leaves
  // presentation_ (and therefore playfield_window_rect) untouched, so the
  // caller keeps seeing the underlying screen's geometry; the next
  // Set*Playfield call from the active screen restores its own state.
  void ApplyWindowPointDrawing();

private:
  // The 640x480 logical content canvas shared by the fixed screens. This is
  // scaled up to the window in kScaled presentation and clipped centred in
  // kCentered; the extending world ignores it and tracks the window size.
  void ApplyCenteredPresentation();
  void ApplyScaledPresentation();
  void ApplyFullscreenPresentation();

  [[nodiscard]] float WindowPixelDensity() const;

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
  SDL_FPoint mouse_window_point_{};
  std::unique_ptr<SDL_Window, WindowDeleter> window_;
  std::unique_ptr<SDL_Renderer, RendererDeleter> renderer_;
};
