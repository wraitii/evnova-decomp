#pragma once

// Stellar radar (minimap) simulation-side helpers: the IFF blip colours, the
// outfit-gated radar capabilities, the interference scanner strength and the
// per-tick proximity-scan roll. The panel drawing itself lives in
// HudRenderer::DrawRadarPanel (hud_renderer.cpp), mirroring the original split
// between the TickSystems scope-0xc roll and the NovaUi panel renderer.
//
// Ghidra model:
//   * Ship_GetShipRadarColor        0x00465f00  IFF blip colour per ship
//   * Stellar_GetStellarRadarColor  0x00466030  IFF blip colour per stellar
//   * Outfit_HasCloakRadarVisibilityFlag 0x004651a0  cloak ModType-17 bit
//   0x0002
//   * Outfit_HasPlayerOwnedOutfitType0x0E_Cached 0x004654b0  IFF outfit gate
//   * Outfit_HasPlayerOwnedOutfitType0x0D_Cached 0x00465410  density scanner
//   gate
//   * Ship_GetScannerStrength       0x0046abb0  ModType-24 (interference mod)
//                                               sum over owned outfits
//   * Frame_RollProximityScanDetection 0x0045d030  per-tick scope 0xc roll

#include "game_state.hpp"

#include <SDL3/SDL.h>

namespace game {

// The primary-target blink colour (Ghidra DAT_00733b50, an 0xc000 grey triple
// seeded by Settings_InitTimingPresets 0x004ad7c0).
inline constexpr SDL_Color kRadarTargetBlinkColor{
    192, 192, 192, SDL_ALPHA_OPAQUE};

struct Stellar;

// Ghidra 0x00465f00 Ship_GetShipRadarColor. IFF radar colour of one ship:
// the player is cyan, disabled ships dark grey, distress-eligible
// (actively attacking the player's side) ships red, ships moving on/with the
// player green and unaligned/blue everything else.
[[nodiscard]] SDL_Color Ship_RadarDisplayColor(const GameState &state,
                                               const Ship &ship);

// Ghidra 0x00466030 Stellar_GetStellarRadarColor. IFF radar colour of one
// stellar body: grey when inactive or uninhabited, green for hazard markers,
// yellow/orange/red by reputation threshold for landable bodies, dark cyan
// for wormholes.
[[nodiscard]] SDL_Color Stellar_RadarDisplayColor(const GameState &state,
                                                  const Stellar &st);

// Ghidra 0x004651a0 Outfit_HasCloakRadarVisibilityFlag. True when the ship's
// ModType-17 cloaking device carries ModVal bit 0x0002, keeping the cloaked
// ship visible on radar. Player ships scan the owned inventory, NPCs their
// ship class default loadout (same split as NovaOutfit_HasCloakingDevice).
[[nodiscard]] bool Outfit_HasCloakRadarVisibility(const GameState &state,
                                                  const Ship &ship);

// Cloak-scanner reveal bits (ModType 30 ModVal): 0x0001 reveals cloaked ships
// on radar, 0x0002 on screen. The original resolves these through the
// Outfit_HasCloakScannerReveal* predicate family (0x004652a0 surface, and the
// sibling radar predicate) and caches the results into ShipState +0xC91C/
// +0xC91E at spawn; that cache population is not reconstructed yet, so this
// scans the owned inventory on demand.
// TODO(decomp(0x004652a0)) port the cached ShipState cloak-scanner fields.
[[nodiscard]] bool Player_HasCloakScannerReveal(const GameState &state,
                                                std::uint16_t reveal_flag);

// Radar-reveal bit (0x0001) of the above.
[[nodiscard]] bool Player_HasCloakScannerRadarReveal(const GameState &state);

// Ghidra 0x004654b0 Outfit_HasPlayerOwnedOutfitType0x0E_Cached: whether the
// player owns an outfit with ModType 14 (IFF colorized radar). The original
// memoizes into a -1 sentinel global; this scans on demand.
[[nodiscard]] bool Outfit_PlayerHasIffOutfit(const GameState &state);

// Ghidra 0x00465410 Outfit_HasPlayerOwnedOutfitType0x0D_Cached: ModType 13
// (density scanner) ownership gate for the radar blip size tiers.
[[nodiscard]] bool Outfit_PlayerHasDensityScanner(const GameState &state);

// Ghidra 0x0046abb0 Ship_GetScannerStrength. Summed scanner strength: for
// every owned outfit, owned_count * ModVal of each ModType 0x18 (24,
// interference mod) slot, clamped to [-100, 100]. The original memoizes into
// DAT_007356ba; this pure scan is cheap enough per tick.
[[nodiscard]] int Ship_ComputeScannerStrength(const GameState &state);

// Ghidra 0x0045d030 Frame_RollProximityScanDetection. Per-tick (TickSystems
// scope 0xc): detection odds = clamp(system.interference - scanner_strength,
// 0, 100); latches GameState.proximity_scan_detected = roll < odds.
void Frame_RollProximityScanDetection(GameState &state);

} // namespace game
