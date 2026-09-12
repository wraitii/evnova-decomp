#include "rank.hpp"

#include <cstddef>

namespace game {

// Ghidra 0x00427df0 Rank_Activate.
void Rank_Activate(GameState &state, std::int16_t rank_slot) {
  auto &table = state.scenario.ranks;
  if (rank_slot < 0 || static_cast<std::size_t>(rank_slot) >= table.size()) {
    return;
  }
  RankDef &rank = table[static_cast<std::size_t>(rank_slot)];
  if (!rank.active) {
    rank.active = true;
    const auto clear_siblings = [&](bool lower_weight_only) {
      for (std::size_t i = 0; i < table.size(); ++i) {
        RankDef &sibling = table[i];
        if (!sibling.active || !sibling.defined) {
          continue;
        }
        if (sibling.id == rank.id) {
          continue;
        }
        if (sibling.government_id != rank.government_id) {
          continue;
        }
        if ((sibling.flags & 0x0008U) != 0) {
          continue; // permanent ranks are never cleared by a sibling
        }
        if (lower_weight_only && sibling.weight >= rank.weight) {
          continue;
        }
        sibling.active = false;
        if (state.recently_activated_rank_id == static_cast<std::int16_t>(i)) {
          state.recently_activated_rank_id = -1;
        }
      }
    };
    if ((rank.flags & 0x0001U) != 0) {
      clear_siblings(/*lower_weight_only=*/false);
    }
    if ((rank.flags & 0x0010U) != 0) {
      clear_siblings(/*lower_weight_only=*/true);
    }
  }
  state.recently_activated_rank_id = rank.id;
}

// Ghidra 0x00427f40 Rank_Deactivate.
void Rank_Deactivate(GameState &state, std::int16_t rank_slot) {
  auto &table = state.scenario.ranks;
  if (rank_slot < 0 || static_cast<std::size_t>(rank_slot) >= table.size()) {
    return;
  }
  RankDef &rank = table[static_cast<std::size_t>(rank_slot)];
  if (rank.active) {
    rank.active = false;
    const auto clear_siblings = [&](bool lower_weight_only) {
      for (std::size_t i = 0; i < table.size(); ++i) {
        RankDef &sibling = table[i];
        if (!sibling.active || !sibling.defined) {
          continue;
        }
        if (sibling.government_id != rank.government_id) {
          continue;
        }
        if ((sibling.flags & 0x0008U) != 0) {
          continue; // permanent ranks are never cleared by a sibling
        }
        if (lower_weight_only && sibling.weight >= rank.weight) {
          continue;
        }
        sibling.active = false;
        if (state.recently_activated_rank_id == static_cast<std::int16_t>(i)) {
          state.recently_activated_rank_id = -1;
        }
      }
    };
    if ((rank.flags & 0x0002U) != 0) {
      clear_siblings(/*lower_weight_only=*/false);
    }
    if ((rank.flags & 0x0020U) != 0) {
      clear_siblings(/*lower_weight_only=*/true);
    }
  }
  if (state.recently_activated_rank_id == rank.id) {
    state.recently_activated_rank_id = -1;
  }
}

// Stellar_BuildTravelDestinationDescription (0x004444f0) <PRK>/<SRK> scan.
// The original keeps the current max in a register and accepts a candidate when
// `max < weight || best == -1` (strict `<`, so earlier slots win ties); the
// name must be non-empty.
std::int16_t Rank_HighestWeightedActiveSlot(const GameState &state,
                                            bool use_short_name) {
  std::int16_t best = -1;
  std::int16_t best_weight = 0;
  const auto &table = state.scenario.ranks;
  for (std::size_t i = 0; i < table.size(); ++i) {
    const RankDef &rank = table[i];
    if (!rank.active || !rank.defined) {
      continue;
    }
    const std::string &name = use_short_name ? rank.short_name : rank.conv_name;
    if (name.empty()) {
      continue;
    }
    if (best_weight < rank.weight || best == -1) {
      best_weight = rank.weight;
      best = static_cast<std::int16_t>(i);
    }
  }
  return best;
}

// Stellar_BuildTravelDestinationDescription (0x004444f0) <PSRK>/<SSRK> scan:
// same as the <SRK> scan but restricted to ranks credited to one government.
std::int16_t
Rank_HighestWeightedActiveSlotForGovernment(const GameState &state,
                                            std::int16_t government_id) {
  std::int16_t best = -1;
  std::int16_t best_weight = 0;
  const auto &table = state.scenario.ranks;
  for (std::size_t i = 0; i < table.size(); ++i) {
    const RankDef &rank = table[i];
    if (!rank.active || !rank.defined) {
      continue;
    }
    if (rank.government_id != government_id || rank.short_name.empty()) {
      continue;
    }
    if (best_weight < rank.weight || best == -1) {
      best_weight = rank.weight;
      best = static_cast<std::int16_t>(i);
    }
  }
  return best;
}

} // namespace game
