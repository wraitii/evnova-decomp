#include "ship_comm_dialog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "nova_font.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "ship_ai.hpp"
#include "targeting.hpp"

#include <SDL3/SDL.h>

namespace game {
namespace {

// ---- Resource ids ----------------------------------------------------------
// The ship-comm window's backdrop PICT (DLOG 0x3ef, the Communications frame).
constexpr std::uint16_t kCommFramePict = 0x213f;

// ---- DLOG 0x3ef / DITL 0x3ef geometry -------------------------------------
// The comm window is 423x215 (DLOG 0x3ef bounds = the backdrop PICT 0x213f,
// decodes to exactly 423x215), centred on the 640x480 playfield at
// window-relative (0,0) = top-left of the backdrop. The three context buttons
// are DITL items 0/1/2 stacked *vertically* on the lower-left (166x26 each at
// x=21..187, y=125/153/181), NOT a bottom row; the ship portrait is DITL item
// 10, a 200x200 box on the right (x=216..416, y=7..207); the ship name is item
// 9 (x=11..203, y=8..66) and the status/prompt block is item 11 (x=40..174,
// y=73..119). The remaining DITL items (3..8, below y=207) sit past the
// 215-tall backdrop and are clipped/redundant in the shipped window.
constexpr int kCommFrameWidth = 423;
constexpr int kCommFrameHeight = 215;
// Window origin on the 640x480 playfield (centred frame).
constexpr float kCommWindowX = (640 - kCommFrameWidth) / 2.0F;
constexpr float kCommWindowY = (480 - kCommFrameHeight) / 2.0F;
// Portrait (DITL item 10): 200x200 on the right side.
constexpr SDL_FRect kCommPictureRect{216.0F, 7.0F, 200.0F, 200.0F};
// Ship name (DITL item 9) / status block (DITL item 11).
constexpr SDL_FRect kCommNameRect{11.0F, 8.0F, 192.0F, 58.0F};
constexpr SDL_FRect kCommStatusRect{40.0F, 73.0F, 134.0F, 46.0F};
// Buttons (DITL items 0/1/2), 166x26, stacked vertically. DITL item 0 = the
// Close Channel button (y=181, bottom), item 1 = the assistance button
// (y=153, middle: label Request Assistance | Beg For Mercy | Release, runs
// the assistance dialogue), item 2 = the Greetings button (y=125, top:
// shows the hail-info text). Label indices come from
// NovaUi_DrawTravelDestinationContextButtons' DAT_007d82ee/f0/f2 table.
constexpr float kCommButtonX = 21.0F;
constexpr float kCommButtonW = 166.0F;
constexpr float kCommButtonH = 26.0F;
constexpr float kCommButtonYTop = 125.0F;    // Greetings button
constexpr float kCommButtonYMiddle = 153.0F; // assistance button
constexpr float kCommButtonYClose = 181.0F;

// Prompt pools: STR# 0xbb8 "Ship Comm Strings" for prompt_index < 0x26, STR#
// 0xbb9 "More Ship Comm" for prompt_index >= 0x26 (the escort-goodbye pool),
// each at `index*5 + random + 1` / `(index*5 + random) - 0xbd` respectively
// (NovaUi_LoadTravelDestinationPromptString 0x004828c0).
constexpr std::uint16_t kPromptStr = 0xbb8;
constexpr std::uint16_t kPromptStrHigh = 0xbb9;

// STR# 0x7d2 "misc strings": the ship status labels drawn under the ship
// picture ("Fighter" / "Captured Escort"). (The hail-failure overlays 0x35/
// 0x36 are used by the spaceflight target-action handler directly.)
constexpr std::uint16_t kMiscStr = 0x7d2;
constexpr std::uint16_t kMiscFighterLabel = 0xa8;        // "Fighter"
constexpr std::uint16_t kMiscCapturedEscortLabel = 0xa6; // "Captured Escort"

// STR# 0x96 "button labels" (the three-state button label table DAT_007d82ee/
// f0/f2 indexes in the original): 0x14 Close Channel, 0x15 Greetings,
// 0x16 Request Assistance, 0x18 Beg For Mercy, 0x1f Release.
constexpr std::uint16_t kButtonLabelStr = 0x96;
constexpr std::uint16_t kBtnCloseChannel = 0x14;
constexpr std::uint16_t kBtnGreetings = 0x15;
constexpr std::uint16_t kBtnRequestAssistance = 0x16;
constexpr std::uint16_t kBtnBegForMercy = 0x18;
constexpr std::uint16_t kBtnRelease = 0x1f;

// ---- Prompt message indices (into the *5+random+1 pools) ------------------
// Names come from the actual STR# 0xbb8 content (see the pool dump).
constexpr std::uint16_t kMsgHailOpen = 0;      // "Communications channel open."
constexpr std::uint16_t kMsgNoResponse = 1;    // "No response."
constexpr std::uint16_t kMsgWhatDoYouWant = 2; // "What is it you want?"
constexpr std::uint16_t kMsgWhatCanIDo = 4;    // "What can I do for you?"
constexpr std::uint16_t kMsgWhatCanIDoSir = 5; // "What can I do for you, sir?"
constexpr std::uint16_t kMsgCantAfford = 0xc;  // "Yeah, come back when you
                                               // actually have some money."
constexpr std::uint16_t kMsgStopWastingTime = 0xd; // "Stop wasting my time."
constexpr std::uint16_t kMsgNoTrouble = 0xe;  // "You're not in any trouble."
constexpr std::uint16_t kMsgImBusy = 0x10;    // "I'm busy."
constexpr std::uint16_t kMsgRatherNot = 0x11; // "I'd rather not."
constexpr std::uint16_t kMsgPayFirst = 0x12;  // "You'll have to pay me first."
constexpr std::uint16_t kMsgDreams = 0x13;    // "In your dreams, pal."
constexpr std::uint16_t kMsgBusiness = 0x14;  // "A pleasure doing business."
constexpr std::uint16_t kMsgCantDoSir = 0x16; // "Sorry sir, I can't do that."
constexpr std::uint16_t kMsgGoodMood = 0x17;  // "You're in luck, I'm in a
                                              // good mood today."
constexpr std::uint16_t kMsgTerribleMood = 0x18; // "I'm in a terrible mood..."
constexpr std::uint16_t kMsgCantDo = 0x19;       // "I can't do that."
constexpr std::uint16_t kMsgHelpIfPay = 0x1c;    // "I'll help you out if you
                                                 // pay me."
constexpr std::uint16_t kMsgOnMyWay = 0x1d;      // "Okay, I'm on my way."
constexpr std::uint16_t kMsgPrepareToDie = 0x1e; // "What? How dare you!
                                                 // Prepare to die!"
constexpr std::uint16_t kMsgComedian = 0x1f;     // "Ha ha ha. What a comedian."
constexpr std::uint16_t kMsgEscortGoodbye =
    0x26; // STR# 0xbb9 (>= 0x26):
          // "See you around the galaxy."

// ---- Government flag gates (GovtDef.flags_primary) ------------------------
// 0x2000: behavior<3 ships take bribes; 0x200: behavior>2 ships take bribes;
// 0x8000: "bribes the player" (expensive bribe branch); 0x1000: keep-flag
// (behavior>2 ships with 0x1000 refuse aid when hailed); 0x0001: xenophobic /
// govt-aid flag (DAT_007d17f4 in the comm window).
constexpr std::uint16_t kGovtFlagBribePeaceful = 0x2000;
constexpr std::uint16_t kGovtFlagBribeAggressive = 0x200;
constexpr std::uint16_t kGovtFlagBribesPlayer = 0x8000;
constexpr std::uint16_t kGovtFlagKeepRefusal = 0x1000;
constexpr std::uint16_t kGovtFlagXenophobic = 0x0001;

// ---- Personality / bribe cost constants (Ghidra doubles) ------------------
// personality = (rand(0x29) + 0x50) * 0.01 (DAT_00575898) with a 1-in-5 -0.5
// mood swing (DAT_005758a0) or a further 1-in-5 +0.5. Drives the bribe price
// and the mood prompt pick (0x17 below 0.8, 0x1c in [0.8,1.2], 0x18 above).
constexpr float kPersonalityStep = 0.01F;
constexpr float kPersonalityMoodSwing = 0.5F;
constexpr double kPersonalityMoodLow = 0.8;
constexpr double kPersonalityMoodHigh = 1.2;
// Bribe upper bound: rand(credits * 5e-07) (DAT_005758a8), *1000 + 3000, times
// the personality factor. The govt 0x8000 branch uses rand(credits * 1e-4)
// (DAT_005758b0), *1000 + 10000. Then capped at 1/3 credits (DAT_005758b8),
// rounded down to /1000 and clamped to [1000, 20000].
constexpr double kCreditsBribeFraction = 5e-07;
constexpr double kGovtBribeFraction = 1e-4;
constexpr double kMaxBribeOfCredits = 0.333;
constexpr std::int32_t kBribeBaseAdd = 3000;
constexpr std::int32_t kGovtBribeBaseAdd = 10000;
constexpr std::int32_t kBribeGrouping = 1000;
constexpr std::int32_t kBribeLowerBound = 1000;
constexpr std::int32_t kBribeUpperBound = 20000;

// The payment window (DLOG 0x3f0) success chance passed by the bribe path
// (0x23 = 35%). A failed roll bumps the bribe cost by 1000 (the haggle).
constexpr int kPaymentChancePercent = 0x23;
constexpr std::int32_t kHaggleIncrement = 1000;
// Payment amount inflation on a successful confirm (bribe mode): * 0.75
// (DAT_005758f0) then rounded to /100 (DAT_00575898).
constexpr double kPaymentBribeScale = 0.75;
constexpr double kPaymentRoundFactor = 0.01;

// Player fuel threshold below which a ship offers to sell fuel (DAT_005758d0,
// a float 100.0).
constexpr float kPlayerFuelOfferThreshold = 100.0F;

// ---- Dialog colours --------------------------------------------------------
constexpr SDL_Color kWindowBg{0, 0, 0, 255};
constexpr SDL_Color kDim{128, 170, 210, 255};
constexpr SDL_Color kTitle{202, 224, 255, 255};
constexpr SDL_Color kScrim{0, 0, 0, 170};

// Uniform integer in [0, bound). Mirrors NovaRandom_Range using the GameState
// PRNG so all comm rolls are reproducible per session.
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

// Loads one flavour variant of a ship-comm prompt (STR# 0xbb8 for
// prompt_index < 0x26, else STR# 0xbb9 at (index*5+random)-0xbd), mirroring
// NovaUi_LoadTravelDestinationPromptString (0x004828c0).
[[nodiscard]] std::optional<std::string>
LoadCommPrompt(std::int16_t random_index, std::uint16_t prompt_index) {
  if (prompt_index < 0x26) {
    return NovaHud_LoadStringEntry(
        kPromptStr,
        static_cast<std::uint16_t>(prompt_index * 5u + random_index + 1));
  }
  const std::uint16_t entry =
      static_cast<std::uint16_t>(prompt_index * 5u + random_index - 0xbd);
  return NovaHud_LoadStringEntry(kPromptStrHigh, entry);
}

// Loads a STR# 0x96 button label with a fallback for missing resources.
[[nodiscard]] std::string LoadButtonLabel(std::uint16_t index) {
  if (auto s = NovaHud_LoadStringEntry(kButtonLabelStr, index)) {
    return *s;
  }
  return "?";
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

// The ship-comm bribe cost. Mirrors the opening block of
// NovaUi_RunTargetShipCommWindow (0x0047e470): a random pick over
// `credits * 5e-07` (or `credits * 1e-4` for the govt-0x8000 branch) drawn
// from `rng`, expressed as `pick * 1000 + base` times the personality factor,
// then capped at 1/3 of credits, rounded down to /1000 and clamped to
// [1000, 20000]. `personality` and `rng` are shared with the caller so the
// cost is reproducible per session and consistent with the mood prompts.
[[nodiscard]] std::int32_t ComputeShipCommBribeCost(std::mt19937 &rng,
                                                    float personality,
                                                    std::int32_t credits,
                                                    std::int16_t government_id,
                                                    bool govt_bribes_player) {
  const double fraction =
      govt_bribes_player ? kGovtBribeFraction : kCreditsBribeFraction;
  const std::int32_t base =
      govt_bribes_player ? kGovtBribeBaseAdd : kBribeBaseAdd;
  std::int64_t upper = static_cast<std::int64_t>(std::floor(
      static_cast<double>(std::max(std::int32_t{1}, credits)) * fraction));
  upper = std::max<std::int64_t>(1, upper);
  const std::int64_t pick =
      1 +
      static_cast<std::int64_t>(NovaRandomRange(rng, static_cast<int>(upper)));
  double cost = static_cast<double>((pick * kBribeGrouping + base)) *
                static_cast<double>(personality);
  std::int64_t cost_int = RoundDouble(cost);

  // Cap at 1/3 of credits (only when the cap is smaller than the cost).
  const std::int64_t credit_cap = static_cast<std::int64_t>(
      static_cast<double>(std::max(std::int32_t{0}, credits)) *
      kMaxBribeOfCredits);
  if (static_cast<double>(credit_cap) < static_cast<double>(cost_int)) {
    cost_int = credit_cap;
  }
  cost_int = (cost_int / kBribeGrouping) * kBribeGrouping;
  cost_int = std::max<std::int64_t>(
      kBribeLowerBound, std::min<std::int64_t>(kBribeUpperBound, cost_int));
  (void)government_id;
  return static_cast<std::int32_t>(cost_int);
}

// Draws the comm-window frame over the dim scrim: the PICT 0x213f backdrop
// centred at natural size (falling back to a bordered placeholder), the ship
// picture box + name panel in the upper-left, the status text block, and the
// three context buttons.
void DrawShipCommDialog(SdlPlatform &platform,
                        NovaFontCache &font_cache,
                        const ServicesButtonArt &button_art,
                        SDL_Texture *backdrop,
                        SDL_Texture *ship_picture,
                        std::string_view ship_name,
                        std::string_view name_status,
                        std::string_view prompt_text,
                        std::span<const ServiceButton> buttons,
                        std::span<const std::string> button_labels,
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

  // The comm window frame is a fixed 423x215 PICT (DLOG 0x3ef), centred on the
  // 640x480 playfield. All DITL item rects are offset by this origin.
  const SDL_FRect frame{kCommWindowX, kCommWindowY,
                        static_cast<float>(kCommFrameWidth),
                        static_cast<float>(kCommFrameHeight)};
  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &frame);
  } else {
    // No backdrop art: draw a bordered placeholder so the dialog stays legible.
    SDL_SetRenderDrawColor(renderer, 16, 40, 72, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &frame);
    SDL_SetRenderDrawColor(renderer, 80, 140, 190, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &frame);
  }

