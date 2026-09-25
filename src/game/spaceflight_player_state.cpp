#include "spaceflight.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "boarding_plunder.hpp"
#include "collision.hpp"
#include "compatibility.hpp"
#include "docked_dialog.hpp"
#include "escort_commands.hpp"
#include "flight_automation.hpp"
#include "frame_timing.hpp"
#include "game_state.hpp"
#include "hud_overlay.hpp"
#include "impact_effects.hpp"
#include "mission.hpp"
#include "mission_script.hpp"
#include "nova_random.hpp"
#include "outfit.hpp"
#include "pilot_file.hpp"
#include "ship_ai.hpp"
#include "ship_spawn.hpp"
#include "spaceflight_internal.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <string>

namespace game {

constexpr std::int16_t kAutoRepairSoundTransitionIndex = 4;

// --- PlayerTick_StatusAndOutfitEvents constants (Ghidra globals) -----------
// g_fire_restricted_velocity_damp (0x00575570, double): per-frame velocity
// damping while disabled (disabled).
constexpr float kPlayerFireRestrictedVelocityDamp = 0.995F;
// Player recently-hit timer (g_player_recently_hit_timer, DAT_0073549c):
// armed to 300 ticks when the player takes a hit, decays one tick per frame
// while at or above g_hyperspace_progress_onset_threshold (0x575540, 0.0);
// gates the disabled auto-repair pass until the post-hit regen-suppression
// window expires.
constexpr float kRecentlyHitRegenCutoff = 0.0F;
// k_one_f32 (0x0057555c, float): death-timer countdown step per original
// spaceflight call. The NPC arm of Ship_HandleShip
// (0x00433050) uses the equal-valued k_unit_f32 (0x00575318) for the same
// step. The port time-adjusts both against the original loop's
// 21 ms minimum frame duration.
constexpr float kJumpTurnaroundTurnRateAddend = 1.0F;
// DAT_00575558 (0x00575558, read as a float; bytes 00 00 70 c3 = -240.0): the
// inactive-player death-timer floor. After Ship_UpdateVisualState's finale
// deactivates the hull the timer keeps draining from ~2.0 down to -240 at
// g_jump_turnaround_turn_rate_addend (1.0) per frame before the core latches
// DAT_00596d38 -- the post-explosion window in which the player watches the
// wreck's effect pool finish. Ghidra types this as a byte; the FCOMP at
// 0x0044af17 reads the 4-byte float.
constexpr float kPlayerDeathInactiveTimerFloor = -240.0F;
// Disabled auto-repair armor restore: max_armor * fraction + addend.
// g_auto_repair_armor_fraction (DAT_00575588, double 1/3) is the standard
// rate, g_auto_repair_armor_fraction_0x10 (DAT_00575578, double 0.1) the Ship
// Flags 0x10 variant, g_armor_state_addend (DAT_00575580, double 1.0) the
// shared addend. The +1 lifts the hull just above the disable threshold so
// the ship un-disables after the repair.
constexpr float kAutoRepairArmorFraction = 1.0F / 3.0F;
constexpr float kAutoRepairArmorFractionFlag0x10 = 0.1F;
constexpr float kAutoRepairArmorAddend = 1.0F;
// Bomb-detonation self-damage roll: max_armor * 0.5 + 1.0
// (g_bomb_damage_armor_fraction DAT_00575598 / g_armor_state_addend
// DAT_00575580), drawn via Random(roll) and added to max_armor.
constexpr float kBombDamageArmorFraction = 0.5F;
constexpr float kBombDamageArmorAddend = 1.0F;
// g_bomb_detonation_timer floor sentinel (DAT_00575538) and reroll ceiling
// (0x0044da75 rolls Random(100)).
constexpr float kBombDetonationTimerFloor = 0.0F;
constexpr std::int16_t kBombDetonationRerollMax = 100;
// g_bomb_detonation_interval_frames (0x00575590, float): the countdown window
// in 30 Hz reference frames before a carried bomb detonates.
constexpr float kBombDetonationIntervalFrames = 300.0F;
// Outfit ModType codes scanned in the four mod-type slots (decompile offsets
// name-0x26..-0x20, 0x37c-byte def stride).
constexpr std::int16_t kBombEscapePodModType = 0x2F; // self-destruct escape pod
constexpr std::int16_t kBombWeaponModType = 0x32;    // carried bomb
constexpr std::int16_t kAutoRepairOutfitModType = 0x31; // repair system
// STR# 0x7d2 entries used by the block.
constexpr std::uint16_t kStringListFlightText = 0x7D2;
constexpr std::uint16_t kStrAutoRepairEngaged =
    0x25; // "repair systems engaged"
constexpr std::uint16_t kStrEscapePodDeployed = 0x26;
constexpr std::uint16_t kStrBombYou = 0x27; // "You " prefix
constexpr std::uint16_t kStrBombDetonatedSingular = 0x28;
constexpr std::uint16_t kStrBombDetonatedPlural = 0x29;
// HUD overlay durations (simulation ticks).
constexpr std::uint64_t kOverlayDurationAutoRepair = 250; // 0xfa
constexpr std::uint64_t kOverlayDurationBomb = 400;       // 0x190
constexpr std::uint64_t kOverlayDurationEscapePod = 360;  // 0x168
// Bomb outfit defs carry a 0x80-biased area-impact effect id in ModVal.
constexpr std::int16_t kBombEffectIdBias = 0x80;
// Frame_ShouldTriggerAutoRepairTick reroll: _DAT_00575830 = 500.0 (the range
// base) and _FLOAT_005757d8 = 0.0 (the low-tick-scale gate).
constexpr float kFrameScaleAutoRepairRerollGate = 0.0F;
constexpr int kAutoRepairFrameRerollRange = 500;

// Ghidra PlayerTick_StatusAndOutfitEvents (internal label of
// Ship_HandlePlayerShipCore 0x0044aa70, block 0x0044b240..0x0044b7c4 plus the
// carried-bomb tails at 0x0044da75/0x0044daa0 and the death-bookkeeping
// prologue of the parent).
// ---------------------------------------------------------------------------

namespace {

// @port 0x0043ADB0 10% gameplay
// @port 0x0046E120 25% gameplay
// @port 0x0046E2F0 55% gameplay
// Ghidra 0x0043adb0 Stellar_TickStellarGravityPull (player-side port; NPC
// iteration/crash consequences not reconstructed). The protected_from_gravity
// check below ports Stellar_ShipHasGravityShielding (0x0046e120) for the
// player via owned outfit effects.
// Ghidra 0x0046e2f0 Ship_AccelerateShipTowardPoint. Stellar_TickStellar-
// GravityPull passes gravity * frame_time as max_accel, divides by the squared
// separation (with a tiny-distance floor), then adds the polar result to the
// ship velocity. The player is exempt with either opcode 38 (inertial
// dampener, used by Outfit_ShipIsInertialess) or opcode 41 (gravity
// resistance, added by Stellar_ShipHasGravityShielding).
bool Ship_AccelerateShipTowardPoint(GameState &state, float elapsed_ticks) {
  const System *const system = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (system == nullptr || elapsed_ticks <= 0.0F) {
    return false;
  }

  bool gravity_present = false;
  const bool protected_from_gravity =
      Outfit_HasOwnedEffect(state, OutfitEffect::kInertialDampener) ||
      Outfit_HasOwnedEffect(state, OutfitEffect::kGravityResist);
  for (const std::int16_t stellar_id : system->nav_defs) {
    const Stellar *const stellar = state.scenario.Stellar(stellar_id);
    if (stellar == nullptr || stellar->gravity == 0) {
      continue;
    }
    gravity_present = true;
    if (protected_from_gravity) {
      continue;
    }
    const float dx = static_cast<float>(stellar->pos_x) - state.player.pos_x;
    const float dy = static_cast<float>(stellar->pos_y) - state.player.pos_y;
    const float distance_sq = std::max(dx * dx + dy * dy, 1.0F);
    const float distance = std::sqrt(distance_sq);
    const float accel =
        static_cast<float>(stellar->gravity) * elapsed_ticks / distance_sq;
    state.player.vel_x += dx / distance * accel;
    state.player.vel_y += dy / distance * accel;
  }
  return gravity_present;
}

// Outfit-def scan shared by Frame_ShouldTriggerAutoRepairTick (0x0046e540) and
// the carried-bomb detonation scans: the original walks the four mod-type
// words of every owned outfit (decompile `name - 0x26 + i*2`, 0x37c-byte def
// stride) and reports the first matching def.
const Outfit *FindOutfitWithModType(const GameState &state,
                                    std::size_t outfit_index,
                                    std::int16_t mod_type) {
  if (outfit_index >= state.scenario.outfits.size()) {
    return nullptr;
  }
  const Outfit &def = state.scenario.outfits[outfit_index];
  if (def.mod_type == mod_type) {
    return &def;
  }
  for (const std::int16_t alt : def.alt_mod_types) {
    if (alt == mod_type) {
      return &def;
    }
  }
  return nullptr;
}

// Jump-arrival call site note: the in-flight jump-arrival block of
// Ship_HandlePlayerShipCore (PlayerTick_SystemTransitionAndArrival
// 0x0044f660) runs Frame_JitterPlayerStatModifiers + Frame_RerollPlayerStat-
// Modifiers at 0x0044fa10/0x0044fa15, right before its
// Mission_RefreshActiveMissionSpawnState call. PlayerTick_JumpArrivalBlock in
// spaceflight.cpp now invokes both there. The launch tail invokes the jitter
// only.

// Ghidra PlayerTick_StatusAndOutfitEvents sub-branch 0x0044ab50: on death with
// a carried bomb, open the escape-pod selection dialog (first owned outfit
// with ModType 0x2f / ModVal > 0).
void RunDeathEscapePodScan(GameState &state) {
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    if (state.inventory.outfit_owned_count[idx] <= 0) {
      continue;
    }
    const Outfit *pod =
        FindOutfitWithModType(state, idx, kBombEscapePodModType);
    if (pod == nullptr || pod->mod_val <= 0) {
      continue;
    }
    // TODO(decomp(0x0044ab6c)) skipped: NovaUi_HideTravelSelectionSprite,
    // NovaUi_RedrawGameplayViewportAndRadar, NovaPlatform_EnsureCursorVisible,
    // Ui_LoadSelectionDialogResource / Ui_RunTravelSelectionDialog and
    // NovaUi_MarkTravelAndStatusPanelsDirty are presentation passes of the
    // interactive selection-dialog shell, not reconstructed yet.
    NovaLog::Info("escape pod dialog requested (outfit {})", pod->name);
    break;
  }
}

// Message composer for the bomb/escape-pod overlays: STR# 0x7d2 entry 0x27
// ("You ") + outfit LC name/plural + entry 0x28/0x29. The original copies the
// name out of stride-0x100 Pascal-string arrays indexed by outfit id; the
// clean-room uses the decoded LC fields.
std::string ComposeBombOverlay(const GameState &state,
                               std::size_t outfit_index,
                               bool plural) {
  std::string text =
      NovaHud_LoadStringEntry(kStringListFlightText, kStrBombYou).value_or("") +
      " ";
  const Outfit &def = state.scenario.outfits[outfit_index];
  text += plural ? def.lc_plural : def.lc_name;
  text += " ";
  text += NovaHud_LoadStringEntry(kStringListFlightText,
                                  plural ? kStrBombDetonatedPlural
                                         : kStrBombDetonatedSingular)
              .value_or("");
  return text;
}

// Ghidra 0x0044daa0 escape-pod bomb variant (carried bomb outfit class 1):
// kills shields/armor immediately, stops the hyperspace audio, shows the
// deployment overlay, and returns to the menu shell (DAT_007354a5).
void DetonateEscapePodBomb(GameState &state) {
  PlayerShip &p = state.player;
  p.armor_points = -1.0F;
  p.shield_points = -1.0F;
  p.ai_station_hold_timer = -1.0F;

  std::string text;
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    const auto owned = state.inventory.outfit_owned_count[idx];
    if (owned <= 0) {
      continue;
    }
    if (FindOutfitWithModType(state, idx, kBombEscapePodModType) == nullptr) {
      continue;
    }
    // Singular/plural choice reproduces the original's name-flag ladder: the
    // plural name needs owned >= 2 and a non-empty plural, the singular needs
    // owned == 1 and a non-empty name; otherwise no name message is shown.
    const Outfit &pod_def = state.scenario.outfits[idx];
    const bool has_plural = !pod_def.lc_plural.empty();
    const bool has_singular = !pod_def.lc_name.empty();
    if (owned < 2 || !has_plural) {
      if (owned != 1 || !has_singular) {
        break; // deployment only, no composed name message
      }
    }
    text = ComposeBombOverlay(state, idx, owned >= 2);
    break;
  }
  if (text.empty()) {
    // DAT_007354a4 can hold a custom latched message; the fallback is STR#
    // 0x7d2 entry 0x26. The custom-message source is not reconstructed yet
    // (TODO(decomp)).
    text = NovaHud_LoadStringEntry(kStringListFlightText, kStrEscapePodDeployed)
               .value_or("");
  }
  NovaHud_ShowOverlayMessage(
      state, text, 0xe0, 0xe0, 0xe0, kOverlayDurationEscapePod);

