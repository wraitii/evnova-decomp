#include "preferences_extra.hpp"

#include "../log.hpp"
#include "../nova_paths.hpp"
#include "../sdl_platform.hpp"
#include "../util/geometry.hpp"
#include "nova_font.hpp"
#include "preferences_ui.hpp"
#include "ui_dialog.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace game {

using evnova::util::Contains;
using namespace preferences_detail;

// Extra Prefs dialog geometry. The title band comes from DLOG 0xfa3's DITL;
// the steppers, checkbox rows and buttons are port-authored local coordinates,
// and the window is extended below the authored 336x296 to fit them.
//
// Scale rows step 31px so the Mission subline clears its label; the bug-fix
// list uses the default Settings DITL's 22px checkbox step, with the bundled
// 'Safe' row set off from the altering fixes by an extra gap.
constexpr std::array<float, 3> kExtraScaleRowY{48.0F, 79.0F, 110.0F};
constexpr float kExtraLabelX = 24.0F;
constexpr float kExtraArrowX = 214.0F;
constexpr float kExtraValueX = 232.0F;
constexpr float kExtraValueWidth = 84.0F;
// Native PICT arrows are only 11x9, so pad the click target around them; the
// up and down targets meet but never overlap.
constexpr float kExtraArrowHitPadX = 8.0F;
constexpr float kExtraArrowHitPadY = 5.0F;
constexpr float kExtraWindowHeight = 350.0F;
constexpr float kExtraButtonWidth = 70.0F;
constexpr float kExtraButtonHeight = 20.0F;
constexpr float kExtraButtonMargin = 13.0F;
constexpr float kExtraBugFixHeaderY = 168.0F;
constexpr float kExtraBugFixFirstRowY = 184.0F;
constexpr float kExtraBugFixRowStep = 22.0F;
constexpr float kExtraBugFixSafeGap = 14.0F;
constexpr float kExtraBugFixBoxX = 24.0F;

namespace {

constexpr std::string_view kExtraPrefsFileName = "EV Nova Extra Prefs.ini";
constexpr std::string_view kInstallRootKey = "install_root";
constexpr std::string_view kUiScaleKey = "ui_scale";
constexpr std::string_view kFlightSceneScaleKey = "flight_scene_scale";
constexpr std::string_view kMissionScaleKey = "mission_scale";
constexpr std::string_view kSafeBugFixesKey = "safe";
constexpr std::string_view kOutfitSlotBalanceKey = "outfit_slot_balance";
constexpr std::string_view kOutfitPricesKey = "outfit_prices";
constexpr std::string_view kCronEventsKey = "cron_events";
constexpr std::string_view kParticleFogKey = "particle_fog";

[[nodiscard]] std::string_view Trim(std::string_view text) {
  constexpr std::string_view kWhitespace = " \t\r\n";
  const std::size_t first = text.find_first_not_of(kWhitespace);
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(kWhitespace);
  return text.substr(first, last - first + 1);
}

// Strips one layer of matching single/double quotes so a path with leading or
// trailing spaces can be stored by hand.
[[nodiscard]] std::string_view Unquote(std::string_view value) {
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\''))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

// Writes a float with enough precision to round-trip the supported range
// without a locale-dependent decimal separator.
[[nodiscard]] std::string FormatScale(float value) {
  return fmt::format("{:.6g}", value);
}

// Parses one multiplier, warning and falling back to 1.0 on rejection.
void ParseScaleOrDefault(std::string_view value,
                         std::string_view key,
                         std::string_view source,
                         std::size_t line_number,
                         float &out) {
  if (const auto parsed = PresentationScale_Parse(value)) {
    out = *parsed;
    return;
  }
  NovaLog::Warn("extra prefs '{}': invalid {} '{}' on line {}; using 1.0",
                std::string{source},
                std::string{key},
                std::string{value},
                line_number);
  out = 1.0F;
}

// Parses one boolean bug-fix flag, warning and keeping the previous (default)
// value on rejection. Accepts 1/0, true/false, on/off and yes/no
// case-insensitively.
void ParseBoolOrDefault(std::string_view value,
                        std::string_view key,
                        std::string_view source,
                        std::size_t line_number,
                        bool &out) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const char c : value) {
    lowered.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lowered == "1" || lowered == "true" || lowered == "on" ||
      lowered == "yes") {
    out = true;
    return;
  }
  if (lowered == "0" || lowered == "false" || lowered == "off" ||
      lowered == "no") {
    out = false;
    return;
  }
  NovaLog::Warn("extra prefs '{}': invalid {} '{}' on line {}; keeping {}",
                std::string{source},
                std::string{key},
                std::string{value},
                line_number,
                out ? "on" : "off");
}

