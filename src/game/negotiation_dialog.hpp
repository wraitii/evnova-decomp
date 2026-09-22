#pragma once

// Clean-room reconstruction of the in-space destination-interaction modal
// (the "hail a planet/station" window), from the target-action pathway that
// is distinct from normal arrival docking. In the original the spaceflight
// loop's Ship_HandlePlayerTargetActionCommand (0x004418d0 stellar branch)
// calls NovaUi_RunTravelDestinationInteractionWindow (0x00480030), which
// builds DLOG 0x3f1 over the flight scene, backed by PICT 0x2140. Despite the
// "negotiation" shape the window is a communications channel: the STR# 0xbba
// status text opens with "Communications channel open to <stellar>." and the
// buttons (labels from the STR# 0x96 pool via the DAT_007d83aa cache, see
// NovaUi_DrawTravelDestinationPrimaryButtons 0x004a0f90) are
//
//   Close Channel (bottom) / Greetings | Offer Bribe (top) /
//   Demand Tribute | Release (middle, disabled for dominated uninhabited
//   bodies)
//
// Landing NEVER happens through this window's buttons: it stays on the normal
// second-E landing-request flow. The one exception is a successful bribe
// (Offer Bribe -> DLOG 0x3f0 payment window 0x00482280): the original then
// shows the "<name>, you're cleared to land. Commence final approach." HUD
// overlay and sets the engage handoff directly.
//
// The negotiation state the original keeps in globals is derived here and
// carried explicitly (bribe cost, denied latch, bribe-offer latch, per-system
// reputation). All window geometry is laid out from the real DLOG/DITL 0x3f1
// resources (a 540x295 backdrop PICT 0x2140 centred on the 640x480 playfield)
// with hard-coded fallbacks only when the resources fail to load.
//
// SCOPE: the bribe-cost computation, denied derivation (incl. the government
// policy-flag override), bribe-eligibility gate, Greetings/refusal status
// ladders, the payment window (rendered each frame over the interaction
// window, boarding-plunder style), the bribe-success landing handoff, and the
// Demand Tribute / Release middle-button branch (reputation decrement,
// Government_ProcessFactionCombatEvent, defense-fleet spawns via
// NovaStellar_SpawnDefenseFleetShip, the domination latch, and
// Mission_ExecuteReactionScript on the stellar's OnDominate/OnRelease
// scripts) are reconstructed.

#include <cstdint>
#include <random>

#include "game_state.hpp"
#include "landed_window.hpp"

class SdlPlatform;

namespace game {

class HudRenderer;
class SpaceflightView;

// Return code from the negotiation modal.
enum class NegotiationExit : std::uint8_t {
  kClosed,
  kQuit,
};

// Computes the credits-scaled base of the destination-interaction bribe
// (credits), mirroring the opening block of
// NovaUi_RunTravelDestinationInteractionWindow (0x00480030): a random pick over
// `credits * 1e-06` drawn from `rng` (NovaRandom_Range), expressed as
// `pick * 1000 + 3000`, then clamped down to 1/3 of credits, rounded down to
// the nearest 1000 and clamped to [1000, 900000]. The returned base is NOT yet
// scaled for the government's 1.5x "bribes the player" flag (that scale is
// applied by NovaNegotiation_RunDestinationDialog when it resolves the
// faction), so the helper ignores `government_id`; passing the same `rng`
// makes the cost reproducible per session.
[[nodiscard]] std::int32_t NovaNegotiation_ComputeBribeCost(
    std::mt19937 &rng, std::int32_t credits, std::int16_t government_id);

// Runs the DLOG 0x3f1 destination-interaction modal for the given stellar
// (resource id >= 0x80). Draws PICT 0x2140 as the dialog backdrop over the
// live flight view (SpaceflightView::DrawGameFrame; the original composites
// its DLOG over the unmodified gameplay surface), shows the target's
// status/prompt text (STR# 0xbba / 0xbb8 flavour variants), the header block
// (name, destination description, Status: word) and the three comm buttons,
// and loops until the player closes the channel, pays an accepted bribe (which
// arms the normal proximity-based landing approach), or the platform quits.
// The middle Demand Tribute / Release button runs the domination / release
// branch (reputation drop, faction crime event, defense-fleet spawn,
// OnDominate / OnRelease scripts).
[[nodiscard]] NegotiationExit
NovaNegotiation_RunDestinationDialog(SdlPlatform &platform,
                                     GameState &state,
                                     std::int16_t stellar_id,
                                     SpaceflightView &view,
                                     HudRenderer &hud);

} // namespace game
