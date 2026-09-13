#include "collision.hpp"

#include "asteroid.hpp"
#include "freeflight_objects.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "impact_effects.hpp"
#include "mission.hpp"
#include "mission_script.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "ship_ai.hpp"
#include "spaceflight.hpp"
#include "sprite_mask.hpp"
#include "targeting.hpp"
#include "weapon.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <utility>

namespace game {
namespace {

constexpr float kImpulseCloseRangePx = 50.0F;   // DAT_0057522c
constexpr float kMaxManeuverTimerOnHit = 20.0F; // DAT_00575224
constexpr float kShieldDepletionFloorFraction =
    0.1F; // DAT_00575208: shields recharge from a pool bounded at -10% of max
constexpr float kPlayerAggroPerHitScale = 1.5F; // DAT_00575240
constexpr float kProximitySpanFraction =
    0.333005F; // DAT_00575338: blast + ship half-span * ~1/3
constexpr std::int16_t kShipClassInvalidSentinel = 0x2ff;
// Clean-room circle radius for a freeflight resource-box when no sprite mask
// is available (the original scoop arm always uses the opaque-pixel test).
// Only used when the spin set cannot be decoded (e.g. archive-free tests).
constexpr int kFreeflightScoopCircleRadiusPx = 16;
// Disable-transition armor pin (Shot_ResolveShipHitFromWeapon 0x0041a4b0):
// _DAT_00575238 = 1/3 and _DAT_00575208 = 0.1 for capability-flags 0x10 hulls,
// both +1.0 (_DAT_00575230).
constexpr float kDisableArmorPinFraction = 1.0F / 3.0F;
constexpr float kDisableArmorPinFractionCap0x10 = 0.1F;

// Asteroid debris particle constants from Weapon_SpawnWeaponImpactEffectPackage
// (0x00462550): speed DAT_00575740 = 0.2 px/tick, speed scatter 0x28, lifetime
// [0xf0, 0x1e0], and position scatter = sprite frame height / 3. Every shipped
// asteroid spin set (800..815) is 50x50, so the original's
// Sprite_GetFrameFullHeight(*asteroid)/3 reduces to 16
// (AsteroidState.collision_radius_px records the 25 px half-span).
constexpr float kAsteroidDebrisParticleSpeed = 0.2F;
constexpr std::int16_t kAsteroidDebrisParticleScatter = 0x28;
constexpr std::int16_t kAsteroidDebrisLifeBase = 0xf0;
constexpr std::int16_t kAsteroidDebrisLifeMax = 0x1e0;
constexpr std::int16_t kAsteroidDebrisPositionScatter = 16;

// The asteroid row stores a packed 15-bit tint; the original converts it to
// the surface pixel format at load. Expand 5-bit channels to 8-bit for the
// 24-bit SDL particle color (the standard (v << 3) | (v >> 2) expansion).
[[nodiscard]] std::uint32_t ExpandAsteroidParticleColor(std::uint32_t rgb555) {
  const std::uint32_t r = (rgb555 >> 10U) & 0x1fU;
  const std::uint32_t g = (rgb555 >> 5U) & 0x1fU;
  const std::uint32_t b = rgb555 & 0x1fU;
  return ((r << 3U) | (r >> 2U)) << 16U | ((g << 3U) | (g >> 2U)) << 8U |
         ((b << 3U) | (b >> 2U));
}

// Ghidra 0x004115c0 Ship_ClearShipState0x09Or0x0FToIdle.
// The sole hit-resolution callsite invokes this after Ship_SetShipHostileTo-
// Player has changed the state to 4, making the check normally inert. Keep
// that original ordering; this helper intentionally does not touch squad
// attachment.
void ClearState9OrFToIdle(Ship &ship) {
  if (ship.ai_state_code != 9 && ship.ai_state_code != 0x0F) {
    return;
  }
  ship.ai_secondary_target_slot = -1;
  ship.primary_target_ship_slot = -1;
  ship.ai_maneuver_timer_ms = 0.0F;
  ship.ai_state_code = 0;
  ship.ai_control_mode = 0;
}

// Ghidra Shot_ResolveShipHitFromWeapon player arms: quick-fail the first
// active, unfailed mission with flags 0x0004 (fail when the player is
// disabled/destroyed), with the STR# 0x7d2 0x11c overlay.
void QuickFailPlayerDependencyMissions(GameState &state) {
  for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions; ++slot) {
    const MissionRuntimeFlags &runtime =
        state.active_mission_runtime_flags[slot];
    const ActiveMission &mission = state.active_missions[slot];
    if (!runtime.is_active || runtime.is_failed ||
        (mission.flags_primary & 0x0004U) == 0U) {
      continue;
    }
    state.pending_ui_sounds.push_back(GameState::PendingUiSound{1, 1});
    if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x11c)) {
      NovaHud_ShowOverlayMessage(
          state, *text, /*duration_frames=*/std::uint64_t{0xf0});
    }
    Mission_FailMissionSlotQuick(state,
                                 static_cast<std::int16_t>(slot),
                                 static_cast<std::uint32_t>(SDL_GetTicks()));
    break;
  }
}

[[nodiscard]] const Weapon *WeaponForShot(const GameState &state,
                                          const ActiveShot &shot) {
  if (shot.weapon_id < 0 || shot.weapon_id >= 0x100) {
    return nullptr;
  }
  return state.scenario.Weapon(
      static_cast<std::int16_t>(shot.weapon_id + 0x80));
}

[[nodiscard]] bool ValidShipSlot(std::int16_t slot) {
  return slot >= 0 && slot < static_cast<std::int16_t>(GameState::kMaxShips);
}

// Ghidra Ship_IsShipDestroyed (0x004688e0): death timer running or armor gone.
[[nodiscard]] bool IsDestroyed(const Ship &ship) {
  return ship.death_timer_active > 0.0F || ship.armor_points <= 0.0F;
}

[[nodiscard]] const ShipClass *ShipClassFor(const GameState &state,
                                            const Ship &ship) {
  return state.scenario.Ship(
      static_cast<std::int16_t>(ship.ship_class_id + 0x80));
}

[[nodiscard]] bool HasGovernmentFlag(const GameState &state,
                                     const Ship &ship,
                                     std::uint16_t flag) {
  if (ship.faction_or_government_id < 0 ||
      ship.faction_or_government_id >=
          static_cast<std::int16_t>(state.scenario.governments.size())) {
    return false;
  }
  return (state.scenario
              .governments[static_cast<std::size_t>(
                  ship.faction_or_government_id)]
              .flags_primary &
          flag) != 0;
}

[[nodiscard]] const DudeDef *DudeFor(const GameState &state, const Ship &ship) {
  if (ship.dude_class_id < 0) {
    return nullptr;
  }
  return state.scenario.Dude(
      static_cast<std::int16_t>(ship.dude_class_id + 0x80));
}

// Ghidra Stellar_ShipImmuneToStellarCrash (0x0046e210). A ship survives flying
// into a fatal stellar (availability_flags 0x100) when its class carries ship
// availability_flags 0x20, or -- for the player only -- when the player owns
// any outfit whose mod slots include ModType 0x2a. Deliberately distinct from
// Stellar_ShipHasGravityShielding (0x0046e120): the NPC flags_secondary 0x40
// gate and the ModType 0x26/0x29 gravity-shield outfits do NOT grant crash
// immunity. The original caches the player-outfit roll in DAT_007356c6; the
// clean-room recomputes it (identical while the inventory is unchanged).
[[nodiscard]] bool Stellar_ShipImmuneToStellarCrash(const GameState &state,
                                                    const Ship &ship) {
  const ShipClass *ship_class = ShipClassFor(state, ship);
  if (ship_class != nullptr && (ship_class->availability_flags & 0x20U) != 0U) {
    return true;
  }
  // The outfit scan is player-only: NPC ships (ship_instance_id != 0) return
  // not-immune before it runs.
  if (ship.ship_instance_id != 0) {
    return false;
  }
  for (std::size_t id = 0; id < state.inventory.outfit_owned_count.size();
       ++id) {
    if (state.inventory.outfit_owned_count[id] <= 0) {
      continue;
    }
    const Outfit *outfit =
        state.scenario.Outfit(static_cast<std::int16_t>(id + 0x80));
    if (outfit == nullptr) {
      continue;
    }
    if (outfit->mod_type == 0x2a ||
        std::any_of(outfit->alt_mod_types.begin(),
                    outfit->alt_mod_types.end(),
                    [](std::int16_t type) { return type == 0x2a; })) {
      return true;
    }
  }
  return false;
}

// Ghidra Ship_ShipsShareSquadRoot (0x0046d190) walks each ship's
// squad_leader_ship_slot to a root and excludes shots within one squad-root
// chain. The walk is bounded to tolerate malformed cycles (the original is
// not; see the function's plate comment).
[[nodiscard]] bool SharesSquadRoot(const GameState &state,
                                   std::int16_t first_slot,
                                   std::int16_t second_slot) {
  auto root = [&state](std::int16_t slot) {
    std::int16_t current = slot;
    for (std::size_t step = 0; step < GameState::kMaxShips; ++step) {
      if (!ValidShipSlot(current)) {
        return current;
      }
      const std::int16_t next = state.ShipAt(static_cast<std::size_t>(current))
                                    .squad_leader_ship_slot;
      if (!ValidShipSlot(next) || next == current) {
        return current;
      }
      current = next;
    }
    return current;
  };
  return root(first_slot) == root(second_slot);
}

// Weapon_CanWeaponHitTarget walks the owner's squad_leader_ship_slot chain and
// only applies its player-escort/booty gates when that chain reaches slot 0.
[[nodiscard]] bool OwnerChainReachesPlayer(const GameState &state,
                                           std::int16_t owner_slot) {
  std::int16_t current = owner_slot;
  for (std::size_t step = 0; step < GameState::kMaxShips; ++step) {
    if (current == 0) {
      return true;
    }
    if (!ValidShipSlot(current)) {
      return false;
    }
    const std::int16_t next =
        state.ShipAt(static_cast<std::size_t>(current)).squad_leader_ship_slot;
    if (next == -1) {
      return false;
    }
    current = next;
  }
  return false;
}

[[nodiscard]] bool ShotIsInLateCollisionWindow(const ActiveShot &shot,
                                               const Weapon &weapon) {
  if (weapon.late_collision_window_ticks <= 0) {
    return false;
  }
  const float shot_age = std::max(0.0F,
                                  static_cast<float>(weapon.lifetime_ticks) -
                                      shot.life_ticks_remaining);
  const float collision_end_age = static_cast<float>(
      weapon.lifetime_ticks - weapon.late_collision_window_ticks);
  // Both Ship_HandleSpritePairCollision and Shot_ResolveCollisions reject
  // contacts once life_time drops below the late-window boundary.
  return collision_end_age < shot_age;
}

// Ghidra Weapon_ApplyWeaponOnHitEffects (0x0046f3f0). Ionization is applied
// unattenuated for the direct hit (the original passes impact_pos = NULL
// there) and distance-attenuated inside the splash radius otherwise.
void ApplyWeaponOnHitEffects(
    Ship &target,
    const Weapon &weapon,
    std::optional<std::pair<float, float>> impact_pos) {
  int points = weapon.ionization_points;
  if (impact_pos.has_value()) {
    const float dx = target.pos_x - impact_pos->first;
    const float dy = target.pos_y - impact_pos->second;
    const float radius = static_cast<float>(weapon.splash_radius);
    const float distance_sq = dx * dx + dy * dy;
    const float radius_sq = radius * radius;
    if (distance_sq > radius_sq) {
      points = 0;
    } else if (distance_sq > 0.0F) {
      points = static_cast<int>(static_cast<float>(points) *
                                (1.0F - distance_sq / radius_sq));
    }
  }
  if (points > 0) {
    target.ionization_points += static_cast<float>(points);
    target.ionization_color |= weapon.ionization_color;
  }
}

