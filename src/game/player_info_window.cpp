#include "player_info_window.hpp"

#include "../log.hpp"
#include "../util/format.hpp"
#include "boarding_plunder.hpp"
#include "brgr_archive.hpp"
#include "command_input.hpp"
#include "game_state.hpp"
#include "hud_overlay.hpp"
#include "mission.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "pict_image.hpp"
#include "pict_texture.hpp"
#include "scenario_data.hpp"
#include "services_buttons.hpp"
#include "ship_ai.hpp"
#include "starmap.hpp"
#include "travel.hpp"
#include "ui_dialog.hpp"
#include "weapon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace game {

using evnova::util::GroupThousands;

namespace {

// ---------------------------------------------------------------------------
// Resources / constants (docs/player_info_window.md)
// ---------------------------------------------------------------------------

constexpr std::uint16_t kDialogId = 0x3f9; // DLOG/DITL 0x3f9
// Backdrop PICTs: top slice, stretchable middle tile, bottom slice
// (Ghidra DAT_007d4be0/.4/.8).
constexpr std::uint16_t kBackdropTopPict = 0x2146;
constexpr std::uint16_t kBackdropMiddlePict = 0x2147;
constexpr std::uint16_t kBackdropBottomPict = 0x2148;

constexpr std::uint16_t kMiscStr = 0x7d2;   // STR# 2002 misc strings
constexpr std::uint16_t kTitlesStr = 0x96;  // STR# 150 button labels
constexpr std::uint16_t kRatingsStr = 0x8a; // STR# 138 combat ratings

// Tab strip: STR# 0x96 entries per strip index (Ghidra icon/entry table
// DAT_007d830e = {4, 0x23, 0x24, 0x25, 0x26, 0x3c} indexing the shared
// label pool). Index 5 (entry 7) is the cargo-page Jettison button.
constexpr std::uint16_t kTabLabelEntries[6] = {
    0x04, 0x23, 0x24, 0x25, 0x26, 0x3c};
constexpr int kTabClose = 0;
constexpr int kTabJettison = 5;

// STR# 0x7d2 fragments used on the pages.
enum MiscString : std::uint16_t {
  kStrShieldStatusLabel = 0x0c, // "Shield Status:"
  kStrShieldsDown = 0x0f,       // "Shields Down"
  kStrArmorStatusLabel = 0x11,  // "Armor Status:"
  kStrEnergyStatusLabel = 0x08, // "Energy Status:"
  kStrManeuveringEnergy = 0x09, // "maneuvering energy"
  kStrCreditsWord = 0x21,       // "credits"
  kStrJump = 0xef,              // "jump"
  kStrJumps = 0xf0,             // "jumps"
  kStrPilotName = 0xfb,         // "Pilot Name:"
  kStrCurrentDate = 0xfc,       // "Current Date:"
  kStrSystem = 0xfd,            // "System:"
  kStrCombatRating = 0xfe,      // "Combat Rating:"
  kStrShipName = 0xff,          // "Ship Name:"
  kStrShipClass = 0x100,        // "Ship Class:"
  kStrTurnRate = 0x101,         // "Turn Rate:"
  kStrDegPerSec = 0x102,        // "<deg>/sec"
  kStrAccelRate = 0x103,        // "Accel Rate:"
  kStrMaxSpeed = 0x104,         // "Max Speed:"
  kStrFailed = 0x105,           // "Failed"
  kStrPlus = 0x106,             // "plus"
  kStrOnly = 0x107,             // "only"
  kStrPerDay = 0x10b,           // "per day" (key-settings hint row tail)
  kStrNoFleetCargo =
      0x10c, // "You don't have any cargo aboard your fleet's ships"
  kStrNoShipCargo = 0x10d,  // "You don't have any cargo aboard your ship"
  kStrNoExtras = 0x10e,     // "You don't have any extras on your ship"
  kStrNoHonors = 0x10f,     // "You don't have any ranks or honors"
  kStrExtrasHeader = 0x110, // "Current extras for your ship:"
  kStrTradeInValue = 0x111, // "Ship trade-in value:"
  kStrHonorsHeader = 0x112, // "Your ranks and honors:"
  kStrLegalStatus = 0x146,  // "Legal Status:"
  kStrNone = 0x14f,         // "None"
  kStrNa = 0x18c,           // "N/A"
  kStrA = 0x189,            // "a"
  kStrAn = 0x18a,           // "an"
  kStrAnd = 0x188,          // "and"
  kStrJettisonConfirm =
      0x123, // "Are you sure you want to jettison your cargo?"
};

// DITL entries (1-based) of DLOG 0x3f9: 1..5 + 7 are the tab-strip buttons,
// 6 is the scrolling text view.
constexpr int kTabStripDitl[6] = {1, 2, 3, 4, 5, 7};
constexpr std::size_t kTextViewDitl = 6;

// Dialog colours: labels DAT_00733b50, values PTR_DAT_00575ad8, window fill
// PTR_DAT_00575acc (same triples the boarding window cites).
constexpr SDL_Color kLabelColor{192, 192, 192, 255};
constexpr SDL_Color kValueColor{255, 255, 255, 255};
constexpr SDL_Color kWindowFill{16, 40, 72, 255};

// Page-1 grid metrics from the decompile: labels at +5 (left) / +0xcd
// (right), values at +0x50 / +0x118, first row 12px below the view-rect top,
// 16px per row. Every offset is relative to the DITL entry-6 text-view rect
// (the decompile's sStack_26/local_28 are entry 6's left/top fed through
// SetCursorPosAdjusted, which adds the window origin).
constexpr float kGridLeftLabelX = 5.0F;
constexpr float kGridLeftValueX = 0x50;
constexpr float kGridRightLabelX = 0xcd;
constexpr float kGridRightValueX = 0x118;
constexpr float kGridFirstRowDy = 0xc;
constexpr float kGridRowStride = 0x10;

// Ghidra doubles/floats feeding the page-1 displays (all read from .data):
// turn: Ship_ComputeShipMaxTurnRateDeg (deg/frame) * DAT_005759b8 (30) ->
// deg/sec. The port's turn_raw is the raw resource Maneuver whose deg/frame
// value is turn_raw * 0.1 (the flight loop's convention), so display =
// turn_raw * 3.
constexpr double kTurnRatePerSec = 3.0;
// thrust: Ship_ComputeShipEffectiveThrust (px/frame^2) * DAT_005759c0
// (2500). The port's thrust_raw is the raw resource accel (px/frame^2 =
// /10000), so display = thrust_raw * 0.25.
constexpr double kThrustDisplay = 0.25;
// speed: Ship_ComputeShipEffectiveMaxSpeed (px/frame) * DAT_005759c8 (100)
// [* DAT_005759d0 (2/3) when the pilot is not strict-play]. The port's
// speed_raw is the raw resource speed (px/frame = /100), so display =
// speed_raw [* 2/3].
constexpr double kSpeedNoStrictScale = 2.0 / 3.0; // DAT_005759d0
constexpr double kPercentScale = 100.0;           // DAT_00575958
constexpr float kEmptyThreshold = 0.0078125F;     // DAT_00575990 (1/128)

// Original key codes (DIK) for the dispatch callback's keyboard arms.
constexpr std::uint16_t kKeyCodeTab = 0x0f;
constexpr std::uint16_t kKeyCodeEnter = 0x1c;
constexpr std::uint16_t kKeyCodeEscape = 0x01;

std::string MiscString(std::uint16_t entry_1based, std::string_view fallback) {
  if (auto s = NovaHud_LoadStringEntry(kMiscStr, entry_1based)) {
    // The pool stays MacRoman here; the font layer converts to UTF-8 at draw
    // time (including the 0xA1 degree sign in the turn-rate fragment).
    return *s;
  }
  NovaLog::Todo("player-info: missing STR# 0x7d2 entry 0x{:x}", entry_1based);
  return std::string(fallback);
}

// Honors list glue from 0x0049c050. The original prints the rank names first
// (each followed by "," when `printed <= total-1` and " and " (STR# 0x7d2
// 0x188) only at `printed == total-1`), then the outfit entries (each followed
// by "," when `printed <= total-1`, never "and"). `total` is the combined
// entry count, so the "and" lands before the final entry only when that entry
// is a rank; a lone rank+outfit pair renders "A, and B" (a quirk kept here).
std::string JoinHonorsEntries(const std::vector<std::string> &rank_names,
                              const std::vector<std::string> &outfits) {
  std::string out;
  const std::size_t total = rank_names.size() + outfits.size();
  std::size_t printed = 0;
  for (const std::string &name : rank_names) {
    out += name;
    ++printed;
    if (printed <= total - 1) {
      out += ",";
    }
    if (total - 1 <= printed) {
      if (printed != total - 1) {
        continue;
      }
      out += " ";
      out += MiscString(kStrAnd, "and");
    }
    out += " ";
  }
  for (const std::string &entry : outfits) {
    out += entry;
    ++printed;
    if (printed <= total - 1) {
      out += ", ";
    }
  }
  return out;
}

// Extras list glue (0x0049c050): comma only when there are 3+ entries (two
// items render "A and B", no Oxford comma), " and " before the final entry.
std::string JoinExtrasEntries(const std::vector<std::string> &entries) {
  std::string out;
  const std::size_t total = entries.size();
  for (std::size_t i = 0; i < total; ++i) {
    out += entries[i];
    const std::size_t printed = i + 1;
    if (printed <= total - 1 && total > 2) {
      out += ",";
    }
    if (printed < total - 1) {
      out += " ";
    } else if (printed == total - 1) {
      out += " ";
      out += MiscString(kStrAnd, "and");
      out += " ";
    }
  }
  return out;
}

// One grouped outfit entry for the Extras/Honors lists. `index` is the
// similar_to representative (first same-LCName outfit) and `count` the summed
// owned total.
struct OutfitSummaryGroup {
  std::size_t index = 0;
  std::int16_t count = 0;
};

// Builds the Extras (rank_outfits=false) or Honors (rank_outfits=true) outfit
// groups (0x0049c050): owned outfits with a non-empty LCName, filtered by the
// 0x2000 flag, grouped via similar_to, ordered by the representative's
// DisplayWeight descending (stable, ascending slot on ties; the original scans
// slots ascending and emits the current weight tier in one pass).
std::vector<OutfitSummaryGroup>
CollectOutfitSummaryGroups(const GameState &state, bool rank_outfits) {
  std::array<std::int16_t, 0x200> counts{};
  const std::size_t outfit_count = std::min(
      state.inventory.outfit_owned_count.size(), state.scenario.outfits.size());
  for (std::size_t i = 0; i < outfit_count; ++i) {
    const std::int16_t owned = state.inventory.outfit_owned_count[i];
    if (owned <= 0) {
      continue;
    }
    const Outfit &outfit = state.scenario.outfits[i];
    if (outfit.lc_name.empty()) {
      continue;
    }
    if (((outfit.flags & 0x2000U) != 0U) != rank_outfits) {
      continue;
    }
    std::size_t representative = i;
    if (outfit.similar_to >= 0 &&
        static_cast<std::size_t>(outfit.similar_to) < i) {
      representative = static_cast<std::size_t>(outfit.similar_to);
    }
    counts[representative] =
        static_cast<std::int16_t>(counts[representative] + owned);
  }
  std::vector<OutfitSummaryGroup> groups;
  for (std::size_t i = 0; i < counts.size(); ++i) {
    if (counts[i] > 0) {
      groups.push_back(OutfitSummaryGroup{i, counts[i]});
    }
  }
  std::stable_sort(
      groups.begin(),
      groups.end(),
      [&](const OutfitSummaryGroup &a, const OutfitSummaryGroup &b) {
        return state.scenario.outfits[a.index].display_weight >
               state.scenario.outfits[b.index].display_weight;
      });
  return groups;
}

// Count word shared by the Extras and Honors lists (0x0049c050): 1 -> "a/an"
// (vowel test on the LCName first letter, MWRuntime_FUN_004d6230), 2/3 ->
// STR# 0x89 0x1e/0x1f ("two"/"three"), 4+ -> decimal digits.
std::string OutfitListCountWord(std::int16_t count, std::string_view lc_name) {
  if (count == 1) {
    const char first = !lc_name.empty() ? lc_name.front() : '\0';
    const bool vowel = first == 'a' || first == 'e' || first == 'i' ||
                       first == 'o' || first == 'u';
    return MiscString(vowel ? kStrAn : kStrA, "a");
  }
  if (count == 2 || count == 3) {
    if (auto word = NovaHud_LoadStringEntry(
            0x89, static_cast<std::uint16_t>(count + 0x1c))) {
      return *word;
    }
  }
  return std::to_string(count);
}

std::string FormatOutfitSummaryGroup(const GameState &state,
                                     const OutfitSummaryGroup &group) {
  const Outfit &outfit = state.scenario.outfits[group.index];
  return OutfitListCountWord(group.count, outfit.lc_name) + " " +
         (group.count == 1 ? outfit.lc_name : outfit.lc_plural);
}

// The strip tables (DAT_007d830e etc.) store 0-based STR# 0x96 indices (the
// ship-comm window's 0x14 "Close Channel" confirms the convention);
// NovaHud_LoadStringEntry is 1-based.
std::string TitleString(std::uint16_t pool_index) {
  if (auto s = NovaHud_LoadStringEntry(
          kTitlesStr, static_cast<std::uint16_t>(pool_index + 1))) {
    return *s;
  }
  return "?";
}

int RoundToInt(float v) {
  return static_cast<int>(v < 0.0F ? v - 0.5F : v + 0.5F);
}

// Word-wrap for the page texts, using the same Geneva-9 metrics as the
// original's text-view measurement (NovaTextView_UpdateContentHeight
// 0x004bce10 wraps at the content-rect width; line height 11, first-line
// offset 11 + trailing 6 -> see selection_text_dialog.cpp).
std::vector<std::string>
WrapText(NovaFontCache &fonts, std::string_view text, float max_width) {
  std::vector<std::string> lines;
  std::string current;
  std::size_t pos = 0;
  const auto too_wide = [&](const std::string &candidate) {
    return fonts.TextWidth(NovaFontFamily::kGeneva,
                           9.0F,
                           kNovaFontStyleRegular,
                           candidate) > max_width;
  };
  while (pos <= text.size()) {
    // Split off one paragraph line (\r-terminated in the original texts).
    std::size_t end = text.find('\r', pos);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view paragraph = text.substr(pos, end - pos);
    current.clear();
    std::size_t word_pos = 0;
    while (word_pos <= paragraph.size()) {
      std::size_t space = paragraph.find(' ', word_pos);
      if (space == std::string_view::npos) {
        space = paragraph.size();
      }
      std::string_view word = paragraph.substr(word_pos, space - word_pos);
      std::string candidate = current.empty()
                                  ? std::string(word)
                                  : current + " " + std::string(word);
      if (!current.empty() && too_wide(candidate)) {
        lines.push_back(current);
        current = std::string(word);
      } else {
        current = candidate;
      }
      word_pos = space + 1;
    }
    lines.push_back(current);
    if (end == text.size()) {
      break;
    }
    pos = end + 1;
  }
  return lines;
}

float MeasureTextHeight(NovaFontCache &fonts,
                        const std::string &text,
                        float view_width) {
  if (text.empty()) {
    return 0.0F;
  }
  const auto lines = WrapText(fonts, text, view_width);
  return static_cast<float>(lines.size()) * 11.0F +
         17.0F; // + first line + tail
}

// ---------------------------------------------------------------------------
// Tab strip painter — Ghidra 0x004a1c40
// NovaUi_DrawPlayerSpecialInteractionTabs(page, pressed).
// ---------------------------------------------------------------------------

void DrawTabStrip(SdlPlatform &platform,
                  NovaFontCache &font_cache,
                  const ServicesButtonArt &art,
                  const SDL_FRect strip_rects[6],
                  int page,
                  int pressed,
                  bool jettison_enabled) {
  for (int i = 0; i < 6; i++) {
    // Enabled flags (acStack_20 in the decompile): 0..4 always, 5 only on
    // the cargo page with cargo/junk/mission cargo present.
    const bool enabled = i != kTabJettison || jettison_enabled;
    if (!enabled) {
      continue;
    }
    const bool is_pressed = (pressed == i) || (page == i);
    art.Draw(platform,
             strip_rects[i],
             is_pressed ? ButtonState::kHover : ButtonState::kNormal);
    NovaText_DrawCentered(platform,
                          font_cache,
                          kThreeStateButtonFontFamily,
                          kThreeStateButtonFontSize,
                          kNovaFontStyleRegular,
                          SDL_Color{255, 255, 255, SDL_ALPHA_OPAQUE},
                          strip_rects[i].x,
                          strip_rects[i].x + strip_rects[i].w,
                          ThreeStateButtonLabelBaseline(strip_rects[i]),
                          TitleString(kTabLabelEntries[i]));
  }
}

// ---------------------------------------------------------------------------
// Tab strip press tracker — Ghidra 0x004a1ae0
// NovaUi_HandlePlayerSpecialInteractionTabs(page, packed_point).
//
// Runs the modal press loop: redraw with the hovered strip index, then while
// input events are pending re-read the mouse and redraw whenever the hover
// changes; returns the index under the release point (-1 if the press
// started outside the strip). `redraw` re-paints the whole window with the
// given hover index, mirroring the original's draw-then-blit cadence.
// ---------------------------------------------------------------------------

int HandleTabStrip(SdlPlatform &platform,
                   const SDL_FRect strip_rects[6],
                   int page,
                   SDL_FPoint point,
                   const std::function<void(int hover)> &redraw) {
  (void)page;
  int pressed = -1;
  for (int i = 0; i < 6; i++) {
    if (SDL_PointInRectFloat(&point, &strip_rects[i])) {
      pressed = i;
      break;
    }
  }
  if (pressed == -1) {
    return -1;
  }

  int hover = pressed;
  redraw(hover);
  platform.Present();

  // Modal press loop: the original spins on NovaInput_PumpAndHasPendingEvents
  // (0x004b68f0) re-reading the mouse; the port pumps events through the same
  // channel and redraws on the decompile's change guard (hover leaving both
  // the current index and the original press index clamps to -1).
  for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
    const SDL_FPoint mouse = platform.mouse_position();
    int next = -1;
    for (int i = 0; i < 6; i++) {
      if (SDL_PointInRectFloat(&mouse, &strip_rects[i])) {
        next = i;
        break;
      }
    }
    if (next != pressed && next != hover) {
      next = -1;
    }
    if (next != hover) {
      redraw(next);
      platform.Present();
      hover = next;
    }
  }
  return hover;
}