  // Ship portrait (DITL item 10): a 200x200 box on the right side of the
  // frame, filled by the class's 200x200 portrait PICT (5000 + class id).
  const SDL_FRect picture{kCommWindowX + kCommPictureRect.x,
                          kCommWindowY + kCommPictureRect.y,
                          kCommPictureRect.w,
                          kCommPictureRect.h};
  if (ship_picture != nullptr) {
    SDL_RenderTexture(renderer, ship_picture, nullptr, &picture);
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

  // Ship name (DITL item 9) at the top of the frame's left panel, with the
  // government / escort status line beneath it.
  const SDL_FRect name{kCommWindowX + kCommNameRect.x,
                       kCommWindowY + kCommNameRect.y,
                       kCommNameRect.w,
                       kCommNameRect.h};
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        13.0F,
                        kNovaFontStyleBold,
                        kTitle,
                        name.x,
                        name.x + name.w,
                        name.y + name.h / 2.0F + 2.0F,
                        ship_name);
  if (!name_status.empty()) {
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          11.0F,
                          kNovaFontStyleRegular,
                          kDim,
                          name.x,
                          name.x + name.w,
                          name.y + name.h / 2.0F + 14.0F,
                          name_status);
  }

  // Prompt / status message text (DITL item 11) below the name.
  const SDL_FRect status{kCommWindowX + kCommStatusRect.x,
                         kCommWindowY + kCommStatusRect.y,
                         kCommStatusRect.w,
                         kCommStatusRect.h};
  if (!prompt_text.empty()) {
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          12.0F,
                          kNovaFontStyleBold,
                          kTitle,
                          status.x,
                          status.x + status.w,
                          status.y + status.h / 2.0F,
                          prompt_text);
  }

  // Three context buttons stacked vertically on the lower-left (DITL items
  // 0/1/2) in slot order bottom-to-top: Close Channel, assistance (label
  // Request Assistance | Beg For Mercy | Release), Greetings. Labels follow
  // the original's DAT_007d82ee/f0/f2 table. For special-scan-mask ships the
  // assistance slot is already omitted from `buttons` (see caller), so only
  // Close Channel and Greetings (pushed down to the middle) are drawn.
  // Labels follow the original three-state button palette: pure white on the
  // normal art in the plain screen font (NovaUi_DrawThreeStateButton sets only
  // font id + size, never bold; label colour table DAT_007d8350).
  constexpr SDL_Color kButtonLabel{255, 255, 255, 255};
  for (const ServiceButton &b : buttons) {
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
                          b.slot < button_labels.size()
                              ? button_labels[b.slot]
                              : "");
  }
}

