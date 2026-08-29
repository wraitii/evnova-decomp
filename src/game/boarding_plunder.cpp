#include "boarding_plunder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_audio.hpp"
#include "../sdl_platform.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "ship_ai.hpp"
#include "ship_visual.hpp"
#include "targeting.hpp"

#include <SDL3/SDL.h>

namespace game {
namespace {

// ---- Decoded binary constants (doubles in the original's data pool) -------
constexpr double kCreditRollThreshold = 2.0;  // _DAT_00575900
constexpr double kCreditScale = 1000.0;       // _DAT_00575918
constexpr double kDudeBootyCostShare = 0.025; // _DAT_00575910
// kMissionBootyShare = 0.5 (_DAT_005758a0) applies to the mission-ship booty
// arm (MissionShipDef +0x618), TODO(decomp) until mission-ship defs are
// modeled.
constexpr float kEscortStatShare = 0.1F;       // _DAT_00575920
constexpr float kCaptureOddsScale = 100.0F;    // _DAT_005758f8
constexpr float kCaptureOddsDenom = 10.0F;     // _DAT_00575928
constexpr std::int16_t kCaptureOddsMax = 0x4b; // 75%
constexpr int kFuelBinSize = 10;               // rand(fuel_max/10) * 10

// OutfitEffect::kMarines (ModType 0x19): ModVal > 0 adds crew, ModVal < 0
// adds capture-odds percent (Bible "marines").
constexpr std::int16_t kMarinesModType = 25;
constexpr int kOutfitModPairCount = 4; // ModType1-4 / ModVal1-4

constexpr std::size_t kEscortCap = 6; // Ship_CanPlayerHaveMoreEscorts soft cap

// Uniform integer in [0, bound). Mirrors NovaRandom_Range (0x004683b0) drawn
// from GameState's PRNG, per the negotiation-dialog convention. Divergence:
// the original reseeds its LCG on bound == 0 and returns an undefined
// register; the port deterministically returns 0 (affects only the fuel roll
// for classes with fuel_max < 10, whose offer is 0 either way).
[[nodiscard]] std::int16_t NovaRandomRange(std::mt19937 &rng, int bound) {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::int16_t>(
      std::uniform_int_distribution<int>{0, bound - 1}(rng));
}

// The original's ROUND(f) + (0 < frac) ladder: round-half-up for positive
// values (all values here are counts/shares and positive).
[[nodiscard]] std::int32_t RoundHalfUp(float v) {
  return static_cast<std::int32_t>(std::llround(static_cast<double>(v)));
}

// Ghidra 0x00469230 Weapon_HasMatchingWeaponAmmoCarried: scans the PLAYER
// ship's weapon banks for ammo/secondaries relevant to `weapon_bank`
// (compared by the weapon's ammo_type code, including a carried mode-99
// launcher). Quirk preserved: when the first stocked bank is a mode-99
// weapon, the original returns immediately (1 on match, 0 on mismatch)
// without scanning later banks.
[[nodiscard]] bool PlayerCarriesMatchingWeaponAmmo(const GameState &state,
                                                   int weapon_bank) {
  const Weapon *probe =
      state.scenario.Weapon(static_cast<std::int16_t>(weapon_bank + 0x80));
  if (probe == nullptr) {
    return false;
  }
  for (int bank = 0; bank < 0x100; ++bank) {
    if (state.weapon_bank_ammo[static_cast<std::size_t>(bank) * 100] <= 0) {
      continue;
    }
    const Weapon *carried =
        state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
    if (carried == nullptr) {
      continue;
    }
    if (carried->weapon_mode_code == 99) {
      return carried->ammo_type == probe->ammo_type;
    }
    if (weapon_bank == carried->ammo_type) {
      return true;
    }
  }
  return false;
}

// One outfit mod pair (ModTypeN/ModValN, N = 1..4). Weapon/ammo/bomb ModVals
// are stored zero-based by the scenario loader (see DecodeOutfit).
struct OutfitModPair {
  std::int16_t type = 0;
  std::int16_t val = 0;
};

[[nodiscard]] OutfitModPair OutfitModPairAt(const Outfit &outfit, int pair) {
  if (pair == 0) {
    return {outfit.mod_type, outfit.mod_val};
  }
  return {outfit.alt_mod_types[static_cast<std::size_t>(pair - 1)],
          outfit.alt_mod_vals[static_cast<std::size_t>(pair - 1)]};
}

// The credits offer roll shared by the dude and mission-ship paths. `v` is
// the scaled booty value in thousands; above the 2.0 threshold the original
// adds a random multiple of 1000 credits on top of the base share.
[[nodiscard]] std::int32_t RollCredits(GameState &state, double v) {
  if (kCreditRollThreshold < v) {
    const int bound = static_cast<int>(std::ceil(v));
    const int roll = NovaRandomRange(state.rng, bound);
    const double raw = (static_cast<double>(roll) + v) * kCreditScale;
    return RoundHalfUp(static_cast<float>(raw));
  }
  return RoundHalfUp(static_cast<float>(v * kCreditScale));
}

} // namespace

// Ghidra 0x00468920 Ship_CanPlayerHaveMoreEscorts. Counts active behavior-6
// escorts (targeting the player, no mission fleet) and compares against the
// soft cap of 6. Unlike the capture-odds accumulation in BuildOptions this
// count has NO default_ai_behavior > 2 condition.
bool NovaShip_CanPlayerHaveMoreEscorts(const GameState &state) {
  std::int16_t escorts = 0;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || ship.ai_target_ship_slot != 0 ||
        ship.ai_behavior_code != 6 || ship.mission_fleet_slot == -1) {
      continue;
    }
    ++escorts;
  }
  return escorts < static_cast<std::int16_t>(kEscortCap);
}