// Ghidra Ship_UpdateVisualState (0x00428340) / Shot_HandleShot (0x00435830) /
// Asteroid_UpdateSprites (0x00436910) frame selection, reused so the collision
// mask matches the frame the renderer presents this tick. Copied from the
// renderer's FrameForHeading (spaceflight_view.cpp) to keep the collision layer
// free of view state.
[[nodiscard]] int MaskFrameForHeading(float heading_radians,
                                      int frames_per_rotation) {
  if (frames_per_rotation <= 0) {
    return 0;
  }
  constexpr float kTwoPi = 6.28318530717958647692F;
  const float normalized = std::fmod(heading_radians + kTwoPi, kTwoPi);
  const float sector =
      (normalized / kTwoPi) * static_cast<float>(frames_per_rotation);
  int frame = static_cast<int>(std::lround(sector)) % frames_per_rotation;
  if (frame < 0) {
    frame += frames_per_rotation;
  }
  return frame;
}

// The original pre-subtracts the sprite half-span from the world position
// before Sprite_SetPositionFromCurrentFrameAnchor, whose stored frame anchor is
// (0,0) for the multi-frame ship/asteroid sheets
// (SpriteFrame_CreateFromRect 0x00476400 zeroes +0x2a/+0x2c). The effective
// collision-frame top-left is therefore world - (half_a, half_b), where ship
// and asteroid callers pass (ceil(height/2), ceil(width/2)) -- the two span
// helpers are swapped in Ship_UpdateVisualState (0x00428340) and
// Asteroid_UpdateSprites (0x00436910) -- and Shot_HandleShot (0x00435830)
// passes ceil(height/2) on both axes. Square frames reduce to the frame centre.
// `swapped_axes` selects the ship/asteroid (true) or shot (false) pair.
void BindEntityMask(CollisionMaskBinding &binding,
                    const SpriteMask *mask,
                    int frame,
                    bool swapped_axes) {
  if (mask == nullptr || mask->Empty()) {
    return;
  }
  const float half_height = static_cast<float>((mask->height + 1) / 2);
  const float half_width = static_cast<float>((mask->width + 1) / 2);
  binding.mask = mask;
  binding.anchor_x = half_height;
  binding.anchor_y = swapped_axes ? half_width : half_height;
  binding.frame = frame;
}

// k_pixel_collision_frame_scale_threshold_f64 (0x005754c8) and the
// Sprite_GetFrameFullHeight (0x00462390) boundary.
// Ship_HandleSpritePairCollision (0x004374f0) uses the opaque mask only when
// `g_avg_frame_tick_scale < 2.0` AND the target ship sprite's full frame height
// is > 0x20; otherwise it deliberately uses the bounding circle.
// `g_avg_frame_tick_scale` is the normalized 30 Hz simulation scale; the port's
// equivalent is GameState::last_frame_tick_scale.
constexpr float kPixelMaskFrameScaleThreshold = 2.0F; // 0x005754c8
constexpr int kPixelMaskFrameHeightThreshold = 0x20;

[[nodiscard]] bool ShipContactUsesPixelMask(const GameState &state,
                                            const Ship &target) {
  if (!(state.last_frame_tick_scale < kPixelMaskFrameScaleThreshold)) {
    return false;
  }
  if (!target.collision_mask.HasMask()) {
    return false;
  }
  return target.collision_mask.mask->height > kPixelMaskFrameHeightThreshold;
}

// Resolves each live entity's current-frame pixel mask from the non-SDL mask
// store, mirroring the sprite-frame selection the renderer performs
// (Ship_UpdateVisualState 0x00428340, Shot_HandleShot 0x00435830,
// Asteroid_UpdateSprites 0x00436910). The original's sprite layer carries these
// masks into Ship_HandleSpritePairCollision (0x004374f0) and
// Asteroid_HandleSpritePairCollision (0x00436f70); the clean-room sim has no
// sprite handles, so it refreshes the bindings from the same sheet/descriptor
// ids immediately before the direct-contact pass.
void RefreshCollisionMasks(GameState &state) {
  if (!state.collision_masks_enabled) {
    return;
  }
  if (!state.sprite_mask_store) {
    state.sprite_mask_store = std::make_shared<SpriteMaskStore>();
  }
  SpriteMaskStore &store = *state.sprite_mask_store;
  const std::int16_t system_id = state.player.current_system_id;

  for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.current_system_id != system_id ||
        ship.ship_class_id < 0) {
      continue;
    }
    const ShipClass *ship_class = ShipClassFor(state, ship);
    if (ship_class == nullptr || ship_class->base_image_id == 0 ||
        ship_class->frames_per_rotation <= 0) {
      continue;
    }
    int row = 0;
    if ((ship_class->sprite_behavior_flags & 1U) != 0U) {
      if (ship.ai_turn_bias_dir < 0) {
        row = 1;
      } else if (ship.ai_turn_bias_dir > 0) {
        row = 2;
      }
    }
    const int frame =
        row * ship_class->frames_per_rotation +
        MaskFrameForHeading(ship.heading, ship_class->frames_per_rotation);
    BindEntityMask(ship.collision_mask,
                   store.Sheet(ship_class->base_image_id, frame),
                   frame,
                   /*swapped_axes=*/true);
  }

  for (ActiveShot &shot : state.active_shots) {
    if (shot.system_id != system_id) {
      continue;
    }
    const Weapon *weapon = WeaponForShot(state, shot);
    if (weapon == nullptr) {
      continue;
    }
    const auto spin_id = static_cast<std::uint16_t>(weapon->sprite_id + 3000);
    const int frame_count = store.SpinFrameCount(spin_id);
    if (frame_count <= 0) {
      continue;
    }
    int frame = 0;
    if ((weapon->flags & 0x0001U) == 0) {
      const float bearing = std::atan2(shot.vel_x, -shot.vel_y);
      frame = MaskFrameForHeading(bearing, frame_count);
    } else {
      frame = std::clamp(shot.frame_cycle_index, 0, frame_count - 1);
    }
    BindEntityMask(shot.collision_mask,
                   store.Spin(spin_id, frame),
                   frame,
                   /*swapped_axes=*/false);
  }

  for (AsteroidState &asteroid : state.asteroid_pool) {
    if (!asteroid.active) {
      continue;
    }
    const auto spin_id = static_cast<std::uint16_t>(
        kAsteroidSpinBase + (asteroid.wander_type & 0x0f));
    const int frame_count = store.SpinFrameCount(spin_id);
    if (frame_count <= 0) {
      continue;
    }
    float wander = asteroid.wander_frame_accumulator;
    while (wander < 0.0F) {
      wander += static_cast<float>(frame_count);
    }
    while (wander >= static_cast<float>(frame_count)) {
      wander -= static_cast<float>(frame_count);
    }
    const int frame = std::clamp(static_cast<int>(wander), 0, frame_count - 1);
    BindEntityMask(asteroid.collision_mask,
                   store.Spin(spin_id, frame),
                   frame,
                   /*swapped_axes=*/true);
  }

  // Freeflight objects: the mining-scoop arm of Ship_HandleSpritePairCollision
  // (0x004374f0) tests each object's current spin frame against the ship. The
  // renderer selects `trunc(frame_counter) % frame_count` from the 500+index
  // spin set, so bind the same frame here.
  for (FreeflightObjectState &object : state.freeflight_objects) {
    if (object.lifetime_ticks < 0.0F || object.system_id != system_id) {
      continue;
    }
    const std::uint16_t spin_id =
        NovaFreeflightSpriteSetId(object.sprite_set_index);
    const int frame_count = store.SpinFrameCount(spin_id);
    if (frame_count <= 0) {
      continue;
    }
    int frame = static_cast<int>(object.frame_counter);
    frame %= frame_count;
    if (frame < 0) {
      frame += frame_count;
    }
    BindEntityMask(object.collision_mask,
                   store.Spin(spin_id, frame),
                   frame,
                   /*swapped_axes=*/true);
  }

  // Stellar ambient sprites: Ghidra Stellar_UpdateStellarSprites (0x0042cd10)
  // assigns the zone sprite set (link_a when the body is not active, link_b
  // when it is, the 0x80 availability bit inverting the zone) and places it at
  // map_x/map_y with the same pre-subtracted (ceil(height/2), ceil(width/2))
  // anchor as ships/asteroids. Only the current system's 16 nav stellars are
  // ever tested (Stellar_HandleShipStellarCrash 0x0043aed0 and the
  // flags_secondary 0x400 arm of Shot_ResolveCollisions 0x00437e20).
  const System *const sys =
      state.scenario.System(static_cast<std::int16_t>(system_id + 0x80));
  if (sys != nullptr) {
    for (const std::int16_t nav : sys->nav_defs) {
      if (nav < 0x80) {
        continue;
      }
      Stellar *stellar = state.scenario.StellarMutable(nav);
      if (stellar == nullptr || stellar->name.empty()) {
        continue;
      }
      const std::int16_t link = NovaTargeting_StellarSpriteLinkId(*stellar);
      if (link < 0) {
        continue;
      }
      const auto spin_id = static_cast<std::uint16_t>(link + 1000);
      const int frame_count = store.SpinFrameCount(spin_id);
      if (frame_count <= 0) {
        continue;
      }
      const int frame = std::clamp(
          static_cast<int>(stellar->sprite_current_frame), 0, frame_count - 1);
      BindEntityMask(stellar->collision_mask,
                     store.Spin(spin_id, frame),
                     frame,
                     /*swapped_axes=*/true);
    }
  }
}

// Math_AddPolarVelocityWithClamp (0x0043b4e0): add impact/mass in the
// impact-to-target direction, then clamp each component to the class's
// base-speed bound. Ship-class Speed is stored in hundredths of px/frame.
// TODO(decomp) skipped: the original clamps against
// Ship_ComputeShipEffectiveMaxSpeed (outfit opcode-8 contributions, NPC skill
// variance, government combat-rating scale) and widens the player clamp 1.8x
// while the afterburner is active (DAT_00575218) without gravity pull.
void ApplyImpactImpulse(GameState &state,
                        Ship &target,
                        float impact_x,
                        float impact_y,
                        std::int16_t impact_impulse) {
  if (impact_impulse == 0 || target.ai_station_hold_timer > 0.0F) {
    return;
  }
  const ShipClass *target_class = ShipClassFor(state, target);
  if (target_class == nullptr || target_class->mass_tons <= 0 ||
      (target_class->capability_flags & 0x0400U) != 0U) {
    return;
  }

  const float dx = target.pos_x - impact_x;
  const float dy = target.pos_y - impact_y;
  const float distance = std::hypot(dx, dy);
  if (distance <= 0.0F) {
    return;
  }

  const float impulse = static_cast<float>(impact_impulse) /
                        static_cast<float>(target_class->mass_tons);
  target.vel_x += dx / distance * impulse;
  target.vel_y += dy / distance * impulse;
  const float max_axis_speed = target_class->speed / 100.0F;
  if (max_axis_speed > 0.0F) {
    target.vel_x = std::clamp(target.vel_x, -max_axis_speed, max_axis_speed);
    target.vel_y = std::clamp(target.vel_y, -max_axis_speed, max_axis_speed);
  }
}

// Shot_ResolveShipHitFromWeapon's leave-one-armor rule: a disable-variant hit
// stops at 1 armor point instead of destroying the hull.
void ApplyArmorDamage(Ship &target, int armor_damage, bool force_armor_only) {
  if (armor_damage <= 0) {
    return;
  }
  if (force_armor_only && target.armor_points > 0.0F &&
      target.armor_points - static_cast<float>(armor_damage) <= 0.0F) {
    target.armor_points = 1.0F;
  } else {
    target.armor_points -= static_cast<float>(armor_damage);
  }
}