// Outcome of the shared bribe sub-flow (NovaUi_RunTravelDestinationBribeWindow
// 0x0047f9c0): 1 = paid, 0 = refused/haggled (cost += 1000), -1 = cannot
// afford. `free_help` mirrors the local_10 bit0 skip: when set, the bribe is
// granted without opening the payment window or charging credits.
enum class BribeOutcome : std::int8_t {
  kPaid = 1,
  kRefused = 0,
  kCannotAfford = -1
};

// Single-pass bribe payment: rolls the payment window's 35% acceptance, and
// on success inflates the amount by the bribe-mode factor (0.75x) then rounds
// to /100 (NovaUi_RunTravelDestinationPaymentWindow 0x00482280). Returns the
// BribeOutcome; on refusal the bribe cost is bumped by +1000 (the haggle).
[[nodiscard]] BribeOutcome
RunBribePayment(GameState &state, std::int32_t &bribe_cost, bool free_help) {
  if (free_help) {
    return BribeOutcome::kPaid; // local_10 bit0: granted without payment
  }
  const bool accepted =
      NovaRandomRange(state.rng, 100) <= kPaymentChancePercent;
  std::int32_t amount = bribe_cost;
  if (accepted) {
    amount = RoundDouble(static_cast<double>(amount) * kPaymentBribeScale);
    amount =
        RoundDouble(static_cast<double>(amount) * kPaymentRoundFactor) * 100;
  }
  if (state.player.credits < amount) {
    return BribeOutcome::kCannotAfford;
  }
  if (!accepted) {
    bribe_cost += kHaggleIncrement;
    return BribeOutcome::kRefused;
  }
  state.player.credits -= amount;
  NovaLog::Info("ship-comm: bribe of {} accepted", amount);
  return BribeOutcome::kPaid;
}