// Ghidra 0x00484230 Ship_BuildBoardingPlunderOptions. Rolls the plunder
// offers and the capture odds for the player's primary target. Call with a
// valid, boardable target (the board command and the window open path both
// gate on that).
BoardingPlunderOptions NovaBoarding_BuildOptions(GameState &state) {
  BoardingPlunderOptions options;

  const std::int16_t target_slot = state.player.primary_target_ship_slot;
  if (target_slot < 1 ||
      !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
    return options; // no boardable target: empty (all offers -1/0)
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  const ShipClass *target_class = state.scenario.Ship(
      static_cast<std::int16_t>(target.ship_class_id + 0x80));
  const ShipClass *player_class = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (target_class == nullptr || player_class == nullptr) {
    return options;
  }

  // ---- Booty flags --------------------------------------------------------
  // The DudeDef.booty_flags word drives cargo + credits. Non-dude ships fall
  // through to the mission-ship booty arm; a ship with neither gets no
  // credits offer.
  std::uint16_t booty_flags = 0;
  if (target.dude_class_id != -1) {
    if (const DudeDef *dude = state.scenario.Dude(
            static_cast<std::int16_t>(target.dude_class_id + 0x80))) {
      booty_flags = dude->booty_flags;
    }
  }

  // ---- Credits (DAT_007d17e0) --------------------------------------------
  if ((booty_flags & 0x40) != 0) {
    // Dude "carries money": 2.5% of the ship class cost, in thousands.
    // The original truncates base_cost to short before the division.
    const double v = static_cast<double>(
                         static_cast<std::int16_t>(target_class->cost / 1000)) *
                     kDudeBootyCostShare;
    options.credits = RollCredits(state, v);
    if (options.credits < 1000) {
      options.credits = 1000; // dude money always pays at least 1000
    }
  } else if (target.mission_ship_slot != -1) {
    // Mission-ship booty: half of the mission def's booty_base_credits.
    // TODO(decomp): the original reads MissionShipDef +0x618
    // (booty_base_credits) from the mission-ship table, which the port does
    // not model yet; mission ships get no credits offer here.
    options.credits = -1;
  } else {
    options.credits = -1;
  }
  if (options.credits < 1) {
    options.credits = -1;
  }

  // ---- Cargo (DAT_007d17d0/d2) -------------------------------------------
  // Money (0x40) is not a cargo bin: the roll picks one of 7 bins and only
  // bins 0..5 (0x1 food .. 0x20 equipment) matching a set booty flag are
  // accepted; the original repeats until one matches. Like the original,
  // booty flags outside bits 0..5 alone (after the 0x40 guard) would spin —
  // shipped dude defs never do that.
  if ((booty_flags & 0xffbfU) != 0) {
    bool accepted = false;
    do {
      options.cargo_type = NovaRandomRange(state.rng, 7);
      if (options.cargo_type >= 0 && options.cargo_type <= 5 &&
          (booty_flags & (1U << static_cast<unsigned>(options.cargo_type))) !=
              0U) {
        accepted = true;
      }
    } while (!accepted);

    // Quantity: half to full cargo holds of the target's class.
    const std::int16_t holds = target_class->cargo_holds;
    if (holds < 1) {
      options.cargo_quantity = 0;
      options.cargo_type = -1;
    } else {
      const std::int16_t half = static_cast<std::int16_t>((holds + 1) / 2);
      options.cargo_quantity =
          static_cast<std::int16_t>(NovaRandomRange(state.rng, half) + half);
    }
  } else {
    options.cargo_type = -1;
  }
  if (options.cargo_quantity < 1) {
    options.cargo_type = -1;
  }
  if (options.cargo_type == -1) {
    options.cargo_quantity = 0;
  }

  // ---- Ammo (DAT_007d17d4/d6) --------------------------------------------
  // Candidates: target banks with stock, non-mode-99 weapons the player can
  // actually carry ammo for (Weapon_HasMatchingWeaponAmmoCarried).
  // The NPC weapon banks are populated from the class stock loadout by
  // EnsureNpcWeaponBanks (src/game/ship_ai.cpp).
  {
    int candidates = 0;
    for (int bank = 0; bank < 0x100; ++bank) {
      const std::int16_t stock =
          target.npc_weapon_bank_secondary[static_cast<std::size_t>(bank)];
      const Weapon *w =
          state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
      if (stock > 0 && w != nullptr && w->weapon_mode_code != 99 &&
          PlayerCarriesMatchingWeaponAmmo(state, bank)) {
        ++candidates;
      }
    }
    if (candidates < 1) {
      options.ammo_bank = -1;
    } else {
      // Rejection-sample a stocked bank with the same predicate.
      while (true) {
        const int bank = NovaRandomRange(state.rng, 0x100);
        const std::int16_t stock =
            target.npc_weapon_bank_secondary[static_cast<std::size_t>(bank)];
        const Weapon *w =
            state.scenario.Weapon(static_cast<std::int16_t>(bank + 0x80));
        if (stock >= 1 && w != nullptr && w->weapon_mode_code != 99 &&
            PlayerCarriesMatchingWeaponAmmo(state, bank)) {
          options.ammo_quantity = stock;
          options.ammo_bank = static_cast<std::int16_t>(bank);
          break;
        }
      }
    }
  }

  // ---- Fuel (DAT_007d17d8) -----------------------------------------------
  // A random number of whole 10-unit bins, up to fuel_max/10.
  if (target_class->base_fuel < 1) {
    options.fuel_quantity = 0;
  } else {
    options.fuel_quantity = static_cast<std::int16_t>(
        NovaRandomRange(state.rng, target_class->base_fuel / kFuelBinSize) *
        kFuelBinSize);
  }

  // ---- Capture odds (g_capture_odds_percent) -----------------------------
  // Two accumulators over the player's fleet:
  //   strength = player strength + 10% of each eligible escort's strength
  //   crew     = player crew     + 10% of each eligible escort's crew
  // followed by the marines outfits (positive ModVal adds crew).
  float strength_acc = static_cast<float>(player_class->strength);
  float crew_acc = static_cast<float>(player_class->crew);
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &escort = state.ShipAt(slot);
    if (!escort.is_active || escort.ai_target_ship_slot != 0 ||
        escort.ai_behavior_code != 6 || escort.mission_fleet_slot == -1) {
      continue;
    }
    const ShipClass *escort_class = state.scenario.Ship(
        static_cast<std::int16_t>(escort.ship_class_id + 0x80));
    if (escort_class == nullptr || escort_class->default_ai_behavior <= 2) {
      continue; // the odds accumulation ignores fighter-class escorts
    }
    strength_acc = static_cast<float>(
        RoundHalfUp(strength_acc + static_cast<float>(escort_class->strength) *
                                       kEscortStatShare));
    crew_acc = static_cast<float>(RoundHalfUp(
        crew_acc + static_cast<float>(escort_class->crew) * kEscortStatShare));
  }

  // Marines outfits: ModVal > 0 contributes ModVal * owned to the crew pool.
  for (std::size_t index = 0; index < state.scenario.outfits.size(); ++index) {
    const std::int16_t owned = state.inventory.outfit_owned_count[index];
    if (owned <= 0) {
      continue;
    }
    const Outfit &outfit = state.scenario.outfits[index];
    for (int pair = 0; pair < kOutfitModPairCount; ++pair) {
      const OutfitModPair mod = OutfitModPairAt(outfit, pair);
      if (mod.type == kMarinesModType && mod.val > 0) {
        crew_acc += static_cast<float>(mod.val * owned);
      }
    }
  }

  // Odds = crew / (target crew * 10) * 100, rounded. A zero-crew target
  // cannot happen through the board command's crew >= 1 gate; the float
  // division would saturate and the clamp below bounds the result either way.
  float odds = static_cast<float>(
      RoundHalfUp((crew_acc / (static_cast<float>(target_class->crew) *
                               kCaptureOddsDenom)) *
                  kCaptureOddsScale));

  // Marines with negative ModVal directly add capture-odds percent.
  for (std::size_t index = 0; index < state.scenario.outfits.size(); ++index) {
    const std::int16_t owned = state.inventory.outfit_owned_count[index];
    if (owned <= 0) {
      continue;
    }
    const Outfit &outfit = state.scenario.outfits[index];
    for (int pair = 0; pair < kOutfitModPairCount; ++pair) {
      const OutfitModPair mod = OutfitModPairAt(outfit, pair);
      if (mod.type == kMarinesModType && mod.val < 0) {
        odds += static_cast<float>(-mod.val * owned);
      }
    }
  }

  // A fleet whose accumulated strength dwarfs the player's own hull gets a
  // flat +10% bonus (compared against the BASE player-class strength x 5).
  if (player_class->strength * 5 < static_cast<int>(strength_acc)) {
    odds += 10.0F;
  }

  // ±5 noise, then clamp to [1, 75].
  odds += static_cast<float>(5 - NovaRandomRange(state.rng, 0xb));
  if (odds < 1.0F) {
    odds = 1.0F;
  }
  if (odds > static_cast<float>(kCaptureOddsMax)) {
    odds = static_cast<float>(kCaptureOddsMax);
  }
  options.capture_odds_percent = static_cast<std::int16_t>(odds);

  // Derelict-government ships (flags 0x800 "starts out disabled") can never
  // be captured.
  if (target.faction_or_government_id != -1) {
    if (const Government *govt =
            state.scenario.Government(target.faction_or_government_id);
        govt != nullptr && (govt->flags_primary & 0x800U) != 0U) {
      options.capture_odds_percent = 0;
    }
  }

  // Unlicensed ship classes are never capturable. The original checks
  // ShipClassDef[g_expression_ship_class_id].is_licensed_runtime (the
  // expression-eval scratch holding the boarded ship's class); the port does
  // not model the license runtime yet and treats all classes as licensed.
  // TODO(decomp): wire is_licensed_runtime through the scenario loader.
  // if (!licensed) options.capture_odds_percent = 0;

  // Escort cap reached: capture is not offered at all.
  if (!NovaShip_CanPlayerHaveMoreEscorts(state)) {
    options.capture_odds_percent = 0;
  }

  return options;
}

// ---------------------------------------------------------------------------
// Board command + capture reset (iterations 2/4 of the boarding work)
// ---------------------------------------------------------------------------

