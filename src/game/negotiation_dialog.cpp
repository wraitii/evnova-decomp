#include "negotiation_dialog.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "hud_overlay.hpp"
#include "nova_font.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace game {
namespace {

// ---- Deferred attack/confrontation hooks (TODO(decomp)) -------------------
// The original's attack branch in NovaUi_RunTravelDestinationInteractionWindow
// (0x00480030) invokes two faction-combat hooks that are intentionally NOT
// reconstructed yet (the attack branch is out of scope for this pass). Their
// exact decomp signatures are recorded here so a later pass can wire them up:
//
//   Government_ProcessFactionCombatEvent 0x00466fc0
//     float __cdecl (short system_id, short faction_or_government_id,
//                    short event_code, short mission_fleet_slot)
//     Propagates a combat event (3 = hostile attack here) to the faction in a
//     system and nearby systems, adjusting per-system reputation (the field we
//     model as GameState.system_reputation).
//
//   Mission_ExecuteReactionScript 0x00448020
//     void __cdecl (char * reaction_script_ptr)
//     Runs the target stellar's reaction-script string (StellarDef.field_0x68
//     / 0x167) after an attack.
//
// Both are called only from the deferred attack branch, so no stub is emitted
// here (an unused declaration would trip -Werror); the signatures above are the
// contract a future attack-branch reconstruction should implement.

// ---- Resource ids (EV Nova Graphics 3 / Nova Data) ------------------------
// The destination-interaction window's backdrop PICT (DLOG 0x3f1).
constexpr std::uint16_t kNegotiationBackdropPict = 0x2140;

// The payment modal's backdrop PICT (DLOG 0x3f0) is 0x2142; used by the
// deferred full payment-window reconstruction (see the header scope note).

// STR# pools carrying the interaction status / prompt text. Status strings
// (STR# 0xbba) and prompts (STR# 0xbb8 for prompt_index < 0x26, 0xbb9
// otherwise) each store five flavour variants per message; the dialog picks
// one at `index*5 + random + 1` (see NovaUi_LoadTravelDestinationStatusString
// 0x00482910 / NovaUi_LoadTravelDestinationPromptString 0x004828c0, which
// reads STR# 0xbb8 for prompt indices below 0x26).
constexpr std::uint16_t kStatusStr = 0xbba;
constexpr std::uint16_t kPromptStr = 0xbb8;

// Status/prompt message indices used by the dialog loop (see the decomp of
// NovaUi_RunTravelDestinationInteractionWindow). Kept explicit so the variant
// arithmetic (`index*5 + random + 1`) is readable at the call sites.
constexpr std::uint16_t kMsgWelcomeStatus = 0; // welcome status (not denied)
constexpr std::uint16_t kMsgRefusedPrompt = 2; // denied, not hostile prompt
constexpr std::uint16_t kMsgRefuseNoBribe = 9; // refused, no bribe offered
constexpr std::uint16_t kMsgCannotAfford = 4;  // credits < payment amount
constexpr std::uint16_t kMsgBribeDeclined = 6; // haggle counter-offer
constexpr std::uint16_t kMsgBribeOffered = 8;  // offered before payment

// g_travel_interaction_bribe_random_latch gate: a per-launch roll in [0,100)
// from NovaRandom_Range(100); values > 0x1e (i.e. greater than 30) admit a
// bribe offer from an eligible faction (the 0x4000 bribable bit / no faction).
constexpr int kBribeLatchCutoff = 0x1e; // 30

// The payment window (DLOG 0x3f0) success chance, passed as `chance_percent`
// by the bribe path (0x23 = 35% chance the bribe is accepted).
constexpr int kPaymentChancePercent = 0x23;

// Indexes the five-variant STR# pools: each message's variants live at
// `message_index*5 + variant_index + 1`, with variant_index in [0,5) chosen
// by NovaRandom_Range(5) at launch (`g_travel_interaction_random_index`).
[[nodiscard]] std::uint16_t StringVariantIndex(std::int16_t random_index,
                                               std::uint16_t message_index) {
  return static_cast<std::uint16_t>(message_index * 5u + random_index + 1);
}

// Colours shared by the interaction/payment windows.
constexpr SDL_Color kWindowBg{0, 0, 0, 255};
constexpr SDL_Color kDim{192, 192, 192, 255};
constexpr SDL_Color kTitle{255, 255, 255, 255};

// The dim scrim laid between the flight scene and the modal (the original
// draws the interaction window over the already-composited gameplay surface).
constexpr SDL_Color kScrim{0, 0, 0, 170};

// ---- DLOG 0x3f1 / DITL 0x3f1 geometry ------------------------------------
// The destination-interaction window is 540x295 (DLOG 0x3f1 bounds = the
// backdrop PICT 0x2140, decodes to exactly 540x295), centred on the 640x480
// playfield. All DITL item rects are window-local (0,0 = the backdrop's
// top-left); they must be offset by the centred window origin before drawing.
//
// Layout (entry numbers are one-based UiPanel_GetEntryInfo indices, matching
// NovaUi_HandleTravelDestinationPrimaryButtons reading entries 1/2/3 and
// NovaUi_DrawTravelDestinationInteractionWindow reading entries 4/5/6):
//   [0] Leave (entry 1)       x=27..173 y=244..270  (bottom)
//   [1] Land/Bribe (entry 2)  x=27..173 y=184..210  (top)
//   [2] Attack (entry 3)      x=27..173 y=214..240  (middle)
//   The listed [n] are the *resources* for each slot; the window shows them
//   stacked vertically on the lower-left column (bottom-to-top: Leave, Attack,
//   Land/Bribe).
constexpr int kNegotiationFrameWidth = 540;
constexpr int kNegotiationFrameHeight = 295;
constexpr float kNegotiationWindowX = (640 - kNegotiationFrameWidth) / 2.0F;
constexpr float kNegotiationWindowY = (480 - kNegotiationFrameHeight) / 2.0F;
// The three primary buttons (DITL items 0/1/2), each a 146x26 box at x=27..
// 173, stacked vertically. DITL item 0 = Leave (y=244, bottom), item 1 =
// Land/Bribe (y=184, top), item 2 = Attack (y=214, middle).
constexpr float kNegotiationButtonX = 27.0F + kNegotiationWindowX;
constexpr float kNegotiationButtonW = 146.0F;
constexpr float kNegotiationButtonH = 26.0F;
constexpr float kNegotiationButtonYLeave =
    kNegotiationWindowY + 244.0F; // DITL item 0
constexpr float kNegotiationButtonYLand =
    kNegotiationWindowY + 184.0F; // DITL item 1
constexpr float kNegotiationButtonYAttack =
    kNegotiationWindowY + 214.0F; // DITL item 2
// Status/prompt text panel (DITL item 3, entry 4): 200x60 at x=5..205,
// y=5..65 -- the top-left text block on the frame.
constexpr SDL_FRect kNegotiationStatusRect{5.0F, 5.0F, 200.0F, 60.0F};
// Target stellar image frame (DITL item 4, entry 5): a 310x283 box at
// x=222..532, y=5..288 on the right side of the frame -- filled by the
// destination planet picture.
constexpr SDL_FRect kNegotiationImageRect{222.0F, 5.0F, 310.0F, 283.0F};
// Destination header block (DITL item 5, entry 6): a 120x50 box at x=16..136,
// y=82..132 -- holds the stellar display name and a short note.
constexpr SDL_FRect kNegotiationHeaderRect{16.0F, 82.0F, 120.0F, 50.0F};

// ---- Government flag gates (GovtDef.flags_primary) ------------------------
// 0x8000: the faction "bribes the player" -- raises the bribe cost 1.5x and
// forces bribe eligibility. 0x4000: the faction's ships will take a bribe (an
// additional way to become bribe-eligible). Both read by
// NovaUi_RunTravelDestinationInteractionWindow from government.flags_primary.
constexpr std::uint16_t kGovtFlagBribeCostly = 0x8000;
constexpr std::uint16_t kGovtFlagBribable = 0x4000;

// ---- Bribe cost constants (mirrors the Ghidra global doubles) -------------
// The random bribe upper bound is a meagre fraction of the player's credits
// (_DAT_005758d8 = 1e-06, so `random(credits * 1e-06)`).
constexpr double kCreditsBribeFraction = 1e-06;
// The government 1.5x bribe-cost scale (_DAT_005758e0).
constexpr double kGovtBribeCostMultiplier = 1.5;
// The bribe is clamped down to 1/3 of the player's credits (_DAT_005758b8).
constexpr double kMaxBribeOfCredits = 0.333;
constexpr std::int32_t kBribeBaseAdd = 3000;      // +3000 to the random pick
constexpr std::int32_t kBribeGrouping = 1000;     // round down to /1000
constexpr std::int32_t kBribeLowerBound = 1000;   // >= 1000
constexpr std::int32_t kBribeUpperBound = 900000; // <= 900000

// ---- Payment-window (DLOG 0x3f0) factors (mirrors the Ghidra doubles) -----
// On a successful confirm the payment amount is inflated per mode then
// rounded to /100 (_DAT_00575898 = 0.01): debt mode scales by 1.33
// (_DAT_005758e8, reserved for the mission/return payoff, not used here),
// bribe mode by 0.75 (_DAT_005758f0).
constexpr double kPaymentBribeScale = 0.75;
constexpr double kPaymentRoundFactor = 0.01;

// A declined bribe bumps the counter-offer price by this much and resets the
// latch to force an eventual offer
// (NovaUi_RunTravelDestinationInteractionWindow).
constexpr std::int32_t kHaggleIncrement = 1000;

// Uniform integer in [0, bound). Mirrors the game's NovaRandom_Range using the
// GameState PRNG so negotiation rolls are reproducible per session.
[[nodiscard]] std::int16_t NovaRandomRange(std::mt19937 &rng, int bound) {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::int16_t>(
      std::uniform_int_distribution<int>{0, bound - 1}(rng));
}

// Round-half-up integer form of Ghidra's double-to-int ROUND().
[[nodiscard]] std::int32_t RoundDouble(double v) {
  return static_cast<std::int32_t>(std::llround(v));
}

// Loads one flavour variant of a status (STR# 0xbba) message. `random_index`
// is the per-launch flavour pick; mirrors NovaUi_LoadTravelDestinationStatus-
// String (0x00482910) loading `message*5 + random + 1`.
[[nodiscard]] std::optional<std::string>
LoadStatusVariant(std::int16_t random_index, std::uint16_t message_index) {
  return NovaHud_LoadStringEntry(
      kStatusStr, StringVariantIndex(random_index, message_index));
}

// Loads one flavour variant of a prompt (STR# 0xbb8) message. Mirrors
// NovaUi_LoadTravelDestinationPromptString (0x004828c0) for the common
// prompt_index < 0x26 range (loading `message*5 + random + 1`); the 0xbb9
// higher-index branch is unused by this dialog.
[[nodiscard]] std::optional<std::string>
LoadPromptVariant(std::int16_t random_index, std::uint16_t message_index) {
  return NovaHud_LoadStringEntry(
      kPromptStr, StringVariantIndex(random_index, message_index));
}

// Loads one PICT resource into a texture (null on failure to locate/decode).
std::unique_ptr<SdlTexture> LoadPictTexture(SdlPlatform &platform,
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

// Ghidra 0x004812c0 NovaUi_DrawTravelDestinationInteractionWindow.
// Draws the interaction-window frame over the dim scrim. The 540x295 backdrop
// PICT (DLOG 0x3f1) is centred on the 640x480 playfield; over it we draw the
// destination planet picture into the DITL item-4 image frame on the right, the
// status/prompt text into the item-3 panel top-left, the stellar header (item
// 5) name block, and the three primary buttons (items 0/1/2) stacked
// vertically down the lower-left column (Leave bottom, Attack middle,
// Land/Bribe top).
void DrawNegotiationDialog(SdlPlatform &platform,
                           NovaFontCache &font_cache,
                           const ServicesButtonArt &button_art,
                           SDL_Texture *backdrop,
                           SDL_Texture *planet_art,
                           std::string_view status,
                           std::string_view header,
                           std::string_view land_label,
                           std::span<const ServiceButton> buttons,
                           const SDL_FRect &panel) {
  SDL_Renderer *renderer = platform.renderer();
  SDL_SetRenderDrawColor(
      renderer, kWindowBg.r, kWindowBg.g, kWindowBg.b, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  platform.SetCenteredPlayfield();

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, kScrim.r, kScrim.g, kScrim.b, kScrim.a);
  SDL_RenderFillRect(renderer, &panel);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  // The interaction-window frame is a fixed 540x295 PICT (DLOG 0x3f1), centred
  // on the 640x480 playfield. All DITL item rects are offset by this origin.
  const SDL_FRect frame{kNegotiationWindowX,
                        kNegotiationWindowY,
                        static_cast<float>(kNegotiationFrameWidth),
                        static_cast<float>(kNegotiationFrameHeight)};
  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &frame);
  } else {
    // No backdrop art: draw a bordered placeholder so the dialog stays legible.
    SDL_SetRenderDrawColor(renderer, 16, 40, 72, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &frame);
    SDL_SetRenderDrawColor(renderer, 80, 140, 190, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &frame);
  }

