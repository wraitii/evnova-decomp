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

std::mutex g_log_mutex;

// Bounded double-write buffer for the probe harness (log.hpp). Always on:
// one deque push per line is noise next to the stderr print + fflush.
constexpr std::size_t kLogBufferLines = 1000;
std::deque<NovaLog::LoggedLine> g_log_buffer;
std::uint64_t g_log_seq = 0;

} // namespace

void NovaLog::Write(Level level, std::string_view message) {
  const std::lock_guard lock{g_log_mutex};
  fmt::print(stderr, "[{}] {}\n", LevelName(level), message);
  std::fflush(stderr);
  g_log_buffer.push_back(
      {++g_log_seq, level, std::string{message}});
  while (g_log_buffer.size() > kLogBufferLines) {
    g_log_buffer.pop_front();
  }
}

std::vector<NovaLog::LoggedLine> NovaLog::ReadLogSince(std::uint64_t seq) {
  const std::lock_guard lock{g_log_mutex};
  std::vector<LoggedLine> out;
  for (const auto &line : g_log_buffer) {
    if (line.seq > seq) {
      out.push_back(line);
    }
  }
  return out;
}

std::uint64_t NovaLog::LogTailSeq() {
  const std::lock_guard lock{g_log_mutex};
  return g_log_seq;
}