// ---------------------------------------------------------------------------
// Page 1 (General) — the stat-grid arm of 0x0049a540.
// ---------------------------------------------------------------------------

// Ghidra NovaUi_DrawCombatRankLabel 0x00469030: rank index 0..10 from the
// doubling threshold chain 100/200/400/800/1600/3200/6400/12800/25600, drawn
// from STR# 138.
std::string CombatRankLabel(const GameState &state) {
  const int rank =
      NovaPlayerInfo_CombatRankIndex(state.player_combat_rating_points);
  auto label = NovaHud_LoadStringEntry(kRatingsStr, rank + 1);
  if (!label) {
    NovaLog::Todo("player-info: missing combat-rank STR# 138 entry {}",
                  rank + 1);
    return MiscString(kStrNa, "N/A");
  }
  return *label;
}

// Ghidra: ROUND(points / max * DAT_00575958) drawn through
// PascalString_FromUInt, i.e. an integer percent (0..100).
int Percent(float value, float max) {
  if (max <= 0.0F) {
    return 0;
  }
  return RoundToInt(static_cast<double>(value / max) * kPercentScale);
}

void DrawGeneralPage(SdlPlatform &platform,
                     NovaFontCache &font_cache,
                     const GameState &state,
                     const SDL_FRect &view_rect) {
  const float label_y = view_rect.y + kGridFirstRowDy;

  auto draw_row = [&](float baseline_y,
                      float label_x,
                      float value_x,
                      std::string_view label,
                      const std::string &value) {
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  9.0F,
                  kNovaFontStyleRegular,
                  kLabelColor,
                  view_rect.x + label_x,
                  baseline_y,
                  label);
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  9.0F,
                  kNovaFontStyleRegular,
                  kValueColor,
                  view_rect.x + value_x,
                  baseline_y,
                  value);
  };

  const std::string na = MiscString(kStrNa, "N/A");

  // Left column: pilot identity + world status.
  std::string pilot_name = state.pilot.first_name;
  if (!state.pilot.last_name.empty()) {
    if (!pilot_name.empty()) {
      pilot_name += " ";
    }
    pilot_name += state.pilot.last_name;
  }
  draw_row(label_y,
           kGridLeftLabelX,
           kGridLeftValueX,
           MiscString(kStrPilotName, "Pilot Name:"),
           pilot_name);
  draw_row(label_y + kGridRowStride,
           kGridLeftLabelX,
           kGridLeftValueX,
           MiscString(kStrCurrentDate, "Current Date:"),
           NovaText_FormatDateString(
               state.date, true, state.date_prefix, state.date_suffix));
  const System *system = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  draw_row(label_y + 2 * kGridRowStride,
           kGridLeftLabelX,
           kGridLeftValueX,
           MiscString(kStrSystem, "System:"),
           system != nullptr ? system->name : std::string("?"));
  const std::string legal_status =
      NovaSystem_HasUsableTravelDestination(state,
                                            state.player.current_system_id)
          ? NovaUi_SystemFactionConflictStatusText(
                state, state.player.current_system_id)
          : na;
  draw_row(label_y + 3 * kGridRowStride,
           kGridLeftLabelX,
           kGridLeftValueX,
           MiscString(kStrLegalStatus, "Legal Status:"),
           legal_status);
  draw_row(label_y + 4 * kGridRowStride,
           kGridLeftLabelX,
           kGridLeftValueX,
           MiscString(kStrCombatRating, "Combat Rating:"),
           CombatRankLabel(state));

  // Right column: ship identity + performance.
  draw_row(label_y,
           kGridRightLabelX,
           kGridRightValueX,
           MiscString(kStrShipName, "Ship Name:"),
           state.player.ship_name);
  const ShipClass *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  draw_row(label_y + kGridRowStride,
           kGridRightLabelX,
           kGridRightValueX,
           MiscString(kStrShipClass, "Ship Class:"),
           ship_class != nullptr ? ship_class->display_name : std::string("?"));

  const PlayerEffectiveStats stats = Outfit_ComputePlayerEffectiveStats(state);
  const int turn_deg_per_sec = RoundToInt(static_cast<float>(
      static_cast<double>(stats.turn_raw) * kTurnRatePerSec));
  draw_row(label_y + 2 * kGridRowStride,
           kGridRightLabelX,
           kGridRightValueX,
           MiscString(kStrTurnRate, "Turn Rate:"),
           std::to_string(turn_deg_per_sec) +
               MiscString(kStrDegPerSec, " deg/sec"));
  const int thrust = RoundToInt(static_cast<float>(
      static_cast<double>(stats.thrust_raw) * kThrustDisplay));
  draw_row(label_y + 3 * kGridRowStride,
           kGridRightLabelX,
           kGridRightValueX,
           MiscString(kStrAccelRate, "Accel Rate:"),
           std::to_string(thrust));
  const int max_speed = RoundToInt(static_cast<float>(
      static_cast<double>(stats.speed_raw) *
      (state.pilot.strict_play ? 1.0 : kSpeedNoStrictScale)));
  draw_row(label_y + 4 * kGridRowStride,
           kGridRightLabelX,
           kGridRightValueX,
           MiscString(kStrMaxSpeed, "Max Speed:"),
           std::to_string(max_speed));

  // Row +0x5c: credits on the RIGHT column (label = the STR# 0x21 "credits"
  // pstring whose first char is input-map translated, plus the ":" spacer
  // 0056d168), shield status on the LEFT.
  draw_row(label_y + 5 * kGridRowStride,
           kGridRightLabelX,
           kGridRightValueX,
           MiscString(kStrCreditsWord, "credits") + ":",
           GroupThousands(state.player.credits));

  // Shield / armor / energy rows. Labels are the runtime pstrings
  // DAT_0072decc/.4cc/.dacc which NovaData_LoadDisplayNamePstringTables
  // (0x004c7040) fills from STR# 0x7d2 0xc/0x11/0x8. The depleted/overcharged
  // shield arms draw static pstrings 0056d17c/0056d174 that are not in any
  // mapped pool; the port draws the clamped percent instead.
  // TODO(decomp): read 0056d17c/0056d174.
  static bool logged_labels = false;
  if (!logged_labels) {
    logged_labels = true;
    NovaLog::Todo("player-info: depleted/overcharged shield+armor value arms "
                  "draw static pstrings 0056d17c/0056d174 (unidentified); "
                  "drawing the clamped percent instead");
  }
  draw_row(label_y + 6 * kGridRowStride,
           kGridLeftLabelX,
           kGridLeftValueX,
           MiscString(kStrShieldStatusLabel, "Shield Status:"),
           stats.max_shield_points <= 0.0F
               ? na
               : std::to_string(Percent(state.player.shield_points,
                                        stats.max_shield_points)) +
                     "%");
  draw_row(label_y + 7 * kGridRowStride,
           kGridLeftLabelX,
           kGridLeftValueX,
           MiscString(kStrArmorStatusLabel, "Armor Status:"),
           NovaAiShip_IsDestroyed(state.player)
               ? MiscString(kStrFailed, "Failed")
               : (stats.max_armor_points <= 0.0F
                      ? na
                      : std::to_string(Percent(state.player.armor_points,
                                               stats.max_armor_points)) +
                            "%"));
  // Energy row: fuel percent, then the jumps figure (fuel/100, rounded):
  // <1 jump with fuel left -> "maneuvering energy only"; otherwise
  // "N jump(s)" and, when the fuel is not a whole multiple of 100,
  // "plus maneuvering energy". The spacer pstrings 0056d170/128/164 are not
  // identified; the port composes the fragments with single spaces.
  std::string energy_value;
  if (stats.fuel_capacity <= 0.0F) {
    energy_value = na;
  } else {
    energy_value =
        std::to_string(Percent(state.player.fuel_points, stats.fuel_capacity)) +
        "%";
    const int jumps = RoundToInt(static_cast<float>(
        static_cast<double>(state.player.fuel_points) / kPercentScale));
    const bool partial = static_cast<int>(state.player.fuel_points) % 100 != 0;
    if (jumps < 1) {
      if (state.player.fuel_points > kEmptyThreshold) {
        energy_value +=
            " " + MiscString(kStrManeuveringEnergy, "maneuvering energy") +
            " " + MiscString(kStrOnly, "only");
      }
    } else {
      energy_value += " " + std::to_string(jumps) + " " +
                      (jumps < 2 ? MiscString(kStrJump, "jump")
                                 : MiscString(kStrJumps, "jumps"));
      if (partial) {
        energy_value += " " + MiscString(kStrPlus, "plus") + " " +
                        MiscString(kStrManeuveringEnergy, "maneuvering energy");
      }
    }
  }
  draw_row(label_y + 8 * kGridRowStride,
           kGridLeftLabelX,
           kGridLeftValueX,
           MiscString(kStrEnergyStatusLabel, "Energy Status:"),
           energy_value);

  // The page tail's fleet-value / tribute block (escort base_cost * 0.01 sum,
  // DAT_00575948) needs the mission-fleet/escort bookkeeping that is not
  // modelled yet. TODO(decomp(0x0049a540)).
  static bool logged_fleet_tail = false;
  if (!logged_fleet_tail) {
    logged_fleet_tail = true;
    NovaLog::Todo("player-info: fleet-value and tribute tail block of the "
                  "General page not ported");
  }
}