  // NovaAudio_UnregisterCallbacks on the warp-up / warp-up-x2 handles (the
  // port's pending one-shots are drained by the loop; clear the latches).
  state.warp_up_sound_pending = false;
  state.warp_out_sound_pending = false;
  state.return_to_menu_pending = true;
}

// Ghidra 0x0044b446 bomb detonation: message, impact effect from the def's
// 0x80-biased ModVal, outfit removal, and an armor-piercing self-damage roll
// of max_armor * fraction + addend (bypass_shields, force_armor_only,
// suppress_retarget, no aggro).
void DetonateCarriedBomb(GameState &state) {
  PlayerShip &p = state.player;
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    if (state.inventory.outfit_owned_count[idx] <= 0) {
      continue;
    }
    const Outfit *bomb = FindOutfitWithModType(state, idx, kBombWeaponModType);
    if (bomb == nullptr) {
      continue;
    }
    NovaHud_ShowOverlayMessage(
        state,
        ComposeBombOverlay(
            state, idx, state.inventory.outfit_owned_count[idx] >= 2),
        0xe0,
        0xe0,
        0xe0,
        kOverlayDurationBomb);
    if (bomb->mod_val >= kBombEffectIdBias &&
        bomb->mod_val < kBombEffectIdBias + 0x40) {
      NovaEffects_SpawnAreaImpact(
          state,
          p.pos_x,
          p.pos_y,
          static_cast<std::int16_t>(bomb->mod_val - kBombEffectIdBias),
          0,
          true);
    }
    state.inventory.outfit_owned_count[idx] = 0;

    const ShipClass *cls =
        state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
    const float max_armor = cls ? static_cast<float>(cls->base_armor) : 0.0F;
    const int roll =
        RandomBelow(state,
                    static_cast<int>(max_armor * kBombDamageArmorFraction +
                                     kBombDamageArmorAddend));
    Ship_ApplyDamageToShip(state,
                           0,
                           p,
                           p.pos_x,
                           p.pos_y,
                           0,
                           static_cast<std::int16_t>(roll + max_armor),
                           0,
                           -1,
                           false,
                           false,
                           true,
                           true,
                           0);
    break;
  }
  // Ghidra g_player_status_panel_dirty + Outfit_RecomputeOutfitDerivedState:
  // the outfit pool changed, so derived stats must be recomputed.
  state.InvalidateDerivedStatCaches();
  state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
  state.stat_cache_valid = true;
}

} // namespace

// @port 0x0046E540 95% cadence
// Ghidra Frame_ShouldTriggerAutoRepairTick (0x0046e540): random gate (1-in-N,
// N = 500 with the low-tick-scale reroll; 500 / frame tick scale otherwise),
// a destroyed-ship veto, and the repair outfit's ModType 0x31 presence. The
// player scans owned outfits, an NPC scans its class default loadout; the
// original additionally checks fire-restriction, and both callers gate on it.
bool Frame_ShouldTriggerAutoRepairTick(GameState &state, const Ship &ship) {
  const int roll_range =
      state.last_frame_tick_scale <= kFrameScaleAutoRepairRerollGate
          ? kAutoRepairFrameRerollRange
          : static_cast<int>(static_cast<float>(kAutoRepairFrameRerollRange) /
                             state.last_frame_tick_scale);
  if (RandomBelow(state, roll_range) != 0) {
    return false;
  }
  if (NovaAiShip_IsDestroyed(ship)) {
    return false;
  }
  const auto has_repair_slot = [](const Outfit &outfit) {
    if (outfit.mod_type == kAutoRepairOutfitModType) {
      return true;
    }
    for (const std::int16_t alt : outfit.alt_mod_types) {
      if (alt == kAutoRepairOutfitModType) {
        return true;
      }
    }
    return false;
  };
  if (ship.ship_instance_id == 0) {
    for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                              idx < state.inventory.outfit_owned_count.size();
         ++idx) {
      if (state.inventory.outfit_owned_count[idx] > 0 &&
          FindOutfitWithModType(state, idx, kAutoRepairOutfitModType) !=
              nullptr) {
        return true;
      }
    }
    return false;
  }
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls == nullptr) {
    return false;
  }
  for (std::size_t slot = 0; slot < cls->default_outfit_ids.size(); ++slot) {
    if (cls->default_outfit_counts[slot] <= 0) {
      continue;
    }
    const Outfit *outfit = state.scenario.Outfit(cls->default_outfit_ids[slot]);
    if (outfit != nullptr && has_repair_slot(*outfit)) {
      return true;
    }
  }
  return false;
}

// Jump-arrival call-site note for the pair below: the in-flight jump-arrival
// block of Ship_HandlePlayerShipCore (PlayerTick_SystemTransitionAndArrival
// 0x0044f660) runs both functions at 0x0044fa10/0x0044fa15, before its
// Mission_RefreshActiveMissionSpawnState call. PlayerTick_JumpArrivalBlock in
// spaceflight.cpp now invokes both there; the launch tail invokes the jitter
// only.

// @port 0x00431480 100%
// Ghidra 0x00431480 Frame_JitterPlayerStatModifiers. The two-step jitter and
// the [85,115] clamp are branch-faithful (NovaRandom_Range(3): 0 -> -1,
// 1 -> +1, 2 -> unchanged).
void NovaFrame_JitterPlayerStatModifiers(GameState &state) {
  for (std::size_t i = 0; i < 2; ++i) {
    const int roll = RandomBelow(state, 3);
    if (roll == 0) {
      --state.player_stat_modifier_pct[i];
    } else if (roll == 1) {
      ++state.player_stat_modifier_pct[i];
    }
    state.player_stat_modifier_pct[i] =
        std::clamp<std::int16_t>(state.player_stat_modifier_pct[i], 0x55, 0x73);
  }
}

// @port 0x00431500 100%
// Ghidra 0x00431500 Frame_RerollPlayerStatModifiers: rand(0x15) + 0x5a, i.e.
// [90, 110] percent.
void NovaFrame_RerollPlayerStatModifiers(GameState &state) {
  for (std::size_t i = 2; i < 4; ++i) {
    state.player_stat_modifier_pct[i] =
        static_cast<std::int16_t>(RandomBelow(state, 0x15) + 0x5a);
  }
}