// The shared "how much for help?" prompt pick: personality < 0.8 -> good mood
// (0x17), in [0.8, 1.2] -> help-if-paid (0x1c), above -> terrible mood (0x18).
// Used by the player-fuel branch. The keep-pressing and distress branches use
// the 0x17/0x12/0x18 ladder ("You'll have to pay me first.") instead.
[[nodiscard]] std::optional<std::string>
LoadMoodPrompt(std::int16_t random_index, double personality) {
  if (personality < kPersonalityMoodLow) {
    return LoadCommPrompt(random_index, kMsgGoodMood);
  }
  if (personality <= kPersonalityMoodHigh) {
    return LoadCommPrompt(random_index, kMsgHelpIfPay);
  }
  return LoadCommPrompt(random_index, kMsgTerribleMood);
}

// The 0x17/0x12/0x18 ladder used by the keep-pressing and distress branches.
[[nodiscard]] std::optional<std::string>
LoadMoodPromptPayFirst(std::int16_t random_index, double personality) {
  if (personality < kPersonalityMoodLow) {
    return LoadCommPrompt(random_index, kMsgGoodMood);
  }
  if (personality <= kPersonalityMoodHigh) {
    return LoadCommPrompt(random_index, kMsgPayFirst);
  }
  return LoadCommPrompt(random_index, kMsgTerribleMood);
}

// The hail-info text (DAT_007d190c) the Greetings button shows for
// government-aid-eligible ships. The full assembly (NovaUi_BuildShipCommHail-
// InfoText 0x004819d0 branch 0: stellar scan + commodity names) is deferred;
// this mirrors the default STR# 0x7d2 0xaf fragment the original shows when
// the hail target carries no dude hail info (the common case).
[[nodiscard]] std::string BuildHailInfoText(const Ship &target) {
  (void)target;
  // TODO(decomp): dude-def hail_info_types / stellar-scan assembly of
  // NovaUi_BuildShipCommHailInfoText branch 0 is not reconstructed; the
  // original's default fragment is shown instead.
  if (auto s = NovaHud_LoadStringEntry(kMiscStr, 0xaf)) {
    return *s; // "is a good place to"
  }
  return {};
}

} // namespace

