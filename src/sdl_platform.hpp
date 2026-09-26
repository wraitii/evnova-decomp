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
#include <utility>
#include <vector>

#include "probe_server.hpp"
#include "util/placement.hpp"

#include "game/presentation_scale.hpp"

void ApplyPlacementToRenderer(SDL_Renderer *renderer,
                              const Placement &placement,
                              float pixel_density);

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
  // Alt/Option state at the moment of the event. The original reads this from
  // the per-poll modifier word (FUN_004cea20: keys 0x38/0x6f set 0x800) to
  // select the landed-store and trade-center quantity prompt (0x0048ea70
  // local_652 & 0x800; 0x0048c730 local_36 & 0x800).
  bool alt = false;
  // True for an OS-generated key repeat (Mac autoKey, the original's event
  // type 5). The original's event polls bind key-down and autoKey to the same
  // handlers (e.g. the text-reader callback 0x00499440), so most consumers
  // can ignore this; it exists for callers that must not re-fire a one-shot
  // action while a key is held.
  bool repeat = false;
};

// Continuous flight-input snapshot polled once per frame from the live
// keyboard state (not edge events, so holding a key steers continuously).
// Mirrors the player-control channel the original reads through the primary
// input driver in Frame_SpaceflightLoop scope 3 (Ship_HandlePlayerShipCore).
struct FlightInput {
  // Held flight commands. Resolved from the persisted binding table by the
  // spaceflight loop: turn-left slot 0x13 (default DIK 0x63 = Left), turn-right
  // slot 0x14 (0x64 = Right), thrust slot 0x15 (0x61 = Up; accelerates toward
  // heading), reverse slot 0x16 (0x66 = Down; turns the ship to fly backward).
  bool turn_left = false;
  bool turn_right = false;
  bool thrust = false;
  bool reverse = false;
  // Held afterburner command, binding slot 0x18 (default DIK 0x2c = Z).
  bool afterburner = false;
  // Primary fire: space (hold to keep firing all primary banks). Mirrors the
  // original's primary-fire command (binding slot 2, default DIK 0x39 =
  // space) read through NovaInput_IsCommandActiveWithGameplayGuards.
  bool fire = false;
  // Secondary fire (hold): Left Ctrl, the original's binding slot 3 default
  // (DIK 0x1d). Fires the currently selected secondary bank
  // (active_weapon_bank_slot).
  bool fire_secondary = false;
  // Cycle the selected secondary weapon: binding slot 0x00 (default DIK
  // 0x11 = W; Shift pair 0x2a/0x36 backwards). Resolved from the persisted
  // binding table by the spaceflight loop. Edge-resolved against
  // g_playerSecondaryCycleCommandLatch (DAT_007cab42).
  bool cycle_secondary = false;
  bool cycle_secondary_backwards = false;
  // Deselect the secondary weapon: binding slot 0x01 (default DIK 0x1f = S).
  // Resolved from the persisted binding table by the spaceflight loop.
  bool clear_secondary = false;
  // Eject command: the arm-modifier pair (0x38/0x6f = Alt) plus binding slot
  // 0x11, whose default is DIK 0x2d = X. Resolved from the persisted binding
  // table by the spaceflight loop. Consumed while the player ship is disabled
  // or destroyed (Ghidra eject block 0x00451024); a destroyed hull with an
  // owned auto-eject outfit ejects without the key once the death presentation
  // is past its gate.
  bool eject = false;
  // Edge-triggered travel engage: binding slot 0x0e (default DIK 0x24 = J),
  // hyperspace jump toward the nearest available travel point.
  bool travel = false;
  // Edge-triggered galaxy-map command: binding slot 0x09 (default DIK 0x32 =
  // M) opens the starmap modal. Mirrors the original's map command checked by
  // Ship_HandlePlayerShip (0x0044b120).
  bool starmap = false;
  // Edge-triggered active-missions command: 'i' opens the mission-info
  // ("mission computer") window listing the pilot's active missions. Mirrors
  // gameplay command 0x28 in Ship_HandlePlayerShipCore (0x0044aa70), whose
  // default binding is DIK 0x17 = I (NovaPrefs_ResetKeyBindings 0x004b4400).
  bool mission_info = false;
  // Edge-triggered normal arrival command: binding slot 0x05 (default DIK
  // 0x26 = L). When the currently selected ordinary stellar is inside the
  // 250-unit arrival envelope, this follows the ticker-text / Spaceport path
  // in Stellar_HandleStellarEntryAndExit instead of opening the target-action
  // interaction dialog.
  bool land = false;
  // Edge-triggered HUD/panel dismiss (binding slot 6, default DIK 0x1c =
  // Return; Ghidra Ship_HandlePlayerShipCore 0x00450ae7): clears the transient
  // HUD overlay message, dismisses the route-map overlay, and closes the Escort
  // Commands panel. Distinct from land (slot 5, default DIK 0x26 = L). Resolved
  // from the persisted binding table by the spaceflight loop.
  bool dismiss = false;
  // Edge-triggered target-action command (binding slot 0x04, default DIK
  // 0x15 = Y): opens the destination-interaction window for the currently
  // targeted stellar. Resolved from the persisted binding table by the
  // spaceflight loop, not from a fixed scancode. It is separate from both
  // physical stellar collision and the later docked UI path.
  bool target_action = false;
  // Edge-triggered clear-target command (binding slot 0x08, default DIK 0x31 =
  // N; Ghidra 0x0044B7C4/0x0044DDD5). With the 0x38/0x6f arm modifier it
  // clears the primary ship target; without it, it clears the travel/landing
  // selection and resets travel_transfer_mode. Resolved from the persisted
  // binding table by the spaceflight loop.
  bool clear_target = false;
  // Edge-triggered board command: binding slot 0x10 (default DIK 0x30 = B)
  // runs Player_HandleBoardTargetCommand (0x0045a3d0) against the primary
  // ship target — disabled-ship validation (range / relative velocity /
  // heading / crew) and the boarding-plunder window. Mirrors the original's
  // g_playerBoardTargetCommandLatch channel.
  bool board = false;
  // Cycle the stellar target. This is a clean-room binding for the original's
  // target-selection command channel; Tab advances through the current
  // system's eligible stellars and Shift+Tab goes backwards.
  bool cycle_target_next = false;
  bool cycle_target_previous = false;
  // Cycle the destination SYSTEM for the next jump: binding slot 0x0d
  // (default DIK 0x2b = Backslash) cycles forward through the systems directly
  // linked to (jumpable from) the current system, with the Shift modifier
  // pair (0x2a/0x36) backwards. Mirrors the original's command 0x60
  // (g_playerCycleTravelTargetCommandLatch) read by Ship_HandlePlayerShip,
  // whose default binding the EV Nova manual describes as "press the
  // Backslash key until the name of your desired destination system appears"
  // (after entering hyperspace mode with H). Distinct from cycle_target_next
  // (Tab), which cycles stellars within the system.
  bool cycle_destination_next = false;
  bool cycle_destination_previous = false;
  // Hyperspace-mode toggle: binding slot 0x0c (default DIK 0x23 = H). Arms
  // the in-flight destination-system selection channel so leading Backslash
  // cycles choose a jump system (manual: "press the H key to set your ship's
  // computer to hyperspace mode. Then, press the Backslash key until the
  // desired destination...").
  bool hyperspace_mode = false;
  // Cycle the ship (primary) target: binding slot 0x0a (default DIK 0x29 =
  // backquote), with the Shift modifier pair (0x2a/0x36) backwards. When
  // cycle_ship_escorts is held (raw DIK 0x1d/0x6b = Left/Right Ctrl) the cycle
  // restricts itself to the player's own squad/escorts; otherwise it cycles
  // the non-squad ships (hostiles included).
  bool cycle_ship_target_next = false;
  bool cycle_ship_target_previous = false;
  bool cycle_ship_escorts = false;
  // Select the nearest hostile combat target (binding slot 0x0b, default DIK
  // 0x13 = R), or the nearest engaged target when the 0x38/0x6f arm modifier
  // is held. Mirrors the original's "target nearest" command whose default arm
  // selects Ship_SelectNearestHostileCombatTarget and whose 0x38/0x6f modifiers
  // select Ship_SelectNearestEngagedTarget.
  bool select_nearest_hostile = false;
  bool select_nearest_engaged = false;
  // Face-target command (held): binding slot 0x07 (default DIK 0x1e = A).
  // While held (with the ship not disabled) the ship auto-steers toward the
  // primary ship target, or toward the selected travel stellar when no ship is
  // targeted or the Alt arm modifier is held, until aligned.
  bool face_target = false;
  // Edge latches from drained SDL events the loop otherwise could not see
  // (PollFlightInput owns the event drain). primary_clicked latches a left
  // mouse press with the current render-coordinate cursor position, used for
  // click-to-target ship picking. The flight-exit/cancel command is polled
  // through the persisted binding table (slot 0x17), not a raw Escape latch.
  bool primary_clicked = false;
  // mouse_x/y are in the active placement's authored space (mapped at poll
  // time). Consumers that own a different target placement must use the raw
  // window-point snapshot below instead: input is polled before DrawGameFrame
  // installs the scene placement, and RefreshPlacementAfterResize can change
  // the active placement mid-drain, so the placement that produced mouse_x/y
  // is not necessarily the one a target was drawn with.
  float mouse_x = 0.0F;
  float mouse_y = 0.0F;
  // Raw window-point cursor position, captured beside mouse_x/y. Map it
  // directly through each consumer's own placement via ToAuthored.
  float window_mouse_x = 0.0F;
  float window_mouse_y = 0.0F;
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
  // Native file chooser for pointing the game at an EV Nova install. SDL
  // cannot offer files and folders in one native dialog (the Cocoa folder
  // picker sets canChooseFiles:NO), so this is an unfiltered file dialog: the
  // player selects EV Nova.exe and the caller derives the containing folder.
  // The chosen file comes back through PollOpenFileDialogResult as `path`.
  [[nodiscard]] bool ShowOpenInstallFileDialog();
  [[nodiscard]] std::optional<OpenFileDialogResult> PollOpenFileDialogResult();
  [[nodiscard]] std::optional<char> PollCommandEvent();
  // Raw editable-key event for modal dialogs. Enter/Escape/Backspace are
  // returned as distinct TextKey values; printable keys return translated
  // ASCII and non-printable physical keys use TextKey::physical. Quit still
  // sets quit_requested_.
  [[nodiscard]] std::optional<TextInput> PollTextEvent();
  // Drains SDL events and returns the platform-sourced FlightInput fields:
  // cursor position, the primary-click latch, and the clean-room fixed keys
  // (Tab stellar cycle, Left/Right Ctrl escort modifier). The original
  // binding-table commands are left false; the spaceflight loop fills them
  // from `NovaInput_IsCommandActive` before consuming the snapshot.
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

