#include "collision.hpp"

#include "asteroid.hpp"
#include "compatibility.hpp"
#include "freeflight_objects.hpp"
#include "game_state.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "impact_effects.hpp"
#include "landed_store.hpp"
#include "mission.hpp"
#include "mission_script.hpp"
#include "nova_math.hpp"
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
constexpr float kPlayerAggroPerHitScale = 1.75F;       // DAT_00575240
constexpr float kPlayerAggroRetargetThreshold = 50.0F; // DAT_0057522c
constexpr float kEscortDefenseRadiusPx = 320.0F;       // DAT_00575248
// Ship_ApplyDamageToShip aggro extras.
constexpr int kHeadingLockSuppressDeg = 0x2d; // < 45 deg to desired heading
constexpr float kHeadingLockDistanceDivisor = 4.0F; // DAT_00575220: 4.0
constexpr float kStellarRedirectDistanceDivisor = 2.0F;
constexpr std::int16_t kStellarRedirectHostilityScale = 0x1e; // x30
constexpr float kCloakFadeFull =
    32.0F; // DAT_00575228 (also shield-bubble flash)
constexpr float kProximitySpanFraction =
    0.333005F; // DAT_00575338: blast + ship half-span * ~1/3
constexpr std::int16_t kShipClassInvalidSentinel = 0x2ff;
// Clean-room circle radius for a freeflight resource-box when no sprite mask
// is available (the original scoop arm always uses the opaque-pixel test).
// Only used when the spin set cannot be decoded (e.g. archive-free tests).
constexpr int kFreeflightScoopCircleRadiusPx = 16;
// Disable-transition armor pin (Ship_ApplyDamageToShip 0x0041a4b0):
// _DAT_00575238 = 0.3333 (NOT 1/3; bytes at 0x00575238) and _DAT_00575208 = 0.1
// for capability-flags 0x10 hulls, both +1.0 (_DAT_00575230). The disable
// threshold is max*0.33333, so the 0.3333 pin lands just above it and a pinned
// hull is no longer armor-disabled (Ship_IsShipDisabled then skips the
// disable-side arms).
constexpr float kDisableArmorPinFraction = 0.3333F;
constexpr float kDisableArmorPinFractionCap0x10 = 0.1F;

// Asteroid debris particle constants from Asteroid_SpawnDestructionPackage
// (0x00462550): speed DAT_00575740 = 0.2 px/tick, speed scatter 0x28, lifetime
// [0xf0, 0x1e0], and position scatter = sprite frame width / 3. Every shipped
// asteroid spin set (800..815) is 50x50, so the original's
// Sprite_GetFrameFullWidth(*asteroid)/3 reduces to 16
// (AsteroidState.collision_radius_px records the 25 px half-span).
constexpr float kAsteroidDebrisParticleSpeed = 0.2F;
constexpr std::int16_t kAsteroidDebrisParticleScatter = 0x28;
constexpr std::int16_t kAsteroidDebrisLifeBase = 0xf0;
constexpr std::int16_t kAsteroidDebrisLifeMax = 0x1e0;
constexpr std::int16_t kAsteroidDebrisPositionScatter = 16;

// @port 0x004115c0 100%
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

// Ghidra Ship_ApplyDamageToShip player arms: quick-fail the first
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
    Mission_FailMissionSlotQuick(
        state,
        static_cast<std::int16_t>(slot),
        static_cast<std::uint32_t>(state.gameplay_now_ms));
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

// Ghidra 0x004688e0 Ship_IsShipDestroyed is game::IsShipDestroyed
// (game_state.hpp).

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

// @port 0x0046E210 95% correctness
// Ghidra Stellar_ShipImmuneToStellarCrash (0x0046e210). A ship survives flying
// into a fatal stellar (availability_flags 0x100) when its class carries ship
// Flags3 0x20, or -- for the player only -- when the player owns
// any outfit whose mod slots include ModType 0x2a. Deliberately distinct from
// Stellar_ShipHasGravityShielding (0x0046e120): the NPC flags_secondary 0x40
// (inertialess) gate and the ModType 0x26 (inertial dampener)/0x29 (gravity
// resistance) outfits do NOT grant crash immunity. The original caches the
// player-outfit roll in DAT_007356c6; the
// clean-room recomputes it (identical while the inventory is unchanged).
[[nodiscard]] bool Stellar_ShipImmuneToStellarCrash(const GameState &state,
                                                    const Ship &ship) {
  const ShipClass *ship_class = ShipClassFor(state, ship);
  if (ship_class != nullptr && (ship_class->flags3 & 0x20U) != 0U) {
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

// @port 0x0046D190 80% gameplay
// Ghidra 0x0046d190 Ship_ShipsShareSquadRoot. Walks each ship's
// squad_leader_ship_slot to a root and returns true when the roots match. The
// walk is bounded to tolerate malformed cycles (the original is not; see the
// function's plate comment). NovaShip_ShipsShareSquadRoot exposes it.
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

[[nodiscard]] bool ShotIsWithinProximitySafetyDelay(const ActiveShot &shot,
                                                    const Weapon &weapon) {
  if (weapon.proximity_safety_ticks <= 0) {
    return false;
  }
  const float armed_lifetime =
      static_cast<float>(weapon.lifetime_ticks - weapon.proximity_safety_ticks);
  // Both Ship_HandleSpritePairCollision and Shot_ResolveCollisions reject
  // contacts while remaining life is above this boundary: ProxSafety is an
  // initial post-launch delay, not a late-life collision window.
  return armed_lifetime < shot.life_ticks_remaining;
}

// @port 0x0046f3f0 65% gameplay
// TODO(decomp): the gameplay-surface==8 overwrite branch of field_0xb0.
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

// The original pre-subtracts the sprite half-span from the world position
// before Sprite_SetPositionFromCurrentFrameAnchor, whose stored frame anchor is
// (0,0) for the multi-frame ship/asteroid sheets
// (SpriteFrame_CreateFromRect 0x00476400 zeroes +0x2a/+0x2c). The effective
// collision-frame top-left is therefore world - (half_x, half_y). Ship,
// asteroid, freeflight-object and stellar callers subtract the current frame's
// FULL WIDTH/2 (Sprite_GetFrameFullWidth 0x00462390) from x and FULL HEIGHT/2
// (Sprite_GetFrameFullHeight 0x004623D0) from y (Ship_UpdateVisualState
// 0x00428340, Asteroid_UpdateSprites 0x00436910); Shot_HandleShot (0x00435830)
// subtracts the full WIDTH/2 on BOTH axes. Square frames reduce to the frame
// centre. `width_on_both_axes` selects the ship/asteroid/etc (false) or shot
// (true) placement.
void BindEntityMask(CollisionMaskBinding &binding,
                    const SpriteMask *mask,
                    int frame,
                    bool width_on_both_axes) {
  if (mask == nullptr || mask->Empty()) {
    return;
  }
  const float half_width = static_cast<float>((mask->width + 1) / 2);
  const float half_height = static_cast<float>((mask->height + 1) / 2);
  binding.mask = mask;
  binding.anchor_x = half_width;
  binding.anchor_y = width_on_both_axes ? half_width : half_height;
  binding.frame = frame;
}

// @port 0x00462390 45% rendering
// @port 0x004623D0 40% rendering
// k_pixel_collision_frame_scale_threshold_f64 (0x005754c8) and the
// Sprite_GetFrameFullWidth (0x00462390) boundary.
// Ship_HandleSpritePairCollision (0x004374f0) uses the opaque mask only when
// `g_avg_frame_tick_scale < 2.0` AND the target ship sprite's full frame WIDTH
// is > 0x20; otherwise it deliberately uses the bounding circle.
// `g_avg_frame_tick_scale` is the normalized 30 Hz simulation scale; the port's
// equivalent is GameState::last_frame_tick_scale.
constexpr float kPixelMaskFrameScaleThreshold = 2.0F; // 0x005754c8
constexpr int kPixelMaskFrameWidthThreshold = 0x20;

[[nodiscard]] bool ShipContactUsesPixelMask(const GameState &state,
                                            const Ship &target) {
  if (!(state.last_frame_tick_scale < kPixelMaskFrameScaleThreshold)) {
    return false;
  }
  if (!target.collision_mask.HasMask()) {
    return false;
  }
  return target.collision_mask.mask->width > kPixelMaskFrameWidthThreshold;
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
        FrameForHeading(ship.heading, ship_class->frames_per_rotation);
    BindEntityMask(ship.collision_mask,
                   store.Sheet(ship_class->base_image_id, frame),
                   frame,
                   /*width_on_both_axes=*/false);
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
      frame = FrameForHeading(bearing, frame_count);
    } else {
      frame = std::clamp(shot.frame_cycle_index, 0, frame_count - 1);
    }
    BindEntityMask(shot.collision_mask,
                   store.Spin(spin_id, frame),
                   frame,
                   /*width_on_both_axes=*/true);
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
                   /*width_on_both_axes=*/false);
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
                   /*width_on_both_axes=*/false);
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
                     /*width_on_both_axes=*/false);
    }
  }
}

