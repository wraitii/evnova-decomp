#include "asteroid.hpp"

#include "../log.hpp"
#include "nova_random.hpp"
#include "scenario_data.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace game {
namespace {

// Ghidra _DAT_00575298: the double 0.01 used across the asteroid spawn for
// converting the integer random rolls into the small fractional world/pixel
// scales the asteroid-drift records use.
constexpr double kAsteroidScale = 0.01;

// Ghidra _DAT_00575280: 0.5. The asteroid-drift scatter centers each random
// target position around the player (`pos +/- radius*0.5`), so a random offset
// of [0, radius) maps to a [-radius*0.5, +radius*0.5) band.
constexpr float kScatterCenter = 0.5F;

// Ghidra _DAT_005751f8: 2.0. The minimum ring radius for the place_in_ring
// branch of Asteroid_Spawn: `max(scatter_y, 2.0)`.
constexpr float kRingRadiusMin = 2.0F;

// Ghidra _DAT_005752d8: -0.01. Negative asteroid scale; the ring branch uses
// it to push one of the two velocity axes away from the player while the other
// pulls inward (see NovaAsteroid_Spawn).
constexpr double kNegAsteroidScale = -0.01;

// Horizontal scatter padding for an asteroid spawn, measured from the player.
// Ghidra Asteroid_Spawn (0x00421830) scatters the target over
// [0, g_viewport_center_x + 0x80) in x and [0, g_viewport_center_y) in y,
// centred on the player (scatter * 0.5); Asteroid_InitSystem's pool pre-warm
// uses g_viewport_center_x + 0x80 and g_viewport_center_y + 0x80. The viewport
// half-size lives on GameState (the original's g_viewport_center_x/y globals).
constexpr std::int32_t kAsteroidScatterPadX = 0x80;
constexpr std::int32_t kAsteroidScatterPadY = 0x80;

// Ring-direction spread used by the place_in_ring branch: the random offset
// along the axis is drawn in [0, trunc(radius/2)). The original computes this
// with the x87 FIST + residual/sign correction on `radius / _DAT_005751f8
// (2.0)` at 0x00421830, i.e. truncation toward zero (floor for the positive
// short radius), not round-to-nearest.
constexpr std::int32_t RingSpread(std::int32_t radius) {
  return std::max(1, static_cast<std::int32_t>(radius / 2.0));
}

} // namespace

