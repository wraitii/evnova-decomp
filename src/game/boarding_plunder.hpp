#pragma once

// Clean-room reconstruction of EV Nova's boarding / plunder window system.
//
// The original flow is:
//   Player_HandleBoardTargetCommand (0x0045a3d0)  validates the player's
//     board command against the primary target (range, relative velocity,
//     heading alignment, target boardability), dispatches mission or carrier
//     arms, and otherwise opens:
//   NovaUi_RunBoardingPlunderWindow   (0x00482940)  the modal window over
//     DLOG 0x3f3 (309x198, DITL 0x3f3: 6 buttons + one text panel), fed by
//   Boarding_BuildOptions  (0x00484230)  which pre-rolls the
//     randomized plunder offers (cargo, credits, ammo, fuel) and the capture
//     odds into the boarding globals (DAT_007d17d0..e0,
//     g_capture_odds_percent).
//
// This module ports the option builder and the modal window. The window's
// layout is taken from the actual DLOG/DITL resources at runtime (no
// hardcoded geometry), its texts from the STR# pools, its buttons from the
// shared three-state button art, and its backdrop from PICT 0x2143.

#include <cstdint>

#include "game_state.hpp"

class SdlAudio;
class SdlPlatform;

namespace game {

class HudRenderer;
class SpaceflightView;

// The boarding-window offer set. Replaces the original's globals:
//   cargo_type          DAT_007d17d0  (0..6 commodity bin, -1 = no offer)
//   cargo_quantity      DAT_007d17d2
//   ammo_bank           DAT_007d17d4  (weapon bank id, -1 = no offer)
//   ammo_quantity       DAT_007d17d6
//   fuel_quantity       DAT_007d17d8
//   credits             DAT_007d17e0  (-1 = no offer)
//   capture_odds        g_capture_odds_percent (0 = capture not offered)
struct BoardingPlunderOptions {
  std::int16_t cargo_type = -1;
  std::int16_t cargo_quantity = 0;
  std::int16_t ammo_bank = -1;
  std::int16_t ammo_quantity = 0;
  std::int16_t fuel_quantity = 0;
  std::int32_t credits = -1;
  std::int16_t capture_odds_percent = 0;

  [[nodiscard]] bool cargo_offer() const { return cargo_type != -1; }

  [[nodiscard]] bool credits_offer() const { return credits >= 1; }

  [[nodiscard]] bool ammo_offer() const { return ammo_bank != -1; }

  [[nodiscard]] bool fuel_offer() const { return fuel_quantity >= 1; }