// Ghidra Government_PropagateHostilityFromAttack (0x004102e0). A player hit
// alerts eligible combat ships related to the victim; in particular, ships of
// the victim's government join the response and target the player. The
// mission/distress gates below preserve the original's conservative response
// rules while keeping this clean-room pass limited to the represented fields.
void PropagateHostilityFromPlayerAttack(GameState &state,
                                        const Ship &target,
                                        std::int16_t owner_ship_slot) {
  if (owner_ship_slot != 0 || target.pers_def_slot == 0x3ff ||
      state.player.pers_def_slot == 0x3fe) {
    return;
  }

  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &responder = state.ShipAt(slot);
    if (!responder.is_active ||
        responder.current_system_id != state.player.current_system_id ||
        responder.ai_behavior_code < 3 || responder.ai_behavior_code > 4 ||
        responder.ai_state_code == 4 || responder.pers_def_slot == 0x3ff ||
        responder.faction_or_government_id < -1 ||
        responder.faction_or_government_id >= 0x100) {
      continue;
    }
    if (NovaAiShip_CanShipRespondToDistressCall(state, responder, target)) {
      continue;
    }

    bool eligible = true;
    const std::int16_t responder_govt = responder.faction_or_government_id;
    const std::int16_t target_govt = target.faction_or_government_id;
    if (responder_govt >= 0 && target_govt >= 0) {
      // Government_PropagateHostilityFromAttack first retains only ships that
      // are not already hostile to the victim, then admits allied/same-govt
      // responders. A policy-0 government can opt out when the attacker is the
      // player (or already targets the player).
      eligible = !NovaGovernment_AreGovtsHostileOrXenophobic(
          state.scenario, responder_govt, target_govt);
      if (HasGovernmentFlag(state, target, 0x0001U) &&
          responder_govt != target_govt) {
        eligible = false;
      }
      if (NovaGovernment_GetPolicyFlag(state.scenario, responder_govt, 0)) {
        eligible = false;
      }
      if (!NovaGovernment_AreGovtsAllied(
              state.scenario, responder_govt, target_govt) &&
          !NovaGovernment_AreGovtsAllied(
              state.scenario, target_govt, responder_govt) &&
          !HasGovernmentFlag(state, responder, 0x0002U)) {
        eligible = false;
      }
      if (NovaGovernment_AreGovtsAllied(
              state.scenario,
              responder_govt,
              state.player.faction_or_government_id)) {
        eligible = false;
      }
    } else if (responder_govt >= 0 && target_govt < 0) {
      eligible = HasGovernmentFlag(state, responder, 0x0002U);
    } else if (responder_govt < 0 && target_govt >= 0) {
      eligible = !HasGovernmentFlag(state, target, 0x0001U);
    }
    if (HasGovernmentFlag(state, target, 0x0800U) ||
        HasGovernmentFlag(state, target, 0x0020U)) {
      eligible = false;
    }
    if (!eligible) {
      continue;
    }
    if (responder.squad_leader_ship_slot == owner_ship_slot ||
        (ValidShipSlot(responder.squad_leader_ship_slot) &&
         state.ShipAt(
                  static_cast<std::size_t>(responder.squad_leader_ship_slot))
                 .squad_leader_ship_slot == owner_ship_slot)) {
      continue;
    }
    responder.ai_state_code = 4;
    responder.primary_target_ship_slot = owner_ship_slot;
  }
}

// Ghidra Shot_ResolveShipHitFromWeapon (0x004192d0). Core ship-hit
// resolution: impulse, shield-first/armor damage, disable-variant armor
// clamping, and the aggro/hostility response.
//
// TODO(decomp) skipped: the pers-attacker damage multiplier block - DECODED
// as Shareware Enforcer escalation: attackers with pers_def_slot 0x3ff
// (enforcer-template ships) deal x3 (days 0-40) / x2 (41-60) / x5 (61-90)
// armor+shield damage by the shareware trial day counter
// g_shareware_day_counter (0x0059799e, FUN_004d4480); 91+ days means no
// boost. Deliberately not reproduced in the clean-room (no shareware trial
// state). check_fire_restriction_transition is 0 from the shot paths and 1
// from the hull-destruction blast (Ship_UpdateVisualState 0x00428340), which
// arms the disable-transition armor pin (33%/10% + 1 armor) and the mission
// DISABLE bookkeeping (escort-goal quick-fail + goal_counter_c++, STR# 0x7d2
// 0x11c) and the player "disabled" overlay (STR# 0x7d2 0x11f). The full
// retarget gate chain (system reputation, government max-odds roll, cloak
// re-entry, escort-command exclusions, and the attacker-side
// Ship_IsInPlayerSquad gate at 0x0041AB03) is approximated by the
// conservative subset below. The stellar-target redirect branch (damage x30
// toward attackers closer than the stellar under attack) is also deferred.
} // namespace

// Ghidra 0x0046f1e0 Frame_AddCombatRatingPoints.
void NovaFrame_AddCombatRatingPoints(GameState &state, float points) {
  constexpr std::int32_t kMaxCombatRating = 10'000'000;
  if (state.player_combat_rating_points >= kMaxCombatRating) {
    state.player_combat_rating_points = kMaxCombatRating;
    return;
  }
  // The original truncates the points for the sub-5 test and otherwise adds
  // trunc(points * 0.2) (DAT_00575870 = 0.2).
  if (static_cast<int>(points) < 5) {
    state.player_combat_rating_points += 1;
    return;
  }
  const double scaled = static_cast<double>(state.player_combat_rating_points) +
                        static_cast<double>(points) * 0.2;
  state.player_combat_rating_points = static_cast<std::int32_t>(scaled);
}

