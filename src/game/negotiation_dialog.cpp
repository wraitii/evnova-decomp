#include "negotiation_dialog.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../pict_image.hpp"
#include "../sdl_platform.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "hud_renderer.hpp"
#include "nova_font.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "spaceflight_view.hpp"
#include "sprite_world.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace game {
namespace {

// ---- Deferred Demand Tribute / Release hooks (TODO(decomp)) ----------------
// The middle button's branch in NovaUi_RunTravelDestinationInteractionWindow
// (0x00480030, local_11e == 3) is intentionally NOT reconstructed yet. It
// spans (decomp, verified 2024 read):
//
//   Government_ProcessFactionCombatEvent 0x00466fc0
//     (system_id, government_id, 3, -1) per attack/demand pulse (now ported as
//     NovaGovernment_ProcessFactionCombatEvent; this branch itself is still
//     deferred)
//   Stellar_SpawnDefenseFleetShip (defense-fleet spawns, capped by
//     StellarDef max_ship_count/present_ship_count bookkeeping)
//   the domination latch: StellarDef.hazard_marker = 1, tribute status
//     (STR# 0xbba msg 25/26), present_ship_count reset, plus the release
//     mirror branch (msg 35/36, hazard_marker = 0)
//   Mission_ExecuteReactionScript 0x00448020 on the stellar's reaction
//     scripts (StellarDef field_0x68 / 0x167)
//
// All are called only from that branch, so no stub is emitted here (an unused
// declaration would trip -Werror).

// ---- Resource ids (EV Nova Graphics 3 / Nova Data) ------------------------
// The destination-interaction window's backdrop PICT (DLOG 0x3f1) and the
// payment modal's backdrop PICT (DLOG 0x3f0).
constexpr std::uint16_t kNegotiationBackdropPict = 0x2140;
constexpr std::uint16_t kPaymentBackdropPict = 0x2142;

// STR# pools carrying the interaction status / prompt text. Status strings
// (STR# 0xbba) and prompts (STR# 0xbb8 for prompt_index < 0x26, 0xbb9
// otherwise) each store five flavour variants per message; the dialog picks
// one at `index*5 + random + 1` (see NovaUi_LoadTravelDestinationStatusString
// 0x00482910 / NovaUi_LoadTravelDestinationPromptString 0x004828c0).
constexpr std::uint16_t kStatusStr = 0xbba;
constexpr std::uint16_t kPromptStr = 0xbb8;
// STR# 0x7d2 "misc strings" (1-based entry numbers, as the original passes
// them to Resource_LoadStringEntry / Resource_DrawStringEntry).
constexpr std::uint16_t kMiscStr = 0x7d2;
// STR# 0x44c: the shipped stellar-class descriptor fragments ("[Class M]",
// "[Ice Moon]", ...) used when the stellar's own destination desc resource is
// absent (0x00480030: FUN_004c73b0(link_a_id + 7000) fallback).
constexpr std::uint16_t kStellarClassStr = 0x44c;

// STR# 0x96 button-label entries (1-based; the original indexes the
// DAT_007d83aa cache by the 0-based pool index, entry = pool + 1):
constexpr std::uint16_t kButtonLabelStr = 0x96;
constexpr std::uint16_t kBtnCloseChannel = 0x15;  // pool 0x14 "Close Channel"
constexpr std::uint16_t kBtnGreetings = 0x16;     // pool 0x15 "Greetings"
constexpr std::uint16_t kBtnOfferBribe = 0x18;    // pool 0x17 "Offer Bribe"
constexpr std::uint16_t kBtnRelease = 0x20;       // pool 0x1f "Release"
constexpr std::uint16_t kBtnDemandTribute = 0x2d; // pool 0x2c "Demand Tribute"
constexpr std::uint16_t kBtnAcceptPrice = 0x1e;   // pool 0x1d "Accept Price"
constexpr std::uint16_t kBtnLowerPrice = 0x1f;    // pool 0x1e "Lower Price"

// STR# 0x7d2 entries used here (1-based; pool = entry - 1):
constexpr std::uint16_t kMiscClearedToDock = 0x5f; // "you're cleared to dock."
constexpr std::uint16_t kMiscClearedToLand = 0x62; // "you're cleared to land."
constexpr std::uint16_t kMiscFinalApproach = 0x64; // "Commence final approach."
constexpr std::uint16_t kMiscCredit = 0x20;        // "credit"
constexpr std::uint16_t kMiscCredits = 0x21;       // "credits"
constexpr std::uint16_t kMiscPayYou = 0xbc;        // "I'll pay you"
constexpr std::uint16_t kMiscStatusLabel = 0xc4;   // "Status:"
constexpr std::uint16_t kMiscUninhabited = 0xaa;   // "Uninhabited"
constexpr std::uint16_t kMiscOwned = 0xab;         // "Owned"
constexpr std::uint16_t kMiscDominated = 0xac;     // "Dominated"
constexpr std::uint16_t kMiscForbidden = 0xad;     // "Forbidden"
constexpr std::uint16_t kMiscHostile = 0xae;       // "Hostile"

// Status/prompt message indices used by the dialog loop (see the decomp of
// NovaUi_RunTravelDestinationInteractionWindow 0x00480030). Kept explicit so
// the variant arithmetic (`index*5 + random + 1`) is readable at call sites.
constexpr std::uint16_t kMsgWelcomeStatus = 0; // "Communications channel
                                               // open to " (+ name + ".")
constexpr std::uint16_t kMsgNoResponse = 1;    // uninhabited prompt
constexpr std::uint16_t kMsgDeniedPrompt = 2;  // "What is it you want?"
constexpr std::uint16_t kMsgCannotAfford = 4;  // "Stop wasting our time."
constexpr std::uint16_t kMsgBribeDeclined = 6; // "No way. Leave immediately."
constexpr std::uint16_t kMsgBribeOffered = 8;  // "We'll let you slip by..."
constexpr std::uint16_t kMsgGreetingResponse = 9; // "Hello there." etc.
constexpr std::uint16_t kMsgRefuseNoBribe = 9; // "Yeah, right." (status pool)

// g_travel_interaction_bribe_random_latch gate: a per-launch roll in [0,100)
// from NovaRandom_Range(100); values > 0x1e (i.e. greater than 30) admit a
// bribe offer from an eligible faction (the 0x4000 bribable bit / no faction).
constexpr int kBribeLatchCutoff = 0x1e; // 30

// The payment window (DLOG 0x3f0) success chance, passed as `chance_percent`
// by the bribe path (0x23 = 35% chance "Lower Price" is granted).
constexpr int kPaymentChancePercent = 0x23;
// Payment-window factors (mirrors the Ghidra doubles): on a granted "Lower
// Price" the bribe-mode amount scales by 0.75 (_DAT_005758f0) and rounds to
// /100 (_DAT_00575898).
constexpr double kPaymentBribeScale = 0.75;
constexpr double kPaymentRoundFactor = 0.01;
// A declined/closed bribe bumps the counter-offer price by this much and
// resets the latch to force an eventual offer (0x00480030).
constexpr std::int32_t kHaggleIncrement = 1000;

// ---- Bribe cost constants (mirrors the Ghidra global doubles) -------------
// The random bribe upper bound is a meagre fraction of the player's credits
// (_DAT_005758d8 = 1e-06, so `random(credits * 1e-06) * 1000 + 3000`).
constexpr double kCreditsBribeFraction = 1e-06;
// The government 1.5x bribe-cost scale (_DAT_005758e0).
constexpr double kGovtBribeCostMultiplier = 1.5;
// The bribe is clamped down to 1/3 of the player's credits (_DAT_005758b8).
constexpr double kMaxBribeOfCredits = 0.333;
constexpr std::int32_t kBribeBaseAdd = 3000;      // +3000 to the random pick
constexpr std::int32_t kBribeGrouping = 1000;     // round down to /1000
constexpr std::int32_t kBribeLowerBound = 1000;   // >= 1000
constexpr std::int32_t kBribeUpperBound = 900000; // <= 900000

// ---- Government flag gates (GovtDef.flags_primary) ------------------------
// 0x8000: the faction "bribes the player" -- raises the bribe cost 1.5x and
// forces bribe eligibility. 0x4000: the faction's ships will take a bribe.
// Both read by NovaUi_RunTravelDestinationInteractionWindow.
constexpr std::uint16_t kGovtFlagBribeCostly = 0x8000;
constexpr std::uint16_t kGovtFlagBribable = 0x4000;

// Indexes the five-variant STR# pools: each message's variants live at
// `message_index*5 + variant_index + 1`, with variant_index in [0,5) chosen
// by NovaRandom_Range(5) at launch (`g_travel_interaction_random_index`).
[[nodiscard]] std::uint16_t StringVariantIndex(std::int16_t random_index,
                                               std::uint16_t message_index) {
  return static_cast<std::uint16_t>(message_index * 5u + random_index + 1);
}

// ---- Palette ---------------------------------------------------------------
// PTR_DAT_00575ad8 -> RGBColor (65535,65535,65535) white and
// PTR_DAT_00575ae4 -> (65535,0,0) red, read from the binary. The remaining
// colours are runtime-initialized (PTR_DAT_00575acc and the SHORT_ARRAY_0073-
// 3b* c.lr palette entries live in .bss); black and the two greys mirror the
// approximations boarding_plunder.cpp established against the shipped art.
constexpr SDL_Color kWhite{255, 255, 255, 255}; // PTR_DAT_00575ad8
constexpr SDL_Color kBlack{0, 0, 0, 255};       // PTR_DAT_00575acc (runtime)
constexpr SDL_Color kGrey{128, 128, 128, 255};  // DAT_00733b56 (runtime)
constexpr SDL_Color kLightGrey{192, 192, 192, 255}; // DAT_00733b50 (runtime)
constexpr SDL_Color kRed{255, 0, 0, 255};           // PTR_DAT_00575ae4
// The "Forbidden" status word draws with a stack-built RGBColor
// (0xffff, 0x6666, 0). SHORT_ARRAY_00733b32 (Owned/Dominated) is unreadable
// .bss; the same warm tone is used for it (TODO(decomp): verify).
constexpr SDL_Color kStatusOrange{255, 102, 0, 255};

// ---- DLOG 0x3f1 layout ----------------------------------------------------
// All geometry comes from the real DLOG/DITL 0x3f1 resources, mapped into the
// centred 640x480 playfield canvas. The constants below are the fallback used
// when the resources fail to decode; they mirror the shipped DITL (see
// docs/dlog_ditl_dialog_format.md).
constexpr float kNegotiationFrameWidth = 540.0F;
constexpr float kNegotiationFrameHeight = 295.0F;
constexpr SDL_FRect kFallbackButtonRects[3] = {
    {27.0F, 244.0F, 146.0F, 26.0F}, // item 0: Close Channel (bottom)
    {27.0F, 184.0F, 146.0F, 26.0F}, // item 1: Greetings / Offer Bribe (top)
    {27.0F, 214.0F, 146.0F, 26.0F}, // item 2: Demand Tribute / Release (middle)
};
constexpr SDL_FRect kFallbackStatusRect{5.0F, 5.0F, 200.0F, 60.0F};   // item 3
constexpr SDL_FRect kFallbackImageRect{222.0F, 5.0F, 310.0F, 283.0F}; // item 4
constexpr SDL_FRect kFallbackHeaderRect{16.0F, 82.0F, 120.0F, 50.0F}; // item 5

// One frame's worth of geometry + text, shared by the draw path and the
// payment sub-window (which re-renders the interaction window beneath itself
// every frame, like the boarding capture-decision dialog).
struct NegotiationFrame {
  SDL_FRect window{};
  SDL_FRect buttons[3]{};
  SDL_FRect status_panel{};
  SDL_FRect image{};
  SDL_FRect header{};
  std::string button_labels[3];
  bool tribute_enabled = false;
  std::string status;
  std::string name;
  std::string description;
  std::string status_label;
  std::string status_word;
  SDL_Color status_word_color = kWhite;
};

[[nodiscard]] SDL_FRect OffsetRect(SDL_FRect rect, SDL_FPoint origin) {
  rect.x += origin.x;
  rect.y += origin.y;
  return rect;
}

// Loads DLOG 0x3f1 + DITL, centred on the fixed 640x480 canvas like
// Dialog_CreateFromDlog; fills `frame`'s geometry from DITL items 0..5.
// Returns false (leaving the shipped-DITL fallback geometry) when the
// resources are unavailable.
[[nodiscard]] bool LoadNegotiationGeometry(SDL_FRect *buttons,
                                           SDL_FRect &status_panel,
                                           SDL_FRect &image,
                                           SDL_FRect &header,
                                           SDL_FRect &window) {
  const auto definition = NovaResource_LoadDialogDefinition(0x3f1);
  const auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (!definition || !items) {
    return false;
  }
  const float width = static_cast<float>(definition->right - definition->left);
  const float height = static_cast<float>(definition->bottom - definition->top);
  const SDL_FPoint origin{std::truncf((640.0F - width) * 0.5F),
                          std::truncf((480.0F - height) * 0.5F)};
  window = {origin.x, origin.y, width, height};
  for (const auto &item : *items) {
    const SDL_FRect rect =
        OffsetRect({static_cast<float>(item.left),
                    static_cast<float>(item.top),
                    static_cast<float>(item.right - item.left),
                    static_cast<float>(item.bottom - item.top)},
                   origin);
    switch (item.index) {
    case 0:
    case 1:
    case 2:
      buttons[item.index] = rect;
      break;
    case 3:
      status_panel = rect;
      break;
    case 4:
      image = rect;
      break;
    case 5:
      header = rect;
      break;
    default:
      break;
    }
  }
  return true;
}

// ---- DLOG 0x3f0 (payment) layout -----------------------------------------
constexpr float kPaymentFrameWidth = 262.0F;
constexpr float kPaymentFrameHeight = 107.0F;
constexpr SDL_FRect kFallbackPaymentButtonRects[2] = {
    {58.0F, 74.0F, 146.0F, 26.0F}, // item 0: Accept Price (bottom)
    {58.0F, 39.0F, 146.0F, 26.0F}, // item 1: Lower Price (top)
};
constexpr SDL_FRect kFallbackPaymentTextRect{
    7.0F, 6.0F, 248.0F, 25.0F}; // item 2

struct PaymentFrame {
  SDL_FRect window{};
  SDL_FRect buttons[2]{};
  SDL_FRect text_band{};
};

[[nodiscard]] PaymentFrame LoadPaymentGeometry() {
  PaymentFrame frame;
  frame.window = {std::truncf((640.0F - kPaymentFrameWidth) * 0.5F),
                  std::truncf((480.0F - kPaymentFrameHeight) * 0.5F),
                  kPaymentFrameWidth,
                  kPaymentFrameHeight};
  frame.buttons[0] = OffsetRect(kFallbackPaymentButtonRects[0],
                                {frame.window.x, frame.window.y});
  frame.buttons[1] = OffsetRect(kFallbackPaymentButtonRects[1],
                                {frame.window.x, frame.window.y});
  frame.text_band =
      OffsetRect(kFallbackPaymentTextRect, {frame.window.x, frame.window.y});
  const auto definition = NovaResource_LoadDialogDefinition(0x3f0);
  const auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (!definition || !items) {
    NovaLog::Todo("DLOG/DITL 0x3f0 unavailable; using the shipped-geometry "
                  "fallback for the payment window");
    return frame;
  }
  const float width = static_cast<float>(definition->right - definition->left);
  const float height = static_cast<float>(definition->bottom - definition->top);
  const SDL_FPoint origin{std::truncf((640.0F - width) * 0.5F),
                          std::truncf((480.0F - height) * 0.5F)};
  frame.window = {origin.x, origin.y, width, height};
  for (const auto &item : *items) {
    const SDL_FRect rect =
        OffsetRect({static_cast<float>(item.left),
                    static_cast<float>(item.top),
                    static_cast<float>(item.right - item.left),
                    static_cast<float>(item.bottom - item.top)},
                   origin);
    if (item.index <= 1) {
      frame.buttons[item.index] = rect;
    } else if (item.index == 2) {
      frame.text_band = rect;
    }
  }
  return frame;
}

// ---- STR# helpers ---------------------------------------------------------
// Loads one flavour variant of a status (STR# 0xbba) message. `random_index`
// is the per-launch flavour pick; mirrors NovaUi_LoadTravelDestinationStatus-
// String (0x00482910) loading `message*5 + random + 1` (a 1-based entry
// number, matching the 1-based STR# helper).
[[nodiscard]] std::optional<std::string>
LoadStatusVariant(std::int16_t random_index, std::uint16_t message_index) {
  return NovaHud_LoadStringEntry(
      kStatusStr, StringVariantIndex(random_index, message_index));
}

// Loads one flavour variant of a prompt (STR# 0xbb8) message. Mirrors
// NovaUi_LoadTravelDestinationPromptString (0x004828c0) for the common
// prompt_index < 0x26 range (loading the 1-based entry `message*5 + random +
// 1`); the 0xbb9 higher-index branch is unused by this dialog.
[[nodiscard]] std::optional<std::string>
LoadPromptVariant(std::int16_t random_index, std::uint16_t message_index) {
  return NovaHud_LoadStringEntry(
      kPromptStr, StringVariantIndex(random_index, message_index));
}

// Loads a STR# 0x96 button label with a literal fallback for missing
// resources.
[[nodiscard]] std::string LoadButtonLabel(std::uint16_t entry,
                                          std::string_view fallback) {
  return NovaHud_LoadStringEntry(kButtonLabelStr, entry)
      .value_or(std::string{fallback});
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

// Ghidra DrawContext_DrawGroupedUInt (see boarding_plunder.cpp): decimal
// digits grouped in threes with commas.
[[nodiscard]] std::string GroupedUInt(std::int32_t value) {
  const std::string digits = std::to_string(value);
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

// Uniform integer in [0, bound). Mirrors the game's NovaRandom_Range using the
// GameState PRNG so negotiation rolls are reproducible per session.
[[nodiscard]] std::int16_t NovaRandomRange(std::mt19937 &rng, int bound) {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::int16_t>(
      std::uniform_int_distribution<int>{0, bound - 1}(rng));
}

// Truncate toward zero, matching the original's x87 FIST + residual/sign
// correction (0x00480030 and the other travel-destination windows), not
// round-half-up.
[[nodiscard]] std::int32_t RoundDouble(double v) {
  return static_cast<std::int32_t>(v);
}

// ---- Drawing --------------------------------------------------------------

// Draws one button slot with the shared three-state art. The hovered slot
// draws the pressed art (NovaUi_DrawTravelDestinationPrimaryButtons passes
// the highlighted slot through to NovaUi_DrawThreeStateButton).
void DrawDialogButton(SdlPlatform &platform,
                      NovaFontCache &font_cache,
                      const ServicesButtonArt &button_art,
                      const SDL_FRect &rect,
                      std::string_view label,
                      bool enabled,
                      bool hovered) {
  const ButtonState state =
      !enabled ? ButtonState::kDisabled
               : (hovered ? ButtonState::kHover : ButtonState::kNormal);
  button_art.Draw(platform, rect, state);
  // Three-state button labels: white on the normal/hover art, 50% grey on the
  // disabled art, plain screen font (NovaUi_DrawThreeStateButton label table
  // DAT_007d8350).
  constexpr SDL_Color kLabel{255, 255, 255, 255};
  constexpr SDL_Color kLabelGrey{128, 128, 128, 255};
  NovaText_DrawCentered(platform,
                        font_cache,
                        kThreeStateButtonFontFamily,
                        kThreeStateButtonFontSize,
                        kNovaFontStyleRegular,
                        enabled ? kLabel : kLabelGrey,
                        rect.x,
                        rect.x + rect.w,
                        ThreeStateButtonLabelBaseline(rect),
                        label);
}

// Ghidra 0x004812c0 NovaUi_DrawTravelDestinationInteractionWindow.
// Draws the interaction-window frame over the live flight view: the 540x295
// backdrop PICT (DLOG 0x3f1) centred on the playfield, the three comm
// buttons, the status/prompt panel (DITL item 3: the original's fill +
// filled-rect text + InvertRect sequence nets to black fill with white
// wrapped text), the ambient stellar sprite thumbnail (item 4) and the
// header block (item 5: name, description, Status: word, left-aligned
// baselines at +12/+26/+42).
void DrawNegotiationDialog(SdlPlatform &platform,
                           const GameState &state,
                           SpaceflightView &view,
                           HudRenderer &hud,
                           NovaFontCache &font_cache,
                           const ServicesButtonArt &button_art,
                           SDL_Texture *backdrop,
                           SDL_Texture *planet_art,
                           const Stellar &stellar,
                           std::int16_t stellar_id,
                           const NegotiationFrame &frame,
                           int hovered_slot) {
  SDL_Renderer *renderer = platform.renderer();
  // Render the live game view beneath the window (the flight sim is paused,
  // so this redraws the same world each frame): the original draws its DLOG
  // over the unmodified gameplay surface.
  view.DrawGameFrame(platform, state, hud);
  platform.SetCenteredPlayfield();

  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &frame.window);
  } else {
    SDL_SetRenderDrawColor(
        renderer, kBlack.r, kBlack.g, kBlack.b, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &frame.window);
  }

  // Comm buttons (DITL items 0/1/2, bottom/top/middle).
  for (int slot = 0; slot < 3; ++slot) {
    const bool enabled = slot != 2 || frame.tribute_enabled;
    DrawDialogButton(platform,
                     font_cache,
                     button_art,
                     frame.buttons[slot],
                     frame.button_labels[slot],
                     enabled,
                     hovered_slot == slot);
  }

  // Status / prompt panel (DITL item 3, entry 4). The original fills the
  // inset rect white, word-wraps the status text into a white-filled inner
  // rect in black and then InvertRects the panel (0x004812c0 via
  // DrawContext_DrawPascalStringInFilledRect 0x004bcd30, DT_WORDBREAK);
  // fill and text colours cancel under the inversion, so the net look is a
  // black panel with white wrapped text, left-aligned from the inner top.
  SDL_FRect inner = frame.status_panel;
  inner.x += 1.0F;
  inner.y += 1.0F;
  inner.w -= 2.0F;
  inner.h -= 2.0F;
  SDL_SetRenderDrawColor(
      renderer, kBlack.r, kBlack.g, kBlack.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &inner);
  if (!frame.status.empty()) {
    inner.x += 4.0F;
    inner.y += 2.0F;
    inner.w -= 8.0F;
    inner.h -= 4.0F;
    const auto lines = WrapDescriptionLines(
        frame.status,
        static_cast<int>(std::max(1.0F, inner.w)),
        [&](std::string_view line) {
          return static_cast<int>(font_cache.TextWidth(
              NovaFontFamily::kGeneva, 12.0F, kNovaFontStyleRegular, line));
        });
    float baseline = inner.y + 12.0F;
    for (const std::string &line : lines) {
      if (baseline > frame.status_panel.y + frame.status_panel.h) {
        break;
      }
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    12.0F,
                    kNovaFontStyleRegular,
                    kWhite,
                    inner.x,
                    baseline,
                    line);
      baseline += 13.0F;
    }
  }