// ---------------------------------------------------------------------------
// Pages 2-4 — prebuilt summary texts (0x0049c050) or the "none" fallbacks.
// ---------------------------------------------------------------------------

void DrawTextPage(SdlPlatform &platform,
                  NovaFontCache &font_cache,
                  const std::string &text,
                  const SDL_FRect &view_rect,
                  std::uint16_t empty_entry,
                  const char *empty_fallback) {
  if (!text.empty()) {
    // The original draws the pstring into a filled rect and inverts it
    // (white-on-dark after inversion); the port draws light text directly.
    const auto lines = WrapText(font_cache, text, view_rect.w - 12.0F);
    const SDL_Rect clip{static_cast<int>(std::floor(view_rect.x)),
                        static_cast<int>(std::floor(view_rect.y)),
                        static_cast<int>(std::ceil(view_rect.w)),
                        static_cast<int>(std::ceil(view_rect.h))};
    SDL_SetRenderClipRect(platform.renderer(), &clip);
    float baseline = view_rect.y + 11.0F;
    for (const auto &line : lines) {
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    9.0F,
                    kNovaFontStyleRegular,
                    kValueColor,
                    view_rect.x + 6.0F,
                    baseline,
                    line);
      baseline += 11.0F;
    }
    SDL_SetRenderClipRect(platform.renderer(), nullptr);
    return;
  }
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                9.0F,
                kNovaFontStyleRegular,
                kLabelColor,
                view_rect.x + 5.0F,
                view_rect.y + 0x37,
                MiscString(empty_entry, empty_fallback));
}