// Ghidra Math_AddPolarVelocityWithClamp (0x0043b4e0): add impact/mass in the
// impact-to-target direction, per-axis clamped to the class base speed, then
// hard-clamp the components against the effective max speed (widened 1.8x for
// the afterburning player outside a stellar gravity pull). Ship-class Speed is
// stored in hundredths of px/frame. The bearing is quantized to whole game
// degrees as the original's short argument does; the sine/cosine uses libm
// rather than the original's integer lookup table, matching the established
// Math_AddPolarVelocityWithClamp port used by the movement integrators.
// impact_pos NULL: the only original NULL caller passes impact_impulse 0
// (Ship_HandlePlayerShipCore's self-targeted 32000/32000 kill), so the block is
// inert; the port always has coordinates.
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

  // Math_BearingFromPointToPoint(impact_pos, target): push runs impact->target.
  // Math_AngleFromVector2D returns 0 for an exactly-zero vector, so a
  // coincident impact point pushes along bearing 0 rather than skipping.
  const float dx = target.pos_x - impact_x;
  const float dy = target.pos_y - impact_y;
  const int bearing_deg =
      (dx == 0.0F && dy == 0.0F)
          ? 0
          : static_cast<int>(
                BearingDeg(impact_x, impact_y, target.pos_x, target.pos_y));
  constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
  const float step = static_cast<float>(impact_impulse) /
                     static_cast<float>(target_class->mass_tons);
  Math_AddPolarVelocityWithClamp(static_cast<float>(bearing_deg) * kDegToRad,
                                 step,
                                 target_class->speed / 100.0F,
                                 target.vel_x,
                                 target.vel_y);

  float max_speed =
      NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, target, *target_class);
  if (target.ship_instance_id == 0 && state.player_afterburner_active &&
      !state.gravity_pull_active) {
    max_speed *= 1.8F; // DAT_00575218
  }
  target.vel_x = std::clamp(target.vel_x, -max_speed, max_speed);
  target.vel_y = std::clamp(target.vel_y, -max_speed, max_speed);
}

// Ship_ApplyDamageToShip's leave-one-armor rule: a disable-variant hit
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