  // Destination picture (DITL item 4, entry 5): the target stellar's ambient
  // system sprite, blitted centred at native size in the panel (the original
  // looks the stellar up in the ambient sprite pool and centres its current
  // spin frame by half-span, clipped only to the window surface --
  // 0x004812c0). The port resolves the same frame through the shared spin-set
  // store (link_a_id + 1000) + the view's animation state; the docked-screen
  // planet PICT is a logged port-only fallback for missing spin sets.
  bool drew_sprite = false;
  const SpriteAsset *spin_set = view.sprite_store().Spin(
      renderer, static_cast<std::uint16_t>(stellar.link_a_id + 1000));
  if (spin_set != nullptr && !spin_set->frames.empty()) {
    const int frame_idx = std::clamp(
        view.StellarCurrentFrame(stellar_id), 0, spin_set->frame_count - 1);
    const SpriteFrame &sprite_frame =
        spin_set->frames[static_cast<std::size_t>(frame_idx)];
    if (sprite_frame.texture != nullptr) {
      float aw = 0.0F;
      float ah = 0.0F;
      SDL_GetTextureSize(sprite_frame.texture->get(), &aw, &ah);
      SDL_FRect dst{frame.image.x + (frame.image.w - aw) / 2.0F,
                    frame.image.y + (frame.image.h - ah) / 2.0F,
                    aw,
                    ah};
      SDL_RenderTexture(renderer, sprite_frame.texture->get(), nullptr, &dst);
      drew_sprite = true;
    }
  }
  if (!drew_sprite && planet_art != nullptr) {
    // Silent per frame; the fallback decision is logged once at window setup.
    float aw = 0.0F;
    float ah = 0.0F;
    SDL_GetTextureSize(planet_art, &aw, &ah);
    SDL_FRect dst = frame.image;
    if (aw > 0.0F && ah > 0.0F) {
      const float scale = std::min(frame.image.w / aw, frame.image.h / ah);
      dst.w = aw * scale;
      dst.h = ah * scale;
      dst.x = frame.image.x + (frame.image.w - dst.w) / 2.0F;
      dst.y = frame.image.y + (frame.image.h - dst.h) / 2.0F;
    }
    SDL_RenderTexture(renderer, planet_art, nullptr, &dst);
  } else if (!drew_sprite) {
    SDL_SetRenderDrawColor(renderer, 8, 24, 44, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &frame.image);
    SDL_SetRenderDrawColor(renderer, 80, 140, 190, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &frame.image);
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          11.0F,
                          kNovaFontStyleRegular,
                          kGrey,
                          frame.image.x,
                          frame.image.x + frame.image.w,
                          frame.image.y + frame.image.h / 2.0F,
                          "[no picture]");
  }

  // Header block (DITL item 5, entry 6): name (white, +12), destination
  // description (grey, +26), Status: label + word (+42), all left-aligned.
  const float left = frame.header.x;
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                kWhite,
                left,
                frame.header.y + 12.0F,
                frame.name);
  if (!frame.description.empty()) {
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kGrey,
                  left,
                  frame.header.y + 26.0F,
                  frame.description);
  }
  if (!frame.status_label.empty()) {
    const float label_width = font_cache.TextWidth(NovaFontFamily::kGeneva,
                                                   12.0F,
                                                   kNovaFontStyleRegular,
                                                   frame.status_label + " ");
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kLightGrey,
                  left,
                  frame.header.y + 42.0F,
                  frame.status_label);
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  frame.status_word_color,
                  left + label_width,
                  frame.header.y + 42.0F,
                  frame.status_word);
  }
}

