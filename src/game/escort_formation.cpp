// Escort/formation positioning and attached-ship system transfer.
// See escort_formation.hpp for the function index and original addresses.
#include "escort_formation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../util/math.hpp"
#include "landed_store.hpp"
#include "scenario_data.hpp"
#include "ship_ai.hpp"
#include "ship_visual.hpp"
#include "spaceflight.hpp"
#include "weapon.hpp"

namespace game {

using evnova::util::AddPolar;

namespace {

constexpr float kDegToRad = 3.14159265359F / 180.0F;
// Ship_MoveShipTowardFormationOffset (0x00414390) constants.
constexpr float kFormationCreepThrustScale = 10.0F; // DOUBLE_00575170
constexpr float kFormationCreepDeadzonePx = 8.0F;   // FLOAT_005751b4
// Ship_UpdateEscortFormations (0x00413990) constants.
constexpr float kFormationRadiusScale = 0.7F;  // FLOAT_005751a8
constexpr float kFormationRadiusMinPx = 24.0F; // 0x18
constexpr float kFormationRadiusMaxPx = 60.0F; // 0x3c
constexpr std::int16_t kLeaderSpanFallback =
    0x40; // leader invalid-class default
constexpr std::int16_t kFollowerSpanFallback =
    0x4b; // Sprite_GetShipClassEscortFrameWidth
// System_RebuildInitialNpcAndMissionPopulation (0x0041af90) escort scatter.
constexpr float kScatterDistanceStart = 45.0F;      // FLOAT_0057524c
constexpr float kScatterDistanceStep = 1.165F;      // DOUBLE_00575250
constexpr float kScatterLaunchSpeed = 50.0F;        // FLOAT_0057522c
constexpr float kJumpArrivalHoldSentinel = -999.0F; // 0xc479c000

float HeadingDeg(const Ship &ship) { return ship.heading / kDegToRad; }

// The original truncates the float heading toward zero before the polar adds
// (x87 fistp/chop pattern, e.g. 0x00413b60 / 0x0041b149) -- not
// round-to-nearest.
int RoundHeadingDeg(const Ship &ship) {
  const int deg = static_cast<int>(HeadingDeg(ship));
  return ((deg % 360) + 360) % 360;
}

// Sprite_GetShipClassEscortFrameWidth (0x004624c0): the ship-class sprite
// span used to size wedge spacing. The original resolves
// ShipClassDef.base_sprite_clone_source_ship_class (+0xa08), which the
// sh\x8an loader writes to the clone-source class; clone classes share the
// source sprite, so the port decodes the class's own sh\x8an (identical span
// for clones). Divergence: non-clone classes read clone source 0 (class 0's
// sprite) in the original when the field defaults to zero; the port always
// uses the class's own span. TODO(decomp) if a scenario shows spacing drift.
// Like Sprite_GetFrameFullWidth, this returns the FULL frame width (see
// Ghidra 0x004624c0/0x00462390),
// fallback 0x4b = 75.
// Cached per class id: the scenario resource set is fixed for the process
// lifetime, and this runs per follower per frame.
std::int16_t EscortClassSpriteSpanPx(std::int16_t class_id) {
  static std::vector<std::int16_t> cache(0x300, -1);
  if (class_id < 0 || class_id >= 0x300) {
    return kFollowerSpanFallback;
  }
  std::int16_t &cached = cache[static_cast<std::size_t>(class_id)];
  if (cached >= 0) {
    return cached;
  }
  const auto resource_id = static_cast<std::uint16_t>(class_id + 0x80);
  if (const auto resource =
          NovaResource_Load(kShipVisualResourceType, resource_id)) {
    if (const auto visual = DecodeShipVisualDescriptor(*resource)) {
      cached = static_cast<std::int16_t>(visual->base_x_size);
      return cached;
    }
  }
  cached = kFollowerSpanFallback;
  return cached;
}

// One wedge slot's (lateral, forward) offset in multiples of the formation
// radius, from Ship_SetEscortLaunchOffsetVelocity (0x00413b60). Forward is
// along the leader's heading (negative = trailing); lateral is along
// heading + 90 (positive = starboard). Slot 1 is the unused dead-centre slot.
// The original has two tables: with an even follower count (flags bit 0
// clear) slots 4..8 fold the second row of three inward; with an odd count
// they do not. Other slots are identical in both tables.
std::pair<float, float> EscortWedgeSlot(int slot, float v, bool even_count) {
  switch (slot) {
  case 2:
    return {-v, -v};
  case 3:
    return {v, -v};
  case 4:
    return even_count ? std::pair{0.0F, -2.0F * v}
                      : std::pair{-2.0F * v, -2.0F * v};
  case 5:
    return even_count ? std::pair{-2.0F * v, -2.0F * v}
                      : std::pair{2.0F * v, -2.0F * v};
  case 6:
    return even_count ? std::pair{2.0F * v, -2.0F * v}
                      : std::pair{-v, -3.0F * v};
  case 7:
    return even_count ? std::pair{-v, -3.0F * v} : std::pair{v, -3.0F * v};
  case 8:
    return even_count ? std::pair{v, -3.0F * v} : std::pair{0.0F, -2.0F * v};
  case 9:
    return {-3.0F * v, -3.0F * v};
  case 10:
    return {3.0F * v, -3.0F * v};
  case 11:
    return {0.0F, -4.0F * v};
  case 12:
    return {-2.0F * v, -4.0F * v};
  case 13:
    return {2.0F * v, -4.0F * v};
  case 14:
    return {-4.0F * v, -4.0F * v};
  case 15:
    return {4.0F * v, -4.0F * v};
  case 16:
    return {-v, -5.0F * v};
  case 17:
    return {v, -5.0F * v};
  case 18:
    return {-3.0F * v, -5.0F * v};
  case 19:
    return {3.0F * v, -5.0F * v};
  case 20:
    return {-5.0F * v, -5.0F * v};
  case 21:
    return {5.0F * v, -5.0F * v};
  default:
    return {0.0F, 0.0F};
  }
}

// Ship_SetEscortLaunchOffsetVelocity (0x00413b60): writes the follower's
// formation offset position (leader position + polar wedge offset).
void SetEscortLaunchOffsetVelocity(GameState &state,
                                   Ship &escort,
                                   const Ship &target,
                                   int slot,
                                   int follower_count,
                                   float radius) {
  const bool even_count = (follower_count & 1) == 0;
  const auto [lateral, forward] = EscortWedgeSlot(slot, radius, even_count);
  escort.formation_offset_x = 0.0F;
  escort.formation_offset_y = 0.0F;
  const int heading_deg = RoundHeadingDeg(target);
  AddPolar(static_cast<float>(heading_deg) * kDegToRad,
           forward,
           escort.formation_offset_x,
           escort.formation_offset_y);
  AddPolar(static_cast<float>(heading_deg + 90) * kDegToRad,
           lateral,
           escort.formation_offset_x,
           escort.formation_offset_y);
  escort.formation_offset_x += target.pos_x;
  escort.formation_offset_y += target.pos_y;
  (void)state;
}

} // namespace

// Ghidra 0x00414390 Ship_MoveShipTowardFormationOffset.
void Ship_MoveShipTowardFormationOffset(GameState &state,
                                        Ship &ship,
                                        bool snap_to_offset,
                                        float elapsed_ticks) {
  // A ship holding station does not creep (the snap variant ignores the hold).
  if (!snap_to_offset && ship.ai_station_hold_timer > 0.0F) {
    return;
  }
  const std::int16_t resolved = ship.resolved_squad_leader_ship_slot;
  if (resolved < 0 ||
      resolved >= static_cast<std::int16_t>(GameState::kMaxShips)) {
    return;
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  // Original creep speed: Ship_ComputeShipEffectiveThrust * 10.0, damped by
  // (1 - min(ionization intensity, 0.8)) while ionized (cap constant 0.8,
  // DOUBLE_00575190; factor constant 1.0, DOUBLE_005750d8).
  float step = (cls ? NovaShip_ComputeEffectiveStats(state, ship, *cls)
                          .thrust_px_per_tick2
                    : 0.0F) *
               kFormationCreepThrustScale;
  if (cls != nullptr && ship.ionization_points > 0.0F &&
      cls->ionization_capacity > 0) {
    const float intensity =
        ship.ionization_points / static_cast<float>(cls->ionization_capacity);
    step *= 1.0F - std::min(intensity, 0.8F);
  }
  step *= elapsed_ticks;
  if (!snap_to_offset) {
    if (ship.pos_x <= ship.formation_offset_x - kFormationCreepDeadzonePx) {
      ship.pos_x += step;
    } else if (ship.formation_offset_x + kFormationCreepDeadzonePx <=
               ship.pos_x) {
      ship.pos_x -= step;
    }
    if (ship.pos_y <= ship.formation_offset_y - kFormationCreepDeadzonePx) {
      ship.pos_y += step;
    } else if (ship.formation_offset_y + kFormationCreepDeadzonePx <=
               ship.pos_y) {
      ship.pos_y -= step;
    }
  } else {
    ship.pos_x = ship.formation_offset_x;
    ship.pos_y = ship.formation_offset_y;
  }
}

// Ghidra 0x00413990 Ship_UpdateEscortFormations.
void Ship_UpdateEscortFormations(GameState &state, Ship &leader, bool snap) {
  // Spacing radius: max participant sprite span * 0.7, clamped to 24..60 px.
  std::int16_t span = kLeaderSpanFallback;
  if (state.scenario.Ship(
          static_cast<std::int16_t>(leader.ship_class_id + 0x80)) != nullptr) {
    span = EscortClassSpriteSpanPx(leader.ship_class_id);
  }
  std::vector<Ship *> followers;
  followers.reserve(8);
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &other = state.ShipAt(slot);
    if (other.ship_instance_id == leader.ship_instance_id || !other.is_active) {
      continue;
    }
    if (other.resolved_squad_leader_ship_slot != leader.ship_instance_id) {
      continue;
    }
    span = static_cast<std::int16_t>(std::max<std::int16_t>(
        span, EscortClassSpriteSpanPx(other.ship_class_id)));
    followers.push_back(&other);
  }
  if (followers.empty()) {
    return;
  }
  // The original truncates span*0.7 toward zero before clamping (x87
  // fistp/ADD 0x7fffffff pattern at 0x00413a72), not round-to-nearest.
  int radius =
      static_cast<int>(static_cast<float>(span) * kFormationRadiusScale);
  radius = std::clamp(radius,
                      static_cast<int>(kFormationRadiusMinPx),
                      static_cast<int>(kFormationRadiusMaxPx));
  // Slot indices start at 2; the count (followers + 1) picks the wedge table.
  int follower_count = static_cast<int>(followers.size()) + 1;
  int slot_index = 1;
  for (Ship *follower : followers) {
    ++slot_index;
    SetEscortLaunchOffsetVelocity(state,
                                  *follower,
                                  leader,
                                  slot_index,
                                  follower_count,
                                  static_cast<float>(radius));
    if (snap && follower->ai_state_code != 0x15) {
      Ship_MoveShipTowardFormationOffset(state, *follower, /*snap=*/true, 0.0F);
    }
  }
}

// Ghidra 0x004156a0 Ship_ReacquireSquadLeader.
void Ship_ReacquireSquadLeader(GameState &state, Ship &ship) {
  const std::int16_t stale = ship.squad_leader_ship_slot;
  if (stale < 0 || stale >= static_cast<std::int16_t>(GameState::kMaxShips)) {
    return;
  }
  const Ship &leader = state.ShipAt(static_cast<std::size_t>(stale));
  if (leader.is_active && !NovaAiShip_IsDisabled(state, leader)) {
    return;
  }
  // Replacement search: the heaviest active, non-disabled hull in the system
  // whose per-tick snapshot target is the same stale leader (a sibling
  // follower is promoted). Hull mass uses the class Mass field
  // (ShipClassDef.mass_tons +0x9ee).
  std::int16_t replacement = -1;
  int replacement_mass = 0;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &candidate = state.ShipAt(slot);
    if (!candidate.is_active ||
        stale != state.squad_leader_slot_snapshot[slot] ||
        NovaAiShip_IsDisabled(state, candidate)) {
      continue;
    }
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(candidate.ship_class_id + 0x80));
    const int mass = cls != nullptr ? cls->mass_tons : 0;
    if (replacement == -1 || mass > replacement_mass) {
      replacement = static_cast<std::int16_t>(slot);
      replacement_mass = mass;
    }
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (replacement == -1) {
    // No sibling can lead: revert to the class default AI (state 0x13 idle).
    ship.squad_leader_ship_slot = -1;
    ship.ai_behavior_code =
        cls != nullptr ? cls->default_ai_behavior : ship.ai_behavior_code;
    ship.ai_state_code = 0x13;
    ship.ai_control_mode = 0;
    ship.ai_station_hold_timer = -1.0F;
    return;
  }
  if (replacement == ship.ship_instance_id) {
    // This ship is the heaviest sibling: it inherits the stale leader's
    // combat state when governments match, otherwise it disengages.
    ship.ai_behavior_code =
        cls != nullptr ? cls->default_ai_behavior : ship.ai_behavior_code;
    ship.ai_station_hold_timer = -1.0F;
    // The original reads the stale leader slot here (the slot's data remains
    // even when the ship is gone).
    const Ship &old_leader = state.ShipAt(static_cast<std::size_t>(stale));
    if (ship.faction_or_government_id == -1 ||
        ship.faction_or_government_id != old_leader.faction_or_government_id) {
      const std::int16_t state_code = ship.ai_state_code;
      if (state_code == 5 || state_code == 10 || state_code == 0xc) {
        ship.ai_state_code = 0;
        ship.ai_control_mode = 0;
      } else if (state_code == 0xb) {
        ship.ai_state_code = 2;
        ship.ai_control_mode = 4;
      } else if (ship.primary_target_ship_slot == -1 || state_code == 7 ||
                 state_code == 9) {
        ship.ai_state_code = 0;
        ship.ai_control_mode = 0;
      } else {
        ship.ai_state_code = 4;
        ship.ai_control_mode = 0;
      }
    } else {
      ship.primary_target_ship_slot = old_leader.primary_target_ship_slot;
      ship.ai_secondary_target_slot = old_leader.ai_secondary_target_slot;
      ship.ai_state_code = old_leader.ai_state_code;
      ship.ai_control_mode = old_leader.ai_control_mode;
    }
    ship.squad_leader_ship_slot = -1;
    return;
  }
  ship.squad_leader_ship_slot = replacement;
  if (ship.ai_behavior_code == 5) {
    ship.ai_behavior_code = 6;
  }
  if (ship.ai_station_hold_timer < 0.0F) {
    ship.ai_state_code = 0x13;
  }
}

