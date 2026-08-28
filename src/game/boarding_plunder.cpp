#include "boarding_plunder.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_audio.hpp"
#include "../sdl_platform.hpp"
#include "hud_overlay.hpp"
#include "scenario_data.hpp"
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

  // ---- Target eligibility (denials share STR# 0x7d2 0x82) ----------------
  const bool rehired_or_surrendering =
      target.escort_rehired_mark == 0 ||
      (target.mission_fleet_slot == -1 && target.post_hit_mode_hint >= 0);
  const bool eligible =
      rehired_or_surrendering && NovaAiShip_IsFireRestricted(state, target) &&
      target.is_active &&
      player.current_system_id == target.current_system_id &&
      target.mission_ship_slot != 0x3ff && !NovaAiShip_IsDestroyed(player);
  if (!eligible) {
    QueueUiSound(state, 3, 1);
    ShowBoardingOverlay(state, 0x82); // "You can't board this ship."
    return;
  }

  constexpr float kBoardVelocityGate = 0.5F; // _DAT_00575598
  constexpr float kBoardRangeShare = 0.5F;   // same constant, per-axis span
  constexpr float kBoardHeadingToleranceDeg = 30.0F;

  // ---- Relative velocity gate -------------------------------------------
  if (std::fabs(target.vel_x - player.vel_x) > kBoardVelocityGate ||
      std::fabs(target.vel_y - player.vel_y) > kBoardVelocityGate) {
    QueueUiSound(state, 3, 1);
    ShowBoardingOverlay(state, 0x84); // "You're moving too fast to board..."
    return;
  }

  // ---- Range gate (per-axis, half of the target's sprite frame) ----------
  const BoardRangeSpan span = TargetSpriteHalfSpan(target);
  if (std::fabs(target.pos_x - player.pos_x) > span.half_x * kBoardRangeShare ||
      std::fabs(target.pos_y - player.pos_y) > span.half_y * kBoardRangeShare) {
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

// Ghidra 0x00482940 NovaUi_RunBoardingPlunderWindow — iteration 3 stub.
// The modal window (DLOG 0x3f3) is not drawn yet; this builds the offers and
// reports the skip so the board command can be exercised end-to-end.
[[nodiscard]] BoardingWindowResult NovaBoarding_RunWindow(
    SdlPlatform & /*platform*/, SdlAudio & /*audio*/, GameState &state) {
  BoardingWindowResult result;
  if (state.player.primary_target_ship_slot == -1) {
    return result; // target lost while the command ran
  }
  const BoardingPlunderOptions options = NovaBoarding_BuildOptions(state);
  NovaLog::Info(
      "board: window stub — offers cargo({})={}x credits={} ammo({})={}x "
      "fuel={} capture_odds={}%",
      options.cargo_type,
      options.cargo_quantity,
      options.credits,
      options.ammo_bank,
      options.ammo_quantity,
      options.fuel_quantity,
      options.capture_odds_percent);
  NovaLog::Todo("board: boarding/plunder window (0x00482940) not drawn yet");
  return result;
}

} // namespace game