void ResolveShipHitFromWeapon(GameState &state,
                              std::int16_t target_slot,
                              Ship &target,
                              float impact_x,
                              float impact_y,
                              std::int16_t impact_impulse,
                              std::int16_t armor_damage,
                              std::int16_t shield_damage,
                              std::int16_t attacker_ship_slot,
                              bool allow_aggro_updates,
                              bool suppress_retarget_logic,
                              bool force_armor_only,
                              bool bypass_shields,
                              std::int16_t player_aggro_delta,
                              bool check_fire_restriction_transition) {
  if (!ValidShipSlot(target_slot) || !target.is_active ||
      target.ship_class_id < 0) {
    return;
  }
  const bool attacker_valid = ValidShipSlot(attacker_ship_slot);

  // Ghidra 0x00415e80 Ship_IsShipInAiState0x0D runs inline here.
  // Disable-mode attackers (AI state 0x0D) and their leaders force the
  // leave-one-armor variant so their fire disables rather than destroys the
  // target they are locked on.
  if (allow_aggro_updates && attacker_valid && attacker_ship_slot > 0 &&
      state.ShipAt(static_cast<std::size_t>(attacker_ship_slot))
              .primary_target_ship_slot == target_slot) {
    const Ship &attacker =
        state.ShipAt(static_cast<std::size_t>(attacker_ship_slot));
    if (attacker.ai_state_code == 0x0D) {
      force_armor_only = true;
    } else if (attacker.squad_leader_ship_slot > 0 &&
               attacker.squad_leader_ship_slot <
                   static_cast<std::int16_t>(GameState::kMaxShips) &&
               state.ShipAt(static_cast<std::size_t>(
                                attacker.squad_leader_ship_slot))
                       .ai_state_code == 0x0D) {
      force_armor_only = true;
    }
  }

  // Negative impulses (pull beams) are ignored at point-blank range.
  if (impact_impulse < 0 && allow_aggro_updates && attacker_valid &&
      (std::abs(
           state.ShipAt(static_cast<std::size_t>(attacker_ship_slot)).pos_x -
           target.pos_x) < kImpulseCloseRangePx ||
       std::abs(
           state.ShipAt(static_cast<std::size_t>(attacker_ship_slot)).pos_y -
           target.pos_y) < kImpulseCloseRangePx)) {
    impact_impulse = 0;
  }

  const bool was_destroyed = IsDestroyed(target);
  // Ghidra local_12a: disabled (disabled) before this hit applied.
  const bool was_fire_restricted = NovaAiShip_IsDisabled(state, target);

  ApplyImpactImpulse(state, target, impact_x, impact_y, impact_impulse);

  const ShipClass *target_class = ShipClassFor(state, target);

  // Shields are consumed first; once they are down the armor damage applies.
  // Depleted shields recharge from a pool bounded at -10% of the ship's max.
  if (!bypass_shields) {
    if (shield_damage > 0) {
      target.shield_points -= static_cast<float>(shield_damage);
    }
    if (target.shield_points <= 0.0F) {
      ApplyArmorDamage(target, armor_damage, force_armor_only);
      if (target_class != nullptr) {
        const float shield_floor =
            -kShieldDepletionFloorFraction *
            static_cast<float>(
                std::max(0, static_cast<int>(target_class->base_shield)));
        if (target.shield_points < shield_floor) {
          target.shield_points = shield_floor;
        }
      }
    }
  } else {
    ApplyArmorDamage(target, armor_damage, force_armor_only);
  }

  // Shot_ResolveShipHitFromWeapon leaves destruction as an armor-state: the
  // hit site only records the alive -> destroyed transition and clears the
  // slot's targeting references. Ship_HandleShip (0x00433050) seeds and runs
  // the NPC death presentation, and Ship_UpdateVisualState (0x00428340) seeds
  // the timer for NPCs and the player (DeathDelay, x3 for the player via
  // g_player_death_timer_scale 0x00575378), drives the Explode1 debris cadence
  // and spawns the Explode2 finale. No explosion or timer is spawned here.
  if (!was_destroyed && IsDestroyed(target) &&
      !target.destruction_visual_triggered) {
    target.destruction_visual_triggered = true;
    NovaTargeting_ClearDestroyedShipReferences(state, target_slot);
    // Kill-side faction event + combat rating (Ghidra 0x00419748): the victim's
    // government takes a kill-event reputation pulse and the player gains the
    // class's combat value. Skipped for the Shareware Enforcer personalities
    // (pers_def_slot >= 0x3ff, the 0x004196e3 gate) and for derelict
    // governments (Government_IsShipGovernmentDerelict).
    if (target.pers_def_slot < 0x3ff &&
        !NovaGovernment_IsGovernmentDerelict(state.scenario,
                                             target.faction_or_government_id)) {
      NovaGovernment_ProcessFactionCombatEvent(state,
                                               state.player.current_system_id,
                                               target.faction_or_government_id,
                                               3,
                                               target.mission_fleet_slot);
      if (target_class != nullptr) {
        NovaFrame_AddCombatRatingPoints(
            state, static_cast<float>(target_class->strength));
      }
    }
  }

  // ---- Fire-restriction (disable) transition arms (0x0041a4b0..) ---------
  const bool now_fire_restricted = NovaAiShip_IsDisabled(state, target);
  if (now_fire_restricted) {
    // Disable-side faction event (Ghidra 0x00419721): the transition into the
    // disabled state pulses event 1; the same pers_def_slot >= 0x3ff gate
    // applies.
    if (!was_fire_restricted && target.pers_def_slot < 0x3ff) {
      NovaGovernment_ProcessFactionCombatEvent(state,
                                               state.player.current_system_id,
                                               target.faction_or_government_id,
                                               1,
                                               target.mission_fleet_slot);
    }
    // Disable-transition armor pin: armor locks at 33% of max (+1 armor;
    // 10% for capability-flags 0x10 hulls), keeping the hull disabled
    // (Ship_HandleShip suppresses regeneration while restricted). Only the
    // destruction-blast caller passes the transition flag, exactly like the
    // original's check_fire_restriction_transition argument.
    if (check_fire_restriction_transition && !was_fire_restricted &&
        target_class != nullptr) {
      const float max_armor =
          target_slot == 0 && state.stat_cache_valid
              ? state.cached_stats.max_armor_points
              : static_cast<float>(target_class->base_armor);
      target.armor_points =
          (target_class->capability_flags & 0x10) != 0U
              ? max_armor * kDisableArmorPinFractionCap0x10 + 1.0F
              : max_armor * kDisableArmorPinFraction + 1.0F;
    }
    // Mission DISABLE bookkeeping: counts one disable per mission ship on the
    // transition into disable restriction; an escort-goal (spawn_behavior 3)
    // mission quick-fails on the first disable unless flags 0x0400 (invisible)
    // hid it.
    const std::int16_t fleet_slot = target.mission_fleet_slot;
    if (!was_fire_restricted && fleet_slot >= 0 &&
        fleet_slot < static_cast<std::int16_t>(GameState::kMaxActiveMissions) &&
        target.defense_fleet_home_stellar_id == -1) {
      MissionRuntimeFlags &runtime =
          state.active_mission_runtime_flags[static_cast<std::size_t>(
              fleet_slot)];
      ActiveMission &mission =
          state.active_missions[static_cast<std::size_t>(fleet_slot)];
      if (runtime.is_active) {
        if (!runtime.is_failed && mission.goal_counter_c == 0 &&
            mission.spawn_behavior == 3 &&
            (mission.flags_primary & 0x0400U) == 0U) {
          state.pending_ui_sounds.push_back(GameState::PendingUiSound{1, 1});
          if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x11c)) {
            NovaHud_ShowOverlayMessage(
                state, *text, /*duration_frames=*/std::uint64_t{0xf0});
          }
          Mission_FailMissionSlotQuick(
              state, fleet_slot, static_cast<std::uint32_t>(SDL_GetTicks()));
        }
        mission.goal_counter_c =
            static_cast<std::int16_t>(mission.goal_counter_c + 1);
      }
    }
    // Post-hit behavior hint for surrendered escorts (squad_leader_ship_slot 0,
    // not a mission ship): stores how the boarding/escort-conversion flow
    // treats this hull. Cargo transfer for behavior-6 escorts is TODO(decomp)
    // (Outfit_TransferCargoAndJunkToEscortByRatio 0x00469810).
    if (target.squad_leader_ship_slot == 0 && fleet_slot == -1) {
      target.post_hit_mode_hint =
          target.ai_behavior_code == 5
              ? 0
              : (target.escort_origin_mark == 0 ? 2 : 1);
      target.squad_leader_ship_slot = -1;
      target.ai_behavior_code = target_class != nullptr
                                    ? target_class->default_ai_behavior
                                    : target.ai_behavior_code;
    }
    // Player disable arm (transition): "ship disabled" overlay, rearm the
    // recent-hit regen latch, truncate armor to whole points (original quirk),
    // and quick-fail every mission flagged 0x0004 (fail on player disable).
    if (target_slot == 0 && !was_fire_restricted) {
      state.pending_ui_sounds.push_back(GameState::PendingUiSound{1, 1});
      if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x11f)) {
        NovaHud_ShowOverlayMessage(
            state, *text, /*duration_frames=*/std::uint64_t{0xf0});
      }
      state.recently_hit_timer = 300.0F;
      target.armor_points =
          static_cast<float>(static_cast<int>(target.armor_points));
      QuickFailPlayerDependencyMissions(state);
    }
  }

  // Player destruction arm (transition into destroyed): "ship destroyed"
  // overlay unless the disable or a Shareware-Enforcer taunt already showed,
  // plus the same flags-0x0004 mission quick-fail sweep.
  if (target_slot == 0 && IsDestroyed(target) && !was_destroyed) {
    state.pending_ui_sounds.push_back(GameState::PendingUiSound{1, 1});
    bool taunt_shown = false;
    if (allow_aggro_updates && ValidShipSlot(attacker_ship_slot) &&
        attacker_ship_slot > 0 &&
        state.ShipAt(static_cast<std::size_t>(attacker_ship_slot))
                .pers_def_slot == 0x3ff) {
      // STR# 30000 entries 8..13: Shareware Enforcer taunts.
      std::uniform_int_distribution<std::int32_t> taunt_roll{0, 5};
      if (auto text = NovaHud_LoadStringEntry(
              30000, static_cast<std::uint16_t>(8 + taunt_roll(state.rng)))) {
        NovaHud_ShowOverlayMessage(
            state, *text, /*duration_frames=*/std::uint64_t{5000});
        taunt_shown = true;
      }
    }
    if (!taunt_shown && !state.player_disable_message_shown) {
      if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x120)) {
        NovaHud_ShowOverlayMessage(
            state, *text, /*duration_frames=*/std::uint64_t{0xf0});
      }
    }
    state.player_disable_message_shown = true;
    QuickFailPlayerDependencyMissions(state);
  }

  if (allow_aggro_updates && target.ai_behavior_code > 0 &&
      target.ai_station_hold_timer <= 0.0F) {
    // Player-owned fire accumulates the per-hit aggro weight (weapon reload
    // scaled by DAT_00575240) regardless of whether the target retargets.
    if (attacker_ship_slot == 0 && !suppress_retarget_logic) {
      target.player_aggro_accumulator +=
          static_cast<float>(player_aggro_delta) * kPlayerAggroPerHitScale;
    }

    const bool self_hit = attacker_valid && attacker_ship_slot == target_slot;
    if (!self_hit && target_slot != 0) {
      // Hostility accumulates from positive damage only, and the escort
      // leader (when it has no stellar assignment) shares the hit.
      if (armor_damage > 0) {
        target.ai_hostility_accumulator = static_cast<std::int16_t>(std::min(
            0x7fff,
            static_cast<int>(target.ai_hostility_accumulator) + armor_damage));
      }
      if (shield_damage > 0) {
        target.ai_hostility_accumulator = static_cast<std::int16_t>(std::min(
            0x7fff,
            static_cast<int>(target.ai_hostility_accumulator) + shield_damage));
      }
      if (attacker_valid) {
        target.primary_target_ship_slot = attacker_ship_slot;
      }
      target.player_aggro_accumulator = 0.0F;
      const std::int16_t leader = target.squad_leader_ship_slot;
      if (leader > 0 &&
          leader < static_cast<std::int16_t>(GameState::kMaxShips) &&
          state.ShipAt(static_cast<std::size_t>(leader))
                  .defense_fleet_home_stellar_id == -1) {
        Ship &leader_ship = state.ShipAt(static_cast<std::size_t>(leader));
        if (armor_damage > 0) {
          leader_ship.ai_hostility_accumulator = static_cast<std::int16_t>(
              std::min(0x7fff,
                       static_cast<int>(leader_ship.ai_hostility_accumulator) +
                           armor_damage));
        }
        if (shield_damage > 0) {
          leader_ship.ai_hostility_accumulator = static_cast<std::int16_t>(
              std::min(0x7fff,
                       static_cast<int>(leader_ship.ai_hostility_accumulator) +
                           shield_damage));
        }
        if (attacker_valid) {
          leader_ship.primary_target_ship_slot = attacker_ship_slot;
        }
      }
    }
    // TODO(decomp) skipped: the stellar-target redirect branch, which awards
    // 30x hostility and retargets ships that are attacking a stellar when the
    // attacker is closer than half the squared distance to that stellar.

    if (attacker_ship_slot == 0) {
      // Ship_SetShipHostileToPlayer (0x00410700) deliberately changes the
      // primary target/state only. squad_leader_ship_slot is the squad-leader
      // attachment link (not a combat target); overwriting it here makes the
      // next player shot look like friendly fire through
      // Ship_ShipsShareSquadRoot.
      NovaAi_SetShipHostileToPlayer(state, target);
      if (target.ai_maneuver_timer_ms > kMaxManeuverTimerOnHit) {
        target.ai_maneuver_timer_ms = kMaxManeuverTimerOnHit;
      }
      if (target_slot != 0 && target.defense_fleet_home_stellar_id == -1) {
        ClearState9OrFToIdle(target);
        target.primary_target_ship_slot = 0;
      }
    }
    if (suppress_retarget_logic && attacker_ship_slot == 0) {
      PropagateHostilityFromPlayerAttack(state, target, attacker_ship_slot);
    }
  }

  // Shot_ResolveShipHitFromWeapon refreshes the non-bypass hit reaction timer.
  // The clean-room renderer does not consume this yet, but retaining the latch
  // prevents later status/AI work from losing the event.
  if (!bypass_shields) {
    target.hit_reaction_timer = 32.0F;
  }
  // TODO(decomp) skipped: cloak_damage_deactivate_latch handling
  // (Ship_OnShipCloakStateCleared) and g_player_status_panel_dirty.
}

// Ghidra Shot_ResolveShotCollisionHit (0x00437780). Applies the primary area
// impact effect, the direct damage/impulse/ionization package, and the axis-
// aligned splash to every other eligible ship.
//
// TODO(decomp) skipped: Weapon_SpawnWeaponImpactParticleBurst (the SWParticle
// impact flurries; gated on WeaponDef.impact_particle_count > 0). ShotState
// +0x32 damage_reduction is not carried on ActiveShot; the original zero-
// initializes it and no writer was found, so the subtraction is a no-op.
//
// `shot_index` is used instead of a reference because the linked-shot spawner
// appends to GameState::active_shots and may reallocate it; the shot is only
// re-accessed through the index after the spawn.
void ResolveShotCollisionHit(GameState &state,
                             std::size_t shot_index,
                             Ship &target,
                             std::int16_t target_slot,
                             bool allow_linked_shots) {
  ActiveShot &shot = state.active_shots[shot_index];
  if (target.ship_class_id < 0 ||
      target.ship_class_id == kShipClassInvalidSentinel) {
    return;
  }
  const Weapon *weapon = WeaponForShot(state, shot);
  if (weapon == nullptr) {
    return;
  }

  NovaEffects_SpawnAreaImpact(state,
                              shot.pos_x,
                              shot.pos_y,
                              weapon->impact_effect_id,
                              weapon->splash_radius,
                              true);
  NovaEffects_SpawnWeaponImpactBurstForWeapon(
      state, shot.pos_x, shot.pos_y, *weapon, /*scatter=*/0x14);

  const int armor_damage = weapon->mass_damage;
  const int shield_damage = weapon->energy_damage;

  // Direct-target ionization is unattenuated (impact_pos = NULL upstream).
  ApplyWeaponOnHitEffects(target, *weapon, std::nullopt);

  const bool suppress_retarget_logic = shot.target_ship_slot == target_slot;
  // The per-hit player aggro weight is the weapon's reload interval, rounded.
  const auto player_aggro_delta = static_cast<std::int16_t>(
      std::lround(static_cast<float>(weapon->reload_ticks)));
  const bool target_was_destroyed = IsDestroyed(target);

  ResolveShipHitFromWeapon(state,
                           target_slot,
                           target,
                           shot.pos_x,
                           shot.pos_y,
                           weapon->impact_impulse,
                           armor_damage,
                           shield_damage,
                           shot.owner_ship_slot,
                           /*allow_aggro_updates=*/true,
                           suppress_retarget_logic,
                           /*force_armor_only=*/shot.impact_variant != 0,
                           /*bypass_shields=*/
                           (weapon->flags & 0x0020U) != 0U,
                           player_aggro_delta);

  if (weapon->splash_radius > 0) {
    for (std::int16_t slot = 0;
         slot < static_cast<std::int16_t>(GameState::kMaxShips);
         ++slot) {
      // The direct target does not take splash damage on top.
      if (slot == target_slot) {
        continue;
      }
      // Owner immunity: NPC owners never splash themselves; the player is
      // splashed by their own weapon unless flags_primary 0x100 is set
      // (polarity verified against disassembly at 0x00437ac0).
      if (slot == shot.owner_ship_slot &&
          ((weapon->flags & 0x0100U) != 0U || shot.owner_ship_slot != 0)) {
        continue;
      }
      Ship &splash_target = state.ShipAt(static_cast<std::size_t>(slot));
      // The original checks only is_active and the class range here - no
      // system or capability gate - so splash crosses systems by raw
      // position. Quirk preserved.
      if (!splash_target.is_active || splash_target.ship_class_id < 0 ||
          splash_target.ship_class_id == kShipClassInvalidSentinel) {
        continue;
      }
      if (std::abs(splash_target.pos_x - shot.pos_x) >
              static_cast<float>(weapon->splash_radius) ||
          std::abs(splash_target.pos_y - shot.pos_y) >
              static_cast<float>(weapon->splash_radius)) {
        continue;
      }
      // Splash keeps the full damage (no variant reduction) but honors the
      // disable variant's leave-one-armor rule; no aggro updates.
      ResolveShipHitFromWeapon(state,
                               slot,
                               splash_target,
                               shot.pos_x,
                               shot.pos_y,
                               weapon->impact_impulse,
                               weapon->mass_damage,
                               weapon->energy_damage,
                               shot.owner_ship_slot,
                               /*allow_aggro_updates=*/false,
                               /*suppress_retarget_logic=*/false,
                               /*force_armor_only=*/shot.impact_variant != 0,
                               /*bypass_shields=*/
                               (weapon->flags & 0x0020U) != 0U,
                               /*player_aggro_delta=*/0);
      ApplyWeaponOnHitEffects(
          splash_target, *weapon, std::make_pair(shot.pos_x, shot.pos_y));
    }
  }

  // Kill-chatter witness loop: ships whose escort chain is rooted at the
  // player (ai_target == 0) and that were attacking the destroyed ship queue
  // one combat chatter line unless their class is flagged mute
  // (flags_secondary 0x10). Ghidra Shot_ResolveShotCollisionHit tail.
  if (!target_was_destroyed && IsDestroyed(target)) {
    for (std::int16_t slot = 1;
         slot < static_cast<std::int16_t>(GameState::kMaxShips);
         ++slot) {
      const Ship &witness = state.ShipAt(static_cast<std::size_t>(slot));
      if (slot == target_slot || !witness.is_active ||
          witness.squad_leader_ship_slot != 0 ||
          witness.primary_target_ship_slot != target_slot ||
          witness.ai_behavior_code <= 2 || witness.ai_state_code != 4) {
        continue;
      }
      const ShipClass *witness_class = ShipClassFor(state, witness);
      if (witness_class == nullptr ||
          (witness_class->flags_secondary & 0x10U) != 0U) {
        continue;
      }
      NovaFrame_QueueCombatChatter(state,
                                   2,
                                   witness_class->inherent_attributes_govt,
                                   witness.voice_type_mode);
    }
  }
  // Ghidra Shot_SpawnLinkedShotsOnImpact (0x00420d30) is reached only from
  // the proximity/blast pass (flag != 0); the direct sprite-contact pass
  // passes 0. Snapshot the impacting shot before the spawner can reallocate
  // GameState::active_shots.
  if (allow_linked_shots && weapon->range_link_gate > 0) {
    const ActiveShot impacting_shot = state.active_shots[shot_index];
    NovaWeapon_SpawnLinkedShotsOnImpact(state, impacting_shot, target_slot);
  }

  state.active_shots[shot_index].consumed = true;
}

