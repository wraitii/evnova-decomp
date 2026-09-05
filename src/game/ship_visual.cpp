#include "ship_visual.hpp"

#include "collision.hpp"
#include "game_state.hpp"
#include "hud_overlay.hpp"
#include "mission.hpp"
#include "scenario_data.hpp"
#include "ship_ai.hpp"

#include <cmath>
#include <cstdint>
#include <random>

namespace game {
namespace {

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) << 8U |
      std::to_integer<std::uint8_t>(bytes[offset + 1]));
}

[[nodiscard]] std::int16_t ReadBeI16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  if (offset + 1 >= bytes.size()) {
    return 0; // descriptor too short for this field; treat as zero
  }
  return static_cast<std::int16_t>(ReadBe16(bytes, offset));
}

constexpr std::size_t kMinDescriptorSize = 0x36;

} // namespace

std::optional<ShipVisualDescriptor>
DecodeShipVisualDescriptor(std::span<const std::byte> resource_data) {
  if (resource_data.size() < kMinDescriptorSize) {
    return std::nullopt;
  }
  ShipVisualDescriptor d;
  d.base_image_id = ReadBe16(resource_data, 0x00);   // BaseImageID
  d.base_mask_id = ReadBe16(resource_data, 0x02);    // BaseMaskID
  d.base_set_count = ReadBeI16(resource_data, 0x04); // BaseSetCount (>=1)
  if (d.base_set_count < 1) {
    d.base_set_count = 1;
  }
  d.base_x_size = ReadBe16(resource_data, 0x06);        // BaseXSize
  d.base_y_size = ReadBe16(resource_data, 0x08);        // BaseYSize
  d.base_transparency = ReadBeI16(resource_data, 0x0a); // BaseTransp
  // Alternate (bank/unfold) sprite sheet. Ghidra Ship_UpdateVisualState
  // (0x00428340) assigns g_ship_sprite_alt[class] to the ship sprite and
  // composes the displayed frame as row * FramesPer + heading_frame, with the
  // alternate rows living after the base rows in frame order.
  d.alt_image_id = ReadBe16(resource_data, 0x0c);          // AltImageID
  d.alt_mask_id = ReadBe16(resource_data, 0x0e);           // AltMaskID
  d.alt_set_count = ReadBeI16(resource_data, 0x10);        // AltSetCount
  d.sprite_behavior_flags = ReadBe16(resource_data, 0x2e); // Flags
  d.anim_delay = ReadBeI16(resource_data, 0x30);           // AnimDelay
  d.weapon_decay = ReadBeI16(resource_data, 0x32);         // WeapDecay
  // Engine-glow layer (+0x16 image / +0x18 mask / +0x1a x / +0x1c y), read as
  // big-endian shorts like the base fields. Negative/zero id = no glow layer.
  d.engine_glow_image_id = ReadBeI16(resource_data, 0x16); // GlowImageID
  d.engine_glow_mask_id = ReadBeI16(resource_data, 0x18);  // GlowMaskID
  d.engine_glow_x_size = ReadBe16(resource_data, 0x1a);    // GlowXSize
  d.engine_glow_y_size = ReadBe16(resource_data, 0x1c);    // GlowYSize
  d.frames_per_rotation = ReadBeI16(resource_data, 0x34);  // FramesPer
  if (d.frames_per_rotation == 0) {
    d.frames_per_rotation = 36;
  }
  // Per-turret-group weapon-exit (muzzle) offsets. The loader
  // (ShipClass_LoadShipClassVisualAndLaunchData 0x004b4ee0) copies these from
  // the sh\x8an payload into ShipClassDef field_0xa42..; Weapon_ApplyTurret-
  // SpreadVelocity (0x0046c5c0) reads them per turret group x quadrant to
  // offset a projectile's muzzle. Layout (group g, quadrant q):
  //   lateral[g][q] = sh\x8an 0x48 + 16g + 2q  (field_0xa42)
  //   forward[g][q] = sh\x8an 0x50 + 16g + 2q  (field_0xa44)
  //   drop[g][q]    = sh\x8an 0x90 + 16g + 2q  (field_0xa82)
  for (std::size_t g = 0; g < d.turret_muzzles.size(); ++g) {
    auto &m = d.turret_muzzles[g];
    for (std::size_t q = 0; q < 4; ++q) {
      m.lateral[q] = ReadBeI16(resource_data, 0x48 + g * 16 + q * 2);
      m.forward[q] = ReadBeI16(resource_data, 0x50 + g * 16 + q * 2);
      m.drop[q] = ReadBeI16(resource_data, 0x90 + g * 16 + q * 2);
    }
  }
  // Compress scales (field_0xaa4/0xaa8); the loader multiplies the raw short
  // by the 0.01 weapon-exit compress scale.
  d.muzzle_scale_x = static_cast<float>(ReadBeI16(resource_data, 0x88)) * 0.01F;
  d.muzzle_scale_y = static_cast<float>(ReadBeI16(resource_data, 0x8a)) * 0.01F;
  return d;
}

