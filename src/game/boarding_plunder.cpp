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
#include "hud_renderer.hpp"
#include "mission.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "ship_ai.hpp"
#include "ship_visual.hpp"
#include "spaceflight_view.hpp"
#include "targeting.hpp"

#include <SDL3/SDL.h>

namespace game {
namespace {

// ---- Decoded binary constants (doubles in the original's data pool) -------
constexpr double kCreditRollThreshold = 2.0;  // _DAT_00575900
constexpr double kCreditScale = 1000.0;       // _DAT_00575918
constexpr double kDudeBootyCostShare = 0.025; // _DAT_00575910
// kMissionBootyShare = 0.5 (_DAT_005758a0) applies to the mission-ship booty
// arm (PersDef +0x618), TODO(decomp) until mission-ship defs are
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
    if (!ship.is_active || ship.squad_leader_ship_slot != 0 ||
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
  } else if (target.pers_def_slot != -1) {
    // Mission-ship booty: half of the mission def's booty_base_credits.
    // TODO(decomp): the original reads PersDef +0x618
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
    if (!escort.is_active || escort.squad_leader_ship_slot != 0 ||
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

// Lazily decodes snd 150 + i into GameState.transition_sounds, mirroring
// NovaAudio_PreloadGameplayData (0x004b0740) which fills
// g_transition_sound_handle_table[i] with NovaSound_LoadDecodedById(0x96+i).
// Called before any transition-table cue plays so the first beep doesn't
// hitch.
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

namespace {

// Queues a transition-table cue on GameState.pending_ui_sounds (drained by
// the spaceflight loop). `index` is the g_transition_sound_handle_table slot:
// 2 = confirm/plunder taken, 3 = denial/error, 4 = "boarded" fanfare.
void QueueUiSound(GameState &state, std::int16_t index, std::int16_t count) {
  state.pending_ui_sounds.push_back(GameState::PendingUiSound{index, count});
}

void ShowBoardingOverlay(GameState &state, std::uint16_t str_index) {
  // str_index is the 1-based STR# 0x7d2 entry number.
  auto text = NovaHud_LoadStringEntry(0x7d2, str_index);
  if (text.has_value()) {
    // Board denials show for 0x168 frames (the doc's recorded duration).
    NovaHud_ShowOverlayMessage(
        state, std::move(*text), 0xe0, 0xe0, 0xe0, 0x168);
  }
}

// The board command's proximity gate compares per-axis position deltas with
// half the target's current sprite frame spans
// (Sprite_GetShotHalfSpan / Sprite_GetFrameVerticalHalfSpan in the original).
// Those helpers return the FULL frame span (a bounds subtraction, like the
// reticle's use in SpaceflightView; their "HalfSpan" names are misleading),
// and default to 0x20 when sprite data is missing. The port reads the same
// spans from the class's sh\x8an descriptor at the renderer's id convention
// (ship_class_id + 0x80); ships without a decodable descriptor fall back to
// the original's 32px default.
struct BoardRangeSpan {
  float full_x = 32.0F; // Sprite_GetShotHalfSpan fallback 0x20
  float full_y = 32.0F; // Sprite_GetFrameVerticalHalfSpan fallback 0x20
};

[[nodiscard]] BoardRangeSpan TargetFrameSpan(const Ship &target) {
  const auto class_id = static_cast<std::uint16_t>(target.ship_class_id + 0x80);
  if (const auto resource =
          NovaResource_Load(kShipVisualResourceType, class_id)) {
    if (const auto visual = DecodeShipVisualDescriptor(*resource)) {
      return {static_cast<float>(visual->base_x_size),
              static_cast<float>(visual->base_y_size)};
    }
  }
  return {};
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
  ship.pers_def_slot = -1;
}

// Ghidra 0x0045a3d0 Ship_HandlePlayerBoardTargetCommand.
//
// Clean-room summary (see docs/boarding_plunder_capture.md for the full map):
// Validates the player's board command against the primary target (fire
// -restricted target, same system, close range, matched heading, low relative
// velocity, target crew >= 1), then dispatches the boarding/plunder window
// (plain ships). Mission arms and the post-hit escort/fighter arms are
// TODO(decomp) below. Entry: called on the 'b' edge during flight; the modal
// renders the live game view through `view`/`hud` while it is open. Exit:
// returns with the target boarded or an STR# 0x7d2 denial overlay queued.
// Confidence: high on gates, dispatch arms partially reconstructed.
void NovaBoarding_HandleBoardTargetCommand(SdlPlatform &platform,
                                           SdlAudio &audio,
                                           GameState &state,
                                           SpaceflightView &view,
                                           HudRenderer &hud) {
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
      target.boarded_target_latch == 0 ||
      (target.mission_fleet_slot == -1 && target.post_hit_mode_hint >= 0);
  const bool fire_restricted = NovaAiShip_IsDisabled(state, target);
  const bool eligible =
      rehired_or_surrendering && fire_restricted && target.is_active &&
      player.current_system_id == target.current_system_id &&
      target.pers_def_slot != 0x3ff && !NovaAiShip_IsDestroyed(player);
  if (!eligible) {
    // Diagnosis aid: the original denies every non-disabled ship here too
    // (Ship_IsShipDisabled must be true). Log which predicate failed
    // with the armor state so in-game denials can be attributed.
    const ShipClass *diag_class = state.scenario.Ship(
        static_cast<std::int16_t>(target.ship_class_id + 0x80));
    NovaLog::Info(
        "board: target slot {} ({}) denied — rehired_mark={} "
        "fire_restricted={} active={} same_system={} mission_ship={:#x} "
        "player_destroyed={} armor={:.0f}/{} (boardable when armor < {:.2f})",
        target_slot,
        diag_class != nullptr ? diag_class->display_name : "?",
        target.boarded_target_latch,
        fire_restricted,
        target.is_active,
        player.current_system_id == target.current_system_id,
        target.pers_def_slot,
        NovaAiShip_IsDestroyed(player),
        target.armor_points,
        diag_class != nullptr ? diag_class->base_armor : 0,
        diag_class != nullptr
            ? static_cast<float>(diag_class->base_armor) *
                  ((diag_class->capability_flags & 0x10) != 0 ? 0.1F
                                                              : 1.0F / 3.0F)
            : 0.0F);
    QueueUiSound(state, 3, 1);
    ShowBoardingOverlay(state, 0x82); // "You can't board this ship."
    return;
  }

  constexpr float kBoardVelocityGate = 0.5F; // _DAT_00575598
  constexpr float kBoardRangeShare = 0.5F; // same constant, per-axis frame span
  constexpr float kBoardHeadingToleranceDeg = 30.0F;

  // ---- Relative velocity gate -------------------------------------------
  if (std::fabs(target.vel_x - player.vel_x) > kBoardVelocityGate ||
      std::fabs(target.vel_y - player.vel_y) > kBoardVelocityGate) {
    QueueUiSound(state, 3, 1);
    ShowBoardingOverlay(state, 0x84); // "You're moving too fast to board..."
    return;
  }

  // ---- Range gate (per-axis, half of the target's full sprite frame) -----
  const BoardRangeSpan span = TargetFrameSpan(target);
  if (std::fabs(target.pos_x - player.pos_x) > span.full_x * kBoardRangeShare ||
      std::fabs(target.pos_y - player.pos_y) > span.full_y * kBoardRangeShare) {
    QueueUiSound(state, 3, 1);
    ShowBoardingOverlay(state, 0x83); // "You're not close enough to board..."
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
  // Mission arms (Ship_HandlePlayerBoardTargetCommand 0x0045a3d0): the
  // pickup_mode-2 cargo pickup and the spawn_behavior 2/5 + flags 0x0001
  // single-ship rescue arm write goal_counter_b, set the target's boarded
  // latch and clear other ships' targeting before falling through to the
  // capture flow. Inactive-mission ships board like plain ships. The
  // mission-ship interaction window branch (pers-linked targets,
  // 0x00442510) is not reachable in the port yet (TODO(decomp)).
  if (target.mission_fleet_slot != -1) {
    const auto mission_idx =
        static_cast<std::size_t>(target.mission_fleet_slot);
    if (state.active_mission_runtime_flags[mission_idx].is_active) {
      ActiveMission &mission = state.active_missions[mission_idx];
      const std::uint16_t mission_flags = mission.flags_primary;
      if (mission.pickup_mode == 2) {
        // Board-for-cargo: the interaction resource gate; on denial the
        // mission's own STR# 0x7d2 0x165/0x166 dialog shows (logged TODO in
        // Mission_TryConsumeMissionInteractionResources) and the command
        // silently returns, exactly like the original's bVar5 path.
        if (!Mission_TryConsumeMissionInteractionResources(
                state, mission.cargo_qty_tons)) {
          return;
        }
        mission.goal_counter_b =
            static_cast<std::int16_t>(mission.goal_counter_b + 1);
        mission.carrying_resources = true;
        // Cargo pickup overlay: STR# 0x7d2 0x6a + " " + optional 0x6b + the
        // cargo item name (CREC table) + " " + 0x6c. The name table is not
        // loaded in the port, so the message shows without the item name.
        // TODO(decomp): CREC cargo-name table (DAT_0069d2cc) + 0x6b/0x6c
        // composition.
        if (auto text = NovaHud_LoadStringEntry(0x7d2, 0x6a)) {
          NovaHud_ShowOverlayMessage(state,
                                     *text,
                                     /*duration_frames=*/std::uint64_t{0xfa});
        }
        QueueUiSound(state, 4, 8);
      } else if ((mission.spawn_behavior == 2 || mission.spawn_behavior == 5) &&
                 (mission_flags & 0x0001U) != 0U &&
                 mission.target_ship_count == 1) {
        // Rescue/board-captain arm. flags 0x0008 swaps the generic STR# 0x7d2
        // 0x7e "boarding" message for a class-name message whose tail strings
        // (DAT_0072d5cc / DAT_00599acc) are not reconstructed. TODO(decomp).
        ShowBoardingOverlay(state, 0x7e);
        QueueUiSound(state, 4, 8);
        mission.goal_counter_b =
            static_cast<std::int16_t>(mission.goal_counter_b + 1);
        target.ai_maneuver_timer_ms = 100.0F;
        target.boarded_target_latch = 1;
        NovaTargeting_ClearDestroyedShipReferences(state,
                                                   target.ship_instance_id);
      }
      // Any other active-mission ship boards like a plain ship (cVar12 = 1).
    }
  }
  // Plain ships: Bible "Ships with 0 crew can't be boarded". Mission ships
  // skip this gate (the original's crew check lives only in the
  // mission_fleet_slot == -1 branch).
  if (target.mission_fleet_slot == -1 && target_class->crew < 1) {
    NovaLog::Info("board: target slot {} ({}) denied — crew 0",
                  target_slot,
                  target_class->display_name);
    QueueUiSound(state, 3, 1);
    ShowBoardingOverlay(state, 0x82); // "You can't board this ship."
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
  // depend on ShipClass_HasPlayerBayCapacityFor (0x004694a0, the fighter-bay
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
      NovaBoarding_RunWindow(platform, audio, state, view, hud);
  (void)result;

  // After the interaction: latch + clear every ship targeting the boarded
  // hull (Ship_ClearOtherShipsTargetingShip 0x00415dc0; the port's
  // destroyed-reference helper performs the same clearing).
  target.boarded_target_latch = 1;
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

// DLOG 0x3fa (257x114, DITL 0x3fa) — the capture-decision dialog shown after a
// successful capture roll. Items: [0] upper button (55,51)-(201,77) = action 1
// "Use As My Ship" (STR# 0x96 0x2e); [1] lower button (55,83)-(201,109) =
// action 2 "Use As Escort" (STR# 0x96 0x2d); [2] 238x40 text panel (9,6)
// drawing STR# 0x7d2 0x76. Backdrop PICT 0x2144. Both choices play the
// transition-table [1] cue. Decoded from the shipped resources (DLOG bounds
// t40 l40 b154 r297) and NovaUi_RedrawTravelBinaryChoiceButtons's label-index
// table {0x2e, 0x2d} into the STR# 0x96 pstring table (0-based pool entries;
// the STR# entry numbers below are those + 1).
constexpr float kCaptureWindowX = (640.0F - 257.0F) / 2.0F; // 191.5
constexpr float kCaptureWindowY = (480.0F - 114.0F) / 2.0F; // 183.0
constexpr float kCaptureWindowW = 257.0F;
constexpr float kCaptureWindowH = 114.0F;
constexpr std::uint16_t kCaptureBackdropPict = 0x2144;
constexpr std::uint16_t kCaptureTextStr = 0x76;   // STR# 0x7d2 1-based entry
constexpr std::uint16_t kCaptureBtnMyShip = 0x2f; // STR# 0x96 1-based entry
constexpr std::uint16_t kCaptureBtnEscort = 0x2e; // STR# 0x96 1-based entry

// STR# 0x96 button-label pool: Abort/Cargo/Credits/Ammo/Energy/Capture Ship.
// 1-based entry numbers, as passed to Resource_LoadStringEntry.
constexpr std::uint16_t kButtonLabelStr = 0x96;
constexpr std::uint16_t kBtnAbort = 0x23;
constexpr std::uint16_t kBtnCargo = 0x28;
constexpr std::uint16_t kBtnCredits = 0x29;
constexpr std::uint16_t kBtnAmmo = 0x2a;
constexpr std::uint16_t kBtnEnergy = 0x2b;
constexpr std::uint16_t kBtnCaptureShip = 0x2c;

// Action codes returned for each option button (NovaUi_PollTravelScriptAction).
constexpr unsigned kActionAbort = 1;
constexpr unsigned kActionCargo = 2;
constexpr unsigned kActionCredits = 3;
constexpr unsigned kActionAmmo = 4;
constexpr unsigned kActionEnergy = 6;
constexpr unsigned kActionCapture = 7;

constexpr std::string_view ActionName(unsigned code) {
  switch (code) {
  case kActionAbort:
    return "Abort";
  case kActionCargo:
    return "Cargo";
  case kActionCredits:
    return "Credits";
  case kActionAmmo:
    return "Ammo";
  case kActionEnergy:
    return "Energy";
  case kActionCapture:
    return "Capture";
  default:
    return "?";
  }
}

// STR# 0x7d2 "misc strings" used by the window (1-based entry numbers, as the
// original passes them to Resource_LoadStringEntry/Resource_DrawStringEntry;
// verified against the shipped pool and docs/reference/boarding.jpg).
constexpr std::uint16_t kMiscStr = 0x7d2;
constexpr std::uint16_t kMiscTonWord = 0x01;          // "ton"
constexpr std::uint16_t kMiscTonsWord = 0x02;         // "tons"
constexpr std::uint16_t kMiscTitle = 0x6d;            // "Select what to
                                                      // plunder from this
                                                      // ship:"
constexpr std::uint16_t kMiscCargoLabel = 0x6e;       // "Cargo:"
constexpr std::uint16_t kMiscAmmoLabel = 0x6f;        // "Ammo:"
constexpr std::uint16_t kMiscCaptureOddsLabel = 0x70; // "Capture Odds:"
constexpr std::uint16_t kMiscCreditsLabel = 0x21;     // "credits"
constexpr std::uint16_t kMiscEnergyLabel = 0x07;      // "Energy:"
constexpr std::uint16_t kMiscStoleAll = 0x74;         // "You stole all the"
constexpr std::uint16_t kMiscSalvaged = 0x73;         // "You salvaged"
constexpr std::uint16_t kMiscFromThisShip = 0x6c;     // "from this ship."
constexpr std::uint16_t kMiscOfWord = 0x187;          // "of"
constexpr std::uint16_t kMiscNoOffer = 0x14f;         // "None"
constexpr std::uint16_t kMiscCargoFull = 0x72;        // "You couldn't store any
                                                      // of the cargo..."
constexpr std::uint16_t kMiscAmmoFull = 0x75;         // "...any of the ammo..."
constexpr std::uint16_t kMiscSelfDestruct = 0x71;     // "Oops! You tripped..."
constexpr std::uint16_t kMiscCaptureFailed = 0x7d;  // "Your attempt to capture
                                                    // this ship was
                                                    // unsuccessful."
constexpr std::uint16_t kMiscEscortCap = 0x7c;      // "You already have the
                                                    // maximum possible
                                                    // number of escorts."
constexpr std::uint16_t kMiscAssignedEscort = 0x7b; // "You assigned this ship
                                                    // to your fleet of
                                                    // escorts."
// Fuel/energy-transfer overlays (STR# 0x7d2 entries 4..6 = pool 0x03..0x05,
// DAT_0072d6cc/d7cc/d8cc per the original loader).
constexpr std::uint16_t kMiscFuelNowFull = 0x04;  // "You filled your reactors
                                                  // and batteries..."
constexpr std::uint16_t kMiscFuelStole = 0x05;    // "You transferred all of
                                                  // this ship's energy..."
constexpr std::uint16_t kMiscFuelTankFull = 0x06; // "You couldn't store any of
                                                  // the energy..."

// Cargo commodity names: the original loader fills DAT_0069d2cc[cargo_type]
// via Resource_LoadStringEntry(0xfa1, cargo_type + 1) (FUN_004c7040); the
// boarding roll only ever uses types 0..5.
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
          kCargoNameStr, static_cast<std::uint16_t>(cargo_type + 1))) {
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
                     SpaceflightView &view,
                     HudRenderer &hud,
                     SDL_Texture *backdrop,
                     int hovered) {
  SDL_Renderer *renderer = platform.renderer();
  // Render the live game view normally beneath the window (the flight sim is
  // paused, so this redraws the same world each frame): the original draws
  // its DLOG over the unmodified gameplay surface, and the HUD's overlay
  // message rect (loot / "Oops!" text) stays visible below the window.
  view.DrawGameFrame(platform, state, hud);
  platform.SetCenteredPlayfield();

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
// device (the modal owns no device). Mirrors NovaAudio_QueueCenteredSound
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
  // Window loot/self-destruct overlays show for 0xf0 frames (the window loop
  // decompile's second argument).
  NovaHud_ShowOverlayMessage(
      state,
      LoadBoardMiscString(str_index, std::move(fallback)),
      0xe0,
      0xe0,
      0xe0,
      0xf0);
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

// Word-wraps `text` into lines that fit `max_width` logical pixels at the
// given font. The original's DrawPascalStringInFilledRect word-wraps inside
// the item rect; the port's NovaText_Draw is single-line.
std::vector<std::string> WordWrapText(NovaFontCache &font_cache,
                                      std::string_view text,
                                      float max_width) {
  std::vector<std::string> lines;
  std::string line;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t space = text.find(' ', start);
    const std::string_view word =
        text.substr(start,
                    space == std::string_view::npos ? std::string_view::npos
                                                    : space - start);
    if (!line.empty()) {
      line += ' ';
    }
    line += word;
    if (font_cache.TextWidth(
            NovaFontFamily::kGeneva, 11.0F, kNovaFontStyleRegular, line) >
        max_width) {
      // Overfull: put the word on its own (or the next) line.
      if (const std::size_t cut = line.rfind(' '); cut != std::string::npos) {
        lines.push_back(line.substr(0, cut));
        line = std::string(word);
      } else {
        lines.push_back(line);
        line.clear();
      }
    }
    if (space == std::string_view::npos) {
      break;
    }
    start = space + 1;
  }
  if (!line.empty()) {
    lines.push_back(line);
  }
  return lines;
}

// Ghidra 0x00497eb0 NovaUi_ShowCaptureDecisionDialog (clean-room). Runs the
// capture-decision modal (DLOG 0x3fa, PICT 0x2144) after a successful capture
// roll and returns true for "Use As My Ship" (the swap arm), false for "Use
// As Escort". The boarding window stays visible beneath, as in the original's
// composited modal stack. Loop shape mirrors NovaUi_RunBoardingPlunderWindow:
// input flush on open, ~60 Hz redraw, click answer (the DITL defines no
// cancel item, so Esc/Enter are inert here).
[[nodiscard]] bool
RunCaptureDecisionDialog(SdlPlatform &platform,
                         SdlAudio &audio,
                         GameState &state,
                         SpaceflightView &view,
                         HudRenderer &hud,
                         NovaFontCache &font_cache,
                         const ServicesButtonArt &art,
                         const std::array<BoardButton, 6> &board_buttons,
                         const BoardingPlunderOptions &board_options,
                         SDL_Texture *board_backdrop) {
  struct CaptureButton {
    SDL_FRect rect;
    unsigned action_code;
    std::string label;
  };

  const auto abs = [](float x, float y, float w, float h) {
    return SDL_FRect{kCaptureWindowX + x, kCaptureWindowY + y, w, h};
  };
  const std::array<CaptureButton, 2> buttons{
      CaptureButton{abs(55.0F, 51.0F, 146.0F, 26.0F),
                    1,
                    LoadBoardButtonLabel(kCaptureBtnMyShip)},
      CaptureButton{abs(55.0F, 83.0F, 146.0F, 26.0F),
                    2,
                    LoadBoardButtonLabel(kCaptureBtnEscort)},
  };
  const SDL_FRect text_panel = abs(9.0F, 6.0F, 238.0F, 40.0F);

  auto backdrop = LoadBoardPictTexture(platform, kCaptureBackdropPict);
  if (!backdrop) {
    NovaLog::Todo("board: capture-dialog backdrop PICT 0x2144 unavailable; "
                  "drawing a bordered placeholder");
  }
  const std::string panel_text =
      LoadBoardMiscString(kCaptureTextStr,
                          "Do you want to use this ship as an escort, or "
                          "would you rather trade places with its captain "
                          "and use it as your own ship?");

  // NovaInputQueue_FlushAllCommands: discard the click that resolved the
  // capture action so it can't dispatch a phantom choice on the first frame.
  while (platform.PollTextEvent().has_value()) {
  }

  bool take_ship = false;
  bool close = false;
  while (!platform.quit_requested() && !close) {
    int hovered = -1;
    const SDL_FPoint mouse = platform.mouse_position();
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (SDL_PointInRectFloat(&mouse, &buttons[i].rect)) {
        hovered = static_cast<int>(i);
        break;
      }
    }

    SDL_Renderer *renderer = platform.renderer();
    // Live game view + the boarding window beneath (the original composites
    // this dialog over the still-open boarding window).
    view.DrawGameFrame(platform, state, hud);
    platform.SetCenteredPlayfield();
    DrawBoardWindow(platform,
                    font_cache,
                    art,
                    board_buttons,
                    board_options,
                    state,
                    view,
                    hud,
                    board_backdrop,
                    -1);

    const SDL_FRect window{
        kCaptureWindowX, kCaptureWindowY, kCaptureWindowW, kCaptureWindowH};
    if (backdrop != nullptr) {
      SDL_RenderTexture(renderer, backdrop->get(), nullptr, &window);
    } else {
      SDL_SetRenderDrawColor(renderer, 16, 40, 72, SDL_ALPHA_OPAQUE);
      SDL_RenderFillRect(renderer, &window);
      SDL_SetRenderDrawColor(renderer, 80, 140, 190, SDL_ALPHA_OPAQUE);
      SDL_RenderRect(renderer, &window);
    }

    // Text panel (DITL item 2): the offer question, word-wrapped like the
    // original's filled-rect pstring draw.
    SDL_SetRenderDrawColor(renderer,
                           kBoardPanelBg.r,
                           kBoardPanelBg.g,
                           kBoardPanelBg.b,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &text_panel);
    const std::vector<std::string> lines =
        WordWrapText(font_cache, panel_text, text_panel.w - 8.0F);
    float y = text_panel.y + 7.0F;
    for (const std::string &line : lines) {
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    11.0F,
                    kNovaFontStyleRegular,
                    kBoardValue,
                    text_panel.x + 4.0F,
                    y,
                    line);
      y += 13.0F;
    }

    // Binary-choice buttons (NovaUi_RedrawTravelBinaryChoiceButtons): the
    // shared three-state art, hover highlights, no press latching.
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      art.Draw(platform,
               buttons[i].rect,
               hovered == static_cast<int>(i) ? ButtonState::kHover
                                              : ButtonState::kNormal);
      NovaText_DrawCentered(platform,
                            font_cache,
                            kThreeStateButtonFontFamily,
                            kThreeStateButtonFontSize,
                            kNovaFontStyleRegular,
                            SDL_Color{255, 255, 255, SDL_ALPHA_OPAQUE},
                            buttons[i].rect.x,
                            buttons[i].rect.x + buttons[i].rect.w,
                            ThreeStateButtonLabelBaseline(buttons[i].rect),
                            buttons[i].label);
    }
    platform.Present();

    // Poll: a click on a button resolves the choice. The DITL defines no
    // cancel item, so the original's loop (and this one) only exits through
    // the two buttons (or a platform quit).
    unsigned action = 0;
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      if (in->key != TextKey::primary) {
        continue;
      }
      const SDL_FPoint click = platform.mouse_position();
      for (std::size_t i = 0; i < buttons.size(); ++i) {
        if (SDL_PointInRectFloat(&click, &buttons[i].rect)) {
          action = buttons[i].action_code;
          break;
        }
      }
      if (action == 0) {
        NovaLog::Info("board: capture-dialog click at ({}, {}) missed both "
                      "buttons",
                      click.x,
                      click.y);
      }
      break;
    }
    if (action == 1) {
      take_ship = true;
      close = true;
    } else if (action == 2) {
      take_ship = false;
      close = true;
    }
    SDL_Delay(16);
  }

  // Both choices play g_transition_sound_handle_table[1].
  PlayTransitionCue(audio, state, 1);
  NovaLog::Info("board: capture decision — {}",
                take_ship ? "use as my ship" : "use as escort");
  return take_ship;
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
                                                          GameState &state,
                                                          SpaceflightView &view,
                                                          HudRenderer &hud) {
  BoardingWindowResult result;

  const std::int16_t target_slot = state.player.primary_target_ship_slot;
  if (target_slot < 1 ||
      !state.SlotInRange(static_cast<std::size_t>(target_slot))) {
    return result; // target lost while the command ran
  }

  const Ship &board_target =
      state.ShipAt(static_cast<std::size_t>(target_slot));
  const ShipClass *target_class = state.scenario.Ship(
      static_cast<std::int16_t>(board_target.ship_class_id + 0x80));

  // The window's panic value: rand(0x1a) + 0xf (15..40). Each loot action
  // multiplies it; the self-destruct re-roll (armed by a loot action) fires
  // when rand(100) <= panic, exactly once on the loop iteration after the
  // action (the original clears its latch each iteration).
  std::int32_t panic = NovaRandomRange(state.rng, 0x1a) + 0xf;

  BoardingPlunderOptions options = NovaBoarding_BuildOptions(state);
  NovaLog::Info(
      "board: opening plunder window for slot {} ({}) — cargo({})={}x "
      "credits={} ammo({})={}x fuel={} capture_odds={}% panic={}",
      target_slot,
      target_class != nullptr ? target_class->display_name : "?",
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
  if (board_target.mission_fleet_slot != -1) {
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
  const char *close_reason = "target lost";

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
                    view,
                    hud,
                    backdrop ? backdrop->get() : nullptr,
                    hovered);
    platform.Present();

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
        if (action == 0) {
          NovaLog::Info(
              "board: click at ({}, {}) missed every button", click.x, click.y);
        }
        break;
      }
    }
    if (action != 0) {
      NovaLog::Info("board: action {} ({}) armed={}",
                    action,
                    ActionName(static_cast<unsigned>(action)),
                    panic_armed);
    }