namespace {

// Lazi ly decodes snd 150 + i into GameState.transition_sounds, mirroring
// NovaAudio_PreloadGameplayData (0x004b0740) which fills
// g_transition_sound_handle_table[i] with LoadStringResourceCopyById(0x96+i).
// Called before any boarding cue plays so the first beep doesn't hitch.
void EnsureTransitionSounds(GameState &state) {
  for (std::size_t i = 0; i < state.transition_sounds.size(); ++i) {
    if (state.transition_sounds[i].has_value()) {
      continue;
    }
    if (const auto resource =
            NovaResource_LoadSndData(static_cast<std::uint16_t>(150 + i))) {
      if (auto decoded = NovaSound_Decode(*resource)) {
        state.transition_sounds[i] = std::move(*decoded);
      }
    }
  }
}

// Queues a transition-table cue on GameState.pending_ui_sounds (drained by
// the spaceflight loop). `index` is the g_transition_sound_handle_table slot:
// 2 = confirm/plunder taken, 3 = denial/error, 4 = "boarded" fanfare.
void QueueUiSound(GameState &state, std::int16_t index, std::int16_t count) {
  state.pending_ui_sounds.push_back(GameState::PendingUiSound{index, count});
}

void ShowBoardingOverlay(GameState &state, std::uint16_t str_index) {
  auto text = NovaHud_LoadStringEntry(0x7d2, str_index);
  if (text.has_value()) {
    NovaHud_ShowOverlayMessage(state, std::move(*text));
  }
}

// The board command's proximity gate compares per-axis position deltas with
// half the target's current sprite frame spans x 0.5
// (Sprite_GetShotHalfSpan / Sprite_GetFrameVerticalHalfSpan in the original).
// The port has no per-ship sprite layer state, so the frame dimensions come
// from the class's sh\x8an descriptor (base frame size / 2). Ships without a
// decodable descriptor fall back to the provisional collision radius.
struct BoardRangeSpan {
  float half_x = 0.0F;
  float half_y = 0.0F;
};

[[nodiscard]] BoardRangeSpan TargetSpriteHalfSpan(const Ship &target) {
  if (const auto resource =
          NovaResource_Load(kShipVisualResourceType,
                            static_cast<std::uint16_t>(target.ship_class_id))) {
    if (const auto visual = DecodeShipVisualDescriptor(*resource)) {
      return {static_cast<float>(visual->base_x_size) / 2.0F,
              static_cast<float>(visual->base_y_size) / 2.0F};
    }
  }
  return {target.collision_radius_px, target.collision_radius_px};
}

} // namespace

// Ghidra 0x00415cb0 Ship_ResetShipAndAttackersAfterBoarding. Ships targeting
// the captured hull drop their combat state; the captured hull itself gets a
// combat/mission reset. TODO(decomp): the original also rolls a random voice
// type (ShipState.voice_type_mode) overridden by the class's
// inherent_attributes_govt voice mode; the port has no voice model.
void NovaBoarding_ResetShipAndAttackersAfterBoarding(GameState &state,
                                                     Ship &ship) {
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &other = state.ShipAt(slot);
    if (!other.is_active || other.ship_instance_id == ship.ship_instance_id) {
      continue;
    }
    if (other.primary_target_ship_slot != ship.ship_instance_id) {
      continue;
    }
    other.ai_state_code = 0;
    other.ai_control_mode = 0;
    other.primary_target_ship_slot = -1;
    other.ai_secondary_target_slot = -1;
    other.ai_hostility_accumulator = 0;
    other.target_stellar_object_id = -1;
  }
  ship.ai_state_code = 0;
  ship.ai_control_mode = 0;
  ship.primary_target_ship_slot = -1;
  ship.ai_secondary_target_slot = -1;
  ship.ai_hostility_accumulator = 0;
  ship.target_stellar_object_id = -1;
  ship.mission_ship_slot = -1;
}

// Ghidra 0x0045a3d0 Ship_HandlePlayerBoardTargetCommand.
//
// Clean-room summary (see docs/boarding_plunder_capture.md for the full map):
// Validates the player's board command against the primary target (fire
// -restricted target, same system, close range, matched heading, low relative
// velocity, target crew >= 1), then opens the boarding/plunder window (plain
// ships). Mission arms and the post-hit escort/fighter arms are TODO(decomp)
// below. Entry: called on the 'b' edge during flight; the target may be
// cleared by the dispatch arms. Exit: returns with the target boarded or an
// STR# 0x7d2 denial overlay queued. Confidence: high on gates, dispatch
// arms partially reconstructed.
void NovaBoarding_HandleBoardTargetCommand(SdlPlatform &platform,
                                           SdlAudio &audio,
                                           GameState &state) {
  // The original latches DAT_007354a5 ("player acted") for the frame-timing
  // refresh in Frame_SpaceflightLoop; not modelled here.
  EnsureTransitionSounds(state);

  if (state.player.primary_target_ship_slot == -1) {
    return;
  }
  if (NovaTargeting_ShipAtCloakVisibilityThreshold(state.player)) {
    return; // player cloaked past the visibility threshold: silent no-op
  }

  Ship &player = state.player;
  const std::int16_t target_slot = player.primary_target_ship_slot;
  if (target_slot < 1 ||
      !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
    return;
  }
  Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
  const ShipClass *target_class = state.scenario.Ship(
      static_cast<std::int16_t>(target.ship_class_id + 0x80));
  if (target_class == nullptr) {
    return;
  }

  // ---- Target eligibility (denial = STR# 0x7d2 pool 0x81) -----------------
  const bool rehired_or_surrendering =
      target.escort_rehired_mark == 0 ||
      (target.mission_fleet_slot == -1 && target.post_hit_mode_hint >= 0);
  const bool fire_restricted = NovaAiShip_IsFireRestricted(state, target);
  const bool eligible =
      rehired_or_surrendering && fire_restricted && target.is_active &&
      player.current_system_id == target.current_system_id &&
      target.mission_ship_slot != 0x3ff && !NovaAiShip_IsDestroyed(player);
  if (!eligible) {
    // Diagnosis aid: the original denies every non-disabled ship here too
    // (Ship_IsShipFireRestricted must be true). Log which predicate failed
    // with the armor state so in-game denials can be attributed.
    const ShipClass *diag_class = state.scenario.Ship(
        static_cast<std::int16_t>(target.ship_class_id + 0x80));
    NovaLog::Info(
        "board: target slot {} ({}) denied — rehired_mark={} "
        "fire_restricted={} active={} same_system={} mission_ship={:#x} "
        "player_destroyed={} armor={:.0f}/{} (disabled < {:.2f})",
        target_slot,
        diag_class != nullptr ? diag_class->display_name : "?",
        target.escort_rehired_mark,
        fire_restricted,
        target.is_active,
        player.current_system_id == target.current_system_id,
        target.mission_ship_slot,
        NovaAiShip_IsDestroyed(player),
        target.armor_points,
        diag_class != nullptr ? diag_class->base_armor : 0,
        diag_class != nullptr
            ? static_cast<float>(diag_class->base_armor) *
                  ((diag_class->capability_flags & 0x10) != 0 ? 0.1F
                                                              : 1.0F / 3.0F)
            : 0.0F);
    QueueUiSound(state, 3, 1);
    ShowBoardingOverlay(state, 0x81); // "You can't board this ship."
    return;
  }

  constexpr float kBoardVelocityGate = 0.5F; // _DAT_00575598
  constexpr float kBoardRangeShare = 0.5F;   // same constant, per-axis span
  constexpr float kBoardHeadingToleranceDeg = 30.0F;

  // ---- Relative velocity gate -------------------------------------------
  if (std::fabs(target.vel_x - player.vel_x) > kBoardVelocityGate ||
      std::fabs(target.vel_y - player.vel_y) > kBoardVelocityGate) {
    QueueUiSound(state, 3, 1);
    ShowBoardingOverlay(state, 0x83); // "You're moving too fast to board..."
    return;
  }

  // ---- Range gate (per-axis, half of the target's sprite frame) ----------
  const BoardRangeSpan span = TargetSpriteHalfSpan(target);
  if (std::fabs(target.pos_x - player.pos_x) > span.half_x * kBoardRangeShare ||
      std::fabs(target.pos_y - player.pos_y) > span.half_y * kBoardRangeShare) {
    QueueUiSound(state, 3, 1);
    ShowBoardingOverlay(state, 0x82); // "You're not close enough to board..."
    return;
  }

  // ---- Heading gate -------------------------------------------------------
  // Original works on integer-rounded degrees (0 = up, clockwise): the
  // heading delta must be within 30 deg, or the target aligned 180 deg
  // (docked nose-to-tail) within 30 deg. Silent return on failure.
  const auto heading_deg = [](float radians) {
    return static_cast<int>(
        std::llround(radians * (180.0F / 3.14159265358979F)));
  };
  const auto shortest_delta = [](int a, int b) {
    int delta = (a - b) % 360;
    if (delta > 180) {
      delta -= 360;
    }
    if (delta < -180) {
      delta += 360;
    }
    return std::abs(delta);
  };
  const int player_heading = heading_deg(player.heading);
  const int target_heading = heading_deg(target.heading);
  if (shortest_delta(player_heading, target_heading) >
          static_cast<int>(kBoardHeadingToleranceDeg) &&
      shortest_delta(player_heading, target_heading + 180) >
          static_cast<int>(kBoardHeadingToleranceDeg)) {
    return;
  }

  // ---- Boardability -------------------------------------------------------
  // Mission-fleet arms (bounty target / cargo pickup / escort repair) are
  // TODO(decomp): the mission-ship def table is not modelled; those arms
  // also cover the STR# 0x7d2 0x7e/0x7f "repaired" messages.
  if (target.mission_fleet_slot != -1) {
    NovaLog::Todo("board: mission-fleet target boarded (slot {}); mission "
                  "arms of Ship_HandlePlayerBoardTargetCommand not "
                  "reconstructed",
                  target.mission_fleet_slot);
    return;
  }
  // Plain ships: Bible "Ships with 0 crew can't be boarded".
  if (target_class->crew < 1) {
    NovaLog::Info("board: target slot {} ({}) denied — crew 0",
                  target_slot,
                  target_class->display_name);
    QueueUiSound(state, 3, 1);
    ShowBoardingOverlay(state, 0x81); // "You can't board this ship."
    return;
  }

  // ---- Dispatch -----------------------------------------------------------
  // Velocity match (the original copies the target's velocity to the player
  // before the window opens).
  player.vel_x = target.vel_x;
  player.vel_y = target.vel_y;

  // Faction reaction to a plain boarding: Government_ProcessFactionCombatEvent
  // (0x00466fc0) with the "boarded" event code 2. The port does not model the
  // faction-combat reaction yet (the negotiation dialog logged the same skip).
  // Government_PropagateHostilityFromAttack (0x00467xxx) in the
  // post-hit arms is likewise deferred.
  NovaLog::Todo("board: Government_ProcessFactionCombatEvent "
                "(0x00466fc0) not reconstructed");

  // Post-hit arms (post_hit_mode_hint 0/-1 => "Fighter captured." carrier-bay
  // conversion; hint >= 1 with escort capacity => direct escort conversion)
  // depend on ShipClass_CanPlayerCaptureShipClass (0x004694a0, the fighter-bay
  // outfit scan), which the port does not model yet. They are reachable only
  // for carriers that surrendered after combat; the plain plunder window
  // handles the common path. TODO(decomp).
  if (target.post_hit_mode_hint >= 1 &&
      NovaShip_CanPlayerHaveMoreEscorts(state)) {
    NovaLog::Todo("board: post-hit escort conversion arm skipped "
                  "(post_hit_mode_hint >= 1)");
  }

  QueueUiSound(state, 4, 8); // the "boarded" cue repeats 8x in the original
  const BoardingWindowResult result =
      NovaBoarding_RunWindow(platform, audio, state);
  (void)result;

  // After the interaction: latch + clear every ship targeting the boarded
  // hull (Ship_ClearOtherShipsTargetingShip 0x00415dc0; the port's
  // destroyed-reference helper performs the same clearing).
  target.escort_rehired_mark = 1;
  NovaTargeting_ClearDestroyedShipReferences(state, target.ship_instance_id);
}

