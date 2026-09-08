#pragma once

// External probe / control harness (deliberate divergence, see
// docs/probe_harness.md). When EVN_PROBE=1 is set at startup the platform
// runs a tiny HTTP server on 127.0.0.1 that can pause/step the game, inject
// input, read game state and capture frames — without the server thread ever
// touching SDL or GameState.
//
// Threading model: the server thread only parses requests and enqueues work;
// everything that touches the renderer or game state runs on the main thread
// at the two universal choke points every loop already shares —
// SdlPlatform::PollTextEvent / PollFlightInput (the pump) and
// SdlPlatform::Present (the frame boundary).

#include <SDL3/SDL.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

// One named control rect published by the active modal (see PublishUi).
struct ProbeNamedRect {
  std::string name;
  SDL_FRect rect{};
};

class ProbeServer {
public:
  ProbeServer() = default;
  ~ProbeServer();
  ProbeServer(const ProbeServer &) = delete;
  ProbeServer &operator=(const ProbeServer &) = delete;

  // Starts the listener thread (inert unless EVN_PROBE=1 was seen).
  bool Start(int port);
  void Stop();

  // ---- main-thread side (called from SdlPlatform) -------------------------

  // Frame-boundary hook, called from SdlPlatform::Present() *before*
  // SDL_RenderPresent so a pending screenshot reads the frame that is about
  // to be swapped in. Also counts down step frames.
  void OnPresent(SDL_Renderer *renderer);

  // Runs queued work and enforces the pause/step latch. Called at the top of
  // every input poll; blocks here while paused (still servicing requests).
  void Pump();

  // Flight-input merge: true when the harness holds this scancode virtually
  // (SDL_GetKeyboardState cannot see injected events, so held flight keys go
  // through this channel instead).
  [[nodiscard]] bool VirtualKey(SDL_Scancode scancode) const;

  // Sets the state reader executed on the main thread (registered by
  // NovaApp_Run with the live GameState). Empty result = unknown query.
  void SetStateProvider(
      std::function<std::string(const std::string &query)> provider);

  // Latch executed on the main thread when the harness asks the game to quit.
  void SetQuitLatch(std::function<void()> latch);

  // ---- UI layout registry (docs/probe_harness.md) -------------------------
  // The active modal publishes its named control rects (window-point space,
  // the same space /probe/click consumes) so the harness can click by intent
  // instead of hand-measured coordinates. Written by the main thread, read by
  // the server thread; a no-op when the harness is not running.
  void PublishUi(std::string window_name, std::vector<ProbeNamedRect> rects);
  void ClearUi();
  // Main-thread cached window geometry (window points + the 640x480 canvas
  // rect) so the harness can convert backing-store screenshots to window
  // points without guessing the display density.
  void SetGeometry(SDL_FPoint window_points, SDL_FRect playfield);
  [[nodiscard]] bool UiActive() const;
  [[nodiscard]] std::optional<SDL_FRect>
  UiElementRect(const std::string &element) const;
  [[nodiscard]] std::optional<std::string> UiElementAt(SDL_FPoint point) const;
  [[nodiscard]] std::string UiJson() const;

private:
  void AcceptLoop(int port);
  // Builds the HTTP response for one request; runs on the server thread.
  void HandleRequest(const std::string &method,
                     const std::string &path,
                     const std::string &query,
                     const std::string &body,
                     std::string &status,
                     std::string &content_type,
                     std::string &body_out);

  // Submits a main-thread job and waits (bounded) for its string result.
  std::string SubmitJob(const std::function<std::string()> &work,
                        bool &timed_out);
  // Executes every queued job; main thread only (holds sync_).
  void RunJobsLocked();

  std::atomic<bool> running_{false};
  std::atomic<int> port_{0};
  std::thread thread_;

  // Control plane (atomics; woken via sync_ notifications).
  std::atomic<bool> paused_{false};
  std::atomic<int> step_remaining_{0};
  std::atomic<bool> quit_requested_{false};

  // One mutex serializes the job queue, the injected-key queue and the cv
  // the pause wait sleeps on.
  std::mutex sync_;
  std::condition_variable sync_cv_;
  std::deque<std::packaged_task<std::string()>> jobs_;
  std::deque<SDL_Event> key_events_;

  // Frame mailbox: the latest captured frame, encoded as a complete BMP.
  std::mutex frame_mutex_;
  std::condition_variable frame_cv_;
  std::vector<std::uint8_t> frame_bytes_;
  std::uint64_t frame_seq_ = 0;
  std::atomic<bool> want_capture_{false};

  mutable std::mutex keys_mutex_;
  std::set<SDL_Scancode> virtual_keys_;

  std::mutex provider_mutex_;
  std::function<std::string(const std::string &query)> state_provider_;
  std::function<void()> quit_latch_;

  mutable std::mutex ui_mutex_;
  std::string ui_window_;
  std::vector<ProbeNamedRect> ui_rects_;
  SDL_FPoint geom_window_points_{};
  SDL_FRect geom_playfield_{};
};