// Ghidra PlayerTick_TimedActionTransition support: the eject/escape-pod
// transform arm (Ship_HandlePlayerShipCore 0x004510b9..0x00453910), the
// Ship_ResetPlayerShipState (0x004b3350) respawn reset, and the
// Stellar_FindValidRespawnStellar flood (0x00467710 / 0x004677a0).
// ---------------------------------------------------------------------------
namespace {

// Bible oütf ModType entries driving the eject gate.
constexpr std::int16_t kAutoEjectOutfitModType =
    0x14; // ModType 20 "auto-eject"
constexpr std::int16_t kEscapePodOutfitModType =
    0x0b; // ModType 11 "escape pod"
// Launch-bay weapons: weapon_mode_code 99; the carried craft class is
// ammo_type - 0x80 and must set shïp Flags 0x8000 (escape ship type).
constexpr std::int16_t kBayWeaponModeCode = 99;
constexpr std::uint16_t kEscapeShipClassFlag = 0x8000;
// The escape-pod ship class (kEscapePodShipClassIndex) is shared from
// game_state.hpp; the eject transform and ship_visual.cpp both key off it.
// The escape-pod flight arms the blocking timed action for 0x15e ticks.
constexpr std::int16_t kEscapePodTimedActionTicks = 0x15e;
// Eject gate: the death presentation must be at least half spent
// (death_timer <= ShipClass.death_delay_frames * g_bomb_damage_armor_fraction
// DAT_00575598 = 0.5, the same global the bomb self-damage roll uses) or be
// within g_hyperspace_engage_hold_30hz (0x5755a8, 30) ticks of ending.
constexpr float kEjectDeathDelayScale = kBombDamageArmorFraction;
constexpr float kEjectArmHoldTicks = 30.0F;
// Respawn catch-up: rand(30) + 15 Mission_TickDailyWorldUpdate passes.
constexpr std::int16_t kRespawnDailyUpdateRange = 30;
constexpr std::int16_t kRespawnDailyUpdateBase = 15;
// STR# 0x7d2 entry 0x34: "You abandon your ship for" (fighter variant).
constexpr std::uint16_t kStrAbandonShipFor = 0x34;
// The 0x100 player weapon banks store their live counter at slot 0 of a
// 100-int16 stride.
constexpr std::size_t kPlayerBankStride = 100;

// @port 0x00464700 100%
// Ghidra Outfit_HasAutoEjectOutfit (0x00464700): any owned outfit with
// ModType 0x14 (Bible "auto-eject", which requires an escape pod to work).
bool Outfit_HasAutoEjectOutfit(const GameState &state) {
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    if (state.inventory.outfit_owned_count[idx] > 0 &&
        FindOutfitWithModType(state, idx, kAutoEjectOutfitModType) != nullptr) {
      return true;
    }
  }
  return false;
}

// Ghidra Weapon_HasLaunchBayWeapon (0x00464520), player specialization:
// the canonical Ship-taking port lives in weapon.cpp
// (NovaWeapon_HasLaunchBayWeapon); the player's banks are the GameState
// strided arrays, reached via ships[0].
bool Weapon_HasPlayerLaunchBayWeapon(const GameState &state) {
  return NovaWeapon_HasLaunchBayWeapon(state, state.player);
}

// @port 0x00464590 100%
// Ghidra ShipClass_FindLaunchBayShipClassId (0x00464590): the zero-based class
// index of the first ejectable bay fighter, -1 when none.
std::int16_t Weapon_FindLaunchBayShipClassIndex(const GameState &state) {
  for (std::size_t bank = 0; bank < 0x100; ++bank) {
    const Weapon *def =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (def == nullptr || def->weapon_mode_code != kBayWeaponModeCode) {
      continue;
    }
    if (state.weapon_count_by_class[bank * kPlayerBankStride] < 1) {
      continue;
    }
    if (def->ammo_type < 0x80) {
      continue;
    }
    const ShipClass *carried = state.scenario.Ship(def->ammo_type);
    if (carried != nullptr &&
        (carried->capability_flags & kEscapeShipClassFlag) != 0) {
      return static_cast<std::int16_t>(def->ammo_type - 0x80);
    }
  }
  return -1;
}

// @port 0x004644a0 100%
// Ghidra Outfit_HasSpecialMovementOutfitOrLaunchBay (0x004644a0, renamed
// Outfit_HasEscapePodOrLaunchBay): an owned escape-pod outfit (ModType 0xb)
// or an ejectable launch bay.
bool Outfit_HasEscapePodOrLaunchBay(const GameState &state) {
  for (std::size_t idx = 0; idx < state.scenario.outfits.size() &&
                            idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    if (state.inventory.outfit_owned_count[idx] > 0 &&
        FindOutfitWithModType(state, idx, kEscapePodOutfitModType) != nullptr) {
      return true;
    }
  }
  return Weapon_HasPlayerLaunchBayWeapon(state);
}

// Zeroes the live (slot 0) counter of every player weapon bank and the per-
// bank cooldown/burst state before a stock reseed (the original sweeps all
// 0x100 banks in the eject/respawn paths).
void ZeroPlayerWeaponBanks(GameState &state) {
  for (std::size_t bank = 0; bank < 0x100; ++bank) {
    state.weapon_count_by_class[bank * kPlayerBankStride] = 0;
    state.weapon_secondary_count_by_class[bank * kPlayerBankStride] = 0;
  }
  state.weapon_bank_cooldown.fill(0.0F);
  state.weapon_bank_burst_counter.fill(0);
  state.active_shots.clear();
}

// Drops non-persistent outfits (OutfitDef field_0x378 / Flags 0x0004 survive
// a ship replacement).
void ClearNonPersistentOutfits(GameState &state) {
  for (std::size_t idx = 0; idx < state.inventory.outfit_owned_count.size();
       ++idx) {
    const Outfit *def = idx < state.scenario.outfits.size()
                            ? &state.scenario.outfits[idx]
                            : nullptr;
    if (def == nullptr || !def->persistent_on_ship_swap) {
      state.inventory.outfit_owned_count[idx] = 0;
    }
  }
}

// Ghidra Ship_ResetPlayerShipState (0x004b3350), param_1 == 0 (the respawn
// call). Fresh kinematics, the hardcoded fresh class 0, recomputed meters and
// cleared targeting/AI/travel state.
void RespawnResetPlayerShipState(GameState &state) {
  PlayerShip &p = state.player;
  p.pos_x = 50.0F;
  p.pos_y = 50.0F;
  p.vel_x = 0.0F;
  p.vel_y = 0.0F;
  p.speed = 0.0F;
  p.heading = 0.0F;
  p.ship_class_id = 0; // the reset always lands the player in class 0 (id 0x80)
  p.is_active = true;
  p.cloak_transition_latch = 0;
  p.cloak_fade_progress = 0.0F;
  p.travel_transfer_mode = -1;
  p.faction_or_government_id = -1;
  p.ai_behavior_code = -1;
  p.current_system_id = 0; // overwritten by the emergency destination

  // Meters are computed BEFORE the outfit counts are cleared (original
  // order), so the pre-death outfit bonuses still apply to class 0 here.
  state.InvalidateDerivedStatCaches();
  const PlayerEffectiveStats eff = Outfit_ComputePlayerEffectiveStats(state);
  p.shield_points = eff.max_shield_points;
  p.armor_points = eff.max_armor_points;
  p.fuel_points = eff.fuel_capacity;
  state.cached_stats = eff;
  state.stat_cache_valid = true;

  p.primary_target_ship_slot = -1;
  p.ai_secondary_target_slot = -1;
  p.active_weapon_bank_slot = -1;
  p.squad_leader_ship_slot = -1;
  p.mission_fleet_slot = -1;
  p.pers_def_slot = -1;
  p.ai_control_mode = 0;
  p.ai_state_code = 0;
  p.death_timer_active = -999.0F;
  p.death_timer_seeded = false;
  p.destruction_finale_triggered = false;
  p.destruction_visual_triggered = false;
  p.destruction_raw_tick_accumulator = 0.0F;
  p.ionization_points = 0.0F;
  // TODO(decomp(0x004b3350)) skipped: field_0xb0/field_0x60 latch words,
  // weapon_exit_animation_phase, alternate_sprite_cycle_index and
  // last_weapon_fire_time_ms - not modelled on Ship.
  p.sprite_animation_cycle_index = 0;
  p.waypoint_arrival_marker_a = 1;
  p.waypoint_arrival_marker_b = 0;
  p.turn_bank_animation_phase = 0.0F;
  p.ai_turn_bias_dir = 0;
  p.shield_bubble_flash_intensity = 0.0F;
  p.player_aggro_accumulator = 0.0F;
  p.sprite_animation_timer = 0.0F;
  p.skill_variance_scale = 1.0F;
  p.ai_station_hold_timer = 0.0F;
  p.jump_destination_system_id = -1;
  p.engine_glow_level = 0;
  p.velocity_match_target_ship_slot = -1;

  ZeroPlayerWeaponBanks(state);
  NovaWeapon_SeedBanksFromShipStock(state, p.ship_class_id);

  // The plotted starmap route (DAT_00735404, 0x20 slots).
  state.travel.starmap_route.fill(-1);

  ClearNonPersistentOutfits(state);
  state.inventory.cargo_bins.fill(0);
  state.inventory.junk_counts.fill(0);

  // Ghidra 0x004b39f2..0x004b3a05 seeds this travel-dialog selector here.
  state.travel.interaction_action_index_b = RandomBelow(state, 0x800);
  // TODO(decomp(0x004b3350)) skipped: DAT_007353f4, g_last_system_for_ambient_
  // rolls, _g_playerSelfDestructCountdown, bribe_random_latch, DAT_00596d32/33
  // and g_travel_countdown - not modelled yet.
  state.travel.selected_stellar_id = -1;
  state.travel.engage_timer = -1;
  state.travel.travel_hint_state = 0x7fff; // hint latch (0x004b3a3b)
  state.player_threat_active = false;
  state.player_threat_active_prev = false;
  state.pending_red_alert = false;
  p.timed_action_counter = -1;

  Mission_RerollOfferingRolls(state);

  // Impact-effect instance timers back to the inactive sentinel.
  for (auto &effect : state.impact_effect_instances) {
    effect.anim_time = -1.0F;
    effect.delay_timer = 0.0F;
  }
  // TODO(decomp(0x004b3350)) skipped: reset the unimplemented 64-entry weapon
  // smoke-puff pool used by Shot_SpawnWeaponSmokePuff (0x004215d0) and
  // Shot_UpdateWeaponSmokePuffs (0x0042c660). No stock Nova weapon enables it;
  // it remains relevant to plug-ins using Flags 0x0200/0x0400 and SmokeSet.

  // Reroll the per-class licensed availability rolls.
  for (auto &roll : state.ship_class_limit_rolls) {
    roll = static_cast<std::int16_t>(RandomBelow(state, 100) + 1);
  }
  for (auto &roll : state.ship_class_threshold_rolls) {
    roll = static_cast<std::int16_t>(RandomBelow(state, 100) + 1);
  }
}

