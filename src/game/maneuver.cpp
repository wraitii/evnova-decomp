#include "maneuver.hpp"

#include "../log.hpp"
#include "scenario_data.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace game {
namespace {

// Mirrors the original's NovaRandom_Range(n) -> integer in [0, n), drawn from
// GameState.rng so runs stay reproducible (the original uses the global
// NovaRandom LCG, a deterministic warp-lock sequence). Returns the signed
// 16-bit value the original stores into the spawn fields.
inline std::int16_t RandomBelow(GameState &state, std::int32_t n) {
  if (n <= 0) {
    return 0;
  }
  std::uniform_int_distribution<std::int32_t> dist{0, n - 1};
  return static_cast<std::int16_t>(dist(state.rng));
}

// Ghidra _DAT_00575298: the double 0.01 used across the manoeuvre spawn for
// converting the integer random rolls into the small fractional world/pixel
// scales the asteroid-drift records use.
constexpr double kManeuverScale = 0.01;

// Ghidra _DAT_00575280: 0.5. The roaming/asteroid-drift scatter centers each
// random target position around the player (`pos +/- radius*0.5`), so a random
// offset of [0, radius) maps to a [-radius*0.5, +radius*0.5) band.
constexpr float kScatterCenter = 0.5F;

// Ghidra _DAT_005751f8: 2.0. The minimum ring radius for the place_in_ring
// branch of Dude_SpawnRoamingShip: `max(scatter_y, 2.0)`.
constexpr float kRingRadiusMin = 2.0F;

// Ghidra _DAT_005752d8: -0.01. Negative manoeuvre scale; the ring branch uses
// it to push one of the two velocity axes away from the player while the other
// pulls inward (see NovaDude_SpawnRoamingShip).
constexpr double kNegManeuverScale = -0.01;

// Scatter/ring radius for a roaming spawn, measured from the player. The
// original reads the (x, y) pair out of the random-encounter fleet-def scratch
// area as `g_random_encounter_fleet_defs[0x72].availability_expression._236_2_`
// (+0x80 for x) / `._238_2_` (for y). The clean-room has no such scratch
// buffer, so these stand in for those two loaded values. TODO(decomp): confirm
// the exact radii once the running game is observed (Step 4 visual validation).
constexpr std::int32_t kRoamingScatterX = 128;
constexpr std::int32_t kRoamingScatterY = 128;

// Ring-direction spread used by the place_in_ring branch: the random offset
// along the axis is drawn in [0, round(radius/2)). The original computes this
// with a float round + sign fixup on `radius / _DAT_005751f8 (2.0)`. For a
// whole-number radius (the payload short) this yields roughly radius/2;
// TODO(decomp) pins the exact tie-handling once observed.
constexpr std::int32_t RingSpread(std::int32_t radius) {
  return std::max(1, static_cast<std::int32_t>(std::lround(radius / 2.0)));
}

} // namespace

