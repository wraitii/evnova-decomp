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
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "landed_window.hpp"
#include "mission.hpp"

class SdlAudio;
class SdlPlatform;
class SdlTexture;
struct SDL_Texture;

namespace game {

class HudRenderer;
class SpaceflightView;

// Modal layering (deliberate divergence, see docs/dlog_ditl_dialog_format.md):
// every sub-window re-renders the preserved underlying screen each frame (the
// docked Spaceport menu, or the live flight view) and layers its DLOG window
// on top, instead of compositing over a captured snapshot.

// The sub-window frame PICT resource id backing a docked service. Returns the
// documented Nova Graphics 3 PICT id, or 0 when the service has no frame art
// (Launch/Refuel/Repair are not sub-windows; they dispatch inline).
[[nodiscard]] std::uint16_t
NovaDocked_SubWindowFramePict(LandedService service);

// Ghidra 0x0047d600 NovaUi_ComposeTravelNewsTexts (disaster-report arm).
// Builds the Holovid news body for an active öops disaster: it prefers a
// record on `landed_stellar_id` (0x80-based) or a target -2 "everywhere"
// record, else any active one, and folds its name, price direction, commodity
// and host stellar into the STR# 0x7d2 template fragments. Returns nullopt
// when no eligible named record is active (the caller then uses the generic
// STR# 0x1fa5 news fallback). Extracted from NovaBar_ComposeNewsTexts so the
// composition can be pinned by tests. Consulted from the Bar Holovid window.
[[nodiscard]] std::optional<std::string>
Bar_ComposeDisasterReport(GameState &state, std::int16_t landed_stellar_id);

// The `dësc` resource id backing the Bar/destination prompt for a landed
// stellar. Ghidra 0x0047c8e0 passes g_ship_states->ai_secondary_target_slot +
// 10000, where that slot is the 0-based g_stellar_defs index -- so this is
// (stellar_id - 0x80) + 10000, e.g. Earth (0x80) -> 10000 "Bars: Earth" and
// Port Kane (0x89) -> 10009 "The Hypergate". Distinct from the landing
// description (NovaResource_LoadStellarDescription), which is keyed by raw
// stellar id. Returns nullopt for a non-0x80-based stellar id.
[[nodiscard]] std::optional<std::uint16_t>
NovaDocked_BarDescriptionId(std::int16_t stellar_id);

// Runs one sub-window dialog modal over the docked screen for `service`.
// `render_background` re-renders the docked menu each frame; the dialog adds
// a dim scrim, then the service's frame PICT (NovaDocked_SubWindowFramePict)
// centered at its natural size, with the service heading and a grey-backed
// "Leave" button. Loops until the player closes the dialog (Esc, Enter, or a
// click/primary press anywhere), the platform quits (returns kQuit), or an
// internal "launch" choice (future) escalates to kLaunched. Mirrors the
// nested-modality of the original sub-windows over the same backing store.
[[nodiscard]] LandedExit NovaLanded_RunSubWindowDialog(
    SdlPlatform &platform,
    GameState &state,
    LandedService service,
    std::int16_t stellar_id,
    const std::function<void()> &render_background = {});

// Ghidra 0x00442510 NovaUi_RunMissionShipInteractionWindow (partial port: the
// text-offer arm). Shows mission definition `mission_def`'s dësc
// (mission_def + 4000) in a read-only text view over DLOG 0x3f8, with the
// Accept/Decline buttons (captions from the mïsn payload +0x75f/+0x77f,
// defaulting to STR# 0x96 entries 0x32/0x33). Accept activates the mission at
// the slot with `landed_stellar_id` as the BBS context; decline runs nothing
// (the payload decline script/reaction chain is not wired yet). Returns the
// offer outcome for Mission_TriggerLandingInteractions' latch handling.
// TODO(decomp) skipped: the mission-ship/hail branches (AvailLoc 2), the
// variant >= 0x80 DLOG 0x3fc art path, the status-string panel (DITL entry 4),
// and the starmap/special-interaction/mission-computer actions (4/5/7). The
// decline arm now shows the payload +0x58 desc via the text reader and runs
// the +0x25a reaction script; the accept arm runs the 0x0043f100 Brief/
// LoadCarg acceptance dialogs. Port conveniences beyond the original: Esc
// counts as decline, DIK arrows scroll.
[[nodiscard]] MissionOfferResult
NovaMission_RunOfferWindow(SdlPlatform &platform,
                           GameState &state,
                           std::int16_t mission_def,
                           std::int16_t landed_stellar_id,
                           const std::function<void()> &render_background = {});

// Ghidra 0x00446150 NovaUi_RunMissionComputerWindow: the in-flight "mission
// computer" window (gameplay command 0x28, default key I) listing the
// pilot's active missions with each selection's quick-brief text. Renders
// the live flight view beneath itself (SpaceflightView::DrawGameFrame, the
// boarding/comm-dialog pattern). Supports the starmap action (with the
// selected mission's destination preselect) and
// aborting missions whose CanAbort latch is set (flags 0x40 apply the -5x
// CompReward reputation reversal). See docked_dialog.cpp for the ported
// helper sites and skips. Opening with zero visible missions is refused by
// the caller (Ship_HandlePlayerShipCore plays the denied cue and the STR#
// 0x7d2 0x162 overlay instead).
void NovaMission_RunMissionInfoWindow(SdlPlatform &platform,
                                      SdlAudio &audio,
                                      GameState &state,
                                      SpaceflightView &view,
                                      HudRenderer &hud);

} // namespace game