// Ghidra Weapon_SpawnWeaponImpactEffectPackage (0x00462550), asteroid arm:
// a broken asteroid spawns YieldQty resource-box freeflight objects, a debris
// particle burst, its destruction area effect, and splits into child asteroids
// from its def row (child types +0x06/+0x08, count derived from +0x0a), then
// deactivates. The debris burst runs between the resource boxes and the area
// effect, matching the original RNG sequence.
//
// Resource-boxes: when YieldQty (row +0x02) > 0 the original rolls
// `(NovaRandom_Range(0x65) + 0x32) * YieldQty * 0.01`, truncates it, and spawns
// that many persistent freeflight objects at the asteroid position carrying
// YieldType (row +0x04) with spin set `(wander_type >> 2) + 1` (501..504, one
// per asteroid material). YieldType 0..5 is standard cargo and 1000..1127 is a
// j\xfcnk id; Ship_HandleSpritePairCollision's scoop arm grants one unit of it.
void ResolveAsteroidDestructionPackage(GameState &state,
                                       AsteroidState &asteroid) {
  const AsteroidDef *def = state.scenario.AsteroidType(
      static_cast<std::int16_t>(asteroid.wander_type + 0x80));
  if (def == nullptr) {
    asteroid.active = false;
    return;
  }
  // Resource-boxes first, matching the original's field order before the
  // area effect and the child-asteroid split.
  if (def->yield_qty > 0) {
    const int roll = std::uniform_int_distribution<int>{0, 0x64}(state.rng);
    const int yield_boxes =
        static_cast<int>(static_cast<float>(roll + 0x32) *
                         static_cast<float>(def->yield_qty) * 0.01F);
    for (int i = 0; i < yield_boxes; ++i) {
      NovaFreeflight_SpawnAtPosition(
          state,
          asteroid.target_pos_x,
          asteroid.target_pos_y,
          def->yield_type,
          static_cast<std::int16_t>((asteroid.wander_type >> 2) + 1));
    }
  }
  // Debris SWParticle burst (row +0x0c count, +0x18 color).
  NovaEffects_SpawnWeaponImpactParticleBurst(
      state,
      asteroid.target_pos_x,
      asteroid.target_pos_y,
      kAsteroidDebrisParticleSpeed,
      kAsteroidDebrisParticleScatter,
      kAsteroidDebrisLifeBase,
      kAsteroidDebrisLifeMax,
      ExpandAsteroidParticleColor(def->color),
      /*blend_mode=*/0x20,
      def->field_0x0c,
      kAsteroidDebrisPositionScatter);
  if (def->field_0x10 != -1) {
    NovaEffects_SpawnAreaImpact(state,
                                asteroid.target_pos_x,
                                asteroid.target_pos_y,
                                def->field_0x10,
                                0,
                                true);
  }
  const int count_base = def->directions[2];
  if (count_base > 0) {
    const int count =
        std::uniform_int_distribution<int>{0, count_base - 1}(state.rng) +
        (count_base + 1) / 2;
    for (int i = 0; i < count; ++i) {
      // Both child types unset means the original spawns nothing; a single
      // set type always spawns; otherwise a random pick per fragment.
      if (def->directions[0] == -1 && def->directions[1] == -1) {
        continue;
      }
      std::int16_t child_type = def->directions[0];
      if (def->directions[0] != -1 && def->directions[1] != -1) {
        child_type = def->directions[std::uniform_int_distribution<int>{0, 1}(
            state.rng)];
      } else if (def->directions[0] == -1) {
        child_type = def->directions[1];
      }
      (void)NovaAsteroid_SpawnRecord(
          state, asteroid.target_pos_x, asteroid.target_pos_y, child_type);
    }
  }
  asteroid.active = false;
}

// Ghidra NovaUi_ResolveWeaponSplashImpact (0x00436ff0): a blast weapon that
// reaches an asteroid (flags_quaternary bit 0 clear) spawns the area impact,
// splashes nearby ships (player-owned shots only), decrements the asteroid's
// integrity counter by the weapon's shield damage, and either runs the
// destruction package or nudges the asteroid along the impact.
void ResolveAsteroidSplashImpact(GameState &state,
                                 ActiveShot &shot,
                                 AsteroidState &asteroid) {
  const Weapon *weapon = WeaponForShot(state, shot);
  if (weapon == nullptr) {
    shot.consumed = true;
    return;
  }

  NovaEffects_SpawnAreaImpact(state,
                              shot.pos_x,
                              shot.pos_y,
                              weapon->impact_effect_id,
                              weapon->splash_radius,
                              true);
  NovaEffects_SpawnWeaponImpactBurstForWeapon(
      state, shot.pos_x, shot.pos_y, *weapon, /*scatter=*/0x14);

  // Ship splash: only player-owned shots splash from asteroid hits, and
  // unlike the shot-vs-ship splash this arm does exclude immaterial
  // (capability 0x400) ships.
  if (shot.owner_ship_slot == 0 && weapon->splash_radius > 0 &&
      (weapon->flags_secondary & 0x0400U) == 0U) {
    for (std::int16_t slot = 0;
         slot < static_cast<std::int16_t>(GameState::kMaxShips);
         ++slot) {
      // Owner immunity polarity as in Shot_ResolveShotCollisionHit: the
      // player is splashed unless flags_primary 0x100 is set.
      if (slot == shot.owner_ship_slot && (weapon->flags & 0x0100U) != 0U) {
        continue;
      }
      Ship &splash_target = state.ShipAt(static_cast<std::size_t>(slot));
      if (!splash_target.is_active || splash_target.ship_class_id < 0) {
        continue;
      }
      const ShipClass *splash_class = ShipClassFor(state, splash_target);
      if (splash_class == nullptr ||
          (splash_class->capability_flags & 0x0400U) != 0U) {
        continue;
      }
      if (std::abs(splash_target.pos_x - shot.pos_x) >
              static_cast<float>(weapon->splash_radius) ||
          std::abs(splash_target.pos_y - shot.pos_y) >
              static_cast<float>(weapon->splash_radius)) {
        continue;
      }
      ResolveShipHitFromWeapon(
          state,
          slot,
          splash_target,
          shot.pos_x,
          shot.pos_y,
          weapon->impact_impulse,
          weapon->mass_damage,
          weapon->energy_damage,
          shot.owner_ship_slot,
          /*allow_aggro_updates=*/false,
          /*suppress_retarget_logic=*/false,
          /*force_armor_only=*/shot.impact_variant != 0,
          /*bypass_shields=*/(weapon->flags & 0x0020U) != 0U,
          static_cast<std::int16_t>(
              std::lround(static_cast<float>(weapon->reload_ticks))));
      ApplyWeaponOnHitEffects(
          splash_target, *weapon, std::make_pair(shot.pos_x, shot.pos_y));
    }
  }

  // Integrity counter: flags_secondary 0x8000 weapons strip 10x.
  if ((weapon->flags_secondary & 0x8000U) == 0U) {
    asteroid.integrity =
        static_cast<std::int16_t>(asteroid.integrity - weapon->energy_damage);
  } else {
    asteroid.integrity = static_cast<std::int16_t>(asteroid.integrity +
                                                   weapon->energy_damage * -10);
  }

  if (asteroid.integrity < 0) {
    ResolveAsteroidDestructionPackage(state, asteroid);
  } else if (weapon->impact_impulse != 0) {
    // Surviving asteroids are nudged along the impact direction. The original
    // reads the bearing from ShotState +0x20 (provisional range_scalar_runtime
    // in the DB); the clean-room derives it from the shot's velocity. The
    // impulse is divided by the def's size-scaled mass (row +0x0e) and the
    // result clamped to +-2.0 px/frame per axis (DAT_0057531c).
    const AsteroidDef *def = state.scenario.AsteroidType(
        static_cast<std::int16_t>(asteroid.wander_type + 0x80));
    if (def != nullptr && def->lifetime > 0) {
      const float speed = static_cast<float>(weapon->impact_impulse) /
                          static_cast<float>(def->lifetime);
      const float bearing_deg = std::atan2(shot.vel_x, -shot.vel_y) *
                                (180.0F / 3.14159265358979323846F);
      const float rad = bearing_deg * (3.14159265358979323846F / 180.0F);
      asteroid.target_vel_x += std::sin(rad) * speed;
      asteroid.target_vel_y += -std::cos(rad) * speed;
      asteroid.target_vel_x = std::clamp(asteroid.target_vel_x, -2.0F, 2.0F);
      asteroid.target_vel_y = std::clamp(asteroid.target_vel_y, -2.0F, 2.0F);
    }
  }

  shot.consumed = true;
}

void RemoveConsumedShots(GameState &state) {
  state.active_shots.erase(
      std::remove_if(state.active_shots.begin(),
                     state.active_shots.end(),
                     [](const ActiveShot &shot) { return shot.consumed; }),
      state.active_shots.end());
}

namespace {} // namespace

