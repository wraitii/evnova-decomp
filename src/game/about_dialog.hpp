#pragma once

#include "../sdl_platform.hpp"

#include <functional>

namespace game {

struct GameState;

// Runs the main-menu ABOUT NOVA modal (Ghidra 0x00486120
// Menu_RunAboutNovaDialog; the sp\n95n 605 button is labelled ABOUT NOVA and
// the dialog loads d\x91sc 0x7fff "About text"). The original builds it on
// Ui_LoadSelectionDialogResource + Ui_RunTravelSelectionDialog over DLOG
// 0xbbb; the port uses that shared reader too.
//
// render_background is invoked each frame before the dialog is drawn so the
// menu stays visible behind the modal. Returns when the OK button is clicked
// or Enter/Escape is pressed.
void NovaMenu_RunAboutDialog(SdlPlatform &platform,
                             GameState &state,
                             const std::function<void()> &render_background);

// Runs the 'x' main-menu ACKNOWLEDGEMENTS reader (Ghidra 0x004872a0's 'x'
// branch): the same shared reader over d\x91sc 0x7ffe. The original invokes
// this directly from NovaCommand_DispatchToMode rather than through
// NovaGameMode_DispatchAction.
void NovaMenu_RunAcknowledgementsDialog(
    SdlPlatform &platform,
    GameState &state,
    const std::function<void()> &render_background);

} // namespace game