// Ghidra 0x004826a0 NovaUi_DrawTravelDestinationPaymentWindow: the DLOG 0x3f0
// payment modal over the still-rendered interaction window. Prompt band
// (DITL item 2): "<I'll pay you> <grouped amount> <credit(s)>." in white;
// buttons Accept Price (item 0, bottom) / Lower Price (item 1, top).
void DrawPaymentWindow(SdlPlatform &platform,
                       NovaFontCache &font_cache,
                       const ServicesButtonArt &button_art,
                       SDL_Texture *backdrop,
                       const PaymentFrame &frame,
                       std::int32_t payment_amount,
                       int hovered_slot) {
  SDL_Renderer *renderer = platform.renderer();
  if (backdrop != nullptr) {
    SDL_RenderTexture(renderer, backdrop, nullptr, &frame.window);
  } else {
    SDL_SetRenderDrawColor(
        renderer, kBlack.r, kBlack.g, kBlack.b, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &frame.window);
  }

  // Prompt band (DITL item 2, entry 3): white left-aligned text, cursor at
  // +4/+12 like the original.
  const std::string credit_word =
      NovaHud_LoadStringEntry(kMiscStr,
                              payment_amount < 2 ? kMiscCredit : kMiscCredits)
          .value_or(payment_amount < 2 ? "credit" : "credits");
  const std::string prompt =
      NovaHud_LoadStringEntry(kMiscStr, kMiscPayYou).value_or("I'll pay you") +
      " " + GroupedUInt(payment_amount) + " " + credit_word + ".";
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                kWhite,
                frame.text_band.x + 4.0F,
                frame.text_band.y + 12.0F,
                prompt);

  DrawDialogButton(platform,
                   font_cache,
                   button_art,
                   frame.buttons[0],
                   LoadButtonLabel(kBtnAcceptPrice, "Accept Price"),
                   true,
                   hovered_slot == 0);
  DrawDialogButton(platform,
                   font_cache,
                   button_art,
                   frame.buttons[1],
                   LoadButtonLabel(kBtnLowerPrice, "Lower Price"),
                   true,
                   hovered_slot == 1);
}

