#pragma once

// Clean-room reconstruction of the sub-window dialogs the game opens over the
// docked Spaceport screen during the stellar landing procedure. In the
// original (Stellar_TravelToSystem 0x00455e10 -> NovaUi_RunTravelDestination-
// ServicesWindow 0x0047c8e0 and siblings) each docked service (Bar, Mission
// BBS, Trade center, Shipyard, Outfitter, Starmap, Communications) presents a
// nested modal window over the still-visible docked backing store, backed by
// one of the sub-window frame PICTs from Nova Graphics 3.rez:
//
//   0x2135 Shipyard, 0x2136 Outfit, 0x2137 Bar, 0x2139 Mission BBS,
//   0x213d Map, 0x213e Trade, 0x213f Communications
//
// This module renders that dialog window on-screen instead of the previous
// NovaLog::Todo mocks: it re-layers the docked backdrop (PICT 0x2134) under a
// dim scrim, draws the service's frame PICT centered at natural size, places a
// three-state "Leave" button, and runs a small modal loop (Esc/Enter/click
// closes back to the dock menu). The service *content* (buy/sell tables,
// outfit list, shipyard purchase, bar holovid/gamble, map navigation) remains
// out of scope; each dialog is a faithful window frame with a heading, not the
// service internals.

#include <cstdint>

#include "landed_window.hpp"

class SdlPlatform;

namespace game {

// The sub-window frame PICT resource id backing a docked service. Returns the
// documented Nova Graphics 3 PICT id, or 0 when the service has no frame art
// (Launch/Refuel/Repair are not sub-windows; they dispatch inline).
[[nodiscard]] std::uint16_t
NovaDocked_SubWindowFramePict(LandedService service);

// Runs one sub-window dialog modal over the docked screen for `service`. Draws
// the docked backdrop (PICT 0x2134) across the full 640x480 playfield, a dim
// scrim, then the service's frame PICT (NovaDocked_SubWindowFramePict)
// centered at its natural size, with the service heading and a grey-backed
// "Leave" button. Loops until the player closes the dialog (Esc, Enter, or a
// click/primary press anywhere), the platform quits (returns kQuit), or an
// internal "launch" choice (future) escalates to kLaunched. The docked menu's
// centred playfield is re-asserted each frame. Mirrors the nested-modality of
// the original sub-windows over the same backing store.
[[nodiscard]] LandedExit NovaLanded_RunSubWindowDialog(SdlPlatform &platform,
                                                       GameState &state,
                                                       LandedService service,
                                                       std::int16_t stellar_id);

} // namespace game
