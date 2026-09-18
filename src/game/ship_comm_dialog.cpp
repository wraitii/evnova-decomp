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
#include "../util/format.hpp"
#include "../util/math.hpp"
#include "button_label.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "hud_renderer.hpp"
#include "landed_store.hpp"
#include "landed_window.hpp"
#include "mission.hpp"
#include "nova_font.hpp"
#include "nova_random.hpp"
#include "pict_texture.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "ship_ai.hpp"
#include "spaceflight_view.hpp"
#include "targeting.hpp"

#include <SDL3/SDL.h>

namespace game {

using evnova::util::GroupThousands;
using evnova::util::TruncateToInt32;

namespace {

// ---- Resource ids ----------------------------------------------------------
// The ship-comm window's backdrop PICT (DLOG 0x3ef, the Communications frame).
constexpr std::uint16_t kCommFramePict = 0x213f;
constexpr std::uint16_t kEscortManagementFramePict = 0x2141;

// ---- DLOG 0x3ef / DITL 0x3ef geometry -------------------------------------
// The comm window is 423x215 (DLOG 0x3ef bounds = the backdrop PICT 0x213f,
// decodes to exactly 423x215), centred on the 640x480 playfield at
// window-relative (0,0) = top-left of the backdrop. The three context buttons
// are DITL items 0/1/2 stacked *vertically* on the lower-left (166x26 each at
// x=21..187, y=125/153/181), NOT a bottom row; the ship portrait is DITL item
// 10, a 200x200 box on the right (x=216..416, y=7..207); the prompt/status
// text panel is item 9 (x=11..203, y=8..66) and the class/comm-name/status
// block is item 11 (x=40..174, y=73..119). The remaining DITL items (3..8,
// below y=207) sit past the 215-tall backdrop and are clipped/redundant in
// the shipped window.
constexpr int kCommFrameWidth = 423;
constexpr int kCommFrameHeight = 215;
// Window origin on the 640x480 playfield: the frame is centred (truncated
// half-offsets, Dialog_CreateFromDlog) in LoadCommFrameLayout below.
// Portrait (DITL item 10): 200x200 on the right side.
constexpr SDL_FRect kCommPictureRect{216.0F, 7.0F, 200.0F, 200.0F};
// Prompt/status text panel (DITL item 9) / class+status block (DITL item 11).
constexpr SDL_FRect kCommPromptPanelRect{11.0F, 8.0F, 192.0F, 58.0F};
constexpr SDL_FRect kCommInfoPanelRect{40.0F, 73.0F, 134.0F, 46.0F};
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

// One frame's worth of comm-window geometry (DLOG/DITL 0x3ef), mapped into
// the centred 640x480 playfield canvas. Loaded from the real DITL at runtime
// (going-forward policy, see docs/dlog_ditl_dialog_format.md); the constants
// above are the fallback used when the resources fail to decode.
struct CommFrameLayout {
  SDL_FRect window{};
  SDL_FRect buttons[3]{};   // DITL items 0/1/2: Close Channel / middle / top
  SDL_FRect prompt_panel{}; // item 9 (word-wrapped comm prompt text)
  SDL_FRect picture{};      // item 10 (200x200 portrait)
  SDL_FRect info_panel{};   // item 11 (Class:/comm-name/Status: block)
};

struct EscortManagementLayout {
  SDL_FRect window{};
  std::array<SDL_FRect, 4> buttons{};
  SDL_FRect details{};
  SDL_FRect picture{};
  SDL_FRect status{};
};

[[nodiscard]] EscortManagementLayout LoadEscortManagementLayout() {
  EscortManagementLayout layout;
  layout.window = {108.0F, 110.0F, 424.0F, 259.0F};
  const auto local = [&](float x, float y, float w, float h) {
    return SDL_FRect{layout.window.x + x, layout.window.y + y, w, h};
  };
  layout.buttons = {local(29, 225, 146, 26),
                    local(29, 197, 146, 26),
                    local(29, 141, 146, 26),
                    local(29, 169, 146, 26)};
  layout.status = local(14, 9, 192, 58);
  layout.picture = local(217, 30, 200, 200);
  layout.details = local(14, 79, 192, 52);

  const auto definition = NovaResource_LoadDialogDefinition(0x3fe);
  const auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (!definition || !items) {
    NovaLog::Todo("DLOG/DITL 0x3fe unavailable; using shipped "
                  "escort-management geometry");
    return layout;
  }
  const float width = static_cast<float>(definition->right - definition->left);
  const float height = static_cast<float>(definition->bottom - definition->top);
  layout.window = {std::truncf((640.0F - width) * 0.5F),
                   std::truncf((480.0F - height) * 0.5F),
                   width,
                   height};
  for (const auto &item : *items) {
    const SDL_FRect rect{layout.window.x + static_cast<float>(item.left),
                         layout.window.y + static_cast<float>(item.top),
                         static_cast<float>(item.right - item.left),
                         static_cast<float>(item.bottom - item.top)};
    if (item.index < 4) {
      layout.buttons[item.index] = rect;
    } else if (item.index == 9) {
      layout.status = rect;
    } else if (item.index == 10) {
      layout.picture = rect;
    } else if (item.index == 11) {
      layout.details = rect;
    }
  }
  return layout;
}

[[nodiscard]] CommFrameLayout LoadCommFrameLayout() {
  CommFrameLayout layout;
  layout.window = {std::truncf((640.0F - kCommFrameWidth) * 0.5F),
                   std::truncf((480.0F - kCommFrameHeight) * 0.5F),
                   static_cast<float>(kCommFrameWidth),
                   static_cast<float>(kCommFrameHeight)};
  const auto local = [&](float x, float y, float w, float h) {
    return SDL_FRect{layout.window.x + x, layout.window.y + y, w, h};
  };
  layout.buttons[0] =
      local(kCommButtonX, kCommButtonYClose, kCommButtonW, kCommButtonH);
  layout.buttons[1] =
      local(kCommButtonX, kCommButtonYMiddle, kCommButtonW, kCommButtonH);
  layout.buttons[2] =
      local(kCommButtonX, kCommButtonYTop, kCommButtonW, kCommButtonH);
  layout.prompt_panel = local(kCommPromptPanelRect.x,
                              kCommPromptPanelRect.y,
                              kCommPromptPanelRect.w,
                              kCommPromptPanelRect.h);
  layout.picture = local(kCommPictureRect.x,
                         kCommPictureRect.y,
                         kCommPictureRect.w,
                         kCommPictureRect.h);
  layout.info_panel = local(kCommInfoPanelRect.x,
                            kCommInfoPanelRect.y,
                            kCommInfoPanelRect.w,
                            kCommInfoPanelRect.h);

  const auto definition = NovaResource_LoadDialogDefinition(0x3ef);
  const auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (!definition || !items) {
    NovaLog::Todo("DLOG/DITL 0x3ef unavailable; using the shipped-geometry "
                  "fallback for the ship-comm window");
    return layout;
  }
  const float width = static_cast<float>(definition->right - definition->left);
  const float height = static_cast<float>(definition->bottom - definition->top);
  layout.window = {std::truncf((640.0F - width) * 0.5F),
                   std::truncf((480.0F - height) * 0.5F),
                   width,
                   height};
  for (const auto &item : *items) {
    const SDL_FRect rect{layout.window.x + static_cast<float>(item.left),
                         layout.window.y + static_cast<float>(item.top),
                         static_cast<float>(item.right - item.left),
                         static_cast<float>(item.bottom - item.top)};
    switch (item.index) {
    case 0:
    case 1:
    case 2:
      layout.buttons[item.index] = rect;
      break;
    case 9:
      layout.prompt_panel = rect;
      break;
    case 10:
      layout.picture = rect;
      break;
    case 11:
      layout.info_panel = rect;
      break;
    default:
      break;
    }
  }
  return layout;
}

// Prompt pools: STR# 0xbb8 "Ship Comm Strings" for prompt_index < 0x26, STR#
// 0xbb9 "More Ship Comm" for prompt_index >= 0x26 (the escort-goodbye pool),
// each at `index*5 + random + 1` / `(index*5 + random) - 0xbd` respectively
// (NovaUi_LoadTravelDestinationPromptString 0x004828c0).
constexpr std::uint16_t kPromptStr = 0xbb8;
constexpr std::uint16_t kPromptStrHigh = 0xbb9;

// STR# 0x7d2 "misc strings": entries drawn into the item-11 info block
// (1-based; NovaUi_DrawTargetShipCommWindow 0x0047fb70): 0xc3
// "Class:", 0xc4 "Status:", 0xa8 "Escort" / 0xa6 "Hired Escort" (picked by
// ShipState +0xbb, the port's escort_origin_mark), 0xae "Hostile".
constexpr std::uint16_t kMiscStr = 0x7d2;
constexpr std::uint16_t kMiscClassLabel = 0xc3;       // "Class:"
constexpr std::uint16_t kMiscStatusLabel = 0xc4;      // "Status:"
constexpr std::uint16_t kMiscEscortLabel = 0xa8;      // "Escort"
constexpr std::uint16_t kMiscHiredEscortLabel = 0xa6; // "Hired Escort"
constexpr std::uint16_t kMiscHostileLabel = 0xae;     // "Hostile"

// STR# 0x96 "button labels" (the three-state button label table DAT_007d82ee/
// f0/f2 indexes in the original), as 1-based entry numbers: 0x15 Close
// Channel, 0x16 Greetings, 0x17 Request Assistance, 0x19 Beg For Mercy,
// 0x20 Release.
constexpr std::uint16_t kBtnCloseChannel = 0x15;
constexpr std::uint16_t kBtnGreetings = 0x16;
constexpr std::uint16_t kBtnRequestAssistance = 0x17;
constexpr std::uint16_t kBtnBegForMercy = 0x19;
constexpr std::uint16_t kBtnRelease = 0x20;

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
// The item-11 info block fills with the space-black colour and labels draw
// in the 50% grey (SHORT_ARRAY_00733b56); the Escort/Hired Escort status
// word uses the same warm runtime tone as the destination window's
// Owned/Dominated words (SHORT_ARRAY_00733b32, unreadable .bss --
// TODO(decomp): verify). "Hostile" draws red with the weight flag set
// (FUN_004b6940(1)).
constexpr SDL_Color kDim{192, 192, 192, 255};
constexpr SDL_Color kTitle{255, 255, 255, 255};
constexpr SDL_Color kBlack{0, 0, 0, 255};
constexpr SDL_Color kPanelGrey{128, 128, 128, 255};
constexpr SDL_Color kStatusOrange{255, 102, 0, 255};
constexpr SDL_Color kHostileRed{255, 0, 0, 255};

// Loads one flavour variant of a ship-comm prompt (STR# 0xbb8 for
// prompt_index < 0x26, else STR# 0xbb9 at (index*5+random)-0xbd), mirroring
// NovaUi_LoadTravelDestinationPromptString (0x004828c0).
[[nodiscard]] std::optional<std::string>
LoadCommPrompt(std::int16_t random_index, std::uint16_t prompt_index) {
  if (prompt_index < 0x26) {
    return NovaHud_LoadStringEntry(
        kPromptStr,
        static_cast<std::uint16_t>(prompt_index * 5u + random_index + 1));
  } else {
    const std::uint16_t entry =
        static_cast<std::uint16_t>(prompt_index * 5u + random_index - 0xbd);
    return NovaHud_LoadStringEntry(kPromptStrHigh, entry);
  }
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
  // NovaRandom_Range is [0, bound); the decompile multiplies the raw pick:
  // `(rand(upper) * 1000 + base) * personality` -- no +1.
  const std::int64_t pick = RandomBelow(rng, static_cast<int>(upper));
  double cost = static_cast<double>((pick * kBribeGrouping + base)) *
                static_cast<double>(personality);
  std::int64_t cost_int = TruncateToInt32(cost);

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

// Draws the comm-window frame over the live flight view: the PICT 0x213f
// backdrop
// centred at natural size (falling back to a bordered placeholder), the ship
// picture box (item 10), the class/comm-name/status block (item 11), the
// word-wrapped prompt panel (item 9), and the three context buttons. Panel
// text is the shared 12-point screen font (DAT_00735684/DAT_00735686 =
// Geneva 12 regular), left-aligned.
void DrawShipCommDialog(SdlPlatform &platform,
                        const GameState &state,
                        SpaceflightView &view,
                        HudRenderer &hud,
                        NovaFontCache &font_cache,
                        const ServicesButtonArt &button_art,
                        SDL_Texture *backdrop,
                        SDL_Texture *ship_picture,
                        const Ship &target,
                        bool escort_latched,
                        std::string_view class_name,
                        bool pers_placeholder,
                        std::string_view comm_name,
                        std::string_view prompt_text,
                        std::span<const ServiceButton> buttons,
                        std::span<const std::string> button_labels,
                        const CommFrameLayout &layout,
                        const SDL_FRect &panel) {
  SDL_Renderer *renderer = platform.renderer();
  // Render the live game view beneath the window (the flight sim is paused,
  // so this redraws the same world each frame): the original draws its DLOG
  // over the unmodified gameplay surface.
  view.DrawGameFrame(platform, state, hud);
  platform.SetCenteredPlayfield();
  (void)panel;

  // The comm window frame is the DITL 0x3ef window (the 423x215 PICT 0x213f
  // backdrop), centred on the 640x480 playfield.
  const SDL_FRect &frame = layout.window;
  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &frame);
  } else {
    // No backdrop art: draw a bordered placeholder so the dialog stays legible.
    SDL_SetRenderDrawColor(renderer, 16, 40, 72, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &frame);
    SDL_SetRenderDrawColor(renderer, 80, 140, 190, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &frame);
  }