namespace {

// Ghidra 0x00482940 NovaUi_RunBoardingPlunderWindow — clean-room port
// (iteration 3). DLOG 0x3f3 (309x198), backdrop PICT 0x2143, DITL 0x3f3 items:
//   [0] Abort (STR# 0x96 0x22, 91..217 x 166..191)
//   [1] Cargo (0x27, 110..199 x 110..135)
//   [2] Credits (0x28, 35..124 x 138..163)
//   [3] Ammo (0x29, 204..293 x 110..135)
//   [4] text panel (UserItem, 11..298 x 7..103)
//   [5] Energy (0x2a, 16..105 x 110..135)
//   [6] Capture Ship (0x2b, 129..275 x 138..163)
// Items 0..3 then 5..6 are the six option buttons (entry 4, the text panel, is
// skipped); UiPanel entry indices are 1-based, so the action codes read back
// by NovaUi_PollTravelScriptAction are 1,2,3,4,6,7 (no code 5). The window is
// centred on the 640x480 playfield.
constexpr float kBoardWindowX = (640.0F - 309.0F) / 2.0F; // 165.5
constexpr float kBoardWindowY = (480.0F - 198.0F) / 2.0F; // 141.0
constexpr float kBoardWindowW = 309.0F;
constexpr float kBoardWindowH = 198.0F;
constexpr std::uint16_t kBoardBackdropPict = 0x2143;

// STR# 0x96 button-label pool: Abort/Cargo/Credits/Ammo/Energy/Capture Ship.
constexpr std::uint16_t kButtonLabelStr = 0x96;
constexpr std::uint16_t kBtnAbort = 0x22;
constexpr std::uint16_t kBtnCargo = 0x27;
constexpr std::uint16_t kBtnCredits = 0x28;
constexpr std::uint16_t kBtnAmmo = 0x29;
constexpr std::uint16_t kBtnEnergy = 0x2a;
constexpr std::uint16_t kBtnCaptureShip = 0x2b;

// Action codes returned for each option button (NovaUi_PollTravelScriptAction).
constexpr unsigned kActionAbort = 1;
constexpr unsigned kActionCargo = 2;
constexpr unsigned kActionCredits = 3;
constexpr unsigned kActionAmmo = 4;
constexpr unsigned kActionEnergy = 6;
constexpr unsigned kActionCapture = 7;

// STR# 0x7d2 "misc strings" used by the window (verified against the shipped
// pool; see docs/reference/boarding.jpg for the shipped window layout).
constexpr std::uint16_t kMiscStr = 0x7d2;
constexpr std::uint16_t kMiscTonWord = 0x00;          // "ton"
constexpr std::uint16_t kMiscTonsWord = 0x01;         // "tons"
constexpr std::uint16_t kMiscTitle = 0x6c;            // "Select what to
                                                      // plunder from this
                                                      // ship:"
constexpr std::uint16_t kMiscCargoLabel = 0x6d;       // "Cargo:"
constexpr std::uint16_t kMiscAmmoLabel = 0x6e;        // "Ammo:"
constexpr std::uint16_t kMiscCaptureOddsLabel = 0x6f; // "Capture Odds:"
constexpr std::uint16_t kMiscCreditsLabel = 0x20;     // "credits"
constexpr std::uint16_t kMiscEnergyLabel = 0x06;      // "Energy:"
constexpr std::uint16_t kMiscStoleAll = 0x73;         // "You stole all the"
constexpr std::uint16_t kMiscSalvaged = 0x72;         // "You salvaged"
constexpr std::uint16_t kMiscFromThisShip = 0x6b;     // "from this ship."
constexpr std::uint16_t kMiscOfWord = 0x186;          // "of"
constexpr std::uint16_t kMiscNoOffer = 0x14f;         // "none"
constexpr std::uint16_t kMiscCargoFull = 0x71;        // "You couldn't store any
                                                      // of the cargo..."
constexpr std::uint16_t kMiscAmmoFull = 0x74;         // "...any of the ammo..."
constexpr std::uint16_t kMiscSelfDestruct = 0x70;     // "Oops! You tripped..."
constexpr std::uint16_t kMiscCaptureFailed = 0x7c;  // "Your attempt to capture
                                                    // this ship was
                                                    // unsuccessful."
constexpr std::uint16_t kMiscEscortCap = 0x7b;      // "You already have the
                                                    // maximum possible
                                                    // number of escorts."
constexpr std::uint16_t kMiscAssignedEscort = 0x7a; // "You assigned this ship
                                                    // to your fleet of
                                                    // escorts."
// Fuel/energy-transfer overlays (STR# 0x7d2 entries 3..5, DAT_0072d6cc/d7cc/
// d8cc per the original loader).
constexpr std::uint16_t kMiscFuelNowFull = 0x03;  // "You filled your reactors
                                                  // and batteries..."
constexpr std::uint16_t kMiscFuelStole = 0x04;    // "You transferred all of
                                                  // this ship's energy..."
constexpr std::uint16_t kMiscFuelTankFull = 0x05; // "You couldn't store any of
                                                  // the energy..."

// Cargo commodity names: the original loader fills DAT_0069d2cc[cargo_type]
// via Resource_LoadStringEntry(0xfa1, cargo_type + 1) (FUN_004c7040) — and
// since that helper is 1-BASED (see 0x004b8ca0), that is 0-based pool entry
// cargo_type: food / industrial goods / medical supplies / luxury goods /
// metal / equipment. The boarding roll only ever uses types 0..5.
constexpr std::uint16_t kCargoNameStr = 0xfa1;

// Panic multipliers after each loot action (Ghidra doubles 00575900/5908/
// 58e0). Applied to the window's panic value; the self-destruct roll on the
// next iteration uses it.
constexpr double kPanicCargo = 2.0;
constexpr double kPanicCredits = 1.25;
constexpr double kPanicAmmo = 2.0;
constexpr double kPanicEnergy = 1.5;

// Capture conversion restores armor to this fraction of the class max armor
// (Ghidra double 005758a0 = 0.5).
constexpr float kCapturedArmorFraction = 0.5F;

// Dialog colours (labels DAT_00733b50, values PTR_DAT_00575ad8, dimmed
// DAT_00733b56, panel fill black).
constexpr SDL_Color kBoardLabel{192, 192, 192, 255};
constexpr SDL_Color kBoardValue{255, 255, 255, 255};
constexpr SDL_Color kBoardDim{128, 128, 128, 255};
constexpr SDL_Color kBoardPanelBg{0, 0, 0, 255};

// Loads one PICT into a texture (null on failure), mirroring the other modal
// dialogs.
std::unique_ptr<SdlTexture> LoadBoardPictTexture(SdlPlatform &platform,
                                                 std::uint16_t pict_id) {
  const auto data = NovaResource_LoadPictData(pict_id);
  if (!data) {
    return {};
  }
  const auto img = Resource_LoadPictAsImage(*data);
  if (!img) {
    return {};
  }
  return SdlTexture::Create(
      platform.renderer(), img->width, img->height, img->rgba_pixels);
}

// Loads a STR# 0x96 button label with a fallback.
std::string LoadBoardButtonLabel(std::uint16_t index) {
  if (auto s = NovaHud_LoadStringEntry(kButtonLabelStr, index)) {
    return *s;
  }
  return "?";
}

// Loads a STR# 0x7d2 overlay fragment with a fallback.
std::string LoadBoardMiscString(std::uint16_t index, std::string fallback) {
  if (auto s = NovaHud_LoadStringEntry(kMiscStr, index)) {
    return *s;
  }
  return fallback;
}

// Ghidra DrawContext_DrawGroupedUInt: decimal digits grouped in threes with
// commas, e.g. 26600 -> "26,600".
std::string GroupedUInt(std::int32_t value) {
  std::string digits = std::to_string(value);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) {
      out.push_back(',');
    }
    out.push_back(digits[i]);
  }
  return out;
}