// Writes a boolean bug-fix flag as 1/0 so the file stays trivially parseable.
[[nodiscard]] const char *BoolFlag(bool value) { return value ? "1" : "0"; }

// Reads a valid presentation multiplier from an environment variable, or
// nullopt when the variable is unset/empty. Invalid values warn and are
// ignored.
[[nodiscard]] std::optional<float> EnvScale(const char *name) {
  const char *raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return std::nullopt;
  }
  const std::string_view text = Trim(std::string_view{raw});
  if (const auto parsed = PresentationScale_Parse(text)) {
    return parsed;
  }
  NovaLog::Warn("extra prefs: ignoring invalid {}='{}' (expected a finite "
                "value in [{}, {}])",
                name,
                raw,
                kPresentationScaleMin,
                kPresentationScaleMax);
  return std::nullopt;
}

} // namespace

std::optional<std::filesystem::path> NovaExtraPrefs_SystemPath() {
  const auto support = NovaPaths::SupportDirectory();
  if (!support) {
    return std::nullopt;
  }
  return *support / std::filesystem::path{kExtraPrefsFileName};
}

void NovaExtraPrefs_Parse(std::istream &in,
                          NovaExtraPrefs &prefs,
                          std::string_view source) {
  prefs = {};
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    const std::string_view trimmed = Trim(line);
    // Section headers are accepted but ignored: keys are flat so old files
    // without a `[display]` section still load.
    if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';' ||
        trimmed.front() == '[') {
      continue;
    }
    const std::size_t separator = trimmed.find('=');
    if (separator == std::string_view::npos) {
      NovaLog::Warn("extra prefs '{}': line {} has no '=', ignoring",
                    std::string{source},
                    line_number);
      continue;
    }
    const std::string_view key = Trim(trimmed.substr(0, separator));
    const std::string_view value = Trim(trimmed.substr(separator + 1));
    if (key == kInstallRootKey) {
      const std::string_view unquoted = Unquote(value);
      if (unquoted.empty()) {
        prefs.install_root.reset();
      } else {
        prefs.install_root = std::filesystem::path{std::string{unquoted}};
      }
    } else if (key == kUiScaleKey) {
      ParseScaleOrDefault(value, key, source, line_number, prefs.scale.ui);
    } else if (key == kFlightSceneScaleKey) {
      ParseScaleOrDefault(
          value, key, source, line_number, prefs.scale.flight_scene);
    } else if (key == kMissionScaleKey) {
      ParseScaleOrDefault(value, key, source, line_number, prefs.scale.mission);
    } else if (key == kSafeBugFixesKey) {
      ParseBoolOrDefault(value, key, source, line_number, prefs.bugfixes.safe);
    } else if (key == kOutfitSlotBalanceKey) {
      ParseBoolOrDefault(
          value, key, source, line_number, prefs.bugfixes.outfit_slot_balance);
    } else if (key == kOutfitPricesKey) {
      ParseBoolOrDefault(
          value, key, source, line_number, prefs.bugfixes.outfit_prices);
    } else if (key == kCronEventsKey) {
      ParseBoolOrDefault(
          value, key, source, line_number, prefs.bugfixes.cron_events);
    } else if (key == kParticleFogKey) {
      ParseBoolOrDefault(
          value, key, source, line_number, prefs.bugfixes.particle_fog);
    } else {
      NovaLog::Warn("extra prefs '{}': unknown key '{}' on line {}, ignoring",
                    std::string{source},
                    std::string{key},
                    line_number);
    }
  }
}

bool NovaExtraPrefs_LoadFromSystemStore(NovaExtraPrefs &prefs) {
  prefs = {};
  const auto path = NovaExtraPrefs_SystemPath();
  if (!path) {
    return false;
  }
  std::ifstream in(*path);
  if (!in) {
    return false; // Missing on first run; the caller treats this as "no file".
  }
  NovaExtraPrefs_Parse(in, prefs, path->string());
  return true;
}