// @port 0x00467710,0x004677a0 100%
// Ghidra 0x00467710/0x004677a0 Stellar_FindValidRespawnStellar{,Recursive}:
// the escape-pod respawn destination lookup. Depth-first flood over visible,
// discovered neighbour systems for an available, travel-usable, ship-selling
// (travel_flags & 8) stellar whose reputation threshold the containing system
// meets. Returns the stellar resource id, or -1 when no neighbour qualifies.
// The start system's own stellars are never scanned (you always respawn
// elsewhere).
std::int16_t Stellar_FindValidRespawnStellar(GameState &state,
                                             std::int16_t origin_zero_based) {
  const auto &systems = state.scenario.systems;
  std::vector<char> visited(systems.size(), 0);
  if (origin_zero_based < 0 ||
      static_cast<std::size_t>(origin_zero_based) >= systems.size()) {
    return -1;
  }

  auto neighbour_eligible = [&](std::size_t idx) -> bool {
    const System &sys = systems[idx];
    return sys.is_visible && sys.discovered_this_rebuild &&
           sys.discovery_state > 0;
  };
  auto scan_stellars = [&](std::size_t idx) -> std::int16_t {
    const System &sys = systems[idx];
    for (std::int16_t nav : sys.nav_defs) {
      if (nav < 0x80) {
        continue;
      }
      const Stellar *st = state.scenario.Stellar(nav);
      if (st == nullptr || !st->is_available ||
          !NovaTargeting_IsStellarUsableForTravel(*st) ||
          (st->flags & 8) == 0 || st->min_status == 0x7fff) {
        continue;
      }
      const std::int16_t reputation = idx < state.system_reputation.size()
                                          ? state.system_reputation[idx]
                                          : 0;
      if (st->min_status <= reputation || st->min_status == -0x7fff) {
        return nav;
      }
    }
    return -1;
  };

  // Recursive flood, matching the original's two passes per system: scan
  // neighbours for an eligible stellar first, then recurse into them.
  std::function<std::int16_t(std::size_t)> recurse =
      [&](std::size_t idx) -> std::int16_t {
    visited[idx] = 1;
    const System &sys = systems[idx];
    for (std::int16_t link : sys.links) {
      if (link < 0x80) {
        continue;
      }
      const std::size_t next = static_cast<std::size_t>(link - 0x80);
      if (next >= systems.size() || visited[next] != 0 ||
          !neighbour_eligible(next)) {
        continue;
      }
      if (const std::int16_t found = scan_stellars(next); found >= 0x80) {
        return found;
      }
    }
    for (std::int16_t link : sys.links) {
      if (link < 0x80) {
        continue;
      }
      const std::size_t next = static_cast<std::size_t>(link - 0x80);
      if (next >= systems.size() || visited[next] != 0 ||
          !neighbour_eligible(next)) {
        continue;
      }
      if (const std::int16_t found = recurse(next); found >= 0x80) {
        return found;
      }
    }
    return -1;
  };

  visited[static_cast<std::size_t>(origin_zero_based)] = 1;
  return recurse(static_cast<std::size_t>(origin_zero_based));
}

// Ghidra Ship_HandlePlayerShipCore synthetic CFG: eject/escape-pod transition
// 0x00451024 -> 0x00451630. The destroyed player's eject transform spawns the
// derelict wreck of the old
// hull, runs the old class's OnRetire expression (ShipClassDef+0x4e8 <- shp
// payload 0x4cf), then rebuilds the player as either the escape pod (ship id
// 0x37f, arming the 0x15e-tick respawn timed action) or a carried bay fighter
// (Flags 0x8000 class at 50..79% stats, no timed action), relaunches at top
// speed along the current heading and re-seeds loadout/meters.
void RunPlayerEjectTransform(GameState &state) {
  PlayerShip &p = state.player;

  // Derelict wreck of the abandoned hull (Ship_AllocateShipSlotInSystem
  // (current_system, 0)); combat AI re-targets it below.
  const std::int16_t wreck_slot = static_cast<std::int16_t>(
      NovaShip_AllocateShipSlot(state, p.current_system_id, 0));
  if (wreck_slot >= 0) {
    Ship &wreck = state.ShipAt(static_cast<std::size_t>(wreck_slot));
    wreck.ai_behavior_code = -1;
    wreck.pos_x = p.pos_x;
    wreck.pos_y = p.pos_y;
    wreck.vel_x = p.vel_x;
    wreck.vel_y = p.vel_y;
    wreck.heading = p.heading;
    wreck.ship_class_id = p.ship_class_id;
    wreck.death_timer_active = p.death_timer_active;
    wreck.ionization_points = 0.0F;
    // TODO(decomp(0x00451130)) skipped: field_0xb0/field_0x60 latch words,
    // weapon_exit_animation_phase, alternate_sprite_cycle_index,
    // sprite_animation_cycle_index, ai_turn_bias_dir, sprite_animation_timer
    // wreck copies and the sprite-set assignment - partly unmodelled.
    wreck.shield_points = p.shield_points;
    wreck.armor_points = p.armor_points;
    wreck.boarded_target_latch = 1;
  }

  p.death_timer_active = -1.0F;
  p.death_timer_seeded = false;
  p.destruction_finale_triggered = false;
  p.destruction_visual_triggered = false;
  p.destruction_raw_tick_accumulator = 0.0F;
  p.primary_target_ship_slot = -1;
  p.ai_secondary_target_slot = -1;
  p.active_weapon_bank_slot = -1;

  // Old ship class's OnRetire reaction script.
  if (const ShipClass *old_cls = state.scenario.Ship(
          static_cast<std::int16_t>(p.ship_class_id + 0x80))) {
    Mission_ExecuteReactionScript(
        state, old_cls->on_retire_expr, MissionScriptContext{"ship OnRetire"});
  }

  if (!Weapon_HasPlayerLaunchBayWeapon(state)) {
    // Escape pod: full base shields/armor, 350-tick respawn countdown, and
    // every ship targeting the player drops it.
    p.ship_class_id = kEscapePodShipClassIndex;
    if (const ShipClass *pod = state.scenario.Ship(0x37f)) {
      p.shield_points = static_cast<float>(pod->base_shield);
      p.armor_points = static_cast<float>(pod->base_armor);
    } else {
      NovaLog::Todo("escape pod ship class 0x37f not in scenario tables; "
                    "pod stats left unchanged");
    }
    p.timed_action_counter = kEscapePodTimedActionTicks;
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      Ship &ship = state.ShipAt(slot);
      if (ship.is_active && ship.primary_target_ship_slot == 0) {
        ship.primary_target_ship_slot = -1;
      }
    }
  } else {
    // Bay fighter: 50..79% of the carried class's base stats, no respawn
    // timed action, and the "You abandon your ship for <class>." overlay.
    const std::int16_t fighter_index =
        Weapon_FindLaunchBayShipClassIndex(state);
    const ShipClass *fighter =
        fighter_index >= 0 ? state.scenario.Ship(static_cast<std::int16_t>(
                                 fighter_index + 0x80))
                           : nullptr;
    if (fighter == nullptr) {
      NovaLog::Todo("eject: launch bay present but no Flags-0x8000 fighter "
                    "class resolved; staying in the current class");
    } else {
      p.ship_class_id = fighter_index;
      p.shield_points = static_cast<float>((RandomBelow(state, 30) + 50) *
                                           fighter->base_shield) *
                        0.01F;
      p.armor_points = static_cast<float>((RandomBelow(state, 30) + 50) *
                                          fighter->base_armor) *
                       0.01F;
      p.fuel_points = static_cast<float>((RandomBelow(state, 30) + 50) *
                                         fighter->base_fuel) *
                      0.01F;
      p.timed_action_counter = -1;
      std::string text =
          NovaHud_LoadStringEntry(kStringListFlightText, kStrAbandonShipFor)
              .value_or("") +
          " " + fighter->display_name + ".";
      NovaHud_ShowOverlayMessage(
          state, text, 0xe0, 0xe0, 0xe0, kOverlayDurationAutoRepair);
    }
  }

  // Common tail: NPC retarget pass, launch velocity, loadout rebuild.
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    if (!ship.is_active) {
      continue;
    }
    if (!NovaTargeting_IsThreatToPlayerSquad(state, ship)) {
      if (ship.squad_leader_ship_slot == 0 &&
          p.ship_class_id == kEscapePodShipClassIndex) {
        ship.squad_leader_ship_slot = -1;
      }
    } else {
      // Combat AI switches onto the fresh wreck.
      if (ship.primary_target_ship_slot == 0) {
        ship.primary_target_ship_slot = wreck_slot;
      }
      if (ship.ai_secondary_target_slot == 0) {
        ship.ai_secondary_target_slot = wreck_slot;
      }
    }
  }
  // TODO(decomp(0x00453830)) skipped: Sprite_AssignSpriteSet / frame reset
  // (the clean-room sprite layer re-derives visuals from the class id).

  // Relaunch: velocity reset, then a full-speed polar step along the heading
  // (Math_AddPolarVelocity 0x0043b4a0, unclamped).
  p.vel_x = 0.0F;
  p.vel_y = 0.0F;
  state.InvalidateDerivedStatCaches();
  const PlayerEffectiveStats launch_eff =
      Outfit_ComputePlayerEffectiveStats(state);
  state.cached_stats = launch_eff;
  state.stat_cache_valid = true;
  p.vel_x += std::sin(p.heading) * (launch_eff.speed_raw / 100.0F);
  p.vel_y -= std::cos(p.heading) * (launch_eff.speed_raw / 100.0F);

  // Loadout rebuild: non-persistent outfits are lost, weapon banks reseeded
  // from the new class's stock, and the bank/outfit pools reconciled.
  ClearNonPersistentOutfits(state);
  ZeroPlayerWeaponBanks(state);
  NovaWeapon_SeedBanksFromShipStock(state, p.ship_class_id);
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
  if (const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(p.ship_class_id + 0x80))) {
    for (std::size_t i = 0; i < cls->default_outfit_ids.size(); ++i) {
      const std::int16_t id = cls->default_outfit_ids[i];
      if (id >= 0x80 && cls->default_outfit_counts[i] > 0 &&
          static_cast<std::size_t>(id - 0x80) <
              state.inventory.outfit_owned_count.size()) {
        state.inventory.outfit_owned_count[id - 0x80] =
            static_cast<std::int16_t>(
                state.inventory.outfit_owned_count[id - 0x80] +
                cls->default_outfit_counts[i]);
      }
    }
  }

  // Launch cue: NovaAudio_QueueCenteredSound(DAT_00591a80, 0x32, ...).
  // TODO(decomp(0x00453810)) skipped: the DAT_00591a80 sound handle is not
  // mapped to the port's transition/impact tables yet.
  state.warp_up_sound_pending = false;
  state.warp_out_sound_pending = false;
  p.ai_station_hold_timer = 0.0F;
  p.death_timer_active = -1.0F;
  // The original's timed-action dispatch has already run when the eject
  // transform arms the countdown; suppress the port's timed-action tick for
  // this frame (consumed by the spaceflight loop).
  state.timed_action_suppress_this_frame = true;
}