// @port 0x00421e60 40% gameplay,divergence
// Ghidra 0x00421e60 Asteroid_SpawnRecord.
int NovaAsteroid_SpawnRecord(GameState &state,
                             float pos_x,
                             float pos_y,
                             std::int16_t type) {
  std::size_t slot = AsteroidState::kPoolSize;
  for (std::size_t i = 0; i < AsteroidState::kPoolSize; ++i) {
    if (!state.asteroid_pool[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == AsteroidState::kPoolSize) {
    return -1;
  }

  AsteroidState &m = state.asteroid_pool[slot];
  m.active = true;
  m.wander_type = type;
  m.target_pos_x = pos_x;
  m.target_pos_y = pos_y;
  // Each velocity component is seeded in the range [-1.0, 1.0).
  m.target_vel_x = static_cast<float>(RandomBelow(state, 200) - 100) *
                   static_cast<float>(kAsteroidScale);
  m.target_vel_y = static_cast<float>(RandomBelow(state, 200) - 100) *
                   static_cast<float>(kAsteroidScale);

  // TODO(decomp): the original seeds the animation frame from
  // g_asteroid_sprite_sets[type]->num_frames; AsteroidDef.mass is a temporary
  // stand-in and has a different range.
  const AsteroidDef *row =
      state.scenario.AsteroidType(static_cast<std::int16_t>(type + 0x80));
  const std::int32_t frame_seed = row != nullptr ? row->mass : 0;
  m.wander_frame_accumulator =
      static_cast<float>(RandomBelow(state, frame_seed));
  const float speed_mult = row != nullptr ? row->spin_rate : 1.0F;
  m.wander_speed = static_cast<float>(RandomBelow(state, 0x29) + 0x50) *
                   speed_mult * static_cast<float>(kAsteroidScale);
  // The original chooses either animation direction evenly; the unsigned rate
  // is 0.8..1.2 times the asteroid type's spin-rate multiplier.
  if (RandomBelow(state, 2) == 0) {
    m.wander_speed = -m.wander_speed;
  }
  m.integrity = row != nullptr ? row->strength : static_cast<std::int16_t>(0);

  NovaLog::Debug("asteroid spawn slot {} type {} frame_seed {} speed {}",
                 slot,
                 type,
                 frame_seed,
                 m.wander_speed);
  return static_cast<int>(slot);
}

// @port 0x00421830 75% license,gameplay
// Ghidra 0x00421830 Asteroid_Spawn: allocates
// one asteroid / drift debris record into a free pool slot. `place_in_ring ==
// 0` scatters the target around the player (a [-radius*0.5, +radius*0.5) band
// with a random velocity), otherwise it parks the target along an axis-aligned
// ring of radius max(scatter_y, 2) near the player. The record's wander type
// is a 0..15 asteroid-type index (the Metal/Ice/Dust/Crystal x size-tier rows,
// i.e. the r\xf6id asteroid types) that the system's ast_types mask must
// permit: `(1 << (type & 0x1f)) & ast_types` must be set, otherwise the random
// pick is redrawn. The wander radius/speed/table value are seeded from that
// type's asteroid row, mirroring the per-type recipe used by
// NovaAsteroid_SpawnRecord. Returns the allocated pool slot, or -1 when the
// system declares no asteroids, its ast_types mask is clear, all slots are
// busy, or the invocation is a no-op.
//
// DIVERGENCE (documented): the original first runs a license-parity probe that
// toggles a license-runtime byte
// (g_ship_class_defs[alt].is_licensed_runtime_alt). Registration/licence
// integrity is out of scope for the recompilation, so that latch is skipped.
int NovaAsteroid_Spawn(GameState &state, bool place_in_ring) {
  const System *sys =
      state.scenario.System(state.player.current_system_id + 0x80);
  if (sys == nullptr || sys->asteroid_count < 1 || sys->ast_types == 0) {
    return -1;
  }

  // The original bails when the number of already-active pool slots reaches
  // the system's asteroid quota (it will not allocate beyond asteroid_count
  // concurrent drift records).
  int active = 0;
  for (const AsteroidState &ent : state.asteroid_pool) {
    active += ent.active ? 1 : 0;
  }
  if (active >= sys->asteroid_count) {
    return -1;
  }

  // Claim the first free slot and mark it active (the +0x20 byte).
  std::size_t slot = AsteroidState::kPoolSize;
  for (std::size_t i = 0; i < AsteroidState::kPoolSize; ++i) {
    if (!state.asteroid_pool[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == AsteroidState::kPoolSize) {
    return -1;
  }
  AsteroidState &m = state.asteroid_pool[slot];
  m.active = true;

  const float px = state.player.pos_x;
  const float py = state.player.pos_y;
  if (!place_in_ring) {
    // Scatter around the player over the live viewport half-size:
    // [0, viewport_center_x + 0x80) in x, [0, viewport_center_y) in y, each
    // band centred with a 0.5 shift (Ghidra 0x00421914..0x004219ad).
    const std::int32_t scatter_x =
        state.viewport_center_x + kAsteroidScatterPadX;
    const std::int32_t scatter_y = state.viewport_center_y;
    m.target_pos_x = static_cast<float>(RandomBelow(state, scatter_x)) + px -
                     static_cast<float>(scatter_x) * kScatterCenter;
    m.target_pos_y = static_cast<float>(RandomBelow(state, scatter_y)) + py -
                     static_cast<float>(scatter_y) * kScatterCenter;
    m.target_vel_x = static_cast<float>(RandomBelow(state, 400) - 200) *
                     static_cast<float>(kAsteroidScale);
    m.target_vel_y = static_cast<float>(RandomBelow(state, 400) - 200) *
                     static_cast<float>(kAsteroidScale);
  } else {
    // Park along an axis-aligned ring of radius max(viewport_center_y, 2). Each
    // axis independently picks a side (+/-); the outward axis gets a negative
    // or positive velocity (push away from center) and the other draws from
    // [0, round(radius/2)) for the along-ring spread (Ghidra
    // 0x004219c0..0x00421a8c).
    const std::int32_t ring = std::max<std::int32_t>(
        state.viewport_center_y, static_cast<std::int32_t>(kRingRadiusMin));
    const std::int32_t spread = RingSpread(ring);
    const bool x_plus = RandomBelow(state, 2) == 0;
    if (x_plus) {
      m.target_pos_x = px + static_cast<float>(ring) +
                       static_cast<float>(RandomBelow(state, spread));
      m.target_vel_x = static_cast<float>(RandomBelow(state, 200)) *
                       static_cast<float>(kNegAsteroidScale);
    } else {
      m.target_pos_x = (px - static_cast<float>(ring)) -
                       static_cast<float>(RandomBelow(state, spread));
      m.target_vel_x = static_cast<float>(RandomBelow(state, 200)) *
                       static_cast<float>(kAsteroidScale);
    }
    const bool y_plus = RandomBelow(state, 2) == 0;
    if (y_plus) {
      m.target_pos_y = py + static_cast<float>(ring) +
                       static_cast<float>(RandomBelow(state, spread));
      m.target_vel_y = static_cast<float>(RandomBelow(state, 200)) *
                       static_cast<float>(kNegAsteroidScale);
    } else {
      m.target_pos_y = (py - static_cast<float>(ring)) -
                       static_cast<float>(RandomBelow(state, spread));
      m.target_vel_y = static_cast<float>(RandomBelow(state, 200)) *
                       static_cast<float>(kAsteroidScale);
    }
  }

  // Pick an asteroid type (index 0..15) permitted by the system ast_types
  // mask, then seed the wander radius/speed/table value from that row.
  std::int16_t dir = 0;
  do {
    dir = RandomBelow(state, 0x10);
  } while ((sys->ast_types & (1U << (dir & 0x1f))) == 0);
  m.wander_type = dir;

  const AsteroidDef *row =
      state.scenario.AsteroidType(static_cast<std::int16_t>(dir + 0x80));
  // TODO(decomp): the original seeds the frame from
  // g_asteroid_sprite_sets[dir]->num_frames; AsteroidDef.mass is a temporary
  // deterministic stand-in with a different range.
  const std::int32_t frame_seed = row != nullptr ? row->mass : 0;
  m.wander_frame_accumulator =
      static_cast<float>(RandomBelow(state, frame_seed));
  const float speed_mult = row != nullptr ? row->spin_rate : 1.0F;
  m.wander_speed = static_cast<float>(RandomBelow(state, 0x29) + 0x50) *
                   speed_mult * static_cast<float>(kAsteroidScale);
  if (RandomBelow(state, 2) == 0) {
    m.wander_speed = -m.wander_speed;
  }
  m.integrity = row != nullptr ? row->strength : static_cast<std::int16_t>(0);

  NovaLog::Debug("asteroid spawn slot {} type {} ring={} radius {}",
                 slot,
                 dir,
                 place_in_ring,
                 m.wander_frame_accumulator);
  return static_cast<int>(slot);
}

// @port 0x00436910 80% divergence,gameplay
// Ghidra 0x00436910 Asteroid_UpdateSprites (simulation half). The original is
// one function that both integrates the drift and re-binds/positions the
// SDL Sprite; the clean-room splits the pure drift advance here from the
// render-side sprite bind, frame selection and viewport wrap in
// SpaceflightView (WrapAsteroids / DrawAsteroids). The -32000 hidden sentinel
// is the original's `integrity <= 0x8300` guard.
void NovaAsteroid_UpdateSprites(GameState &state, float elapsed_ticks) {
  const float scale = std::max(0.0F, elapsed_ticks);
  for (AsteroidState &m : state.asteroid_pool) {
    if (!m.active || m.integrity <= -32000 || state.no_asteroids_latch) {
      // The original hides + deactivates the slot; the sprite is hidden by
      // the draw pass because the record is no longer active.
      m.active = false;
      continue;
    }
    // Motion-integrating subtickers skip physics while gameplay time is frozen
    // (the original's g_gameplay_time_frozen guard); this port has no explicit
    // freeze flag, so callers pass a zero scale for frozen transitions.
    m.target_pos_x += m.target_vel_x * scale;
    m.target_pos_y += m.target_vel_y * scale;
    m.wander_frame_accumulator += m.wander_speed * scale;
  }
}

// @port 0x004216b0 85% verify
// Ghidra 0x004216B0 Asteroid_InitSystem:
// restores the current system's asteroid / drift-debris population on
// spaceflight entry / cross-system travel. When the system declares no
// asteroids (asteroid_count < 1) it sets the NoAsteroids latch
// (GameState.no_asteroids_latch): the original writes a 1 byte into
// g_random_encounter_fleet_defs[0x4d].availability_expression[0x94] (a scratch
// area); the clean-room stores it in the explicit game-state flag instead
// since that scratch buffer is not modelled. Otherwise it spawns
// `asteroid_count` asteroid records (scatter placement) and pre-warms all 16
// pool slots with a random wander target around the player.
//
// The original leaves the active flags alone because the departure/landing
// paths set g_no_asteroids_latch (DAT_00596d2c), and Asteroid_UpdateSprites
// deactivates every record while the latch is up; the clean-room consolidates
// that by clearing the pool here, so entering a system (or launching) never
// inherits the previous system's live records. no_asteroids_latch is cleared
// for a populated system so the freshly spawned field is visible.
void NovaAsteroid_InitSystem(GameState &state) {
  const System *sys =
      state.scenario.System(state.player.current_system_id + 0x80);
  if (sys == nullptr) {
    return;
  }
  for (AsteroidState &m : state.asteroid_pool) {
    m.active = false;
  }
  if (sys->asteroid_count < 1) {
    state.no_asteroids_latch = true;
    return;
  }
  state.no_asteroids_latch = false;

  for (std::int16_t i = 0; i < sys->asteroid_count; ++i) {
    (void)NovaAsteroid_Spawn(state, /*place_in_ring=*/false);
  }

  // Pre-warm all 16 pool slots with a random wander target around the player
  // (Ghidra 0x00421709..0x0042181b uses viewport_center + 0x80 on both axes).
  const std::int32_t warm_x = state.viewport_center_x + kAsteroidScatterPadX;
  const std::int32_t warm_y = state.viewport_center_y + kAsteroidScatterPadY;
  for (AsteroidState &m : state.asteroid_pool) {
    m.target_pos_x = static_cast<float>(RandomBelow(state, warm_x)) +
                     state.player.pos_x -
                     static_cast<float>(warm_x) * kScatterCenter;
    m.target_pos_y = static_cast<float>(RandomBelow(state, warm_y)) +
                     state.player.pos_y -
                     static_cast<float>(warm_y) * kScatterCenter;
    m.target_vel_x = static_cast<float>(RandomBelow(state, 400) - 200) *
                     static_cast<float>(kAsteroidScale);
    m.target_vel_y = static_cast<float>(RandomBelow(state, 400) - 200) *
                     static_cast<float>(kAsteroidScale);
  }
}

} // namespace game
