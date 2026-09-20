#include "log.hpp"

#include <fmt/format.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>

namespace {

[[nodiscard]] std::string_view LevelName(NovaLog::Level level) {
  switch (level) {
  case NovaLog::Level::debug:
    return "DEBUG";
  case NovaLog::Level::info:
    return "INFO";
  case NovaLog::Level::warn:
    return "WARN";
  case NovaLog::Level::error:
    return "ERROR";
  case NovaLog::Level::todo:
    return "TODO(decomp)";
  }
  return "UNKNOWN";
}

// Bounded double-write buffer for the probe harness (log.hpp). Always on:
// one deque push per line is noise next to the stderr print + fflush.
constexpr std::size_t kLogBufferLines = 1000;

// stderr can be a detached, closed or absent handle when the process is started
// by a windowed launcher (or Wine/CrossOver starts this console-subsystem
// binary with no attached console). fmt::print(FILE*, ...) throws
// std::system_error when the underlying fwrite fails, so a failed sink write
// would escape Write() as an uncaught exception and abort the process. Build
// the line in memory and treat the sink writes as best-effort instead.
void WriteBestEffort(std::FILE *stream, std::string_view text) {
  if (stream == nullptr) {
    return;
  }
  std::fwrite(text.data(), 1, text.size(), stream);
  std::fflush(stream);
}

// Optional persistent sink: EVNOVA_LOG_FILE=<path> truncates that file at first
// use and mirrors every line to it, so a launch whose console is detached still
// leaves a log behind (used to diagnose Wine/bottle launches).
[[nodiscard]] std::FILE *OpenLogFileFromEnvironment() {
  const char *const path = std::getenv("EVNOVA_LOG_FILE");
  if (path == nullptr || *path == '\0') {
    return nullptr;
  }
  return std::fopen(path, "w");
}

struct LogState {
  std::mutex mutex;
  std::deque<NovaLog::LoggedLine> buffer;
  std::uint64_t seq = 0;
  std::FILE *file = nullptr;
  bool file_checked = false;
};

// The log store is intentionally leaked so it is never torn down by
// __cxa_finalize. Other translation units may log from their own static
// destructors (SDL teardown, RAII handles, etc.); a namespace-scope deque here
// could be destroyed first and then used after free, corrupting its block map
// and aborting in ~deque (R_BEING_FREED_WAS_NOT_ALLOCATED). Leaking a few KiB
// of log ring at process exit is cheap and makes logging safe from any static
// destructor regardless of initialization order across translation units.
[[nodiscard]] LogState &State() {
  static LogState *state = new LogState();
  return *state;
}

} // namespace

void NovaLog::Write(Level level, std::string_view message) {
  auto &state = State();
  const std::lock_guard lock{state.mutex};
  // Format to memory (no I/O) first, so the sink writes below cannot throw.
  const std::string line = fmt::format("[{}] {}\n", LevelName(level), message);
  WriteBestEffort(stderr, line);
  if (!state.file_checked) {
    state.file_checked = true;
    state.file = OpenLogFileFromEnvironment();
  }
  WriteBestEffort(state.file, line);
  state.buffer.push_back({++state.seq, level, std::string{message}});
  while (state.buffer.size() > kLogBufferLines) {
    state.buffer.pop_front();
  }
}

std::vector<NovaLog::LoggedLine> NovaLog::ReadLogSince(std::uint64_t seq) {
  auto &state = State();
  const std::lock_guard lock{state.mutex};
  std::vector<LoggedLine> out;
  for (const auto &line : state.buffer) {
    if (line.seq > seq) {
      out.push_back(line);
    }
  }
  return out;
}

std::uint64_t NovaLog::LogTailSeq() {
  auto &state = State();
  const std::lock_guard lock{state.mutex};
  return state.seq;
}
