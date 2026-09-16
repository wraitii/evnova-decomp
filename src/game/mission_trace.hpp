#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace game {

struct PilotControlState; // game_state.hpp

// Diagnostic-only trace of mission-script execution and control-bit mutation.
// The original executable has no equivalent: Mission_ExecuteMisnScriptEngine
// (Ghidra 0x00449370) references no strings and calls no logging routine, so
// this is a deliberate reconstruction aid, not a reproduction of behaviour.
// It is inert unless enabled through EVN_MISSION_TRACE or the probe harness,
// and is intended to record what ran and why.
namespace MissionTrace {

enum class Mode : std::uint8_t {
  off = 0,
  // One line per executed command and control-bit transition.
  commands = 1,
  // Commands plus the raw script text before each execution.
  full = 2,
};

// Reads EVN_MISSION_TRACE ("full" -> full, "1"/"on"/"commands" -> commands,
// anything else -> off). Safe to call once at startup; a no-op when the
// variable is unset.
void ConfigureFromEnvironment();

void SetMode(Mode mode);
[[nodiscard]] Mode GetMode();
[[nodiscard]] bool Enabled();
[[nodiscard]] std::string_view ModeName(Mode mode);

// Logs the raw script once before executing it (Mode::full only). `origin` is
// a short reason such as "OnAccept"; `slot` is the active mission slot or -1.
void LogScript(std::string_view origin,
               std::int16_t slot,
               std::string_view script);

// Logs one executed command with a short description of its effect.
void LogCommand(std::string_view origin,
                std::int16_t slot,
                std::size_t offset,
                char opcode,
                std::int32_t operand,
                std::string_view action);

// Logs a control-bit transition from a mission-script `b`/`!b`/`^b` directive.
void LogScriptBit(std::string_view origin,
                  std::int16_t slot,
                  std::size_t offset,
                  char modifier,
                  std::uint32_t bit,
                  bool old_value,
                  bool new_value);

// Logs a control-bit transition from a non-mission set string (nebula
// OnExplore, outfit/ship OnPurchase/OnSell/OnRetire, ...).
void LogBit(std::string_view origin,
            std::uint32_t bit,
            bool old_value,
            bool new_value);

// Applies a gameplay control-bit write and logs the transition when tracing
// is enabled. Returns the bit's previous value. Pilot-file restore does not
// go through here (it is loading state, not a gameplay side effect).
bool SetControlBit(PilotControlState &control,
                   std::uint32_t bit,
                   bool value,
                   std::string_view origin);

} // namespace MissionTrace
} // namespace game
