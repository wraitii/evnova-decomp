#pragma once

// On-disk layout resolution for the reimplementation.
//
// The original keeps a "Nova Support" folder that holds both the shipped game
// data (Nova.rez / Nova Files / Nova Plug-ins) and the per-user state (the
// prefs file, the Pilots folder and, on the Mac, user plug-ins). The port
// splits that into two locations:
//
//   install root   - read-only shipped data, resolved next to the executable
//   support folder - per-user writable state, from SDL_GetPrefPath
//
// This is the single place both are resolved so brgr_archive, preferences and
// pilot_file agree on the layout.

#include <filesystem>
#include <optional>
#include <string_view>

namespace NovaPaths {

// Shipped, read-only game root: the folder holding Nova.rez / Nova Files /
// Nova Plug-ins. Candidates in order, first one that looks like an install
// (has Nova.rez or a Nova Files directory) wins:
//
//   1. the executable's own directory (SDL_GetBasePath), so dropping the
//      reimplementation next to an existing install works unchanged;
//   2. "EV Nova" and "../../../EV Nova" relative to the working directory,
//      which is how the dev build and the ctest suite find the data.
//
// The CE build additionally lets the first command-line argument (or a
// ".nplay" file) pick a total-conversion folder. That override is deferred;
// TODO(decomp) add it together with a mod-manager design.
[[nodiscard]] std::optional<std::filesystem::path> InstallRoot();

// Per-user writable support folder, from SDL_GetPrefPath("Ambrosia Software",
// "EV Nova"). On macOS this is ~/Library/Application Support/EV Nova/. The
// directory is created on demand; nullopt when it cannot be resolved.
[[nodiscard]] std::optional<std::filesystem::path> SupportDirectory();

// A named child of SupportDirectory() (e.g. "Pilots", "Nova Plug-ins"),
// created on demand. nullopt when the support folder itself is unavailable.
[[nodiscard]] std::optional<std::filesystem::path>
SupportSubdirectory(std::string_view name);

} // namespace NovaPaths