// Ghidra Shot_ResolveShipHitFromWeapon (0x004192d0) wrapper: resolve a hit
// against a ship given by slot (validates the slot / active state).
void NovaCollision_ResolveShipHitFromWeaponSlot(
    GameState &state,
    std::int16_t target_slot,
    float impact_x,
    float impact_y,
    std::int16_t impact_impulse,
    std::int16_t armor_damage,
    std::int16_t shield_damage,
    std::int16_t attacker_ship_slot,
    bool allow_aggro_updates,
    bool suppress_retarget_logic,
    bool force_armor_only,
    bool bypass_shields,
    std::int16_t player_aggro_delta) {
  if (!ValidShipSlot(target_slot)) {
    return;
  }
  ResolveShipHitFromWeapon(state,
                           target_slot,
                           state.ShipAt(static_cast<std::size_t>(target_slot)),
                           impact_x,
                           impact_y,
                           impact_impulse,
                           armor_damage,
                           shield_damage,
                           attacker_ship_slot,
                           allow_aggro_updates,
                           suppress_retarget_logic,
                           force_armor_only,
                           bypass_shields,
                           player_aggro_delta);
}

// Public test seam for the internal RefreshCollisionMasks pass (resolves each
// live entity's current-frame sprite mask from the non-SDL store).
void NovaCollision_RefreshCollisionMasks(GameState &state) {
  RefreshCollisionMasks(state);
}

// Ghidra Stellar_TickStellarDefenseBatteries (0x0042d890).
//
// Runs in Frame_TickSystems scope 8 before Stellar_TickStellarGravityPull /
// Stellar_HandleShipStellarCrash. Walk the 16 nav stellars of the player's
// current system; a battery fires only when the stellar is_available, carries a
// weapon (StellarDef +0x2c), and is NOT in its "active" (destroyed/engaged)
// state. The cooldown (StellarDef +0x494) counts down by the frame tick scale;
// on expiry the nearest hostile ship within range(weapon)^2 is chosen and a
// shot is spawned. The cooldown is reloaded only when a shot actually spawns;
// NovaWeapon_SpawnStellarBatteryShot preserves the original 128-shot shared
// pool limit, so a full pool retries next frame.
//
// The original reads g_weapon_defs[StellarDef.field_0x2c]. That field is a
// weapon BANK slot, not the raw resource id: the loader (0x004bd3c0) reads the
// raw spob Weapon word (+0x23a), maps values < 0x80 to -1 and subtracts 0x80
// otherwise (Spacedock II 196 -> bank 68 = Enormous Blaster Turret). The port
// keeps the raw resource id in Stellar.weapon_id and resolves it through
// ScenarioData::Weapon (the same -0x80), so the intended weapon fires. The
// disabled-target gate reproduces the original's bVar4: a weapon that is
// disable-only (flags_secondary 0x1000) or has no mass damage skips
// already-disabled hulls; a lethal weapon does not.
void NovaStellar_TickStellarDefenseBatteries(GameState &state) {
  const System *const system = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (system == nullptr) {
    return;
  }
  const float tick_scale = state.last_frame_tick_scale;
  for (const std::int16_t nav : system->nav_defs) {
    if (nav == -1) {
      continue;
    }
    Stellar *const stellar = state.scenario.StellarMutable(nav);
    if (stellar == nullptr || !stellar->is_available ||
        stellar->weapon_id == -1 || NovaTargeting_IsStellarActive(*stellar)) {
      continue;
    }
    if (stellar->defense_battery_cooldown > 0.0F) {
      stellar->defense_battery_cooldown -= tick_scale;
      continue;
    }
    const Weapon *const weapon = state.scenario.Weapon(stellar->weapon_id);
    if (weapon == nullptr) {
      continue;
    }
    const float range_sq = weapon->range_scalar * weapon->range_scalar;
    // bVar4 == true when the weapon cannot destroy (disable-only or no mass
    // damage): such a battery does not target already-disabled hulls.
    const bool skip_disabled_targets = !(
        (weapon->flags_secondary & 0x1000U) == 0U && weapon->mass_damage != 0);
    std::int16_t best_slot = -1;
    float best_dist_sq = 0.0F;
    for (std::int16_t slot = 0;
         slot < static_cast<std::int16_t>(GameState::kMaxShips);
         ++slot) {
      const Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
      if (!ship.is_active ||
          NovaTargeting_ShipAtCloakVisibilityThreshold(ship) ||
          !NovaGovernment_IsCandidateHostileToTargeter(
              state, ship, *stellar, nav)) {
        continue;
      }
      const float dx = ship.pos_x - static_cast<float>(stellar->pos_x);
      const float dy = ship.pos_y - static_cast<float>(stellar->pos_y);
      const float dist_sq = dx * dx + dy * dy;
      if (dist_sq > range_sq) {
        continue;
      }
      if (skip_disabled_targets && NovaAiShip_IsDisabled(state, ship)) {
        continue;
      }
      if (best_slot == -1 || dist_sq < best_dist_sq) {
        best_dist_sq = dist_sq;
        best_slot = slot;
      }
    }
    if (best_slot == -1) {
      continue;
    }
    if (NovaWeapon_SpawnStellarBatteryShot(
            state, *stellar, best_slot, stellar->weapon_id) < 0) {
      continue;
    }
    // Reload, applying the burst-cycle wrap: while the weapon fires a burst the
    // counter advances and, when it reaches burst_cycle_ticks (the
    // Weapon_GetWeaponFireIntervalTicks result for a single mount), the counter
    // resets and the longer reset cooldown is used instead.
    std::int16_t cooldown = weapon->reload_ticks;
    if (weapon->burst_cycle_ticks > 0) {
      stellar->burst_shot_count =
          static_cast<std::int16_t>(stellar->burst_shot_count + 1);
      if (weapon->burst_cycle_ticks <= stellar->burst_shot_count) {
        stellar->burst_shot_count = 0;
        cooldown = weapon->burst_reset_cooldown;
      }
    }
    stellar->defense_battery_cooldown = static_cast<float>(cooldown);
  }
}

// Ghidra Stellar_HandleShipStellarCrash (0x0043aed0). Physical collision pass
// running in Frame_TickSystems scope 8, immediately after
// Stellar_TickStellarGravityPull (0x0043adb0). For each fatal stellar
// (availability_flags 0x100) of the current system whose ambient sprite is
// loaded, every active, non-destroyed, non-immune ship is tested with the
// shared opaque pixel-mask overlap Sprite_TestPixelMaskOverlap (0x00475c80). A
// contact is an instant, animation-free kill: the hull is deactivated, armor is
// forced to -1000.0f (0xc47a0000), the death timer is zeroed, the player's
// primary target is cleared when it was the victim, and the class's final
// explosion effect (Explode2, ShipClassDef +0xa1e) is spawned at the ship. The
// ship scan does not stop after a kill, and a deactivated hull is skipped by
// the remaining stellars.
void NovaStellar_HandleShipStellarCrash(GameState &state) {
  const System *const sys = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (sys == nullptr) {
    return;
  }
  for (const std::int16_t nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const Stellar *const stellar = state.scenario.Stellar(nav);
    if (stellar == nullptr || (stellar->availability_flags & 0x100U) == 0U) {
      continue;
    }
    // `StellarDef +0` is the live ambient Sprite*; the clean-room gate is the
    // resolved mask (a failed spin load leaves no collision body).
    if (!stellar->collision_mask.HasMask()) {
      continue;
    }
    for (std::int16_t slot = 0;
         slot < static_cast<std::int16_t>(GameState::kMaxShips);
         ++slot) {
      Ship &ship = state.ShipAt(static_cast<std::size_t>(slot));
      if (!ship.is_active || IsDestroyed(ship)) {
        continue;
      }
      if (Stellar_ShipImmuneToStellarCrash(state, ship)) {
        continue;
      }
      if (!ship.collision_mask.HasMask()) {
        continue;
      }
      if (!SpriteMask_TestOverlap(*stellar->collision_mask.mask,
                                  static_cast<float>(stellar->pos_x),
                                  static_cast<float>(stellar->pos_y),
                                  stellar->collision_mask.anchor_x,
                                  stellar->collision_mask.anchor_y,
                                  *ship.collision_mask.mask,
                                  ship.pos_x,
                                  ship.pos_y,
                                  ship.collision_mask.anchor_x,
                                  ship.collision_mask.anchor_y)) {
        continue;
      }

      // Instant kill: deactivate instead of entering the death presentation.
      ship.is_active = false;
      ship.armor_points = -1000.0F;
      ship.death_timer_active = 0.0F;
      if (state.player.primary_target_ship_slot == slot) {
        state.player.primary_target_ship_slot = -1;
        // g_playerShipPresentationDirty is implicit in the clean-room's
        // immediate-mode HUD/status redraw.
      }
      const ShipClass *const ship_class = ShipClassFor(state, ship);
      NovaEffects_SpawnAreaImpact(
          state,
          ship.pos_x,
          ship.pos_y,
          ship_class == nullptr ? -1 : ship_class->destruction_effect_final,
          0,
          true);
    }
  }
}

