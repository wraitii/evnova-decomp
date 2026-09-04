#pragma once

// Clean-room reconstruction of the in-space ship-comm modal (the "hail a
// ship" window), from the E/target-action pathway's ship branch -- the
// counterpart of the stellar-conversation window in negotiation_dialog.cpp.
// In the original the spaceflight loop's Ship_HandlePlayerTargetActionCommand
// (0x00454910 ship branch) calls NovaUi_RunTargetShipCommWindow (0x0047e470),
// which builds DLOG 0x3ef over the flight scene, backed by PICT 0x213f (the
// Communications frame), with the target ship picture/name panel and three
// context buttons:
//
//   Close Channel (Leave) / Greetings (primary comm action) /
//   Request Assistance | Beg For Mercy | Release (secondary action)
//
// (labels from the STR# 0x96 "button labels" pool entries 0x14/0x15/0x16/
// 0x18/0x1f). The window is laid out from DLOG/DITL 0x3ef: the 423x215 PICT
// 0x213f backdrop is centred on the 640x480 playfield, with the 200x200 ship
// portrait (ShipClass pict_fallback_sprite_resource_id = PICT 5000+class,
// overridden by the pers HailPict, from
// NovaData_LoadAllShipClassVisualAndLaunchData 0x004aeda0) in DITL item 10 on
// the right, the word-wrapped comm prompt panel in item 9 and the Class:/
// comm-name/Status: block in item 11 (drawn by
// NovaUi_DrawTargetShipCommWindow 0x0047fb70, renamed 2024 from the
// misleading "...BribeWindow" label), and the three
// context buttons stacked *vertically* on the lower-left in DITL
// items 0/1/2 (Close Channel at the bottom, Request Assistance / Beg For
// Mercy / Release in the middle, Greetings at the top; each button's label
// matches its action -- slot 1 runs the assistance dialogue, slot 2 shows
// the hail-info text), mirroring NovaUi_DrawTravelDestinationContextButtons'
// DAT_007d82ee/f0/f2 label table.
// The assistance button is hidden for special-scan-mask governments (mirroring
// the same function's local_10[1] gate), and the Greetings button then drops
// down to the middle rect (item 1, y=153). Behavior-6 ships with no AI target
// and no mission fleet open the escort-management window instead (not
// reconstructed; see scope).
//
// The comm state machine the original keeps in globals is derived here and
// carried explicitly on the dialog frame: the per-launch random flavour index
// (g_travel_interaction_random_index), the ship "personality" factor
// (_DAT_007d17ec, drives the bribe price and the mood prompt 0x17/0x1c/0x18),
// the bribe cost (g_travel_bribe_cost), the bribe-offered latch
// (g_travel_interaction_bribe_offered), and the government scan-mask gates
// (local_11/1d/1e + DAT_007d17f4 in the original). Prompt text is loaded from
// STR# 0xbb8 (prompt_index < 0x26) / STR# 0xbb9 (>= 0x26) at
// `index*5 + random + 1` exactly like NovaUi_LoadTravelDestinationPrompt-
// String (0x004828c0). The dialog honours the original keyboard shortcuts
// (NovaUi_PollTargetShipCommWindow 0x0047fa40): Enter/'e'/Esc close the
// channel, 'r' triggers the assistance button (Request Assistance | Beg For
// Mercy | Release), 'g' the Greetings button.
//
// SCOPE: the window shell, the initial prompt selection, the Greetings state
// machine (disabled / escort cargo-transfer / keep-pressing-target /
// comm-aid / distress / fuel-bribe branches), the bribe payment sub-window
// (single-pass DLOG 0x3f0-style confirm, 35% acceptance, 0.75x discount,
// +1000 haggle) and the escort release side-effects on close are
// reconstructed. Deferred with loud Todo(decomp) logs: the escort-management
// window handoff (behavior-6 ships, NovaUi_RunEscortShipManagementWindow
// 0x004853a0), the mission-fleet escort-def branches (fleet defs not
// modelled), the full hail-info text assembly beyond the default fragment
// (NovaUi_BuildShipCommHailInfoText 0x004819d0 branch 0) and
// Outfit_TransferCargoAndJunkToEscortByRatio (0x00469810).

#include <cstdint>

#include "game_state.hpp"

class SdlPlatform;

namespace game {

class HudRenderer;
class SpaceflightView;

// Runs the DLOG 0x3ef ship-comm modal for the ship in `ship_slot` (must be a
// valid active NPC slot). Draws PICT 0x213f as the window backdrop over the
// live flight view (SpaceflightView::DrawGameFrame; the original composites
// its DLOG over the unmodified gameplay surface), shows the 200x200 ship
// portrait (ShipClass
// pict_fallback_sprite_resource_id) on the right and name/government panel on
// the left, and loops the three context buttons (stacked vertically on the
// lower-left) until the player closes the channel (Esc/Enter/'e'/Close
// Channel), the platform quits, or the internal escort-transfer latch is
// armed (the window closes once the transfer prompt is shown, mirroring the
// original's one-shot escort release). May mutate the target ship's AI state
// (escort release, state 0x09/0x0F/0x04 entries, hostile flip) and the
// player's credits (bribe payment). Returns false when the dialog could not
// be opened (slot invalid / inactive).
[[nodiscard]] bool NovaShipComm_RunShipDialog(SdlPlatform &platform,
                                              GameState &state,
                                              std::int16_t ship_slot,
                                              SpaceflightView &view,
                                              HudRenderer &hud);

// Mirrors the bVar1 hail-eligibility gate of Ship_HandlePlayerTargetAction-
// Command (0x00454910): the player can hail `target` only when the ship is
// not disabled, is not a 0x3ff mission slot, is not ship class 0x2ff,
// and (when it holds an AI target or a mission fleet) neither its own nor its
// class's inherent government carries the busy flag (flags_primary 0x400).
[[nodiscard]] bool NovaShipComm_TargetEligibleForHail(const GameState &state,
                                                      const Ship &target);

} // namespace game
