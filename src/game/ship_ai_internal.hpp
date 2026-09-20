#pragma once

#include "game_state.hpp"

namespace game::ship_ai_detail {

// Shared movement and combat constants decoded from the original
// _DAT_00575xxx table. Control-mode constants live here only when both the
// state machine and the control bridge consume them.
// "Moving" / arrival-stopped velocity threshold, px/tick (0x575080, double).
inline constexpr float kVerySlowSpeed = 0.35F;
// Mode-1 "still fast" threshold (0x575118); aligned and stopped damping are
// also shared by the relative-velocity branches in the control bridge.
inline constexpr float kMode1StillFastThreshold = 1.75F;
inline constexpr float kMode1Damp = 0.94F;
inline constexpr float kMode1StopDamp = 0.95F;
// Combat close range (0x5750b0, float).
inline constexpr float kCombatCloseRange = 165.0F;

// Mode-0x14 scripted velocity-match merge gate turn+10 (0x5750a0 = 10.0).
inline constexpr float kScriptAlignAddend = 10.0F;
// Scripted-manoeuvre turn addend (0x5750d4 = 15.0): the mode-0x13 alignment
// window and the class-turn distance scale in the state-0x10 0x13/0x14 gate.
inline constexpr float kScriptTurnAddend = 15.0F;
inline constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
inline constexpr float kFullCircleDeg = 360.0F;

// Shared geometry used by the AI state, behavior, and weapon translation
// units. These mirror the original Math_SquaredDistance and current-system
// resource lookup without widening the public ship-AI API.
inline float SquaredDistance(float x1, float y1, float x2, float y2) {
  const float dx = x2 - x1;
  const float dy = y2 - y1;
  return dx * dx + dy * dy;
}

inline const System *CurrentSystem(const GameState &state) {
  return state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
}

inline const Stellar *StellarByResourceId(const GameState &state,
                                          int resource_id) {
  if (resource_id < 0) {
    return nullptr;
  }
  return state.scenario.Stellar(static_cast<std::int16_t>(resource_id));
}

} // namespace game::ship_ai_detail

namespace game {

// Cross-translation-unit AI orchestration hooks. These stay out of the public
// ship-AI header because they are only called by the split implementation
// units and the top-level dispatcher.
void NovaAi_DefenseFleetPrioritizePlayerThreat(GameState &state, Ship &ship);
void NovaAi_UpdateAvailabilityBehavior(GameState &state, Ship &ship);
void NovaAi_UpdateBehavior0x01(GameState &state,
                               Ship &ship,
                               std::uint32_t now_ms);
void NovaAi_UpdateBehavior0x02(GameState &state, Ship &ship);
void NovaAi_UpdateBehavior0x03(GameState &state, Ship &ship);

[[nodiscard]] bool
WeaponBankCanFire(const GameState &state, const Ship &ship, std::int16_t bank);

} // namespace game
