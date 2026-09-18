#include "nova_paths.hpp"

#include "log.hpp"

#include <SDL3/SDL_filesystem.h>

#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

namespace {

// A directory "looks like an install" when it carries the main Nova.rez or a
// Nova Files folder (the same validity test the old in-file helper used).
[[nodiscard]] bool IsInstallRoot(const std::filesystem::path &path) {
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec) || ec) {
    return false;
  }
  return std::filesystem::exists(path / "Nova.rez", ec) ||
         std::filesystem::is_directory(path / "Nova Files", ec);
}

[[nodiscard]] std::optional<std::filesystem::path> FindInstallRoot() {
  std::vector<std::filesystem::path> candidates;
  // SDL3 returns a const pointer it caches internally and frees in
  // SDL_QuitFilesystem, so the caller must not free it. emplace_back copies.
  if (const char *base = SDL_GetBasePath()) {
    candidates.emplace_back(base);
  }
  candidates.emplace_back("EV Nova");
  candidates.emplace_back("../../../EV Nova");

  for (const auto &candidate : candidates) {
    if (IsInstallRoot(candidate)) {
      NovaLog::Info("install root: '{}'", candidate.string());
      return candidate;
    }
  }
  NovaLog::Todo("no EV Nova install folder was found next to the executable "
                "or relative to the working directory");
  return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> ResolveSupportDirectory() {
  char *raw = SDL_GetPrefPath("Ambrosia Software", "EV Nova");
  if (raw == nullptr) {
    NovaLog::Error("support folder: SDL_GetPrefPath failed: {}",
                   SDL_GetError());
    return std::nullopt;
  }
  const std::filesystem::path directory{raw};
  SDL_free(raw);
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    NovaLog::Error("support folder: could not create '{}': {}",
                   directory.string(),
                   ec.message());
    return std::nullopt;
  }
  return directory;
}

} // namespace

std::optional<std::filesystem::path> NovaPaths::InstallRoot() {
  static const std::optional<std::filesystem::path> root = FindInstallRoot();
  return root;
}

std::optional<std::filesystem::path> NovaPaths::SupportDirectory() {
  static const std::optional<std::filesystem::path> directory =
      ResolveSupportDirectory();
  return directory;
}

std::optional<std::filesystem::path>
NovaPaths::SupportSubdirectory(std::string_view name) {
  const auto support = SupportDirectory();
  if (!support) {
    return std::nullopt;
  }
  const std::filesystem::path directory =
      *support / std::filesystem::path{name};
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    NovaLog::Error("support folder: could not create '{}': {}",
                   directory.string(),
                   ec.message());
    return std::nullopt;
  }
  return directory;
}
