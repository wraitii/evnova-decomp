#include "world_wrap.hpp"

#include "game_state.hpp"

#include <cmath>

namespace game {
namespace {

// Ghidra 0x005756a8 / 0x005756ac. The player is recentered once its position
// exceeds this band on either axis; the same radius is the "travels with the
// player" pull-in radius used per object by the relative helper.
constexpr float kWorldWrapTrigger = 15000.0F;
// Ghidra 0x46c35000 / 0xc6c35000. The fixed torus shift applied on a crossing.
constexpr float kWorldWrapDelta = 25000.0F;

} // namespace

// @port 0x0045C6D0 100%
// Ghidra 0x0045C6D0 Ship_WorldWrapPositionRelativeToPlayer.
void WorldWrapPositionRelativeToPlayer(float &x,
                                       float &y,
                                       float player_x,
                                       float player_y,
                                       float delta_x,
                                       float delta_y) {
  if (std::abs(x - player_x) < kWorldWrapTrigger &&
      std::abs(y - player_y) < kWorldWrapTrigger) {
    x += delta_x;
    y += delta_y;
  }
}

// @port 0x0045BAA0 100%
// Ghidra 0x0045BAA0 Ship_RecenterSpaceObjectsForWorldWrap.
WorldWrapDelta RecenterSpaceObjectsForWorldWrap(GameState &state) {
  WorldWrapDelta delta;
  const float player_x = state.player.pos_x;
  const float player_y = state.player.pos_y;
  if (player_x > kWorldWrapTrigger) {
    delta.x = -kWorldWrapDelta;
  } else if (player_x < -kWorldWrapTrigger) {
    delta.x = kWorldWrapDelta;
  }
  if (player_y > kWorldWrapTrigger) {
    delta.y = -kWorldWrapDelta;
  } else if (player_y < -kWorldWrapTrigger) {
    delta.y = kWorldWrapDelta;
  }
  if (!delta.applied()) {
    return delta;
  }

  const auto shift = [&](float &x, float &y) {
    WorldWrapPositionRelativeToPlayer(
        x, y, player_x, player_y, delta.x, delta.y);
  };

  // The original walks every pool slot, not just the active ones. Ship slot 0
  // is the player and is shifted directly at the end so the whole pass compares
  // against the pre-recenter player position (matching the global used by the
  // helper); the rest of the ships are shifted relative to the player.
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    shift(ship.pos_x, ship.pos_y);
  }
  // Ghidra g_shot_states[128] (stride 0x48), positions at +0x04/+0x08. The port
  // stores only live shots in active_shots; inactive originals are stale and
  // never observed.
  for (ActiveShot &shot : state.active_shots) {
    shift(shot.pos_x, shot.pos_y);
  }
  // Ghidra ImpactEffectInstance[32] (stride 0x18).
  for (ImpactEffectInstance &instance : state.impact_effect_instances) {
    shift(instance.pos_x, instance.pos_y);
  }
  // Ghidra FreeflightObjectState[64] (stride 0x28).
  for (FreeflightObjectState &object : state.freeflight_objects) {
    shift(object.pos_x, object.pos_y);
  }
  // Ghidra DirectionalWeaponEffectInstance[64] (stride 0x18,
  // g_weapon_smoke_puff_instances_ptr).
  for (WeaponSmokePuff &puff : state.weapon_smoke_puffs) {
    shift(puff.pos_x, puff.pos_y);
  }
  // Ghidra FadingEffectSpriteState[32] (stride 0x18).
  for (FadingEffectInstance &fragment : state.fading_effect_instances) {
    shift(fragment.pos_x, fragment.pos_y);
  }
  // Ghidra g_beam_hit_queue[64] (stride 0x22): source and target positions.
  // The original stores the coordinates as shorts and rewrites them through the
  // x87 truncate idiom; the port keeps floats, so the delta is simply added.
  for (BeamHit &hit : state.beam_hit_queue) {
    hit.source_x += delta.x;
    hit.source_y += delta.y;
    hit.target_x += delta.x;
    hit.target_y += delta.y;
  }

  // Ship 0 (the player) is unconditional in the original.
  state.player.pos_x += delta.x;
  state.player.pos_y += delta.y;
  return delta;
}

} // namespace game
