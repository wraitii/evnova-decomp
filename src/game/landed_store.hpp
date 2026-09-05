#pragma once

#include "game_state.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace game {

enum class LandedStoreKind : std::uint8_t { kOutfitter, kShipyard };

// State shared by the two 4x5 landed-store lists.  IDs are scenario resource
// IDs (0x80-based), while cursor/page positions are UI positions.
struct LandedStoreSession {
  static constexpr std::size_t kPageSlots = 20;
  LandedStoreKind kind = LandedStoreKind::kOutfitter;
  // Shipyard variant: escort-hire mode (Ghidra g_shipyard_purchase_mode 1).
  // Builds the listing from the ShipClassDef +0xa2a HireRandom lane and
  // prices the main action at 10% of the scaled purchase price.
  bool hire_mode = false;
  std::vector<std::int16_t> available_ids;
  std::array<std::int16_t, 0x200> opening_outfit_counts{};
  std::int16_t selected_id = -1;
  std::int16_t cursor_slot = -1;
  std::size_t page_base = 0;

  [[nodiscard]] std::int16_t IdAtCursor() const;
  [[nodiscard]] bool CanPagePrevious() const;
  [[nodiscard]] bool CanPageNext() const;
  void PagePrevious();
  void PageNext();
  void SelectSlot(std::size_t slot);
};

[[nodiscard]] ControlExpressionState
NovaLanded_ControlExpressionState(const GameState &state);
void NovaLanded_ExecuteControlSet(GameState &state,
                                  std::string_view expression);

// Ghidra 0x0048ea70 (open path): runs Weapon_ReconcileOutfitPoolWithWeaponBanks
// at modal entry (registering stock-bank weapons not yet owned as owned
// outfits), then builds the stellar-filtered listing. The caller passes a
// mutable state because the reconcile can modify outfit ownership.
[[nodiscard]] LandedStoreSession
NovaLanded_OpenOutfitterSession(GameState &state, std::int16_t stellar_id);
[[nodiscard]] LandedStoreSession NovaLanded_OpenShipyardSession(
    const GameState &state, std::int16_t stellar_id, bool hire_mode = false);
// Ghidra 0x0048ea70 (in-place refresh): rebuilds the outfitter/shipyard listing
// in place after a buy/sell mutation, preserving the session's opening-count
// snapshot (which gates same-session full-price refunds) and the selection when
// it is still offered.
void NovaLanded_RefreshStoreSession(GameState &state,
                                    LandedStoreSession &session,
                                    std::int16_t stellar_id);

[[nodiscard]] std::int32_t
NovaLanded_ScaledStorePrice(std::int32_t base_price,
                            std::int16_t item_tech,
                            std::int16_t stellar_tech,
                            float scale = 1.0F);
[[nodiscard]] std::int32_t NovaLanded_OutfitPrice(const GameState &state,
                                                  std::int16_t stellar_id,
                                                  std::int16_t outfit_id);
// Ghidra 0x00491950 Ship_ComputeShipCurrentMass: remaining outfit mass after
// the class FreeMass allowance and all installed purchase masses are applied.
[[nodiscard]] std::int32_t NovaLanded_FreeMass(const GameState &state);
[[nodiscard]] bool NovaLanded_CanBuyOutfit(const GameState &state,
                                           std::int16_t stellar_id,
                                           std::int16_t outfit_id);
[[nodiscard]] std::int16_t NovaLanded_BuyOutfit(GameState &state,
                                                std::int16_t stellar_id,
                                                std::int16_t outfit_id,
                                                std::int16_t requested);
[[nodiscard]] std::int16_t NovaLanded_SellOutfit(GameState &state,
                                                 LandedStoreSession &session,
                                                 std::int16_t stellar_id,
                                                 std::int16_t outfit_id,
                                                 std::int16_t requested);
// Ghidra 0x0048ea70 (close path): applies the landed Outfitter's final
// inventory cleanup before returning to the Spaceport. Temporary outfits (flag
// 0x10) do not survive the modal, and damage/fuel are capped when installed
// maxima were reduced.
void NovaLanded_CloseOutfitterSession(GameState &state);

[[nodiscard]] std::int32_t NovaLanded_ShipTradeInValue(const GameState &state,
                                                       std::int16_t stellar_id);
[[nodiscard]] std::int32_t NovaLanded_ShipPurchasePrice(const GameState &state,
                                                        std::int16_t stellar_id,
                                                        std::int16_t ship_id);
// Ghidra 0x00498dc0's escort-hire arm: the scaled purchase price times the
// hire multiplier (DAT_00575950 = 0.1), rounded to nearest.
[[nodiscard]] std::int32_t NovaLanded_ShipHirePrice(const GameState &state,
                                                    std::int16_t stellar_id,
                                                    std::int16_t ship_id);
// Ghidra 0x00498dc0 NovaUi_UpdateSelectedShipPurchaseAllowed (hire arm):
// the selected class must still be offered and the player able to afford the
// hire price.
[[nodiscard]] bool NovaLanded_CanHireShip(const GameState &state,
                                          std::int16_t stellar_id,
                                          std::int16_t ship_id);
// Ghidra 0x00492f30 NovaUi_RunShipyardPurchaseLoop (hire arm of the confirm
// action): deducts the hire price, spawns the escort via
// ShipClass_SpawnEscortShipFromClass (0x00422400) at the landed stellar and
// rerolls the class's daily hire roll (the original stores it into
// ShipClassDef +0xa2a; the clean-room keeps GameState.ship_class_threshold_
// rolls). Returns the spawned ship slot, or -1 when the purchase is denied.
[[nodiscard]] int NovaLanded_HireShip(GameState &state,
                                      std::int16_t stellar_id,
                                      std::int16_t ship_id);
[[nodiscard]] bool NovaLanded_CanBuyShip(const GameState &state,
                                         std::int16_t stellar_id,
                                         std::int16_t ship_id);
[[nodiscard]] bool
NovaLanded_ReplacePlayerShip(GameState &state,
                             std::int16_t stellar_id,
                             std::int16_t ship_id,
                             std::string_view player_ship_name);

} // namespace game