  // Destination planet picture (DITL item 4, entry 5): a 310x283 frame on the
  // right side of the window (x=222..532, y=5..288). The original scales the
  // target stellar's ambient sprite into this panel; we draw the destination
  // planet PICT 1:1 (like the ship-comm portrait), with a bordered placeholder
  // when the picture is absent.
  const SDL_FRect picture{kNegotiationWindowX + kNegotiationImageRect.x,
                          kNegotiationWindowY + kNegotiationImageRect.y,
                          kNegotiationImageRect.w,
                          kNegotiationImageRect.h};
  if (planet_art != nullptr) {
    // Scale the planet art to fit the panel while preserving aspect ratio.
    float aw = 0.0F;
    float ah = 0.0F;
    SDL_GetTextureSize(planet_art, &aw, &ah);
    SDL_FRect dst = picture;
    if (aw > 0.0F && ah > 0.0F) {
      const float scale = std::min(picture.w / aw, picture.h / ah);
      dst.w = aw * scale;
      dst.h = ah * scale;
      dst.x = picture.x + (picture.w - dst.w) / 2.0F;
      dst.y = picture.y + (picture.h - dst.h) / 2.0F;
    }
    SDL_RenderTexture(renderer, planet_art, nullptr, &dst);
  } else {
    SDL_SetRenderDrawColor(renderer, 8, 24, 44, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &picture);
    SDL_SetRenderDrawColor(renderer, 80, 140, 190, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &picture);
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          11.0F,
                          kNovaFontStyleRegular,
                          kDim,
                          picture.x,
                          picture.x + picture.w,
                          picture.y + picture.h / 2.0F,
                          "[no picture]");
  }

  // Destination header (DITL item 5, entry 6): the stellar display name and a
  // short note in the small box under the status panel.
  const SDL_FRect header_rect{kNegotiationWindowX + kNegotiationHeaderRect.x,
                              kNegotiationWindowY + kNegotiationHeaderRect.y,
                              kNegotiationHeaderRect.w,
                              kNegotiationHeaderRect.h};
  if (!header.empty()) {
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          12.0F,
                          kNovaFontStyleBold,
                          kTitle,
                          header_rect.x,
                          header_rect.x + header_rect.w,
                          header_rect.y + header_rect.h / 2.0F + 2.0F,
                          header);
  }

  // Status / prompt text block (DITL item 3, entry 4): a 200x60 panel at the
  // top-left of the frame (x=5..205, y=5..65), centred vertically.
  const SDL_FRect status_rect{kNegotiationWindowX + kNegotiationStatusRect.x,
                              kNegotiationWindowY + kNegotiationStatusRect.y,
                              kNegotiationStatusRect.w,
                              kNegotiationStatusRect.h};
  if (!status.empty()) {
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          12.0F,
                          kNovaFontStyleBold,
                          kTitle,
                          status_rect.x,
                          status_rect.x + status_rect.w,
                          status_rect.y + status_rect.h / 2.0F,
                          status);
  }

  // The three primary buttons stacked vertically down the lower-left column
  // (DITL items 0/1/2): Leave at the bottom, Attack in the middle, Land/Bribe
  // at the top -- each a 146x26 box at x=27..173. Labels follow the original
  // per-slot table ({LEAVE, LAND/BRIBE, ATTACK}); the attack branch is
  // deferred (see the header scope note).
  // Three-state button labels: white on the normal art, plain screen font
  // (the original's shared renderer never bolds them; see
  // NovaUi_InitThreeStateButtonArt DAT_007d8350 / NovaUi_DrawThreeStateButton).
  constexpr SDL_Color kButtonLabel{255, 255, 255, 255};
  const std::string_view labels[3] = {"LEAVE", land_label, "ATTACK"};
  for (const ServiceButton &b : buttons) {
    const std::size_t slot = static_cast<std::size_t>(b.slot) < 3
                                 ? static_cast<std::size_t>(b.slot)
                                 : 0;
    button_art.Draw(platform, b.rect, ButtonState::kNormal);
    NovaText_DrawCentered(platform,
                          font_cache,
                          kThreeStateButtonFontFamily,
                          kThreeStateButtonFontSize,
                          kNovaFontStyleRegular,
                          kButtonLabel,
                          b.rect.x,
                          b.rect.x + b.rect.w,
                          ThreeStateButtonLabelBaseline(b.rect),
                          labels[slot]);
  }

  // Footer hint, just above the top (Land/Bribe) button's rect.
  const float hint_y =
      buttons.empty() ? 480.0F - 50.0F : buttons[0].rect.y - 16.0F;
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        12.0F,
                        kNovaFontStyleRegular,
                        kDim,
                        panel.x,
                        panel.x + panel.w,
                        hint_y,
                        "Esc / Enter / click to close");
}

} // namespace

