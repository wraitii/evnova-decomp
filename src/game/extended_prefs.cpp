#include "extended_prefs.hpp"

#include "../log.hpp"
#include "../nova_paths.hpp"

#include <fmt/format.h>

#include <cstdlib>
#include <fstream>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace game {
namespace {

constexpr std::string_view kExtraPrefsFileName = "EV Nova Extra Prefs.ini";
constexpr std::string_view kInstallRootKey = "install_root";
constexpr std::string_view kUiScaleKey = "ui_scale";
constexpr std::string_view kFlightSceneScaleKey = "flight_scene_scale";
constexpr std::string_view kMissionScaleKey = "mission_scale";

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
         "# multipliers in [0.5, 4]; 1.0 is native. See "
         "docs/display_scaling.md.\n"
         "[paths]\n";
  if (prefs.install_root) {
    out << kInstallRootKey << '=' << prefs.install_root->string() << '\n';
  }
  out << "\n[display]\n"
      << kUiScaleKey << '=' << FormatScale(prefs.scale.ui) << '\n'
      << kFlightSceneScaleKey << '=' << FormatScale(prefs.scale.flight_scene)
      << '\n'
      << kMissionScaleKey << '=' << FormatScale(prefs.scale.mission) << '\n';
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

} // namespace game
