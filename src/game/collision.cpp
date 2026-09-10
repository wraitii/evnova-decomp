#include "collision.hpp"

#include "asteroid.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "impact_effects.hpp"
#include "mission.hpp"
#include "scenario_data.hpp"
#include "ship_ai.hpp"
#include "spaceflight.hpp"
#include "targeting.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
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
// Disable-transition armor pin (Shot_ResolveShipHitFromWeapon 0x0041a4b0):
// _DAT_00575238 = 1/3 and _DAT_00575208 = 0.1 for capability-flags 0x10 hulls,
// both +1.0 (_DAT_00575230).
constexpr float kDisableArmorPinFraction = 1.0F / 3.0F;
constexpr float kDisableArmorPinFractionCap0x10 = 0.1F;

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
      points = static_cast<int>(std::lround(static_cast<float>(points) *
                                            (1.0F - distance_sq / radius_sq)));
    }
  }
  if (points > 0) {
    target.ionization_points += static_cast<float>(points);
    target.ionization_color |= weapon.ionization_color;
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
// re-entry, escort-command exclusions) is approximated by the conservative
// subset below, and the kill-side faction combat events plus combat-rating
// award are deferred. The stellar-target redirect branch (damage x30 toward
// attackers closer than the stellar under attack) is also deferred.
} // namespace

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

  // Shot_ResolveShipHitFromWeapon leaves destruction as an armor-state. The
  // later Ship_HandleShip path owns the original fading debris pool; queue the
  // first visible equivalent at the exact alive -> destroyed transition so a
  // lethal NPC hit cannot leave its hull silently on screen.
  if (!was_destroyed && IsDestroyed(target) &&
      !target.destruction_visual_triggered) {
    target.destruction_visual_triggered = true;
    const std::int16_t death_delay =
        target_class == nullptr
            ? 0
            : std::max<std::int16_t>(0, target_class->death_delay_frames);
    // Unit/test states without loaded ship tables retain the old armor-only
    // sentinel; real scenario ships use the Bible DeathDelay timer. The timer
    // itself is seeded by Ship_UpdateVisualState (0x00428340): x1 for NPCs and
    // x3 for the player (g_player_death_timer_scale 0x00575378). NPCs run
    // through the port's Ship_UpdateVisualState pass; the player's is bridged
    // in NovaPlayer_TickStatusAndOutfitEvents, which owns the 3x scale, so
    // leave the player's timer untouched here.
    if (target_class != nullptr && death_delay > 0 && target_slot != 0) {
      target.death_timer_active = static_cast<float>(death_delay);
    }
    target.destruction_visual_timer_ms =
        static_cast<float>(death_delay) * (1000.0F / 30.0F);
    NovaTargeting_ClearDestroyedShipReferences(state, target_slot);
    NovaEffects_SpawnShipDestructionBurst(
        state,
        target,
        target_class == nullptr
            ? 0
            : target_class->destruction_effect_while_breaking);
    if (target.destruction_visual_timer_ms <= 0.0F && target_class != nullptr) {
      NovaEffects_SpawnShipDestructionFinale(
          state, target, target_class->destruction_effect_final);
      target.destruction_finale_triggered = true;
    }
    // TODO(decomp) skipped: kill-side Government_ProcessFactionCombatEvent
    // (event 3) plus Frame_AddCombatRatingPoints when the victim's government
    // tracks reputation.
  }

  // ---- Fire-restriction (disable) transition arms (0x0041a4b0..) ---------
  const bool now_fire_restricted = NovaAiShip_IsDisabled(state, target);
  if (now_fire_restricted) {
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
        target.target_stellar_object_id == -1) {
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
    // recent-hit regen latch, round armor to whole points (original quirk),
    // and quick-fail every mission flagged 0x0004 (fail on player disable).
    if (target_slot == 0 && !was_fire_restricted) {
      state.pending_ui_sounds.push_back(GameState::PendingUiSound{1, 1});
      if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x11f)) {
        NovaHud_ShowOverlayMessage(
            state, *text, /*duration_frames=*/std::uint64_t{0xf0});
      }
      state.recently_hit_timer = 300.0F;
      target.armor_points = static_cast<float>(
          static_cast<int>(std::llround(target.armor_points)));
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
                  .target_stellar_object_id == -1) {
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
// impact flurries; gated on WeaponDef.impact_particle_count > 0), and
// Shot_SpawnLinkedShotsOnImpact (0x00420d30, gated on range_link_gate > 0 and
// the allow_linked_shots flag). ShotState +0x32 damage_reduction is not
// carried on ActiveShot; the original zero-initializes it and no writer was
// found, so the subtraction is a no-op.
void ResolveShotCollisionHit(GameState &state,
                             ActiveShot &shot,
                             Ship &target,
                             std::int16_t target_slot,
                             bool allow_linked_shots) {
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
  (void)allow_linked_shots; // linked-shot spawner deferred (see TODO above)

  shot.consumed = true;
}

// Ghidra Weapon_SpawnWeaponImpactEffectPackage (0x00462550), asteroid arm:
// a broken asteroid spawns junk freeflight objects, a debris particle burst,
// its destruction area effect, and splits into child asteroids from its def
// row (child types +0x06/+0x08, count derived from +0x0a), then deactivates.
// TODO(decomp) skipped: junk freeflight objects (row +0x02 count, +0x04 type;
// no freeflight pool yet) and the debris SWParticle burst (row +0x0c).
void ResolveAsteroidDestructionPackage(GameState &state,
                                       AsteroidState &asteroid) {
  const AsteroidDef *def = state.scenario.AsteroidType(
      static_cast<std::int16_t>(asteroid.wander_type + 0x80));
  if (def == nullptr) {
    asteroid.active = false;
    return;
  }
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
  // TODO(decomp) skipped: Weapon_SpawnWeaponImpactParticleBurst
  // (impact_particle_count > 0).

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

bool NovaWeapon_CanProjectileHitShip(const GameState &state,
                                     const ActiveShot &shot,
                                     std::int16_t target_slot) {
  // Ghidra 0x00426ef0 Weapon_CanWeaponHitTarget.
  const Weapon *weapon = WeaponForShot(state, shot);
  if (weapon == nullptr || !ValidShipSlot(target_slot) || shot.consumed) {
    return false;
  }
  if (!ValidShipSlot(shot.owner_ship_slot) ||
      shot.owner_ship_slot == target_slot) {
    // TODO(decomp): unowned shots (e.g. stellar defense batteries) accept
    // only the recorded target slot; the clean-room has no ownerless shots.
    return false;
  }
  const std::int16_t owner_slot = shot.owner_ship_slot;
  const Ship &owner = state.ShipAt(static_cast<std::size_t>(owner_slot));
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));

  if (!target.is_active || target.current_system_id != shot.system_id ||
      IsDestroyed(target)) {
    return false;
  }
  // Clean-room guard: the original relies on shot lifetime to drop dead or
  // departed owners; the explicit checks keep stale shots from damaging
  // across systems in this simplified model.
  if (owner.current_system_id != shot.system_id || IsDestroyed(owner)) {
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
  if (owner.target_stellar_object_id != -1 &&
      target.target_stellar_object_id == owner.target_stellar_object_id) {
    return false;
  }
  // TODO(decomp) skipped: the disabled range gate comparing
  // ShipClassDef +0xa10 against ShotState +0x42 (per-shot scatter range,
  // 0xffff for non-turret modes) - the field semantics are still provisional.

  const bool owner_chain_to_player = OwnerChainReachesPlayer(state, owner_slot);
  if (owner_chain_to_player) {
    // Player-aligned fire never hits the player's own escort chain,
    // xenophobic (0x40) owner governments, immaterial (0x08) target
    // governments, or booty-flagged (0x100) dude targets.
    const std::int16_t target_leader = target.squad_leader_ship_slot;
    const bool target_is_player_escort =
        target_slot != 0 && ValidShipSlot(target_leader) &&
        state.ShipAt(static_cast<std::size_t>(target_leader))
                .squad_leader_ship_slot == 0;
    if (target_is_player_escort) {
      return false;
    }
    if (HasGovernmentFlag(state, owner, 0x0040U)) {
      return false;
    }
    if (HasGovernmentFlag(state, target, 0x0008U)) {
      return false;
    }
    const DudeDef *target_dude = DudeFor(state, target);
    if (target_dude != nullptr && (target_dude->booty_flags & 0x0100U) != 0U) {
      return false;
    }
    // Owner-side booty gate: when the owner has no valid dude record, the
    // owner's squad leader's dude record is consulted instead.
    const DudeDef *owner_dude = DudeFor(state, owner);
    if (owner_dude == nullptr && ValidShipSlot(owner.squad_leader_ship_slot)) {
      owner_dude = DudeFor(
          state,
          state.ShipAt(static_cast<std::size_t>(owner.squad_leader_ship_slot)));
    }
    if (owner_dude != nullptr && (owner_dude->booty_flags & 0x0100U) != 0U) {
      return false;
    }
  }
  if (owner_slot > 0 && owner.ai_state_code == 0x10) {
    return false;
  }

  // The target's planet-type capability bit must agree with weapon
  // flags_secondary bit 0x400.
  const ShipClass *target_class = ShipClassFor(state, target);
  if (target_class == nullptr ||
      ((target_class->capability_flags ^ weapon->flags_secondary) & 0x0400U) !=
          0U) {
    return false;
  }
  // Government aggro flag (0x40): NPC targets carrying it are only hittable
  // by owners that are neither the player nor player escorts.
  if (target_slot != 0 && HasGovernmentFlag(state, target, 0x0040U) &&
      (owner_slot == 0 || (ValidShipSlot(owner.squad_leader_ship_slot) &&
                           owner.squad_leader_ship_slot == 0))) {
    return false;
  }
  if (SharesSquadRoot(state, target_slot, owner_slot)) {
    return false;
  }
  return true;
}

// Ghidra Ship_HandleSpritePairCollision (0x004374f0) with its sprite-layer
// driver inlined: the original is invoked per overlapping (ship sprite, shot
// sprite) pair by TestSpriteLayerOverlaps and picks the bounding-circle or
// pixel-mask test by frame-time budget (circle when avg frame time >= 2.0ms
// or half-span <= 0x20).
void NovaWeapon_ResolveDirectShotCollisions(GameState &state) {
  for (ActiveShot &shot : state.active_shots) {
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
      // TODO(decomp): pixel-mask overlap (Sprite_TestPixelMaskOverlap
      // 0x00475c80) when sprites are available; the circle envelope stands in
      // for Sprite_TestBoundingCircleOverlap.
      const float dx = target.pos_x - shot.pos_x;
      const float dy = target.pos_y - shot.pos_y;
      const float radius = std::max(0.0F, target.collision_radius_px) +
                           std::max(0.0F, shot.collision_radius_px);
      if (dx * dx + dy * dy > radius * radius) {
        continue;
      }
      if (ShotIsInLateCollisionWindow(shot, *weapon)) {
        break;
      }
      ResolveShotCollisionHit(state,
                              shot,
                              state.ShipAt(static_cast<std::size_t>(slot)),
                              slot,
                              /*allow_linked_shots=*/false);
      break;
    }
  }
  RemoveConsumedShots(state);
}