// Ghidra 0x00428340 Ship_UpdateVisualState, destruction slice. See the header
// for scope notes. Constants decoded from data: g_cloak_fade_passive_decay
// (0x00575318) = 1.0 tick, destroyed-finale threshold DAT_0057531c = 2.0,
// player death-timer scale _DAT_00575378 = 3.0, blast radius scale
// _DAT_00575380/_DAT_00575388 = 1.4/3.125, blast damage scale
// _DAT_00575390/_DAT_00575398 = 1.275/2.875.
void NovaShip_TickDestroyedShipVisualState(GameState &state,
                                           Ship &ship,
                                           float elapsed_ticks) {
  (void)elapsed_ticks;
  if (!ship.is_active) {
    return;
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  // Ship_HandleShip's validation prologue deactivates hulls outside the class
  // range or with the -9999 nonexistent sentinel before the visual scope.
  if (cls == nullptr || cls->tech_level == kShipClassNonexistentTechLevel) {
    return;
  }
  if (!NovaAiShip_IsDestroyed(ship)) {
    return;
  }
  if (ship.death_timer_active <= 0.0F) {
    float reseed = static_cast<float>(cls->death_delay_frames);
    if (ship.ship_instance_id == 0) {
      reseed *= 3.0F;
    }
    ship.death_timer_active = reseed;
    if (reseed <= 0.0F) {
      // Port divergence: a zero DeathDelay hull would linger forever in the
      // original (reseed to 0 keeps the finale from ever firing); the port's
      // hit path already treats those as immediate destructions.
      NovaShip_RunShipDestructionFinale(state, ship);
    }
    return;
  }
  if (ship.death_timer_active > 2.0F) {
    // Debris-puff window (2.0 < timer, roll 1-in-1/2/4/8 by the 20/40/60
    // frame thresholds): visual-only, owned by the SDL view.
    return;
  }
  NovaShip_RunShipDestructionFinale(state, ship);
}

void NovaShip_RunShipDestructionFinale(GameState &state, Ship &ship) {
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  const std::int16_t self_slot = ship.ship_instance_id;

  // Hull blast: hull mass drives the splash radius and damage; capability
  // flags 0x400 hulls (mass-less/damped) do not blast. Ships within the
  // per-axis radius take the damage as a disable restriction-checking hit (no
  // aggro; the original passes force_armor_only 1, transition check 1). The
  // original has no same-system gate here - quirk preserved, matching the
  // weapon splash path.
  if (cls != nullptr && (cls->capability_flags & 0x0400U) == 0U &&
      ship.pers_def_slot != 0x3ff) {
    const std::int16_t mass = cls->mass_tons;
    std::int16_t blast_radius = 0;
    std::int16_t blast_damage = 0;
    if (mass >= 100) {
      blast_radius = static_cast<std::int16_t>(
          std::llround(static_cast<float>(mass) * 1.4F + 3.125F));
      blast_damage = static_cast<std::int16_t>(
          std::llround(static_cast<float>(mass) * 1.275F + 2.875F));
    }
    if (blast_radius > 0) {
      for (std::int16_t slot = 0;
           slot < static_cast<std::int16_t>(GameState::kMaxShips);
           ++slot) {
        if (slot == self_slot) {
          continue;
        }
        Ship &victim = state.ShipAt(static_cast<std::size_t>(slot));
        if (!victim.is_active) {
          continue;
        }
        if (std::fabs(victim.pos_x - ship.pos_x) > blast_radius ||
            std::fabs(victim.pos_y - ship.pos_y) > blast_radius) {
          continue;
        }
        ResolveShipHitFromWeapon(state,
                                 slot,
                                 victim,
                                 ship.pos_x,
                                 ship.pos_y,
                                 /*impact_impulse=*/0,
                                 blast_damage,
                                 blast_damage,
                                 /*attacker_ship_slot=*/self_slot,
                                 /*allow_aggro_updates=*/false,
                                 /*suppress_retarget_logic=*/false,
                                 /*force_armor_only=*/true,
                                 /*bypass_shields=*/false,
                                 /*player_aggro_delta=*/0,
                                 /*check_fire_restriction_transition=*/true);
      }
    }
  }

  // Mission DESTRUCTION bookkeeping (non-player mission ships only).
  const std::int16_t fleet_slot = ship.mission_fleet_slot;
  if (fleet_slot != -1 && self_slot != 0) {
    const auto slot_idx = static_cast<std::size_t>(fleet_slot);
    MissionRuntimeFlags &runtime = state.active_mission_runtime_flags[slot_idx];
    ActiveMission &mission = state.active_missions[slot_idx];
    const std::int16_t spawn_behavior = mission.spawn_behavior;
    const bool quick_fail_shape =
        spawn_behavior == 1 || spawn_behavior == 3 ||
        ((spawn_behavior == 2 || spawn_behavior == 5) &&
         ship.boarded_target_latch == 0);
    if (runtime.is_active && mission.goal_counter_a == 0 && quick_fail_shape &&
        !runtime.is_failed &&
        (runtime.flags_primary_at_accept & 0x0400U) == 0U) {
      state.pending_ui_sounds.push_back(GameState::PendingUiSound{1, 1});
      if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x11c)) {
        NovaHud_ShowOverlayMessage(state,
                                   *text,
                                   /*duration_frames=*/std::uint64_t{0xf0});
      }
      Mission_FailMissionSlotQuick(
          state, fleet_slot, static_cast<std::uint32_t>(SDL_GetTicks()));
    }
    mission.goal_counter_a =
        static_cast<std::int16_t>(mission.goal_counter_a + 1);
    if (mission.target_ship_count > 0) {
      mission.target_ship_count =
          static_cast<std::int16_t>(mission.target_ship_count - 1);
    }
  }

  // TODO(decomp) skipped: the escort cargo-return arm (destroyed escort with
  // ai_target_ship_slot 0 / behavior 6 / default AI < 3 runs
  // Outfit_TransferCargoAndJunkToEscortByRatio 0x00469810 plus the weapon-bank
  // pool reconciliation), and the finale audio de-registrations.

  // Personality deactivation: the wreck's personality def leaves the ambient
  // spawn pool (1-in-8 chance for the 0x3fe forced sentinel; otherwise unless
  // Flags 0x0002 keeps it available).
  if (ship.pers_def_slot != -1 &&
      ship.pers_def_slot <
          static_cast<std::int16_t>(state.scenario.pers_defs.size())) {
    PersDef &def =
        state.scenario.pers_defs[static_cast<std::size_t>(ship.pers_def_slot)];
    if (ship.pers_def_slot == 0x3fe) {
      std::uniform_int_distribution<std::int32_t> roll{0, 7};
      if (roll(state.rng) == 0) {
        def.present = false;
      }
    } else if ((def.flags_primary & 0x0002U) == 0U) {
      def.present = false;
    }
  }

  // TODO(decomp) skipped: Shot_SpawnAreaImpactEffects for the finale effect
  // (sh¥8an Explode2); the SDL view spawns it from the destruction timer.

  // Ship_UpdateVisualState tail: the hull deactivates with its target cleared.
  ship.is_active = false;
  ship.ai_target_ship_slot = -1;
}

} // namespace game