// Ghidra 0x004186b0 Frame_TickSystems, scope-6 leader-flag pass
// (disassembly 0x00418a04..0x00418db4).
void Ship_TickLeaderFlags(GameState &state) {
  for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
    state.squad_leader_slot_snapshot[slot] =
        state.ShipAt(slot).squad_leader_ship_slot;
    Ship &ship = state.ShipAt(slot);
    ship.is_any_ships_squad_leader = false;
    ship.ai_followed_as_leader = false;
    ship.ai_selected_as_resolved_target = false;
  }
  const std::int16_t current_system = state.player.current_system_id;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.ai_behavior_code <= 4 ||
        ship.current_system_id != current_system) {
      continue;
    }
    const std::int16_t target = ship.squad_leader_ship_slot;
    if (target < 1) {
      if (target == 0) {
        state.player.is_any_ships_squad_leader = true;
      }
    } else {
      Ship_ReacquireSquadLeader(state, ship);
      const std::int16_t reacquired = ship.squad_leader_ship_slot;
      if (reacquired != -1 &&
          state.SlotInRange(static_cast<std::size_t>(reacquired))) {
        state.ShipAt(static_cast<std::size_t>(reacquired))
            .is_any_ships_squad_leader = true;
      }
    }
    // Provisional: the original additionally requires the leader slot to be
    // greater than the follower's own slot (slot counter < leader < 0x40),
    // which excludes player-led and earlier-slot leads from +0xC1. Nothing
    // consumes +0xC1 yet; the condition is preserved as decompiled.
    const std::int16_t formation_lead = ship.formation_leader_ship_slot;
    if (static_cast<std::int16_t>(slot) < formation_lead &&
        formation_lead < static_cast<std::int16_t>(GameState::kMaxShips)) {
      state.ShipAt(static_cast<std::size_t>(formation_lead))
          .ai_followed_as_leader = true;
    }
    std::int16_t resolved;
    if (ship.ai_behavior_code < 5) {
      resolved = ship.formation_leader_ship_slot;
    } else {
      resolved = -1;
      if (ship.primary_target_ship_slot != -1 && ship.ai_state_code == 4) {
        resolved = ship.formation_leader_ship_slot;
      }
      if (resolved == -1) {
        resolved = ship.squad_leader_ship_slot;
      }
    }
    // The original writes the resolved value back into the ship
    // (LAB_00418d70); the escort formation pass and the wedge-offset
    // movement arms key off this field, so skipping the write leaves hired
    // escorts with resolved == -1 and an inert formation system.
    ship.resolved_squad_leader_ship_slot = resolved;
    if (resolved >= 0 &&
        resolved < static_cast<std::int16_t>(GameState::kMaxShips)) {
      state.ShipAt(static_cast<std::size_t>(resolved))
          .ai_selected_as_resolved_target = true;
    }
  }
}