// ---------------------------------------------------------------------------
// The ship-comm modal
// ---------------------------------------------------------------------------
bool NovaShipComm_RunShipDialog(SdlPlatform &platform,
                                GameState &state,
                                std::int16_t ship_slot) {
  if (ship_slot <= 0 ||
      ship_slot >= static_cast<std::int16_t>(GameState::kMaxShips)) {
    NovaLog::Warn("ship-comm: slot {} out of range",
                  static_cast<int>(ship_slot));
    return false;
  }
  Ship &target = state.ShipAt(static_cast<std::size_t>(ship_slot));
  if (!target.is_active) {
    NovaLog::Warn("ship-comm: slot {} inactive", static_cast<int>(ship_slot));
    return false;
  }
  NovaLog::Info("opening ship-comm dialog 0x3ef for ship slot {}",
                static_cast<int>(ship_slot));

  // Behavior-6 escorts with no AI target and no mission fleet open the
  // escort-management window in the original (NovaUi_RunEscortShipManagement-
  // Window 0x004853a0); that window is not reconstructed, so the comm dialog
  // stands in (the assistance button still runs the escort-release flow).
  if (target.ai_behavior_code == 6 && target.ai_target_ship_slot == 0 &&
      target.mission_fleet_slot == -1) {
    NovaLog::Todo("ship-comm: escort-management window 0x004853a0 not "
                  "reconstructed; showing the comm dialog instead");
  }

  // ---- Opening gates (mirrors 0x0047e470) ---------------------------------
  const Government *govt =
      target.faction_or_government_id != -1
          ? state.scenario.Government(target.faction_or_government_id)
          : nullptr;
  const ShipClass *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(target.ship_class_id + 0x80));
  const Government *class_govt =
      ship_class != nullptr && ship_class->inherent_attributes_govt != -1
          ? state.scenario.Government(ship_class->inherent_attributes_govt)
          : nullptr;

  // bribe-offered latch (g_travel_interaction_bribe_offered): faction-less
  // ships always offer, peaceful behaviors (code < 3) with govt 0x2000 offer,
  // aggressive behaviors (code > 2) with govt 0x200 offer.
  bool bribe_offered = false;
  if (target.faction_or_government_id == -1) {
    bribe_offered = true;
  } else if (govt != nullptr) {
    if (target.ai_behavior_code < 3 &&
        (govt->flags_primary & kGovtFlagBribePeaceful) != 0U) {
      bribe_offered = true;
    }
    if (target.ai_behavior_code > 2 &&
        (govt->flags_primary & kGovtFlagBribeAggressive) != 0U) {
      bribe_offered = true;
    }
  }

  // Government scan-mask gates: bit 1 = "special" (hides the assistance
  // button / short-circuits comm), bit 8 = comm-special, bit 0x10 = free help
  // (local_10: the bribe is granted without the payment window). The class's
  // inherent government contributes its bit 8.
  const bool special_mask = govt != nullptr && (govt->scan_mask_short & 1) != 0;
  bool comm_special = govt != nullptr && (govt->scan_mask_short & 8) != 0;
  if (class_govt != nullptr && (class_govt->scan_mask_short & 8) != 0) {
    comm_special = true;
  }
  const bool keep_refusal = govt != nullptr && target.ai_behavior_code > 2 &&
                            (govt->flags_primary & kGovtFlagKeepRefusal) != 0U;
  const bool govt_aid_flag =
      govt != nullptr && (govt->flags_primary & kGovtFlagXenophobic) != 0U;
  const bool free_help = govt != nullptr && (govt->scan_mask_short & 0x10) != 0;

  // Per-launch random flavour index (g_travel_interaction_random_index).
  const std::int16_t random_index = NovaRandomRange(state.rng, 5);

  // Ship "personality" factor (DAT_007d17ec): (rand(0x29)+0x50)*0.01 with a
  // 1-in-5 -0.5 / further 1-in-5 +0.5 mood swing.
  float personality =
      static_cast<float>(NovaRandomRange(state.rng, 0x29) + 0x50) *
      kPersonalityStep;
  if (NovaRandomRange(state.rng, 5) == 0) {
    personality -= kPersonalityMoodSwing;
  } else if (NovaRandomRange(state.rng, 5) == 0) {
    personality += kPersonalityMoodSwing;
  }

  // Bribe cost (govt 0x8000 "bribes the player" branch uses the 1e-4 scale).
  const bool govt_bribes_player =
      govt != nullptr && (govt->flags_primary & kGovtFlagBribesPlayer) != 0U;
  std::int32_t bribe_cost =
      ComputeShipCommBribeCost(state.rng,
                               personality,
                               state.player.credits,
                               target.faction_or_government_id,
                               govt_bribes_player);

  // Mission-fleet escort latch (local_20): needs the random-encounter fleet
  // defs (not modelled) -- stays false (TODO(decomp)).
  const bool mission_escort = false;

  // ---- Initial prompt (the branch ladder before LAB_0047ec2e) --------------
  // Default: the hail-open message. Fire-restricted or special-mask ships keep
  // it; keep-pressing ships get the hostile "What is it you want?"; ships with
  // an AI target get the hail-open / hostile pick by government-aid
  // eligibility; behavior 5 -> sir-hail, behavior 6 -> friendly hail.
  std::string status;
  if (auto s = LoadCommPrompt(random_index, kMsgHailOpen)) {
    status = *s;
  } else {
    status = "Communications channel open.";
  }
  const bool fire_restricted = NovaAiShip_IsFireRestricted(state, target);
  if (!fire_restricted && !special_mask) {
    if (NovaAiShip_ShouldKeepPressingTarget(state, target)) {
      status = LoadCommPrompt(random_index, kMsgWhatDoYouWant).value_or(status);
    } else if (target.ai_target_ship_slot != 0) {
      status = NovaGovernment_IsShipEligibleForGovernmentAid(state, target)
                   ? LoadCommPrompt(random_index, kMsgHailOpen).value_or(status)
                   : LoadCommPrompt(random_index, kMsgWhatDoYouWant)
                         .value_or(status);
    } else if (target.ai_behavior_code == 5) {
      status = LoadCommPrompt(random_index, kMsgWhatCanIDoSir).value_or(status);
    } else if (target.ai_behavior_code == 6) {
      status = LoadCommPrompt(random_index, kMsgWhatCanIDo).value_or(status);
    }
  }

  // ---- Assets --------------------------------------------------------------
  auto backdrop = LoadPictTexture(platform, kCommFramePict);
  if (!backdrop) {
    NovaLog::Todo("ship-comm: frame PICT 0x213f unavailable; drawing a "
                  "bordered placeholder");
  }
  // Ship portrait PICT: ShipClass.pict_fallback_sprite_resource_id (5000 +
  // class id, populated by the scenario loader at startup) decodes to a
  // 200x200 portrait drawn into DITL item 10 on the frame's right.
  std::unique_ptr<SdlTexture> ship_picture;
  if (ship_class != nullptr &&
      ship_class->pict_fallback_sprite_resource_id != 0) {
    ship_picture =
        LoadPictTexture(platform, ship_class->pict_fallback_sprite_resource_id);
  }
  ServicesButtonArt button_art;
  if (!button_art.Initialize(platform)) {
    NovaLog::Warn("three-state button art unavailable for the comm dialog "
                  "buttons");
  }
  NovaFontCache font_cache;
  const SDL_FRect panel{0.0F, 0.0F, 640.0F, 480.0F};
  platform.SetCenteredPlayfield();

  // Ship name panel: "<ship class name>" + government comm name. The original
  // reads the government comm-name table (DAT_007d1d0c) or a "?" placeholder
  // for faction-less ships; the clean-room Government carries comm_name.
  const std::string ship_name =
      ship_class != nullptr ? ship_class->display_name : target.ship_name;
  const std::string govt_line = [&]() {
    if (govt != nullptr) {
      return govt->comm_name;
    }
    return std::string("?");
  }();
  const std::string status_line = [&]() {
    if (target.escort_origin_mark == 0) {
      // "Fighter" (0xa8) / "Captured Escort" (0xa6) label under the name.
      return NovaHud_LoadStringEntry(kMiscStr, kMiscFighterLabel)
          .value_or(govt_line);
    }
    return NovaHud_LoadStringEntry(kMiscStr, kMiscCapturedEscortLabel)
        .value_or(govt_line);
  }();

  // ---- Buttons -------------------------------------------------------------
  // Slots: 0 Close Channel (bottom), 1 the assistance button (middle; label
  // Request Assistance | Beg For Mercy | Release, hidden for special-mask
  // ships), 2 the Greetings button (top; shows the hail-info text).
  enum ButtonSlot : std::uint8_t {
    kCloseChannel = 0,
    kAssistance = 1,
    kGreetings = 2
  };

  // The three buttons live at DITL items 0/1/2: a 166x26 stack on the frame's
  // lower-left, slot 0 (Close Channel) at the bottom y=181, slot 1 (the
  // assistance button) in the middle y=153, slot 2 (Greetings) at the top
  // y=125. For special-scan-mask ships the original hides the assistance
  // button and drops the Greetings button down to the middle rect (y=153)
  // -- NovaUi_HandleTravelDestinationContextButtons reads entry 2 (item 1,
  // y=153) for the Greetings slot instead of entry 3 (item 2, y=125), and
  // the slot-1 draw rect is the offscreen item 3 (y=241..266). To keep the
  // hit test unambiguous the assistance slot is omitted from the button list
  // for special-mask ships rather than kept-but-hidden.
  const bool keep_assistance = !special_mask;
  const float greetings_y =
      special_mask ? kCommButtonYMiddle : kCommButtonYTop;
  const SDL_FRect btn_rects[3] = {
      {kCommWindowX + kCommButtonX, kCommWindowY + kCommButtonYClose,
       kCommButtonW, kCommButtonH},
      {kCommWindowX + kCommButtonX, kCommWindowY + kCommButtonYMiddle,
       kCommButtonW, kCommButtonH},
      {kCommWindowX + kCommButtonX, kCommWindowY + greetings_y, kCommButtonW,
       kCommButtonH},
  };
  std::vector<ServiceButton> buttons;
  buttons.reserve(3);
  for (std::uint8_t i = 0; i < 3; ++i) {
    if (!keep_assistance && i == kAssistance) {
      continue; // Assistance (slot 1) is suppressed for special-mask ships.
    }
    buttons.push_back(ServiceButton{btn_rects[i], i});
  }
  std::string assistance_label;
  if (NovaAiShip_ShouldKeepPressingTarget(state, target)) {
    assistance_label = LoadButtonLabel(kBtnBegForMercy);
  } else if (target.ai_target_ship_slot == 0 && target.ai_behavior_code == 6 &&
             !mission_escort) {
    assistance_label = LoadButtonLabel(kBtnRelease);
  } else {
    assistance_label = LoadButtonLabel(kBtnRequestAssistance);
  }
  // Label per slot, matching the original's DAT_007d82ee/f0/f2 table: slot
  // 0 = Close Channel (0x14), slot 1 = Request Assistance | Beg For Mercy |
  // Release (0x16/0x18/0x1f), slot 2 = Greetings (0x15). Each slot's label
  // agrees with its action (slot 1 runs the assistance dialogue, slot 2
  // shows the hail-info text). NOTE: the plate comments on NovaUi_PollTarget-
  // ShipCommWindow call action 2 "Greetings" and action 3 "secondary", which
  // is swapped relative to the buttons and is the historical source of the
  // label confusion here.
  const std::array<std::string, 3> button_labels{
      LoadButtonLabel(kBtnCloseChannel),
      assistance_label,
      LoadButtonLabel(kBtnGreetings)};

  // The escort-release latch (DAT_007d17f5): armed by the assistance button
  // ("Release") on a behavior-6 escort without an AI target; on close it
  // transfers cargo/junk
  // and releases the escort back to its default behavior.
  bool escort_transfer_armed = false;

  // The assistance dialogue (runner for slot 1, the Request Assistance | Beg
  // For Mercy | Release button). Mirrors the decomp's local_22 == 2 block;
  // factored out so both the button click and the 'r' keyboard shortcut
  // (NovaUi_PollTargetShipCommWindow 0x0047fa40) share it.
  const auto RunAssistanceDialogue = [&]() {
    // ---- Assistance dialogue (local_22 == 2) -----------------------------
    if (fire_restricted) {
      // "No response." (prompt 1).
      status = LoadCommPrompt(random_index, kMsgNoResponse).value_or(status);
      return;
    }
    if (target.ai_target_ship_slot == 0 && target.ai_behavior_code == 6 &&
        !mission_escort) {
      // Escort release: latch the cargo transfer and show the goodbye message
      // (STR# 0xbb9). The window stays open until the player closes the
      // channel; the transfer/re-hire runs on close.
      escort_transfer_armed = true;
      status =
          LoadCommPrompt(random_index, kMsgEscortGoodbye).value_or(status);
      return;
    }
    if (comm_special) {
      // scan_mask & 8 ships: "No response." and no further comm.
      status = LoadCommPrompt(random_index, kMsgNoResponse).value_or(status);
      return;
    }
    if (NovaAiShip_ShouldKeepPressingTarget(state, target)) {
      // Keep-pressing ship: the mission-fleet bribe/hostility branch (prompts
      // 0x13 refusal, 0x17/0x12/0x18 mood ladder, outcomes 0x14 re-hired / 0x1e
      // hostile / 0xc can't-afford).
      if (target.target_stellar_object_id == -1 && bribe_offered) {
        status =
            LoadMoodPromptPayFirst(random_index, personality).value_or(status);
        const BribeOutcome outcome =
            RunBribePayment(state, bribe_cost, free_help);
        if (outcome == BribeOutcome::kPaid) {
          status = LoadCommPrompt(random_index, kMsgBusiness).value_or(status);
          NovaAi_EnterState2ClearPrimaryTarget(
              target, static_cast<std::uint32_t>(SDL_GetTicks()));
          target.ai_behavior_code = 1;
        } else if (outcome == BribeOutcome::kRefused) {
          status =
              LoadCommPrompt(random_index, kMsgPrepareToDie).value_or(status);
          NovaAi_SetShipHostileToPlayer(state, target);
        } else {
          status = LoadCommPrompt(random_index, kMsgCantAfford).value_or(status);
        }
      } else {
        status = LoadCommPrompt(random_index, kMsgDreams).value_or(status);
      }
      return;
    }
    if (!NovaGovernment_IsShipEligibleForGovernmentAid(state, target) ||
        govt_aid_flag || keep_refusal) {
      // No aid on offer: "In your dreams, pal."
      status = LoadCommPrompt(random_index, kMsgDreams).value_or(status);
      return;
    }
    if (NovaAiShip_IsShipEligibleForCommAidInteraction(state, target) ||
        NovaAiShip_IsShipInNonIdleAiState(target)) {
      // The ship is already busy with something: "I'm busy." / "Sorry sir, I
      // can't do that." (escorts), or "Okay, I'm on my way." when it is
      // braking onto the player in state 0x09/0x0F.
      if (!NovaAiShip_IsShipBrakingOnPlayerState9(state, target) &&
          !NovaAiShip_IsShipBrakingOnPlayerState0xF(state, target)) {
        status = target.ai_behavior_code < 5
                     ? LoadCommPrompt(random_index, kMsgImBusy).value_or(status)
                     : LoadCommPrompt(random_index, kMsgCantDoSir)
                           .value_or(status);
      } else {
        status = LoadCommPrompt(random_index, kMsgOnMyWay).value_or(status);
      }
      return;
    }
    if (!NovaAi_AreAnyShipsEligibleForDistressCall(state)) {
      // No distress call in progress: the fuel-offer branch triggers when the
      // player's tank is low (or the player is fire-restricted). A govt-aid
      // ship (xenophobic flag) refuses with "In your dreams, pal." instead;
      // otherwise "You're not in any trouble."
      const bool player_fuel_low =
          state.player.fuel_points < kPlayerFuelOfferThreshold &&
          state.cached_stats.fuel_capacity > 0;
      if (player_fuel_low ||
          NovaAiShip_IsFireRestricted(state, state.player)) {
        if (govt_aid_flag) {
          status = LoadCommPrompt(random_index, kMsgDreams).value_or(status);
        } else if (target.ai_behavior_code < 5) {
          status = LoadMoodPrompt(random_index, personality).value_or(status);
          const BribeOutcome outcome =
              RunBribePayment(state, bribe_cost, free_help);
          if (outcome == BribeOutcome::kPaid) {
            status =
                LoadCommPrompt(random_index, kMsgOnMyWay).value_or(status);
            if (NovaAiShip_IsFireRestricted(state, state.player)) {
              NovaAi_EnterState0FTargetPlayerAndBrake(target);
            } else {
              NovaAi_EnterState9TargetPlayerAndBrake(target);
            }
          } else if (outcome == BribeOutcome::kRefused) {
            status = LoadCommPrompt(random_index, kMsgComedian).value_or(status);
          } else {
            status =
                LoadCommPrompt(random_index, kMsgCantAfford).value_or(status);
          }
        } else {
          status = LoadCommPrompt(random_index, kMsgCantDo).value_or(status);
        }
      } else {
        status = LoadCommPrompt(random_index, kMsgNoTrouble).value_or(status);
      }
      return;
    }
    // A distress call is possible: route on the target's responders.
    if (!NovaAiShip_HasShipDistressResponder(state, target)) {
      status = LoadCommPrompt(random_index, kMsgDreams).value_or(status);
      return;
    }
    // TODO(decomp): the local_1c fleet-def bit (allied 0x400 fleet defs) gates
    // the "on my way" + Government_TryTriggerGovtAssistanceEncounter arm; fleet
    // defs are not modelled, so the behavior ladder below runs instead.
    if (!govt_aid_flag) {
      const std::int16_t behavior = target.ai_behavior_code;
      if (behavior == 1) {
        status = LoadCommPrompt(random_index, kMsgRatherNot).value_or(status);
      } else if (behavior == 2 && target.ship_instance_id % 3 == 0 &&
                 target.faction_or_government_id == -1) {
        status =
            LoadMoodPromptPayFirst(random_index, personality).value_or(status);
        const BribeOutcome outcome =
            RunBribePayment(state, bribe_cost, free_help);
        if (outcome == BribeOutcome::kPaid) {
          status = LoadCommPrompt(random_index, kMsgOnMyWay).value_or(status);
          NovaAi_EnterState4TargetRandomUnengagedShip(state, target);
        } else if (outcome == BribeOutcome::kRefused) {
          status = LoadCommPrompt(random_index, kMsgComedian).value_or(status);
        } else {
          status = LoadCommPrompt(random_index, kMsgCantAfford).value_or(status);
        }
      } else if (behavior == 3 || behavior == 4) {
        status =
            LoadMoodPromptPayFirst(random_index, personality).value_or(status);
        const BribeOutcome outcome =
            RunBribePayment(state, bribe_cost, free_help);
        if (outcome == BribeOutcome::kPaid) {
          status = LoadCommPrompt(random_index, kMsgOnMyWay).value_or(status);
          NovaAi_EnterState4TargetRandomUnengagedShip(state, target);
        } else if (outcome == BribeOutcome::kRefused) {
          status = LoadCommPrompt(random_index, kMsgComedian).value_or(status);
        } else {
          status = LoadCommPrompt(random_index, kMsgCantAfford).value_or(status);
        }
      } else {
        status = LoadCommPrompt(random_index, kMsgOnMyWay).value_or(status);
        NovaAi_EnterState4TargetRandomCombatCandidate(state, target);
      }
    } else {
      status = LoadCommPrompt(random_index, kMsgDreams).value_or(status);
    }
  };

  // The Greetings action (runner for slot 2, the Greetings button): shows the
  // hail-info text (DAT_007d190c) or a refusal prompt. Mirrors the decomp's
  // local_22 == 3 block; shared by the button click and the 'g' keyboard
  // shortcut.
  const auto RunGreetings = [&]() {
    if (!fire_restricted && !special_mask) {
      if (!NovaAiShip_ShouldKeepPressingTarget(state, target) &&
          NovaGovernment_IsShipEligibleForGovernmentAid(state, target)) {
        const std::string info = BuildHailInfoText(target);
        if (!info.empty()) {
          status = info;
        }
      } else {
        status =
            LoadCommPrompt(random_index, kMsgStopWastingTime).value_or(status);
      }
    } else {
      status = LoadCommPrompt(random_index, kMsgNoResponse).value_or(status);
    }
  };

  // ---- Modal loop ----------------------------------------------------------
  while (!platform.quit_requested()) {
    DrawShipCommDialog(platform,
                       font_cache,
                       button_art,
                       backdrop ? backdrop->get() : nullptr,
                       ship_picture ? ship_picture->get() : nullptr,
                       ship_name,
                       status_line,
                       status,
                       buttons,
                       button_labels,
                       panel);
    SDL_RenderPresent(platform.renderer());

    bool close_channel = false;
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      switch (in->key) {
      case TextKey::escape:
      case TextKey::enter:
        close_channel = true;
        break;
      case TextKey::primary: {
        const auto clicked =
            ServiceButtonAt(buttons, platform.mouse_position());
        if (!clicked) {
          break;
        }
        switch (static_cast<ButtonSlot>(*clicked)) {
        case kCloseChannel:
          close_channel = true;
          break;
        case kAssistance:
          // Assistance dialogue (local_22 == 2). Only reachable for
          // non-special-mask ships (the slot is omitted from `buttons` for
          // special-mask ships).
          RunAssistanceDialogue();
          break;
        case kGreetings:
          // Greetings action (local_22 == 3): hail-info text.
          RunGreetings();
          break;
        }
        break;
      }
      case TextKey::character: {
        // Original keyboard shortcuts (NovaUi_PollTargetShipCommWindow
        // 0x0047fa40): 'e'/Enter/Esc close the channel, 'r' triggers the
        // assistance button (Request Assistance | Beg For Mercy | Release),
        // 'g' triggers the Greetings button. The assistance shortcut is
        // suppressed for special-scan-mask ships (the button is hidden there
        // too).
        switch (in->character) {
        case 'e':
        case 'E':
          close_channel = true;
          break;
        case 'r':
        case 'R':
          if (!special_mask) {
            RunAssistanceDialogue();
          }
          break;
        case 'g':
        case 'G':
          RunGreetings();
          break;
        default:
          break;
        }
        break;
      }
      default:
        break;
      }
    }

    if (close_channel) {
      break;
    }
    SDL_Delay(16);
  }

  // ---- Close-time side effects ---------------------------------------------
  if (escort_transfer_armed) {
    // The original transfers cargo/junk to the escort by ratio and resets its
    // AI to re-hire it (Outfit_TransferCargoAndJunkToEscortByRatio 0x00469810
    // + field_0xb9 + post_hit_mode_hint + Ship_ResetShipAiBehaviorRuntimeFields
    // 0x00402810). The cargo transfer itself is deferred (TODO(decomp): the
    // cargo/junk inventory split is not modelled); the escort re-hire fields
    // are applied.
    NovaLog::Todo("ship-comm: escort cargo transfer "
                  "(Outfit_TransferCargoAndJunkToEscortByRatio 0x00469810) not "
                  "reconstructed");
    target.ai_target_ship_slot = -1;
    target.ai_behavior_code =
        ship_class != nullptr ? ship_class->default_ai_behavior : 1;
    target.escort_rehired_mark = 1;
    target.post_hit_mode_hint = -1;
    // Ship_ResetShipAiBehaviorRuntimeFields (0x00402810) subset:
    target.ai_state_code = 0;
    target.ai_control_mode = 0;
    target.ai_secondary_target_slot = -1;
    NovaAi_EnterState2ClearPrimaryTarget(
        target, static_cast<std::uint32_t>(SDL_GetTicks()));
  }
  target.comm_interacted_mark = 1;
  return true;
}