// Ghidra 0x00451024 eject block. The original runs it when the player is
// disabled OR destroyed. A destroyed hull with an owned auto-eject outfit
// ejects automatically once the death presentation is at least half spent (or
// within 30 ticks of ending); otherwise the arm-modifier + binding pair
// (0x38/0x6f + slot 0x11, Alt+X) is the manual request, which also works on a
// disabled (not destroyed) hull. An owned escape-pod outfit or ejectable launch
// bay is then required. Returns true when the transform ran.
bool TryRunPlayerEjectTransform(GameState &state,
                                bool eject_command,
                                bool destroyed,
                                bool disabled) {
  if (!destroyed && !disabled) {
    return false;
  }
  bool auto_eject_ready = false;
  if (destroyed) {
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
    const float death_delay =
        cls != nullptr ? static_cast<float>(cls->death_delay_frames) : 0.0F;
    if (state.player.death_timer_active <=
            death_delay * kEjectDeathDelayScale ||
        state.player.death_timer_active <= kEjectArmHoldTicks) {
      auto_eject_ready = Outfit_HasAutoEjectOutfit(state);
    }
  }
  if (!eject_command && !auto_eject_ready) {
    return false;
  }
  if (!Outfit_HasEscapePodOrLaunchBay(state)) {
    return false;
  }
  RunPlayerEjectTransform(state);
  return true;
}

} // namespace

// @port 0x0044B240 78% gameplay,ui,synthetic
// Ghidra 0x0044B240 PlayerTick_StatusAndOutfitEvents, internal label of
// Ship_HandlePlayerShipCore. Synthetic CFG: 0x0044B240 ->
// [0x0044B7C4, 0x0044D490]; includes the reordered carried-bomb tails. Runs
// after the hyperspace-exit gate and before every flight-input branch.
bool PlayerTick_StatusAndOutfitEvents(GameState &state,
                                      float elapsed_ticks,
                                      bool eject_command) {
  PlayerShip &p = state.player;

  // --- Player-death bookkeeping (Ship_HandlePlayerShipCore prologue) ------
  // Ship_IsShipDestroyed (0x004688e0: armor <= 0 OR timer running) gates the
  // eject arm, matching the original's check at 0x0045392e -- so armor-only
  // destruction (self-destruct, script kills, collision blasts) enters the
  // sequence too. The presentation itself is owned by Ship_UpdateVisualState
  // (0x00428340), which Frame_TickSystems scope 10 calls on the player right
  // after this core: it seeds the timer (x3 for the player,
  // g_player_death_timer_scale 0x00575378), drives the Explode1 debris cascade
  // while the timer is above 2.0, and runs the Explode2 finale/boom at 2.0.
  // This core only ticks the timer, exactly like the original at 0x004522f3.
  if (p.is_active && NovaAiShip_IsDestroyed(p)) {
    // Fire-restricted damping (0x0044b240, before the eject arm): a destroyed
    // hull is fire-restricted, so the original bleeds 0.5% of each velocity
    // component per raw call here. Exponentiate by the time-adjusted raw-call
    // count so the port matches the original 21 ms maximum-rate cadence. The
    // coast integration runs later in the loop.
    const float restricted_damp =
        std::pow(kPlayerFireRestrictedVelocityDamp,
                 RawSpaceflightCallTicks(elapsed_ticks));
    p.vel_x *= restricted_damp;
    p.vel_y *= restricted_damp;
    // While an escape-pod timed action is armed the original reaches the
    // timed-action block at 0x0044d490 (before the death/eject block at
    // 0x00451024) and returns from the player core. Do not consume the frame
    // here: return false so the spaceflight loop runs
    // PlayerTick_TimedActionTransition, moving the pod and decrementing the
    // countdown. The pod is structurally 0-shield/0-armor, so
    // NovaAiShip_IsDestroyed is true for its whole 0x15e-tick flight.
    if (p.timed_action_counter > 0) {
      return false;
    }
    // PlayerTick eject block (0x004510b9..0x00453910): an owned auto-eject
    // outfit ejects automatically once the death presentation is at least half
    // spent (or within 30 ticks of ending); Alt+X ejects manually. The
    // transform replaces the player with the escape pod or a carried bay
    // fighter.
    (void)TryRunPlayerEjectTransform(state,
                                     eject_command,
                                     /*destroyed=*/true,
                                     /*disabled=*/false);
    // Ship_UpdateVisualState, called immediately after the core, replays the
    // raw-call timer decrement together with its discrete Explode1 RNG pass.
    return true;
  }

  // --- Inactive branch (0x0044aa70 prologue at 0x0044af14) -----------------
  if (!p.is_active) {
    p.destruction_raw_tick_accumulator +=
        std::max(0.0F, RawSpaceflightCallTicks(elapsed_ticks));
    // While the timer is above DAT_00575558 (-240.0) the hull is gone but the
    // spaceflight loop keeps presenting: the Explode2 area effect spawned by
    // the finale (and the fading debris pool) finish playing before the core
    // latches DAT_00596d38. Only once the timer reaches the floor does the
    // inactive branch fall through to the escape-pod scan / game-over latch.
    if (p.death_timer_active > kPlayerDeathInactiveTimerFloor) {
      while (p.destruction_raw_tick_accumulator + 1.0e-6F >= 1.0F &&
             p.death_timer_active > kPlayerDeathInactiveTimerFloor) {
        p.destruction_raw_tick_accumulator -= 1.0F;
        p.death_timer_active -= kJumpTurnaroundTurnRateAddend;
      }
      p.destruction_raw_tick_accumulator =
          std::max(0.0F, p.destruction_raw_tick_accumulator);
      return true;
    }
    if (state.bomb_outfit_class != 0) {
      RunDeathEscapePodScan(state);
    }
    // Ghidra DAT_00596d38, consumed by the spaceflight loop.
    state.game_over_pending = true;
    // Restart command (0x0044abf8): the original polls key-binding slot 0x20
    // through NovaInput_IsCommandActiveWithGameplayGuards and calls
    // Ship_ResetPlayerShipState to relaunch. TODO(decomp(0x0044abf8)) skipped:
    // the gameplay-command input channel is not reconstructed; the spaceflight
    // loop returns to the menu shell on the death latch instead.
    return true;
  }

  // --- Fire-restricted damping (0x0044b240) --------------------------------
  const bool fire_restricted = NovaAiShip_IsDisabled(state, p);
  if (fire_restricted) {
    const float restricted_damp =
        std::pow(kPlayerFireRestrictedVelocityDamp,
                 RawSpaceflightCallTicks(elapsed_ticks));
    p.vel_x *= restricted_damp;
    p.vel_y *= restricted_damp;
  } else {
    p.boarded_target_latch = 0;
  }

  // g_player_recently_hit_timer (DAT_0073549c): armed to 300 ticks when the
  // player takes a hit, decays one tick per frame while at or above the
  // cutoff; suppresses armor regeneration and the disabled auto-repair pass
  // until it falls back below the cutoff.
  if (kRecentlyHitRegenCutoff <= state.recently_hit_timer) {
    state.recently_hit_timer -= elapsed_ticks;
  }

  // --- Disabled auto-repair (0x0044b2a5) -----------------------------------
  if (fire_restricted && state.recently_hit_timer < kRecentlyHitRegenCutoff &&
      Frame_ShouldTriggerAutoRepairTick(state, state.player)) {
    const ShipClass *cls =
        state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
    if (cls != nullptr) {
      const float max_armor = static_cast<float>(cls->base_armor);
      p.armor_points =
          (cls->capability_flags & 0x10) != 0
              ? max_armor * kAutoRepairArmorFractionFlag0x10 +
                    kAutoRepairArmorAddend
              : max_armor * kAutoRepairArmorFraction + kAutoRepairArmorAddend;
    }
    NovaHud_ShowOverlayMessage(
        state,
        NovaHud_LoadStringEntry(kStringListFlightText, kStrAutoRepairEngaged)
            .value_or(""),
        0xe0,
        0xe0,
        0xe0,
        kOverlayDurationAutoRepair);
    state.pending_ui_sounds.push_back(
        GameState::PendingUiSound{kAutoRepairSoundTransitionIndex, 1});
  }

  // --- Combat-alert cue (0x0044b3a8, tail at 0x0044d410) -------------------
  // Every 60th frame the original re-evaluates Ship_IsAnyShipThreatToPlayer-
  // Squad (0x00410060); a rising edge with no blocking timed action queues
  // the snd 370 "Red Alert" cue (g_nova_control_bits[144], priority width 5).
  if (state.spaceflight_frame_counter % 60 == 0) {
    state.player_threat_active_prev = state.player_threat_active;
    state.player_threat_active = NovaAi_IsAnyShipThreatToPlayerSquad(state);
    if (state.player_threat_active && !state.player_threat_active_prev &&
        p.timed_action_counter < 1) {
      state.pending_red_alert = true;
      // The original also sets g_travel_countdown = 0x1e when
      // g_pref_sound_volume < 2; the preference and the countdown consumer are
      // not modelled yet (TODO(decomp)).
    }
  }

  // --- Carried-bomb countdown / detonation (0x0044b3d4) ---------------------
  if (state.bomb_outfit_class == 0) {
    state.bomb_detonation_timer = 0.0F;
  } else if (state.bomb_detonation_timer < kBombDetonationTimerFloor) {
    // 0x0044da75 tail: expired timer rerolls a whole number of intervals.
    state.bomb_detonation_timer =
        static_cast<float>(RandomBelow(state, kBombDetonationRerollMax));
  } else if (!NovaAiShip_IsDestroyed(p)) {
    state.bomb_detonation_timer += elapsed_ticks;
    if (state.bomb_detonation_timer > kBombDetonationIntervalFrames) {
      state.pending_impact_sounds.push_back(
          GameState::PendingImpactSound{0, p.pos_x, p.pos_y});
      if (state.bomb_outfit_class == 1) {
        DetonateEscapePodBomb(state);
      } else {
        DetonateCarriedBomb(state);
      }
    }
  }

  // Ghidra 0x00451024: a disabled (not destroyed) hull ejects manually with
  // the Alt+X arm pair once it owns an escape pod / launch bay. The destroyed
  // case is handled at the top of this function, where auto-eject also
  // applies. `fire_restricted` is the original's prologue-computed
  // Ship_IsShipDisabled flag, so it is not re-evaluated after auto-repair.
  if (fire_restricted && TryRunPlayerEjectTransform(state,
                                                    eject_command,
                                                    /*destroyed=*/false,
                                                    /*disabled=*/true)) {
    return true;
  }
  return false;
}

