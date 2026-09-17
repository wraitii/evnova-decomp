#include "log.hpp"

#include <fmt/format.h>

#include <cstdio>
#include <deque>
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

struct LogState {
  std::mutex mutex;
  std::deque<NovaLog::LoggedLine> buffer;
  std::uint64_t seq = 0;
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
  fmt::print(stderr, "[{}] {}\n", LevelName(level), message);
  std::fflush(stderr);
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