// The STR# pool stores "credits" lowercase; the shipped window draws the row
// label capitalized ("Credits:", see docs/reference/boarding.jpg).
std::string Capitalized(std::string text) {
  if (!text.empty()) {
    text[0] =
        static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
  }
  return text;
}

// The commodity name for a cargo type (STR# 0xfa1 entry cargo_type).
std::string CargoName(const GameState &state, int cargo_type) {
  if (cargo_type < 0 || cargo_type > 5) {
    return "?";
  }
  (void)state;
  if (auto s = NovaHud_LoadStringEntry(
          kCargoNameStr, static_cast<std::uint16_t>(cargo_type))) {
    return *s;
  }
  // Fallbacks mirror the six standard boarding commodities.
  static constexpr const char *kFallback[6] = {"food",
                                               "industrial goods",
                                               "medical supplies",
                                               "luxury goods",
                                               "metal",
                                               "equipment"};
  return kFallback[static_cast<std::size_t>(cargo_type)];
}

// One boarding-window option button.
struct BoardButton {
  SDL_FRect rect;            // absolute 640x480 screen rect
  std::uint8_t action_code;  // 1 (Abort) .. 7 (Capture), skipping 5
  std::uint16_t label_index; // STR# 0x96 index
  std::string label;
};

// Builds the six option buttons from the DITL item rects (absolute screen
// coords). Order matches the hit-test set (items 0..3 then 5..6).
std::array<BoardButton, 6> BuildBoardButtons() {
  const auto abs = [](float x, float y, float w, float h) {
    return SDL_FRect{kBoardWindowX + x, kBoardWindowY + y, w, h};
  };
  return std::array<BoardButton, 6>{
      BoardButton{abs(91.0F, 166.0F, 126.0F, 25.0F),
                  kActionAbort,
                  kBtnAbort,
                  LoadBoardButtonLabel(kBtnAbort)},
      BoardButton{abs(110.0F, 110.0F, 89.0F, 25.0F),
                  kActionCargo,
                  kBtnCargo,
                  LoadBoardButtonLabel(kBtnCargo)},
      BoardButton{abs(35.0F, 138.0F, 89.0F, 25.0F),
                  kActionCredits,
                  kBtnCredits,
                  LoadBoardButtonLabel(kBtnCredits)},
      BoardButton{abs(204.0F, 110.0F, 89.0F, 25.0F),
                  kActionAmmo,
                  kBtnAmmo,
                  LoadBoardButtonLabel(kBtnAmmo)},
      BoardButton{abs(16.0F, 110.0F, 89.0F, 25.0F),
                  kActionEnergy,
                  kBtnEnergy,
                  LoadBoardButtonLabel(kBtnEnergy)},
      BoardButton{abs(129.0F, 138.0F, 146.0F, 25.0F),
                  kActionCapture,
                  kBtnCaptureShip,
                  LoadBoardButtonLabel(kBtnCaptureShip)},
  };
}

// Finds the zero-based scenario outfit index whose ModType-3 (weapon) mod pair
// carries ModVal == `weapon_bank`; -1 when none does. Mirrors the scan in
// NovaUi_DrawBoardingPlunderWindow / HandleBoardingPlunderOptionButtons.
std::int16_t FindWeaponOutfitIndex(const GameState &state, int weapon_bank) {
  for (std::size_t i = 0; i < state.scenario.outfits.size(); ++i) {
    const Outfit &outfit = state.scenario.outfits[i];
    if (OutfitModPairAt(outfit, 0).type == 3 &&
        OutfitModPairAt(outfit, 0).val == weapon_bank) {
      return static_cast<std::int16_t>(i);
    }
    for (int pair = 1; pair < kOutfitModPairCount; ++pair) {
      const OutfitModPair mod = OutfitModPairAt(outfit, pair);
      if (mod.type == 3 && mod.val == weapon_bank) {
        return static_cast<std::int16_t>(i);
      }
    }
  }
  return -1;
}

// The singular/plural weapon display name for a boarded ammo offer (the
// original's DAT_0063cccc/65cccc outfit LCName/LCPlural tables).
std::string
WeaponOfferName(const GameState &state, int ammo_bank, int quantity) {
  const std::int16_t index = FindWeaponOutfitIndex(state, ammo_bank);
  if (index < 0 ||
      static_cast<std::size_t>(index) >= state.scenario.outfits.size()) {
    return "?";
  }
  const Outfit &outfit =
      state.scenario.outfits[static_cast<std::size_t>(index)];
  return quantity < 2
             ? (outfit.lc_name.empty() ? outfit.name : outfit.lc_name)
             : (outfit.lc_plural.empty() ? outfit.name : outfit.lc_plural);
}