// @port 0x004102e0 100%
// Ghidra Government_PropagateHostilityFromAttack (0x004102e0). A player hit
// alerts eligible combat ships related to the victim; in particular, ships of
// the victim's government join the response and target the player. This keeps
// the original's full government, policy, personality, distress, derelict,
// nosy-victim, and squad-chain gate order.
void PropagateHostilityFromPlayerAttack(GameState &state,
                                        const Ship &target,
                                        std::int16_t owner_ship_slot) {
  if (owner_ship_slot != 0 || target.pers_def_slot == 0x3ff ||
      state.player.pers_def_slot == 0x3fe) {
    return;
  }
  if (HasGovernmentFlag(state, target, 0x0800U)) {
    return;
  }

  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &responder = state.ShipAt(slot);
    if (responder.ai_behavior_code < 3 || responder.ai_behavior_code > 4 ||
        responder.ai_state_code == 4 || responder.pers_def_slot == 0x3ff ||
        responder.faction_or_government_id < -1 ||
        responder.faction_or_government_id >= 0x100) {
      continue;
    }
    if (NovaAiShip_IsEnemyOfShip(state, responder, target)) {
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
    if (HasGovernmentFlag(state, target, 0x0020U)) {
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

} // namespace

// Public wrapper for the file-local Government_PropagateHostilityFromAttack
// port above (canonical citation at its primary site), so the
// Player_HandleBoardTargetCommand plunder fallback (0x0045a9f3 / 0x0045ab0f)
// reuses the same responder scan.
void NovaGovernment_PropagateHostilityFromAttack(
    GameState &state,
    const Ship &target_ship,
    std::int16_t attacker_ship_slot) {
  PropagateHostilityFromPlayerAttack(state, target_ship, attacker_ship_slot);
}

// Public checkpoint to the file-local SquadRoot walk above (see its canonical
// citation) so Shot_HandleShot's player target-chain arm reuses the exact same
// chain semantics as Weapon_CanWeaponHitTarget.
bool NovaShip_ShipsShareSquadRoot(const GameState &state,
                                  std::int16_t first_slot,
                                  std::int16_t second_slot) {
  return SharesSquadRoot(state, first_slot, second_slot);
}

// Split expiry-impact helper for Shot_HandleShot (canonical citation at the
// primary site NovaWeapon_TickShots, src/game/weapon_shots.cpp).
void NovaWeapon_ResolveShotExpiryImpact(GameState &state,
                                        const ActiveShot &shot,
                                        const Weapon &weapon) {
  if ((weapon.flags & 0x8000U) == 0U) {
    // Quirk preserved: the original tests impact_effect_id > 0 but always
    // passes effect id 0 to Shot_SpawnImpactEffectSprite(x, y, 0, 0, 0).
    if (weapon.impact_effect_id > 0) {
      NovaEffects_SpawnImpactEffect(state, shot.pos_x, shot.pos_y, 0, 0);
    }
    return;
  }

  NovaEffects_SpawnAreaImpact(state,
                              shot.pos_x,
                              shot.pos_y,
                              weapon.impact_effect_id,
                              weapon.splash_radius,
                              /*play_sound=*/true);
  if (weapon.splash_radius <= 0 || (weapon.flags_secondary & 0x0400U) != 0U) {
    return;
  }

  // x87 FIST + residual/sign truncation of Reload toward zero, matching the
  // other damage callsites.
  const auto player_aggro_delta =
      static_cast<std::int16_t>(static_cast<std::int32_t>(weapon.reload_ticks));
  for (std::int16_t slot = 0;
       slot < static_cast<std::int16_t>(GameState::kMaxShips);
       ++slot) {
    // NPC owners never splash themselves; the player owner is skipped (immune)
    // only when flags_primary 0x100 is set.
    if (slot == shot.owner_ship_slot &&
        ((weapon.flags & 0x0100U) != 0U || shot.owner_ship_slot != 0)) {
      continue;
    }
    Ship &target = state.ShipAt(static_cast<std::size_t>(slot));
    if (!target.is_active || target.ship_class_id < 0 ||
        target.ship_class_id == kShipClassInvalidSentinel) {
      continue;
    }
    const ShipClass *cls = ShipClassFor(state, target);
    if (cls != nullptr && (cls->capability_flags & 0x0400U) != 0U) {
      continue;
    }
    if (std::abs(target.pos_x - shot.pos_x) >
            static_cast<float>(weapon.splash_radius) ||
        std::abs(target.pos_y - shot.pos_y) >
            static_cast<float>(weapon.splash_radius)) {
      continue;
    }
    Ship_ApplyDamageToShip(state,
                           slot,
                           target,
                           shot.pos_x,
                           shot.pos_y,
                           weapon.impact_impulse,
                           weapon.mass_damage,
                           weapon.energy_damage,
                           shot.owner_ship_slot,
                           /*allow_aggro_updates=*/false,
                           /*suppress_retarget_logic=*/false,
                           /*force_armor_only=*/shot.impact_variant != 0,
                           /*bypass_shields=*/(weapon.flags & 0x0020U) != 0U,
                           player_aggro_delta,
                           /*check_fire_restriction_transition=*/false);
    ApplyWeaponOnHitEffects(
        target, weapon, std::make_pair(shot.pos_x, shot.pos_y));
  }
}

// @port 0x0046f1e0 100%
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

// @port 0x004192d0 100% license
// Ghidra Ship_ApplyDamageToShip (0x004192d0). Core ship-hit
// resolution: impulse, shield-first/armor damage, disable-variant armor
// clamping, and the aggro/hostility response (eligibility chain, retaliation,
// cloak re-entry/deactivation, and the defense-fleet stellar redirect).
//
// TODO(decomp(0x004192d0)) deliberately skipped: the pers-attacker damage
// multiplier block. Attackers with pers_def_slot 0x3ff (Shareware Enforcer
// template ships) multiply armor+shield damage by the shareware trial day
// counter g_shareware_day_counter (0x0059799e, NovaTime_GetSharewareDayCounter
// 0x004d4480): no boost for days 0-40, x2 for 41-60, x3 for 61-90, and x5 for
// 91+. The clean-room models a registered game and does not run the trial-day
// counter, so the block is out of scope. (This is a trial-system divergence,
// not an unported branch of normal combat.)
// check_fire_restriction_transition is 0 from the shot paths and 1 from the
// hull-destruction blast (Ship_UpdateVisualState 0x00428340), which arms the
// disable-transition armor pin (33%/10% + 1 armor) and the mission DISABLE
// bookkeeping (escort-goal quick-fail + goal_counter_c++, STR# 0x7d2 0x11c)
// and the player "disabled" overlay (STR# 0x7d2 0x11f).
void Ship_ApplyDamageToShip(GameState &state,
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
      target.ship_class_id < 0 ||
      target.ship_class_id > 0x2fe || // Ghidra 0x2ff sentinel bound
      target.ship_instance_id < 0 ||
      target.ship_instance_id >=
          static_cast<std::int16_t>(GameState::kMaxShips)) {
    return;
  }
  const bool attacker_valid = ValidShipSlot(attacker_ship_slot);
  // Ghidra local bVar4 at 0x004196b1: faction events and combat-rating credit
  // belong only to the player or a direct player escort. NPC-on-NPC combat is
  // deliberately invisible to the player's record.
  const bool player_involved =
      attacker_ship_slot == 0 ||
      (attacker_ship_slot > 0 && attacker_valid &&
       state.ShipAt(static_cast<std::size_t>(attacker_ship_slot))
               .squad_leader_ship_slot == 0);

  // @port 0x00415e80 100%
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

  const bool was_destroyed = IsShipDestroyed(target);
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
      // The floor is -10% of the effective max shield (Ship_ComputeShipMax-
      // ShieldPoints): the cached player snapshot includes outfit bonuses and
      // the NPC fallback folds in the personality shield_armor_scale and the
      // behavior-5 difficulty scale.
      const float max_shield =
          target_slot == 0 && state.stat_cache_valid
              ? state.cached_stats.max_shield_points
              : static_cast<float>(
                    NovaAi_ComputeMaxShieldPoints(state, target));
      const float shield_floor = -kShieldDepletionFloorFraction * max_shield;
      if (target.shield_points < shield_floor) {
        target.shield_points = shield_floor;
      }
    }
  } else {
    ApplyArmorDamage(target, armor_damage, force_armor_only);
  }

  // ---- Disable-transition armor pin (Ghidra 0x0041a4b0) -------------------
  // Runs BEFORE the kill-side faction/comparison arms below. A
  // hull-destruction blast (check_fire_restriction_transition) that drops armor
  // to zero/destroyed gets pinned back to 0.3333*max+1 (10%+1 for
  // capability-flags 0x10 hulls), so the blast registers no kill and no combat
  // rating. The pin fraction sits just below the max*0.33333 armor-disable
  // threshold, so the +1 lands above it and the hull is no longer
  // armor-disabled either, skipping the disable-side arms (quirk preserved).
  if (check_fire_restriction_transition && !was_fire_restricted &&
      NovaAiShip_IsDisabled(state, target) && target_class != nullptr) {
    const float max_armor = target_slot == 0 && state.stat_cache_valid
                                ? state.cached_stats.max_armor_points
                                : NovaAi_ComputeMaxArmorPoints(state, target);
    target.armor_points =
        (target_class->capability_flags & 0x10) != 0U
            ? max_armor * kDisableArmorPinFractionCap0x10 + 1.0F
            : max_armor * kDisableArmorPinFraction + 1.0F;
  }

  // Disable-side faction event (Ghidra 0x00419721): event 1 pulses on the
  // transition into the disabled state and runs BEFORE the kill-side event 3 /
  // combat rating below (the original runs both in one player-involved block,
  // disable first). Same pers_def_slot < 0x3ff gate.
  if (player_involved && target.defense_fleet_home_stellar_id == -1 &&
      !was_fire_restricted && NovaAiShip_IsDisabled(state, target) &&
      target.pers_def_slot < 0x3ff) {
    NovaGovernment_ProcessFactionCombatEvent(state,
                                             state.player.current_system_id,
                                             target.faction_or_government_id,
                                             1,
                                             target.mission_fleet_slot);
  }

  // Ship_ApplyDamageToShip leaves destruction as an armor-state: the
  // hit site only records the alive -> destroyed transition and clears the
  // slot's targeting references. Ship_HandleShip (0x00433050) seeds and runs
  // the NPC death presentation, and Ship_UpdateVisualState (0x00428340) seeds
  // the timer for NPCs and the player (DeathDelay, x3 for the player via
  // g_player_death_timer_scale 0x00575378), drives the Explode1 debris cadence
  // and spawns the Explode2 finale. No explosion or timer is spawned here.
  if (!was_destroyed && IsShipDestroyed(target) &&
      !target.destruction_visual_triggered) {
    target.destruction_visual_triggered = true;
    NovaTargeting_ClearDestroyedShipReferences(state, target_slot);
    // Kill-side faction event + combat rating (Ghidra 0x00419748): only a
    // player/player-escort kill of a non-defense-fleet ship affects the
    // player's record. Shareware Enforcer personalities and derelict
    // governments are excluded too.
    if (player_involved && target.defense_fleet_home_stellar_id == -1 &&
        target.pers_def_slot < 0x3ff &&
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
    // Disable-side faction event fired above, before the kill-side arms.
    // Mission DISABLE bookkeeping: counts one disable per mission ship on the
    // transition into disable restriction; an escort goal (Bible ShipGoal 3)
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
            mission.ship_goal == 3 && (mission.flags_primary & 0x0400U) == 0U) {
          state.pending_ui_sounds.push_back(GameState::PendingUiSound{1, 1});
          if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x11c)) {
            NovaHud_ShowOverlayMessage(
                state, *text, /*duration_frames=*/std::uint64_t{0xf0});
          }
          Mission_FailMissionSlotQuick(
              state,
              fleet_slot,
              static_cast<std::uint32_t>(state.gameplay_now_ms));
        }
        mission.goal_counter_c =
            static_cast<std::int16_t>(mission.goal_counter_c + 1);
      }
    }
    // Fleet-recovery hint for surrendered escorts (squad_leader_ship_slot 0,
    // not a mission ship): stores how the boarding/escort-conversion flow
    // treats this hull (the behavior-6 escort arms a cargo transfer below).
    if (target.squad_leader_ship_slot == 0 && fleet_slot == -1) {
      target.fleet_recovery_hint =
          target.ai_behavior_code == 5
              ? 0
              : (target.escort_origin_mark == 0 ? 2 : 1);
      target.squad_leader_ship_slot = -1;
      if (target.ai_behavior_code == 6 && target_class != nullptr &&
          target_class->default_ai_behavior < 3) {
        // A surrendering behavior-6 escort converts to a cargo hauler and takes
        // its share of the fleet cargo. The original also marks the inventory
        // dirty and redraws the cargo panel; the transfer calls
        // NovaOutfit_RecomputeOutfitDerivedState and the HUD redraws live.
        Player_TransferCargoAndJunkToEscortByRatio(state,
                                                   target.ship_instance_id);
      }
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
  if (target_slot == 0 && IsShipDestroyed(target) && !was_destroyed) {
    state.pending_ui_sounds.push_back(GameState::PendingUiSound{1, 1});
    if (allow_aggro_updates && ValidShipSlot(attacker_ship_slot) &&
        attacker_ship_slot > 0 &&
        state.ShipAt(static_cast<std::size_t>(attacker_ship_slot))
                .pers_def_slot == 0x3ff) {
      // STR# 30000 entries 8..13: Shareware Enforcer taunts. Taking this branch
      // latches the disable/destroy overlay so the generic 0x120 message is
      // suppressed even if the taunt resource cannot be loaded.
      std::uniform_int_distribution<std::int32_t> taunt_roll{0, 5};
      if (auto text = NovaHud_LoadStringEntry(
              30000, static_cast<std::uint16_t>(8 + taunt_roll(state.rng)))) {
        NovaHud_ShowOverlayMessage(
            state, *text, /*duration_frames=*/std::uint64_t{5000});
      }
      state.player_disable_message_shown = true;
    }
    if (!state.player_disable_message_shown) {
      if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x120)) {
        NovaHud_ShowOverlayMessage(
            state, *text, /*duration_frames=*/std::uint64_t{0xf0});
      }
    }
    QuickFailPlayerDependencyMissions(state);
  }

  if (allow_aggro_updates && target.ai_behavior_code > 0 &&
      target.ai_station_hold_timer <= 0.0F) {
    const auto squared_distance = [](float x1, float y1, float x2, float y2) {
      const float dx = x2 - x1;
      const float dy = y2 - y1;
      return dx * dx + dy * dy;
    };

    // Faithful port of the original `bVar4` eligibility computation. The
    // low-byte flag `local_1c._0_1_` is set only for a self-hit where the
    // target (or attacker) belongs to a defense fleet; that case takes the
    // short secondary branch instead of the full government/squad logic.
    bool should_retarget = false;
    bool self_hit_defense_fleet = false;
    if (attacker_ship_slot == target_slot) {
      if (target.defense_fleet_home_stellar_id != -1 ||
          (attacker_valid &&
           state.ShipAt(static_cast<std::size_t>(attacker_ship_slot))
                   .defense_fleet_home_stellar_id != -1)) {
        should_retarget = true;
        self_hit_defense_fleet = true;
      }
    } else {
      should_retarget = true;
    }

    if (should_retarget && allow_aggro_updates && target.ai_behavior_code > 0 &&
        target.ai_station_hold_timer <= 0.0F) {
      should_retarget = false;
      const Ship *attacker =
          attacker_valid
              ? &state.ShipAt(static_cast<std::size_t>(attacker_ship_slot))
              : nullptr;
      if (!self_hit_defense_fleet) {
        // (A) different governments are potential enemies.
        if (attacker != nullptr && target.faction_or_government_id !=
                                       attacker->faction_or_government_id) {
          should_retarget = true;
        }
        // (B) squad-leader relations.
        const std::int16_t target_leader = target.squad_leader_ship_slot;
        if (target_leader >= 0 &&
            target_leader < static_cast<std::int16_t>(GameState::kMaxShips)) {
          if (attacker_ship_slot != target_leader) {
            should_retarget = true;
          }
          if (attacker != nullptr && attacker->squad_leader_ship_slot != -1 &&
              target_leader != attacker->squad_leader_ship_slot) {
            should_retarget = true;
          }
        }
        // (C) government/crime-tolerance and same-government rules.
        const std::int16_t target_nation = target.faction_or_government_id;
        const Government *target_govt =
            target_nation >= 0 && target_nation < 0x100
                ? state.scenario.GovernmentByIndex(target_nation)
                : nullptr;
        if (target_nation < 0 || target_nation > 0xff) {
          if (attacker != nullptr && (attacker_ship_slot == 0 ||
                                      attacker->squad_leader_ship_slot == 0)) {
            should_retarget = true;
          }
        } else {
          const System *system = state.scenario.System(
              static_cast<std::int16_t>(state.player.current_system_id + 0x80));
          const std::int16_t system_govt =
              system != nullptr ? system->government_id : -1;
          const std::int16_t reputation =
              state.player.current_system_id >= 0 &&
                      static_cast<std::size_t>(state.player.current_system_id) <
                          state.system_reputation.size()
                  ? state.system_reputation[static_cast<std::size_t>(
                        state.player.current_system_id)]
                  : 0;
          const int crime_tol =
              target_govt != nullptr ? target_govt->crime_tol : 0;
          // A player (or direct escort) hit in a system where the player's
          // reputation is already criminal does not start a fight.
          if (system_govt >= 0 && system_govt < 0x100 &&
              state.player.primary_target_ship_slot != target_slot &&
              static_cast<int>(reputation) - 2 * crime_tol < 0 &&
              (attacker_ship_slot == 0 ||
               (attacker != nullptr &&
                attacker->squad_leader_ship_slot == 0))) {
            should_retarget = false;
          }
          if (attacker != nullptr &&
              target_nation == attacker->faction_or_government_id) {
            should_retarget = false;
          }
        }
        // (D) target and attacker share the same defense-fleet leader.
        if (attacker != nullptr && target_leader >= 0 &&
            target_leader < static_cast<std::int16_t>(GameState::kMaxShips) &&
            target_leader == attacker->squad_leader_ship_slot &&
            attacker->defense_fleet_home_stellar_id != -1) {
          should_retarget = false;
        }
        // (E) the hit must carry positive damage. The original's -32000
        // overflow guard is preserved verbatim.
        if (should_retarget && shield_damage < 1 && armor_damage < 1 &&
            shield_damage > -32000 && armor_damage > -32000) {
          should_retarget = false;
        }
        // (F) incidental-fire gates, all skipped when the shot was targeted.
        if (!suppress_retarget_logic && should_retarget) {
          if (attacker_ship_slot == 0) {
            const std::int16_t player_target =
                state.player.primary_target_ship_slot;
            if (player_target >= 0 &&
                player_target <
                    static_cast<std::int16_t>(GameState::kMaxShips)) {
              if (target_slot == player_target) {
                should_retarget = true;
              } else if (NovaAiShip_IsEnemyOfShip(
                             state,
                             target,
                             state.ShipAt(
                                 static_cast<std::size_t>(player_target)))) {
                should_retarget = false;
              }
            }
          } else if (attacker != nullptr &&
                     target_slot != attacker->primary_target_ship_slot) {
            should_retarget = false;
          }
          if (should_retarget && target_nation >= 0 && target_nation < 0x100 &&
              attacker_ship_slot == 0 && target_govt != nullptr &&
              target_govt->shoot_penalty < 1) {
            std::uniform_int_distribution<std::int32_t> roll{0, 3};
            if (roll(state.rng) != 0) {
              should_retarget = false;
            }
          }
          // Incidental player fire must cross the aggro threshold; a targeted
          // shot (suppress_retarget_logic) bypasses this.
          if (attacker_ship_slot == 0 &&
              target.player_aggro_accumulator < kPlayerAggroRetargetThreshold) {
            should_retarget = false;
          }
        }
        // (G) accumulate the player-aggro pressure after the eligibility test,
        // so this hit cannot trigger its own retarget.
        if (attacker_ship_slot == 0 && !suppress_retarget_logic) {
          target.player_aggro_accumulator +=
              static_cast<float>(player_aggro_delta) * kPlayerAggroPerHitScale;
        }
        // (H) escort-defense and same-leader exclusions.
        if (attacker != nullptr) {
          if (attacker->squad_leader_ship_slot == 0 &&
              attacker->ai_behavior_code == 5 &&
              target.primary_target_ship_slot == 0 &&
              std::abs(target.pos_x - state.player.pos_x) <=
                  kEscortDefenseRadiusPx &&
              std::abs(target.pos_y - state.player.pos_y) <=
                  kEscortDefenseRadiusPx) {
            should_retarget = false;
          }
          if (target.squad_leader_ship_slot ==
                  attacker->squad_leader_ship_slot &&
              target.squad_leader_ship_slot != -1) {
            should_retarget = false;
          }
        }
        if (target.ai_station_hold_timer > 0.0F) {
          should_retarget = false;
        }
        if (!suppress_retarget_logic && should_retarget &&
            attacker != nullptr) {
          if (attacker->faction_or_government_id >= 0 &&
              attacker->faction_or_government_id < 0x100 &&
              NovaShip_IsInPlayerSquad(state, *attacker) &&
              NovaGovernment_GetPolicyFlag(
                  state.scenario, attacker->faction_or_government_id, 0)) {
            should_retarget = false;
          }
        }
        // Grandparent-of-leader equality is a further friendly-fire exclusion.
        std::int16_t target_grandparent = -1;
        std::int16_t attacker_grandparent = -1;
        if (target_leader >= 0 &&
            target_leader < static_cast<std::int16_t>(GameState::kMaxShips)) {
          const std::int16_t grandparent =
              state.ShipAt(static_cast<std::size_t>(target_leader))
                  .squad_leader_ship_slot;
          if (grandparent >= 0 &&
              grandparent < static_cast<std::int16_t>(GameState::kMaxShips)) {
            target_grandparent = grandparent;
          }
        }
        if (attacker_ship_slot > 0 &&
            attacker_ship_slot <
                static_cast<std::int16_t>(GameState::kMaxShips)) {
          const std::int16_t leader =
              state.ShipAt(static_cast<std::size_t>(attacker_ship_slot))
                  .squad_leader_ship_slot;
          if (leader > 0 &&
              leader < static_cast<std::int16_t>(GameState::kMaxShips)) {
            attacker_grandparent =
                state.ShipAt(static_cast<std::size_t>(leader))
                    .squad_leader_ship_slot;
          }
        }
        if (target_grandparent == attacker_grandparent &&
            target_grandparent >= 0 &&
            target_grandparent <
                static_cast<std::int16_t>(GameState::kMaxShips) &&
            attacker_grandparent >= 0 &&
            attacker_grandparent <
                static_cast<std::int16_t>(GameState::kMaxShips)) {
          should_retarget = false;
        }
      } else if (attacker == nullptr) {
        should_retarget = true;
      } else if (target.defense_fleet_home_stellar_id == -1 ||
                 attacker->defense_fleet_home_stellar_id == -1) {
        should_retarget = true;
      } else {
        should_retarget = false;
      }
      // Shared post-gates: target and leader hold timers, the 0x0F control
      // mode, and the class-category mismatch.
      if (target.ai_station_hold_timer > 0.0F) {
        should_retarget = false;
      } else {
        const std::int16_t leader = target.squad_leader_ship_slot;
        if (leader > 0 &&
            leader < static_cast<std::int16_t>(GameState::kMaxShips) &&
            state.ShipAt(static_cast<std::size_t>(leader))
                    .ai_station_hold_timer > 0.0F) {
          should_retarget = false;
        }
      }
      // @port 0x00412530 100%
      // Ghidra 0x00412530 Ship_IsShipInAiControlMode0x0F runs inline here.
      if (should_retarget && target.ship_instance_id > 0 &&
          attacker_ship_slot > 0 && attacker_valid &&
          target.ai_control_mode == 0x0F) {
        should_retarget = false;
      }
      if (should_retarget && attacker != nullptr && target_class != nullptr) {
        const ShipClass *attacker_class = state.scenario.Ship(
            static_cast<std::int16_t>(attacker->ship_class_id + 0x80));
        if (attacker_class != nullptr && attacker_class->class_category == 0 &&
            target_class->class_category != 0 &&
            target.primary_target_ship_slot != -1 &&
            NovaAiShip_IsShipInAiState4(target)) {
          should_retarget = false;
        }
      }
    }

    if (should_retarget) {
      // The original clears the accumulated player-aggro pressure whenever the
      // victim decides to fight back.
      target.player_aggro_accumulator = 0.0F;
      // Cloak re-entry reset: a cloaked target that was not already locked on
      // its attacker re-arms its burst weapons and starts the cloak.
      if (target_class != nullptr &&
          (target_class->flags_secondary & 0x4000U) != 0U && attacker_valid &&
          NovaAiShip_CanMaintainCloakState(state, target) &&
          !NovaAiShip_IsShipLockedOnAttackerInState4(
              target,
              state.ShipAt(static_cast<std::size_t>(attacker_ship_slot)))) {
        NovaWeapon_InitShipWeaponBursts(state, target);
        NovaAi_OnShipCloakStateEntered(state, target);
      }

      bool retargeted = false;
      if (target.defense_fleet_home_stellar_id == -1) {
        retargeted = true;
        // Hold fire when the target is already lined up on a closer primary
        // target than the attacker.
        if (target.primary_target_ship_slot != -1 && attacker_valid &&
            NovaAiShip_IsShipInAiState4(target) &&
            ValidShipSlot(target.primary_target_ship_slot)) {
          const int heading_deg = static_cast<int>(
              WrapDeg(target.heading * (180.0F / 3.14159265358979323846F)));
          if (ShortestAngleDeltaDeg(heading_deg,
                                    target.ai_desired_heading_deg) <
              kHeadingLockSuppressDeg) {
            const auto primary =
                static_cast<std::size_t>(target.primary_target_ship_slot);
            const Ship &attacker_ship =
                state.ShipAt(static_cast<std::size_t>(attacker_ship_slot));
            const float primary_distance_sq =
                squared_distance(target.pos_x,
                                 target.pos_y,
                                 state.ShipAt(primary).pos_x,
                                 state.ShipAt(primary).pos_y);
            const float attacker_distance_sq =
                squared_distance(target.pos_x,
                                 target.pos_y,
                                 attacker_ship.pos_x,
                                 attacker_ship.pos_y);
            if (primary_distance_sq / kHeadingLockDistanceDivisor <
                attacker_distance_sq) {
              retargeted = false;
            }
          }
        }
        if (retargeted && target.ship_instance_id != 0) {
          if (armor_damage > 0) {
            // The original adds straight into the int16 field with no
            // saturating clamp (16-bit wrap on overflow); keep that quirk.
            target.ai_hostility_accumulator = static_cast<std::int16_t>(
                target.ai_hostility_accumulator + armor_damage);
          }
          if (shield_damage > 0) {
            target.ai_hostility_accumulator = static_cast<std::int16_t>(
                target.ai_hostility_accumulator + shield_damage);
          }
          if (attacker_valid) {
            target.primary_target_ship_slot = attacker_ship_slot;
          }
          const std::int16_t leader = target.squad_leader_ship_slot;
          if (leader > 0 &&
              leader < static_cast<std::int16_t>(GameState::kMaxShips) &&
              state.ShipAt(static_cast<std::size_t>(leader))
                      .defense_fleet_home_stellar_id == -1) {
            Ship &leader_ship = state.ShipAt(static_cast<std::size_t>(leader));
            if (armor_damage > 0) {
              leader_ship.ai_hostility_accumulator = static_cast<std::int16_t>(
                  leader_ship.ai_hostility_accumulator + armor_damage);
            }
            if (shield_damage > 0) {
              leader_ship.ai_hostility_accumulator = static_cast<std::int16_t>(
                  leader_ship.ai_hostility_accumulator + shield_damage);
            }
            if (attacker_valid) {
              leader_ship.primary_target_ship_slot = attacker_ship_slot;
            }
          }
        }
      } else if (attacker_valid) {
        // Defense-fleet ship away from home: an attacker closer than half the
        // squared distance to the defended stellar is treated as the threat,
        // with 30x hostility.
        Stellar *home =
            state.scenario.StellarMutable(target.defense_fleet_home_stellar_id);
        if (home != nullptr) {
          const Ship &attacker_ship =
              state.ShipAt(static_cast<std::size_t>(attacker_ship_slot));
          const float stellar_distance_sq =
              squared_distance(target.pos_x,
                               target.pos_y,
                               static_cast<float>(home->pos_x),
                               static_cast<float>(home->pos_y)) /
              kStellarRedirectDistanceDivisor;
          const float attacker_distance_sq =
              squared_distance(target.pos_x,
                               target.pos_y,
                               attacker_ship.pos_x,
                               attacker_ship.pos_y);
          if (attacker_distance_sq < stellar_distance_sq) {
            if (target.ship_instance_id != 0) {
              if (armor_damage > 0) {
                target.ai_hostility_accumulator = static_cast<std::int16_t>(
                    target.ai_hostility_accumulator +
                    armor_damage * kStellarRedirectHostilityScale);
              }
              if (shield_damage > 0) {
                target.ai_hostility_accumulator = static_cast<std::int16_t>(
                    target.ai_hostility_accumulator +
                    shield_damage * kStellarRedirectHostilityScale);
              }
              target.primary_target_ship_slot = attacker_ship_slot;
            }
            retargeted = true;
          }
        }
      }

      if (retargeted) {
        if (attacker_ship_slot == 0) {
          // Ship_SetShipHostileToPlayer (0x00410700) deliberately changes the
          // primary target/state only. squad_leader_ship_slot is the
          // squad-leader attachment link (not a combat target).
          NovaAi_SetShipHostileToPlayer(state, target);
        }
        if (target.ai_maneuver_timer_ms > kMaxManeuverTimerOnHit) {
          target.ai_maneuver_timer_ms = kMaxManeuverTimerOnHit;
        }
        if (attacker_ship_slot == 0 &&
            target.defense_fleet_home_stellar_id == -1) {
          if (target.pers_def_slot >= 0 &&
              static_cast<std::size_t>(target.pers_def_slot) <
                  state.scenario.pers_defs.size()) {
            PersDef &pers =
                state.scenario
                    .pers_defs[static_cast<std::size_t>(target.pers_def_slot)];
            if ((static_cast<std::uint16_t>(pers.flags_primary) & 0x0001U) !=
                0U) {
              pers.grudge = true;
            }
          }
          if (target.ship_instance_id != 0) {
            ClearState9OrFToIdle(target);
            target.primary_target_ship_slot = 0;
          }
        }
      }
      if (suppress_retarget_logic && attacker_ship_slot == 0) {
        PropagateHostilityFromPlayerAttack(state, target, attacker_ship_slot);
      }
    }
  }

  // Ship_ApplyDamageToShip starts the shield-bubble flash on every
  // non-bypass hit. Its Ship_UpdateVisualState decay/render branch is deferred.
  if (!bypass_shields) {
    target.shield_bubble_flash_intensity = 32.0F;
  }
  // g_player_status_panel_dirty is a render-cadence latch; the clean-room HUD
  // redraws the status panels from live state each frame, so it has no port.
  // A landed hit knocks the target out of cloak once the fade has completed (or
  // a fade transition is already running).
  if (allow_aggro_updates && target.cloak_damage_deactivate_latch == 1 &&
      (target.cloak_fade_progress == kCloakFadeFull ||
       target.cloak_transition_latch > 0)) {
    NovaAi_OnShipCloakStateCleared(state, target);
  }
}