  // Ship portrait (DITL item 10): the class's 200x200 portrait PICT
  // (5000 + class id) in the item's box on the frame's right.
  const SDL_FRect &picture = layout.picture;
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

  // Class / comm-name / status block (DITL item 11, entry 12). The original
  // fills the rect black and draws left-aligned Geneva-12 lines (0x0047fb70):
  // the grey "Class:" label (STR# 0x7d2 0xc3) with the white class name at
  // +0x23 (the pers 0x3ff placeholder "Ambrosia Mascot", DAT_0056cc30,
  // replaces it), the white "(<comm name>)" line at +0x19 for faction ships
  // (the DAT_0056cc40/44 parens around the government comm-name table entry),
  // and the grey "Status:" line (0xc4) at +0x28. The status word is the
  // orange Escort (0xa8) / Hired Escort (0xa6, +0xbb set) -- suppressed once
  // the ship has an AI target or the escort-release latch (DAT_007d17f5) is
  // armed -- or a red "Hostile" while the ship is keep-pressing.
  const SDL_FRect &info = layout.info_panel;
  SDL_SetRenderDrawColor(
      renderer, kBlack.r, kBlack.g, kBlack.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &info);
  const std::string class_label =
      NovaHud_LoadStringEntry(kMiscStr, kMiscClassLabel).value_or("Class:");
  const std::string status_label =
      NovaHud_LoadStringEntry(kMiscStr, kMiscStatusLabel).value_or("Status:");
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                kPanelGrey,
                info.x,
                info.y + 12.0F,
                class_label);
  const std::string class_value = pers_placeholder
                                      ? std::string{"Ambrosia Mascot"}
                                      : std::string{class_name};
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                kTitle,
                info.x + 35.0F,
                info.y + 12.0F,
                class_value);
  if (!pers_placeholder && !comm_name.empty()) {
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kTitle,
                  info.x + 35.0F,
                  info.y + 25.0F,
                  "(" + std::string{comm_name} + ")");
  }
  if (NovaAiShip_ShouldKeepPressingTarget(state, target)) {
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kPanelGrey,
                  info.x,
                  info.y + 40.0F,
                  status_label);
    // The original appends the DAT_0056cc48 two-space pstring before the
    // bold-flipped "Hostile".
    const float hostile_x =
        info.x +
        static_cast<float>(font_cache.TextWidth(NovaFontFamily::kGeneva,
                                                12.0F,
                                                kNovaFontStyleRegular,
                                                status_label + "  "));
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleBold,
                  kHostileRed,
                  hostile_x,
                  info.y + 40.0F,
                  NovaHud_LoadStringEntry(kMiscStr, kMiscHostileLabel)
                      .value_or("Hostile"));
  } else if (target.squad_leader_ship_slot == 0 && !escort_latched) {
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kPanelGrey,
                  info.x,
                  info.y + 40.0F,
                  status_label);
    const std::string status_word =
        NovaHud_LoadStringEntry(kMiscStr,
                                target.escort_origin_mark != 0
                                    ? kMiscHiredEscortLabel
                                    : kMiscEscortLabel)
            .value_or(target.escort_origin_mark != 0 ? "Hired Escort"
                                                     : "Escort");
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kStatusOrange,
                  info.x + 35.0F,
                  info.y + 40.0F,
                  status_word);
  }

  // Prompt / status message panel (DITL item 9, entry 10). The original
  // fills the inset rect white, draws the prompt word-wrapped in black into
  // a white-filled inner rect and InvertRects the panel (0x0047fb70 via
  // DrawContext_DrawPascalStringInFilledRect 0x004bcd30, DT_WORDBREAK); the
  // colours cancel under the inversion, so the net look is a black panel
  // with white wrapped text, left-aligned from the inner top.
  const SDL_FRect &prompt_panel = layout.prompt_panel;
  SDL_FRect prompt_inner = prompt_panel;
  prompt_inner.x += 1.0F;
  prompt_inner.y += 1.0F;
  prompt_inner.w -= 2.0F;
  prompt_inner.h -= 2.0F;
  SDL_SetRenderDrawColor(
      renderer, kBlack.r, kBlack.g, kBlack.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &prompt_inner);
  if (!prompt_text.empty()) {
    prompt_inner.x += 4.0F;
    prompt_inner.y += 2.0F;
    prompt_inner.w -= 8.0F;
    prompt_inner.h -= 4.0F;
    const auto lines = WrapDescriptionLines(
        prompt_text,
        static_cast<int>(std::max(1.0F, prompt_inner.w)),
        [&](std::string_view line) {
          return static_cast<int>(font_cache.TextWidth(
              NovaFontFamily::kGeneva, 12.0F, kNovaFontStyleRegular, line));
        });
    float baseline = prompt_inner.y + 12.0F;
    for (const std::string &line : lines) {
      if (baseline > prompt_panel.y + prompt_panel.h) {
        break;
      }
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    12.0F,
                    kNovaFontStyleRegular,
                    kTitle,
                    prompt_inner.x,
                    baseline,
                    line);
      baseline += 13.0F;
    }
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
                          b.slot < button_labels.size() ? button_labels[b.slot]
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
  const bool accepted = RandomBelow(state.rng, 100) <= kPaymentChancePercent;
  std::int32_t amount = bribe_cost;
  if (accepted) {
    amount = TruncateToInt32(static_cast<double>(amount) * kPaymentBribeScale);
    amount =
        TruncateToInt32(static_cast<double>(amount) * kPaymentRoundFactor) *
        100;
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
// (0x17), in [0.8, 1.2] -> `mid_msg`, above -> terrible mood (0x18). The
// player-fuel branch passes 0x1c ("I'll help you out if you pay"), while the
// keep-pressing and distress branches pass 0x12 ("You'll have to pay me
// first.").
[[nodiscard]] std::optional<std::string> LoadMoodPrompt(
    std::int16_t random_index, double personality, std::uint16_t mid_msg) {
  if (personality < kPersonalityMoodLow) {
    return LoadCommPrompt(random_index, kMsgGoodMood);
  }
  if (personality <= kPersonalityMoodHigh) {
    return LoadCommPrompt(random_index, mid_msg);
  }
  return LoadCommPrompt(random_index, kMsgTerribleMood);
}

// The hail-info text (DAT_007d190c) the Greetings button shows for
// Ghidra 0x004819d0 NovaUi_BuildShipCommHailInfoText (partial port).
// Used for government-aid-eligible ships. The full assembly (branch 0:
// stellar scan + commodity names) is deferred;
// this mirrors the default STR# 0x7d2 0xaf fragment the original shows when
// the hail target carries no dude hail info (the common case).
[[nodiscard]] std::string BuildHailInfoText(const GameState &state,
                                            const Ship &target) {
  // TODO(decomp): dude-def hail_info_types / stellar-scan assembly of
  // NovaUi_BuildShipCommHailInfoText branch 0 is not reconstructed; the
  // original's default fragment is shown instead.
  // 1-based STR# 0x7d2 entry 0xb0 = "is a good place to".
  std::string text = NovaHud_LoadStringEntry(kMiscStr, 0xb0).value_or("");

  // The original applies a personality CommQuote last, replacing whichever
  // generic/dude hail-info branch was selected above. A sparse `STR ` at
  // 15000 + CommQuote overrides the matching STR# 7100 entry.
  if (target.pers_def_slot >= 0 &&
      target.pers_def_slot <
          static_cast<std::int16_t>(state.scenario.pers_defs.size())) {
    const PersDef &pers =
        state.scenario
            .pers_defs[static_cast<std::size_t>(target.pers_def_slot)];
    if (pers.comm_quote_id > 0) {
      if (auto quote = NovaResources_LoadStringResource(
              static_cast<std::uint16_t>(pers.comm_quote_id + 15000))) {
        return *quote;
      }
      if (auto quote = NovaHud_LoadStringEntry(
              0x1bbc, static_cast<std::uint16_t>(pers.comm_quote_id))) {
        return *quote;
      }
    }
  }
  return text;
}

void DrawEscortManagementDialog(SdlPlatform &platform,
                                const GameState &state,
                                SpaceflightView &view,
                                HudRenderer &hud,
                                NovaFontCache &font_cache,
                                const ServicesButtonArt &button_art,
                                SDL_Texture *backdrop,
                                SDL_Texture *ship_picture,
                                const Ship &escort,
                                const ShipClass &ship_class,
                                bool can_upgrade,
                                bool can_sell,
                                const EscortManagementLayout &layout) {
  view.DrawGameFrame(platform, state, hud);
  platform.SetCenteredPlayfield();
  SDL_Renderer *renderer = platform.renderer();
  constexpr float kScreenFontSize = 9.0F;
  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &layout.window);
  } else {
    SDL_SetRenderDrawColor(renderer, 16, 40, 72, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &layout.window);
  }
  if (ship_picture != nullptr) {
    SDL_RenderTexture(renderer, ship_picture, nullptr, &layout.picture);
  }

  const auto draw_line = [&](const SDL_FRect &rect,
                             float x,
                             float y,
                             SDL_Color color,
                             std::string_view text) {
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  kScreenFontSize,
                  kNovaFontStyleRegular,
                  color,
                  rect.x + x,
                  rect.y + y,
                  text);
  };
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &layout.details);
  SDL_RenderFillRect(renderer, &layout.status);
  draw_line(layout.details,
            0,
            12,
            kPanelGrey,
            NovaHud_LoadStringEntry(
                kMiscStr, escort.escort_origin_mark == 0 ? 0xa7 : 0xa6)
                .value_or(escort.escort_origin_mark == 0 ? "Captured Escort"
                                                         : "Hired Escort"));
  draw_line(layout.details, 3, 27, kTitle, ship_class.display_name);
  if (!ship_class.subtitle.empty()) {
    draw_line(layout.details, 3, 41, kTitle, ship_class.subtitle);
  }

  if (!can_upgrade) {
    draw_line(layout.status,
              0,
              12,
              kPanelGrey,
              NovaHud_LoadStringEntry(kMiscStr, 0x126)
                  .value_or("No upgrades available"));
  } else if (escort.escort_upgrade_mark != 0) {
    draw_line(layout.status,
              0,
              12,
              kPanelGrey,
              NovaHud_LoadStringEntry(kMiscStr, 0x124)
                  .value_or("Marked for upgrade"));
  } else {
    draw_line(
        layout.status,
        0,
        12,
        kPanelGrey,
        NovaHud_LoadStringEntry(kMiscStr, 0x125).value_or("Upgrade cost:"));
    draw_line(layout.status,
              70,
              12,
              kTitle,
              GroupThousands(static_cast<std::uint32_t>(
                  std::max(0, ship_class.escort_upgrade_cost))) +
                  " " +
                  (ship_class.escort_upgrade_cost == 1 ? "credit" : "credits"));
  }
  if (can_sell) {
    if (escort.escort_pending_sale_mark != 0) {
      draw_line(
          layout.status,
          0,
          28,
          kPanelGrey,
          NovaHud_LoadStringEntry(kMiscStr, 0x127).value_or("Marked for sale"));
    } else {
      draw_line(
          layout.status,
          0,
          28,
          kPanelGrey,
          NovaHud_LoadStringEntry(kMiscStr, 0x128).value_or("Sale value:"));
      draw_line(layout.status,
                70,
                28,
                kTitle,
                GroupThousands(static_cast<std::uint32_t>(
                    std::max(0, ship_class.escort_sell_value))) +
                    " " +
                    (ship_class.escort_sell_value == 1 ? "credit" : "credits"));
    }
  }
  if (escort.escort_origin_mark != 0) {
    const auto daily_cost =
        static_cast<std::int32_t>(static_cast<double>(ship_class.cost) * 0.01);
    draw_line(layout.status,
              0,
              42,
              kPanelGrey,
              NovaHud_LoadStringEntry(kMiscStr, 0x129).value_or("Pay:"));
    draw_line(layout.status,
              30,
              42,
              kTitle,
              GroupThousands(static_cast<std::uint32_t>(
                  std::max(std::int32_t{0}, daily_cost))) +
                  " " + (daily_cost == 1 ? "credit" : "credits") + " " +
                  NovaHud_LoadStringEntry(kMiscStr, 0x10b).value_or("per day"));
  }

  const std::array<std::string, 4> labels{
      LoadButtonLabel(0x15),
      LoadButtonLabel(0x20),
      LoadButtonLabel(escort.escort_upgrade_mark == 0 ? 0x34 : 0x35),
      LoadButtonLabel(escort.escort_pending_sale_mark == 0 ? 0x36 : 0x37)};
  const SDL_FPoint mouse = platform.mouse_position();
  const bool mouse_down =
      (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0U;
  for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
    const bool enabled = i < 2 || (i == 2 ? can_upgrade : can_sell);
    const bool pressed = enabled && mouse_down &&
                         SDL_PointInRectFloat(&mouse, &layout.buttons[i]);
    button_art.Draw(platform,
                    layout.buttons[i],
                    !enabled  ? ButtonState::kDisabled
                    : pressed ? ButtonState::kHover
                              : ButtonState::kNormal);
    DrawThreeStateButtonLabel(platform,
                              font_cache,
                              layout.buttons[i],
                              labels[i],
                              enabled ? kTitle : kPanelGrey);
  }
}

