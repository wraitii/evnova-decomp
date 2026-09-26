#pragma once

// Port-only preferences that have no slot in the original 0x8c-byte
// `EV Nova Prefs.prf` payload. They live in a sibling INI file,
// `EV Nova Extra Prefs.ini`, under the per-user support folder resolved by
// NovaPaths. Keeping them separate means the original save format stays
// byte-faithful while the port can still persist its own settings: the
// user-selected EV Nova install root and the presentation multipliers
// (see docs/preferences_keybindings.md and docs/display_scaling.md).

#include "presentation_scale.hpp"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string_view>

namespace game {

struct NovaExtraPrefs {
  // User-selected EV Nova install root (the folder holding Nova.rez /
  // Nova Files). Empty when the normal candidate search should run.
  std::optional<std::filesystem::path> install_root;
  // Presentation multipliers. Values are already validated on parse; the
  // default is neutral (1.0).
  PresentationScale scale{};
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

// Parses extra-prefs INI text. `source` is used only for warnings. Recognized
// keys outside `[paths]`/`[display]` still load; unknown keys are ignored.
void NovaExtraPrefs_Parse(std::istream &in,
                          NovaExtraPrefs &prefs,
                          std::string_view source);

// Writes the complete extra-prefs INI text. The on-disk file is replaced
// wholesale, so every field must be emitted or it is lost.
void NovaExtraPrefs_Write(std::ostream &out, const NovaExtraPrefs &prefs);

// Resolves the effective presentation scale from the loaded prefs, then any
// `EVN_UI_SCALE` / `EVN_FLIGHT_SCENE_SCALE` / `EVN_MISSION_SCALE` environment
// override. The override is a debug aid for iterating without restarting from
// the INI; invalid values are ignored with a warning.
[[nodiscard]] PresentationScale
NovaExtraPrefs_ResolvePresentationScale(const NovaExtraPrefs &prefs);

} // namespace game
