#pragma once

#include "game_state.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace game {

struct MissionScriptDiagnostic {
  std::size_t offset = 0;
  std::string message;
};

struct MissionScriptResult {
  std::size_t commands_executed = 0;
  std::vector<MissionScriptDiagnostic> diagnostics;

  [[nodiscard]] bool ok() const { return diagnostics.empty(); }
};

// Clean-room counterpart of Mission_ExecuteReactionScript (0x00448020) and
// Mission_ExecuteMisnScriptEngine (0x00449370). Mission scripts are a compact
// opcode-plus-number stream (for example `F130`, `G128`, or `b311`); numeric
// operands are resource ids unless noted.
// The executor deliberately reports commands whose gameplay side effects are
// not modelled yet instead of silently discarding them.
MissionScriptResult Mission_ExecuteScript(GameState &state,
                                          std::string_view script);

// Named entrypoints corresponding one-to-one to the original call boundaries.
// They currently share the same explicit executor because the clean-room
// runtime has no global reaction-script buffer.
MissionScriptResult Mission_ExecuteReactionScript(GameState &state,
                                                  std::string_view script);
MissionScriptResult Mission_RunMisnScriptPayload(GameState &state,
                                                 std::string_view script,
                                                 std::int16_t mission_slot);
MissionScriptResult Mission_ExecuteMisnScriptEngine(GameState &state,
                                                    std::string_view script);

} // namespace game