// Ghidra 0x004853a0 NovaUi_RunEscortShipManagementWindow; drawing function
// 0x00485970 and button helpers 0x004a1d90/0x004a1ed0 run inline here.
[[nodiscard]] bool RunEscortManagementDialog(SdlPlatform &platform,
                                             GameState &state,
                                             Ship &escort,
                                             SpaceflightView &view,
                                             HudRenderer &hud) {
  const ShipClass *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(escort.ship_class_id + 0x80));
  if (ship_class == nullptr) {
    return false;
  }
  const ShipClass *upgrade = state.scenario.Ship(
      static_cast<std::int16_t>(ship_class->upgrade_to_ship_class_id + 0x80));
  const bool can_upgrade =
      ship_class->upgrade_to_ship_class_id >= 0 && upgrade != nullptr &&
      Mission_CheckReactionConditionSatisfied(state,
                                              upgrade->availability_expr);
  const bool can_sell = escort.escort_origin_mark == 0;
  const EscortManagementLayout layout = LoadEscortManagementLayout();
  auto backdrop = LoadPictTexture(platform, kEscortManagementFramePict);
  auto ship_picture =
      LoadPictTexture(platform, ship_class->pict_fallback_sprite_resource_id);
  NovaFontCache font_cache;
  ServicesButtonArt button_art;
  (void)button_art.Initialize(platform);

  while (!platform.quit_requested()) {
    DrawEscortManagementDialog(platform,
                               state,
                               view,
                               hud,
                               font_cache,
                               button_art,
                               backdrop ? backdrop->get() : nullptr,
                               ship_picture ? ship_picture->get() : nullptr,
                               escort,
                               *ship_class,
                               can_upgrade,
                               can_sell,
                               layout);
    platform.Present();
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      std::optional<EscortManagementAction> action;
      if (in->key == TextKey::escape || in->key == TextKey::enter) {
        action = EscortManagementAction::kClose;
      } else if (in->key == TextKey::character) {
        switch (in->character) {
        case 'c':
        case 'C':
          action = EscortManagementAction::kClose;
          break;
        case 'r':
        case 'R':
          action = EscortManagementAction::kRelease;
          break;
        case 'u':
        case 'U':
          action = EscortManagementAction::kToggleUpgrade;
          break;
        case 's':
        case 'S':
          action = EscortManagementAction::kToggleSale;
          break;
        default:
          break;
        }
      } else if (in->key == TextKey::primary) {
        const SDL_FPoint point = platform.mouse_position();
        for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
          if (SDL_PointInRectFloat(&point, &layout.buttons[i])) {
            action = static_cast<EscortManagementAction>(i);
            break;
          }
        }
      }
      if (!action ||
          (*action == EscortManagementAction::kToggleUpgrade && !can_upgrade) ||
          (*action == EscortManagementAction::kToggleSale && !can_sell)) {
        continue;
      }
      if (NovaEscortManagement_ApplyAction(
              state, escort, *action, platform.gameplay_ticks_ms())) {
        return true;
      }
    }
    platform.PaceFrame();
  }
  return true;
}

} // namespace