void NovaExtraPrefs_Write(std::ostream &out, const NovaExtraPrefs &prefs) {
  out << "# EV Nova SDL port extra preferences.\n"
         "# Port-only settings that have no slot in EV Nova Prefs.prf.\n"
         "# install_root: folder containing Nova.rez / Nova Files.\n"
         "# ui_scale / flight_scene_scale / mission_scale: presentation\n"
         "# multipliers in [0.5, 4]; 1.0 is native.\n"
         "[paths]\n";
  if (prefs.install_root) {
    out << kInstallRootKey << '=' << prefs.install_root->string() << '\n';
  }
  out << "\n[display]\n"
      << kUiScaleKey << '=' << FormatScale(prefs.scale.ui) << '\n'
      << kFlightSceneScaleKey << '=' << FormatScale(prefs.scale.flight_scene)
      << '\n'
      << kMissionScaleKey << '=' << FormatScale(prefs.scale.mission) << '\n';
  out << "\n# Clean-room fixes for confirmed bugs in the original game.\n"
         "# safe bundles the correctness fixes; the rest can be turned off\n"
         "# for a more faithful original experience.\n"
         "[bugfixes]\n"
      << kSafeBugFixesKey << '=' << BoolFlag(prefs.bugfixes.safe) << '\n'
      << kOutfitSlotBalanceKey << '='
      << BoolFlag(prefs.bugfixes.outfit_slot_balance) << '\n'
      << kOutfitPricesKey << '=' << BoolFlag(prefs.bugfixes.outfit_prices)
      << '\n'
      << kCronEventsKey << '=' << BoolFlag(prefs.bugfixes.cron_events) << '\n'
      << kParticleFogKey << '=' << BoolFlag(prefs.bugfixes.particle_fog)
      << '\n';
}

bool NovaExtraPrefs_SaveToSystemStore(const NovaExtraPrefs &prefs) {
  const auto path = NovaExtraPrefs_SystemPath();
  if (!path) {
    NovaLog::Error("extra prefs: the support folder is unavailable; cannot "
                   "save '{}'",
                   std::string{kExtraPrefsFileName});
    return false;
  }
  std::ofstream out(*path, std::ios::trunc);
  if (!out) {
    NovaLog::Error("extra prefs: could not open '{}' for writing",
                   path->string());
    return false;
  }
  NovaExtraPrefs_Write(out, prefs);
  out.flush();
  if (!out) {
    NovaLog::Error("extra prefs: failed while writing '{}'", path->string());
    return false;
  }
  NovaLog::Info("extra prefs: saved '{}'", path->string());
  return true;
}

PresentationScale
NovaExtraPrefs_ResolvePresentationScale(const NovaExtraPrefs &prefs) {
  PresentationScale scale = prefs.scale;
  if (const auto value = EnvScale("EVN_UI_SCALE")) {
    scale.ui = *value;
  }
  if (const auto value = EnvScale("EVN_FLIGHT_SCENE_SCALE")) {
    scale.flight_scene = *value;
  }
  if (const auto value = EnvScale("EVN_MISSION_SCALE")) {
    scale.mission = *value;
  }
  return scale;
}