// ---------------------------------------------------------------------------
// Bribe cost computation (seeded by the GameState PRNG)
// ---------------------------------------------------------------------------
std::int32_t NovaNegotiation_ComputeBribeCost(std::mt19937 &rng,
                                              std::int32_t credits,
                                              std::int16_t government_id) {
  (void)government_id; // the 1.5x government scale is applied by the caller
                       // (which resolves the government table); this helper is
                       // kept pure and returns the un-scaled base cost.
  // Base: `random(0..ceil(credits * 1e-06)) * 1000 + 3000`, mirroring the
  // decompile `g_travel_bribe_cost = (NovaRandom_Range(credits*1e-06)) * 1000
  // + 3000`. The random upper bound is `credits * 1e-06`, drawn from the same
  // GameState PRNG so the price is reproducible per session.
  std::int64_t upper = static_cast<std::int64_t>(
      std::floor(static_cast<double>(std::max(std::int32_t{1}, credits)) *
                 kCreditsBribeFraction));
  upper = std::max<std::int64_t>(1, upper);
  const std::int64_t pick =
      1 +
      static_cast<std::int64_t>(NovaRandomRange(rng, static_cast<int>(upper)));
  std::int64_t cost = pick * kBribeGrouping + kBribeBaseAdd;

  // Clamp to 1/3 of credits, round down to /1000, then clamp bounds.
  std::int64_t credit_cap = static_cast<std::int64_t>(
      static_cast<double>(std::max(std::int32_t{0}, credits)) *
      kMaxBribeOfCredits);
  cost = std::min(cost, credit_cap);
  cost = (cost / kBribeGrouping) * kBribeGrouping;
  cost = std::max<std::int64_t>(kBribeLowerBound,
                                std::min<std::int64_t>(kBribeUpperBound, cost));
  return static_cast<std::int32_t>(cost);
}