bool NovaEscortManagement_ApplyAction(GameState &state,
                                      Ship &escort,
                                      EscortManagementAction action,
                                      std::uint32_t now_ms) {
  switch (action) {
  case EscortManagementAction::kClose:
    return true;
  case EscortManagementAction::kRelease: {
    const ShipClass *ship_class = state.scenario.Ship(
        static_cast<std::int16_t>(escort.ship_class_id + 0x80));
    Player_TransferCargoAndJunkToEscortByRatio(state, escort.ship_instance_id);
    escort.squad_leader_ship_slot = -1;
    escort.post_hit_mode_hint = -1;
    escort.boarded_target_latch = 1;
    escort.ai_behavior_code =
        ship_class != nullptr ? ship_class->default_ai_behavior : 1;
    NovaShip_ResetAiBehaviorRuntimeFields(escort);
    NovaAi_EnterState2ClearPrimaryTarget(escort, now_ms);
    state.InvalidateDerivedStatCaches();
    return true;
  }
  case EscortManagementAction::kToggleUpgrade:
    if (const ShipClass *ship_class = state.scenario.Ship(
            static_cast<std::int16_t>(escort.ship_class_id + 0x80));
        ship_class == nullptr || ship_class->upgrade_to_ship_class_id < 0) {
      return false;
    } else if (const ShipClass *upgrade =
                   state.scenario.Ship(static_cast<std::int16_t>(
                       ship_class->upgrade_to_ship_class_id + 0x80));
               upgrade == nullptr || !Mission_CheckReactionConditionSatisfied(
                                         state, upgrade->availability_expr)) {
      return false;
    }
    escort.escort_upgrade_mark = escort.escort_upgrade_mark == 0 ? 1 : 0;
    if (escort.escort_upgrade_mark != 0) {
      escort.escort_pending_sale_mark = 0;
    }
    return false;
  case EscortManagementAction::kToggleSale:
    if (escort.escort_origin_mark != 0) {
      return false;
    }
    escort.escort_pending_sale_mark =
        escort.escort_pending_sale_mark == 0 ? 1 : 0;
    if (escort.escort_pending_sale_mark != 0) {
      escort.escort_upgrade_mark = 0;
    }
    return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// The ship-comm modal
// ---------------------------------------------------------------------------
bool NovaShipComm_RunShipDialog(SdlPlatform &platform,
                                GameState &state,
                                std::int16_t ship_slot,
                                SpaceflightView &view,
                                HudRenderer &hud) {
  state.gameplay_now_ms = platform.gameplay_ticks_ms();
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

  // Behavior-6 player escorts with no mission fleet open the
  // dedicated escort-management window rather than ordinary communications.
  if (target.ai_behavior_code == 6 && target.squad_leader_ship_slot == 0 &&
      target.mission_fleet_slot == -1) {
    return RunEscortManagementDialog(platform, state, target, view, hud);
  }

  // ---- Opening gates (mirrors 0x0047e470) ---------------------------------
  // Ship faction and class inherent-govt fields are zero-based def indexes.
  const Government *govt =
      target.faction_or_government_id != -1
          ? state.scenario.GovernmentByIndex(target.faction_or_government_id)
          : nullptr;
  const ShipClass *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(target.ship_class_id + 0x80));
  const Government *class_govt =
      ship_class != nullptr && ship_class->inherent_attributes_govt != -1
          ? state.scenario.GovernmentByIndex(
                ship_class->inherent_attributes_govt)
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
  const bool special_mask = govt != nullptr && (govt->flags_secondary & 1) != 0;
  bool comm_special = govt != nullptr && (govt->flags_secondary & 8) != 0;
  if (class_govt != nullptr && (class_govt->flags_secondary & 8) != 0) {
    comm_special = true;
  }
  const bool keep_refusal = govt != nullptr && target.ai_behavior_code > 2 &&
                            (govt->flags_primary & kGovtFlagKeepRefusal) != 0U;
  const bool govt_aid_flag =
      govt != nullptr && (govt->flags_primary & kGovtFlagXenophobic) != 0U;
  const bool free_help = govt != nullptr && (govt->flags_secondary & 0x10) != 0;

  // Per-launch random flavour index (g_travel_interaction_random_index).
  const std::int16_t random_index = RandomBelow(state.rng, 5);

  // Ship "personality" factor (DAT_007d17ec): (rand(0x29)+0x50)*0.01 with a
  // 1-in-5 -0.5 / further 1-in-5 +0.5 mood swing.
  float personality = static_cast<float>(RandomBelow(state.rng, 0x29) + 0x50) *
                      kPersonalityStep;
  if (RandomBelow(state.rng, 5) == 0) {
    personality -= kPersonalityMoodSwing;
  } else if (RandomBelow(state.rng, 5) == 0) {
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
  const bool fire_restricted = NovaAiShip_IsDisabled(state, target);
  if (!fire_restricted && !special_mask) {
    if (NovaAiShip_ShouldKeepPressingTarget(state, target)) {
      status = LoadCommPrompt(random_index, kMsgWhatDoYouWant).value_or(status);
    } else if (target.squad_leader_ship_slot != 0) {
      status = NovaShip_DoesShipLikePlayer(state, target)
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
  // Ship portrait PICT (0x0047e470): the class's 200x200 portrait PICT
  // (ShipClass.pict_fallback_sprite_resource_id, 5000 + class id, populated
  // by the scenario loader at startup), overridden by the ship's pers
  // personality HailPict (+0x12) when it names a real PICT (> 0x7f).
  std::unique_ptr<SdlTexture> ship_picture;
  {
    std::uint16_t pict_id = 0;
    if (ship_class != nullptr &&
        ship_class->pict_fallback_sprite_resource_id != 0) {
      pict_id = ship_class->pict_fallback_sprite_resource_id;
    }
    if (target.pers_def_slot >= 0 &&
        target.pers_def_slot <
            static_cast<std::int16_t>(state.scenario.pers_defs.size())) {
      const PersDef &pers =
          state.scenario
              .pers_defs[static_cast<std::size_t>(target.pers_def_slot)];
      if (pers.alive && pers.hail_pict_id > 0x7f) {
        pict_id = static_cast<std::uint16_t>(pers.hail_pict_id);
      }
    }
    if (pict_id != 0) {
      ship_picture = LoadPictTexture(platform, pict_id);
    }
  }
  ServicesButtonArt button_art;
  if (!button_art.Initialize(platform)) {
    NovaLog::Warn("three-state button art unavailable for the comm dialog "
                  "buttons");
  }
  const CommFrameLayout layout = LoadCommFrameLayout();
  NovaFontCache font_cache;
  const SDL_FRect panel{0.0F, 0.0F, 640.0F, 480.0F};
  platform.SetCenteredPlayfield();

  // Item-11 info block contents (0x0047e470 + 0x0047fb70): the class's Bible
  // CommName (DAT_006bd2cc table), the pers 0x3ff "Ambrosia Mascot"
  // placeholder (DAT_0056cc30) and the government comm-name table entry
  // (DAT_007d1d0c; faction-less ships get an empty pstring, DAT_0056cc2c,
  // which drops the parenthesised line).
  const std::string class_name =
      ship_class != nullptr && !ship_class->comm_name.empty()
          ? ship_class->comm_name
          : target.ship_name;
  const bool pers_placeholder = target.pers_def_slot == 0x3ff;
  const std::string comm_name = govt != nullptr && !govt->comm_name.empty()
                                    ? govt->comm_name
                                    : std::string{};

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
  // For special-scan-mask ships the original drops the Greetings button down
  // to the middle rect (DITL item 1, y=153) while the assistance slot hits
  // the offscreen item 3 -- the omitted assistance slot here leaves the
  // middle rect free for Greetings.
  std::vector<ServiceButton> buttons;
  buttons.reserve(3);
  buttons.push_back(ServiceButton{layout.buttons[0], kCloseChannel});
  if (keep_assistance) {
    buttons.push_back(ServiceButton{layout.buttons[1], kAssistance});
  }
  buttons.push_back(ServiceButton{
      special_mask ? layout.buttons[1] : layout.buttons[2], kGreetings});
  std::string assistance_label;
  if (NovaAiShip_ShouldKeepPressingTarget(state, target)) {
    assistance_label = LoadButtonLabel(kBtnBegForMercy);
  } else if (target.squad_leader_ship_slot == 0 &&
             target.ai_behavior_code == 6 && !mission_escort) {
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
    if (target.squad_leader_ship_slot == 0 && target.ai_behavior_code == 6 &&
        !mission_escort) {
      // Escort release: latch the cargo transfer and show the goodbye message
      // (STR# 0xbb9). The window stays open until the player closes the
      // channel; the transfer/re-hire runs on close.
      escort_transfer_armed = true;
      status = LoadCommPrompt(random_index, kMsgEscortGoodbye).value_or(status);
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
      if (target.defense_fleet_home_stellar_id == -1 && bribe_offered) {
        status = LoadMoodPrompt(random_index, personality, kMsgPayFirst)
                     .value_or(status);
        const BribeOutcome outcome =
            RunBribePayment(state, bribe_cost, free_help);
        if (outcome == BribeOutcome::kPaid) {
          status = LoadCommPrompt(random_index, kMsgBusiness).value_or(status);
          NovaAi_EnterState2ClearPrimaryTarget(
              target, static_cast<std::uint32_t>(platform.gameplay_ticks_ms()));
          target.ai_behavior_code = 1;
        } else if (outcome == BribeOutcome::kRefused) {
          status =
              LoadCommPrompt(random_index, kMsgPrepareToDie).value_or(status);
          NovaAi_SetShipHostileToPlayer(state, target);
        } else {
          status =
              LoadCommPrompt(random_index, kMsgCantAfford).value_or(status);
        }
      } else {
        status = LoadCommPrompt(random_index, kMsgDreams).value_or(status);
      }
      return;
    }
    if (!NovaShip_DoesShipLikePlayer(state, target) || govt_aid_flag ||
        keep_refusal) {
      // No aid on offer: "In your dreams, pal."
      status = LoadCommPrompt(random_index, kMsgDreams).value_or(status);
      return;
    }
    if (NovaAiShip_IsThreatened(state, target) ||
        NovaAiShip_IsShipInNonIdleAiState(target)) {
      // The ship is already busy with something: "I'm busy." / "Sorry sir, I
      // can't do that." (escorts), or "Okay, I'm on my way." when it is
      // braking onto the player in state 0x09/0x0F.
      if (!NovaAiShip_IsShipAssistingPlayerState9(state, target) &&
          !NovaAiShip_IsShipAssistingPlayerState0xF(state, target)) {
        status =
            target.ai_behavior_code < 5
                ? LoadCommPrompt(random_index, kMsgImBusy).value_or(status)
                : LoadCommPrompt(random_index, kMsgCantDoSir).value_or(status);
      } else {
        status = LoadCommPrompt(random_index, kMsgOnMyWay).value_or(status);
      }
      return;
    }
    if (!NovaAi_IsAnyShipThreatToPlayerSquad(state)) {
      // No distress call in progress: the fuel-offer branch triggers when the
      // player's tank is low (or the player is disabled). A govt-aid
      // ship (xenophobic flag) refuses with "In your dreams, pal." instead;
      // otherwise "You're not in any trouble."
      const bool player_fuel_low =
          state.player.fuel_points < kPlayerFuelOfferThreshold &&
          state.cached_stats.fuel_capacity > 0;
      if (player_fuel_low || NovaAiShip_IsDisabled(state, state.player)) {
        if (govt_aid_flag) {
          status = LoadCommPrompt(random_index, kMsgDreams).value_or(status);
        } else if (target.ai_behavior_code < 5) {
          status = LoadMoodPrompt(random_index, personality, kMsgHelpIfPay)
                       .value_or(status);
          const BribeOutcome outcome =
              RunBribePayment(state, bribe_cost, free_help);
          if (outcome == BribeOutcome::kPaid) {
            status = LoadCommPrompt(random_index, kMsgOnMyWay).value_or(status);
            if (NovaAiShip_IsDisabled(state, state.player)) {
              NovaAi_EnterState0FTargetPlayerForAssist(target);
            } else {
              NovaAi_EnterState9TargetPlayerForAssist(target);
            }
          } else if (outcome == BribeOutcome::kRefused) {
            status =
                LoadCommPrompt(random_index, kMsgComedian).value_or(status);
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
    if (!NovaAiShip_IsPlayerThreatenedByEnemyOfShip(state, target)) {
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
        status = LoadMoodPrompt(random_index, personality, kMsgPayFirst)
                     .value_or(status);
        const BribeOutcome outcome =
            RunBribePayment(state, bribe_cost, free_help);
        if (outcome == BribeOutcome::kPaid) {
          status = LoadCommPrompt(random_index, kMsgOnMyWay).value_or(status);
          NovaAi_EnterState4TargetRandomUnengagedShip(state, target);
        } else if (outcome == BribeOutcome::kRefused) {
          status = LoadCommPrompt(random_index, kMsgComedian).value_or(status);
        } else {
          status =
              LoadCommPrompt(random_index, kMsgCantAfford).value_or(status);
        }
      } else if (behavior == 3 || behavior == 4) {
        status = LoadMoodPrompt(random_index, personality, kMsgPayFirst)
                     .value_or(status);
        const BribeOutcome outcome =
            RunBribePayment(state, bribe_cost, free_help);
        if (outcome == BribeOutcome::kPaid) {
          status = LoadCommPrompt(random_index, kMsgOnMyWay).value_or(status);
          NovaAi_EnterState4TargetRandomUnengagedShip(state, target);
        } else if (outcome == BribeOutcome::kRefused) {
          status = LoadCommPrompt(random_index, kMsgComedian).value_or(status);
        } else {
          status =
              LoadCommPrompt(random_index, kMsgCantAfford).value_or(status);
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
          NovaShip_DoesShipLikePlayer(state, target)) {
        const std::string info = BuildHailInfoText(state, target);
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
                       state,
                       view,
                       hud,
                       font_cache,
                       button_art,
                       backdrop ? backdrop->get() : nullptr,
                       ship_picture ? ship_picture->get() : nullptr,
                       target,
                       escort_transfer_armed,
                       class_name,
                       pers_placeholder,
                       comm_name,
                       status,
                       buttons,
                       button_labels,
                       layout,
                       panel);
    platform.Present();

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
    platform.PaceFrame();
  }

  // ---- Close-time side effects ---------------------------------------------
  if (escort_transfer_armed) {
    Player_TransferCargoAndJunkToEscortByRatio(state, target.ship_instance_id);
    target.squad_leader_ship_slot = -1;
    target.ai_behavior_code =
        ship_class != nullptr ? ship_class->default_ai_behavior : 1;
    target.boarded_target_latch = 1;
    target.post_hit_mode_hint = -1;
    // Ship_ResetShipAiBehaviorRuntimeFields (0x00402810) subset:
    target.ai_state_code = 0;
    target.ai_control_mode = 0;
    target.ai_secondary_target_slot = -1;
    NovaAi_EnterState2ClearPrimaryTarget(
        target, static_cast<std::uint32_t>(platform.gameplay_ticks_ms()));
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
  if (target.squad_leader_ship_slot != 0 || target.mission_fleet_slot != -1) {
    // Busy ships whose government (or whose class's inherent government)
    // carries the 0x400 busy flag cannot be hailed.
    if (target.faction_or_government_id != -1) {
      const Government *g =
          state.scenario.GovernmentByIndex(target.faction_or_government_id);
      if (g != nullptr && (g->flags_primary & 0x400U) != 0) {
        eligible = false;
      }
    }
    const ShipClass *cls = state.scenario.Ship(
        static_cast<std::int16_t>(target.ship_class_id + 0x80));
    if (cls != nullptr && cls->inherent_attributes_govt != -1) {
      const Government *g =
          state.scenario.GovernmentByIndex(cls->inherent_attributes_govt);
      if (g != nullptr && (g->flags_primary & 0x400U) != 0) {
        eligible = false;
      }
    }
  }
  if (target.ship_class_id == 0x2ff) {
    eligible = false; // special non-comm ship class
  }
  if (NovaAiShip_IsDisabled(state, target)) {
    eligible = false;
  }
  if (target.pers_def_slot == 0x3ff) {
    eligible = false;
  }
  return eligible;
}

} // namespace game