namespace {

// Port-only Extra Prefs modal (no original counterpart). Reuses the Settings
// dialog chrome (DLOG 0xfa3) and native arrow art; the scale steppers,
// checkbox rows and buttons are port-authored.

struct ExtraPrefsLayout {
  SDL_FRect window{};
  SDL_FRect title_band{};
  SDL_FRect ok_button{};
  bool from_ditl = false;
};

[[nodiscard]] ExtraPrefsLayout BuildExtraPrefsLayout(const SDL_FRect &panel) {
  ExtraPrefsLayout out;
  const auto def = NovaResource_LoadDialogDefinition(kSettingsDialogId);
  const auto items = NovaResource_LoadDialogItems(kSettingsDialogId);
  if (!def || !items) {
    NovaLog::Todo("Extra Prefs: Settings DLOG/DITL 0x{:04x} unavailable",
                  kSettingsDialogId);
    return out;
  }
  const float win_w = static_cast<float>(def->right - def->left);
  // Height is port-extended past the authored DLOG to fit the bug-fix rows.
  out.window = CenterWindowOnPanel(panel, win_w, kExtraWindowHeight);
  // Reuse the Settings title band (DITL item 18); it stays pinned at the top.
  for (const auto &item : *items) {
    if (item.index == 18) {
      out.title_band = ItemRect(item, out.window);
      break;
    }
  }
  const float button_y =
      kExtraWindowHeight - kExtraButtonMargin - kExtraButtonHeight;
  // No Cancel button, so OK is centered.
  out.ok_button = SDL_FRect{(win_w - kExtraButtonWidth) * 0.5F,
                            button_y,
                            kExtraButtonWidth,
                            kExtraButtonHeight};
  out.from_ditl = true;
  return out;
}

enum class ExtraPrefsControl {
  none,
  ok,
  ui_up,
  ui_down,
  flight_up,
  flight_down,
  mission_up,
  mission_down,
  bugfix_safe,
  bugfix_outfit_slot_balance,
  bugfix_outfit_prices,
  bugfix_cron_events,
  bugfix_particle_fog,
};

// One row per bug-fix toggle, in dialog order. `safe` bundles the correctness
// fixes; the rest are independently selectable. The tooltip shows on hover.
struct ExtraBugFixRow {
  ExtraPrefsControl control;
  const char *label;
  const char *tooltip;
};

constexpr std::array<ExtraBugFixRow, 5> kExtraBugFixRows{{
    {ExtraPrefsControl::bugfix_safe,
     "Safe bug fixes",
     "Fixes for small issues that won't really affect your game. "
     "See the known bugs for a complete list. "
     "Recommended on."},
    {ExtraPrefsControl::bugfix_outfit_slot_balance,
     "Stacking max-gun/turret outfits",
     "Buying several max-gun/max-turret outfits, such as the Sigma upgrade, "
     "now works properly. "
     "Off gives N copies only one copy's bonus, as the original did."},
    {ExtraPrefsControl::bugfix_outfit_prices,
     "Rank discounts affect outfits",
     "Apply the allied-rank discount to outfit prices, as the shipyard "
     "already does. Off charges the normal outfit price, as the original "
     "did."},
    {ExtraPrefsControl::bugfix_cron_events,
     "Correct cr\x9an events",
     "Fixes several bugs with cr\x9an events. Should only be turned off "
     "if it breaks some specific plug-in that relied on bugged behaviour."},
    {ExtraPrefsControl::bugfix_particle_fog,
     "Apply murk to particles",
     "Nova did not apply murk to particles such as weapon trails or impacts. "
     "This seems like an oversight / performance issue at the time. "
     "Turning this on/off is purely visual, no gameplay impact."},
}};

// Dialog-local rects for one scale row's stacked arrow images. The drawn art is
// the native 11x9 PICT; the hit targets are padded so the tiny arrows are still
// easy to click.
[[nodiscard]] SDL_FRect ExtraUpRect(std::size_t row) {
  return SDL_FRect{kExtraArrowX, kExtraScaleRowY[row], 11.0F, 9.0F};
}

[[nodiscard]] SDL_FRect ExtraDownRect(std::size_t row) {
  return SDL_FRect{kExtraArrowX, kExtraScaleRowY[row] + 9.0F, 11.0F, 9.0F};
}

[[nodiscard]] SDL_FRect ExtraUpHitRect(std::size_t row) {
  return SDL_FRect{kExtraArrowX - kExtraArrowHitPadX,
                   kExtraScaleRowY[row] - kExtraArrowHitPadY,
                   11.0F + 2.0F * kExtraArrowHitPadX,
                   9.0F + kExtraArrowHitPadY};
}

[[nodiscard]] SDL_FRect ExtraDownHitRect(std::size_t row) {
  return SDL_FRect{kExtraArrowX - kExtraArrowHitPadX,
                   kExtraScaleRowY[row] + 9.0F,
                   11.0F + 2.0F * kExtraArrowHitPadX,
                   9.0F + kExtraArrowHitPadY};
}

// Top edge of one bug-fix checkbox row. The 'Safe bug fixes' row (0) sits on
// its own below the header; the altering fixes follow after an extra gap.
[[nodiscard]] float ExtraBugFixRowTop(std::size_t row) {
  float top =
      kExtraBugFixFirstRowY + kExtraBugFixRowStep * static_cast<float>(row);
  if (row > 0) {
    top += kExtraBugFixSafeGap;
  }
  return top;
}

// Dialog-local checkbox row for one bug-fix entry. The whole label line is a
// hit target, so hovering anywhere on the row shows the tooltip.
[[nodiscard]] SDL_FRect ExtraBugFixBoxRect(std::size_t row) {
  return SDL_FRect{kExtraBugFixBoxX, ExtraBugFixRowTop(row), 17.0F, 17.0F};
}

[[nodiscard]] SDL_FRect ExtraBugFixRowRect(std::size_t row) {
  return SDL_FRect{kExtraBugFixBoxX,
                   ExtraBugFixRowTop(row) - 1.0F,
                   kExtraValueX + kExtraValueWidth - kExtraBugFixBoxX,
                   19.0F};
}

// Pointer to the BugFixPolicy field a bug-fix control edits.
[[nodiscard]] bool *ExtraBugFixField(BugFixPolicy &policy,
                                     ExtraPrefsControl control) {
  switch (control) {
  case ExtraPrefsControl::bugfix_safe:
    return &policy.safe;
  case ExtraPrefsControl::bugfix_outfit_slot_balance:
    return &policy.outfit_slot_balance;
  case ExtraPrefsControl::bugfix_outfit_prices:
    return &policy.outfit_prices;
  case ExtraPrefsControl::bugfix_cron_events:
    return &policy.cron_events;
  case ExtraPrefsControl::bugfix_particle_fog:
    return &policy.particle_fog;
  default:
    return nullptr;
  }
}

[[nodiscard]] const bool *ExtraBugFixField(const BugFixPolicy &policy,
                                           ExtraPrefsControl control) {
  return ExtraBugFixField(const_cast<BugFixPolicy &>(policy), control);
}

// The tooltip for a hovered control, or empty when it has none.
[[nodiscard]] const char *ExtraTooltip(ExtraPrefsControl control) {
  for (const ExtraBugFixRow &row : kExtraBugFixRows) {
    if (row.control == control) {
      return row.tooltip;
    }
  }
  return nullptr;
}

// The port-only rects are dialog-local; shift them to playfield space.
[[nodiscard]] SDL_FRect OffsetRect(const SDL_FRect &rect,
                                   const SDL_FRect &window) {
  return SDL_FRect{window.x + rect.x, window.y + rect.y, rect.w, rect.h};
}

[[nodiscard]] ExtraPrefsControl
HitTestExtraPrefs(const ExtraPrefsLayout &layout, float x, float y) {
  auto hit = [&](const SDL_FRect &local) {
    return Contains(OffsetRect(local, layout.window), x, y);
  };
  if (hit(layout.ok_button)) {
    return ExtraPrefsControl::ok;
  }
  constexpr std::array<ExtraPrefsControl, 3> kUpControl{
      ExtraPrefsControl::ui_up,
      ExtraPrefsControl::mission_up,
      ExtraPrefsControl::flight_up};
  constexpr std::array<ExtraPrefsControl, 3> kDownControl{
      ExtraPrefsControl::ui_down,
      ExtraPrefsControl::mission_down,
      ExtraPrefsControl::flight_down};
  for (std::size_t row = 0; row < kExtraScaleRowY.size(); ++row) {
    if (hit(ExtraUpHitRect(row))) {
      return kUpControl[row];
    }
    if (hit(ExtraDownHitRect(row))) {
      return kDownControl[row];
    }
  }
  for (std::size_t row = 0; row < kExtraBugFixRows.size(); ++row) {
    if (hit(ExtraBugFixRowRect(row))) {
      return kExtraBugFixRows[row].control;
    }
  }
  return ExtraPrefsControl::none;
}

void DrawExtraArrow(SdlPlatform &platform,
                    NovaFontCache &font_cache,
                    const SettingsArtwork &artwork,
                    const SDL_FRect &rect,
                    bool up) {
  SdlTexture *const texture =
      (up ? artwork.arrow_up : artwork.arrow_down).get();
  if (texture != nullptr) {
    SDL_RenderTexture(platform.renderer(), texture->get(), nullptr, &rect);
  } else {
    DrawSliderArrow(platform, font_cache, rect, up, false);
  }
}

// Wraps a tooltip on spaces to at most `max_width` logical pixels using the
// same 11pt face it is drawn with.
[[nodiscard]] std::vector<std::string> WrapTooltipText(
    NovaFontCache &font_cache, std::string_view text, float max_width) {
  std::vector<std::string> lines;
  std::string current;
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t start = i;
    while (i < text.size() && text[i] != ' ') {
      ++i;
    }
    const std::string_view word = text.substr(start, i - start);
    while (i < text.size() && text[i] == ' ') {
      ++i;
    }
    if (word.empty()) {
      continue;
    }
    std::string candidate =
        current.empty() ? std::string{word} : current + " " + std::string{word};
    const int width = font_cache.TextWidth(
        NovaFontFamily::kGeneva, 11.0F, kNovaFontStyleRegular, candidate);
    if (!current.empty() && static_cast<float>(width) > max_width) {
      lines.push_back(std::move(current));
      current = std::string{word};
    } else {
      current = std::move(candidate);
    }
  }
  if (!current.empty()) {
    lines.push_back(std::move(current));
  }
  return lines;
}

