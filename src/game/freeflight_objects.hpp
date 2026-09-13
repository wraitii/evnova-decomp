#pragma once

// Clean-room model of Nova's 64-slot FreeflightObjectState pool
// (g_freeflight_objects_ptr, 0x005914a8): the generic in-flight cosmetic
// objects drawn through the 500+index spin-sprite table. The pool backs the
// jettisoned cargo/junk pods spawned by Player_RedistributeFleetCargoOverflow
// (0x0041f330) via Ship_SpawnFreeflightObjectForShip (0x0041f800), plus the
// beam-hit / effect-package / launched-drone variants spawned at an explicit
// position (0x0041fb50). The per-tick pass is Frame_UpdateFreeflightObject-
// Sprites (0x0042c1b0).
//
// The original clones one Sprite per pool slot at startup and assigns the
// spin set from the 5-entry table DAT_00596ca4 (spin 500..504) when an object
// activates; the port resolves the sprite lazily at draw time from
// FreeflightObjectState::sprite_set_index.

#include <cstdint>

namespace game {

struct GameState;
struct Ship;

// Ghidra 0x0041f800 Ship_SpawnFreeflightObjectForShip: activate one free pool
// slot at `ship`'s position/velocity with the stock cargo/junk spin set
// (index 0), a random 180-269 tick lifetime, a randomized spin direction and
// a backward launch scatter (position offset + velocity).
void NovaFreeflight_SpawnForShip(GameState &state, const Ship &ship);

// Ghidra 0x0041fb50 Ship_SpawnFreeflightObjectAtPosition: activate one free
// pool slot at an explicit world position with a random radial velocity and a
// 300-499 tick lifetime, marked persistent. `extra` is the caller payload
// (sprite frame / effect id) and `sprite_set_index` selects spin 500+index.
void NovaFreeflight_SpawnAtPosition(GameState &state,
                                    float pos_x,
                                    float pos_y,
                                    std::int16_t extra,
                                    std::int16_t sprite_set_index);

// Ghidra 0x0042c1b0 Frame_UpdateFreeflightObjectSprites: integrate position,
// advance the spin/frame counter and expire lifetimes. `elapsed_ticks` is in
// original normalized 30 Hz frame-time units (the port draws at ~60 Hz and
// passes frame_time_ms / (1000/30)).
void NovaFreeflight_Tick(GameState &state, float elapsed_ticks);

// The spin-descriptor resource id backing one freeflight object type
// (Ghidra's DAT_00596ca4 table: spin 500+index). Index 0 is the cargo/junk
// pod set used by the jettison pass.
[[nodiscard]] std::uint16_t
NovaFreeflightSpriteSetId(std::int16_t sprite_set_index);

} // namespace game
