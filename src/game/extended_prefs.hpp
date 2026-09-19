#pragma once

// Port-only preferences that have no slot in the original 0x8c-byte
// `EV Nova Prefs.prf` payload. They live in a sibling INI file,
// `EV Nova Extra Prefs.ini`, under the per-user support folder resolved by
// NovaPaths. Keeping them separate means the original save format stays
// byte-faithful while the port can still persist its own settings (currently
// the user-selected EV Nova install root; see docs/preferences_keybindings.md).

#include <filesystem>
#include <optional>

namespace game {

struct NovaExtraPrefs {
  // User-selected EV Nova install root (the folder holding Nova.rez /
  // Nova Files). Empty when the normal candidate search should run.
  std::optional<std::filesystem::path> install_root;
};

// The INI path under the support folder, or nullopt when the support folder
// cannot be resolved. Does not create the file.
[[nodiscard]] std::optional<std::filesystem::path> NovaExtraPrefs_SystemPath();

// Loads the extra prefs. Returns false when the file is absent or unreadable;
// `prefs` is left empty in that case (a missing file is not an error). Unknown
// keys, comments and section headers are ignored so older/newer files still
// load.
[[nodiscard]] bool NovaExtraPrefs_LoadFromSystemStore(NovaExtraPrefs &prefs);

// Writes the extra prefs, creating the support folder if needed. Returns false
// and logs the reason on failure.
[[nodiscard]] bool
NovaExtraPrefs_SaveToSystemStore(const NovaExtraPrefs &prefs);

} // namespace game
