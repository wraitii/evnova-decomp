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

#include "game_state.hpp"
#include "scenario_data.hpp"

#include <span>

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

// Ghidra 0x0046bff0 Government_DoGovtsShareClass. True when the ids are equal
// or the two governments share any matching class_1..4 value (a's class
// non-sentinel and equal to one of b's). Unlike the allied/hostile helpers
// this check has no derelict exclusion.
[[nodiscard]] bool NovaGovernment_DoGovtsShareClass(
    const ScenarioData &scenario, std::int16_t govt_a, std::int16_t govt_b);

// Ghidra 0x0046e860 Government_GetGovernmentPolicyFlag. Reads one of the two
// per-government boolean policy flags (Government.policy_flags, GovtDef
// +0x84). flag_index must be 0 or 1; out-of-range government ids read 0.
// Flag 0 gates player target acquisition (Ship_IsShipAcquirableAsTarget
// 0x0040faa0) and several aggro/relation decisions. The writer is not yet
// identified, so the flags stay 0 in the current build (TODO(decomp)).
[[nodiscard]] bool NovaGovernment_GetPolicyFlag(const ScenarioData &scenario,
                                                std::int16_t govt_id,
                                                int flag_index);

// Ghidra 0x004629E0 Government_IsCandidateHostileToTargeter. Whether `ship` is
// a hostile target for a stellar defense battery. `stellar` is the targeter and
// `stellar_id` its raw resource id (Ghidra StellarDef +0x8). Branches:
//  - a hazard-marked targeter (+0x46) or a non-player ship in AI state 8 is
//    never a candidate;
//  - ordinary stellars (availability_flags +0x34 without 0x200): the player or
//    a squad leader uses the system-reputation / government-relation ladder
//    (falling back to the system government when the stellar has none), while
//    an ordinary NPC compares its own faction against the stellar government
//    directly;
//  - special (0x200) stellars admit the player/leader as a target only when the
//    derelict sentinel (+0x47) is set.
// A stellar currently selected as the travel destination is never hostile.
// `government` is the ship class's inherent-combat government id used by the
// ladder. Confidence: high (single caller, the defense-battery tick).
[[nodiscard]] bool
NovaGovernment_IsCandidateHostileToTargeter(const GameState &state,
                                            const Ship &ship,
                                            const Stellar &stellar,
                                            std::int16_t stellar_id);

// Ghidra 0x0040fd20 Government_IsShipEligibleForGovernmentAid. Whether the
// ship's government would send it to help the player when hailed: false when
// the ship keeps pressing its own target; true when it is idle with no AI
// target, or its government policy flag 0 is set, or it has no faction at all.
// Otherwise resolves the player's current system government against the
// ship's faction: the xenophobic flag (0x0001) admits aid when the system
// reputation clears the system government's flee threshold, the 0x0002 flag
// admits aid when reputation + the relevant government's flee threshold stays
// negative, and hostile system governments admit aid under the same threshold
// test. The mission-fleet branch (random-encounter fleet defs) and the
// GovtDef +0x83 byte gate are deferred (TODO(decomp): not modelled).
[[nodiscard]] bool
NovaGovernment_IsShipEligibleForGovernmentAid(const GameState &state,
                                              const Ship &ship);

// Ghidra 0x00413610 Government_TryTriggerGovtAssistanceEncounter.
[[nodiscard]] bool NovaGovernment_TryTriggerAssistanceEncounter(
    GameState &state, const Ship &ship, bool force);

// Ghidra 0x0046f100 Government_IsShipGovernmentDerelict: true when the ship's
// faction is a valid 0-based government carrying the derelict bit
// (flags_primary 0x0800). Callers use the true result to SKIP the kill-side
// reputation event (derelict/story hulks do not cost standing).
[[nodiscard]] bool
NovaGovernment_IsGovernmentDerelict(const ScenarioData &scenario,
                                    std::int16_t government_id);

// Ghidra 0x00440750 Government_ApplyReputationCreditDelta. Applies the
// mission PayVal opcode: flat credit award (positive), per-government
// negative-reputation clears (-10128/-20128/-30128 families), or a percentage
// cash deduction (-40001..-40099). See government.cpp for the full range map.
void NovaGovernment_ApplyReputationCreditDelta(GameState &state,
                                               std::int32_t delta);

// Returns the government's combat penalty for one crime event code, i.e. the
// runtime GovtDef +0x48 + event_code*2 word (0 = SmugPenalty, 1 = DisabPenalty,
// 2 = BoardPenalty, 3 = KillPenalty, 4 = the unused ShootPenalty slot).
// Out-of-range codes return 0 (the original indexes without a bound check, but
// no caller uses a code outside 0..4).
[[nodiscard]] std::int16_t
NovaGovernment_CrimePenalty(const Government &government,
                            std::int16_t event_code);

// Ghidra 0x00467140 Government_PropagateFactionCombatInfluenceToNearbySystems.
// Recursive reputation flood over the visibility-twin chain and the 16-way
// system adjacency graph. `scale` starts at 1.0 and is multiplied by 0.65 on
// each adjacency recursion; `visit_mask` is the caller-owned re-entry guard
// (0x800 entries, cleared by ProcessFactionCombatEvent before the flood).
void NovaGovernment_PropagateFactionCombatInfluence(
    GameState &state,
    std::int16_t system_id,
    std::int16_t faction_or_government_id,
    std::int16_t event_code,
    double scale,
    std::span<bool> visit_mask);

// Ghidra 0x00466fc0 Government_ProcessFactionCombatEvent. Single entry point
// for the crime -> reputation -> rank-revocation chain. event_code is 0
// smuggle, 1 disable, 2 board, 3 kill. `mission_fleet_slot` is always -1 in
// the shipped mission fleet script calls; the != -1 arm (which skips the flood
// and revocation) is inert today. Derelict governments (flags_primary 0x0800)
// short-circuit the whole event.
void NovaGovernment_ProcessFactionCombatEvent(
    GameState &state,
    std::int16_t system_id,
    std::int16_t faction_or_government_id,
    std::int16_t event_code,
    std::int16_t mission_fleet_slot);

} // namespace game
