#include "extended_prefs.hpp"

#include "../log.hpp"
#include "../nova_paths.hpp"

#include <fstream>
#include <string>
#include <string_view>

namespace game {
namespace {

constexpr std::string_view kExtraPrefsFileName = "EV Nova Extra Prefs.ini";
constexpr std::string_view kInstallRootKey = "install_root";

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

} // namespace

std::optional<std::filesystem::path> NovaExtraPrefs_SystemPath() {
  const auto support = NovaPaths::SupportDirectory();
  if (!support) {
    return std::nullopt;
  }
  return *support / std::filesystem::path{kExtraPrefsFileName};
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
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    const std::string_view trimmed = Trim(line);
    if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';' ||
        trimmed.front() == '[') {
      continue;
    }
    const std::size_t separator = trimmed.find('=');
    if (separator == std::string_view::npos) {
      NovaLog::Warn("extra prefs '{}': line {} has no '=', ignoring",
                    path->string(),
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
    } else {
      NovaLog::Warn("extra prefs '{}': unknown key '{}' on line {}, ignoring",
                    path->string(),
                    key,
                    line_number);
    }
  }
  return true;
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
  out << "# EV Nova SDL port extra preferences.\n"
         "# Port-only settings that have no slot in EV Nova Prefs.prf.\n"
         "# install_root: folder containing Nova.rez / Nova Files.\n"
         "[paths]\n";
  if (prefs.install_root) {
    out << kInstallRootKey << '=' << prefs.install_root->string() << '\n';
  }
  out.flush();
  if (!out) {
    NovaLog::Error("extra prefs: failed while writing '{}'", path->string());
    return false;
  }
  NovaLog::Info("extra prefs: saved '{}'", path->string());
  return true;
}

} // namespace game
