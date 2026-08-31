#pragma once

#include "../sdl_platform.hpp"

#include <functional>

namespace game {

class NovaFontCache;

// Runs the main-menu ABOUT NOVA modal (Ghidra 0x00486120, named
// Menu_OpenGalaxyMapDialog there; the sp\n95n 605 button is labelled ABOUT
// NOVA and the dialog loads d\x91sc 0x7fff "About text", so the Galaxy/Starmap
// role in the Ghidra plate is a misnomer). The original builds it on
// Ui_LoadSelectionDialogResource + Ui_RunTravelSelectionDialog over DLOG
// 0xbbb; this port keeps the DLOG geometry (window 441x313, text area, OK
// button, scroll arrows) but draws the scrolling text region itself.
//
// render_background is invoked each frame before the dialog is drawn so the
// menu stays visible behind the modal. Returns when the OK button is clicked
// or Enter/Escape is pressed.
void NovaMenu_RunAboutDialog(SdlPlatform &platform,
                             NovaFontCache &font_cache,
                             const std::function<void()> &render_background);

} // namespace game
