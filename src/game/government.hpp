#pragma once

// Clean-room reconstruction of government cross-relations used by the faction
// / encounter systems. Mirrors the original's Government_AreGovtsAllied
// (0x0046bc90) and Government_AreGovtsHostileOrXenophobic (0x0046bdf0).
//
// How the original reads the relation tables: g_government_defs is indexed by
// the zero-based government id (resource id minus 0x80); each entry's class /
// ally / enemy id lists (GovernmentDef +0x26 / +0x2e / +0x36, i.e. the
// clean-room Government.classes / ally_classes / enemy_classes) hold raw ids
// (-1 sentineled) that match by equality across governments. Governments with
// the derelict flag (flags_primary 0x0800) are excluded from alliance and
// hostility class checks, and the xenophobic flag (flags_primary 0x0001) drives
// the fallback hostility override. The helper takes the ScenarioData so it can
// look up governments by id without any hidden global state (AGENTS.md).

#include "scenario_data.hpp"

namespace game {

// Ghidra 0x0046bc90 Government_AreGovtsAllied. Two governments are allied when
// they share the same id, or when either one's class id appears in the other's
// ally list (checked in both directions). Governments carrying the derelict
// flag (flags_primary & 0x0800) are excluded from the relation checks (return
// false unless equal). Out-of-range ids are treated as non-allied.
[[nodiscard]] bool NovaGovernment_AreGovtsAllied(const ScenarioData &scenario,
                                                 std::int16_t govt_a,
                                                 std::int16_t govt_b);

// Ghidra 0x0046bdf0 Government_AreGovtsHostileOrXenophobic. True when the two
// governments are hostile either by class relationship (one's class id appears
// in the other's enemy list, checked both directions) or by the xenophobic
// override: when not allied, if either government has the xenophobic flag
// (flags_primary & 0x0001) they are treated as hostile-from-sight. Derelict
// governments are excluded from the class checks (same as the allied helper).
[[nodiscard]] bool NovaGovernment_AreGovtsHostileOrXenophobic(
    const ScenarioData &scenario, std::int16_t govt_a, std::int16_t govt_b);

} // namespace game