// @port 0x0044D490 60% gameplay,ui,rendering,synthetic
// Ghidra 0x0044D490 PlayerTick_TimedActionTransition, internal label of
// Ship_HandlePlayerShipCore. Synthetic CFG: 0x0044D490 -> 0x0044D560,
// including the reordered zero-count respawn branch at 0x0044D570..0x0044DA74;
// 0x0044D560 is the nonzero-count continuation. Reached from status/outfit
// handling while timed_action_counter > 0; consumes the remaining player tick.
bool PlayerTick_TimedActionTransition(GameState &state, float elapsed_ticks) {
  PlayerShip &p = state.player;
  if (p.timed_action_counter <= 0) {
    return false;
  }

  // Blocking movement: full effective thrust along the current heading,
  // per-axis polar-clamped at the effective top speed (Math_AddPolarVelocity-
  // WithClamp 0x0043b4e0), then the position integration.
  if (!state.stat_cache_valid) {
    state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
    state.stat_cache_valid = true;
  }
  const PlayerEffectiveStats &eff = state.cached_stats;
  const float max_speed = eff.speed_raw / 100.0F;
  const float thrust_step = eff.thrust_raw / 10000.0F * 2.0F * elapsed_ticks;
  Math_AddPolarVelocityWithClamp(
      p.heading, thrust_step, max_speed, p.vel_x, p.vel_y);
  p.pos_x += p.vel_x * elapsed_ticks;
  p.pos_y += p.vel_y * elapsed_ticks;

  p.timed_action_counter =
      static_cast<std::int16_t>(p.timed_action_counter - 1);
  if (p.timed_action_counter != 0) {
    return true;
  }

  // --- Countdown reached zero: the death/escape-pod respawn transition ----
  // TODO(decomp(0x0044d570)) skipped: the blocking presentation passes -
  // Frame_CommitFrameAndLatchTransitionWait / Frame_FinishBlockingTransition-
  // Frame, the black DrawContext wipes, NovaEffects ambient-star clear/queue,
  // SWParticles_ResetEntries, SpriteWorld_ReleaseAllSpriteFrames,
  // NovaPlatform_EnsureCursorVisible, and the dësc 13999 death dialog
  // (Ui_LoadSelectionDialogResource + Ui_RunTravelSelectionDialog; dësc 13999
  // is the Bible "message shown after the player uses an escape pod").
  NovaLog::Info("escape-pod respawn: blocking transition (dësc 13999 dialog "
                "not reconstructed)");

  // Abort every active mission (Mission_ClearMisnSlotAssignments(slot, 1)).
  const std::uint32_t now_ms =
      static_cast<std::uint32_t>(state.gameplay_now_ms);
  for (std::size_t slot = 0; slot < state.active_mission_runtime_flags.size();
       ++slot) {
    if (state.active_mission_runtime_flags[slot].is_active) {
      Mission_ClearMisnSlotAssignments(state,
                                       static_cast<std::int16_t>(slot),
                                       /*emit_completion_payload=*/true,
                                       now_ms);
      state.active_mission_runtime_flags[slot].is_active = false;
    }
  }

  // Ship_ResetPlayerShipState(0) + the fresh (class 0) ship's OnPurchase
  // reaction script (ShipClassDef+0x1eb <- shp payload 0x26a).
  RespawnResetPlayerShipState(state);
  if (const ShipClass *cls = state.scenario.Ship(0x80)) {
    Mission_ExecuteReactionScript(
        state, cls->on_purchase_expr, MissionScriptContext{"ship OnPurchase"});
  }
  // Ship_DeactivateVacantShipsAndTally(1, 1): the clean-room models the
  // second flag through keep_player_engaged.
  NovaShip_DeactivateVacantShipsAndTally(state, /*keep_player_engaged=*/true);

  // Relocate to the nearest reachable emergency destination (a ship-selling,
  // reputation-acceptable stellar in a visible, discovered neighbour system).
  const std::int16_t dest_stellar =
      Stellar_FindValidRespawnStellar(state, p.current_system_id);
  if (dest_stellar < 0x80) {
    p.current_system_id = 0;
  } else if (const Stellar *st = state.scenario.Stellar(dest_stellar);
             st != nullptr && st->system_id >= 0) {
    p.current_system_id = st->system_id;
  } else {
    p.current_system_id = 0;
  }
  if (const System *sys = state.scenario.System(
          static_cast<std::int16_t>(p.current_system_id + 0x80))) {
    // Starmap pan origin re-centres on the new system.
    state.starmap_pan_x = static_cast<float>(sys->pos_x);
    state.starmap_pan_y = static_cast<float>(sys->pos_y);
  }

  // Weapon banks reloaded from the fresh class's stock (the reset already
  // reseeded them; the original re-runs the sweep here).
  ZeroPlayerWeaponBanks(state);
  NovaWeapon_SeedBanksFromShipStock(state, p.ship_class_id);

  // System_RebuildSystemVisibilityMap(cur, 0, 1).
  NovaSystem_RebuildDiscoveryState(state, p.current_system_id, 0, 1);
  // TODO(decomp(0x0044d814)) skipped: NovaEffects_QueuedAmbientStarParticles.

  // Meters refill. 0x0044d83f: the original calls
  // Ship_ComputeShipMaxShieldPoints (0x00463550) for the ARMOR refill too, so
  // the player returns with armor equal to the max SHIELD value. With
  // kApplyOriginalBugFixes on, use the armor max instead: a respawn class whose
  // max shield is 0 (e.g. a modded shuttle with no shield) otherwise returns
  // with 0 armor and the fresh hull is destroyed on the next tick.
  state.InvalidateDerivedStatCaches();
  const PlayerEffectiveStats refill = Outfit_ComputePlayerEffectiveStats(state);
  p.fuel_points = refill.fuel_capacity;
  p.shield_points = refill.max_shield_points;
  // BUGFIX(original): the armor refill used the max-shield value.
  p.armor_points = kApplyOriginalBugFixes ? refill.max_armor_points
                                          : refill.max_shield_points;
  state.cached_stats = refill;
  state.stat_cache_valid = true;

  // Re-populate the system's NPC/mission ships.
  NovaSystem_PopulateInitialNpcShips(state, p.current_system_id);
  NovaSystem_RestoreMissionFleets(
      state, p.current_system_id, /*copy_player_heading=*/false, now_ms);

  // The world catches up over rand(30) + 15 elapsed game-days.
  const std::int16_t elapsed_days = static_cast<std::int16_t>(
      RandomBelow(state, kRespawnDailyUpdateRange) + kRespawnDailyUpdateBase);
  for (std::int16_t day = 0; day < elapsed_days; ++day) {
    Mission_TickDailyWorldUpdate(state);
  }

  // System_UpdateSystemAndStellarDisplayState (0x00432470) display-state pass.
  NovaTargeting_UpdateStellarAvailability(state);

  // Fresh registration number: class display name + four rand(9)+1 digits
  // (never 0).
  if (const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(p.ship_class_id + 0x80))) {
    std::string registration = cls->display_name + " ";
    for (int digit = 0; digit < 4; ++digit) {
      registration += std::to_string(RandomBelow(state, 9) + 1);
    }
    p.ship_name = registration;
  }
  // TODO(decomp(0x0044d91e)) skipped: the viewport/radar redraw and the
  // per-panel dirty flags (the clean-room re-renders every frame).

  // Rebuild the per-system reputations from each system government's InitialRec
  // (GovtDef 0x52, payload +0x14) over all 0x800 systems, matching
  // Game_ResetReputationAndAvailability (0x004b4220).
  const std::size_t system_count =
      std::min(state.scenario.systems.size(), state.system_reputation.size());
  for (std::size_t i = 0; i < system_count; ++i) {
    const std::int16_t govt = state.scenario.systems[i].government_id;
    const Government *gov_def =
        govt >= 0
            ? state.scenario.Government(static_cast<std::int16_t>(govt + 0x80))
            : nullptr;
    state.system_reputation[i] = gov_def != nullptr ? gov_def->initial_rec : 0;
  }

  // Conditional post-respawn auto-save (0x0044da30): under Strict Play
  // (g_strict_play -> state.pilot.strict_play), save at the new system's first
  // defined nav stellar (NavDef1-16, SystemDef +0x2a), or at slot 0 when it
  // has none. The original loops the 16 words and takes the first != -1, then
  // passes the raw value to PilotFile_SaveGame; the port's nav_defs are
  // 0x80-based resource ids, so rebase at this boundary. Tests and bootstrap
  // states without a pilot name are skipped, like the launch autosave.
  if (state.pilot.strict_play) {
    std::int16_t destination_index = 0;
    if (const System *sys = state.scenario.System(
            static_cast<std::int16_t>(p.current_system_id + 0x80))) {
      for (const std::int16_t nav : sys->nav_defs) {
        if (nav != -1) {
          destination_index = PilotFileStellarIndexFromResourceId(nav);
          break;
        }
      }
    }
    if (!state.pilot.first_name.empty()) {
      if (const auto directory = PilotFileSaveDirectory()) {
        if (!PilotFileSaveGame(*directory, state, destination_index)) {
          NovaLog::Error("respawn: could not autosave pilot '{}'",
                         state.pilot.first_name);
        }
      }
    }
  }
  NovaLog::Info("escape-pod respawn complete: system {}, class {}, ' {}'",
                p.current_system_id,
                p.ship_class_id,
                p.ship_name);
  // Frame_FinishBlockingTransitionFrame: presentation pass, skipped above.
  return true;
}