bool NovaWeapon_CanProjectileHitShip(const GameState &state,
                                     const ActiveShot &shot,
                                     std::int16_t target_slot) {
  // Ghidra 0x00426ef0 Weapon_CanWeaponHitTarget.
  const Weapon *weapon = WeaponForShot(state, shot);
  if (weapon == nullptr || !ValidShipSlot(target_slot) || shot.consumed) {
    return false;
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  // Original self-hit gate: owner_ship_slot == target_ship->ship_instance_id,
  // and ship_instance_id is the target's own slot (identity == slot).
  if (shot.owner_ship_slot == target_slot) {
    return false;
  }
  // Ghidra 0x00426f6f: the 998 "target lost" latch makes the shot inert.
  if (shot.retarget_cooldown == 998) {
    return false;
  }
  if (!target.is_active || target.current_system_id != shot.system_id ||
      IsDestroyed(target)) {
    return false;
  }
  if (target.pers_def_slot == 0x3ff) {
    return false;
  }
  if (target.ship_class_id < 0 ||
      target.ship_class_id == kShipClassInvalidSentinel) {
    return false;
  }

  // Mode-1 shots reject a contact other than the recorded target unless
  // flags_secondary bit 0x08 opts into the broader path.
  if (weapon->weapon_mode_code == 1 &&
      (weapon->flags_secondary & 0x0008U) == 0 &&
      shot.target_ship_slot != target_slot) {
    return false;
  }

  const bool owner_valid = ValidShipSlot(shot.owner_ship_slot);
  if (!owner_valid) {
    // Ownerless shots (stellar defense batteries, owner_ship_slot -1). Ghidra
    // 0x00427435: with a valid recorded target slot the shot hits only the ship
    // occupying that slot, or a ship whose defense_fleet_home_stellar_id
    // matches it; an invalid recorded slot accepts any target. The owner-based
    // gates are skipped and control joins the original's common tail below.
    const std::int16_t recorded_target = shot.target_ship_slot;
    if (recorded_target >= 0 && recorded_target < 0x40 &&
        target_slot != recorded_target &&
        target.defense_fleet_home_stellar_id != recorded_target) {
      return false;
    }
  } else {
    const std::int16_t owner_slot = shot.owner_ship_slot;
    const Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_slot));
    // Clean-room guard: the original relies on shot lifetime to drop dead or
    // departed owners; the explicit checks keep stale shots from damaging
    // across systems in this simplified model.
    if (owner.current_system_id != shot.system_id || IsDestroyed(owner)) {
      return false;
    }
    if (owner.squad_leader_ship_slot != -1 &&
        target.squad_leader_ship_slot != -1 &&
        target.squad_leader_ship_slot == owner.squad_leader_ship_slot) {
      return false;
    }
    if (owner.faction_or_government_id >= 0 &&
        owner.faction_or_government_id < 0x100 &&
        target.faction_or_government_id == owner.faction_or_government_id) {
      return false;
    }
    if (owner.defense_fleet_home_stellar_id != -1 &&
        target.defense_fleet_home_stellar_id ==
            owner.defense_fleet_home_stellar_id) {
      return false;
    }
    // TODO(decomp) skipped: the disabled range gate comparing
    // ShipClassDef +0xa10 against ShotState +0x42 (per-shot scatter range,
    // 0xffff for non-turret modes) - the field semantics are still provisional.

    const bool owner_chain_to_player =
        OwnerChainReachesPlayer(state, owner_slot);
    if (owner_chain_to_player) {
      // Player-aligned fire never hits the player's own squad,
      // xenophobic (0x40) owner governments, immaterial (0x08) target
      // governments, or booty-flagged (0x100) dude targets.
      //
      // Ghidra 0x004272C7: the original ANDs Ship_IsInPlayerSquad (0x0046b8d0)
      // with an explicit leader clause (target_leader in [0,0x40) whose own
      // leader is the player). The first term is implied by the second, so
      // this keeps the original's structure.
      const std::int16_t target_leader = target.squad_leader_ship_slot;
      const bool target_leader_attached_to_player =
          target_leader >= 0 &&
          target_leader < static_cast<std::int16_t>(GameState::kMaxShips) &&
          state.ShipAt(static_cast<std::size_t>(target_leader))
                  .squad_leader_ship_slot == 0;
      if (NovaShip_IsInPlayerSquad(state, target) &&
          target_leader_attached_to_player) {
        return false;
      }
      if (HasGovernmentFlag(state, owner, 0x0040U)) {
        return false;
      }
      if (HasGovernmentFlag(state, target, 0x0008U)) {
        return false;
      }
      const DudeDef *target_dude = DudeFor(state, target);
      if (target_dude != nullptr &&
          (target_dude->booty_flags & 0x0100U) != 0U) {
        return false;
      }
      // Owner-side booty gate: when the owner has no valid dude record, the
      // owner's squad leader's dude record is consulted instead.
      const DudeDef *owner_dude = DudeFor(state, owner);
      if (owner_dude == nullptr &&
          ValidShipSlot(owner.squad_leader_ship_slot)) {
        owner_dude = DudeFor(state,
                             state.ShipAt(static_cast<std::size_t>(
                                 owner.squad_leader_ship_slot)));
      }
      if (owner_dude != nullptr && (owner_dude->booty_flags & 0x0100U) != 0U) {
        return false;
      }
    }
    // Ghidra 0x00415e60 Ship_IsShipInAiState0x10 runs inline here and in the
    // beam-impact guard below.
    if (owner_slot > 0 && owner.ai_state_code == 0x10) {
      return false;
    }
  }

  // Common tail reached by owned and ownerless shots: the target's planet-type
  // capability bit must agree with weapon flags_secondary bit 0x400.
  const ShipClass *target_class = ShipClassFor(state, target);
  if (target_class == nullptr ||
      ((target_class->capability_flags ^ weapon->flags_secondary) & 0x0400U) !=
          0U) {
    return false;
  }
  // Government aggro flag (0x40): NPC targets carrying it are only hittable
  // by owners that are neither the player nor player escorts. An ownerless
  // shot (owner_ship_slot -1) falls through this gate in the original.
  if (target_slot != 0 && HasGovernmentFlag(state, target, 0x0040U)) {
    const std::int16_t owner_slot = shot.owner_ship_slot;
    if (owner_slot == 0) {
      return false;
    }
    if (ValidShipSlot(owner_slot) && owner_slot > 0 &&
        state.ShipAt(static_cast<std::size_t>(owner_slot))
                .squad_leader_ship_slot == 0) {
      return false;
    }
  }
  if (owner_valid &&
      SharesSquadRoot(state, target_slot, shot.owner_ship_slot)) {
    return false;
  }
  return true;
}

// Ghidra Asteroid_HandleSpritePairCollision (0x00436f70): the sprite-layer
// callback installed on the 16 asteroid sprites. Ship_TestSpriteLayerOverlaps
// invokes it for every (asteroid sprite, shot sprite) overlap. It resolves the
// shot and asteroid records from the sprites' presented_object_id (+0xC8),
// rejects weapons whose flags_quaternary bit 0x0001 is set (Seeker 1 "passes
// over asteroids"), does a pixel-mask overlap test
// (Sprite_TestPixelMaskOverlap 0x00475c80), then funnels the contact through
// NovaUi_ResolveWeaponSplashImpact. Unlike the ship callback there is no
// late-collision-window gate. The clean-room resolves each asteroid's current
// wander frame mask before the pass and falls back to the circle envelope only
// when a mask is unavailable.
void ResolveDirectAsteroidContact(GameState &state,
                                  ActiveShot &shot,
                                  const Weapon &weapon) {
  if ((weapon.flags_quaternary & 0x0001U) != 0U) {
    return;
  }
  for (AsteroidState &asteroid : state.asteroid_pool) {
    if (!asteroid.active) {
      continue;
    }
    // Pixel-mask overlap, falling back to the circle when either side has no
    // resolved sprite mask. The original's Asteroid_HandleSpritePairCollision
    // (0x00436f70) always uses Sprite_TestPixelMaskOverlap.
    if (!CollisionMask_TestContact(shot.collision_mask,
                                   shot.pos_x,
                                   shot.pos_y,
                                   static_cast<int>(std::lround(std::max(
                                       0.0F, shot.collision_radius_px))),
                                   asteroid.collision_mask,
                                   asteroid.target_pos_x,
                                   asteroid.target_pos_y,
                                   static_cast<int>(std::lround(std::max(
                                       0.0F, asteroid.collision_radius_px))))) {
      continue;
    }
    ResolveAsteroidSplashImpact(state, shot, asteroid);
    return;
  }
}

// Ghidra Ship_HandleSpritePairCollision (0x004374f0) with its sprite-layer
// driver inlined: the original is invoked per overlapping (ship sprite, shot
// sprite) pair by TestSpriteLayerOverlaps and picks the bounding-circle or
// pixel-mask test by frame-time budget (circle when avg frame time >= 2.0ms
// or half-span <= 0x20). Asteroid_HandleSpritePairCollision (0x00436f70) runs
// over the same shot containers against g_asteroid_sprite_layer; the
// reimplementation evaluates ships first, then asteroids for a shot that did
// not already connect. Both contacts test the decoded per-frame pixel masks
// (Sprite_TestPixelMaskOverlap 0x00475c80) with the bounding-circle fallback.
void NovaWeapon_ResolveDirectShotCollisions(GameState &state) {
  // Resolve the per-frame sprite masks the original's sprite layer would have
  // carried into the two pair-collision callbacks before testing contacts.
  RefreshCollisionMasks(state);
  for (std::size_t shot_index = 0; shot_index < state.active_shots.size();
       ++shot_index) {
    ActiveShot &shot = state.active_shots[shot_index];
    if (shot.life_ticks_remaining <= 0.0F && shot.life_frames > 0) {
      // Compatibility for records created by older callers/tests that only
      // populated the original integer lifetime view.
      shot.life_ticks_remaining = static_cast<float>(shot.life_frames);
    }
    if (shot.consumed || shot.life_ticks_remaining <= 0.0F) {
      continue;
    }
    const Weapon *weapon = WeaponForShot(state, shot);
    if (weapon == nullptr || shot.system_id != state.player.current_system_id) {
      continue;
    }

    for (std::int16_t slot = 0;
         slot < static_cast<std::int16_t>(GameState::kMaxShips);
         ++slot) {
      const Ship &target = state.ShipAt(static_cast<std::size_t>(slot));
      if (!target.is_active ||
          target.current_system_id != state.player.current_system_id) {
        continue;
      }
      if (!NovaWeapon_CanProjectileHitShip(state, shot, slot)) {
        continue;
      }
      // Pixel-mask overlap (Sprite_TestPixelMaskOverlap 0x00475c80), with the
      // bounding-circle fallback (Sprite_TestBoundingCircleOverlap 0x00475be0).
      // Ship_HandleSpritePairCollision (0x004374f0) selects the mask only when
      // the frame scale is < 2.0 and the ship frame height > 0x20; otherwise it
      // deliberately uses the circle.
      if (!CollisionMask_TestContact(
              shot.collision_mask,
              shot.pos_x,
              shot.pos_y,
              static_cast<int>(
                  std::lround(std::max(0.0F, shot.collision_radius_px))),
              target.collision_mask,
              target.pos_x,
              target.pos_y,
              static_cast<int>(
                  std::lround(std::max(0.0F, target.collision_radius_px))),
              /*allow_pixel_mask=*/ShipContactUsesPixelMask(state, target))) {
        continue;
      }
      if (ShotIsInLateCollisionWindow(shot, *weapon)) {
        break;
      }
      ResolveShotCollisionHit(state,
                              shot_index,
                              state.ShipAt(static_cast<std::size_t>(slot)),
                              slot,
                              /*allow_linked_shots=*/false);
      break;
    }

    // Asteroid contact pass. The original's late-collision window only gates
    // the ship callback, so a shot in its fuse window may still strike an
    // asteroid directly (Ship_HandleSpritePairCollision vs 0x00436f70).
    if (!shot.consumed) {
      ResolveDirectAsteroidContact(state, shot, *weapon);
    }
  }
  RemoveConsumedShots(state);
}

// Ghidra Ship_HandleSpritePairCollision (0x004374f0), freeflight-object arm:
// the sprite layer pairs every ship with every freeflight object; an eligible
// ship that opaque-pixel-overlaps a *persistent* object collects it. A ship is
// eligible when it is the player with the mining_scoop_active latch set or an
// NPC in AI state 0x11. Collection retires the object (lifetime -1, sprite
// hidden) and grants one unit of its payload: `extra` 0..5 adds to that
// commodity bin, and (player only) 1000..1127 adds to the matching j\xfcnk
// count. The original then sets g_playerInventoryAndLoadoutDirty and calls
// Outfit_RecomputeOutfitDerivedState, whose mining-scoop arm clears the latch
// once cargo+junk reaches fleet capacity; the port routes that through
// NovaOutfit_RecomputeOutfitDerivedState. Unlike the shot arm there is no
// bounding-circle fallback: the original always runs
// Sprite_TestPixelMaskOverlap. The clean room still falls back to the circle
// only when a mask cannot be decoded.
void NovaWeapon_ResolveFreeflightScoop(GameState &state) {
  RefreshCollisionMasks(state);
  const std::int16_t system_id = state.player.current_system_id;
  for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.current_system_id != system_id) {
      continue;
    }
    const bool player_ship = ship.ship_instance_id == 0;
    for (FreeflightObjectState &object : state.freeflight_objects) {
      if (object.lifetime_ticks < 0.0F || !object.persistent ||
          object.system_id != system_id) {
        continue;
      }
      // Re-read the latch per object: the original re-runs the outfit
      // recompute after every pickup, so a full hold stops mid-pass.
      // Ghidra 0x00414530 Ship_IsShipInAiState0x11 runs inline here.
      const bool eligible = player_ship ? state.player.mining_scoop_active
                                        : ship.ai_state_code == 0x11;
      if (!eligible) {
        break;
      }
      if (!CollisionMask_TestContact(ship.collision_mask,
                                     ship.pos_x,
                                     ship.pos_y,
                                     static_cast<int>(std::lround(std::max(
                                         0.0F, ship.collision_radius_px))),
                                     object.collision_mask,
                                     object.pos_x,
                                     object.pos_y,
                                     kFreeflightScoopCircleRadiusPx,
                                     /*allow_pixel_mask=*/true)) {
        continue;
      }
      object.lifetime_ticks = -1.0F;
      const std::int16_t payload = object.extra;
      if (payload >= 0 && payload <= 5) {
        if (player_ship) {
          state.inventory.cargo_bins[static_cast<std::size_t>(payload)] =
              static_cast<std::int16_t>(
                  state.inventory
                      .cargo_bins[static_cast<std::size_t>(payload)] +
                  1);
        }
        // TODO(decomp): NPC cargo bins live on the original ShipState
        // (+0x7a) but the clean-room does not model NPC holds; the NPC arm is
        // also gated by AI state 0x11, which is not reconstructed yet.
      } else if (player_ship && payload >= 1000 && payload < 0x468) {
        const std::size_t index = static_cast<std::size_t>(payload - 1000);
        if (index < state.inventory.junk_counts.size()) {
          state.inventory.junk_counts[index] =
              static_cast<std::int16_t>(state.inventory.junk_counts[index] + 1);
        }
      }
      if (player_ship) {
        // g_playerInventoryAndLoadoutDirty + Outfit_RecomputeOutfitDerivedState
        // (0x0046d4b0), including the cargo-capacity mining-scoop gate.
        NovaOutfit_RecomputeOutfitDerivedState(state);
      }
    }
  }
}

