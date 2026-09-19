#pragma once

// Clean-room reconstruction of the Spaceport-style destination window. The
// original entry is confirmed as Stellar_RunDockAndLaunchSequence (0x00455e10)
// calling NovaUi_RunTravelDestinationInteractionLoop (0x00491f30), which
// creates DLOG 1000 (0x3e8). This SDL modal is still not wired to that travel
// path.
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
// TODO(decomp): cargo buy/sell, outfitting, shipyard purchasing, the bar
// mini-game, starmap, mission computer, and the landing/launch cinematic
// transitions. The arrival-side
// accounting that the original always performs (fee deduction, auto-refuel-
// ler refuel) is real; shield/armor refill and the calendar tick happen at
// LAUNCH (Stellar_RunDockAndLaunchSequence's post-loop tail), not at arrival.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <SDL3/SDL.h>

#include <vector>

#include "game_state.hpp"

class SdlPlatform;
class SdlAudio;
class HudRenderer;

namespace game {

struct NovaPreferences;

// The service menu actions a docked player may pick, mirroring the original
// travel-destination services.  The Spaceport has exactly seven actions;
// their visual placement comes from the DITL item indices, not a synthetic
// two-column service grid.
// TODO(decomp): shipyard, buy/sell, and bar service content is not implemented.
enum class LandedService : std::uint8_t {
  // Leave the dock and resume free flight over the stellar (the "Leave"
  // button at the bottom-right of the docked panel).
  kLaunch = 0,
  kRefuel,
  kBuySellCargo,
  kOutfit,
  kShipyard,
  kMissionBbs,
  kBar,
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
  std::size_t ditl_index = 0;
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

// Maps the original Spaceport's zero-based DITL item ordinal to its action.
// Ghidra's UI calls name the same controls by one-based item numbers 12, 4, 7,
// 8, 9, 10, and 11.
[[nodiscard]] std::optional<LandedService>
NovaDialog_DockedServiceForDitlItem(std::size_t ditl_index);

// Greedy word-wrap of a long description string into lines that each fit a
// `max_width` (device-independent) measurement. `measure` returns the width of
// a candidate line and must be consistent with the font actually drawn (the
// caller supplies the same NovaFontCache::TextWidth used for rendering). Words
// are never split; a single word wider than the row is emitted on its own line
// (the renderer may clip it). An explicit '\r'/`\n` terminates the line; a run
// of two (the "...\r\rRequires: ..." desc form) leaves an empty line, matching
// the original DrawTextW(DT_WORDBREAK) layout. Pure text math; testable without
// a renderer.
[[nodiscard]] std::vector<std::string>
WrapDescriptionLines(std::string_view text,
                     int max_width,
                     const std::function<int(std::string_view)> &measure);

// ---- Testable (SDL-free) landed state --------------------------------------
// A docked session captures the destination stellar and the derived
// refuel/repair economics on a single landing. Kept off GameState so the modal
// can be nested and the service bookkeeping unit-tested without touching SDL.
// Why a landing request was not accepted (set by Stellar_Dock).
// Maps to the on-screen HUD feedback text (STR# 0x7d2) the spaceflight loop
// shows; mirrors Stellar_HandleStellarEntryAndExit's feedback cases.
enum class LandedDenial : std::uint8_t {
  kNone,         // the landing was accepted (ctx.landed == true)
  kUnavailable,  // no selected/valid ordinary stellar at all
  kUnauthorized, // reputation/scan-mask/mission/government policy denied
  kTooFar,       // outside the envelope or the approach request is unarmed
  kTooFast,      // within range but still moving / maneuvering
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
  // Stellar_Dock so the spaceflight loop can show the matching
  // STR# 0x7d2 HUD overlay instead of a bare log line.
  LandedDenial denial = LandedDenial::kNone;
  // The service most recently activated (by a mouse click or a letter
  // shortcut). The original has NO persistent keyboard focus/highlight state:
  // only a momentarily mouse-hovered slot is visually pressed, so this value is
  // used purely for dispatch, not for rendering a selection.
  LandedService selection = LandedService::kLaunch;
};

// Stellar_MaxLandingDistance: per-axis half-extent of the normal-arrival
// envelope for a target stellar (Stellar_HandleStellarEntryAndExit 0x00457580
// gate, inlined at 0x00458786..0x004587a9). Returns 75 (0x4b) when
// target_sprite_full_width <= 0 (no prepared sprite), else
// round(target_sprite_full_width * 1.75) with the 1.75 double
// k_stellar_arrival_envelope_scale_f64 (0x005756a0). The landing is in range
// only when BOTH axis deltas are strictly less than this value (a square
// envelope, not a radius). The input is Sprite_GetFrameFullWidth on the
// link_a spin set (full frame width); the 250/0xfa check belongs to the
// starmap travel-arm branch (0x00459369), not this normal dock gate.
[[nodiscard]] float
Stellar_MaxLandingDistance(std::int16_t target_sprite_full_width);

// Ghidra 0x004250f0 Player_RefuelShipWithCredits. Arrival auto-refuel for the
// auto-refueller outfit (ModType 19): when the player owns one, rounds fuel
// to the nearest unit and tops it up to the effective capacity at exactly
// 1 credit per unit, clamped to the available credits. Runs from the
// Stellar_RunDockAndLaunchSequence arrival subset before the Spaceport
// interaction loop.
void Player_RefuelShipWithCredits(GameState &state);

// Ghidra 0x00455e10 Stellar_RunDockAndLaunchSequence, arrival half (the
// clean-room split into Stellar_Dock + Stellar_Launch). After the arrival
// envelope/approach gate above (engage timer >= 0x2ee, |vel| <= 0.75 per axis,
// ai_maneuver_timer_ms <= 0), verifies and deducts the stellar fee, runs the
// arrival-side auto-refueller refuel (Player_RefuelShipWithCredits 0x004250f0),
// reconciles the outfit pool, and prepares the Spaceport context. This also
// folds in the caller Stellar_HandleStellarEntryAndExit (0x00457580)'s
// normal-arrival gate/fee, kept in place rather than extracted. Keeps the modal
// UI out of the transition so its accounting is testable. Returns false without
// mutating the player when arrival cannot proceed. Ship meters and the calendar
// are NOT touched here: the original restores/refills them in the LAUNCH tail
// (0x00455f99..0x00456268), after the interaction loop returns -- see
// Stellar_Launch.
[[nodiscard]] bool Stellar_Dock(GameState &state,
                                LandedContext &ctx,
                                std::int16_t target_sprite_full_width = 0);

// Ghidra 0x00455e10 Stellar_RunDockAndLaunchSequence, launch tail (0x00455f99..
// 0x00456268): runs when the destination-interaction loop returns, i.e. on
// leaving the dock. Zeroes velocity and repositions the ship at the queued
// travel stellar (ai_secondary_target_slot, which a docked M may have
// repointed at the destination system's first nav, unless a docked N latched
// g_skip_player_reposition_once), refills shields and armor to the effective
// maxima, runs the single
// daily world tick (0x00456033 -- so the Spaceport's mission gate sees the
// pre-landing date), jitters/rerolls the persisted stat modifiers, saves the
// pilot, rolls a random launch heading, resets the travel selection, and
// wipes the transient shot pool. The caller then shows the departure overlay
// (0x00456323, NovaHud_ShowLaunchDepartureMessage) and resyncs its frame
// clock (the original zeroes g_avg_frame_tick_scale at 0x00456174).
void Stellar_Launch(GameState &state);

// Refuels the player ship toward its effective fuel capacity. Mirrors the
// landed fuel service: the player pays a per-unit price for the fuel added,
// clamped so credits never go negative; capacity comes from the effective
// stats. Returns the credits actually spent (>= 0).
std::int32_t NovaLanded_Refuel(GameState &state, std::int32_t price_per_unit);

// Repairs armor (and shields) toward the effective maximums. NOTE: the original
// has no billed docked Repair service -- shields and armor are auto-refilled
// for free on landing by Stellar_RunDockAndLaunchSequence (0x00455e10). This
// helper models the delta top-up for completeness / unit tests only; the docked
// menu does not bill it. Returns the credits actually spent.
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
// 0x2710 + link_a_id, or its custom picture id at CustPicID >=
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
// Stellar_HandleStellarEntryAndExit / Stellar_RunDockAndLaunchSequence. The
// separate target-action DLOG 0x3f1 (bribe/hostility/script interaction)
// remains out of scope. In resolution-extension mode the playfield stays a
// fixed 640x480 centred with black borders; the F5 scale toggle (documented
// divergence) scales it to fill the window.
// `hud` is the live spaceflight HUD (pitched at the same interface layout as
// the flight overlay). It is composited behind the docked dialog with the
// radar forced empty, matching Ghidra 0x00491f30 redrawing the gameplay
// viewport/radar/cargo panel before presenting the centered Spaceport window.
[[nodiscard]] LandedExit NovaLanded_RunWindow(SdlPlatform &platform,
                                              SdlAudio &audio,
                                              GameState &state,
                                              LandedContext &ctx,
                                              const NovaPreferences &prefs,
                                              HudRenderer &hud);

} // namespace game