// @port 0x0044CA6B 90% correctness,synthetic
// Ghidra 0x0044CA6B PlayerTick_TurnBankAnimation, internal label of
// Ship_HandlePlayerShipCore. Synthetic CFG: 0x0044CA6B -> 0x0044CB99.
static void TickPlayerTurnBankAnimation(GameState &state,
                                        int turn_dir,
                                        float elapsed_ticks) {
  PlayerShip &p = state.player;
  // Banking classes (sh\x8an Flags & 1) accumulate
  // turn_bank_animation_phase (+0xc8e4) while a turn is held (capped +16 /
  // -16, DAT_005755c0/c4), decay it toward zero at 2x frame rate
  // (DAT_00575618) when straight, and raise ai_turn_bias_dir (+0xc8f8) once
  // the phase passes the +6/-6 hysteresis thresholds (DAT_005755c8 = 6,
  // DAT_00575620 = -6). The sprite layer maps the bias to the bank-left /
  // bank-right alt rows (Ship_UpdateVisualState 0x00428340).
  // turn_dir is the original's sVar8: set by the keyboard steering branch AND
  // by the auto-turn continuation (reverse command, face-target command, jump
  // alignment) whenever a rotation step was applied this frame, so auto-turns
  // bank exactly like keyboard turns.
  p.ai_turn_bias_dir = 0;
  const ShipClass *player_cls =
      state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
  if (player_cls != nullptr && (player_cls->sprite_behavior_flags & 1U) != 0U) {
    constexpr float kBankPhaseCap = 16.0F;
    constexpr float kBankDecayRate = 2.0F;
    constexpr float kBankBiasThreshold = 6.0F;
    if (turn_dir == 0) {
      const float decay = kBankDecayRate * elapsed_ticks;
      if (p.turn_bank_animation_phase > decay) {
        p.turn_bank_animation_phase -= decay;
      } else if (p.turn_bank_animation_phase < -decay) {
        p.turn_bank_animation_phase += decay;
      } else {
        p.turn_bank_animation_phase = 0.0F;
      }
    } else {
      if (turn_dir > 0 && p.turn_bank_animation_phase < kBankPhaseCap) {
        p.turn_bank_animation_phase += elapsed_ticks;
      }
      if (turn_dir < 0 && p.turn_bank_animation_phase > -kBankPhaseCap) {
        p.turn_bank_animation_phase -= elapsed_ticks;
      }
    }
    if (turn_dir >= 1 && p.turn_bank_animation_phase > kBankBiasThreshold) {
      p.ai_turn_bias_dir = 1;
    } else if (turn_dir <= 0 &&
               p.turn_bank_animation_phase < -kBankBiasThreshold) {
      p.ai_turn_bias_dir = -1;
    }
    // Station-hold / maneuver-lock reset arm: while the hold or a maneuver
    // lockout is active the phase snaps to zero (station hold past the
    // 30-tick engage gate) and the glow fades one step (handled by the
    // target-based glow drive below; the original decays it explicitly here).
    if ((p.ai_maneuver_timer_ms > 0.0F || p.ai_station_hold_timer > 0.0F) &&
        p.ai_station_hold_timer > 30.0F) {
      p.turn_bank_animation_phase = 0.0F;
      p.ai_turn_bias_dir = 0;
    }
  }
}

// @port 0x0044C0B1 90% correctness,synthetic
// Ghidra 0x0044aa70 face-target command (0x0044c0b1 -> 0x0044c18a, with the
// out-of-line bearing tails at 0x0044ec90/0x0044ecb7), part of the
// PlayerTick_WeaponCommands region. While held with the station-hold timer
// idle the command stores the integer heading to face (Math_BearingFromPoint-
// ToPoint from the player position, written to ai_desired_heading_deg +0x68)
// and arms the manual-flight auto-turn (the parent's local_265 latch): the
// primary ship target wins unless the 0x38/0x6f arm modifier is held, which
// -- like having no ship target -- faces the selected travel stellar instead
// (the original reads the player's +0x6c slot; the port keeps that selection
// in travel.selected_stellar_id because its travel commands never populate
// the player ship field). With neither targeted the command is inert.
// Returns true when armed: NovaPlayer_IntegrateMovement then suppresses
// keyboard steering and turns one step per frame toward the stored heading
// until within one step (no snap -- the |delta| <= step exit rejoins the
// keyboard path, so the final step-short alignment is kept).
bool PlayerTick_FaceTargetCommand(GameState &state,
                                  const FlightInput &input,
                                  bool arm_modifier_held) {
  PlayerShip &p = state.player;
  if (!input.face_target || p.ai_station_hold_timer > 0.0F) {
    return false;
  }
  const std::int16_t ship_target = p.primary_target_ship_slot;
  const std::int16_t stellar_target = state.travel.selected_stellar_id;
  bool face_stellar = false;
  const Ship *target_ship = nullptr;
  const Stellar *target_stellar = nullptr;
  if (ship_target >= 0 &&
      state.SlotInRange(static_cast<std::size_t>(ship_target))) {
    if (arm_modifier_held && stellar_target >= 0) {
      face_stellar = true;
    } else {
      target_ship = &state.ShipAt(static_cast<std::size_t>(ship_target));
    }
  } else if (stellar_target >= 0) {
    face_stellar = true;
  } else {
    return false;
  }
  if (face_stellar) {
    target_stellar = state.scenario.Stellar(stellar_target);
    if (target_stellar == nullptr) {
      return false;
    }
  }
  // Math_BearingFromPointToPoint: integer degrees, 0 = up, clockwise.
  const float dx = face_stellar
                       ? static_cast<float>(target_stellar->pos_x) - p.pos_x
                       : target_ship->pos_x - p.pos_x;
  const float dy = face_stellar
                       ? static_cast<float>(target_stellar->pos_y) - p.pos_y
                       : target_ship->pos_y - p.pos_y;
  const float bearing_deg =
      std::atan2(dx, -dy) * (180.0F / 3.14159265358979323846F);
  int heading_deg = static_cast<int>(std::lround(bearing_deg));
  heading_deg = ((heading_deg % 360) + 360) % 360;
  p.ai_desired_heading_deg = static_cast<std::int16_t>(heading_deg);
  return true;
}

