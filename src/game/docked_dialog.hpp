#pragma once

// Clean-room reconstruction of the sub-window dialogs the game opens over the
// docked Spaceport screen during the stellar landing procedure. In the
// original (Stellar_RunDockAndLaunchSequence 0x00455e10 ->
// NovaUi_RunTravelDestination- ServicesWindow 0x0047c8e0 and siblings) each
// docked service (Bar, Mission BBS, Trade center, Shipyard, Outfitter, Starmap,
// Communications) presents a nested modal window over the still-visible docked
// backing store, backed by one of the sub-window frame PICTs from Nova
// Graphics 3.rez:
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

#include <array>
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

// Modal layering (deliberate divergence, see docs/dlog_ditl_dialog_format.md):
// every sub-window re-renders the preserved underlying screen each frame (the
// docked Spaceport menu, or the live flight view) and layers its DLOG window
// on top, instead of compositing over a captured snapshot.

// The sub-window frame PICT resource id backing a docked service. Returns the
// documented Nova Graphics 3 PICT id, or 0 when the service has no frame art
// (Launch/Refuel/Repair are not sub-windows; they dispatch inline).
[[nodiscard]] std::uint16_t
NovaLanded_SubWindowFramePict(LandedService service);

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

// Ghidra 0x0047d600 NovaUi_ComposeTravelNewsTexts (crön-news arm): selects the
// active crön event's news STR# id for the landed stellar. An event past its
// holdoff contributes local news when one of its NewsGovt ids is allied with
// the stellar's government (using that entry's GovtNewsStr, last match wins),
// else independent news when IndNewsStr is set. Local wins over independent;
// the id is drawn uniformly across the contributing slots. Returns nullopt
// when no active event contributes. Exposed for tests; deterministic given
// the caller's RNG state.
[[nodiscard]] std::optional<std::int16_t>
Bar_SelectCronNewsStr(GameState &state, std::int16_t landed_stellar_id);

// Ghidra 0x0047d180 NovaUi_RunTravelNewsWindow (prologue): resolves the
// background PICT id for the Bar Holovid window at `stellar_id`: the stellar
// government's news_pic_id when set, else the generic PICT 9000. Stellar
// government ids are zero-based (the loader rebases them -0x80), so this goes
// through the index lookup, not the 0x80-based resource-id Government().
// Exposed so the Federation/ICN regression can be pinned by tests.
[[nodiscard]] std::uint16_t NovaBar_NewsPictId(const GameState &state,
                                               std::int16_t stellar_id);

// The `dësc` resource id backing the Bar/destination prompt for a landed
// stellar. Ghidra 0x0047c8e0 passes g_ship_states->ai_secondary_target_slot +
// 10000, where that slot is the 0-based g_stellar_defs index -- so this is
// (stellar_id - 0x80) + 10000, e.g. Earth (0x80) -> 10000 "Bars: Earth" and
// Port Kane (0x89) -> 10009 "The Hypergate". Distinct from the landing
// description (NovaResource_LoadStellarDescription), which is keyed by raw
// stellar id. Returns nullopt for a non-0x80-based stellar id.
[[nodiscard]] std::optional<std::uint16_t>
NovaLanded_BarDescriptionId(std::int16_t stellar_id);

// Window-relative (left, top, right, bottom) rects for the Holovid news text
// panels, from NovaUi_DrawTravelNewsWindow 0x0047d370: the headline band is
// (10,140)-(w-10,180) and the body (10,170)-(w-10,h-4). The two overlap by
// 10px; the body is drawn second and covers it. Exposed so the shipped
// 300x230 geometry can be pinned by tests.
struct NewsTextPanels {
  std::array<float, 4> headline{};
  std::array<float, 4> body{};
};

[[nodiscard]] NewsTextPanels NovaBar_NewsTextPanelRects(float window_w,
                                                        float window_h);

// Runs one sub-window dialog modal over the docked screen for `service`.
// `render_background` re-renders the docked menu each frame; the dialog adds
// a dim scrim, then the service's frame PICT (NovaLanded_SubWindowFramePict)
// centered at its natural size, with the service heading and a grey-backed
// "Leave" button. Loops until the player closes the dialog (Esc, Enter, or a
// click/primary press anywhere), the platform quits (returns kQuit), or an
// internal "launch" choice (future) escalates to kLaunched. Mirrors the
// nested-modality of the original sub-windows over the same backing store.
[[nodiscard]] LandedExit NovaLanded_RunSubWindowDialog(
    SdlPlatform &platform,
    SdlAudio &audio,
    GameState &state,
    LandedService service,
    std::int16_t stellar_id,
    const std::function<void()> &render_background = {});

// Ghidra 0x00442510 NovaUi_RunMissionOfferWindow. Shows mission definition
// `mission_def`'s dësc
// (mission_def + 4000) in a read-only text view over DLOG 0x3f8, with the
// Accept/Decline buttons (captions from the mïsn payload +0x75f/+0x77f,
// defaulting to STR# 0x96 entries 0x32/0x33). Accept activates the mission at
// the slot with `landed_stellar_id` as the BBS context; decline runs the
// payload +0x58 text reader and the +0x25a reaction script. Returns the
// offer outcome for Mission_RunAvailLocOffers' latch handling.
// A dësc variant >= 0x80 selects the custom-art arm: DLOG 0x3fc (DITL 1020)
// with backdrop PICT 0x2150 and the variant PICT in entry 8.
// The target-action path (Ship_HandlePlayerTargetActionCommand 0x00454910)
// reuses this shell for eligible AvailLoc 2 offers; it passes -1 for the
// landed-stellar context and supplies the live-flight render callback. The
// starmap (action 4), player-special (action 5) and mission-computer
// (action 7) sub-actions are wired, matching 0x00442510; the mission computer
// (when it opens) uses `audio` for its cues. Flags 0x0004 ("can't refuse")
// draws/hit-tests the single accept button from DITL entry 6 and suppresses
// the decline slot, as 0x004a1820/0x004a1670 do. TODO(decomp) skipped: the
// status-string panel (Ui_PlayMovieFileModal, DITL entry 4 -- a QuickTime
// platform replacement). Port conveniences beyond the original: Esc counts as
// decline, DIK arrows scroll.
[[nodiscard]] MissionOfferResult
NovaMission_RunOfferWindow(SdlPlatform &platform,
                           SdlAudio &audio,
                           GameState &state,
                           std::int16_t mission_def,
                           std::int16_t landed_stellar_id,
                           const std::function<void()> &render_background = {});

// Ghidra 0x00446150 NovaUi_RunMissionComputerWindow: the in-flight "mission
// computer" window (gameplay command 0x28, default key I) listing the
// pilot's active missions with each selection's quick-brief text. Renders
// through `render_background` (the caller supplies the live flight frame in
// flight, or the docked menu when opened from the BBS/offer window), exactly
// like RunMissionBbsWindow/NovaMission_RunOfferWindow. Supports the starmap
// action (with the selected mission's destination preselect) and
// aborting missions whose CanAbort latch is set (flags 0x40 apply the -5x
// CompReward reputation reversal). See docked_mission_dialog.cpp for the
// ported helper sites and skips. Opening with zero visible missions is refused
// by the caller (Ship_HandlePlayerShipCore plays the denied cue and the STR#
// 0x7d2 0x162 overlay instead).
void NovaMission_RunMissionInfoWindow(
    SdlPlatform &platform,
    SdlAudio &audio,
    GameState &state,
    const std::function<void()> &render_background);

} // namespace game
