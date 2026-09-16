#pragma once

#include "game_state.hpp"
#include "mission.hpp"

#include <cstdint>
#include <string_view>

namespace game {

// Caller-supplied context for the diagnostic trace (MissionTrace). `source`
// is the short "why" recorded on every traced line; `mission_slot` is the
// active mission slot when the script belongs to one, else -1. This has no
// effect on execution and the original has no equivalent.
struct MissionScriptContext {
  std::string_view source = "script";
  std::int16_t mission_slot = -1;
};

// Clean-room counterpart of Mission_ExecuteReactionScript (0x00448020) and
// Mission_ExecuteMisnScriptEngine (0x00449370). Mission scripts are a compact
// opcode-plus-number stream (for example `F130`, `G128`, or `b311`); numeric
// operands are resource ids unless noted. Malformed commands are logged;
// enabling MissionTrace additionally records every executed command,
// control-bit write, and unmodelled opcode.
void Mission_ExecuteScript(GameState &state,
                           std::string_view script,
                           const MissionScriptContext &context = {},
                           const MissionAcceptanceSink &acceptance = {});

// Named entrypoints corresponding one-to-one to the original call boundaries.
// They currently share the same explicit executor because the clean-room
// runtime has no global reaction-script buffer.
void Mission_ExecuteReactionScript(
    GameState &state,
    std::string_view script,
    const MissionScriptContext &context = {},
    const MissionAcceptanceSink &acceptance = {});
void Mission_RunMisnScriptPayload(GameState &state,
                                  std::string_view script,
                                  std::int16_t mission_slot,
                                  const MissionScriptContext &context = {},
                                  const MissionAcceptanceSink &acceptance = {});
void Mission_ExecuteMisnScriptEngine(
    GameState &state,
    std::string_view script,
    const MissionScriptContext &context = {},
    const MissionAcceptanceSink &acceptance = {});

} // namespace game