// @port 0x0044C8D0 85% gameplay,synthetic
// Ghidra 0x0044C8D0 PlayerTick_ManualFlightAndRegeneration, internal umbrella
// of Ship_HandlePlayerShipCore. Relevant synthetic CFGs: turn input
// 0x0044C92E -> 0x0044C980; joined afterburner/thrust/glow
// 0x0044C9AB -> 0x0044CA6B; bank animation 0x0044CA6B -> 0x0044CB99; reordered
// afterburner speed-cap/fuel/glow tail 0x00451630 -> 0x004518EF.
void PlayerTick_ManualFlightAndRegeneration(GameState &state,
                                            const FlightInput &input,
                                            float elapsed_ticks,
                                            bool face_target_armed) {
  PlayerShip &p = state.player;
  if (!state.stat_cache_valid) {
    state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
    state.stat_cache_valid = true;
  }
  const PlayerEffectiveStats &eff = state.cached_stats;

  // Ghidra Ship_HandlePlayerShipCore 0x0044aa70: the keyboard turn, reverse,
  // thrust and afterburner arms are all gated on the parent's local_251 latch
  // (`!Ship_IsShipDisabled(p)`) and `ai_station_hold_timer <= 0`. The
  // face-target auto-turn continuation is NOT gated, so keep
  // `face_target_armed` as passed and clear only the input latches when the
  // controls are down; gravity, speed caps and position integration still run.
  const bool fire_restricted = NovaAiShip_IsDisabled(state, p);
  const bool controls_disabled =
      fire_restricted || p.ai_station_hold_timer > 0.0F;
  FlightInput effective_input = input;
  if (controls_disabled) {
    effective_input.turn_left = false;
    effective_input.turn_right = false;
    effective_input.thrust = false;
    effective_input.reverse = false;
    effective_input.afterburner = false;
  }

  // @port 0x0044C9AB 85% correctness,synthetic
  // Ghidra 0x0044c9ab PlayerTick_AfterburnerCommand (synthetic region of
  // 0x0044aa70): capability/fuel activation and burn. Exact station-hold /
  // maneuver gates remain approximate.
  const float fuel_burn = Outfit_GetPlayerAfterburnerFuelBurnRate(state);
  const bool afterburner_active =
      effective_input.afterburner && !effective_input.reverse &&
      fuel_burn <= p.fuel_points && fuel_burn > 0.0F;
  // Ghidra's old name g_player_in_gravity_well is misleading: this is the
  // opcode-15 afterburner latch. Stellar_TickStellarGravityPull independently
  // sets g_gravity_pull_active, which disables the afterburner's boosted speed
  // branch but does not prevent normal thrust.
  const bool gravity_present =
      Ship_AccelerateShipTowardPoint(state, elapsed_ticks);
  // Expose the two latches the impact-impulse clamp reads (Ghidra
  // g_player_afterburner_active / g_gravity_pull_active).
  state.player_afterburner_active = afterburner_active;
  state.gravity_pull_active = gravity_present;

  ShipClass effective_class;
  effective_class.accel = eff.thrust_raw;
  effective_class.speed = eff.speed_raw;
  effective_class.turn_rate = eff.turn_raw;
  // Player tails of Ship_ComputeShipEffectiveThrust (0x004640a0) and
  // Ship_ComputeShipMaxTurnRateDeg (0x00463e70). TODO(decomp): the original
  // turn-damping gate uses ShipState +0x28/+0x5c, which is not fully decoded.
  if (p.ionization_points > 0.0F) {
    // Ship_GetIonizationIntensity (0x0046c160) includes the player ModType-40
    // ion absorber capacity additions.
    const float intensity =
        std::min(0.7F, NovaOutfit_GetIonizationIntensity(state, p));
    effective_class.accel *= (1.0F - intensity);
    if (!effective_input.thrust) {
      effective_class.turn_rate *= (1.0F - intensity);
    }
  }
  if (afterburner_active && !gravity_present) {
    effective_class.speed *= 1.8F;
  }
  // @port 0x0044D05B 90% correctness,synthetic
  // Ghidra 0x0044d05b PlayerTick_ClampVelocityToSpeedCaps (synthetic region of
  // 0x0044aa70). TODO(decomp): the exact same-frame parent ordering (the
  // original updates the caps at the frame tail).
  // Per-axis velocity caps (Ghidra LAB_00451630 afterburner speed-cap tail,
  // 0x00451630 -> 0x004518ef; g_player_speed_cap_x/y maintained on GameState):
  // while afterburning outside a stellar gravity pull both caps jump to 1.8x
  // the effective max speed (g_afterburner_overspeed_factor 0x00575610);
  // otherwise they decay by effective thrust * 0.4 (DAT_00575680) per frame
  // and are clamped up to the effective max speed. Maintained ahead of the
  // integration so the velocity clamp sees current-frame caps (the original
  // updates them at the frame tail; one-frame lag is not observable). The
  // tail's engine-glow > 24 decay is owned by the port's glow drive.
  {
    const float eff_max_speed = effective_class.speed / 100.0F;
    if (afterburner_active && !gravity_present) {
      constexpr float kAfterburnerOverspeedFactor = 1.8F; // 0x00575610
      state.player_speed_cap_x = eff_max_speed * kAfterburnerOverspeedFactor;
      state.player_speed_cap_y = eff_max_speed * kAfterburnerOverspeedFactor;
    } else {
      const float cap_decay = effective_class.accel / 10000.0F * 2.0F * 0.4F;
      if (eff_max_speed < state.player_speed_cap_x) {
        state.player_speed_cap_x -= cap_decay;
      }
      if (eff_max_speed < state.player_speed_cap_y) {
        state.player_speed_cap_y -= cap_decay;
      }
      if (state.player_speed_cap_x < eff_max_speed) {
        state.player_speed_cap_x = eff_max_speed;
      }
      if (state.player_speed_cap_y < eff_max_speed) {
        state.player_speed_cap_y = eff_max_speed;
      }
    }
  }
  // Inertialess movement model (Outfit_ShipIsInertialess 0x0046df70
  // player branch: class flags_secondary 0x40 or an owned inertial dampener).
  PlayerMovementOptions movement_opts;
  movement_opts.face_target_armed = face_target_armed;
  movement_opts.inertialess = NovaPlayer_IsInertialess(state);
  movement_opts.fire_restricted = fire_restricted;
  movement_opts.speed_cap_x = state.player_speed_cap_x;
  movement_opts.speed_cap_y = state.player_speed_cap_y;
  // Capture the applied turn direction (keyboard OR auto-turn) for the bank
  // animation.
  const PlayerMovementStats movement_stats = NovaPlayer_IntegrateMovement(
      p, effective_input, effective_class, elapsed_ticks, movement_opts);
  if (afterburner_active) {
    p.fuel_points = std::max(0.0F, p.fuel_points - fuel_burn * elapsed_ticks);
  }

  TickPlayerTurnBankAnimation(state, movement_stats.turn_dir, elapsed_ticks);
  const ShipClass *player_cls =
      state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
  // @port 0x0044CA3D 85% gameplay,synthetic
  // Ghidra 0x0044ca3d PlayerTick_ThrustAndEngineGlow (synthetic region of
  // 0x0044aa70). Inertialess scalar-speed branch remains incomplete.
  // TODO(decomp(0x0044aa70)) skipped: the player engine-glow state machine is
  // still a clean-room target interpolation. Reconstruct its distinct normal
  // thrust, inertialess, afterburner, banking, turnaround, and hyperspace
  // branches before changing its cadence. Those original integer mutations
  // run once per raw spaceflight call and should eventually replay at the
  // 21 ms cadence used by the NPC Ship_HandleShip path above; do not merely
  // put this approximation behind that cadence.
  // ShipState +0xc8d4 is an integer engine/glow control, not a free-running
  // alpha ramp. Normal thrust approaches 24; afterburning extends it to 32;
  // coasting and reverse decrement by one renderer frame. This makes the
  // observable rise/fall and afterburner cap match player control; rendering
  // derives its alpha from that level until SpriteWorld's native glow blend is
  // reconstructed.
  const std::int16_t glow_target =
      afterburner_active ? 32 : (p.engine_thrust ? 24 : 0);
  // Banking boost (Flags & 2 classes): the original adds +2 per frame while
  // ai_turn_bias_dir is set, capped at 0x18 -- equivalent to holding the
  // cruise glow while banking without thrust.
  const std::int16_t effective_glow_target =
      (p.ai_turn_bias_dir != 0 && player_cls != nullptr &&
       (player_cls->sprite_behavior_flags & 2U) != 0U)
          ? std::max<std::int16_t>(glow_target, 24)
          : glow_target;
  if (p.engine_glow_level < effective_glow_target) {
    ++p.engine_glow_level;
  } else if (p.engine_glow_level > effective_glow_target) {
    --p.engine_glow_level;
  }
  p.engine_glow_intensity =
      std::clamp(static_cast<float>(p.engine_glow_level) / 24.0F, 0.0F, 1.0F);
}

// @port 0x0044CB99 100% synthetic
// Ghidra Ship_HandlePlayerShipCore 0x0044AA70 synthetic CFG:
// PlayerTick_ShieldAndArmorRegeneration 0x0044CB99 -> 0x0044CCAF.
void PlayerTick_ShieldAndArmorRegeneration(GameState &state,
                                           float frame_time_ms) {
  PlayerShip &p = state.player;
  if (!state.stat_cache_valid) {
    state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
    state.stat_cache_valid = true;
  }
  const PlayerEffectiveStats &eff = state.cached_stats;
  const float tick_scale = frame_time_ms / (1000.0F / 30.0F);
  const bool destroyed = NovaAiShip_IsDestroyed(p);
  const bool disabled = NovaAiShip_IsDisabled(state, p);

  // Shields recover whenever the hull is intact and not disabled (no
  // recently-hit gate, and the original does not clamp to max within the
  // frame -- the < max gate applies to the next frame's addition).
  if (!destroyed && !disabled && p.shield_points < eff.max_shield_points) {
    p.shield_points += std::max(0.0F, eff.shield_recharge) * tick_scale;
  }
  // Armor recovery additionally holds off until the post-hit suppression
  // window (g_player_recently_hit_timer, armed +300 on a hit) has decayed
  // below zero. The rate is the class ArmorRech base (0 on stock ships) plus
  // ModType-29 outfit bonuses (Bible: 1000 = one more point per frame); the
  // cheat-mode latch scales it by k_armor_regen_player_scale_f32 = 50
  // (Ship_ComputeShipArmorRegenRate 0x004638e0 player tail).
  if (!destroyed && !disabled && p.armor_points < eff.max_armor_points &&
      state.recently_hit_timer < 0.0F) {
    float armor_rate = std::max(0.0F, eff.armor_recharge);
    if (state.cheat_mode_active) {
      armor_rate *= 50.0F;
    }
    p.armor_points += armor_rate * tick_scale;
  }
}

// @port 0x00450717 100% divergence,synthetic
// DIVERGENCE(original): the rate is scaled by a float tick scale (not the
// original double g_avg_frame_tick_scale) and the 0.7/0.025 constants are
// float literals.
// Ghidra Ship_HandlePlayerShipCore 0x0044AA70 synthetic CFG:
// PlayerTick_IonizationAndFuelRegeneration 0x00450717 -> 0x004507B4.
void PlayerTick_IonizationAndFuelRegeneration(GameState &state,
                                              float frame_time_ms) {
  PlayerShip &p = state.player;
  if (!state.stat_cache_valid) {
    state.cached_stats = Outfit_ComputePlayerEffectiveStats(state);
    state.stat_cache_valid = true;
  }
  const PlayerEffectiveStats &eff = state.cached_stats;
  const float tick_scale = frame_time_ms / (1000.0F / 30.0F);
  // Ionization decay then the ionized-velocity ramp. The gate, unclamped
  // (possibly negative) decay, 0.7 intensity cap and 0.025 per-frame ramp
  // constant (DAT_00575668/DAT_00575670) match the original block at
  // 0x0045073f/0x00452304. The cap uses the full Ship_ComputeShipEffectiveMax-
  // Speed 0x004642e0 including the mission x2 and non-strict-play 1.5x factors.
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(p.ship_class_id + 0x80));
  spaceflight_detail::NovaShip_UpdateIonizationCharge(
      state,
      p,
      cls != nullptr
          ? NovaShip_ComputeEffectiveMaxSpeedPxPerTick(state, p, *cls)
          : eff.speed_raw / 100.0F,
      tick_scale);

  // Fuel-scoop recharge (Ship_ComputeShipFuelRechargeRate 0x00463b30 via its
  // HandlePlayerShipCore call site): rate cached in the stats snapshot; a
  // negative rate is the "fuel sucking" mode. Then the original clamps fuel
  // to [0, fuel capacity].
  p.fuel_points += eff.fuel_regen_rate * tick_scale;
  p.fuel_points =
      std::clamp(p.fuel_points, 0.0F, static_cast<float>(eff.fuel_capacity));
}

} // namespace game