// Ghidra Shot_ResolveCollisions (0x00437e20): the blast-proximity pass. It
// runs after the direct-contact pass, so a shot that already connected skips.
void NovaWeapon_ResolveProjectileCollisions(GameState &state) {
  for (ActiveShot &shot : state.active_shots) {
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

    // TODO(decomp) skipped: the stellar-contact branch (weapons with
    // flags_secondary 0x400 pixel-mask against the system's stellar sprites,
    // damaging StellarDef +0x3c and re-arming destroyed bodies) - it needs
    // sprite masks and the stellar health model.
    if (weapon->blast_radius > 0) {
      bool ship_hit = false;
      for (std::int16_t slot = 0;
           slot < static_cast<std::int16_t>(GameState::kMaxShips);
           ++slot) {
        if (!NovaWeapon_CanProjectileHitShip(state, shot, slot)) {
          continue;
        }
        const Ship &target = state.ShipAt(static_cast<std::size_t>(slot));
        // Ghidra: radius = ROUND(blast_radius + ship half-span * 0.333)
        // (DAT_00575338); positions and distances are compared in integer
        // space. The clean-room's collision_radius_px stands in for the
        // sprite half-span.
        const auto radius = static_cast<float>(
            std::lround(static_cast<float>(weapon->blast_radius) +
                        std::max(0.0F, target.collision_radius_px) *
                            kProximitySpanFraction));
        const auto dx =
            static_cast<int>(std::lround(target.pos_x - shot.pos_x));
        const auto dy =
            static_cast<int>(std::lround(target.pos_y - shot.pos_y));
        if (dx * dx + dy * dy <=
            static_cast<int>(radius) * static_cast<int>(radius)) {
          ResolveShotCollisionHit(state,
                                  shot,
                                  state.ShipAt(static_cast<std::size_t>(slot)),
                                  slot,
                                  /*allow_linked_shots=*/true);
          ship_hit = true;
          break;
        }
      }
      // Asteroid branch: when no ship was hit and flags_quaternary bit 0 is
      // clear, the 16 asteroid records are scanned (integer dist^2 <=
      // blast_radius^2, round-half-away positions) and the first contact
      // runs NovaUi_ResolveWeaponSplashImpact.
      if (!ship_hit && (weapon->flags_quaternary & 0x0001U) == 0U) {
        const auto blast = static_cast<int>(weapon->blast_radius);
        for (AsteroidState &asteroid : state.asteroid_pool) {
          if (!asteroid.active) {
            continue;
          }
          const auto dx = static_cast<int>(
              std::lround(std::abs(asteroid.target_pos_x - shot.pos_x)));
          const auto dy = static_cast<int>(
              std::lround(std::abs(asteroid.target_pos_y - shot.pos_y)));
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
