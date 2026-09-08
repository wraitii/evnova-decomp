#pragma once

#include <string>

namespace game {

struct GameState;

// Probe-harness state reader (docs/probe_harness.md). Renders a JSON snapshot
// of `state` for the given query; runs on the main thread via the probe pump.
// Queries: "" / "summary", "player", "missions", "ships", "travel", "system".
// Unknown queries return {"error": "..."}.
[[nodiscard]] std::string ProbeState_Snapshot(const GameState &state,
                                              const std::string &query);

} // namespace game
