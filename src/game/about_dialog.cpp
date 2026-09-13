#include "about_dialog.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "selection_text_dialog.hpp"

#include <string>
#include <string_view>

namespace game {
namespace {

[[nodiscard]] std::string LoadAboutText() {
  const auto description = NovaResource_LoadDescription(0x7fff);
  if (!description) {
    NovaLog::Todo("About Nova description resource 0x7fff unavailable");
    return {};
  }
  std::string text = description->text;

  // TODO(decomp(0x0045d100)) skipped: the original wildcard expansion fills
  // this from the registration record, which the port does not have yet.
  constexpr std::string_view marker = "<REG>";
  for (std::size_t pos = text.find(marker); pos != std::string::npos;
       pos = text.find(marker, pos + 12)) {
    text.replace(pos, marker.size(), "Unregistered");
  }
  return text;
}

} // namespace

// Ghidra 0x00486120 Menu_RunAboutNovaDialog.
void NovaMenu_RunAboutDialog(SdlPlatform &platform,
                             GameState &state,
                             const std::function<void()> &render_background) {
  NovaUi_RunTextReaderDialog(
      platform, state, LoadAboutText(), false, render_background);
}

} // namespace game