// Ghidra 0x004a24e0 NovaUi_DrawBoardingPlunderOptionButtons. Draws the six
// option strips from the shared three-state button art. Enabled buttons use
// the normal art (or the pressed/hover art when `hovered`); disabled buttons
// (no offer / capture odds < 1) use the grey art.
void DrawBoardOptionButtons(SdlPlatform &platform,
                            NovaFontCache &font_cache,
                            const ServicesButtonArt &art,
                            const std::array<BoardButton, 6> &buttons,
                            const BoardingPlunderOptions &options,
                            int hovered) {
  const bool enabled[6] = {
      true,                    // Abort always enabled
      options.cargo_offer(),   // Cargo
      options.credits_offer(), // Credits
      options.ammo_offer(),    // Ammo
      options.fuel_offer(),    // Energy
      options.capture_offer(), // Capture Ship
  };
  constexpr SDL_Color kButtonLabel{255, 255, 255, 255};
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    const BoardButton &b = buttons[i];
    const ButtonState state =
        !enabled[i] ? ButtonState::kDisabled
                    : (hovered == static_cast<int>(i) ? ButtonState::kHover
                                                      : ButtonState::kNormal);
    art.Draw(platform, b.rect, state);
    NovaText_DrawCentered(platform,
                          font_cache,
                          kThreeStateButtonFontFamily,
                          kThreeStateButtonFontSize,
                          kNovaFontStyleRegular,
                          kButtonLabel,
                          b.rect.x,
                          b.rect.x + b.rect.w,
                          ThreeStateButtonLabelBaseline(b.rect),
                          b.label);
  }
}

// Ghidra 0x00484d30 NovaUi_DrawBoardingPlunderWindow. Draws the window
// backdrop (PICT 0x2143) and the offers into the DITL item-4 text panel.
// Layout per the shipped window (docs/reference/boarding.jpg):
//   y+12  "Select what to plunder from this ship:" (title)
//   y+28  "Cargo:"   | x+50  "<qty> <ton(s)> of <commodity>"
//   y+42  "Credits:" | x+50  grouped credits
//   y+56  "Ammo:"    | x+50  "<qty> <weapon>"
//   y+70  "Energy:" (x+1) | fuel qty (x+50) | "Capture Odds:" (x+120)
//         odds% "%." (x+195)
// No-offer values render as STR# 0x7d2 0x14f ("none") in the dimmed colour.
// The space below these rows is the status line: empty by default (the
// "Oops! ... self-destruct" string appears only as the HUD overlay when a
// self-destruct actually fires, never as a panel row).
void DrawBoardWindow(SdlPlatform &platform,
                     NovaFontCache &font_cache,
                     const ServicesButtonArt &art,
                     const std::array<BoardButton, 6> &buttons,
                     const BoardingPlunderOptions &options,
                     const GameState &state,
                     SDL_Texture *backdrop,
                     int hovered) {
  SDL_Renderer *renderer = platform.renderer();
  // Full-screen dim scrim behind the window.
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  platform.SetCenteredPlayfield();
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
  const SDL_FRect field{0.0F, 0.0F, 640.0F, 480.0F};
  SDL_RenderFillRect(renderer, &field);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const SDL_FRect window{
      kBoardWindowX, kBoardWindowY, kBoardWindowW, kBoardWindowH};
  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &window);
  } else {
    SDL_SetRenderDrawColor(renderer, 16, 40, 72, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &window);
    SDL_SetRenderDrawColor(renderer, 80, 140, 190, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &window);
  }

  // Text panel (DITL item 4): 11..298 x 7..103 in window coords, filled
  // black; labels/values are drawn at panel-relative offsets.
  const SDL_FRect panel{
      kBoardWindowX + 11.0F, kBoardWindowY + 7.0F, 287.0F, 96.0F};
  SDL_SetRenderDrawColor(renderer,
                         kBoardPanelBg.r,
                         kBoardPanelBg.g,
                         kBoardPanelBg.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &panel);
  const float px = panel.x;
  const float py = panel.y;

  const auto draw_text = [&](float x,
                             float y,
                             const std::string &text,
                             SDL_Color color,
                             float size = 12.0F) {
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  size,
                  kNovaFontStyleRegular,
                  color,
                  x,
                  y,
                  text);
  };

  // Title row, then the label column.
  draw_text(
      px,
      py + 12.0F,
      LoadBoardMiscString(kMiscTitle, "Select what to plunder from this ship:"),
      kBoardValue);
  draw_text(px,
            py + 28.0F,
            LoadBoardMiscString(kMiscCargoLabel, "Cargo:"),
            kBoardLabel);
  draw_text(px,
            py + 42.0F,
            Capitalized(LoadBoardMiscString(kMiscCreditsLabel, "credits")) +
                ":",
            kBoardLabel);
  draw_text(px,
            py + 56.0F,
            LoadBoardMiscString(kMiscAmmoLabel, "Ammo:"),
            kBoardLabel);

  // Value column (panel x + 50).
  const float vx = px + 50.0F;
  const std::string none = LoadBoardMiscString(kMiscNoOffer, "none");
  if (options.cargo_type == -1) {
    draw_text(vx, py + 28.0F, none, kBoardDim);
  } else {
    const std::string qty =
        fmt::format("{}", static_cast<int>(options.cargo_quantity));
    const std::string ton_word =
        options.cargo_quantity == 1
            ? LoadBoardMiscString(kMiscTonWord, "ton")
            : LoadBoardMiscString(kMiscTonsWord, "tons");
    const std::string text = qty + " " + ton_word + " " +
                             LoadBoardMiscString(kMiscOfWord, "of") + " " +
                             CargoName(state, options.cargo_type);
    draw_text(vx, py + 28.0F, text, kBoardValue);
  }
  if (options.credits < 1) {
    draw_text(vx, py + 42.0F, none, kBoardDim);
  } else {
    // DrawGroupedUInt: thousands-separated, e.g. "26,600".
    draw_text(vx, py + 42.0F, GroupedUInt(options.credits), kBoardValue);
  }
  if (options.ammo_bank == -1) {
    draw_text(vx, py + 56.0F, none, kBoardDim);
  } else {
    const std::string qty =
        fmt::format("{}", static_cast<int>(options.ammo_quantity));
    const std::string text =
        qty + " " +
        WeaponOfferName(state, options.ammo_bank, options.ammo_quantity);
    draw_text(vx, py + 56.0F, text, kBoardValue);
  }

  // Energy / odds row (panel y + 70): "Energy:" at x+1, fuel qty at x+50,
  // "Capture Odds:" at x+120 and the odds% + "%" at x+195.
  draw_text(px + 1.0F,
            py + 70.0F,
            LoadBoardMiscString(kMiscEnergyLabel, "Energy:"),
            kBoardLabel);
  if (options.fuel_quantity < 1) {
    draw_text(px + 50.0F, py + 70.0F, none, kBoardDim);
  } else {
    draw_text(px + 50.0F,
              py + 70.0F,
              fmt::format("{}", static_cast<int>(options.fuel_quantity)),
              kBoardValue);
  }
  draw_text(px + 120.0F,
            py + 70.0F,
            LoadBoardMiscString(kMiscCaptureOddsLabel, "Capture Odds:"),
            kBoardLabel);
  draw_text(px + 195.0F,
            py + 70.0F,
            fmt::format("{}%.", static_cast<int>(options.capture_odds_percent)),
            kBoardValue);

  // Option buttons.
  DrawBoardOptionButtons(platform, font_cache, art, buttons, options, hovered);
}

// Plays a transition-table cue directly through the flight-loop-owned audio
// device (the modal owns no device). Mirrors NovaEffects_QueueCenteredResource
// on g_transition_sound_handle_table[index] with `count` repeats; index 2 =
// confirm/taken, 3 = denial/error.
void PlayTransitionCue(SdlAudio &audio,
                       GameState &state,
                       std::int16_t index,
                       std::int16_t count = 1) {
  EnsureTransitionSounds(state);
  if (index < 0 ||
      index >= static_cast<std::int16_t>(state.transition_sounds.size())) {
    return;
  }
  const auto &sound = state.transition_sounds[static_cast<std::size_t>(index)];
  if (!sound.has_value()) {
    return;
  }
  for (std::int16_t repeat = 0; repeat < count; ++repeat) {
    audio.Play(*sound, 1.0F, 1.0F, 150 + index);
  }
}

void BoardShowOverlay(GameState &state,
                      std::uint16_t str_index,
                      std::string fallback) {
  NovaHud_ShowOverlayMessage(
      state, LoadBoardMiscString(str_index, std::move(fallback)));
}

