#include "about_dialog.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "mission.hpp"
#include "selection_text_dialog.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace game {
namespace {

// Ghidra 0x004872a0 ('x' branch) and 0x00486120 (Menu_RunAboutNovaDialog)'s
// shared body: Ui_LoadSelectionDialogResource + Stellar_BuildTravelDestination-
// Description. The port expands the only wildcard it can source (<REG>); the
// rest of the description engine is unported.
[[nodiscard]] MissionDialogText LoadReaderText(std::uint16_t resource_id) {
  const auto description = NovaResource_LoadDescription(resource_id);
  if (!description) {
    NovaLog::Todo("selection-dialog description resource {} unavailable",
                  resource_id);
    return {};
  }
  MissionDialogText message;
  message.dialog_variant = description->dialog_variant;
  std::string text = description->text;

  // TODO(decomp(0x0045d100)) skipped: the original wildcard expansion fills
  // this from the registration record, which the port does not have yet.
  constexpr std::string_view marker = "<REG>";
  for (std::size_t pos = text.find(marker); pos != std::string::npos;
       pos = text.find(marker, pos + 12)) {
    text.replace(pos, marker.size(), "Unregistered");
  }
  message.text = std::move(text);
  return message;
}

} // namespace

// Ghidra 0x00486120 Menu_RunAboutNovaDialog.
void NovaMenu_RunAboutDialog(SdlPlatform &platform,
                             GameState &state,
                             const std::function<void()> &render_background) {
  const MissionDialogText message = LoadReaderText(0x7fff);
  NovaUi_RunTextReaderDialog(platform,
                             state,
                             message.text,
                             false,
                             render_background,
                             message.dialog_variant);
}

// Ghidra 0x004872a0 'x' branch (d\x91sc 0x7ffe ACKNOWLEDGEMENTS).
void NovaMenu_RunAcknowledgementsDialog(
    SdlPlatform &platform,
    GameState &state,
    const std::function<void()> &render_background) {
  const MissionDialogText message = LoadReaderText(0x7ffe);
  NovaUi_RunTextReaderDialog(platform,
                             state,
                             message.text,
                             false,
                             render_background,
                             message.dialog_variant);
}

} // namespace game
