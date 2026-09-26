#pragma once

// Clean-room model of EV Nova's in-system world-wrap recenter
// (Ship_RecenterSpaceObjectsForWorldWrap 0x0045BAA0 and its per-position
// helper Ship_WorldWrapPositionRelativeToPlayer 0x0045C6D0).
//
// The original never lets the player's coordinates grow without bound: once
// the player ship leaves the +-15000 band (Ghidra 0x005756a8 / 0x005756ac),
// every space object within 15000 units of the player is shifted by a fixed
// -+25000 delta (Ghidra 0x46c35000 / 0xc6c35000) and the player is shifted by
// the same delta. The shift keeps the simulation inside a bounded coordinate
// bubble (and hence inside float precision); nearby objects travel with the
// player so no visual pop occurs, while far objects stay put.
//
// The player's frame-start position cached by Frame_SpaceflightLoop
// (Ghidra g_player_frame_start_pos_x/g_player_frame_start_pos_y) is adjusted
// by the same delta inside
// the original recenter. The port keeps that cache in the flight loop's local
// `prev_x/prev_y`; the loop must add the returned delta to them or the wrap
// would be misread as a 25000-pixel ship jump by the ambient-star parallax.

namespace game {

struct GameState;

// The delta applied by a recenter pass. Each component is 0 when the player
// did not cross that axis's boundary, otherwise -25000 or +25000.
struct WorldWrapDelta {
  float x = 0.0F;
  float y = 0.0F;

  [[nodiscard]] bool applied() const { return x != 0.0F || y != 0.0F; }
};

// Ghidra 0x0045C6D0 Ship_WorldWrapPositionRelativeToPlayer: when (x, y) lies
// within 15000 units of the player on both axes, shift it by the wrap delta.
// The fractional truncation the original performs before the distance test
// is exactly equivalent to a float magnitude comparison, so the float form is
// used here.
void WorldWrapPositionRelativeToPlayer(float &x,
                                       float &y,
                                       float player_x,
                                       float player_y,
                                       float delta_x,
                                       float delta_y);

// Ghidra 0x0045BAA0 Ship_RecenterSpaceObjectsForWorldWrap: if the player has
// crossed an axis boundary, shift every ship/shot/effect/beam position within
// the pull-in radius and the player itself by the wrap delta. Returns the
// applied delta (0,0 when nothing crossed).
WorldWrapDelta RecenterSpaceObjectsForWorldWrap(GameState &state);

} // namespace game
