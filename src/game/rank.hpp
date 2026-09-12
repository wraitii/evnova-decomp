#pragma once

// Clean-room reconstruction of the EV Nova r\x8ank rank/honor system
// (g_rank_defs). The mission-script K/L opcodes, the crime revocation loop in
// Government_ProcessFactionCombatEvent (0x00466fc0), and the <PRK>/<SRK>/<RRK>
// mission-text wildcards all read the RankDef table declared in
// scenario_data.hpp. Activate/deactivate mirror the original's sibling-clearing
// status flags and g_recently_activated_rank_id maintenance.

#include "game_state.hpp"

namespace game {

// Ghidra 0x00427df0 Rank_Activate(rank_def): activates rank slot `rank_slot`.
// Sets RankDef.active when it was clear, then applies the activation-side
// status flags (+0x1c): 0x0001 deactivates every other active, defined,
// non-permanent rank credited to the same government; 0x0010 additionally
// deactivates every such rank with a strictly lower weight. Always records the
// slot index in GameState.recently_activated_rank_id (the <RRK> source), even
// when the rank was already active.
void Rank_Activate(GameState &state, std::int16_t rank_slot);

// Ghidra 0x00427f40 Rank_Deactivate(rank_def): deactivates rank slot
// `rank_slot`. Clears RankDef.active when set, then applies the
// deactivation-side status flags: 0x0002 deactivates every other active,
// defined, non-permanent rank of the same government; 0x0020 additionally
// deactivates every such rank with a strictly lower weight. Clears
// recently_activated_rank_id when it names this slot.
void Rank_Deactivate(GameState &state, std::int16_t rank_slot);

// Highest-weighted active + defined rank whose ConvName (`use_short_name`
// false) or ShortName (true) is non-empty; -1 when no rank applies. This is
// the <PRK> / <SRK> name scan from Stellar_BuildTravelDestinationDescription
// (0x004444f0); ties keep the lowest slot index.
[[nodiscard]] std::int16_t
Rank_HighestWeightedActiveSlot(const GameState &state, bool use_short_name);

// Highest-weighted active + defined rank whose government_id matches
// `government_id` and whose ShortName is non-empty; -1 when none. This is the
// per-government <PSRK>/<SSRK> scan from the same function.
[[nodiscard]] std::int16_t
Rank_HighestWeightedActiveSlotForGovernment(const GameState &state,
                                            std::int16_t government_id);

} // namespace game