// Ghidra Shot_ResolveCollisions (0x00437e20) stellar arm. Planet-type weapons
// (flags_secondary 0x400) test their current-frame pixel mask against the
// current system's 16 nav stellar sprites. A body is eligible when its live
// ambient sprite is loaded (mask resolved) and it is neither destroyed nor
// engaged (Stellar_IsStellarActive 0x0046e3c0). A contact subtracts combined
// energy+mass damage from the body's live Strength (+0x3c), spawns the
// weapon's area impact, and kills the shot. When Strength goes negative the
// body explodes with its ExplodType (+0x46e), runs its OnDestroy script
// (+0x266), re-arms its regeneration countdown (+0x47c = schedule seed +0x47a)
// and -- for a player-owned shot -- fires 10 faction-combat events. The scan
// stops at the first overlapping nav entry. Returns true when the shot dies.
bool ResolveShotStellarContact(GameState &state,
                               ActiveShot &shot,
                               const Weapon &weapon) {
  const System *const sys = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (sys == nullptr) {
    return false;
  }
  for (const std::int16_t nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    Stellar *const stellar = state.scenario.StellarMutable(nav);
    if (stellar == nullptr || stellar->strength_capacity <= 0 ||
        NovaTargeting_IsStellarActive(*stellar)) {
      continue;
    }
    // The original pixel test requires both prepared frames; the clean-room
    // equivalent is a resolved mask. Unlike ship/asteroid contact there is no
    // bounding-circle fallback on this arm.
    if (!shot.collision_mask.HasMask() || !stellar->collision_mask.HasMask()) {
      continue;
    }
    if (!SpriteMask_TestOverlap(*shot.collision_mask.mask,
                                shot.pos_x,
                                shot.pos_y,
                                shot.collision_mask.anchor_x,
                                shot.collision_mask.anchor_y,
                                *stellar->collision_mask.mask,
                                static_cast<float>(stellar->pos_x),
                                static_cast<float>(stellar->pos_y),
                                stellar->collision_mask.anchor_x,
                                stellar->collision_mask.anchor_y)) {
      continue;
    }

    // Strength takes the combined mass+energy damage (Bible "Strength").
    stellar->strength -= static_cast<std::int32_t>(weapon.energy_damage) +
                         static_cast<std::int32_t>(weapon.mass_damage);
    NovaEffects_SpawnAreaImpact(state,
                                shot.pos_x,
                                shot.pos_y,
                                weapon.impact_effect_id,
                                weapon.splash_radius,
                                true);
    NovaEffects_SpawnWeaponImpactBurstForWeapon(
        state, shot.pos_x, shot.pos_y, weapon, /*scatter=*/0x14);
    shot.life_ticks_remaining = -1.0F;
    shot.consumed = true;

    if (NovaTargeting_IsStellarActive(*stellar)) {
      if (stellar->explosion_type != -1) {
        NovaEffects_SpawnAreaImpact(state,
                                    static_cast<float>(stellar->pos_x),
                                    static_cast<float>(stellar->pos_y),
                                    stellar->explosion_type,
                                    0,
                                    true);
      }
      // The original logs the display name + OnDestroy string first, then runs
      // the reaction script. The log is diagnostic only.
      (void)Mission_ExecuteReactionScript(state, stellar->on_destroy_script);
      stellar->strength = -1;
      stellar->engage_access = stellar->schedule_days;
      if (shot.owner_ship_slot == 0) {
        // Ghidra 0x004381d4..0x004381fe: a player shot that destroys the
        // stellar pulses the stellar government's kill event ten times.
        for (int pulse = 0; pulse < 10; ++pulse) {
          NovaGovernment_ProcessFactionCombatEvent(
              state,
              state.player.current_system_id,
              stellar->government_id,
              3,
              -1);
        }
      }
    }
    return true;
  }
  return false;
}

// Ghidra Shot_ResolveCollisions (0x00437e20): the blast-proximity pass. It
// runs after the direct-contact pass, so a shot that already connected skips.
void NovaWeapon_ResolveProjectileCollisions(GameState &state) {
  // The original reads the ambient sprites prepared by the sprite tick; the
  // clean-room resolves the same current-frame masks here so the stellar arm
  // works even when this pass runs without the direct pass (tests).
  RefreshCollisionMasks(state);
  // Process only the shots that exist when the pass begins. The original's
  // fixed 0x80-slot pool could revisit a submunition appended into a free slot
  // ahead of the cursor; this compacted vector always appends at the end, so
  // new children first act on the next frame.
  const std::size_t proximity_shot_count = state.active_shots.size();
  for (std::size_t shot_index = 0; shot_index < proximity_shot_count;
       ++shot_index) {
    ActiveShot &shot = state.active_shots[shot_index];
    if (shot.life_ticks_remaining <= 0.0F && shot.life_frames > 0) {
      shot.life_ticks_remaining = static_cast<float>(shot.life_frames);
    }
    if (shot.consumed || shot.life_ticks_remaining <= 0.0F) {
      continue;
    }
    if (shot.system_id != state.player.current_system_id) {
      // Shot_HandleShot 0x00435830 rejects shots whose system no longer
      // matches the player before doing any movement or impact work.
      shot.consumed = true;
      continue;
    }
    const Weapon *weapon = WeaponForShot(state, shot);
    if (weapon == nullptr) {
      shot.consumed = true;
      continue;
    }
    if (ShotIsInLateCollisionWindow(shot, *weapon)) {
      continue;
    }

    // Planet-type weapon arm (flags_secondary 0x400): test the shot against
    // the current system's stellar sprites before the blast/ship/asteroid
    // proximity arms. The original re-checks `life >= 0` before those arms, so
    // a stellar contact consumes the shot and skips them.
    if ((weapon->flags_secondary & 0x0400U) != 0U) {
      (void)ResolveShotStellarContact(state, shot, *weapon);
    }
    if (!shot.consumed && shot.life_ticks_remaining >= 0.0F &&
        weapon->blast_radius > 0) {
      bool ship_hit = false;
      for (std::int16_t slot = 0;
           slot < static_cast<std::int16_t>(GameState::kMaxShips);
           ++slot) {
        if (!NovaWeapon_CanProjectileHitShip(state, shot, slot)) {
          continue;
        }
        const Ship &target = state.ShipAt(static_cast<std::size_t>(slot));
        // Ghidra 0x00437e20: radius = trunc(blast_radius + ship half-span *
        // 0.333) (DAT_00575338); positions and distances are compared in
        // integer space. The clean-room's collision_radius_px stands in for
        // the sprite half-span.
        const auto radius = static_cast<float>(
            static_cast<int>(static_cast<float>(weapon->blast_radius) +
                             std::max(0.0F, target.collision_radius_px) *
                                 kProximitySpanFraction));
        const auto dx = static_cast<int>(target.pos_x - shot.pos_x);
        const auto dy = static_cast<int>(target.pos_y - shot.pos_y);
        if (dx * dx + dy * dy <=
            static_cast<int>(radius) * static_cast<int>(radius)) {
          ResolveShotCollisionHit(state,
                                  shot_index,
                                  state.ShipAt(static_cast<std::size_t>(slot)),
                                  slot,
                                  /*allow_linked_shots=*/true);
          ship_hit = true;
          break;
        }
      }
      // Asteroid branch: when no ship was hit and flags_quaternary bit 0 is
      // clear, the 16 asteroid records are scanned (integer dist^2 <=
      // blast_radius^2, trunc-toward-zero positions) and the first contact
      // runs NovaUi_ResolveWeaponSplashImpact.
      if (!ship_hit && (weapon->flags_quaternary & 0x0001U) == 0U) {
        const auto blast = static_cast<int>(weapon->blast_radius);
        for (AsteroidState &asteroid : state.asteroid_pool) {
          if (!asteroid.active) {
            continue;
          }
          const auto dx =
              static_cast<int>(std::abs(asteroid.target_pos_x - shot.pos_x));
          const auto dy =
              static_cast<int>(std::abs(asteroid.target_pos_y - shot.pos_y));
          if (dx * dx + dy * dy <= blast * blast) {
            ResolveAsteroidSplashImpact(state, shot, asteroid);
            break;
          }
        }
      }
    }
  }
  RemoveConsumedShots(state);
}

void NovaWeapon_ResolveDirectWeaponHit(GameState &state,
                                       std::int16_t owner_ship_slot,
                                       std::int16_t target_ship_slot,
                                       std::int16_t weapon_id,
                                       std::int8_t impact_variant) {
  if (!ValidShipSlot(owner_ship_slot) || !ValidShipSlot(target_ship_slot) ||
      owner_ship_slot == target_ship_slot || weapon_id < 0 ||
      weapon_id >= 0x100) {
    return;
  }
  Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_ship_slot));
  Ship &target = state.ShipAt(static_cast<std::size_t>(target_ship_slot));
  if (!owner.is_active || !target.is_active ||
      owner.current_system_id != target.current_system_id ||
      owner.current_system_id != state.player.current_system_id) {
    return;
  }
  // Ghidra 0x0042f270 Shot_UpdateBeamHitQueue rejects a queued beam's
  // non-player target when it is in the scripted/invulnerable manoeuvre.
  if (target_ship_slot > 0 && target.ai_state_code == 0x10) {
    return;
  }
  ActiveShot shot;
  shot.weapon_id = weapon_id;
  shot.owner_ship_slot = owner_ship_slot;
  shot.target_ship_slot = target_ship_slot;
  shot.system_id = owner.current_system_id;
  shot.pos_x = owner.pos_x;
  shot.pos_y = owner.pos_y;
  shot.impact_variant = impact_variant;
  const Weapon *weapon = WeaponForShot(state, shot);
  if (weapon != nullptr) {
    // Beam expiry routes through the same primary impact visual/audio helper
    // as projectile contact (Ghidra 0x0042f270 -> 0x00437780).
    NovaEffects_SpawnAreaImpact(state,
                                shot.pos_x,
                                shot.pos_y,
                                weapon->impact_effect_id,
                                weapon->splash_radius,
                                true);
    // Beam hits use scatter 0x19 (Shot_UpdateBeamHitQueue 0x0042f270); the
    // projectile contact paths use 0x14.
    NovaEffects_SpawnWeaponImpactBurstForWeapon(
        state, shot.pos_x, shot.pos_y, *weapon, /*scatter=*/0x19);
    NovaEffects_SpawnImpactEffectPackage(
        state, shot.pos_x, shot.pos_y, shot.impact_package_id, false);
    ResolveShipHitFromWeapon(state,
                             target_ship_slot,
                             target,
                             shot.pos_x,
                             shot.pos_y,
                             weapon->impact_impulse,
                             weapon->mass_damage,
                             weapon->energy_damage,
                             owner_ship_slot,
                             /*allow_aggro_updates=*/true,
                             /*suppress_retarget_logic=*/false,
                             /*force_armor_only=*/impact_variant != 0,
                             /*bypass_shields=*/
                             (weapon->flags & 0x0020U) != 0U,
                             static_cast<std::int16_t>(std::lround(
                                 static_cast<float>(weapon->reload_ticks))));
  }
}

} // namespace game
