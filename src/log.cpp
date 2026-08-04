#include "log.hpp"

#include <fmt/format.h>

#include <cstdio>
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

} // namespace

void NovaLog::Write(Level level, std::string_view message) {
  const std::lock_guard lock{g_log_mutex};
  fmt::print(stderr, "[{}] {}\n", LevelName(level), message);
  std::fflush(stderr);
}
