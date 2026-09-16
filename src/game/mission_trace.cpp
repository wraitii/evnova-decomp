#include "mission_trace.hpp"

#include "../log.hpp"
#include "game_state.hpp"

#include <atomic>
#include <cstdlib>
#include <string_view>

namespace game {
namespace MissionTrace {
namespace {

std::atomic<int> g_mode{static_cast<int>(Mode::off)};

[[nodiscard]] bool IsFullEnabled() {
  return g_mode.load(std::memory_order_relaxed) == static_cast<int>(Mode::full);
}

} // namespace

void SetMode(Mode mode) {
  g_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

Mode GetMode() {
  return static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
}

bool Enabled() { return GetMode() != Mode::off; }

std::string_view ModeName(Mode mode) {
  switch (mode) {
  case Mode::off:
    return "off";
  case Mode::commands:
    return "commands";
  case Mode::full:
    return "full";
  }
  return "off";
}

void ConfigureFromEnvironment() {
  const char *requested = std::getenv("EVN_MISSION_TRACE");
  if (requested == nullptr) {
    return;
  }
  const std::string_view value{requested};
  if (value == "full") {
    SetMode(Mode::full);
  } else if (value == "1" || value == "on" || value == "commands") {
    SetMode(Mode::commands);
  } else {
    SetMode(Mode::off);
  }
}

void LogScript(std::string_view origin,
               std::int16_t slot,
               std::string_view script) {
  if (!IsFullEnabled()) {
    return;
  }
  if (slot >= 0) {
    NovaLog::Debug("mission-trace {} slot {}: \"{}\"", origin, slot, script);
  } else {
    NovaLog::Debug("mission-trace {}: \"{}\"", origin, script);
  }
}

void LogCommand(std::string_view origin,
                std::int16_t slot,
                std::size_t offset,
                char opcode,
                std::int32_t operand,
                std::string_view action) {
  if (!Enabled()) {
    return;
  }
  if (slot >= 0) {
    NovaLog::Info("mission-trace {} slot {} +{}: {}{} -> {}",
                  origin,
                  slot,
                  offset,
                  opcode,
                  operand,
                  action);
  } else {
    NovaLog::Info("mission-trace {} +{}: {}{} -> {}",
                  origin,
                  offset,
                  opcode,
                  operand,
                  action);
  }
}

void LogScriptBit(std::string_view origin,
                  std::int16_t slot,
                  std::size_t offset,
                  char modifier,
                  std::uint32_t bit,
                  bool old_value,
                  bool new_value) {
  if (!Enabled()) {
    return;
  }
  const std::string_view prefix = modifier == '!'   ? "!b"
                                  : modifier == '^' ? "^b"
                                                    : "b";
  if (slot >= 0) {
    NovaLog::Info("mission-trace {} slot {} +{}: {}{} {} -> {}",
                  origin,
                  slot,
                  offset,
                  prefix,
                  bit,
                  old_value,
                  new_value);
  } else {
    NovaLog::Info("mission-trace {} +{}: {}{} {} -> {}",
                  origin,
                  offset,
                  prefix,
                  bit,
                  old_value,
                  new_value);
  }
}

void LogBit(std::string_view origin,
            std::uint32_t bit,
            bool old_value,
            bool new_value) {
  if (!Enabled()) {
    return;
  }
  NovaLog::Info(
      "mission-trace {}: b{} {} -> {}", origin, bit, old_value, new_value);
}

bool SetControlBit(PilotControlState &control,
                   std::uint32_t bit,
                   bool value,
                   std::string_view origin) {
  if (bit >= PilotControlState::kControlBitCount) {
    control.SetControlBit(bit, value);
    return false;
  }
  const bool old_value = control.ControlBit(bit);
  control.SetControlBit(bit, value);
  LogBit(origin, bit, old_value, value);
  return old_value;
}

} // namespace MissionTrace
} // namespace game