int NovaManeuver_SpawnState(GameState &state,
                            float pos_x,
                            float pos_y,
                            std::int16_t type) {
  // Find the first inactive pool slot; give up when all 16 are busy.
  std::size_t slot = ManeuverState::kPoolSize;
  for (std::size_t i = 0; i < ManeuverState::kPoolSize; ++i) {
    if (!state.maneuver_pool[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == ManeuverState::kPoolSize) {
    return -1;
  }

  ManeuverState &m = state.maneuver_pool[slot];
  m.active = true;
  m.wander_type = type;
  m.target_pos_x = pos_x;
  m.target_pos_y = pos_y;
  // Scatter velocity: [-100,100) * 0.01 -> [-1,1) px/frame.
  m.target_vel_x = static_cast<float>(RandomBelow(state, 200) - 100) *
                   static_cast<float>(kManeuverScale);
  m.target_vel_y = static_cast<float>(RandomBelow(state, 200) - 100) *
                   static_cast<float>(kManeuverScale);

  // Wander radius/lifetime. The original draws this from the per-type sprite
  // descriptor's frame count (descriptor[mode]+0x54). Until the drift-sprite
  // system lands (Step 3/4) the decoded per-type `lifetime` field stands in;
  // see ManeuverTypeDef.lifetime TODO(decomp).
  const ManeuverTypeDef *row =
      state.scenario.ManeuverType(static_cast<std::int16_t>(type + 0x80));
  const std::int32_t lifetime = row != nullptr ? row->lifetime : 0;
  m.wander_radius = static_cast<float>(RandomBelow(state, lifetime));
  const float speed_mult = row != nullptr ? row->wander_speed_multiplier : 1.0F;
  // wander_speed = (rand(0x29)+0x50) * speed_mult * 0.01 -> 0.8..1.2 scaled.
  m.wander_speed = static_cast<float>(RandomBelow(state, 0x29) + 0x50) *
                   speed_mult * static_cast<float>(kManeuverScale);
  // Flip the wander direction sign half of the time (Ghidra _DAT_005752e0 =
  // -1.0): the sprite drifts toward or away from its spawn.
  if (RandomBelow(state, 2) == 0) {
    m.wander_speed = -m.wander_speed;
  }
  m.wander_table_value =
      row != nullptr ? row->wander_table_value : static_cast<std::int16_t>(0);

  NovaLog::Debug("maneuver spawn slot {} type {} lifetime {} speed {}",
                 slot,
                 type,
                 lifetime,
                 m.wander_speed);
  return static_cast<int>(slot);
}

// Mirrors Dude_SpawnRoamingShip (0x00421830): allocates one roaming/asteroid-
// drift manoeuvre record into a free pool slot. `place_in_ring == 0` scatters
// the target around the player (a [-radius*0.5, +radius*0.5) band with a random
// velocity), otherwise it parks the target along an axis-aligned ring of radius
// max(scatter_y, 2) near the player. The record's wander type is a 0..15
// manoeuvre-type index (the Metal/Ice/Dust/Crystal x size-tier rows) that the
// roam direction bitmap must permit: `(1 << (type & 0x1f)) & bitmap` must be
// set, otherwise the random pick is redrawn. The wander radius/speed/table
// value are seeded from that type's manoeuvre row, mirroring the per-type
// recipe used by NovaManeuver_SpawnState. Returns the allocated pool slot, or
// -1 when the system declares no roaming ships, its direction bitmap is clear,
// all slots are busy, or the invocation is a no-op.
//
// DIVERGENCE (documented): the original first runs a license-parity probe that
// toggles a license-runtime byte
// (g_ship_class_defs[alt].is_licensed_runtime_alt). Registration/licence
// integrity is out of scope for the recompilation (see
// docs/recomp_startup_main_menu.md), so that latch is skipped.
int NovaDude_SpawnRoamingShip(GameState &state, bool place_in_ring) {
  const System *sys =
      state.scenario.System(state.player.current_system_id + 0x80);
  if (sys == nullptr || sys->roaming_ship_count < 1 ||
      sys->roaming_direction_bitmap == 0) {
    return -1;
  }

  // The original bails when the number of already-active pool slots reaches
  // the system's roaming quota (it will not allocate beyond roaming_ship_count
  // concurrent drift records).
  int active = 0;
  for (const ManeuverState &ent : state.maneuver_pool) {
    active += ent.active ? 1 : 0;
  }
  if (active >= sys->roaming_ship_count) {
    return -1;
  }

  // Claim the first free slot and mark it active (the +0x20 byte).
  std::size_t slot = ManeuverState::kPoolSize;
  for (std::size_t i = 0; i < ManeuverState::kPoolSize; ++i) {
    if (!state.maneuver_pool[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == ManeuverState::kPoolSize) {
    return -1;
  }
  ManeuverState &m = state.maneuver_pool[slot];
  m.active = true;

  const float px = state.player.pos_x;
  const float py = state.player.pos_y;
  if (!place_in_ring) {
    // Scatter around the player: [px - rx*0.5, px + rx*0.5) per axis.
    m.target_pos_x = static_cast<float>(RandomBelow(state, kRoamingScatterX)) +
                     px - static_cast<float>(kRoamingScatterX) * kScatterCenter;
    m.target_pos_y = static_cast<float>(RandomBelow(state, kRoamingScatterY)) +
                     py - static_cast<float>(kRoamingScatterY) * kScatterCenter;
    m.target_vel_x = static_cast<float>(RandomBelow(state, 400) - 200) *
                     static_cast<float>(kManeuverScale);
    m.target_vel_y = static_cast<float>(RandomBelow(state, 400) - 200) *
                     static_cast<float>(kManeuverScale);
  } else {
    // Park along an axis-aligned ring of radius max(scatter_y, 2). Each axis
    // independently picks a side (+/-); the outward axis gets a negative or
    // positive velocity (push away from center) and the other draws from
    // [0, round(radius/2)) for the along-ring spread.
    const std::int32_t ring = std::max<std::int32_t>(
        kRoamingScatterY, static_cast<std::int32_t>(kRingRadiusMin));
    const std::int32_t spread = RingSpread(ring);
    const bool x_plus = RandomBelow(state, 2) == 0;
    if (x_plus) {
      m.target_pos_x = px + static_cast<float>(ring) +
                       static_cast<float>(RandomBelow(state, spread));
      m.target_vel_x = static_cast<float>(RandomBelow(state, 200)) *
                       static_cast<float>(kNegManeuverScale);
    } else {
      m.target_pos_x = (px - static_cast<float>(ring)) -
                       static_cast<float>(RandomBelow(state, spread));
      m.target_vel_x = static_cast<float>(RandomBelow(state, 200)) *
                       static_cast<float>(kManeuverScale);
    }
    const bool y_plus = RandomBelow(state, 2) == 0;
    if (y_plus) {
      m.target_pos_y = py + static_cast<float>(ring) +
                       static_cast<float>(RandomBelow(state, spread));
      m.target_vel_y = static_cast<float>(RandomBelow(state, 200)) *
                       static_cast<float>(kNegManeuverScale);
    } else {
      m.target_pos_y = (py - static_cast<float>(ring)) -
                       static_cast<float>(RandomBelow(state, spread));
      m.target_vel_y = static_cast<float>(RandomBelow(state, 200)) *
                       static_cast<float>(kManeuverScale);
    }
  }

  // Pick a roam direction (manoeuvre-type index 0..15) permitted by the system
  // bitmap, then seed the wander radius/speed/table value from that row.
  std::int16_t dir = 0;
  do {
    dir = RandomBelow(state, 0x10);
  } while ((sys->roaming_direction_bitmap & (1U << (dir & 0x1f))) == 0);
  m.wander_type = dir;

  const ManeuverTypeDef *row =
      state.scenario.ManeuverType(static_cast<std::int16_t>(dir + 0x80));
  const std::int32_t lifetime = row != nullptr ? row->lifetime : 0;
  m.wander_radius = static_cast<float>(RandomBelow(state, lifetime));
  const float speed_mult = row != nullptr ? row->wander_speed_multiplier : 1.0F;
  m.wander_speed = static_cast<float>(RandomBelow(state, 0x29) + 0x50) *
                   speed_mult * static_cast<float>(kManeuverScale);
  if (RandomBelow(state, 2) == 0) {
    m.wander_speed = -m.wander_speed;
  }
  m.wander_table_value =
      row != nullptr ? row->wander_table_value : static_cast<std::int16_t>(0);

  NovaLog::Debug("roaming spawn slot {} dir {} ring={} radius {}",
                 slot,
                 dir,
                 place_in_ring,
                 m.wander_radius);
  return static_cast<int>(slot);
}

// Mirrors System_InitRoamingShips (0x004216B0): restores the current system's
// roaming/asteroid-drift ship population on spaceflight entry / cross-system
// travel. When the system declares no roaming ships (roaming_ship_count < 1) it
// sets the NoRoamingShips latch (GameState.no_roaming_ships_latch): the
// original writes a 1 byte into
// g_random_encounter_fleet_defs[0x4d].availability_expression [0x94] (a scratch
// area); the clean-room stores it in the explicit game-state flag instead since
// that scratch buffer is not modelled. Otherwise it spawns `roaming_ship_count`
// roaming records (scatter placement) and pre-warms all 16 manoeuvre-pool slots
// with a random wander target around the player.
void NovaSystem_InitRoamingShips(GameState &state) {
  const System *sys =
      state.scenario.System(state.player.current_system_id + 0x80);
  if (sys == nullptr) {
    return;
  }
  if (sys->roaming_ship_count < 1) {
    state.no_roaming_ships_latch = true;
    return;
  }

  for (std::int16_t i = 0; i < sys->roaming_ship_count; ++i) {
    (void)NovaDude_SpawnRoamingShip(state, /*place_in_ring=*/false);
  }

  // Pre-warm all 16 pool slots with a random wander target around the player.
  for (ManeuverState &m : state.maneuver_pool) {
    m.target_pos_x = static_cast<float>(RandomBelow(state, kRoamingScatterX)) +
                     state.player.pos_x -
                     static_cast<float>(kRoamingScatterX) * kScatterCenter;
    m.target_pos_y = static_cast<float>(RandomBelow(state, kRoamingScatterY)) +
                     state.player.pos_y -
                     static_cast<float>(kRoamingScatterY) * kScatterCenter;
    m.target_vel_x = static_cast<float>(RandomBelow(state, 400) - 200) *
                     static_cast<float>(kManeuverScale);
    m.target_vel_y = static_cast<float>(RandomBelow(state, 400) - 200) *
                     static_cast<float>(kManeuverScale);
  }
}

} // namespace game
