#pragma once

#include <fmt/format.h>

#include <string_view>
#include <utility>

namespace NovaLog {

enum class Level { debug, info, warn, error, todo };

void Write(Level level, std::string_view message);

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
