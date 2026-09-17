#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include "probe_server.hpp"

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
  // keyboard so modal loops that only read PollTextEvent can also act on a
  // click (the intro cinematic's in-rect local_19 frame advance, dialogs'
  // OK/default handling). The menu's PollCommandEvent channel reports the same
  // press as 'm'.
  primary,
};

struct TextInput {
  TextKey key = TextKey::none;
  char character = '\0'; // valid when key == TextKey::character
  // Original EV Nova normalized physical-key code (mostly DIK-style, with
  // navigation and right-side modifiers in the compact 0x60..0x6f range).
  // 0xffff means that this event has no bindable key code.
  std::uint16_t key_code = 0xffff;
  // Shift state at the moment of the event. The original reads this from the
  // per-poll modifier word (FUN_004cea20) to select the landed-store quantity
  // prompt (0x0048ea70 local_652 & 0x800).
  bool shift = false;
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
  // Held afterburner command, Z (the original's binding slot 0x18 default,
  // DIK 0x2c).
  bool afterburner = false;
  // Primary fire: space (hold to keep firing all primary banks). Mirrors the
  // original's primary-fire command (binding slot 2, default DIK 0x39 =
  // space) read through NovaInput_IsCommandActiveWithGameplayGuards.
  bool fire = false;
  // Secondary fire (hold): Left Ctrl, the original's binding slot 3 default
  // (DIK 0x1d). Fires the currently selected secondary bank
  // (active_weapon_bank_slot).
  bool fire_secondary = false;
  // Cycle the selected secondary weapon: X (next), Shift+X (previous). The
  // original binds slot 0 to DIK 0x11 = W with its 0x38/0x6f Shift pair as
  // the backwards modifier; W/S are the port's thrust/reverse keys
  // (documented divergence), so X stands in. Edge-resolved by the flight loop
  // against g_playerSecondaryCycleCommandLatch (DAT_007cab42).
  bool cycle_secondary = false;
  bool cycle_secondary_backwards = false;
  // Deselect the secondary weapon: C (the original's slot 1 default is
  // DIK 0x1f = S, taken by reverse; documented divergence).
  bool clear_secondary = false;
  // Eject command: Alt+X. The original eject (PlayerTick eject block,
  // Ship_HandlePlayerShipCore 0x004510b9) requires the 0x38/0x6f arm-modifier
  // pair (Left Alt) plus binding slot 0x11, whose default is DIK 0x2d = X;
  // plain/Shift+X are taken by the secondary-cycle bindings here. Only
  // consumed while the player ship is destroyed and owns an auto-eject outfit.
  bool eject = false;
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
  // ticker-text / Spaceport path in Stellar_HandleStellarEntryAndExit instead
  // of opening the target-action interaction dialog.
  bool land = false;
  // Edge-triggered target-action command: 'e' opens the destination-
  // interaction window for the currently targeted stellar. It is separate
  // from both physical stellar collision and the later docked UI path.
  bool target_action = false;
  // Edge-triggered board command: 'b' runs Player_HandleBoardTargetCommand
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
  // Face-target command (held): 'r'. The original's binding slot 7 default is
  // DIK 0x1e = A, which the port already uses for turn-left; R is the
  // documented clean-room stand-in. While held (with the ship not disabled)
  // the ship auto-steers toward the primary ship target, or toward the
  // selected travel stellar when no ship is targeted or the Alt arm modifier
  // is held, until aligned.
  bool face_target = false;
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
  struct OpenFileDialogResult {
    std::optional<std::filesystem::path> path;
    std::string error;
  };

  SdlPlatform() = default;
  ~SdlPlatform();

  SdlPlatform(const SdlPlatform &) = delete;
  SdlPlatform &operator=(const SdlPlatform &) = delete;
  SdlPlatform(SdlPlatform &&) = delete;
  SdlPlatform &operator=(SdlPlatform &&) = delete;

  [[nodiscard]] bool Initialize();
  [[nodiscard]] SDL_Renderer *renderer() const;
  // SDL3's native chooser is asynchronous and may invoke its callback from a
  // worker thread. Results are copied into platform-owned state and consumed
  // by the main loop through PollOpenFileDialogResult.
  [[nodiscard]] bool ShowOpenPilotFileDialog();
  [[nodiscard]] std::optional<OpenFileDialogResult> PollOpenFileDialogResult();
  [[nodiscard]] std::optional<char> PollCommandEvent();
  // Raw editable-key event for modal dialogs. Enter/Escape/Backspace are
  // returned as distinct TextKey values; printable keys return translated
  // ASCII and non-printable physical keys use TextKey::physical. Quit still
  // sets quit_requested_.
  [[nodiscard]] std::optional<TextInput> PollTextEvent();
  // Live keyboard-state flight control snapshot (held-key steering).
  [[nodiscard]] FlightInput PollFlightInput();
  // Live held-state of an original DIK-style key code (the values stored in
  // KeyBindings::cmd_to_key, plus fixed inputs such as escort groups 1..5).
  // Covers the codes the current consumers use; unmapped codes return false.
  [[nodiscard]] bool IsOriginalKeyCodeHeld(std::uint16_t key_code);
  [[nodiscard]] bool quit_requested() const;
  // Host wall-clock time used by presentation/UI domains. Gameplay code must
  // use gameplay_ticks_ms(), so a probe-controlled clock can replace the
  // simulation domain without changing SDL or modal timing.
  [[nodiscard]] std::uint64_t wall_ticks_ms() const;
  // Clock owned by the platform/runtime for gameplay timestamps. It currently
  // follows the wall clock; probe acceleration can scale this domain while
  // leaving genuine wall-clock effects alone.
  [[nodiscard]] std::uint64_t gameplay_ticks_ms() const;