  [[nodiscard]] bool capture_offer() const { return capture_odds_percent >= 1; }
};

// Ghidra 0x00484230 Boarding_BuildOptions.
// Rolls the plunder offers for state.player's primary target. Must be called
// with a valid, boardable primary target; values land in the returned
// struct (and are NOT mirrored anywhere else — the original's globals were
// consumed only by the boarding window and its draw callback).
[[nodiscard]] BoardingPlunderOptions Boarding_BuildOptions(GameState &state);

// Outcome of one boarding-window session. The original latches these as
// local flags inside NovaUi_RunBoardingPlunderWindow and as window-result
// codes returned to Player_HandleBoardTargetCommand.
struct BoardingWindowResult {
  // The boarded target's shields/armor were dropped to zero (the 10% crew
  // panic roll or the "Oops! ... self-destruct" 1/10 capture roll); the
  // death timer is armed and the flight sim resolves the kill.
  bool target_self_destructed = false;
  // A capture roll succeeded and the target was converted to an escort
  // (ai_behavior_code 6). The keep-as-new-ship / capture-decision dialog is
  // TODO(decomp) — the port currently always takes the escort path.
  bool target_captured_as_escort = false;
  // Capture was attempted and failed, or was refused because of the escort
  // cap / an unlicensed ship class.
  bool capture_attempt_failed = false;
};

// Ghidra 0x00482940 NovaUi_RunBoardingPlunderWindow (clean-room).
// Runs the modal boarding/plunder window (DLOG 0x3f3, PICT 0x2143) for
// state.player's primary target. Builds the plunder offers itself, as the
// original does, then runs the event loop until the player aborts, the target
// is lost, or a capture resolves. Each loot action (cargo/credits/ammo/fuel)
// applies its transfer to GameState and re-arms the panic self-destruct
// re-roll; the capture arm converts the boarded hull to a behavior-6 escort
// (the capture-decision dialog / ship swap, NovaUi_ShowCaptureDecisionDialog
// 0x00497eb0, is TODO(decomp) — see the BoardingWindowResult note).
//
// Note: the flight loop owns the SDL audio device, so the modal plays its
// one-shot cues directly through `audio` (the original queues them into
// NovaAudio_QueueCenteredSound; see the per-cue comments).
//
// The modal renders the live game view beneath itself each frame via
// `view.DrawGameFrame` (world + HUD across the whole window) and composites
// the window over it — the original draws its DLOG over the unmodified
// gameplay surface. The flight simulation itself is paused while the window
// is open, as in the original.
[[nodiscard]] BoardingWindowResult
NovaUi_RunBoardingPlunderWindow(SdlPlatform &platform,
                                SdlAudio &audio,
                                GameState &state,
                                SpaceflightView &view,
                                HudRenderer &hud);

// Lazily decodes snd 150 + i into GameState.transition_sounds (mirrors
// NovaAudio_PreloadGameplayData 0x004b0740). Call before queueing a
// transition-table cue so the first play doesn't hitch.
void EnsureTransitionSounds(GameState &state);

// Ghidra 0x0045a3d0 Player_HandleBoardTargetCommand. The player's one-shot
// "board target" command (input.board edge in the port): validates range /
// relative velocity / heading alignment / boardability of the primary target,
// then dispatches the plunder window (plain ships) or the mission arms
// (TODO(decomp)). Denial feedback is STR# 0x7d2 overlays plus a centered
// error beep queued on GameState.pending_ui_sounds. The plain-ship dispatch
// runs the modal synchronously (blocking the flight loop, as the original
// blocks in its own loop).
void Player_HandleBoardTargetCommand(SdlPlatform &platform,
                                     SdlAudio &audio,
                                     GameState &state,
                                     SpaceflightView &view,
                                     HudRenderer &hud);

// Ghidra 0x00415cb0 Boarding_ResetShipAndAttackersAfterBoarding. Clears the
// targeting state of every active ship whose primary target is `ship`, then
// resets most of `ship`'s own combat/mission state after a capture.
void Boarding_ResetShipAndAttackersAfterBoarding(GameState &state, Ship &ship);

// Ghidra 0x00412550 Boarding_BoardShipAndTransferCargo. AI boarding resolution,
// called by the capture-variant AI supervisor when its board approach
// completes (and reachable against the player). Moves as much cargo as fits
// from `boarded` to `boarder`, takes a share of the player's credits when the
// player is the victim, shows the loot HUD overlay, and (for non-player,
// non-mission victims) rolls the capture-odds conversion: the victim becomes
// a behavior-6 follower of the boarder with its faction converted. Mission
// failures armed with flags_primary 0x8000 fire when the player is boarded.
// `now_ms` is the port's sim clock for the mission teardown helpers.
void Boarding_BoardShipAndTransferCargo(GameState &state,
                                        Ship &boarder,
                                        Ship &boarded,
                                        std::uint32_t now_ms);

// Ghidra 0x00468920 Ship_CanPlayerHaveMoreEscorts. True while the count of
// active behavior-6 escorts (targeting the player, no mission fleet) is
// below the soft cap of 6. Shared by the capture arm and the hire path.
[[nodiscard]] bool NovaShip_CanPlayerHaveMoreEscorts(const GameState &state);

} // namespace game
