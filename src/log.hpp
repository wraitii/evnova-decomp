#pragma once

#include <fmt/format.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NovaLog {

enum class Level { debug, info, warn, error, todo };

void Write(Level level, std::string_view message);

// Probe-harness double-write (docs/probe_harness.md): every line that reaches
// Write is also kept in a bounded in-memory ring buffer, so the external
// harness can tail the game log without a console attached.
struct LoggedLine {
  std::uint64_t seq = 0; // monotonically increasing across the process
  Level level = Level::info;
  std::string text;
};

// All buffered lines with seq > since (oldest first). Pass 0 for the whole
// retained buffer.
[[nodiscard]] std::vector<LoggedLine> ReadLogSince(std::uint64_t seq);
// The highest seq currently assigned (the tail cursor to resume tailing from).
[[nodiscard]] std::uint64_t LogTailSeq();

template <typename... Args>
void Debug(fmt::format_string<Args...> format, Args &&...args) {
  Write(Level::debug, fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void Info(fmt::format_string<Args...> format, Args &&...args) {
  Write(Level::info, fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void Warn(fmt::format_string<Args...> format, Args &&...args) {
  Write(Level::warn, fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void Error(fmt::format_string<Args...> format, Args &&...args) {
  Write(Level::error, fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void Todo(fmt::format_string<Args...> format, Args &&...args) {
  Write(Level::todo, fmt::format(format, std::forward<Args>(args)...));
}

} // namespace NovaLog
