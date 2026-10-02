#pragma once

// Access tracing for the port host. The port code is compiled with clang's
// SanitizerCoverage load/store tracing (CMake option EVNOVA_MICROEXEC_TRACE),
// which calls __sanitizer_cov_load<N> / __sanitizer_cov_store<N> before every
// memory access. trace.cpp defines those hooks; they do nothing except inside
// the window TraceWindow opens around the port call.
//
// Before the call the host maps every scalar reachable from the root
// GameState (a LeafRun per scalar field or contiguous scalar array, built by
// Collect in reflect.hpp). Hooks mark the leaves the port read before writing
// and the ones it stored to; afterwards ReportTrace re-walks the state, so a
// vector the port reallocated is still read by path, and prints the report
// lines documented in main.cpp.
//
// Limits: accesses through library calls (memcpy, memset, std::fill) and
// accesses wider than the leaf they hit (struct copies) are not seen by the
// hooks; writes are still caught because a leaf whose bytes differ after the
// call is reported as written. Elements added by a resize during the call
// have no pre-call address and are ignored.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace game {
struct GameState;
}

namespace microexec {

#ifdef EVNOVA_MICROEXEC_TRACE
inline constexpr bool kTraceInstrumented = true;
#else
inline constexpr bool kTraceInstrumented = false;
#endif

// A contiguous run of scalars at one port path: a scalar field (one element,
// not indexed) or a std::array / std::vector of scalars (elements `path[i]`).
struct LeafRun {
  static constexpr std::uint8_t kRead = 1;    // read before any store to it
  static constexpr std::uint8_t kWritten = 2; // stored to

  std::string path;
  std::uintptr_t base = 0;
  std::size_t element_size = 0;
  std::size_t count = 0;
  bool indexed = false;
  std::string kind; // KindOf<element>
  std::string (*format)(const std::byte *) = nullptr;
  std::vector<std::byte> initial;  // bytes when collected
  std::vector<std::uint8_t> flags; // per element

  [[nodiscard]] std::string ElementPath(std::size_t index) const;
};

using LeafRuns = std::vector<LeafRun>;

// "a.b[3].c" from a bridge path (leading separators and spacing normalized),
// or an empty string when it does not parse.
[[nodiscard]] std::string CanonicalPath(std::string_view path);

// Maps the scalars reachable from `state`. `set_paths` are the case's `set`
// paths: a read of any other leaf is reported as read-unset.
void PrepareTrace(game::GameState &state,
                  const std::vector<std::string> &set_paths);

// Drops the map without reporting (a case that never ran the port).
void ResetTrace();

// Adds leaves that live outside the state (the records the host builds for
// record parameters, named by their argument) to the prepared map; call before
// the TraceWindow opens. CaptureTraceRuns hands ReportTrace their values after
// the call, while the records are still alive.
void AddTraceRuns(LeafRuns runs);
void CaptureTraceRuns(LeafRuns runs);

// Prints `write` / `read-unset` lines for the prepared case and resets.
void ReportTrace(game::GameState &state, std::ostream &out);

// Enables the hooks for the scope of a port call.
class TraceWindow {
public:
  TraceWindow();
  ~TraceWindow();
  TraceWindow(const TraceWindow &) = delete;
  TraceWindow &operator=(const TraceWindow &) = delete;
};

} // namespace microexec