    // Abort (action 1): panic = -1, disarm, close with the confirm cue.
    if (action == static_cast<int>(kActionAbort)) {
      panic = -1;
      panic_armed = false;
      PlayTransitionCue(audio, state, 2);
      close = true;
      close_reason = "abort";
    }

    // Self-destruct re-roll: armed by the previous iteration's loot action;
    // the original rolls exactly once per loot action (its latch clears each
    // loop iteration), so one rand(100) <= panic check here.
    if (panic_armed) {
      const std::int16_t trip_roll = NovaRandomRange(state.rng, 100);
      if (trip_roll <= panic) {
        NovaLog::Info("board: self-destruct tripped — roll {} <= panic {}",
                      trip_roll,
                      panic);
        SelfDestructTarget(state);
        PlayTransitionCue(audio, state, 2);
        result.target_self_destructed = true;
        close = true;
        close_reason = "self-destruct (panic re-roll)";
        break;
      }
      NovaLog::Info("board: panic re-roll survived — roll {} > panic {}",
                    trip_roll,
                    panic);
    }
    panic_armed = false;

    // ---- Cargo (action 2) -----
    if (action == static_cast<int>(kActionCargo)) {
      if (options.cargo_type == -1) {
        NovaLog::Info("board: cargo clicked with no offer — denial beep");
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
          NovaLog::Info(
              "board: cargo {}t did not fit (free {}/{}); left behind",
              options.cargo_type,
              capacity - total,
              capacity);
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
          NovaLog::Info("board: cargo transferred; panic now {}", panic);
        }
        panic = RoundHalfUp(static_cast<double>(panic) * kPanicCargo);
        panic_armed = true;
      }
    }