// ---------------------------------------------------------------------------
// Jettison confirmation — Ghidra Ui_ShowConfirmDialog 0x004977d0 (DLOG 0xbba).
// Returns true when the player confirms.
// ---------------------------------------------------------------------------

bool RunJettisonConfirmDialog(SdlPlatform &platform,
                              NovaFontCache &font_cache,
                              const std::function<void()> &render_background) {
  // Ghidra Ui_ShowConfirmDialog 0x004977d0 (DLOG 0xbba): entry 3 is the
  // message text; the loop resolves OK to 1 and Cancel/dismiss to 5.
  return NovaUi_ShowConfirmDialog(platform,
                                  font_cache,
                                  MiscString(kStrJettisonConfirm, ""),
                                  render_background);
}

} // namespace

// ---------------------------------------------------------------------------
// Ghidra 0x0049c050 NovaUi_BuildPlayerSpecialInteractionStrings.
// ---------------------------------------------------------------------------

int NovaPlayerInfo_CombatRankIndex(std::int32_t points) {
  int rank = points > 0 ? 1 : 0;
  static constexpr std::int32_t kThresholds[] = {
      100, 200, 400, 800, 1600, 3200, 6400, 12800, 25600};
  for (const std::int32_t threshold : kThresholds) {
    if (points >= threshold) {
      ++rank;
    }
  }
  return rank;
}