// Draws a small wrapped tooltip near the cursor. It is drawn through a
// full-window overlay placement at the dialog's effective scale, so it can
// float past the dialog edges instead of being clipped by the dialog viewport
// (which previously forced the clamp to push bottom-row tooltips back up over
// the row labels). It is still clamped to the window.
void DrawExtraTooltip(SdlPlatform &platform,
                      NovaFontCache &font_cache,
                      const SDL_FPoint &mouse,
                      std::string_view text) {
  constexpr float kPad = 6.0F;
  constexpr float kWrapWidth = 230.0F;
  constexpr float kLineHeight = 14.0F;
  const std::vector<std::string> lines =
      WrapTooltipText(font_cache, text, kWrapWidth);
  if (lines.empty()) {
    return;
  }
  float box_w = 0.0F;
  for (const std::string &line : lines) {
    box_w = std::max(
        box_w,
        static_cast<float>(font_cache.TextWidth(
            NovaFontFamily::kGeneva, 11.0F, kNovaFontStyleRegular, line)));
  }
  box_w += 2.0F * kPad;
  const float box_h =
      2.0F * kPad + kLineHeight * static_cast<float>(lines.size());
  // Re-anchor into the overlay placement: convert the dialog-local cursor to
  // window points, then to the overlay's authored space. The overlay authored
  // extent is the whole window, so this also gives the clamp bounds.
  const Placement &dialog = platform.current_placement();
  const Placement overlay =
      PlaceWindow(platform.logical_playfield_size(), dialog.scale);
  const SDL_FPoint anchor = overlay.ToAuthored(dialog.ToWindow(mouse));
  const SDL_FPoint limit = overlay.authored_size;
  // Prefer below-right of the cursor, then keep the box on screen.
  const float x = std::clamp(
      anchor.x + 14.0F, 2.0F, std::max(2.0F, limit.x - box_w - 2.0F));
  const float y = std::clamp(
      anchor.y + 14.0F, 2.0F, std::max(2.0F, limit.y - box_h - 2.0F));
  // Swap to the overlay for the tooltip draw; the guard restores the dialog
  // placement (and its viewport) on scope exit.
  const SdlPlatform::ScopedPlacement guard(platform, overlay);
  SDL_Renderer *const renderer = platform.renderer();
  const SDL_FRect box{x, y, box_w, box_h};
  SDL_SetRenderDrawColor(renderer, 255, 255, 225, 245);
  SDL_RenderFillRect(renderer, &box);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &box);
  float baseline = y + kPad + 11.0F;
  for (const std::string &line : lines) {
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  11.0F,
                  kNovaFontStyleRegular,
                  kControlText,
                  x + kPad,
                  baseline,
                  line);
    baseline += kLineHeight;
  }
}

