#pragma once

// Port-only preferences with no slot in the original `EV Nova Prefs.prf`
// payload. They live in `EV Nova Extra Prefs.ini` under the per-user support
// folder (NovaPaths) so the original save format stays byte-faithful: install
// root, presentation multipliers and the runtime bug-fix policy (see
// docs/preferences_keybindings.md). This translation unit also owns the
// port-only Extra Prefs modal.

#include "presentation_scale.hpp"

#include "compatibility.hpp"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string_view>

class SdlPlatform;

namespace game {

class NovaFontCache;

struct NovaExtraPrefs {
  // Empty when the normal install-root candidate search should run.
  std::optional<std::filesystem::path> install_root;
  PresentationScale scale{};
  // Clean-room bug-fix policy (see compatibility.hpp), edited live by the UI.
  BugFixPolicy bugfixes{};
};

// The INI path under the support folder, or nullopt if it cannot be resolved.
[[nodiscard]] std::optional<std::filesystem::path> NovaExtraPrefs_SystemPath();

// Loads the extra prefs. A missing/unreadable file returns false and leaves
// `prefs` empty; unknown keys, comments and section headers are ignored.
[[nodiscard]] bool NovaExtraPrefs_LoadFromSystemStore(NovaExtraPrefs &prefs);

// Writes the extra prefs, creating the support folder if needed.
[[nodiscard]] bool
NovaExtraPrefs_SaveToSystemStore(const NovaExtraPrefs &prefs);

// Parses extra-prefs INI text; `source` is only for warnings. Unknown keys are
// ignored.
void NovaExtraPrefs_Parse(std::istream &in,
                          NovaExtraPrefs &prefs,
                          std::string_view source);

// Writes the complete INI. The file is replaced wholesale, so every field must
// be emitted or it is lost.
void NovaExtraPrefs_Write(std::ostream &out, const NovaExtraPrefs &prefs);

// Runs the port-only Extra Prefs modal. Reuses the Settings dialog chrome
// (DLOG 0xfa3) and native arrow art; the steppers, checkboxes and OK button
// are port-authored. Edits `extra_prefs` in place and never rolls an edit back
// on close: OK, Enter, Esc and a quit request all keep the changes and persist
// the INI. Bug-fix changes reach the game via the caller's `bugfixes` copy.
bool NovaMenu_RunExtraPrefsDialog(
    SdlPlatform &platform,
    NovaFontCache &font_cache,
    NovaExtraPrefs &extra_prefs,
    const std::function<void()> &render_background = {});

} // namespace game