// ---------------------------------------------------------------------------
// SDL modal
// ---------------------------------------------------------------------------
NegotiationExit NovaNegotiation_RunDestinationDialog(SdlPlatform &platform,
                                                     GameState &state,
                                                     std::int16_t stellar_id) {
  NovaLog::Info("opening destination-interaction dialog 0x3f1 for stellar {}",
                static_cast<int>(stellar_id));

  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr) {
    NovaLog::Warn("target stellar {} not in the scenario; cancelling the "
                  "interaction dialog",
                  static_cast<int>(stellar_id));
    return NegotiationExit::kClosed;
  }

  // Derive the negotiation state (mirrors NovaUi_RunTravelDestinationInterac-
  // tionWindow's opening gates):
  //   denied: the stellar's reputation_threshold gates landing. 0x7fff means
  //           "always denied"; -0x7fff means "never denied"; otherwise the
  //           containing system's reputation must be >= the threshold.
  const std::int16_t sys_index =
      stellar->system_id >= 0 &&
              stellar->system_id <
                  static_cast<std::int16_t>(state.system_reputation.size())
          ? stellar->system_id
          : -1;
  const std::int16_t sys_rep =
      sys_index >= 0
          ? state.system_reputation[static_cast<std::size_t>(sys_index)]
          : 0;
  const bool always_denied = (stellar->min_status == 0x7fff);
  const bool never_denied = (stellar->min_status == -0x7fff);
  const bool denied =
      always_denied || (!never_denied && sys_rep < stellar->min_status);

  // Item 1: per-launch random flavour index (g_travel_interaction_random_index)
  // and the bribe-offer latch (g_travel_interaction_bribe_random_latch), both
  // from the GameState PRNG so they are reproducible per session and stable
  // across the modal's redraws.
  const std::int16_t random_index = NovaRandomRange(state.rng, 5);
  int bribe_random_latch = NovaRandomRange(state.rng, 100);

  // bribe-eligible mirrors the original gates: a denied target accepts a bribe
  // from a faction whose ships take bribes (0x4000) or, with no faction at all,
  // only when the per-launch latch exceeds the 0x1e cutoff; the faction whose
  // ships are "bribes the player" (0x8000) forces an offer regardless.
  const Government *gov =
      stellar->government_id != -1
          ? state.scenario.Government(stellar->government_id)
          : nullptr;
  const bool no_faction = gov == nullptr;
  const bool gov_bribable =
      gov != nullptr && (gov->flags_primary & kGovtFlagBribable) != 0U;
  const bool gov_costly =
      gov != nullptr && (gov->flags_primary & kGovtFlagBribeCostly) != 0U;
  const bool latch_admits = bribe_random_latch > kBribeLatchCutoff;
  const bool bribe_eligible =
      (denied && (no_faction || gov_bribable) && latch_admits) || gov_costly;

  // Resolve the bribe cost (credits-scaled, random, clamped) with the
  // government's 1.5x scale applied when the costly-bribe flag is set.
  std::int32_t bribe_cost = NovaNegotiation_ComputeBribeCost(
      state.rng, state.player.credits, stellar->government_id);
  if (gov_costly) {
    bribe_cost = static_cast<std::int32_t>(std::llround(
        static_cast<double>(bribe_cost) * kGovtBribeCostMultiplier));
  }

  // Load the status/prompt text, indexing one of the five STR# flavour variants
  // via StringVariantIndex (which applies `message*5 + random_index + 1`).
  // Not denied -> welcome status 0; denied (no hostility modeled) -> the
  // refused/approach prompt 2, mirroring the original's initial text.
  std::string status;
  if (denied) {
    if (auto s = LoadPromptVariant(random_index, kMsgRefusedPrompt)) {
      status = *s;
    } else {
      status = "The " + stellar->name + " refuses to let you land.";
    }
  } else {
    if (auto p = LoadStatusVariant(random_index, kMsgWelcomeStatus)) {
      status = *p;
    } else {
      status = "How would you like to approach " + stellar->name + "?";
    }
  }

  auto backdrop = LoadPictTexture(platform, kNegotiationBackdropPict);
  if (!backdrop) {
    NovaLog::Todo("destination-interaction backdrop PICT 0x2140 unavailable; "
                  "drawing a bordered placeholder");
  }

  // The destination planet picture shown in the DITL item-4 image frame (the
  // original scales the target stellar's ambient sprite into it). The planet
  // PICT comes from the same selection the docked screen uses: the stellar's
  // custom picture (engage_highlight_frame >= 0x80) or link_a_id + 0x2710.
  // This mirrors the destination-planet art in landed_window.cpp.
  std::unique_ptr<SdlTexture> planet_art;
  if (stellar) {
    const std::int16_t stell_pict =
        (stellar->engage_highlight_frame >= 0x80)
            ? stellar->engage_highlight_frame
            : static_cast<std::int16_t>(stellar->link_a_id + 0x2710);
    if (stell_pict >= 0x80) {
      planet_art =
          LoadPictTexture(platform, static_cast<std::uint16_t>(stell_pict));
      if (planet_art) {
        NovaLog::Info("negotiation destination PICT 0x{} ({}) for stellar {}",
                      static_cast<int>(stell_pict),
                      stellar->name,
                      static_cast<int>(stellar_id));
      } else {
        NovaLog::Todo("no negotiation destination PICT {} for stellar '{}'; "
                      "the image frame stays a flat placeholder",
                      static_cast<int>(stell_pict),
                      stellar->name);
      }
    }
  }
  ServicesButtonArt button_art;
  if (!button_art.Initialize(platform)) {
    NovaLog::Warn("three-state button art unavailable for the negotiation "
                  "dialog buttons");
  }
  NovaFontCache font_cache;
  const SDL_FRect panel{0.0F, 0.0F, 640.0F, 480.0F};
  platform.SetCenteredPlayfield();

  // The middle button label: "LAND" when docking is freely allowed, "BRIBE"
  // when the faction refuses and a bribe is on offer (mirrors the original
  // label swap 0x15 land / 0x17 bribe gated on
  // g_travel_interaction_denied_state).
  const std::string land_label = (denied && bribe_eligible) ? "BRIBE" : "LAND";

  // ---- Button geometry / hit-testing --------------------------------------
  // The three primary buttons stack vertically down the frame's lower-left
  // column (DITL items 0/1/2), each 146x26 at x=27..173, slot order bottom-to-
  // top: Leave (item 0, y=244), Attack (item 2, y=214), Land/Bribe (item 1,
  // y=184). The existing `ServiceButton`/`ServiceButtonAt` abstractions make
  // the drawn rects and the click hit-test share identical geometry.
  enum ButtonSlot : std::uint8_t { kLeave = 0, kLandBribe = 1, kAttack = 2 };

  const SDL_FRect btn_rects[3] = {
      {kNegotiationButtonX,
       kNegotiationButtonYLeave,
       kNegotiationButtonW,
       kNegotiationButtonH},
      {kNegotiationButtonX,
       kNegotiationButtonYLand,
       kNegotiationButtonW,
       kNegotiationButtonH},
      {kNegotiationButtonX,
       kNegotiationButtonYAttack,
       kNegotiationButtonW,
       kNegotiationButtonH},
  };
  std::vector<ServiceButton> buttons;
  buttons.reserve(3);
  for (std::uint8_t i = 0; i < 3; ++i) {
    buttons.push_back(ServiceButton{btn_rects[i], i});
  }

  // Destination header: the stellar display name shown in the DITL item-5
  // (entry 6) block, mirroring NovaUi_DrawTravelDestinationInteractionWindow
  // drawing g_stellar_defs[...].display_name there.
  const std::string header = stellar->name;

  while (!platform.quit_requested()) {
    DrawNegotiationDialog(platform,
                          font_cache,
                          button_art,
                          backdrop ? backdrop->get() : nullptr,
                          planet_art ? planet_art->get() : nullptr,
                          status,
                          header,
                          land_label,
                          buttons,
                          panel);
    SDL_RenderPresent(platform.renderer());

    bool proceed_to_land = false;
    bool redraw_status = false;
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      switch (in->key) {
      case TextKey::escape:
      case TextKey::enter:
        return NegotiationExit::kClosed;
      case TextKey::primary: {
        // Click the Leave / Land-Bribe / Attack button under the cursor
        // (ServiceButtonAt hit-test). A click outside the buttons is ignored.
        const auto clicked =
            ServiceButtonAt(buttons, platform.mouse_position());
        if (!clicked) {
          break;
        }
        switch (static_cast<ButtonSlot>(*clicked)) {
        case kLeave:
          return NegotiationExit::kClosed;
        case kLandBribe:
          if (denied && !bribe_eligible) {
            // The faction refuses with no bribe on offer: show the refusal
            // status and stay.
            if (auto s = LoadStatusVariant(random_index, kMsgRefuseNoBribe)) {
              status = *s;
            } else {
              status = "The " + stellar->name + " refuses to let you land.";
            }
            redraw_status = true;
            break;
          }
          if (denied) {
            // Item 3: bribe path -- a nested DLOG 0x3f0-style payment confirm.
            // Show the offered bribe status, then let the player confirm or
            // refuse (the price-haggle). This is a single-pass confirm: the
            // payment sub-window closes on the first decision (pay / haggle /
            // exit), avoiding a nested busy-wait.
            if (auto s = LoadStatusVariant(random_index, kMsgBribeOffered)) {
              status = *s;
            }
            redraw_status = true;

            // The eventual resolution of the payment sub-window.
            bool paid_ok = false;
            bool haggled = false;
            std::int32_t charged = 0;
            for (std::optional<TextInput> p; (p = platform.PollTextEvent());) {
              if (p->key == TextKey::escape || p->key == TextKey::enter) {
                break; // player closes the payment sub-window, no decision
              }
              if (p->key != TextKey::primary) {
                continue;
              }
              // Roll the payment window's random success (35%) and, on
              // success, inflate the bribe by the bribe-mode factor then round
              // to /100 (mirrors NovaUi_RunTravelDestinationPaymentWindow).
              const int can_pay =
                  NovaRandomRange(state.rng, 100) <= kPaymentChancePercent;
              if (!can_pay) {
                // Haggle counter-offer: declined bribe -> +1000 price and the
                // offer is kept available to re-try (the original resets the
                // random latch so an offer is eventually re-made).
                bribe_cost += kHaggleIncrement;
                if (auto s =
                        LoadStatusVariant(random_index, kMsgBribeDeclined)) {
                  status = *s;
                }
                haggled = true;
                break;
              }
              charged = bribe_cost;
              charged = RoundDouble(kPaymentBribeScale * charged);
              charged = RoundDouble(kPaymentRoundFactor * charged) * 100;
              if (state.player.credits < charged) {
                if (auto s =
                        LoadStatusVariant(random_index, kMsgCannotAfford)) {
                  status = *s;
                }
                redraw_status = true;
                break;
              }
              paid_ok = true;
              break;
            }

            if (haggled) {
              // The dialog stays open with the raised counter-offer; the
              // player may re-click the Bribe button to try again.
              redraw_status = true;
            } else if (paid_ok) {
              state.player.credits -= charged;
              NovaLog::Info("bribe of {} accepted at stellar {}; dock "
                            "granted",
                            charged,
                            static_cast<int>(stellar_id));
              NovaHud_ShowOverlayMessage(
                  state, "Bribe accepted -- docking.", 0xe0, 0xe0, 0xe0, 250);
              proceed_to_land = true;
            }
            break;
          }
          // Not denied: the player simply lands.
          NovaLog::Info("landing accepted at stellar {} via the interaction "
                        "dialog",
                        static_cast<int>(stellar_id));
          proceed_to_land = true;
          break;
        case kAttack:
          // The attack/confrontation branch is out of scope for this pass (a
          // loud Todo; see the header scope note). Leave it a no-op that shows
          // the refusal status so the player is not trapped.
          NovaLog::Todo("target-action: attack/confrontation branch at a "
                        "destination is not reconstructed");
          if (auto s = LoadStatusVariant(random_index, kMsgRefuseNoBribe)) {
            status = *s;
          }
          redraw_status = true;
          break;
        }
        break;
      }
      default:
        break;
      }
    }

    if (proceed_to_land) {
      // Hand off to the Spaceport (DLOG 0x3e8) by staging the target stellar,
      // exactly as the original's land/bribe path sets
      // g_travel_selected_stellar_id.
      state.travel.selected_stellar_id = stellar_id;
      return NegotiationExit::kProceedToLand;
    }
    if (redraw_status) {
      continue; // re-draw with the updated status immediately
    }
    SDL_Delay(16);
  }
  return NegotiationExit::kQuit;
}

} // namespace game