  // Compatibility name for presentation code that has not yet been classified
  // as a wall-clock or gameplay domain.
  [[nodiscard]] std::uint64_t ticks_ms() const { return wall_ticks_ms(); }

  // Shared loop pacing. Normal execution preserves the historical 16-ms
  // yield; probe-armed accelerated execution returns immediately.
  void PaceFrame();
  // Execution settings intentionally have no CLI/preferences surface: they
  // are applied only after a probe command has been consumed on the main
  // thread.
  void ApplyProbeExecutionSettings(bool enabled,
                                   std::uint32_t speed_multiplier,
                                   bool suppress_audio = false);

  void SetProbeAudioSuppressionHandler(std::function<void(bool)> handler) {
    probe_audio_suppression_handler_ = std::move(handler);
  }

  [[nodiscard]] bool accelerated() const { return accelerated_; }

  [[nodiscard]] std::uint32_t speed_multiplier() const {
    return speed_multiplier_;
  }

  [[nodiscard]] SDL_FPoint mouse_position() const;

  // Frame-boundary hook: captures a pending probe screenshot (while the
  // current frame is still the active render target), counts step frames,
  // then presents. Every game loop must present through this instead of
  // calling SDL_RenderPresent directly (see docs/probe_harness.md).
  void Present();

  // The external probe harness (inert unless EVN_PROBE=1). See
  // docs/probe_harness.md; NovaApp_Run registers the state provider here.
  [[nodiscard]] ProbeServer &probe() { return probe_; }

  // UI layout registry: the active modal publishes its named control rects
  // (window-point space) so the harness can click by intent. No-op without
  // the harness; pair with ProbeUiAutoClear so a closing modal leaves nothing
  // stale.
  void PublishProbeUi(std::string window_name,
                      std::vector<std::pair<std::string, SDL_FRect>> rects) {
    std::vector<ProbeNamedRect> named;
    named.reserve(rects.size());
    for (auto &entry : rects) {
      ProbeNamedRect named_entry;
      named_entry.name = std::move(entry.first);
      named_entry.rect = entry.second;
      named.push_back(std::move(named_entry));
    }
    probe_.PublishUi(std::move(window_name), std::move(named));
  }

  // Rich variant for list rows: entries marked `has_value` also surface in
  // /probe/ui's "items" array with their label and numeric payload (e.g. a
  // trade center commodity row's name and price).
  void PublishProbeUiItems(std::string window_name,
                           std::vector<ProbeNamedRect> entries) {
    probe_.PublishUi(std::move(window_name), std::move(entries));
  }

  void ClearProbeUi() { probe_.ClearUi(); }

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

  // Pushes fresh window geometry to the probe, then services the harness.
  void PumpProbe();

  // DIVERGENCE(original): temporary test hook that maps the original x2 mode
  // toggle key (Caps Lock by default; EVNova.ini key_x2mode, default 0x14 =
  // VK_CAPITAL) onto the probe speed multiplier. See the definition.
  void ServiceX2SpeedDivergence();

  bool accelerated_ = false;
  bool x2_speed_divergence_active_ = false;
  std::uint32_t speed_multiplier_ = 1;
  std::uint64_t gameplay_clock_anchor_ms_ = 0;
  std::uint64_t wall_clock_anchor_ms_ = 0;
  std::function<void(bool)> probe_audio_suppression_handler_;

  bool sdl_initialized_ = false;
  bool quit_requested_ = false;
  ProbeServer probe_;
  Presentation presentation_ = Presentation::kFullscreen;
  SDL_FPoint mouse_position_{};
  SDL_FPoint mouse_window_point_{};
  std::unique_ptr<SDL_Window, WindowDeleter> window_;
  std::unique_ptr<SDL_Renderer, RendererDeleter> renderer_;
  std::mutex open_file_dialog_mutex_;
  bool open_file_dialog_pending_ = false;
  bool open_file_dialog_completed_ = false;
  OpenFileDialogResult open_file_dialog_result_;
};

// Clears the probe UI layout registry at scope exit so a closed modal never
// leaves stale clickable rects behind (see SdlPlatform::PublishProbeUi).
struct ProbeUiAutoClear {
  SdlPlatform &platform;

  explicit ProbeUiAutoClear(SdlPlatform &platform_ref)
      : platform(platform_ref) {}

  ~ProbeUiAutoClear() { platform.ClearProbeUi(); }

  ProbeUiAutoClear(const ProbeUiAutoClear &) = delete;
  ProbeUiAutoClear &operator=(const ProbeUiAutoClear &) = delete;
};