PlayerInfoSummaryTexts
NovaPlayerInfo_BuildSummaryTexts(const GameState &state) {
  PlayerInfoSummaryTexts texts;

  // Cargo page text: walks the 6 player cargo bins, every active mission's
  // cargo (0x14-stride runtime block) and the 0x80 g_junk_defs entries
  // (count +0x22, name +0x228), naming each commodity through the 0x100-
  // stride name table DAT_0069d2cc. The scenario model has no commodity or
  // junk definition names yet. TODO(decomp(0x0049c050)): port once
  // commodity/junk defs are modelled (tracker: junk resource defs
  // unmodelled).
  {
    if (Player_HasAnyCargoMissionOrJunk(state)) {
      NovaLog::Todo("player-info: cargo summary text (commodity/junk names "
                    "unmodelled) — page falls back to the draw-side cargo "
                    "list");
    }
  }

  // Extras text (0x7d6278, 0x0049c050): owned non-0x2000 outfits grouped by
  // similar_to and ordered by DisplayWeight descending, then the ship
  // trade-in footer.
  {
    const auto groups =
        CollectOutfitSummaryGroups(state, /*rank_outfits=*/false);
    if (!groups.empty()) {
      std::vector<std::string> entries;
      entries.reserve(groups.size());
      for (const OutfitSummaryGroup &group : groups) {
        entries.push_back(FormatOutfitSummaryGroup(state, group));
      }
      // Ghidra 0x0049c050: the list ends with ".\r\r" and, when the ship's
      // trade-in is positive, the STR# 0x7d2 0x111 label + grouped quantity +
      // "credits" word (base trade-in, before store scaling).
      std::string extras = JoinExtrasEntries(entries) + ".";
      const std::int32_t trade_in = Ship_ComputeTradeInValue(state);
      if (trade_in > 0) {
        extras += "\r\r" +
                  MiscString(kStrTradeInValue, "Ship trade-in value:") + " " +
                  GroupThousands(trade_in) + " " +
                  MiscString(kStrCreditsWord, "credits") + "\r";
      }
      texts.extras = MiscString(kStrExtrasHeader, "") + "\r\r" + extras;
    }
  }

  // Honors text (0x7d7278, 0x0049c050): the active + defined rank full names
  // first (record name at +0x1e, non-empty), ordered by Weight descending
  // (higher weight displayed first per the Bible), then the owned 0x2000-flag
  // ("Show as Rank in player info") outfits grouped by similar_to and ordered
  // by DisplayWeight descending. Entries share the 0x0049c050 list glue
  // (JoinHonorsEntries).
  {
    std::vector<const RankDef *> ranked;
    for (const RankDef &rank : state.scenario.ranks) {
      if (rank.active && rank.defined && !rank.full_name.empty()) {
        ranked.push_back(&rank);
      }
    }
    // The original scans slots ascending and emits the current weight tier in
    // one pass, so equal weights keep slot order.
    std::stable_sort(
        ranked.begin(), ranked.end(), [](const RankDef *a, const RankDef *b) {
          return a->weight > b->weight;
        });
    std::vector<std::string> rank_names;
    rank_names.reserve(ranked.size());
    for (const RankDef *rank : ranked) {
      rank_names.push_back(rank->full_name);
    }
    std::vector<std::string> outfits;
    for (const OutfitSummaryGroup &group :
         CollectOutfitSummaryGroups(state, /*rank_outfits=*/true)) {
      outfits.push_back(FormatOutfitSummaryGroup(state, group));
    }
    if (!rank_names.empty() || !outfits.empty()) {
      texts.honors = MiscString(kStrHonorsHeader, "") + "\r\r" +
                     JoinHonorsEntries(rank_names, outfits);
    }
  }

  return texts;
}