// @port 0x00437780 90% gameplay
// Ghidra Shot_ResolveShotCollisionHit (0x00437780). Applies the primary area
// impact effect, the direct damage/impulse/ionization package, and the axis-
// aligned splash to every other eligible ship.
// TODO(decomp(0x00437780)): verify the remaining detail against Ghidra - the
// exact splash damage terms, the suppress-retarget condition (shot +0x28 vs
// the direct target slot), and the linked-shot gate/RNG order.
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

  int armor_damage = weapon->mass_damage;
  int shield_damage = weapon->energy_damage;
  if (shot.damage_decay_points > 0) {
    armor_damage = std::max(0, armor_damage - shot.damage_decay_points);
    shield_damage = std::max(0, shield_damage - shot.damage_decay_points);
  }

  // Direct-target ionization is unattenuated (impact_pos = NULL upstream).
  ApplyWeaponOnHitEffects(target, *weapon, std::nullopt);

  const bool suppress_retarget_logic = shot.target_ship_slot == target_slot;
  // The x87 FIST + residual/sign sequence truncates the reload toward zero.
  const auto player_aggro_delta = static_cast<std::int16_t>(
      static_cast<std::int32_t>(weapon->reload_ticks));
  const bool target_was_destroyed = IsShipDestroyed(target);

  Ship_ApplyDamageToShip(state,
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
      Ship_ApplyDamageToShip(state,
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
  if (!target_was_destroyed && IsShipDestroyed(target)) {
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

// @port 0x00462550 90% rng,bugfix
// Ghidra Asteroid_SpawnDestructionPackage (0x00462550): a broken asteroid
// spawns YieldQty resource-box freeflight objects, a part_count debris burst,
// its explode_type area effect, and splits into child asteroids from its def
// row (frag_type1/frag_type2, count derived from frag_count), then deactivates.
// The debris burst runs between the resource boxes and the area effect,
// matching the original RNG sequence.
//
// Resource-boxes: when yield_qty > 0 the original rolls
// `(NovaRandom_Range(0x65) + 0x32) * yield_qty * 0.01`, truncates it, and
// spawns that many persistent freeflight objects at the asteroid position
// carrying yield_type with spin set `(wander_type >> 2) + 1` (501..504, one per
// asteroid material). yield_type 0..5 is standard cargo and 1000..1127 is a
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
  // Debris SWParticle burst (part_count, part_color).
  NovaEffects_SpawnWeaponImpactParticleBurst(state,
                                             asteroid.target_pos_x,
                                             asteroid.target_pos_y,
                                             kAsteroidDebrisParticleSpeed,
                                             kAsteroidDebrisParticleScatter,
                                             kAsteroidDebrisLifeBase,
                                             kAsteroidDebrisLifeMax,
                                             def->part_color,
                                             /*blend_mode=*/0x20,
                                             def->part_count,
                                             kAsteroidDebrisPositionScatter);
  if (def->explode_type != -1) {
    NovaEffects_SpawnAreaImpact(state,
                                asteroid.target_pos_x,
                                asteroid.target_pos_y,
                                def->explode_type,
                                0,
                                true);
  }
  if (def->frag_count > 0) {
    // The original rolls NovaRandom_Range(frag_count) once and adds a baseline
    // of floor(frag_count/2) (disasm 0x004625df..0x004625eb:
    // MOVSX/CMP 0x80000000/SBB -1/SAR 1), so its documented "average +/-50%"
    // spread sits one low on odd counts. Keep the floor baseline for every
    // count, but BUGFIX(original): frag_count == 1 evaluates to
    // rand(1) + 0 == 0, contradicting the Bible's "average number of
    // sub-asteroids (+/- 50%)", whose lower bound is one. The roll is still
    // consumed first so the RNG sequence matches the original.
    const int roll =
        std::uniform_int_distribution<int>{0, def->frag_count - 1}(state.rng);
    int baseline = def->frag_count / 2;
    if (kApplyOriginalBugFixes && def->frag_count == 1) {
      baseline = 1;
    }
    const int count = roll + baseline;
    for (int i = 0; i < count; ++i) {
      // Both child types unset means the original spawns nothing; a single
      // set type always spawns; otherwise a random pick per fragment.
      if (def->frag_type1 == -1 && def->frag_type2 == -1) {
        continue;
      }
      std::int16_t child_type = def->frag_type1;
      if (def->frag_type1 != -1 && def->frag_type2 != -1) {
        child_type = std::uniform_int_distribution<int>{0, 1}(state.rng) == 0
                         ? def->frag_type1
                         : def->frag_type2;
      } else if (def->frag_type1 == -1) {
        child_type = def->frag_type2;
      }
      (void)NovaAsteroid_SpawnRecord(
          state, asteroid.target_pos_x, asteroid.target_pos_y, child_type);
    }
  }
  asteroid.active = false;
}

// @port 0x00436ff0 75% gameplay
// Ghidra NovaUi_ResolveWeaponSplashImpact (0x00436ff0): a blast weapon that
// reaches an asteroid (flags_quaternary bit 0 clear) spawns the area impact,
// splashes nearby ships (player-owned shots only), decrements the asteroid's
// integrity counter by the weapon's shield damage, and either runs the
// destruction package or nudges the asteroid along the impact.
// TODO(decomp(0x00436ff0)): the surviving-asteroid nudge derives the impact
// bearing from the shot's velocity; the original reads ShotState +0x20 (a
// range/heading scalar whose semantics are unconfirmed). Verify the field and
// the resulting impulse direction.
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
      Ship_ApplyDamageToShip(state,
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
                             static_cast<std::int16_t>(std::lround(
                                 static_cast<float>(weapon->reload_ticks))));
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
    // impulse is divided by the def's size-scaled mass and the result clamped
    // to +-2.0 px/frame per axis (DAT_0057531c).
    const AsteroidDef *def = state.scenario.AsteroidType(
        static_cast<std::int16_t>(asteroid.wander_type + 0x80));
    if (def != nullptr && def->mass > 0) {
      const float speed = static_cast<float>(weapon->impact_impulse) /
                          static_cast<float>(def->mass);
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

// Public test seam for the internal RefreshCollisionMasks pass (resolves each
// live entity's current-frame sprite mask from the non-SDL store).
void NovaCollision_RefreshCollisionMasks(GameState &state) {
  RefreshCollisionMasks(state);
}

// @port 0x0042d890 90% gameplay,cadence,audio
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

// @port 0x0043AED0 80% ui
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
      if (!ship.is_active || IsShipDestroyed(ship)) {
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
  // @port 0x00426ef0 90% gameplay
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
  if (shot.guidance_state == 998) {
    return false;
  }
  if (!target.is_active || target.current_system_id != shot.system_id ||
      IsShipDestroyed(target)) {
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
    if (owner.current_system_id != shot.system_id || IsShipDestroyed(owner)) {
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
    // @port 0x00415e60 100%
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

// @port 0x00436f70 100% divergence,synthetic
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
// DIVERGENCE(original): the original always uses Sprite_TestPixelMaskOverlap;
// the clean-room collapses the sprite-layer pair into a direct contact pass and
// falls back to a bounding circle when either side lacks a resolved mask. We
// keep the fallback rather than reproduce the sprite-layer mask lifetime.
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

// @port 0x004374f0 90% gameplay
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
      if (ShotIsWithinProximitySafetyDelay(shot, *weapon)) {
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
      // @port 0x00414530 100%
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
      Mission_ExecuteReactionScript(state,
                                    stellar->on_destroy_script,
                                    MissionScriptContext{"stellar OnDestroy"});
      stellar->strength = -1;
      stellar->destroyed_days_remaining = stellar->schedule_days;
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

// @port 0x00437e20 80% gameplay
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
    if (ShotIsWithinProximitySafetyDelay(shot, *weapon)) {
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
                                       std::int8_t impact_variant,
                                       bool suppress_retarget_logic) {
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
    // This clean-room path resolves only the beam queue's recorded ship
    // target. The original 0x0042f270 passes true for that intended contact;
    // its still-deferred incidental beam-contact sweep passes false.
    // Ghidra Shot_UpdateBeamHitQueue (0x0042f270) negative-impact-impulse arm:
    // a tractor/repulsor beam (impact_impulse < 0) from a ship arms a
    // velocity-match lock on the target or the source. The target must be a
    // real ship (pers_def_slot != 0x3ff), mass >= 1 ton and capability Flags
    // 0x400 clear; otherwise the impulse is suppressed. When the target is at
    // most 4/3 the source's mass (target.mass * 0.75 <= source.mass) the
    // TARGET locks onto the source and the negative impulse still reaches
    // Ship_ApplyDamageToShip. Otherwise the SOURCE self-locks and is stamped,
    // and -- when the two are at least 50 px apart on either axis -- is pulled
    // toward the target, after which the impulse is suppressed so the target
    // takes no push. DAT_005753e8 = 0.75, DAT_005753f0 = 50.0.
    std::int16_t effective_impulse = weapon->impact_impulse;
    if (effective_impulse < 0 && target.pers_def_slot != 0x3ff) {
      const ShipClass *target_class = ShipClassFor(state, target);
      const ShipClass *owner_class = ShipClassFor(state, owner);
      if (target_class == nullptr || owner_class == nullptr ||
          target_class->mass_tons < 1 ||
          (target_class->capability_flags & 0x0400U) != 0U) {
        effective_impulse = 0;
      } else if (static_cast<float>(target_class->mass_tons) * 0.75F <=
                 static_cast<float>(owner_class->mass_tons)) {
        target.velocity_match_target_ship_slot = owner_ship_slot;
        target.velocity_match_start_tick_60hz = state.tick_60hz;
      } else {
        if (owner.velocity_match_target_ship_slot == -1) {
          owner.velocity_match_target_ship_slot = owner_ship_slot;
        }
        owner.velocity_match_start_tick_60hz = state.tick_60hz;
        if ((std::abs(target.pos_x - owner.pos_x) >= kImpulseCloseRangePx ||
             std::abs(target.pos_y - owner.pos_y) >= kImpulseCloseRangePx) &&
            owner_class->mass_tons > 0 &&
            (owner_class->capability_flags & 0x0400U) == 0U) {
          // Bearing target->source with the (negative) impulse / source mass,
          // per-axis clamped to the class base speed then to the effective max
          // speed. Unlike Ship_ApplyDamageToShip's impulse block this self-lock
          // tug has no station-hold gate and no player-afterburner 1.8x widen.
          const int bearing_deg = static_cast<int>(
              BearingDeg(target.pos_x, target.pos_y, owner.pos_x, owner.pos_y));
          constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
          Math_AddPolarVelocityWithClamp(
              static_cast<float>(bearing_deg) * kDegToRad,
              static_cast<float>(effective_impulse) /
                  static_cast<float>(owner_class->mass_tons),
              owner_class->speed / 100.0F,
              owner.vel_x,
              owner.vel_y);
          const float max_speed = NovaShip_ComputeEffectiveMaxSpeedPxPerTick(
              state, owner, *owner_class);
          owner.vel_x = std::clamp(owner.vel_x, -max_speed, max_speed);
          owner.vel_y = std::clamp(owner.vel_y, -max_speed, max_speed);
        }
        effective_impulse = 0;
      }
    }
    Ship_ApplyDamageToShip(state,
                           target_ship_slot,
                           target,
                           shot.pos_x,
                           shot.pos_y,
                           effective_impulse,
                           weapon->mass_damage,
                           weapon->energy_damage,
                           owner_ship_slot,
                           /*allow_aggro_updates=*/true,
                           suppress_retarget_logic,
                           /*force_armor_only=*/impact_variant != 0,
                           /*bypass_shields=*/
                           (weapon->flags & 0x0020U) != 0U,
                           static_cast<std::int16_t>(std::lround(
                               static_cast<float>(weapon->reload_ticks))));
  }
}

} // namespace game