// The roll => target self-destructs (shields/armor zeroed, death timer armed)
// with the "Oops" overlay. Re-derives the boarded hull from the player's
// primary target slot (the one the window was opened on).
void SelfDestructTarget(GameState &state) {
  const std::int16_t slot = state.player.primary_target_ship_slot;
  if (slot < 1 || !state.SlotInRange(static_cast<std::size_t>(slot))) {
    return;
  }
  Ship &hull = state.ShipAt(static_cast<std::size_t>(slot));
  hull.shield_points = 0.0F;
  hull.armor_points = 0.0F;
  hull.death_timer_active = 0.0F;
  BoardShowOverlay(state,
                   kMiscSelfDestruct,
                   "Oops! You tripped this ship's security "
                   "self-destruct mechanism.");
}

} // namespace

// Ghidra 0x00482940 NovaUi_RunBoardingPlunderWindow (clean-room, iteration 3).
// Builds the offers, then runs the modal loop over DLOG 0x3f3 until the player
// aborts or captures the target. Each loot action applies its transfer to
// GameState, multiplies the panic value and re-arms the self-destruct re-roll
// (checked on the next loop iteration). The capture arm converts the boarded
// hull to a behavior-6 escort (the capture-decision dialog / ship swap,
// NovaUi_ShowCaptureDecisionDialog 0x00497eb0, is TODO(decomp)). The window
// plays its one-shot cues directly through `audio` (the flight loop owns the
// device).
[[nodiscard]] BoardingWindowResult NovaBoarding_RunWindow(SdlPlatform &platform,
                                                          SdlAudio &audio,
                                                          GameState &state) {
  BoardingWindowResult result;

  const std::int16_t target_slot = state.player.primary_target_ship_slot;
  if (target_slot < 1 ||
      !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
    return result; // target lost while the command ran
  }

  // The window's panic value: rand(0x1a) + 0xf (15..40). Each loot action
  // multiplies it; the self-destruct re-roll (armed by a loot action) fires
  // when rand(100) <= panic.
  std::int32_t panic = NovaRandomRange(state.rng, 0x1a) + 0xf;

  BoardingPlunderOptions options = NovaBoarding_BuildOptions(state);
  NovaLog::Info(
      "board: opening plunder window — cargo({})={}x credits={} ammo({})={}x "
      "fuel={} capture_odds={}% panic={}",
      options.cargo_type,
      options.cargo_quantity,
      options.credits,
      options.ammo_bank,
      options.ammo_quantity,
      options.fuel_quantity,
      options.capture_odds_percent,
      panic);

  // Mission-ship free-outfit bonus arm (the opening block that grants a random
  // free outfit to mission ships with booty_ammo_max) is TODO(decomp): the
  // mission-ship def table is not modelled.
  if (state.ShipAt(static_cast<std::size_t>(target_slot)).mission_fleet_slot !=
      -1) {
    NovaLog::Todo("board: mission-ship free-outfit bonus arm of "
                  "NovaUi_RunBoardingPlunderWindow not reconstructed");
  }

  // ---- Assets -------------------------------------------------------------
  auto backdrop = LoadBoardPictTexture(platform, kBoardBackdropPict);
  if (!backdrop) {
    NovaLog::Todo("board: window backdrop PICT 0x2143 unavailable; drawing a "
                  "bordered placeholder");
  }
  ServicesButtonArt art;
  if (!art.Initialize(platform)) {
    NovaLog::Warn("three-state button art unavailable for the boarding window");
  }
  NovaFontCache font_cache;
  platform.SetCenteredPlayfield();
  const std::array<BoardButton, 6> buttons = BuildBoardButtons();

  // NovaInputQueue_FlushAllCommands: the original discards pending input when
  // the window opens, so the 'b' keypress that opened it (or a queued click)
  // can't dispatch a phantom action on the first frame.
  while (platform.PollTextEvent().has_value()) {
  }

  // local_223: the self-destruct re-roll latch (armed by a loot action).
  bool panic_armed = false;
  bool close = false;

  while (!platform.quit_requested() && !close) {
    // Mouse-hover (mirrors NovaUi_HandleBoardingPlunderOptionButtons 0x004a22e0
    // filter: disabled slots are excluded from the hovered index).
    int hovered = -1;
    const SDL_FPoint mouse = platform.mouse_position();
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      bool slot_enabled = true;
      switch (buttons[i].action_code) {
      case kActionAbort:
        slot_enabled = true;
        break;
      case kActionCargo:
        slot_enabled = options.cargo_offer();
        break;
      case kActionCredits:
        slot_enabled = options.credits_offer();
        break;
      case kActionAmmo:
        slot_enabled = options.ammo_offer();
        break;
      case kActionEnergy:
        slot_enabled = options.fuel_offer();
        break;
      case kActionCapture:
        slot_enabled = options.capture_offer();
        break;
      default:
        break;
      }
      if (slot_enabled && SDL_PointInRectFloat(&mouse, &buttons[i].rect)) {
        hovered = static_cast<int>(i);
        break;
      }
    }

    DrawBoardWindow(platform,
                    font_cache,
                    art,
                    buttons,
                    options,
                    state,
                    backdrop ? backdrop->get() : nullptr,
                    hovered);
    SDL_RenderPresent(platform.renderer());

    // Poll for a button action (1..7). Esc/Enter = Abort; left-click hits the
    // hovered option button (the original has no keyboard shortcuts beyond
    // that).
    int action = 0;
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      if (in->key == TextKey::escape || in->key == TextKey::enter) {
        action = static_cast<int>(kActionAbort);
        break;
      }
      if (in->key == TextKey::primary) {
        const SDL_FPoint click = platform.mouse_position();
        for (std::size_t i = 0; i < buttons.size(); ++i) {
          if (SDL_PointInRectFloat(&click, &buttons[i].rect)) {
            action = static_cast<int>(buttons[i].action_code);
            break;
          }
        }
        break;
      }
    }

    // Abort (action 1): panic = -1, disarm, close with the confirm cue.
    if (action == static_cast<int>(kActionAbort)) {
      panic = -1;
      panic_armed = false;
      PlayTransitionCue(audio, state, 2);
      close = true;
    }

    // Self-destruct re-roll (armed by the previous iteration's loot action).
    if (panic_armed && NovaRandomRange(state.rng, 100) <= panic) {
      SelfDestructTarget(state);
      PlayTransitionCue(audio, state, 2);
      result.target_self_destructed = true;
      close = true;
      break;
    }
    panic_armed = false;

    // ---- Cargo (action 2) -----
    if (action == static_cast<int>(kActionCargo)) {
      if (options.cargo_type == -1) {
        PlayTransitionCue(audio, state, 3); // denial beep
      } else {
        // Clamp quantity to the free fleet cargo space.
        const std::int16_t total = Outfit_ComputePlayerCargoAndJunkTotal(state);
        const std::int16_t capacity =
            Outfit_ComputePlayerFleetCargoCapacity(state);
        if (capacity - total < options.cargo_quantity) {
          options.cargo_quantity = static_cast<std::int16_t>(capacity - total);
        }
        if (options.cargo_quantity < 1) {
          PlayTransitionCue(audio, state, 2);
          BoardShowOverlay(
              state,
              kMiscCargoFull,
              "You couldn't store any of the cargo, so you left it.");
        } else {
          PlayTransitionCue(audio, state, 2);
          // "You salvaged <qty> <ton(s)> of <commodity> from this ship."
          const std::string text =
              LoadBoardMiscString(kMiscSalvaged, "You salvaged") + " " +
              fmt::format("{}", static_cast<int>(options.cargo_quantity)) +
              " " +
              (options.cargo_quantity == 1
                   ? LoadBoardMiscString(kMiscTonWord, "ton")
                   : LoadBoardMiscString(kMiscTonsWord, "tons")) +
              " " + LoadBoardMiscString(kMiscOfWord, "of") + " " +
              CargoName(state, options.cargo_type) + " " +
              LoadBoardMiscString(kMiscFromThisShip, "from this ship.");
          NovaHud_ShowOverlayMessage(state, text);
          if (options.cargo_type >= 0 && options.cargo_type < 6) {
            state.inventory
                .cargo_bins[static_cast<std::size_t>(options.cargo_type)] +=
                options.cargo_quantity;
          }
          options.cargo_quantity = 0;
          options.cargo_type = -1;
        }
        panic = RoundHalfUp(static_cast<double>(panic) * kPanicCargo);
        panic_armed = true;
      }
    }

    // ---- Credits (action 3) -----
    if (action == static_cast<int>(kActionCredits)) {
      if (options.credits < 1) {
        PlayTransitionCue(audio, state, 3);
      } else {
        PlayTransitionCue(audio, state, 2);
        // "You stole all the <credits> credits from this ship."
        const std::string text =
            LoadBoardMiscString(kMiscStoleAll, "You stole all the") + " " +
            GroupedUInt(options.credits) + " " +
            Capitalized(LoadBoardMiscString(kMiscCreditsLabel, "credits")) +
            " " + LoadBoardMiscString(kMiscFromThisShip, "from this ship.");
        NovaHud_ShowOverlayMessage(state, text);
        state.player.credits += options.credits;
        options.credits = 0;
        panic = RoundHalfUp(static_cast<double>(panic) * kPanicCredits);
        panic_armed = true;
      }
    }

    // ---- Ammo (action 4) -----
    if (action == static_cast<int>(kActionAmmo)) {
      const std::int16_t outfit_index =
          FindWeaponOutfitIndex(state, options.ammo_bank);
      if (options.ammo_bank == -1 || outfit_index < 0) {
        PlayTransitionCue(audio, state, 3);
      } else {
        const std::int16_t resource_id =
            static_cast<std::int16_t>(0x80 + outfit_index);
        int transferred = 0;
        // Transfer up to the offer while the ownership maximum allows (the
        // original's mass gate Ship_ComputeShipCurrentMass is approximated by
        // the ownership clamp; TODO(decomp): free-mass accounting).
        while (transferred < options.ammo_quantity) {
          const OutfitOwnership own =
              Outfit_ClampOwnedCountToLimits(state, resource_id);
          if (own.effective_owned >= own.max_allowed) {
            break;
          }
          state.weapon_bank_secondary[static_cast<std::size_t>(
                                          options.ammo_bank) *
                                      100] += 1;
          ++transferred;
        }
        if (transferred < 1) {
          PlayTransitionCue(audio, state, 2);
          BoardShowOverlay(state, kMiscAmmoFull, "couldn't store any ammo.");
        } else {
          PlayTransitionCue(audio, state, 2);
          // "You salvaged <n> <weapon(s)> from this ship."
          const std::string text =
              LoadBoardMiscString(kMiscSalvaged, "You salvaged") + " " +
              fmt::format("{}", transferred) + " " +
              WeaponOfferName(state, options.ammo_bank, transferred) + " " +
              LoadBoardMiscString(kMiscFromThisShip, "from this ship.");
          NovaHud_ShowOverlayMessage(state, text);
        }
        options.ammo_quantity = 0;
        options.ammo_bank = -1;
        panic = RoundHalfUp(static_cast<double>(panic) * kPanicAmmo);
        panic_armed = true;
      }
    }

    // ---- Energy (action 6) -----
    if (action == static_cast<int>(kActionEnergy)) {
      if (options.fuel_quantity < 1) {
        PlayTransitionCue(audio, state, 3);
      } else {
        PlayTransitionCue(audio, state, 2);
        const float capacity = state.cached_stats.fuel_capacity;
        const double available = static_cast<double>(capacity) -
                                 static_cast<double>(state.player.fuel_points);
        int fill = static_cast<int>(std::llround(available));
        if (fill > options.fuel_quantity) {
          fill = options.fuel_quantity;
        }
        if (fill < 1) {
          BoardShowOverlay(
              state, kMiscFuelTankFull, "Your fuel tanks are already full.");
        } else {
          state.player.fuel_points += static_cast<float>(fill);
          if (state.player.fuel_points >= capacity) {
            BoardShowOverlay(
                state, kMiscFuelNowFull, "Your fuel tanks are now full.");
          } else {
            BoardShowOverlay(
                state, kMiscFuelStole, fmt::format("You took {} fuel.", fill));
          }
        }
        options.fuel_quantity = 0;
        panic = RoundHalfUp(static_cast<double>(panic) * kPanicEnergy);
        panic_armed = true;
      }
    }

    // ---- Capture (action 7) -----
    if (action == static_cast<int>(kActionCapture)) {
      Ship &target = state.ShipAt(static_cast<std::size_t>(target_slot));
      // Derelict-government ships can never be captured (the roll becomes -1).
      if (target.faction_or_government_id != -1) {
        if (const Government *g =
                state.scenario.Government(target.faction_or_government_id);
            g != nullptr && (g->flags_primary & 0x800U) != 0U) {
          options.capture_odds_percent = -1;
        }
      }
      const std::int16_t roll = NovaRandomRange(state.rng, 100);
      const bool auto_fail = options.capture_odds_percent < 1;
      if (roll > options.capture_odds_percent || auto_fail) {
        close = true;
        PlayTransitionCue(audio, state, 3);
        BoardShowOverlay(state,
                         kMiscCaptureFailed,
                         "Your attempt to capture this ship was unsuccessful.");
        result.capture_attempt_failed = true;
      } else {
        // 1-in-10 "Oops" self-destruct roll.
        if (NovaRandomRange(state.rng, 10) == 0) {
          close = true;
          SelfDestructTarget(state);
          PlayTransitionCue(audio, state, 2);
          result.target_self_destructed = true;
        } else {
          PlayTransitionCue(audio, state, 2);
          if (!NovaShip_CanPlayerHaveMoreEscorts(state)) {
            PlayTransitionCue(audio, state, 3);
            BoardShowOverlay(
                state,
                kMiscEscortCap,
                "You already have the maximum possible number of escorts.");
          } else {
            // The original shows NovaUi_ShowCaptureDecisionDialog (DLOG 0x3fa,
            // PICT 0x2144: escort vs. swap) when the player class has
            // capture_power >= 1 and the swap path can call
            // Outfit_SwapPlayerShipWithEscort. Both are TODO(decomp); the port
            // always takes the escort path.
            NovaLog::Todo("board: NovaUi_ShowCaptureDecisionDialog 0x00497eb0 "
                          "+ ship-swap not reconstructed; taking escort path");
            close = true;

            const ShipClass *target_class = state.scenario.Ship(
                static_cast<std::int16_t>(target.ship_class_id + 0x80));
            const float max_armor =
                target_class != nullptr
                    ? static_cast<float>(target_class->base_armor)
                    : target.armor_points;
            // TODO(decomp): the original runs the OnCapture reaction script
            // (Mission_ExecuteReactionScript of ShipClassDef.field_0x3e9)
            // before conversion.
            target.ai_behavior_code = 6;
            target.ai_target_ship_slot = 0;
            target.escort_origin_mark = 0; // field_0xbb
            target.armor_points = max_armor * kCapturedArmorFraction;
            target.faction_or_government_id = -1;
            target.mission_ship_slot = -1;
            target.primary_target_ship_slot = -1;
            target.escort_rehired_mark = 1; // field_0xb9
            target.cloak_transition_latch = 0;
            target.cloak_fade_progress = 0.0F;
            target.target_stellar_object_id = -1;
            target.ai_hostility_accumulator = 0;
            // TODO(decomp): escort_released_mark / escort_upgrade_mark /
            // jamming_score_* fields are not modelled in the port.
            target.ai_state_code = 0;
            target.ai_control_mode = 0;
            target.ai_secondary_target_slot = -1;
            NovaBoarding_ResetShipAndAttackersAfterBoarding(state, target);
            BoardShowOverlay(
                state,
                kMiscAssignedEscort,
                "You assigned this ship to your fleet of escorts.");
            state.stat_cache_valid = false;
            result.target_captured_as_escort = true;
          }
        }
      }
    }

    // Frame cap. The original blocks on NovaUi_PollTravelScriptAction for the
    // next click, so this loop runs at ~60 Hz, not a CPU-burning spin. This
    // matters for the panic self-destruct re-roll above: it is meant to be
    // re-checked once per frame (rand(100) <= panic each frame), not on the
    // very next microsecond after a loot action, which made the window close
    // "instantly" after any loot.
    SDL_Delay(16);
  }

  // Original restores the draw context and recomputes outfit-derived state on
  // close; the SDL modal has no context stack, so just mark the derived stats
  // stale for the flight loop.
  state.stat_cache_valid = false;
  return result;
}

} // namespace game