// ---------------------------------------------------------------------------
// Ghidra 0x00499c10 NovaUi_RunPlayerSpecialInteractionWindow (+ draw
// 0x0049a540 + dispatch 0x0049a3a0).
// ---------------------------------------------------------------------------

PlayerInfoWindowResult
NovaPlayerInfo_RunWindow(SdlPlatform &platform,
                         GameState &state,
                         const std::function<void()> &render_background) {
  SdlPlatform::ScopedPlacement placement_guard(platform,
                                               platform.current_placement());
  PlayerInfoWindowResult result;

  // Guards: the original returns when a modal is already up (BOOL_007354a8),
  // the station-hold timer is not exactly +0.0, or the death timer exceeds
  // DAT_00575990 (1/128). The port runs modals synchronously on the flight
  // loop, so the modal latch has no counterpart.
  if (state.player.ai_station_hold_timer != 0.0F ||
      state.player.death_timer_active > kEmptyThreshold) {
    return result;
  }

  // Weapon_ReconcileOutfitPoolWithWeaponBanks (0x00462ec0) refreshes the
  // outfit ledger before the window opens. This materializes mounted stock
  // weapons that are not DefaultItems (for example the Terrapin's Medium
  // Blaster) so the Extras page includes them.
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);

  auto definition = NovaResource_LoadDialogDefinition(kDialogId);
  auto items =
      definition ? NovaResource_LoadDialogItems(definition->dialog_item_list_id)
                 : std::nullopt;
  if (!definition || !items) {
    NovaLog::Todo("player-info: DLOG/DITL 0x3f9 unavailable; window skipped");
    return result;
  }

  // Dialog_CreateFromDlog 0x008730a1 ignores the DLOG's stored origin (0x3f9
  // ships at (40,40)); the port keeps the DLOG geometry local and lets its
  // placement centre the complete composition in the window.
  const float window_w =
      static_cast<float>(definition->right - definition->left);
  const float window_h =
      static_cast<float>(definition->bottom - definition->top);
  const SDL_FRect window_rect{0.0F, 0.0F, window_w, window_h};

  auto entry_rect =
      [&items](std::size_t row_1based) -> std::optional<SDL_FRect> {
    for (const auto &item : *items) {
      if (item.index + 1 == row_1based) {
        return SDL_FRect{static_cast<float>(item.left),
                         static_cast<float>(item.top),
                         static_cast<float>(item.right - item.left),
                         static_cast<float>(item.bottom - item.top)};
      }
    }
    return std::nullopt;
  };

  SDL_FRect view_rect;
  if (auto r = entry_rect(kTextViewDitl)) {
    view_rect = *r;
  } else {
    NovaLog::Todo("player-info: DITL 0x3f9 entry 6 (text view) missing");
    return result;
  }
  SDL_FRect strip_rects[6];
  for (int i = 0; i < 6; i++) {
    if (auto r = entry_rect(static_cast<std::size_t>(kTabStripDitl[i]))) {
      strip_rects[i] = *r;
    } else {
      NovaLog::Todo("player-info: DITL 0x3f9 tab entry {} missing",
                    kTabStripDitl[i]);
      return result;
    }
  }

  const auto texts = NovaPlayerInfo_BuildSummaryTexts(state);

  NovaFontCache font_cache;
  // Auto-grow (0x00499c10): the longest page text must fit the view rect;
  // the window grows by delta = content - view_height + 4, shifts up by
  // delta/2 (DAT_00575940 = 0.5), extends entry 6, and moves entries 1/7
  // down by delta.
  float delta = 0.0F;
  for (const std::string *text : {&texts.cargo, &texts.extras, &texts.honors}) {
    const float height =
        MeasureTextHeight(font_cache, *text, view_rect.w - 12.0F);
    delta = std::max(delta, height - view_rect.h + 4.0F);
  }
  SDL_FRect window_pos{window_rect};
  if (delta > 0.0F) {
    // FUN_004d18b0 grows the window by delta; FUN_004d1a30 shifts it up by
    // delta * 0.5 (DAT_00575940). Only the bottom-anchored entries move with
    // the new bottom edge: entries 1 (Done) and 7 (Jettison) offset down by
    // delta (window-relative), while entry 6 extends its bottom edge by delta;
    // the top tab entries 2..5 stay glued to the window top.
    // The local composition is centred by Placement; only its height grows.
    window_pos.y = 0.0F;
    window_pos.h += delta;
    view_rect.h += delta;
    strip_rects[kTabClose].y += delta;
    strip_rects[kTabJettison].y += delta;
  }
  const SDL_FRect window_rect_final{window_pos};
  // The entry rects above are window-relative DITL coordinates; the original
  // draws through the window's owner draw context (SetCursorPosAdjusted adds
  // the window origin) and hit-tests window-relative mouse points. The port
  // draws straight onto the centred-playfield renderer, so bake the window
  // origin into the rects once.
  view_rect.x += window_rect_final.x;
  view_rect.y += window_rect_final.y;
  for (auto &rect : strip_rects) {
    rect.x += window_rect_final.x;
    rect.y += window_rect_final.y;
  }

  ServicesButtonArt art;
  if (!art.Initialize(platform)) {
    NovaLog::Warn("player-info: three-state button art unavailable");
  }
  auto backdrop_top = LoadPictTexture(platform, kBackdropTopPict);
  auto backdrop_middle = LoadPictTexture(platform, kBackdropMiddlePict);
  auto backdrop_bottom = LoadPictTexture(platform, kBackdropBottomPict);
  if (!backdrop_top || !backdrop_middle || !backdrop_bottom) {
    NovaLog::Todo("player-info: backdrop PICTs 0x2146-0x2148 incomplete; "
                  "drawing a filled placeholder");
  }

  bool any_cargo = Ship_HasAnyCargoLootOrActiveMission(state);

  // NovaInputQueue_FlushAllCommands: discard the P keypress that opened the
  // window so it cannot dispatch a phantom action.
  while (platform.PollTextEvent().has_value()) {
  }

  std::int16_t page = 1; // g_player_special_page
  bool close = false;

  auto redraw = [&](int strip_hover) {
    if (render_background) {
      render_background();
    } else {
      SDL_SetRenderDrawColor(platform.renderer(), 0, 0, 0, SDL_ALPHA_OPAQUE);
      SDL_RenderClear(platform.renderer());
    }
    platform.SetPlacement(
        PlaceContained({window_rect_final.w, window_rect_final.h},
                       platform.logical_playfield_size()));
    SDL_Renderer *renderer = platform.renderer();

    // Window plate + backdrop slices.
    SDL_SetRenderDrawColor(renderer,
                           kWindowFill.r,
                           kWindowFill.g,
                           kWindowFill.b,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &window_rect_final);
    auto blit = [&](const std::unique_ptr<SdlTexture> &texture, SDL_FRect dst) {
      if (texture) {
        SDL_RenderTexture(renderer, texture->get(), nullptr, &dst);
      }
    };
    if (backdrop_top) {
      float w = 0.0F;
      float h = 0.0F;
      SDL_GetTextureSize(backdrop_top->get(), &w, &h);
      blit(backdrop_top,
           SDL_FRect{window_rect_final.x,
                     window_rect_final.y,
                     window_rect_final.w,
                     h});
    }
    if (backdrop_bottom) {
      float w = 0.0F;
      float h = 0.0F;
      SDL_GetTextureSize(backdrop_bottom->get(), &w, &h);
      blit(backdrop_bottom,
           SDL_FRect{window_rect_final.x,
                     window_rect_final.y + window_rect_final.h - h,
                     window_rect_final.w,
                     h});
    }
    if (backdrop_middle) {
      float w = 0.0F;
      float h = 0.0F;
      SDL_GetTextureSize(backdrop_middle->get(), &w, &h);
      float top_h = 0.0F;
      SDL_GetTextureSize(
          backdrop_top ? backdrop_top->get() : nullptr, &w, &top_h);
      float bottom_h = 0.0F;
      SDL_GetTextureSize(
          backdrop_bottom ? backdrop_bottom->get() : nullptr, &w, &bottom_h);
      const float middle_top = window_rect_final.y + top_h;
      const float middle_bottom =
          window_rect_final.y + window_rect_final.h - bottom_h;
      blit(backdrop_middle,
           SDL_FRect{window_rect_final.x,
                     middle_top,
                     window_rect_final.w,
                     std::max(0.0F, middle_bottom - middle_top)});
    }

    // Page content (the draw callback 0x0049a540's page switch).
    if (page == 1) {
      DrawGeneralPage(platform, font_cache, state, view_rect);
    } else if (page == 2) {
      // Cargo page: the original draws the prebuilt cargo text when
      // Player_HasAnyCargoMissionOrJunk (0x0046a680) passes (the port's
      // builder is TODO(decomp), so texts.cargo stays empty and the page
      // shows the empty-hold fallback, selected by the decompile's
      // capacity-vs-fleet-capacity branch 0x10c/0x10d).
      const bool under_capacity = Ship_ComputeShipTotalCargoCapacity(state) <
                                  Player_ComputeFleetCargoCapacity(state);
      DrawTextPage(platform,
                   font_cache,
                   texts.cargo,
                   view_rect,
                   under_capacity ? kStrNoFleetCargo : kStrNoShipCargo,
                   "You don't have any cargo");
    } else if (page == 3) {
      DrawTextPage(platform,
                   font_cache,
                   texts.extras,
                   view_rect,
                   kStrNoExtras,
                   "You don't have any extras on your ship");
    } else if (page == 4) {
      DrawTextPage(platform,
                   font_cache,
                   texts.honors,
                   view_rect,
                   kStrNoHonors,
                   "You don't have any ranks or honors");
    }

    // Tab strip: tab 5 (Jettison) enabled only on the cargo page.
    DrawTabStrip(platform,
                 font_cache,
                 art,
                 strip_rects,
                 page,
                 strip_hover,
                 page == 2 && any_cargo);
  };

  redraw(-1);
  platform.Present();

  // The dispatch callback closes on a still-held P binding; after the port's
  // flush the physical key is still down on the opening frame, so the arm
  // engages only after one release (the original's queue drain had the same
  // effect on its PumpAndTestCommand latch).
  bool special_key_released = false;
  while (!platform.quit_requested() && !close) {
    // Dispatch callback 0x0049a3a0: Tab cycles pages (shift reverses,
    // wrapping 1..4); Enter/Esc close; a still-held P binding closes; a
    // click on the strip runs the modal press tracker.
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      if (in->key == TextKey::physical && in->key_code == kKeyCodeTab) {
        const bool reverse = platform.IsOriginalKeyCodeHeld(0x2a) ||
                             platform.IsOriginalKeyCodeHeld(0x36);
        page = static_cast<std::int16_t>(page + (reverse ? -1 : 1));
        if (page > 4) {
          page = 1;
        } else if (page < 1) {
          page = 4;
        }
        redraw(-1);
        platform.Present();
      } else if (in->key == TextKey::physical &&
                 (in->key_code == kKeyCodeEnter ||
                  in->key_code == kKeyCodeEscape)) {
        close = true;
      } else if (in->key == TextKey::primary) {
        const int released = HandleTabStrip(
            platform, strip_rects, page, platform.mouse_position(), redraw);
        // Dispatch mapping (0x0049a3a0): 0 -> close, 1-4 -> page select,
        // 5 -> jettison arm, otherwise consumed.
        if (released == kTabClose) {
          close = true;
        } else if (released >= 1 && released <= 4) {
          page = static_cast<std::int16_t>(released);
          redraw(-1);
          platform.Present();
        } else if (released == kTabJettison) {
          if (page == 2 && any_cargo &&
              RunJettisonConfirmDialog(
                  platform, font_cache, [&]() { redraw(-1); })) {
            // The original runs Player_RedistributeFleetCargoOverflow(true)
            // (0x0041f330) here and closes; the port surfaces the confirmed
            // flag so the caller can apply it with the sim clock in scope.
            result.jettison_confirmed = true;
            close = true;
          }
          redraw(-1);
          platform.Present();
        }
      }
    }
    // g_player_key_bindings[0x19] (default P, DIK 0x19) closes after a
    // release-then-press, as in the dispatch callback's drain arm.
    if (!NovaInput_IsCommandActive(platform, 0x19)) {
      special_key_released = true;
    } else if (special_key_released) {
      close = true;
    }

    // The original redraws the window (draw callback) and blits it every
    // loop iteration; the port repaints and presents per frame.
    redraw(-1);
    platform.Present();

    platform.PaceFrame();
  }

  // Teardown: the original drains the special binding, destroys the window,
  // frees the three PICT images and flushes input; the port's loop-scoped
  // assets release automatically.
  while (platform.PollTextEvent().has_value()) {
  }
  return result;
}

} // namespace game
