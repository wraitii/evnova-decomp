#pragma once

#include "game_state.hpp"

#include <array>
#include <cstdint>
#include <functional>
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
[[nodiscard]] bool NovaLanded_CanBuyOutfit(GameState &state,
                                           std::int16_t stellar_id,
                                           std::int16_t outfit_id);
// Ghidra 0x00491950 step 5: RequireGovt scopes an outfit's Require bits to
// one of four government-keyed outfit-id bands (see Outfit::require_govt).
// local_govt is the landed stellar's zero-based government id (-1 =
// independent). Any value outside the bands (including -1) allows all shops.
[[nodiscard]] bool NovaLanded_RequireGovtAllows(const ScenarioData &scenario,
                                                std::int16_t require_govt,
                                                std::int16_t local_govt);
[[nodiscard]] std::int16_t NovaLanded_BuyOutfit(GameState &state,
                                                std::int16_t stellar_id,
                                                std::int16_t outfit_id,
                                                std::int16_t requested);
// Ghidra 0x0048ea70: a stellar lists purchasable outfits when it has a
// nonzero TechLevel or any positive SpecialTech entry. This gates the Buy
// button globally (DAT_007d4c0d) independently of per-item eligibility.
[[nodiscard]] bool NovaLanded_StellarSellsOutfits(const GameState &state,
                                                  std::int16_t stellar_id);

// Why a landed-Outfitter sale stopped (Ghidra 0x0048ea70 unit loop). The
// original aborts the remaining units and shows an STR# 0x7d2 message when a
// removal would strand dependent ammo or leave negative free mass.
enum class OutfitSaleBlock : std::uint8_t {
  kNone = 0,
  kNegativeMass,    // entry 0xcf
  kDependentOutfit, // 0xd0 + count + dependent name + 0xd4 + item name
  kWeaponAmmo,      // 0xd0 + count + ammo outfit or unit(s) of ammunition + ...
};

struct OutfitSaleResult {
  // Units actually removed by this call.
  std::int16_t sold = 0;
  OutfitSaleBlock block = OutfitSaleBlock::kNone;
  // Units that must be sold first (kDependentOutfit / kWeaponAmmo).
  std::int16_t excess = 0;
  // Resource id the message names: the dependent outfit or the ammo outfit
  // feeding the weapon. -1 selects the generic "unit(s) of ammunition" form.
  std::int16_t blocker_id = -1;
  // Plural form of the blocker name (original: excess >= 2).
  bool blocker_plural = false;
  // Plural form of the selected item name (original: the aborted unit is not
  // the final requested one).
  bool item_plural = false;
};

[[nodiscard]] OutfitSaleResult
NovaLanded_SellOutfit(GameState &state,
                      LandedStoreSession &session,
                      std::int16_t stellar_id,
                      std::int16_t outfit_id,
                      std::int16_t requested);
// Ghidra 0x0048ea70 (close path): applies the landed Outfitter's final
// inventory cleanup before returning to the Spaceport. Temporary outfits (flag
// 0x10) do not survive the modal, and damage/fuel are capped when installed
// maxima were reduced.
void NovaLanded_CloseOutfitterSession(GameState &state);

// Ghidra 0x00498dc0 / 0x004948b0 / 0x00492f30: the current ship's trade-in
// credit (Ship_ComputeTradeInValue) scaled through the two shipyard price
// stages. Used by the shipyard price display, the buy gate, and the purchase.
[[nodiscard]] std::int32_t NovaLanded_ShipTradeInValue(const GameState &state,
                                                       std::int16_t stellar_id);
// Ghidra 0x00498dc0 / 0x004948b0: the selected ship's scaled list price before
// the trade-in credit (shipyard "Ship Price" row).
[[nodiscard]] std::int32_t NovaLanded_ShipPrice(const GameState &state,
                                                std::int16_t stellar_id,
                                                std::int16_t ship_id);
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
[[nodiscard]] bool Player_SwapShipWithEscort(GameState &state,
                                             std::int16_t stellar_id,
                                             std::int16_t ship_id,
                                             std::string_view player_ship_name);

// Ghidra 0x004229d0 Player_ProcessEscortFleetAtStellar. Auto fleet-trade pass
// run at the end of the Spaceport interaction loop: at a shipyard-capable
// stellar (`travel_flags & 8`) it sells escorts marked for release, upgrades
// escorts marked for upgrade (when UpgradeTo is set and affordable), refills
// base shield/armor and weapon secondary for every ship slot, shows the
// localized summary, then advances `(sold + upgraded) / 2` days. Always ends
// by calling Player_ProcessEscortPayroll(1). `show_text` presents the summary
// messages (the original's Ui_RunTravelSelectionDialog); pass an empty
// callback to suppress the modal (tests).
void Player_ProcessEscortFleetAtStellar(
    GameState &state,
    std::int16_t stellar_id,
    const std::function<void(const std::string &)> &show_text);

// Ghidra 0x004232d0 Player_ProcessEscortPayroll. Runs `periods` payroll passes
// over the player's behavior-6 escorts attached to the player: each period
// deducts trunc(class.base_cost * 0.01) from the player's credits; an escort
// that cannot be paid defects (deactivated), and the localized STR# 0x7d2
// 0x12e/0x12f message is shown once if any did. `show_text` as above.
void Player_ProcessEscortPayroll(
    GameState &state,
    std::int16_t periods,
    const std::function<void(const std::string &)> &show_text);

} // namespace game
