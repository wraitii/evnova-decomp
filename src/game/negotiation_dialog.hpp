#pragma once

// Clean-room reconstruction of the in-space destination-interaction modal
// (the "negotiate a landing" window), from the E/target-action pathway that is
// distinct from normal arrival docking. In the original the spaceflight loop's
// Ship_HandlePlayerTargetActionCommand (0x004418d0 stellar branch) calls
// NovaUi_RunTravelDestinationInteractionWindow (0x00480030), which builds
// DLOG 0x3f1 over the flight scene, backed by PICT 0x2140, with the three
// primary- (context) buttons laid out by NovaUi_DrawTravelDestinationPrimary-
// Buttons (0x004a0f90). This module mirrors docked_dialog: it renders a modal
// window on screen and runs a small SDL loop, but drives the
// landing-negotiation state machine instead of the Spaceport service list.
//
// The negotiation state the original keeps in globals is derived here and
// carried explicitly on GameState (bribe cost, denied/hostile latches, per-
// system reputation). The three interlinked windows share this state:
//
//   DLOG 0x3f1  the destination-interaction window (this module);
//   DLOG 0x3f0  the payment modal (NovaUi_RunTravelDestinationPaymentWindow
//               0x00482280) shown over the interaction window when a bribe is
//               accepted, backed by PICT 0x2142;
//   DLOG 0x3e8  the normal Spaceport window reached after a successful
//               land/bribe (not built here; hand off through
//               state.travel.selected_stellar_id).
//
// SCOPE: the bribe-cost computation, denied/hostile derivation, bribe-
// eligibility gate, the land/status primary action and the payment window are
// reconstructed. The attack/confrontation branch (reputation decrement, hostile
// ship spawns via _Stellar_SpawnHostileShipForStellar, and Mission_ExecuteReac-
// tionScript) and the price-haggle counter-offer in the payment window are
// deferred with loud Todo(decomp) logs (AGENTS.md "leave" decisions).

#include <cstdint>
#include <random>

#include "game_state.hpp"
#include "landed_window.hpp"

class SdlPlatform;

namespace game {

// Return code from the negotiation modal. The spaceflight loop either returns
// to free flight (kClosed / kQuit) or, once the player has successfully landed
// or paid a bribe to dock, runs the normal Spaceport window for the stellar.
enum class NegotiationExit : std::uint8_t {
  kClosed,        // dismissed (Leave / Esc / close) without buying in
  kQuit,          // the platform quit latch tripped
  kProceedToLand, // the player secured docking (land or paid bribe); the
                  // caller should open the Spaceport for the target stellar
};

// Computes the credits-scaled base of the destination-interaction bribe
// (credits), mirroring the opening block of
// NovaUi_RunTravelDestinationInteractionWindow (0x00480030): a random pick over
// `credits * 1e-06` drawn from `rng` (NovaRandom_Range), expressed as
// `pick * 1000 + 3000`, then clamped down to 1/3 of credits, rounded down to
// the nearest 1000 and clamped to [1000, 900000]. The returning base is NOT yet
// scaled for the government's 1.5x "bribes the player" flag (that scale is
// applied by NovaNegotiation_RunDestinationDialog when it resolves the
// faction), so the helper ignores `government_id`; passing the same `rng` makes
// the cost reproducible per session.
[[nodiscard]] std::int32_t NovaNegotiation_ComputeBribeCost(
    std::mt19937 &rng, std::int32_t credits, std::int16_t government_id);

// Runs the DLOG 0x3f1 destination-interaction modal for the given stellar
// (resource id >= 0x80). Draws PICT 0x2140 as the dialog backdrop on a dim
// scrim over a black playfield, shows the target's status/prompt text (from
// STR# 0xbb8/0xbb9/0xbba via NovaHud_LoadStringEntry) and three buttons
// (Leave, Land/Bribe, Attack), and loops until the player closes it, pays a
// bribe to dock (returns kProceedToLand with state.travel.selected_stellar_id
// set), or the platform quits. The attack button is deferred (loud Todo).
// Mirrors the nested-modality of the original interaction window over the
// flight scene.
[[nodiscard]] NegotiationExit NovaNegotiation_RunDestinationDialog(
    SdlPlatform &platform, GameState &state, std::int16_t stellar_id);

} // namespace game
