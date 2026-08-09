#include "government.hpp"

#include <cstddef>

namespace game {
namespace {

// Mirrors the original's range guard (`-1 < id && id < 0x100`) and derelict
// exclusion (flags_primary & 0x0800) shared by both relation helpers. Returns
// the government entry for a zero-based id when it is valid and not derelict,
// else nullptr.
[[nodiscard]] const Government *RelationEligible(const ScenarioData &scenario,
                                                 std::int16_t govt_id) {
  if (govt_id < 0 || govt_id >= 0x100) {
    return nullptr;
  }
  const auto idx = static_cast<std::size_t>(govt_id);
  if (idx >= scenario.governments.size()) {
    return nullptr;
  }
  const Government &g = scenario.governments[idx];
  if ((g.flags_primary & 0x0800U) != 0) {
    return nullptr; // derelict
  }
  return &g;
}

} // namespace

bool NovaGovernment_AreGovtsAllied(const ScenarioData &scenario,
                                   std::int16_t govt_a,
                                   std::int16_t govt_b) {
  if (govt_a == govt_b) {
    return true;
  }
  const Government *a = RelationEligible(scenario, govt_a);
  const Government *b = RelationEligible(scenario, govt_b);
  if (a == nullptr || b == nullptr) {
    return false;
  }
  // Either side's class id appearing in the other side's ally list (both
  // directions), matching the decompiled 4x4 double loop.
  for (std::size_t i = 0; i < 4; ++i) {
    if (a->classes[i] != -1) {
      for (std::size_t j = 0; j < 4; ++j) {
        if (b->ally_classes[j] == a->classes[i]) {
          return true;
        }
      }
    }
    if (b->classes[i] != -1) {
      for (std::size_t j = 0; j < 4; ++j) {
        if (a->ally_classes[j] == b->classes[i]) {
          return true;
        }
      }
    }
  }
  return false;
}

bool NovaGovernment_AreGovtsHostileOrXenophobic(const ScenarioData &scenario,
                                                std::int16_t govt_a,
                                                std::int16_t govt_b) {
  if (govt_a == govt_b) {
    return false;
  }
  const Government *a = RelationEligible(scenario, govt_a);
  const Government *b = RelationEligible(scenario, govt_b);
  // Direct enemy-class hostility check (both directions), only when both sides
  // are relation-eligible (non-derelict, in range).
  if (a != nullptr && b != nullptr) {
    for (std::size_t i = 0; i < 4; ++i) {
      if (a->classes[i] != -1) {
        for (std::size_t j = 0; j < 4; ++j) {
          if (b->enemy_classes[j] == a->classes[i]) {
            return true;
          }
        }
      }
      if (b->classes[i] != -1) {
        for (std::size_t j = 0; j < 4; ++j) {
          if (a->enemy_classes[j] == b->classes[i]) {
            return true;
          }
        }
      }
    }
  }
  // Fallback: not allied + one side xenophobic -> hostile from sight.
  if (NovaGovernment_AreGovtsAllied(scenario, govt_a, govt_b)) {
    return false;
  }
  const auto xenophobic = [&scenario](std::int16_t id) {
    if (id < 0 || id >= 0x100 ||
        static_cast<std::size_t>(id) >= scenario.governments.size()) {
      return false;
    }
    return (scenario.governments[static_cast<std::size_t>(id)].flags_primary &
            0x0001U) != 0;
  };
  return xenophobic(govt_a) || xenophobic(govt_b);
}

bool NovaGovernment_GetPolicyFlag(const ScenarioData &scenario,
                                  std::int16_t govt_id,
                                  int flag_index) {
  if (govt_id < 0 || govt_id >= 0x100 ||
      static_cast<std::size_t>(govt_id) >= scenario.governments.size()) {
    return false;
  }
  if (flag_index < 0 || flag_index >= 2) {
    return false;
  }
  return scenario.governments[static_cast<std::size_t>(govt_id)]
             .policy_flags[static_cast<std::size_t>(flag_index)] != 0;
}

} // namespace game
