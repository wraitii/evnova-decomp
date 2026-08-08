#pragma once

// Clean-room reconstruction of the Spaceport-style destination window. The
// original entry is confirmed as Stellar_TravelToSystem (0x00455e10) calling
// NovaUi_RunTravelDestinationInteractionLoop (0x00491f30), which creates
// DLOG 1000 (0x3e8). This SDL modal is still not wired to that travel path.
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
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <SDL3/SDL.h>

#include <vector>

#include "game_state.hpp"

class SdlPlatform;

namespace game {

// The service menu actions a docked player may pick, mirroring the original
// travel-destination services (the Spaceport screen: on the left Bar, Mission
// BBS, Trade center, Repair; on the right Shipyard, Outfitter, Refuel, Leave).
// The shipyard/buy-sell/bar stubs where out of scope are (mocked) in the MVP.
enum class LandedService : std::uint8_t {
  // Leave the dock and resume free flight over the stellar (the "Leave"
  // button at the bottom-right of the docked panel).
  kLaunch = 0,
  kRefuel,       // top up fuel toward the effective capacity
  kRepair,       // top up armor (and shields) toward effective maximums
  kBuySellCargo, // (mocked) the Trade center (commodity exchange)
  kOutfit,       // (mocked) the Outfitter
  kShipyard,     // (mocked) the Shipyard
  kBar,          // (mocked) the Bar
  kStarmap,      // (mocked) the Starmap (no on-screen slot; number-key only)
  kMissionBoard, // (mocked) the Mission BBS
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

// ---- Testable (SDL-free) dialog-window layout adapter ---------------------
// Small adapter that lays a dialog window (DLOG + DITL) out onto a logical
// panel, mirroring how the original centres a dialog window on the display
// (Dialog_CreateFromDlog). Used by the docked screen to drive its panels,
// header band, and buttons from the real Nova.rez dialog data instead of
// hardcoded geometry.

// The role each laid-out DITL item plays on the docked screen.
enum class DockedItemKind : std::uint8_t {
  kButton,     // a 145x25 service button
  kOuterPanel, // the large outer window panel
  kInnerPanel, // the inner content panel
  kTitleBand,  // the header band above the inner panel (holds the title)
  kOrnament,   // other decorative / sub-window-frame rects
};

// One laid-out docked item: its on-screen rect (logical panel space).
struct DockedItem {
  DockedItemKind kind = DockedItemKind::kOrnament;
  SDL_FRect rect = {0.0F, 0.0F, 0.0F, 0.0F};
};

// The laid-out docked screen: the on-screen dialog-window bounds plus every
// DITL item mapped to panel space. `from_ditl` is false when the dialog
// resources were unavailable and a fallback (buttons-only) layout was used.
struct DockedLayout {
  SDL_FRect window = {0.0F, 0.0F, 0.0F, 0.0F};
  bool from_ditl = false;
  std::vector<DockedItem> items;
};

// Loads the Spaceport dialog (DLOG 0x3e8 -> DITL 0x3e8) from Nova.rez and lays
// every item onto the logical `panel`, centering the dialog window on it and
// mapping item rects to panel space (screen = window_origin + dialog_rect).
// Returns false and produces a button-only fallback layout when the dialog
// resources cannot be decoded. Pure rect math; testable without a renderer.
bool NovaDialogWindow_Layout(const SDL_FRect &panel, DockedLayout &out);

// Physical docked-button order matches the original Spaceport screen: LEFT
// column (top-to-bottom) is Bar, Mission BBS, Trade center, Repair, and RIGHT
// column is Shipyard, Outfitter, Refuel, Leave. `side` is 0 (left) or 1
// (right); `row` is 0..3 top-to-bottom. The starmap service has no on-screen
// slot (it stays reachable via the number keys). These pure lookups are shared
// by the button builder and the keyboard navigation so they never drift.
[[nodiscard]] LandedService NovaDialog_DockedServiceAt(std::size_t side,
                                                       std::size_t row);

// Inverse of NovaDialog_DockedServiceAt: the (side, row) grid position of an
// on-screen docked service, or std::nullopt for a no-slot service (starmap).
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
NovaDialog_DockedGridOf(LandedService svc);

// Greedy word-wrap of a long description string into lines that each fit a
// `max_width` (device-independent) measurement. `measure` returns the width of
// a candidate line and must be consistent with the font actually drawn (the
// caller supplies the same NovaFontCache::TextWidth used for rendering). Words
// are never split; a single word wider than the row is emitted on its own line
// (the renderer may clip it). Pure text math; testable without a renderer.
[[nodiscard]] std::vector<std::string>
WrapDescriptionLines(std::string_view text,
                     int max_width,
                     const std::function<int(std::string_view)> &measure);

// ---- Testable (SDL-free) landed state --------------------------------------
// A docked session captures the destination stellar and the derived
// refuel/repair economics on a single landing. Kept off GameState so the modal
// can be nested and the service bookkeeping unit-tested without touching SDL.
// Why a landing request was not accepted (set by NovaLanding_EnterDocked).
// Maps to the on-screen HUD feedback text (STR# 0x7d2) the spaceflight loop
// shows; mirrors Stellar_ProcessTravelAndLanding's feedback cases.
enum class LandedDenial : std::uint8_t {
  kNone,         // the landing was accepted (ctx.landed == true)
  kUnavailable,  // no selected/valid ordinary stellar at all
  kTooFar,       // selected stellar is outside the 250-unit arrival envelope
  kTooFast,      // ship is moving too fast to dock (decomp constraint)
  kTooExpensive, // service_cost exceeds credits
};

struct LandedContext {
  // Resource id (>= 0x80) of the stellar we are docked at, or -1 before the
  // landing transition resolves it.
  std::int16_t stellar_id = -1;
  // Whether a landing transition ran for this session. No original caller has
  // yet been confirmed, so callers must establish this state independently.
  bool landed = false;
  // Why a landing was denied (kNone when accepted). Set by
  // NovaLanding_EnterDocked so the spaceflight loop can show the matching
  // STR# 0x7d2 HUD overlay instead of a bare log line.
  LandedDenial denial = LandedDenial::kNone;
  // The service most recently activated (by a mouse click or a letter
  // shortcut). The original has NO persistent keyboard focus/highlight state:
  // only a momentarily mouse-hovered slot is visually pressed, so this value is
  // used purely for dispatch, not for rendering a selection.
  LandedService selection = LandedService::kLaunch;
};

// Applies the normal-arrival subset of Stellar_ProcessTravelAndLanding
// (0x00457580) / Stellar_TravelToSystem (0x00455e10): the selected stellar
// must be an ordinary active destination and within the original's 250-unit
// per-axis arrival envelope. Then verifies its fee, stops/positions the ship,
// restores armor/shields, and prepares the Spaceport context. This keeps the
// modal UI out of the transition so its accounting is testable. Returns false
// without mutating the player when arrival cannot proceed.
[[nodiscard]] bool NovaLanding_EnterDocked(GameState &state,
                                           LandedContext &ctx);

// Refuels the player ship toward its effective fuel capacity. Mirrors the
// landed fuel service: the player pays a per-unit price for the fuel added,
// clamped so credits never go negative; capacity comes from the effective
// stats. Returns the credits actually spent (>= 0).
std::int32_t NovaLanded_Refuel(GameState &state, std::int32_t price_per_unit);

// Repairs armor (and shields) toward the effective maximums. NOTE: the original
// has no billed docked Repair service -- shields and armor are auto-refilled
// for free on landing by Stellar_TravelToSystem (0x00455e10). This helper
// models the delta top-up for completeness / unit tests only; the docked menu
// does not bill it. Returns the credits actually spent.
std::int32_t NovaLanded_Repair(GameState &state,
                               std::int32_t price_per_armor_point);

// ---- SDL modal -------------------------------------------------------------
// Runs the docked-screen modal for a stellar, driving navigation from the
// platform input channels. Owns a NovaFontCache for the duration of the modal
// (mirroring the original's DrawContext font state) and lays the window text
// out with the real screen fonts (Chicago/Charcoal titles + Geneva body).
// Renders the Spaceport backdrop (PICT 0x2134 via
// g_travel_overlay_sprite_handle, Ghidra FUN_0048e970) across the 640x480
// playfield, then draws the destination stellar's own planet picture (PICT
// 0x2710 + link_a_id, or its custom picture id at engage_highlight_frame >=
// 0x80, per FUN_0048e970) into the Spaceport DITL-0x3e8 outer panel at its
// natural 612x285 size. The destination name is centred in the header band and
// the services sit in the DITL-0x3e8 two-column (4-row) button layout at the
// lower left/right, driven by the same geometry as the mouse hit-test.
//
// Input follows the original, not a keyboard-focus model: a mouse click
// activates, the mouse-hovered slot is visually pressed, unavailable services
// draw grey, and the first-letter shortcuts (r/f refuel, c/t trade, o outfit,
// s shipyard, n mission, b bar; Enter/Esc leave) activate immediately. Returns
// the exit code describing how the window closed (see LandedExit).
// Normal in-range arrival reaches this DLOG 0x3e8 path through
// Stellar_ProcessTravelAndLanding / Stellar_TravelToSystem. The separate
// target-action DLOG 0x3f1 (bribe/hostility/script interaction) remains out
// of scope. In resolution-extension mode the
// playfield stays a fixed 640x480 centred with black borders; the F5 scale
// toggle (documented divergence) scales it to fill the window.
[[nodiscard]] LandedExit NovaLanded_RunWindow(SdlPlatform &platform,
                                              GameState &state,
                                              LandedContext &ctx);

} // namespace game