void DrawExtraPrefsDialog(SdlPlatform &platform,
                          NovaFontCache &font_cache,
                          const ExtraPrefsLayout &layout,
                          const PresentationScale &scale,
                          const BugFixPolicy &bugfixes,
                          const SettingsArtwork &artwork,
                          ExtraPrefsControl hover,
                          const SDL_FPoint &mouse,
                          const std::function<void()> &render_background) {
  SDL_Renderer *const renderer = platform.renderer();
  DrawOwningScreen(platform, render_background);
  platform.SetPlacement(PlaceContained({layout.window.w, layout.window.h},
                                       platform.logical_playfield_size(),
                                       platform.ui_scale()));
  if (!layout.from_ditl) {
    return;
  }

  const SDL_FRect &win = layout.window;
  SDL_SetRenderDrawColor(
      renderer, kWindowFill.r, kWindowFill.g, kWindowFill.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &win);
  SDL_SetRenderDrawColor(renderer,
                         kWindowFrame.r,
                         kWindowFrame.g,
                         kWindowFrame.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &win);

  // Title band, identical chrome to DrawSettingsDialog but with the port-only
  // title.
  const SDL_FRect &band = layout.title_band;
  if (band.w > 0.0F) {
    SDL_SetRenderDrawColor(renderer,
                           kWindowFill.r,
                           kWindowFill.g,
                           kWindowFill.b,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &band);
    SDL_SetRenderDrawColor(renderer,
                           kWindowFrame.r,
                           kWindowFrame.g,
                           kWindowFrame.b,
                           SDL_ALPHA_OPAQUE);
    SDL_RenderLine(renderer,
                   band.x,
                   band.y + band.h - 1.0F,
                   band.x + band.w,
                   band.y + band.h - 1.0F);
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kChicago,
                          18.0F,
                          kNovaFontStyleRegular,
                          kControlText,
                          band.x,
                          band.x + band.w,
                          band.y + 17.0F,
                          "Extra Prefs");
  }

  constexpr std::array<const char *, 3> kLabels{
      "UI/HUD Scale", "Mission Dialog Scale", "Flight Scene Scale"};
  // Mission multiplies UI (PresentationScale::mission_dialog), so it gets a
  // clarifying subline.
  const std::array<float, 3> values{
      scale.ui, scale.mission, scale.flight_scene};
  for (std::size_t row = 0; row < values.size(); ++row) {
    const float top = win.y + kExtraScaleRowY[row];
    NovaText_Draw(platform,
                  font_cache,
                  NovaFontFamily::kGeneva,
                  12.0F,
                  kNovaFontStyleRegular,
                  kControlText,
                  win.x + kExtraLabelX,
                  top + 14.0F,
                  kLabels[row]);
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kGeneva,
                          12.0F,
                          kNovaFontStyleRegular,
                          kControlText,
                          win.x + kExtraValueX,
                          win.x + kExtraValueX + kExtraValueWidth,
                          top + 14.0F,
                          fmt::format("{:.2f}x", values[row]));
    DrawExtraArrow(
        platform, font_cache, artwork, OffsetRect(ExtraUpRect(row), win), true);
    DrawExtraArrow(platform,
                   font_cache,
                   artwork,
                   OffsetRect(ExtraDownRect(row), win),
                   false);
    if (row == 1) {
      // Baseline dropped so the subline ascenders clear the label above it.
      NovaText_Draw(platform,
                    font_cache,
                    NovaFontFamily::kGeneva,
                    10.0F,
                    kNovaFontStyleRegular,
                    kControlDisabledText,
                    win.x + kExtraLabelX,
                    top + 26.0F,
                    "(multiplied by UI scale)");
    }
  }

  // Separator, enlarged header, then one checkbox row per policy flag.
  SDL_SetRenderDrawColor(renderer,
                         kControlDisabledHighlight.r,
                         kControlDisabledHighlight.g,
                         kControlDisabledHighlight.b,
                         SDL_ALPHA_OPAQUE);
  SDL_RenderLine(renderer,
                 win.x + kExtraLabelX,
                 win.y + kExtraBugFixHeaderY - 22.0F,
                 win.x + kExtraValueX + kExtraValueWidth,
                 win.y + kExtraBugFixHeaderY - 22.0F);
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                14.0F,
                kNovaFontStyleBold,
                kControlText,
                win.x + kExtraLabelX,
                win.y + kExtraBugFixHeaderY,
                "Bug Fixes");
  for (std::size_t row = 0; row < kExtraBugFixRows.size(); ++row) {
    const ExtraBugFixRow &entry = kExtraBugFixRows[row];
    const bool *field = ExtraBugFixField(bugfixes, entry.control);
    const bool checked = field != nullptr && *field;
    if (hover == entry.control) {
      const SDL_FRect row_rect = OffsetRect(ExtraBugFixRowRect(row), win);
      SDL_SetRenderDrawColor(renderer, 226, 226, 226, SDL_ALPHA_OPAQUE);
      SDL_RenderFillRect(renderer, &row_rect);
    }
    DrawCheckBox(platform,
                 font_cache,
                 OffsetRect(ExtraBugFixBoxRect(row), win),
                 entry.label,
                 checked,
                 false);
  }

  DrawButton(platform,
             font_cache,
             OffsetRect(layout.ok_button, win),
             "OK",
             hover == ExtraPrefsControl::ok);
  if (const char *tooltip = ExtraTooltip(hover); tooltip != nullptr) {
    DrawExtraTooltip(platform, font_cache, mouse, tooltip);
  }
}

