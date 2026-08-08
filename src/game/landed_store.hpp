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

[[nodiscard]] LandedStoreSession
NovaLanded_OpenOutfitterSession(const GameState &state,
                                std::int16_t stellar_id);
[[nodiscard]] LandedStoreSession
NovaLanded_OpenShipyardSession(const GameState &state, std::int16_t stellar_id);

[[nodiscard]] std::int32_t
NovaLanded_ScaledStorePrice(std::int32_t base_price,
                            std::int16_t item_tech,
                            std::int16_t stellar_tech,
                            float scale = 1.0F);
[[nodiscard]] std::int32_t NovaLanded_OutfitPrice(const GameState &state,
                                                  std::int16_t stellar_id,
                                                  std::int16_t outfit_id);
// 0x00491950 / Ship_ComputeShipCurrentMass: remaining outfit mass after the
// class FreeMass allowance and all installed purchase masses are applied.
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
                                                 std::int16_t outfit_id,
                                                 std::int16_t requested);
// 0x0048ea70: applies the landed Outfitter's final inventory cleanup before
// returning to the Spaceport. Temporary outfits (flag 0x10) do not survive
// the modal, and damage/fuel are capped when installed maxima were reduced.
void NovaLanded_CloseOutfitterSession(GameState &state);

[[nodiscard]] std::int32_t NovaLanded_ShipTradeInValue(const GameState &state,
                                                       std::int16_t stellar_id);
[[nodiscard]] std::int32_t NovaLanded_ShipPurchasePrice(const GameState &state,
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