    // ---- Credits (action 3) -----
    if (action == static_cast<int>(kActionCredits)) {
      if (options.credits < 1) {
        NovaLog::Info("board: credits clicked with no offer — denial beep");
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
        NovaLog::Info("board: stole {} credits; panic now {}",
                      options.credits,
                      RoundHalfUp(static_cast<double>(panic) * kPanicCredits));
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
        NovaLog::Info("board: ammo clicked with no offer — denial beep");
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
        const int offered = options.ammo_quantity;
        options.ammo_quantity = 0;
        options.ammo_bank = -1;
        NovaLog::Info("board: ammo transferred {} of {}; panic now {}",
                      transferred,
                      offered,
                      RoundHalfUp(static_cast<double>(panic) * kPanicAmmo));
        panic = RoundHalfUp(static_cast<double>(panic) * kPanicAmmo);
        panic_armed = true;
      }
    }

    // ---- Energy (action 6) -----
    if (action == static_cast<int>(kActionEnergy)) {
      if (options.fuel_quantity < 1) {
        NovaLog::Info("board: energy clicked with no offer — denial beep");
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
        NovaLog::Info("board: energy transferred {}; panic now {}",
                      fill,
                      RoundHalfUp(static_cast<double>(panic) * kPanicEnergy));
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
      NovaLog::Info("board: capture roll {} vs odds {}{}",
                    roll,
                    options.capture_odds_percent,
                    auto_fail ? " (auto-fail)" : "");
      if (roll > options.capture_odds_percent || auto_fail) {
        close = true;
        close_reason = "capture roll failed";
        PlayTransitionCue(audio, state, 3);
        BoardShowOverlay(state,
                         kMiscCaptureFailed,
                         "Your attempt to capture this ship was unsuccessful.");
        result.capture_attempt_failed = true;
      } else {
        // 1-in-10 "Oops" self-destruct roll.
        if (NovaRandomRange(state.rng, 10) == 0) {
          NovaLog::Info("board: capture Oops roll hit — self-destruct");
          close = true;
          close_reason = "capture Oops self-destruct";
          SelfDestructTarget(state);
          PlayTransitionCue(audio, state, 2);
          result.target_self_destructed = true;
        } else {
          PlayTransitionCue(audio, state, 2);
          if (!NovaShip_CanPlayerHaveMoreEscorts(state)) {
            NovaLog::Info("board: capture blocked by the escort cap");
            PlayTransitionCue(audio, state, 3);
            BoardShowOverlay(
                state,
                kMiscEscortCap,
                "You already have the maximum possible number of escorts.");
          } else {
            // The original shows NovaUi_ShowCaptureDecisionDialog (DLOG 0x3fa,
            // PICT 0x2144: "Use As My Ship" / "Use As Escort") when the
            // player's class has capture_power (crew) >= 1; with capture_power
            // 0 it skips straight to the escort conversion.
            bool take_ship = false;
            if (const ShipClass *player_class =
                    state.scenario.Ship(static_cast<std::int16_t>(
                        state.player.ship_class_id + 0x80));
                player_class != nullptr && player_class->crew >= 1) {
              take_ship = RunCaptureDecisionDialog(platform,
                                                   audio,
                                                   state,
                                                   view,
                                                   hud,
                                                   font_cache,
                                                   art,
                                                   buttons,
                                                   options,
                                                   backdrop ? backdrop->get()
                                                            : nullptr);
            }
            if (take_ship) {
              // TODO(decomp(0x00497eb0)) skipped: the swap arm (rename-confirm
              // dialog with class name + 3 random digits, then
              // Outfit_SwapPlayerShipWithEscort + gameplay layout reinstall)
              // is not reconstructed; the escort conversion below is the
              // port's fallback for both choices.
              NovaLog::Todo("board: 'Use As My Ship' chosen, but "
                            "Outfit_SwapPlayerShipWithEscort is not "
                            "reconstructed; converting to escort instead");
            }
            close = true;
            close_reason = "target captured as escort";

            const float max_armor =
                target_class != nullptr
                    ? static_cast<float>(target_class->base_armor)
                    : target.armor_points;
            // TODO(decomp): the original runs the OnCapture reaction script
            // (Mission_ExecuteReactionScript of ShipClassDef.field_0x3e9)
            // before conversion.
            target.ai_behavior_code = 6;
            target.squad_leader_ship_slot = 0;
            target.escort_origin_mark = 0; // field_0xbb
            target.armor_points = max_armor * kCapturedArmorFraction;
            target.faction_or_government_id = -1;
            target.pers_def_slot = -1;
            target.primary_target_ship_slot = -1;
            target.boarded_target_latch = 1; // field_0xb9
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

    // Frame cap. The original's NovaUi_PollTravelScriptAction pumps one event
    // batch per call (15 ms throttle inside UiWindow_RunInteractionLoop), so
    // this loop runs at ~60 Hz, not a CPU-burning spin.
    SDL_Delay(16);
  }

  NovaLog::Info("board: window closed ({})", close_reason);
  // Original restores the draw context and recomputes outfit-derived state on
  // close; the SDL modal has no context stack, so just mark the derived stats
  // stale for the flight loop.
  state.stat_cache_valid = false;
  return result;
}

// Ghidra 0x00412550 Outfit_BoardShipAndTransferCargo. AI boarding
// resolution (the capture-variant supervisor calls it when its board approach
// completes; it is also reachable against the player). Stages:
//   1. cargo plunder: random-bin transfer up to the boarder's free hold space,
//   2. credits share when the victim is the player,
//   3. loot HUD overlay + transition-table voice cue,
//   4. capture-odds conversion for non-player, non-mission victims: the
//      victim becomes a behavior-6 follower of the boarder (faction
//      converted, shields zeroed, armor restored to 66%),
//   5. mission failure for missions armed with flags_primary 0x8000 when the
//      player is boarded.
// Capture-odds model (ship-class Strength + marine outfits, Bible ModType
// 25): positive marine ModVals add crew; negative ModVals on the BOARDER's
// loadout raise the odds ("-1..-100 increase capture odds by this amount");
// negative ModVals on the victim lower them. The original's 16-bit unsigned
// wrap arithmetic is congruent (mod 2^16, and every later use sign-extends
// the low half) to the plain signed adds used here.
void NovaBoarding_BoardShipAndTransferCargo(GameState &state,
                                            Ship &boarder,
                                            Ship &boarded,
                                            std::uint32_t now_ms) {
  if (NovaAiShip_IsDestroyed(boarded) || !boarded.is_active) {
    return;
  }

  const ShipClass *boarder_class = state.scenario.Ship(
      static_cast<std::int16_t>(boarder.ship_class_id + 0x80));
  const ShipClass *boarded_class = state.scenario.Ship(
      static_cast<std::int16_t>(boarded.ship_class_id + 0x80));

  // ---- Capture odds -------------------------------------------------------
  std::int32_t boarder_crew =
      boarder_class != nullptr ? boarder_class->strength : 0;
  std::int32_t boarded_crew =
      boarded_class != nullptr ? boarded_class->strength : 0;

  // Positive marines on the boarder's class default loadout add crew.
  for (std::size_t slot = 0; boarder_class != nullptr &&
                             slot < boarder_class->default_outfit_ids.size();
       ++slot) {
    if (boarder_class->default_outfit_counts[slot] <= 0) {
      continue;
    }
    const Outfit *outfit =
        state.scenario.Outfit(boarder_class->default_outfit_ids[slot]);
    if (outfit == nullptr) {
      continue;
    }
    for (int pair = 0; pair < kOutfitModPairCount; ++pair) {
      const OutfitModPair mod = OutfitModPairAt(*outfit, pair);
      if (mod.type == kMarinesModType && mod.val > 0) {
        boarder_crew += mod.val * boarder_class->default_outfit_counts[slot];
      }
    }
  }

  // Victim crew: the player contributes its whole owned-outfit marine pool,
  // NPCs their class default loadout.
  if (boarded.ship_instance_id == 0) {
    for (std::size_t index = 0; index < state.scenario.outfits.size();
         ++index) {
      const std::int16_t owned = state.inventory.outfit_owned_count[index];
      if (owned <= 0) {
        continue;
      }
      const Outfit &outfit = state.scenario.outfits[index];
      for (int pair = 0; pair < kOutfitModPairCount; ++pair) {
        const OutfitModPair mod = OutfitModPairAt(outfit, pair);
        if (mod.type == kMarinesModType && mod.val > 0) {
          boarded_crew += mod.val * owned;
        }
      }
    }
  } else {
    for (std::size_t slot = 0; boarded_class != nullptr &&
                               slot < boarded_class->default_outfit_ids.size();
         ++slot) {
      if (boarded_class->default_outfit_counts[slot] <= 0) {
        continue;
      }
      const Outfit *outfit =
          state.scenario.Outfit(boarded_class->default_outfit_ids[slot]);
      if (outfit == nullptr) {
        continue;
      }
      for (int pair = 0; pair < kOutfitModPairCount; ++pair) {
        const OutfitModPair mod = OutfitModPairAt(*outfit, pair);
        if (mod.type == kMarinesModType && mod.val > 0) {
          boarded_crew += mod.val * boarded_class->default_outfit_counts[slot];
        }
      }
    }
  }
  if (static_cast<std::int16_t>(boarded_crew) < 1) {
    boarded_crew = 1;
  }

  // odds = boarder_crew * 100 / (boarded_crew * 2), rounded half away from
  // zero (the doubles: 00575010 = 100, 005751a0 = 2).
  std::int32_t odds = RoundHalfUp(
      static_cast<float>(static_cast<double>(boarder_crew) * 100.0 /
                         (static_cast<double>(boarded_crew) * 2.0)));

  // Negative marines: the boarder's raise the odds, the victim's lower them.
  for (std::size_t slot = 0; boarder_class != nullptr &&
                             slot < boarder_class->default_outfit_ids.size();
       ++slot) {
    if (boarder_class->default_outfit_counts[slot] <= 0) {
      continue;
    }
    const Outfit *outfit =
        state.scenario.Outfit(boarder_class->default_outfit_ids[slot]);
    if (outfit == nullptr) {
      continue;
    }
    for (int pair = 0; pair < kOutfitModPairCount; ++pair) {
      const OutfitModPair mod = OutfitModPairAt(*outfit, pair);
      if (mod.type == kMarinesModType && mod.val < 0) {
        odds -= mod.val * boarder_class->default_outfit_counts[slot];
      }
    }
  }
  if (boarded.ship_instance_id == 0) {
    for (std::size_t index = 0; index < state.scenario.outfits.size();
         ++index) {
      const std::int16_t owned = state.inventory.outfit_owned_count[index];
      if (owned <= 0) {
        continue;
      }
      const Outfit &outfit = state.scenario.outfits[index];
      for (int pair = 0; pair < kOutfitModPairCount; ++pair) {
        const OutfitModPair mod = OutfitModPairAt(outfit, pair);
        if (mod.type == kMarinesModType && mod.val < 0) {
          odds += mod.val * owned;
        }
      }
    }
  } else {
    for (std::size_t slot = 0; boarded_class != nullptr &&
                               slot < boarded_class->default_outfit_ids.size();
         ++slot) {
      if (boarded_class->default_outfit_counts[slot] <= 0) {
        continue;
      }
      const Outfit *outfit =
          state.scenario.Outfit(boarded_class->default_outfit_ids[slot]);
      if (outfit == nullptr) {
        continue;
      }
      for (int pair = 0; pair < kOutfitModPairCount; ++pair) {
        const OutfitModPair mod = OutfitModPairAt(*outfit, pair);
        if (mod.type == kMarinesModType && mod.val < 0) {
          odds += mod.val * boarded_class->default_outfit_counts[slot];
        }
      }
    }
  }

  // +-10 noise (10 - rand(0x15)) and clamp to [10, 100]; every consumer
  // sign-extends the low 16 bits, as the original's (short) casts do.
  odds += 10 - NovaRandomRange(state.rng, 0x15);
  if (static_cast<std::int16_t>(odds) < 10) {
    odds = 10;
  }
  if (100 < static_cast<std::int16_t>(odds)) {
    odds = 100;
  }

  // ---- Random-bin cargo transfer ------------------------------------------
  // TODO(decomp) skipped: the original moves cargo bin-by-bin from the
  // victim's ShipState.field_0x7a..0x84 bins into the boarder's, capped by
  // the boarder's free Holds and the victim's capacity (Ship_ComputeShip-
  // TotalMass for a player victim). The port models cargo bins only on the
  // player's PlayerInventory, not per Ship, and an AI boarder has nowhere to
  // carry plundered bins, so no transfer can be represented. The loot
  // overlay therefore only ever reports the credits share below.
  const std::int32_t transferred = 0;

  // ---- Credits share (only when the victim is the player) -----------------
  // transfer = credits * odds * 29 * 0.0001 (disasm 0x00412d6a: odds*29 then
  // two 0.01 multiplies; the decompiler's 0x1e is a misread).
  std::int32_t credits_taken = 0;
  if (boarded.ship_instance_id == 0) {
    const float raw =
        static_cast<float>(static_cast<double>(29 * static_cast<int>(odds)) *
                           0.01 * static_cast<double>(boarded.credits) * 0.01);
    credits_taken = RoundHalfUp(raw);
    boarder.credits += credits_taken;
    boarded.credits -= credits_taken;
  } else {
    // NPC victims simply lose their (unused) credits.
    boarded.credits = 0;
  }

  // ---- Loot HUD overlay ---------------------------------------------------
  const bool show_loot_message =
      boarded.ship_instance_id == 0 ||
      (boarded.squad_leader_ship_slot == -1 &&
       boarded.mission_fleet_slot == -1 && boarded.post_hit_mode_hint > 0);
  if (show_loot_message) {
    // g_playerInventoryAndLoadoutDirty.
    state.stat_cache_valid = false;
    if (transferred > 0 || credits_taken > 0) {
      std::string text;
      if (transferred > 0) {
        text += GroupedUInt(transferred);
        text += ' ';
        text +=
            LoadBoardMiscString(transferred == 1 ? kMiscTonWord : kMiscTonsWord,
                                transferred == 1 ? "ton" : "tons");
        text += ' ';
        text += LoadBoardMiscString(kMiscOfWord, "of");
        text += ' ';
        // STR# 0x7d2 0x175 (pool "cargo").
        text += LoadBoardMiscString(0x175, "cargo");
        text += ' ';
        if (credits_taken > 0) {
          // STR# 0x7d2 0x188 ("and").
          text += LoadBoardMiscString(0x188, "and");
          text += ' ';
        }
      }
      if (credits_taken > 0) {
        if (transferred < 1) {
          text.clear();
        }
        text += GroupedUInt(credits_taken);
        text += ' ';
        text += credits_taken < 2 ? "credit" : "credits";
        text += ' ';
      }
      // STR# 0x7d2 0x176 ("stolen!").
      text += LoadBoardMiscString(0x176, "stolen!");
      // 400-frame overlay (decompile argument) with the shared HUD tint.
      NovaHud_ShowOverlayMessage(
          state, std::move(text), static_cast<std::uint64_t>(400U));
      // Voice: transition-table slot 1 via NovaAudio_FillVoiceSlotDescriptor.
      QueueUiSound(state, 1, 1);
    }
  }

  // The player and mission-fleet ships are never converted.
  if (boarded.ship_instance_id == 0 || boarded.mission_fleet_slot != -1) {
    if (boarded.ship_instance_id == 0) {
      // Missions armed with flags_primary 0x8000 fail when their captain is
      // boarded/captured.
      for (std::size_t slot = 0; slot < GameState::kMaxActiveMissions; ++slot) {
        const MissionRuntimeFlags &runtime =
            state.active_mission_runtime_flags[slot];
        if (runtime.is_active && !runtime.is_failed &&
            (state.active_missions[slot].flags_primary & 0x8000U) != 0U) {
          Mission_ResolveMissionFailure(
              state, static_cast<std::int16_t>(slot), now_ms);
        }
      }
    }
    return;
  }

  // ---- Capture-odds conversion --------------------------------------------
  // odds < 41 only converts under the cheat flag; otherwise the roll must
  // not exceed odds/2 (double 00575038 = 0.5).
  bool convert = false;
  if (static_cast<std::int16_t>(odds) < 0x29) {
    convert = state.cheat_mode_active;
  } else {
    const int roll = NovaRandomRange(state.rng, 0x65);
    convert = !(static_cast<double>(static_cast<std::int16_t>(odds)) * 0.5 <
                static_cast<double>(roll));
    if (!convert) {
      convert = state.cheat_mode_active;
    }
  }
  if (!convert) {
    return;
  }

  // "Fighter/Escort stolen!" + victim's targeters drop it. STR# 0x7d2 0xa9
  // ("Fighter") for post-hit hint 0, 0xa8 ("Escort") otherwise.
  if (boarded.squad_leader_ship_slot == 0 || boarded.post_hit_mode_hint >= 0) {
    const std::uint16_t kind_entry =
        boarded.post_hit_mode_hint == 0 ? 0xa9 : 0xa8;
    std::string text = LoadBoardMiscString(kind_entry, "Escort");
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      Ship &attacker = state.ShipAt(slot);
      if (!attacker.is_active ||
          attacker.primary_target_ship_slot != boarded.ship_instance_id) {
        continue;
      }
      attacker.primary_target_ship_slot = -1;
      attacker.ai_secondary_target_slot = -1;
      attacker.ai_state_code = 0;
      attacker.ai_control_mode = 0;
      attacker.ai_hostility_accumulator = 0;
    }
    text += ' ';
    text += LoadBoardMiscString(0x176, "stolen!");
    NovaHud_ShowOverlayMessage(
        state, std::move(text), static_cast<std::uint64_t>(400U));
    QueueUiSound(state, 1, 1);
  }

  // Faction conversion: the victim becomes the boarder's behavior-6 follower.
  boarded.squad_leader_ship_slot = boarder.ship_instance_id;
  boarded.faction_or_government_id = boarder.faction_or_government_id;
  boarded.pers_def_slot = -1;
  boarded.ai_behavior_code = 6;
  boarded.post_hit_mode_hint = -1;
  boarded.ai_state_code = 0;
  boarded.ai_control_mode = 0;
  boarded.boarded_target_latch = 0;
  boarded.ai_maneuver_timer_ms = 150.0F;
  // Ship_ComputeShipMaxArmor (0x004637a0) x double 00575168 = 0.66. The NPC
  // mission-fleet multiplier / behavior-5 difficulty scaling of that helper
  // is not modelled (TODO(decomp)).
  const float max_armor = boarded_class != nullptr
                              ? static_cast<float>(boarded_class->base_armor)
                              : boarded.armor_points;
  boarded.armor_points = max_armor * 0.66F;
  boarded.shield_points = 0.0F;
}

} // namespace game