// Ghidra 0x0041e240 Ship_ResetShipToDefaultCombatState.
void NovaShip_ResetToDefaultCombatState(GameState &state,
                                        Ship &ship,
                                        bool refill) {
  if (ship.squad_leader_ship_slot != 0) {
    return;
  }
  ship.jump_destination_stellar_id = -2;
  ship.jump_destination_system_id = -2;
  ship.ai_forward_thrust_cmd = 0.0F;
  ship.ai_desired_speed = 0.0F;
  ship.pers_def_slot = -1;
  ship.ai_fire_trigger_latch = 0;
  // TODO(decomp(0x0041e240)) skipped: the six cargo-bin clears and the
  // ai_odds_score / field_0xac / last_fired_weapon_bank_slot resets -- NPC
  // cargo bins and the combat-odds score are not modelled on the port Ship.
  ship.engine_glow_level = 0;
  ship.engine_glow_intensity = 0.0F;
  ship.dude_class_id = -1;
  ship.mission_fleet_slot = -1;
  ship.ionization_points = 0.0F;
  ship.escort_command_pending = 0;
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (refill && cls != nullptr) {
    // Ship_ComputeShipMaxShieldPoints / MaxArmor reduce to the class base for
    // non-mission NPC ships (attached escorts have no fleet slot).
    ship.shield_points = static_cast<float>(cls->base_shield);
    ship.armor_points = static_cast<float>(cls->base_armor);
  }
  ship.current_system_id = state.player.current_system_id;
  ship.heading = state.player.heading;
  ship.formation_leader_ship_slot = -1;
  ship.resolved_squad_leader_ship_slot = 0;
  ship.formation_offset_x = 0.0F;
  ship.formation_offset_y = 0.0F;
  // The original arms the PLAYER's +0xC2 here so the per-frame formation pass
  // runs for the freshly adopted wedge.
  state.player.ai_selected_as_resolved_target = true;
  ship.vel_y = 0.0F;
  ship.vel_x = 0.0F;
  ship.speed = 0.0F;
  if (refill) {
    // Refill the class-default weapon ammo/secondary stock by forcing the
    // cached NPC bank reseed (the original copies default_weapon_ammo /
    // default_weapon_secondary into the per-ship tables).
    ship.npc_weapon_banks_ship_class = -1;
    NovaWeapon_EnsureNpcWeaponBanks(state, ship);
  }
  NovaShip_EnterSquadReturnState(state, ship);
}