// Outcome of the payment modal (NovaUi_RunTravelDestinationPaymentWindow
// 0x00482280): kPaid (Accept Price, or a granted Lower Price discount),
// kRefused (a declined Lower Price roll) or kClosed (Esc; the caller treats
// it like a refusal).
enum class PaymentResult { kPaid, kRefused, kClosed };

// Runs the DLOG 0x3f0 payment modal for the bribe path. `draw_underneath`
// re-renders the flight view + interaction window each frame (the boarding
// capture-decision dialog's keep-rendering pattern). The acceptance roll is
// made once when the window opens; a granted "Lower Price" discounts the
// payment amount by the bribe-mode factor (0.75x, DAT_005758f0) rounded to
// /100 (DAT_00575898) and leaves the window open on the new figure.
[[nodiscard]] PaymentResult
RunBribePaymentWindow(SdlPlatform &platform,
                      GameState &state,
                      NovaFontCache &font_cache,
                      const ServicesButtonArt &button_art,
                      SDL_Texture *backdrop,
                      const std::function<void()> &draw_underneath,
                      std::int32_t &payment_amount) {
  const PaymentFrame frame = LoadPaymentGeometry();
  // The window-open roll: "Lower Price" is granted when rand(100) <= 0x23.
  bool lower_granted = NovaRandomRange(state.rng, 100) <= kPaymentChancePercent;
  // The original's sVar4 result latch: once "Lower Price" is granted the
  // window stays open on the discounted figure and any later exit returns
  // "paid"; Esc before any decision returns "closed".
  bool paid = false;

  // NovaInputQueue_FlushAllCommands: the original discards pending input when
  // the window opens, so the confirming click can't double-dispatch.
  while (platform.PollTextEvent().has_value()) {
  }

  while (!platform.quit_requested()) {
    int hovered = -1;
    const SDL_FPoint mouse = platform.mouse_position();
    for (int slot = 0; slot < 2; ++slot) {
      const SDL_FRect rect = frame.buttons[slot];
      if (SDL_PointInRectFloat(&mouse, &rect)) {
        hovered = slot;
        break;
      }
    }

    draw_underneath();
    DrawPaymentWindow(platform,
                      font_cache,
                      button_art,
                      backdrop,
                      frame,
                      payment_amount,
                      hovered);
    platform.Present();

    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      if (in->key == TextKey::escape || in->key == TextKey::enter) {
        return paid ? PaymentResult::kPaid : PaymentResult::kClosed;
      }
      if (in->key != TextKey::primary) {
        continue;
      }
      const SDL_FPoint click = platform.mouse_position();
      if (SDL_PointInRectFloat(&click, &frame.buttons[0])) {
        // Accept Price: pay the offered (possibly already discounted) amount.
        return PaymentResult::kPaid;
      }
      if (SDL_PointInRectFloat(&click, &frame.buttons[1])) {
        // Lower Price: the window-open roll decides. Granted -> discount
        // 0.75x, round to /100, keep the window open on the new figure (the
        // latch clears, so a second Lower Price is refused).
        if (!lower_granted) {
          return PaymentResult::kRefused;
        }
        payment_amount = RoundDouble(static_cast<double>(payment_amount) *
                                     kPaymentBribeScale);
        payment_amount = RoundDouble(static_cast<double>(payment_amount) *
                                     kPaymentRoundFactor) *
                         100;
        lower_granted = false;
        paid = true;
      }
    }
    SDL_Delay(16);
  }
  return paid ? PaymentResult::kPaid : PaymentResult::kClosed;
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
  // decompile `g_travel_bribe_cost = NovaRandom_Range(credits*1e-06) * 1000
  // + 3000` (no +1; NovaRandom_Range is [0, bound) -- cf. boarding panic
  // `rand(0x1a) + 0xf` = 15..40). Drawn from the same GameState PRNG so the
  // price is reproducible per session.
  std::int32_t upper = RoundDouble(
      std::max(1.0,
               static_cast<double>(std::max(std::int32_t{1}, credits)) *
                   kCreditsBribeFraction));
  std::int64_t cost =
      static_cast<std::int64_t>(NovaRandomRange(rng, upper)) * kBribeGrouping +
      kBribeBaseAdd;

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
// Ghidra 0x00480030 NovaUi_RunTravelDestinationInteractionWindow.
NegotiationExit NovaNegotiation_RunDestinationDialog(SdlPlatform &platform,
                                                     GameState &state,
                                                     std::int16_t stellar_id,
                                                     SpaceflightView &view,
                                                     HudRenderer &hud) {
  NovaLog::Info("opening destination-interaction dialog 0x3f1 for stellar {}",
                static_cast<int>(stellar_id));

  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr) {
    NovaLog::Warn("target stellar {} not in the scenario; cancelling the "
                  "interaction dialog",
                  static_cast<int>(stellar_id));
    return NegotiationExit::kClosed;
  }

  // Item 1: per-launch random flavour index (g_travel_interaction_random_index)
  // and the bribe-offer latch (g_travel_interaction_bribe_random_latch), both
  // from the GameState PRNG so they are reproducible per session. The
  // original's latch persists across windows; the port re-rolls per run
  // (TODO(decomp): latch persistence divergence).
  const std::int16_t random_index = NovaRandomRange(state.rng, 5);
  const int bribe_random_latch = NovaRandomRange(state.rng, 100);

  // Denied latch (g_travel_interaction_denied_state): the stellar's
  // reputation_threshold gates landing against the CURRENT system's
  // reputation. 0x7fff means "always denied"; -0x7fff "never".
  const std::int16_t sys_index =
      state.player.current_system_id >= 0 &&
              state.player.current_system_id <
                  static_cast<std::int16_t>(state.system_reputation.size())
          ? state.player.current_system_id
          : -1;
  const std::int16_t sys_rep =
      sys_index >= 0
          ? state.system_reputation[static_cast<std::size_t>(sys_index)]
          : 0;
  bool denied =
      (stellar->min_status == 0x7fff) ||
      (stellar->min_status != -0x7fff && sys_rep < stellar->min_status);
  const Government *gov =
      stellar->government_id != -1
          ? state.scenario.Government(
                static_cast<std::int16_t>(stellar->government_id + 0x80))
          : nullptr;
  if (denied && gov != nullptr) {
    // Government policy-flag override (Government_GetGovernmentPolicyFlag
    // index 1): a set flag clears the denial. The GovtDef +0x83 byte gate
    // (field_0x83 != 0 -> not denied) is not modelled (no clean-room field;
    // TODO(decomp)).
    if (NovaGovernment_GetPolicyFlag(
            state.scenario, stellar->government_id, 1)) {
      denied = false;
    }
  }

  // Bribe-offer latch: the per-launch roll (> 0x1e) admits an offer for a
  // faction-less stellar or one whose government takes bribes (0x4000); the
  // "bribes the player" flag (0x8000) forces an offer regardless.
  bool bribe_offered = false;
  if (bribe_random_latch > kBribeLatchCutoff) {
    if (gov == nullptr || (gov->flags_primary & kGovtFlagBribable) != 0U) {
      bribe_offered = true;
    }
  }
  if (gov != nullptr && (gov->flags_primary & kGovtFlagBribeCostly) != 0U) {
    bribe_offered = true;
  }

  // Resolve the bribe cost (credits-scaled, random, clamped) with the
  // government's 1.5x scale applied when the costly-bribe flag is set.
  std::int32_t bribe_cost = NovaNegotiation_ComputeBribeCost(
      state.rng, state.player.credits, stellar->government_id);
  if (gov != nullptr && (gov->flags_primary & kGovtFlagBribeCostly) != 0U) {
    bribe_cost =
        RoundDouble(static_cast<double>(bribe_cost) * kGovtBribeCostMultiplier);
  }

  // ---- Window content --------------------------------------------------
  NegotiationFrame frame;
  frame.window = {std::truncf((640.0F - kNegotiationFrameWidth) * 0.5F),
                  std::truncf((480.0F - kNegotiationFrameHeight) * 0.5F),
                  kNegotiationFrameWidth,
                  kNegotiationFrameHeight};
  for (int i = 0; i < 3; ++i) {
    frame.buttons[i] =
        OffsetRect(kFallbackButtonRects[i], {frame.window.x, frame.window.y});
  }
  frame.status_panel =
      OffsetRect(kFallbackStatusRect, {frame.window.x, frame.window.y});
  frame.image =
      OffsetRect(kFallbackImageRect, {frame.window.x, frame.window.y});
  frame.header =
      OffsetRect(kFallbackHeaderRect, {frame.window.x, frame.window.y});
  if (!LoadNegotiationGeometry(frame.buttons,
                               frame.status_panel,
                               frame.image,
                               frame.header,
                               frame.window)) {
    NovaLog::Todo("DLOG/DITL 0x3f1 unavailable; using the shipped-geometry "
                  "fallback for the destination-interaction window");
  }

  frame.tribute_enabled =
      !(stellar->hazard_marker && (stellar->availability_flags & 0x20) != 0U);
  frame.button_labels[0] = LoadButtonLabel(kBtnCloseChannel, "Close Channel");
  frame.button_labels[1] = denied
                               ? LoadButtonLabel(kBtnOfferBribe, "Offer Bribe")
                               : LoadButtonLabel(kBtnGreetings, "Greetings");
  frame.button_labels[2] =
      stellar->hazard_marker
          ? LoadButtonLabel(kBtnRelease, "Release")
          : LoadButtonLabel(kBtnDemandTribute, "Demand Tribute");
  frame.name = stellar->name;

  // Destination description (0x00480030): the desc resource keyed at
  // link_a_id + 7000, falling back to the shipped stellar-class fragment
  // STR# 0x44c at link_a_id + 1.
  if (const auto desc = NovaResource_LoadDescription(
          static_cast<std::uint16_t>(stellar->link_a_id + 7000));
      desc && !desc->text.empty()) {
    frame.description = desc->text;
  } else {
    frame.description = NovaHud_LoadStringEntry(
                            kStellarClassStr,
                            static_cast<std::uint16_t>(stellar->link_a_id + 1))
                            .value_or("");
  }

  // Header Status: word. A normal landable stellar draws no Status: line;
  // uninhabited / dominated / denied bodies draw "Status:" plus a colored
  // word ("Hostile" goes red once the system reputation has gone negative).
  if ((stellar->flags & 0x20) != 0U) {
    frame.status_word = NovaHud_LoadStringEntry(kMiscStr, kMiscUninhabited)
                            .value_or("Uninhabited");
    frame.status_word_color = kLightGrey; // SHORT_ARRAY_00733b50
  } else if (stellar->hazard_marker) {
    frame.status_word =
        NovaHud_LoadStringEntry(kMiscStr,
                                (stellar->availability_flags & 0x20) != 0U
                                    ? kMiscOwned
                                    : kMiscDominated)
            .value_or("Dominated");
    // SHORT_ARRAY_00733b32 is runtime-initialized (unreadable .bss).
    frame.status_word_color = kStatusOrange;
  } else if (denied) {
    if (sys_rep < 0) {
      frame.status_word =
          NovaHud_LoadStringEntry(kMiscStr, kMiscHostile).value_or("Hostile");
      frame.status_word_color = kRed;
    } else {
      frame.status_word = NovaHud_LoadStringEntry(kMiscStr, kMiscForbidden)
                              .value_or("Forbidden");
      frame.status_word_color = kStatusOrange;
    }
  }
  if (!frame.status_word.empty()) {
    frame.status_label =
        NovaHud_LoadStringEntry(kMiscStr, kMiscStatusLabel).value_or("Status:");
  }

  // Initial status text (the branch ladder before the modal loop): welcome +
  // name when landing is open (or already dominated), the hostile prompt when
  // denied, "No response." for uninhabited bodies.
  if ((stellar->flags & 0x20) == 0U) {
    if (!denied || stellar->hazard_marker) {
      frame.status = LoadStatusVariant(random_index, kMsgWelcomeStatus)
                         .value_or("Communications channel open to ") +
                     stellar->name + ".";
    } else {
      frame.status = LoadPromptVariant(random_index, kMsgDeniedPrompt)
                         .value_or("What is it you want?");
    }
  } else {
    frame.status = LoadPromptVariant(random_index, kMsgNoResponse)
                       .value_or("No response.");
  }

  // ---- Assets -----------------------------------------------------------
  auto backdrop = LoadPictTexture(platform, kNegotiationBackdropPict);
  if (!backdrop) {
    NovaLog::Todo("destination-interaction backdrop PICT 0x2140 unavailable; "
                  "drawing a flat background");
  }
  auto payment_backdrop = LoadPictTexture(platform, kPaymentBackdropPict);
  if (!payment_backdrop) {
    NovaLog::Todo("payment backdrop PICT 0x2142 unavailable; drawing a flat "
                  "background");
  }

  // The destination planet picture shown in the DITL item-4 image frame (the
  // original scales the target stellar's ambient sprite into it). The planet
  // PICT comes from the same selection the docked screen uses: the stellar's
  // custom picture (engage_highlight_frame >= 0x80) or link_a_id + 0x2710.
  std::unique_ptr<SdlTexture> planet_art;
  const std::int16_t stell_pict =
      (stellar->engage_highlight_frame >= 0x80)
          ? stellar->engage_highlight_frame
          : static_cast<std::int16_t>(stellar->link_a_id + 0x2710);
  if (stell_pict >= 0x80) {
    planet_art =
        LoadPictTexture(platform, static_cast<std::uint16_t>(stell_pict));
    if (planet_art) {
      // The picture panel shows the stellar's ambient spin sprite (0x004812c0);
      // this PICT is only the port's fallback when the spin set fails to load.
      if (view.sprite_store().Spin(platform.renderer(),
                                   static_cast<std::uint16_t>(
                                       stellar->link_a_id + 1000)) == nullptr) {
        NovaLog::Todo("destination-interaction: stellar '{}' has no spin set "
                      "{}; the item-4 thumbnail falls back to the docked "
                      "planet PICT 0x{} (port-only divergence)",
                      stellar->name,
                      stellar->link_a_id + 1000,
                      static_cast<int>(stell_pict));
      }
    } else {
      NovaLog::Todo("no negotiation destination PICT {} for stellar '{}'; "
                    "the image frame stays a flat placeholder",
                    static_cast<int>(stell_pict),
                    stellar->name);
    }
  }

  ServicesButtonArt button_art;
  if (!button_art.Initialize(platform)) {
    NovaLog::Warn("three-state button art unavailable for the negotiation "
                  "dialog buttons");
  }
  NovaFontCache font_cache;
  platform.SetCenteredPlayfield();

  std::vector<ServiceButton> buttons;
  buttons.reserve(3);
  for (std::uint8_t i = 0; i < 3; ++i) {
    buttons.push_back(ServiceButton{frame.buttons[i], i});
  }

  // One full frame of the interaction window + present; the payment window
  // re-renders this beneath itself every frame (boarding keep-rendering
  // pattern).
  const auto draw_dialog = [&]() {
    DrawNegotiationDialog(platform,
                          state,
                          view,
                          hud,
                          font_cache,
                          button_art,
                          backdrop ? backdrop->get() : nullptr,
                          planet_art ? planet_art->get() : nullptr,
                          *stellar,
                          stellar_id,
                          frame,
                          -1);
    platform.Present();
  };

  // The top button's action (NovaUi_PollTravelScriptAction ordinal 2 = DITL
  // item 1): Greetings when landing is open, the bribe ladder when denied.
  bool proceed_to_land = false;
  const auto run_comm_action = [&]() {
    if (!denied || stellar->hazard_marker) {
      if (!denied) {
        // Greetings: the greeting-response flavour prompt (msg 9).
        frame.status = LoadPromptVariant(random_index, kMsgGreetingResponse)
                           .value_or(frame.status);
      } else {
        // Dominated target: rude dismissal (status msg 4).
        frame.status = LoadStatusVariant(random_index, kMsgCannotAfford)
                           .value_or(frame.status);
      }
      return;
    }
    if (!bribe_offered) {
      // Offering a bribe with none on the table: refusal (status msg 9).
      frame.status = LoadStatusVariant(random_index, kMsgRefuseNoBribe)
                         .value_or(frame.status);
      return;
    }
    // Offer Bribe: the offer line, then the DLOG 0x3f0 payment window.
    if (auto s = LoadStatusVariant(random_index, kMsgBribeOffered)) {
      frame.status = *s;
    }
    std::int32_t payment_amount = bribe_cost;
    const PaymentResult result = RunBribePaymentWindow(
        platform,
        state,
        font_cache,
        button_art,
        payment_backdrop ? payment_backdrop->get() : nullptr,
        draw_dialog,
        payment_amount);
    if (state.player.credits < payment_amount) {
      frame.status = LoadStatusVariant(random_index, kMsgCannotAfford)
                         .value_or(frame.status);
    } else if (result == PaymentResult::kPaid) {
      // The original's HUD overlay composes "<name>, you're cleared to
      // dock/land. Commence final approach." (STR# 0x7d2 0x5f/0x62 + 100)
      // and sets the travel handoff directly.
      const std::string cleared =
          NovaHud_LoadStringEntry(kMiscStr,
                                  (stellar->flags & 0x10) != 0U
                                      ? kMiscClearedToDock
                                      : kMiscClearedToLand)
              .value_or("you're cleared to land.");
      const std::string approach =
          NovaHud_LoadStringEntry(kMiscStr, kMiscFinalApproach)
              .value_or("Commence final approach.");
      NovaHud_ShowOverlayMessage(state,
                                 stellar->name + ", " + cleared + " " +
                                     approach,
                                 0xe0,
                                 0xe0,
                                 0xe0,
                                 250);
      state.player.credits -= payment_amount;
      proceed_to_land = true;
    } else {
      // Refused / closed: the haggle -- +1000, latch reset (offer stays
      // available per the original's bribe_offered = false), status msg 6.
      bribe_offered = false;
      bribe_cost += kHaggleIncrement;
      frame.status = LoadStatusVariant(random_index, kMsgBribeDeclined)
                         .value_or(frame.status);
    }
  };

  // NovaInputQueue_FlushAllCommands: the original discards pending input when
  // the window opens (called five times in 0x00480030).
  while (platform.PollTextEvent().has_value()) {
  }

  while (!platform.quit_requested()) {
    // Mouse-hover highlight over enabled slots (the original redraws with the
    // hovered slot as the pressed button).
    int hovered = -1;
    const SDL_FPoint mouse = platform.mouse_position();
    for (int slot = 0; slot < 3; ++slot) {
      if ((slot != 2 || frame.tribute_enabled) &&
          SDL_PointInRectFloat(&mouse, &frame.buttons[slot])) {
        hovered = slot;
        break;
      }
    }
    DrawNegotiationDialog(platform,
                          state,
                          view,
                          hud,
                          font_cache,
                          button_art,
                          backdrop ? backdrop->get() : nullptr,
                          planet_art ? planet_art->get() : nullptr,
                          *stellar,
                          stellar_id,
                          frame,
                          hovered);
    platform.Present();

    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      switch (in->key) {
      case TextKey::escape:
      case TextKey::enter:
        // Close Channel (the original's action-1 path).
        return NegotiationExit::kClosed;
      case TextKey::primary: {
        const auto clicked =
            ServiceButtonAt(buttons, platform.mouse_position());
        if (!clicked) {
          break;
        }
        switch (*clicked) {
        case 0:
          return NegotiationExit::kClosed;
        case 1:
          run_comm_action();
          break;
        case 2:
          if (frame.tribute_enabled) {
            // Demand Tribute / Release (action 3) is deferred: domination
            // latch, defense-fleet spawns, faction combat event and reaction
            // scripts (see the file-head note).
            NovaLog::Todo("target-action: Demand Tribute / Release branch of "
                          "NovaUi_RunTravelDestinationInteractionWindow is not "
                          "reconstructed");
          }
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

    if (proceed_to_land) {
      // Hand off to the Spaceport (DLOG 0x3e8) by staging the target stellar,
      // exactly as the original's bribe path sets g_travel_selected_stellar_id.
      state.travel.selected_stellar_id = stellar_id;
      return NegotiationExit::kProceedToLand;
    }
    SDL_Delay(16);
  }
  return NegotiationExit::kQuit;
}

} // namespace game
