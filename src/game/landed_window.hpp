#pragma once

// Clean-room reconstruction of the "landed window" -- the UI shown while the
// player is docked at a stellar (planet/station). Mirrors the original's
// docked screen (the Spaceport window, Ghidra NovaUi_RunTravelDestination-
// InteractionLoop 0x00491f30 creating DLOG 0x3e8, and its backdrop/button draw
// helpers), exposed as a self-contained modal run-loop on top of SDL.
//
// SCOPE / ARCHITECTURE: the original is a full GVNO UiWindow docking screen
// built from a dialog resource (DLOG 0x3e8 -> DITL 0x3e8), filling a near-
// full-screen panel with the destination-art PICT 0x2134 backdrop
// (FUN_0048e970) and the service controls in a two-column button layout down
// the lower left/right edges (plus the docked sub-window frame PICTs in Nova
// Graphics 3: 0x2135 Shipyard, 0x2136 Outfit, 0x2137 Bar, 0x2139 Mission BBS,
// 0x213d Map, 0x213e Trade, 0x213f Communications). The
// buy/sell/outfit/shipyard/bar/starmap/mission sub-windows are nested modals
// over the same backing store. That whole GVNO UI stack is NOT reconstructed
// here. This build presents the same landing *architecture* -- a persistent
// docked screen with a header (destination name + credits) and the two-column
// service grid -- driven by the same input channel, where each service is an
// entry point into a (mocked where out of scope) sub-screen.
//
// OUT OF SCOPE for the MVP (each is a loud NovaLog::Todo stub): cargo buy/sell,
// outfitting, shipyard purchasing, the bar mini-game, starmap, mission
// computer, and the landing/launch cinematic transitions. The generic
// housekeeping that the original always performs on landing (refill shields
// and armor, deduct the stellar service cost) is real; refueling and armor
// repair are handled as actual state mutations so the loop is exerciseable.

#include <cstdint>

#include "game_state.hpp"

class SdlPlatform;

namespace game {

// The service menu actions a docked player may pick, mirroring the ordering of
// the original travel-destination services buttons (services window 0x47c8e0;
// shipyard/buy-sell/bar stubs where out of scope).
enum class LandedService : std::uint8_t {
  // Leave the dock and resume free flight over the stellar (the "get up and
  // go / launch" head of the list).
  kLaunch = 0,
  kRefuel,       // top up fuel toward the effective capacity
  kRepair,       // top up armor (and shields) toward effective maximums
  kBuySellCargo, // (mocked) cargo trading
  kOutfit,       // (mocked) change ship loadout
  kShipyard,     // (mocked) buy/sell ships
  kBar,          // (mocked) bar mini-game / local rumours
  kStarmap,      // (mocked) system navigation map
  kMissionBoard, // (mocked) mission computer
  kCount,
};

// Return code from the landed-window modal. The run loop returns
// kLaunched when the player picks "launch" (back to the spaceflight loop),
// kQuit when the platform quit latch trips, kServiceComplete when a sub-screen
// returned normally without leaving the dock.
enum class LandedExit : std::uint8_t {
  kLaunched,
  kQuit,
  kServiceComplete,
};

// ---- Testable (SDL-free) landed state --------------------------------------
// A docked session captures the destination stellar and the derived
// refuel/repair economics on a single landing. Kept off GameState so the modal
// can be nested and the service bookkeeping unit-tested without touching SDL.
struct LandedContext {
  // Resource id (>= 0x80) of the stellar we are docked at, or -1 before the
  // landing transition resolves it.
  std::int16_t stellar_id = -1;
  // Whether a landing transition actually ran for this session (the player may
  // open the dock with a "try to refuel" walk-up). Mirrors Stellar_LandOnSpob
  // being a prerequisite of the docked state.
  bool landed = false;
  // The currently focused service row (0..kCount-1). The MVP uses the same
  // channel as spaceflight targeting (up/down) so no new input surface is
  // needed.
  LandedService selection = LandedService::kLaunch;
};

// Performs the landing transition for the current selected stellar: repositions
// the ship at the stellar, zeroes velocity, refills shields/armor from the
// effective maximums, and deducts the stellar's service cost (clamped >= 0
// credits). Faithful subset of Ghidra Stellar_LandOnSpob (0x00456480). Call
// before NovaLanded_RunWindow starts its modal. Returns true when a landing
// was performed.
bool NovaLanding_EnterDocked(GameState &state, LandedContext &ctx);

// Refuels the player ship toward its effective fuel capacity. Mirrors the
// landed fuel service: the player pays a per-unit price for the fuel added,
// clamped so credits never go negative; capacity comes from the effective
// stats. Returns the credits actually spent (>= 0).
std::int32_t NovaLanded_Refuel(GameState &state, std::int32_t price_per_unit);

// Repairs armor (and shields) toward the effective maximums. The original hides
// the repair price behind a fixed shop cost model; the MVP charges a flat
// per-point rate for the gap. Returns the credits actually spent.
std::int32_t NovaLanded_Repair(GameState &state,
                               std::int32_t price_per_armor_point);

// ---- SDL modal -------------------------------------------------------------
// Runs the docked-screen modal for a stellar, driving navigation from the
// platform input channels. Owns a NovaFontCache for the duration of the modal
// (mirroring the original's DrawContext font state) and lays the window text
// out with the real screen fonts (Chicago/Charcoal titles + Geneva body).
// Renders the docked-screen backdrop (destination-art PICT 0x2134, ~618x517)
// across the whole 640x480 panel, mirroring Ghidra FUN_0048e970
// (g_travel_overlay_sprite_handle = Resource_LoadPictAsImage(0x2134)) blitting
// it over the full window rect. The destination name is centred near the top
// and the services sit in the DITL-0x3e8 two-column (4-row) button layout at
// the lower left/right, driven by the same geometry as the mouse hit-test.
// Returns the exit code describing how the window closed (see LandedExit).
// `state.player` must already be positioned at the dock (e.g. after
// NovaLanding_EnterDocked).
[[nodiscard]] LandedExit NovaLanded_RunWindow(SdlPlatform &platform,
                                              GameState &state,
                                              LandedContext &ctx);

} // namespace game