// Ghidra 0x0041af90 System_RebuildInitialNpcAndMissionPopulation, escort
// adoption slice (disassembly 0x0041b050..0x0041b237).
void NovaSystem_RestorePlayerEscorts(GameState &state,
                                     bool refill,
                                     std::uint32_t now_ms) {
  (void)now_ms;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.squad_leader_ship_slot != 0) {
      continue;
    }
    if (NovaAiShip_IsDisabled(state, ship)) {
      // 0x0041b026 clears active before transfer: the lost escort no longer
      // contributes to the denominator, but retains its leader attachment.
      ship.is_active = false;
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      if (ship.ai_behavior_code == 6 && ship.mission_fleet_slot == -1 &&
          cls != nullptr && cls->default_ai_behavior < 3) {
        Player_TransferCargoAndJunkToEscortByRatio(
            state, static_cast<std::int16_t>(slot));
      }
      continue;
    }
    if (ship.mission_fleet_slot == -1) {
      NovaShip_ResetToDefaultCombatState(state, ship, refill);
    }
  }
  Ship_UpdateEscortFormations(state, state.player, /*snap=*/true);
  // Jump arrival: the player core sets its station-hold timer to -999 around
  // the rebuild (0x0044fa83 / 0x0044faa2), which arms this scatter -- escorts
  // stream in behind the player at full speed instead of sitting on the wedge.
  if (state.player.ai_station_hold_timer != 0.0F) {
    float term = kScatterDistanceStart;
    float distance = 0.0F;
    while (term > 0.0F) {
      distance += term;
      term -= kScatterDistanceStep;
    }
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      Ship &ship = state.ShipAt(slot);
      if (!ship.is_active || ship.squad_leader_ship_slot != 0) {
        continue;
      }
      const float heading_deg = static_cast<float>(RoundHeadingDeg(ship));
      AddPolar(
          (heading_deg + 180.0F) * kDegToRad, distance, ship.pos_x, ship.pos_y);
      ship.ai_station_hold_timer = kJumpArrivalHoldSentinel;
      ship.ai_mode_start_time_ms = 0;
      AddPolar(
          heading_deg * kDegToRad, kScatterLaunchSpeed, ship.vel_x, ship.vel_y);
      // Arm the arrival-slowdown ladder (state 8 -> dispatcher arms control
      // mode 0x0a, whose -50 override decays 1.165/tick): the sentinel pair
      // written above is exactly Ship_EnterShipAiState0x08_Slowdown
      // (0x00410e20), and the follow-player mission-fleet arrival slice
      // (Stellar_HandleStellarEntryAndExit 0x00457580) runs the same ladder for
      // attached ships. Without it the 50 px/tick fling is only shed by
      // mode-9 thrust, which takes tens of seconds for slow hulls (cargo
      // drones flew ~28000 px before stopping). TODO(decomp): the original
      // write site that re-enters state 8 after escort adoption was not
      // located, so this is a reconstructed glue write.
      ship.ai_state_code = 8;
      ship.ai_control_mode = 0;
      // Clear a stale coast timer: the state-8 dispatcher arm is gated off
      // while ai_maneuver_timer_ms > 0.
      ship.ai_maneuver_timer_ms = -1.0F;
    }
  }
  // The original's flag != 0 tail call re-snaps after the mission-fleet and
  // ambient spawn slices; call sites run those in between.
  if (refill) {
    Ship_UpdateEscortFormations(state, state.player, /*snap=*/true);
  }
}

} // namespace game