  // Live left-button state for hold-to-repeat widgets (the text-view scroll
  // arrows). Reads SDL's real input state; probe-injected button events do
  // not update that state, so a /probe/click stays a discrete click and never
  // reports as held (see docs/probe_harness.md).
  [[nodiscard]] bool PrimaryMouseDown() const;

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

  // The full window size in logical draw coordinates. World/spaceflight
  // drawing queries this to extend to the
  // (possibly larger) window; it is independent of whether fixed screens are
  // being upscaled or bordered.
  [[nodiscard]] SDL_FPoint logical_playfield_size() const;

  // Physical output pixels occupied by one current render-coordinate unit.
  // Text uses this to rasterize at the destination resolution, then draws the
  // resulting texture at its unchanged logical size.
  [[nodiscard]] float text_raster_scale() const;

  [[nodiscard]] const Placement &current_placement() const {
    return placement_;
  }

  // Port-only presentation multipliers, set once at startup from the resolved
  // extra prefs. Authored UI compositions request `ui_scale()`; the flight
  // scene and mission dialogs use the other factors.
  void SetPresentationScale(const game::PresentationScale &scale) {
    presentation_scale_ = scale;
  }

  [[nodiscard]] const game::PresentationScale &presentation_scale() const {
    return presentation_scale_;
  }