// ---------------------------------------------------------------------------
// Hail-eligibility gate (Ship_HandlePlayerTargetActionCommand bVar1)
// ---------------------------------------------------------------------------
bool NovaShipComm_TargetEligibleForHail(const GameState &state,
                                        const Ship &target) {
  bool eligible = true;
  if (target.ai_target_ship_slot != 0 || target.mission_fleet_slot != -1) {
    // Busy ships whose government (or whose class's inherent government)
    // carries the 0x400 busy flag cannot be hailed.
    if (target.faction_or_government_id != -1) {
      const Government *g =
          state.scenario.Government(target.faction_or_government_id);
      if (g != nullptr && (g->flags_primary & 0x400U) != 0) {
        eligible = false;
      }
    }
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(target.ship_class_id + 0x80));
    if (cls != nullptr && cls->inherent_attributes_govt != -1) {
      const Government *g =
          state.scenario.Government(cls->inherent_attributes_govt);
      if (g != nullptr && (g->flags_primary & 0x400U) != 0) {
        eligible = false;
      }
    }
  }
  if (target.ship_class_id == 0x2ff) {
    eligible = false; // special non-comm ship class
  }
  if (NovaAiShip_IsFireRestricted(state, target)) {
    eligible = false;
  }
  if (target.mission_ship_slot == 0x3ff) {
    eligible = false;
  }
  return eligible;
}

} // namespace game