constexpr float kExtraScaleStep = 0.05F;

// Step values stay on the 0.05 grid so repeated clicks cannot accumulate
// float noise (the display shows two decimals).
[[nodiscard]] float SnapScaleToStep(float value) {
  return std::round(value / kExtraScaleStep) * kExtraScaleStep;
}

// The three stepper rows edit working.scale fields in this order
// (UI, Mission, Flight) to match the dialog layout.
[[nodiscard]] float *ExtraScaleField(PresentationScale &scale,
                                     std::size_t row) {
  switch (row) {
  case 0:
    return &scale.ui;
  case 1:
    return &scale.mission;
  default:
    return &scale.flight_scene;
  }
}

} // namespace

// Extra Prefs modal entry point. Scale edits apply to `platform` live; closing
// (OK, Enter, Esc or quit) keeps and persists every edit, matching the original
// Settings dialog.
bool NovaMenu_RunExtraPrefsDialog(
    SdlPlatform &platform,
    NovaFontCache &font_cache,
    NovaExtraPrefs &extra_prefs,
    const std::function<void()> &render_background) {
  SdlPlatform::ScopedPlacement placement_guard(platform,
                                               platform.current_placement());
  const SDL_FRect panel{0.0F, 0.0F, 640.0F, 480.0F};
  const ExtraPrefsLayout layout = BuildExtraPrefsLayout(panel);
  if (!layout.from_ditl) {
    NovaLog::Todo("cancelled: Extra Prefs dialog chrome unavailable");
    return false;
  }
  const SettingsArtwork artwork = LoadSettingsArtwork(platform);
  NovaExtraPrefs working = extra_prefs;
  // Closing never rolls edits back: like the original Settings dialog, every
  // change applies live and stays applied after the modal closes. Closing
  // also persists to 'EV Nova Extra Prefs.ini' so nothing is lost on restart.
  auto commit = [&] {
    extra_prefs = working;
    (void)NovaExtraPrefs_SaveToSystemStore(extra_prefs);
  };
  // Apply the edited values directly, not the env-resolved scale. The
  // EVN_UI_SCALE / EVN_FLIGHT_SCENE_SCALE / EVN_MISSION_SCALE overrides are a
  // startup iteration aid; if they won here, editing the dialog would appear to
  // do nothing (the resolved scale would stay pinned to the env value). Env
  // overrides still apply on the next startup.
  auto apply_scale = [&] { platform.SetPresentationScale(working.scale); };
  apply_scale();
  NovaLog::Info("opening Extra Prefs dialog (port-only)");

  while (!platform.quit_requested()) {
    platform.SetPlacement(PlaceContained({layout.window.w, layout.window.h},
                                         platform.logical_playfield_size(),
                                         platform.ui_scale()));
    const SDL_FPoint mouse = platform.mouse_position();
    const ExtraPrefsControl hover = HitTestExtraPrefs(layout, mouse.x, mouse.y);
    DrawExtraPrefsDialog(platform,
                         font_cache,
                         layout,
                         working.scale,
                         working.bugfixes,
                         artwork,
                         hover,
                         mouse,
                         render_background);
    platform.Present();

    for (auto in = platform.PollTextEvent(); in;
         in = platform.PollTextEvent()) {
      if (in->key == TextKey::escape) {
        commit();
        return false; // Close (keep the applied scale and policy edits).
      }
      if (in->key == TextKey::enter) {
        commit();
        return true; // Enter acts as OK.
      }
      if (in->key != TextKey::primary) {
        continue;
      }
      const SDL_FPoint click = platform.mouse_position();
      const ExtraPrefsControl hit = HitTestExtraPrefs(layout, click.x, click.y);
      if (hit == ExtraPrefsControl::ok) {
        commit();
        return true;
      }
      if (bool *field = ExtraBugFixField(working.bugfixes, hit);
          field != nullptr) {
        *field = !*field; // Edited copy; commit() applies and persists it
        continue;
      }
      std::size_t row = 0;
      float delta = 0.0F;
      switch (hit) {
      case ExtraPrefsControl::ui_up:
        row = 0;
        delta = kExtraScaleStep;
        break;
      case ExtraPrefsControl::ui_down:
        row = 0;
        delta = -kExtraScaleStep;
        break;
      case ExtraPrefsControl::mission_up:
        row = 1;
        delta = kExtraScaleStep;
        break;
      case ExtraPrefsControl::mission_down:
        row = 1;
        delta = -kExtraScaleStep;
        break;
      case ExtraPrefsControl::flight_up:
        row = 2;
        delta = kExtraScaleStep;
        break;
      case ExtraPrefsControl::flight_down:
        row = 2;
        delta = -kExtraScaleStep;
        break;
      default:
        continue;
      }
      float *value = ExtraScaleField(working.scale, row);
      *value = std::clamp(SnapScaleToStep(*value + delta),
                          kPresentationScaleMin,
                          kPresentationScaleMax);
      apply_scale();
      NovaLog::Info("extra prefs: applied ui={:.2f} flight={:.2f} "
                    "mission={:.2f}",
                    platform.ui_scale(),
                    platform.flight_scene_scale(),
                    platform.mission_scale());
    }
    platform.PaceFrame();
  }
  commit(); // Quit request: keep the applied edits and persist them.
  return false;
}

} // namespace game