  [[nodiscard]] float ui_scale() const { return presentation_scale_.ui; }

  // The HUD chrome (cockpit strip, radar, panels, overlays) follows the
  // general UI scale. Kept as its own accessor so a dedicated HUD factor can
  // be added later without touching call sites.
  [[nodiscard]] float hud_scale() const { return presentation_scale_.ui; }

  [[nodiscard]] float flight_scene_scale() const {
    return presentation_scale_.flight_scene;
  }

  [[nodiscard]] float mission_scale() const {
    return presentation_scale_.mission;
  }

  // Mission offer/variant and mission info dialogs compose the two: they are
  // authored UI (so they follow `U`) with an extra mission-dialog multiplier.
  // Composed before the single fit clamp in PlaceContained/PlaceCenteredIn.
  [[nodiscard]] float mission_dialog_scale() const {
    return presentation_scale_.mission_dialog();
  }

  void SetPlacement(Placement placement);
  void PushPlacement(const Placement &placement);
  void PopPlacement();

  class ScopedPlacement {
  public:
    ScopedPlacement(SdlPlatform &platform, const Placement &placement)
        : platform_(&platform) {
      platform_->PushPlacement(placement);
    }

    ~ScopedPlacement() {
      if (platform_ != nullptr) {
        platform_->PopPlacement();
      }
    }

    ScopedPlacement(const ScopedPlacement &) = delete;
    ScopedPlacement &operator=(const ScopedPlacement &) = delete;

    ScopedPlacement(ScopedPlacement &&other) noexcept
        : platform_(std::exchange(other.platform_, nullptr)) {}

    ScopedPlacement &operator=(ScopedPlacement &&) = delete;

  private:
    SdlPlatform *platform_;
  };

  // Applies the "Run in a Window" preference (NovaPreferences::run_in_window)
  // to the OS window. windowed=true keeps the normal resizable window;
  // windowed=false switches SDL to exclusive fullscreen. Idempotent and safe
  // to call at startup and every time the Settings checkbox is toggled. This
  // is independent of the active authored placement.
  void ApplyWindowMode(bool windowed);

  // Raw mouse position in window coordinates (SDL window points).
  [[nodiscard]] SDL_FPoint mouse_window_point() const;

  // Where the current authored content sits on the window, in window points.
  [[nodiscard]] SDL_FRect playfield_window_rect() const;

private:
  void ApplyPlacement();
  void RefreshPlacementAfterResize();

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
  // toggle key (Caps Lock by default; ddraw.ini key_x2mode, default 0x14 =
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
  Placement placement_{};
  game::PresentationScale presentation_scale_{};
  std::vector<Placement> placement_stack_;
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
