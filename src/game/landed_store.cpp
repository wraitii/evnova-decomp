#include "landed_store.hpp"

#include "hud_overlay.hpp"
#include "mission.hpp"
#include "outfit.hpp"
#include "ship_ai.hpp"
#include "ship_spawn.hpp"
#include "weapon.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>

namespace game {
namespace {

[[nodiscard]] bool HasTech(const Stellar &stellar, std::int16_t tech) {
  // 0x00469e90 / 0x0046a220 ignore the sentinel values outside the normal
  // 0..32766 tech range. A valid item is stocked at or below the port's base
  // tech, or when its exact level appears in one of SpecialTech1-8.
  if (tech < 0 || tech == std::numeric_limits<std::int16_t>::max()) {
    return false;
  }
  if (stellar.tech_level >= tech) {
    return true;
  }
  return std::find(stellar.special_tech.begin(),
                   stellar.special_tech.end(),
                   tech) != stellar.special_tech.end();
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
ContributeMask(const GameState &state) {
  // Ghidra Outfit_EvaluateRequireMask (0x0046cd80) ->
  // Mission_AccumulatePlayerContributeMask (0x0046cca0) seeds the aggregate
  // from the *player's ship class* contribute (ship
  // ShipClassDef.field_0xa30/0xa34), then ORs contributions from owned outfits
  // (plus active missions / ranks in the original). Without the ship baseline
  // a fresh player with only the starter stock would fail every outfit Require
  // mask, so buying fails.
  std::uint32_t lo = 0;
  std::uint32_t hi = 0;
  const std::int16_t ship_class_id = state.player.ship_class_id;
  const ShipClass *cls =
      ship_class_id >= 0
          ? state.scenario.Ship(static_cast<std::int16_t>(ship_class_id + 0x80))
          : nullptr;
  if (cls != nullptr) {
    lo |= cls->contribute_lo;
    hi |= cls->contribute_hi;
  }
  for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size() &&
                          i < state.scenario.outfits.size();
       ++i) {
    if (state.inventory.outfit_owned_count[i] > 0) {
      lo |= state.scenario.outfits[i].contribute_lo;
      hi |= state.scenario.outfits[i].contribute_hi;
    }
  }
  return {lo, hi};
}

[[nodiscard]] bool
MeetsRequire(const GameState &state, std::uint32_t lo, std::uint32_t hi) {
  const auto [have_lo, have_hi] = ContributeMask(state);
  return (have_lo & lo) == lo && (have_hi & hi) == hi;
}

[[nodiscard]] bool IsValidOutfit(const GameState &state, std::int16_t id) {
  return id >= 0x80 &&
         static_cast<std::size_t>(id - 0x80) < state.scenario.outfits.size();
}

[[nodiscard]] bool IsValidShip(const GameState &state, std::int16_t id) {
  return id >= 0x80 &&
         static_cast<std::size_t>(id - 0x80) < state.scenario.ships.size();
}

// The selected outfit's four (ModType, ModVal) slots: primary plus the three
// alternates. Outfit mod values for weapon/ammo/bomb types are zero-based
// bank slots (see Outfit::mod_val).
using OutfitModSlot = std::pair<std::int16_t, std::int16_t>;

[[nodiscard]] std::array<OutfitModSlot, 4>
OutfitModSlots(const Outfit &outfit) {
  return {{{outfit.mod_type, outfit.mod_val},
           {outfit.alt_mod_types[0], outfit.alt_mod_vals[0]},
           {outfit.alt_mod_types[1], outfit.alt_mod_vals[1]},
           {outfit.alt_mod_types[2], outfit.alt_mod_vals[2]}}};
}

} // namespace

// Ghidra 0x0049458d NovaUi_ShipyardSetCursorSlot. The session tracks the
// 20-slot (4x5 grid) cursor selection and preserves/clears it across paging.
std::int16_t LandedStoreSession::IdAtCursor() const {
  const std::size_t absolute =
      page_base +
      static_cast<std::size_t>(std::max<std::int16_t>(cursor_slot, 0));
  return cursor_slot >= 0 && absolute < available_ids.size()
             ? available_ids[absolute]
             : -1;
}

bool LandedStoreSession::CanPagePrevious() const { return page_base >= 4; }

bool LandedStoreSession::CanPageNext() const {
  return page_base + kPageSlots < available_ids.size();
}

void LandedStoreSession::PagePrevious() {
  if (!CanPagePrevious())
    return;
  page_base -= 4;
  // The original keeps the same selected resource visible by moving its
  // grid cursor down one row. A selection already on the final row no longer
  // fits after a previous-page move, so it is cleared instead.
  if (cursor_slot < 0 || cursor_slot > 15) {
    cursor_slot = -1;
    selected_id = -1;
    return;
  }
  cursor_slot = static_cast<std::int16_t>(cursor_slot + 4);
  selected_id =
      available_ids[page_base + static_cast<std::size_t>(cursor_slot)];
}

void LandedStoreSession::PageNext() {
  if (!CanPageNext())
    return;
  page_base += 4;
  // Symmetric to PagePrevious: the first row scrolls offscreen, while any
  // later selected row remains selected one row higher.
  if (cursor_slot < 4) {
    cursor_slot = -1;
    selected_id = -1;
    return;
  }
  cursor_slot = static_cast<std::int16_t>(cursor_slot - 4);
  selected_id =
      available_ids[page_base + static_cast<std::size_t>(cursor_slot)];
}

void LandedStoreSession::SelectSlot(std::size_t slot) {
  if (slot >= kPageSlots || page_base + slot >= available_ids.size()) {
    cursor_slot = -1;
    selected_id = -1;
    return;
  }
  cursor_slot = static_cast<std::int16_t>(slot);
  selected_id = available_ids[page_base + slot];
}

ControlExpressionState
NovaLanded_ControlExpressionState(const GameState &state) {
  return ControlExpressionState{
      .get_control_bit =
          [&state](std::uint32_t bit) { return state.control.ControlBit(bit); },
      .is_registered =
          [&state](std::uint32_t) { return state.control.registered; },
      .is_male = [&state] { return state.control.male; },
      .owns_outfit =
          [&state](std::int16_t id) {
            return Outfit_PlayerHasOutfitForControlExpression(state, id);
          },
      .has_explored =
          [&state](std::int16_t id) {
            return id >= 0 &&
                   static_cast<std::size_t>(id) <
                       state.control.explored_systems.size() &&
                   state.control.explored_systems.test(
                       static_cast<std::size_t>(id));
          }};
}

void NovaLanded_ExecuteControlSet(GameState &state,
                                  std::string_view expression) {
  NovaControlExpression_ExecuteSet(
      expression, {.set_control_bit = [&state](std::uint32_t bit, bool value) {
        state.control.SetControlBit(bit, value);
      }});
}

std::vector<std::int16_t>
BuildOutfitterIds(GameState &state,
                  const Stellar &stellar,
                  const ControlExpressionState &expr) {
  std::vector<std::int16_t> ids;
  for (std::size_t i = 0; i < state.scenario.outfits.size() && i < 0x200; ++i) {
    const Outfit &outfit = state.scenario.outfits[i];
    // ScenarioData keeps resource-addressable holes in the 0x200-entry table.
    // The original marks absent runtime definitions invalid; an empty record
    // name is our corresponding validity marker.
    if (outfit.name.empty())
      continue;
    const bool owned = state.inventory.outfit_owned_count[i] > 0;
    bool visible = HasTech(stellar, outfit.tech_level);
    if (owned && (stellar.availability_flags & 0x400U) != 0U &&
        (outfit.flags & 0x0008U) == 0U)
      visible = true;
    if (!owned && (outfit.flags & 0x4000U) != 0U)
      visible = visible &&
                NovaControlExpression_Evaluate(outfit.availability_expr, expr);
    if (!owned && (outfit.flags & 0x0100U) != 0U)
      visible =
          visible && MeetsRequire(state, outfit.require_lo, outfit.require_hi);
    if (owned && (outfit.flags & 0x0800U) != 0U)
      visible = true;
    // Daily in-stock gate (Outfit_RebuildAvailableOutfitListForTravelStellar
    // 0x0046a220): an unowned outfit shows only while the per-day stock roll
    // is non-zero and <= the def's In-Stock percent; owning a visible one
    // zeroes its roll, holding it in stock for the rest of the day (the next
    // daily reroll in 0x00466cb0 refreshes it).
    if (visible) {
      if (owned) {
        state.outfit_stock_rolls[i] = 0;
      } else {
        const std::int16_t roll = state.outfit_stock_rolls[i];
        if (outfit.stock_threshold < 1 || outfit.stock_threshold < roll) {
          visible = false;
        }
      }
    }
    if (visible)
      ids.push_back(static_cast<std::int16_t>(i + 0x80));
  }
  // 0x0046a220: an eligible item with flag 0x1000 suppresses later resource
  // IDs carrying the same display weight. This is intentionally applied
  // before the weight ordering, matching the original's resource-order pass.
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const Outfit *outfit = state.scenario.Outfit(ids[i]);
    if ((outfit->flags & 0x1000U) == 0U)
      continue;
    const auto weight = outfit->display_weight;
    ids.erase(std::remove_if(
                  ids.begin() + static_cast<std::ptrdiff_t>(i + 1),
                  ids.end(),
                  [&state, weight](std::int16_t candidate) {
                    return state.scenario.Outfit(candidate)->display_weight ==
                           weight;
                  }),
              ids.end());
  }
  std::stable_sort(ids.begin(), ids.end(), [&state](auto a, auto b) {
    return state.scenario.Outfit(a)->display_weight >
           state.scenario.Outfit(b)->display_weight;
  });
  return ids;
}

// Ghidra 0x00469e90 NovaUi_RebuildShipyardAvailabilityList: shared filter
// chain (tech level, per-class daily roll pair, Require bits, availability
// expression, display-weight ordering and the 0x4000 equal-weight
// suppression). `hire_mode` selects the ShipClassDef +0xa2a threshold pair
// (HireRandom) instead of the +0xa2c limit pair (BuyRandom).
// TODO(decomp) skipped: the original multiplies the daily hire roll by the
// shareware-license flag (ShipClassDef[g_expression_ship_class_id]
// +0xab8, a global registration byte the decompiler patterns as a class
// field): an unregistered binary treats every class with HireRandom != 0 as
// always offered. This port models the registered behavior only.
std::vector<std::int16_t> BuildShipyardIds(const GameState &state,
                                           const Stellar &stellar,
                                           const ControlExpressionState &expr,
                                           bool hire_mode) {
  std::vector<std::int16_t> ids;
  for (std::size_t i = 0; i < state.scenario.ships.size() && i < 0x200; ++i) {
    const ShipClass &ship = state.scenario.ships[i];
    // 0x00469e90 first applies the per-class daily BuyRandom availability
    // latch. A zero BuyRandom is unconditional: the class is never offered
    // for purchase. Nova's many mission/paint/loadout variants deliberately
    // use zero here, so admitting them based on tech alone floods the list
    // with duplicate hulls. Positive percents then gate on the per-day limit
    // roll (ShipClassDef +0xa2c, rerolled by the 0x00466cb0 tail): the class
    // is offered while the roll is <= BuyRandom. The hire lane mirrors this
    // with HireRandom (+0xa2a) and the threshold roll.
    const std::int16_t roll = hire_mode
                                  ? (i < state.ship_class_threshold_rolls.size()
                                         ? state.ship_class_threshold_rolls[i]
                                         : 0)
                                  : (i < state.ship_class_limit_rolls.size()
                                         ? state.ship_class_limit_rolls[i]
                                         : 0);
    const std::int16_t random_percent =
        hire_mode ? ship.hire_random : ship.buy_random;
    if (ship.display_name.empty() || random_percent <= 0 ||
        roll > random_percent || !HasTech(stellar, ship.tech_level))
      continue;
    if ((ship.availability_flags & 0x0200U) != 0U &&
        !MeetsRequire(state, ship.require_lo, ship.require_hi))
      continue;
    if ((ship.availability_flags & 0x0100U) != 0U &&
        !NovaControlExpression_Evaluate(ship.availability_expr, expr))
      continue;
    ids.push_back(static_cast<std::int16_t>(i + 0x80));
  }
  // 0x00469e90 uses the corresponding ship-class flag 0x4000.
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const ShipClass *ship = state.scenario.Ship(ids[i]);
    if ((ship->availability_flags & 0x4000U) == 0U)
      continue;
    const auto weight = ship->display_weight;
    ids.erase(std::remove_if(
                  ids.begin() + static_cast<std::ptrdiff_t>(i + 1),
                  ids.end(),
                  [&state, weight](std::int16_t candidate) {
                    return state.scenario.Ship(candidate)->display_weight ==
                           weight;
                  }),
              ids.end());
  }
  std::stable_sort(ids.begin(), ids.end(), [&state](auto a, auto b) {
    return state.scenario.Ship(a)->display_weight >
           state.scenario.Ship(b)->display_weight;
  });
  return ids;
}

void NovaLanded_RefreshStoreSession(GameState &state,
                                    LandedStoreSession &session,
                                    std::int16_t stellar_id) {
  // 0x0048ea70 re-runs the stellar-filtered list build after every buy/sell
  // mutation, but it keeps the opening-count snapshot taken when the modal
  // was entered. That snapshot gates the same-session full-price refund on
  // purchase (vs. the half-price resale of opening stock). We rebuild only the
  // listing here, preserving kind, snapshot, and (where still present) the
  // selection; a freshly bought/sold item no longer offered drops out.
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr)
    return;
  const auto expr = NovaLanded_ControlExpressionState(state);
  const auto fresh =
      session.kind == LandedStoreKind::kOutfitter
          ? BuildOutfitterIds(state, *stellar, expr)
          : BuildShipyardIds(state, *stellar, expr, session.hire_mode);
  if (fresh == session.available_ids) {
    return;
  }
  session.available_ids = std::move(fresh);
  if (session.kind == LandedStoreKind::kOutfitter) {
    // 0x0048ea70: when the rebuilt outfitter listing changes, the modal
    // selects definition index 0 (resource id 0x80), clears the grid cursor
    // and pages back to the top. The original does this unconditionally, even
    // when the prior selection is still present.
    session.selected_id = session.available_ids.empty() ? -1 : 0x80;
  } else if (session.selected_id >= 0 &&
             std::find(session.available_ids.begin(),
                       session.available_ids.end(),
                       session.selected_id) == session.available_ids.end()) {
    session.selected_id = -1;
  }
  session.cursor_slot = -1;
  session.page_base = 0;
}

LandedStoreSession NovaLanded_OpenOutfitterSession(GameState &state,
                                                   std::int16_t stellar_id) {
  LandedStoreSession session;
  session.kind = LandedStoreKind::kOutfitter;
  // The outfitter clears both one-shot effect latches on entry
  // (NovaUi_RunOutfitterInteractionLoop 0x0048ea70): one map purchase and
  // one record-clean per visit.
  state.control.map_grant_latch = false;
  state.control.record_grant_latch = false;
  // Ghidra NovaUi_RunOutfitterInteractionLoop (0x0048ea70) runs
  // Weapon_ReconcileOutfitPoolWithWeaponBanks at modal entry, before
  // Outfit_RebuildAvailableOutfitListForTravelStellar builds the listing, so
  // any stock-bank weapon not yet registered as an owned outfit (e.g. a
  // lately-acquired ship's mounted gun) becomes a sellable owned item.
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
  session.opening_outfit_counts = state.inventory.outfit_owned_count;
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr)
    return session;
  const auto expr = NovaLanded_ControlExpressionState(state);
  session.available_ids = BuildOutfitterIds(state, *stellar, expr);
  return session;
}

LandedStoreSession NovaLanded_OpenShipyardSession(const GameState &state,
                                                  std::int16_t stellar_id,
                                                  bool hire_mode) {
  LandedStoreSession session;
  session.kind = LandedStoreKind::kShipyard;
  session.hire_mode = hire_mode;
  session.opening_outfit_counts = state.inventory.outfit_owned_count;
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr)
    return session;
  const auto expr = NovaLanded_ControlExpressionState(state);
  session.available_ids =
      BuildShipyardIds(state, *stellar, expr, session.hire_mode);
  return session;
}

bool NovaLanded_StellarSellsOutfits(const GameState &state,
                                    std::int16_t stellar_id) {
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr)
    return false;
  if (stellar->tech_level != 0)
    return true;
  return std::any_of(stellar->special_tech.begin(),
                     stellar->special_tech.end(),
                     [](std::int16_t tech) { return tech > 0; });
}

// Ghidra 0x0049d640 Outfit_ComputeScaledPurchasePrice (tech-discount
// truncation, threshold quanta). Both float-to-int conversions end with the
// x87 FIST + residual/sign correction (ADD 0x7fffffff / SBB) that truncates
// toward zero, not round-to-nearest (cf. ship_ai.cpp).
std::int32_t NovaLanded_ScaledStorePrice(std::int32_t base_price,
                                         std::int16_t item_tech,
                                         std::int16_t stellar_tech,
                                         float scale) {
  if (base_price <= 0)
    return 0;
  std::int32_t value = base_price;
  // The original only guards the upper bound (signed item/stellar tech < 6)
  // and item_tech < stellar_tech; there is no lower-bound test, so a negative
  // item tech still takes the discount/surcharge path.
  if (item_tech < 6 && stellar_tech < 6 && item_tech < stellar_tech &&
      base_price > 99) {
    value = static_cast<std::int32_t>(
        static_cast<float>(base_price) *
        static_cast<float>(100 - (stellar_tech - item_tech) * 3) * 0.01F);
  }
  value =
      std::max(1, static_cast<std::int32_t>(static_cast<float>(value) * scale));
  const int quantum = value <= 10000 ? 10 : value <= 100000 ? 100 : 1000;
  return value > 100 ? value / quantum * quantum : value;
}

std::int32_t NovaLanded_OutfitPrice(const GameState &state,
                                    std::int16_t stellar_id,
                                    std::int16_t outfit_id) {
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  const Outfit *outfit = state.scenario.Outfit(outfit_id);
  const ShipClass *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (stellar == nullptr || outfit == nullptr || ship == nullptr)
    return 0;
  return NovaLanded_ScaledStorePrice(outfit->PurchasePrice(ship->mass_tons),
                                     outfit->tech_level,
                                     stellar->tech_level);
}

// Ghidra 0x00491950 NovaUi_IsOutfitterPurchaseAllowed (partial port of the
// tech/require/availability gate).
bool NovaLanded_CanBuyOutfit(GameState &state,
                             std::int16_t stellar_id,
                             std::int16_t outfit_id) {
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  const Outfit *outfit = state.scenario.Outfit(outfit_id);
  if (stellar == nullptr || outfit == nullptr ||
      !HasTech(*stellar, outfit->tech_level))
    return false;
  const ShipClass *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (ship == nullptr)
    return false;
  // Original: continue when the purchase mass fits the free mass OR the item
  // is massless/negative-mass (Outfit_ComputeOutfitPurchaseMass < 1); only a
  // positive item that exceeds the free allowance is rejected.
  const std::int32_t purchase_mass = outfit->PurchaseMass(ship->mass_tons);
  if (purchase_mass > Outfit_ComputePlayerFreeMass(state) && purchase_mass >= 1)
    return false;
  const OutfitOwnership ownership = Outfit_ClampOwnedCountToLimits(
      state, static_cast<std::int16_t>(outfit_id - 0x80));
  if (ownership.effective_owned >= ownership.max_allowed)
    return false;

  // Mod-slot specializations in the original's precedence (0x00491950): a
  // primary ModType-3 (ammo/launcher) slot is tested first for a mode-99
  // fighter bay; otherwise the first applicable cargo/map/record arm runs.
  bool has_cargo = false;
  std::int16_t cargo_mod = 0;
  bool has_map = false;
  bool has_record = false;
  for (const auto &[type, value] : OutfitModSlots(*outfit)) {
    if (type == static_cast<std::int16_t>(OutfitEffect::kCargoSpace)) {
      has_cargo = true;
      cargo_mod = value;
    } else if (type == static_cast<std::int16_t>(OutfitEffect::kMap)) {
      has_map = true;
    } else if (type == static_cast<std::int16_t>(OutfitEffect::kCleanRecord)) {
      has_record = true;
    }
  }
  if (outfit->mod_type == static_cast<std::int16_t>(OutfitEffect::kAmmo)) {
    // Fighter-bay launcher: the player must own a bay that can hold one more
    // craft of the class encoded in the weapon's ammo/cost code.
    const std::int16_t weapon_bank = outfit->mod_val;
    if (weapon_bank >= 0) {
      const Weapon *weapon =
          state.scenario.Weapon(static_cast<std::int16_t>(weapon_bank + 0x80));
      if (weapon != nullptr && weapon->weapon_mode_code == 99 &&
          !NovaShipClass_HasPlayerBayCapacityFor(
              state,
              static_cast<std::int16_t>(weapon->ammo_type - 0x80),
              static_cast<std::int16_t>(outfit_id - 0x80),
              weapon_bank))
        return false;
    }
  } else if (has_cargo) {
    // A negative cargo-space mod (0x00491950) needs the ship class's +0xa40
    // capability and total mass over the current cargo/junk load.
    // TODO(decomp): ShipClassDef +0xa40 is not decoded in the port, so the
    // reduction is currently ungated.
    (void)cargo_mod;
  } else if (has_map) {
    if (state.control.map_grant_latch)
      return false;
  } else if (has_record) {
    if (state.control.record_grant_latch)
      return false;
  }

  if (!MeetsRequire(state, outfit->require_lo, outfit->require_hi) ||
      !NovaControlExpression_Evaluate(outfit->availability_expr,
                                      NovaLanded_ControlExpressionState(state)))
    return false;
  // QUIRK (0x00491950): the original's tail price check reads the
  // g_outfitter_selected_id global rather than its param_1 argument. Every
  // caller passes that same global, so testing `outfit_id` here is equivalent.
  return state.player.credits >=
         NovaLanded_OutfitPrice(state, stellar_id, outfit_id);
}

std::int16_t NovaLanded_BuyOutfit(GameState &state,
                                  std::int16_t stellar_id,
                                  std::int16_t outfit_id,
                                  std::int16_t requested) {
  if (requested <= 0)
    return 0;
  std::int16_t bought = 0;
  for (; bought < requested &&
         NovaLanded_CanBuyOutfit(state, stellar_id, outfit_id);
       ++bought) {
    const std::int32_t price =
        NovaLanded_OutfitPrice(state, stellar_id, outfit_id);
    // Ghidra 0x00427770 Outfit_GrantOutfitToPlayer runs on every shop take:
    // plain outfits stack in the inventory, while map-reveal / paint /
    // clean-record outfits are consumed one-shot effect items that never
    // enter the owned inventory.
    if (NovaOutfit_GrantOutfitToPlayer(
            state, static_cast<std::int16_t>(outfit_id - 0x80))) {
      // One-shot effect item: the purchase applies and the unit is consumed,
      // so multi-unit buys stop here.
      state.player.credits -= price;
      NovaLanded_ExecuteControlSet(
          state, state.scenario.Outfit(outfit_id)->on_purchase_expr);
      return 1;
    }
    state.player.credits -= price;
    NovaLanded_ExecuteControlSet(
        state, state.scenario.Outfit(outfit_id)->on_purchase_expr);
  }
  if (bought > 0)
    NovaWeapon_RebuildBanksFromOwnedOutfits(state);
  return bought;
}

OutfitSaleResult NovaLanded_SellOutfit(GameState &state,
                                       LandedStoreSession &session,
                                       std::int16_t stellar_id,
                                       std::int16_t outfit_id,
                                       std::int16_t requested) {
  OutfitSaleResult result;
  if (requested <= 0 || !IsValidOutfit(state, outfit_id))
    return result;
  Outfit const *outfit = state.scenario.Outfit(outfit_id);
  if ((outfit->flags & 0x0008U) != 0U)
    return result; // original no-sell marker
  const std::size_t index = static_cast<std::size_t>(outfit_id - 0x80);
  const std::int16_t allowed =
      std::min(requested, state.inventory.outfit_owned_count[index]);
  if (allowed <= 0)
    return result;
  const ShipClass *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (ship == nullptr)
    return result;
  // 0x0048ea70 prices each sale off the scaled purchase price at this port,
  // not the raw base cost: a unit held at or below the opening snapshot
  // resells at trunc(price * 0.5) (the double 0.5 at DAT_00575940, x87
  // FIST + residual/sign correction), while a unit bought earlier this session
  // is refunded at the full scaled price.
  const std::int32_t scaled_price =
      NovaLanded_OutfitPrice(state, stellar_id, outfit_id);
  const std::int32_t resale_value =
      static_cast<std::int32_t>(static_cast<float>(scaled_price) * 0.5F);
  const std::int32_t purchase_mass = outfit->PurchaseMass(ship->mass_tons);
  const auto mod_slots = OutfitModSlots(*outfit);
  const std::int16_t opening = session.opening_outfit_counts[index];

  for (std::int16_t unit = 0; unit < allowed; ++unit) {
    // Negative free mass: a negative-mass item cannot be removed when it would
    // push the remaining free mass below zero (0x0048ea70 -> entry 0xcf).
    if (purchase_mass < 0 &&
        Outfit_ComputePlayerFreeMass(state) + purchase_mass < 0) {
      result.block = OutfitSaleBlock::kNegativeMass;
      break;
    }
    const std::int16_t current_owned =
        state.inventory.outfit_owned_count[index];
    bool blocked = false;
    // ModType-27 dependents: the item feeds another outfit; the kept units
    // must be able to consume everything already owned (0xd0 + name + 0xd4).
    for (const auto &[type, value] : mod_slots) {
      if (type != 27 || value < 0x80 || value >= 0x280)
        continue;
      const std::size_t dep = static_cast<std::size_t>(value - 0x80);
      if (dep >= state.scenario.outfits.size())
        continue;
      const std::int16_t excess = static_cast<std::int16_t>(
          state.inventory.outfit_owned_count[dep] -
          (current_owned - allowed) * state.scenario.outfits[dep].max_count);
      if (excess > 0) {
        result.block = OutfitSaleBlock::kDependentOutfit;
        result.excess = excess;
        result.blocker_id = value;
        result.blocker_plural = excess >= 2;
        result.item_plural = unit < allowed - 1;
        blocked = true;
        break;
      }
    }
    if (blocked)
      break;
    // Weapon ammo protection: a mounted weapon keeps its loaded rounds, so the
    // remaining weapon count must be able to carry them (0xd0 + ammunition +
    // 0xd4).
    std::int16_t weapon_index = -1;
    for (const auto &[type, value] : mod_slots) {
      if (type == 1) {
        weapon_index = value;
        break;
      }
    }
    if (weapon_index >= 0) {
      const Weapon *weapon =
          state.scenario.Weapon(static_cast<std::int16_t>(weapon_index + 0x80));
      if (weapon != nullptr && weapon->max_ammo > 0) {
        const std::int16_t ammo_type = weapon->ammo_type;
        const std::size_t bank = (ammo_type < 0 || ammo_type > 0xff ||
                                  weapon->weapon_mode_code == 99)
                                     ? static_cast<std::size_t>(weapon_index)
                                     : static_cast<std::size_t>(ammo_type);
        std::int32_t loaded = state.weapon_bank_secondary[bank * 100];
        if (weapon->weapon_mode_code == 99) {
          // Carrier-bay weapon: count active, non-disabled behavior-5
          // fighters whose class matches the id encoded in ammo_type.
          for (std::size_t i = 1; i < GameState::kMaxShips; ++i) {
            const Ship &fighter = state.ShipAt(i);
            if (fighter.is_active && fighter.squad_leader_ship_slot == 0 &&
                fighter.ai_behavior_code == 5 &&
                fighter.ship_class_id ==
                    static_cast<std::int16_t>(ammo_type - 0x80) &&
                !NovaAiShip_IsDisabled(state, fighter)) {
              ++loaded;
            }
          }
        }
        const std::int32_t remaining = current_owned - (unit + 1);
        const std::int32_t excess = loaded - remaining * weapon->max_ammo;
        if (excess > 0 && loaded > 0) {
          result.block = OutfitSaleBlock::kWeaponAmmo;
          result.excess = static_cast<std::int16_t>(excess);
          result.blocker_plural = excess >= 2;
          result.item_plural = unit < allowed - 1;
          // Name the ammo outfit whose ModType-3 slot feeds this weapon.
          for (std::size_t i = 0; i < state.scenario.outfits.size(); ++i) {
            bool feeds = false;
            for (const auto &[type, value] :
                 OutfitModSlots(state.scenario.outfits[i])) {
              if (type == 3 && value == weapon_index) {
                feeds = true;
                break;
              }
            }
            if (feeds) {
              result.blocker_id = static_cast<std::int16_t>(
                  static_cast<std::int32_t>(i) + 0x80);
              break;
            }
          }
          blocked = true;
        }
      }
    }
    if (blocked)
      break;

    state.inventory.outfit_owned_count[index]--;
    NovaOutfit_RecomputeOutfitDerivedState(state);
    // Clear the active weapon bank when the last unit of a weapon outfit went
    // away (0x0048ea70).
    if (state.inventory.outfit_owned_count[index] < 1) {
      for (const auto &[type, value] : mod_slots) {
        if (type == 1 && state.player.active_weapon_bank_slot == value) {
          state.player.active_weapon_bank_slot = -1;
          state.stat_cache_valid = false;
          break;
        }
      }
    }
    state.player.credits +=
        current_owned <= opening ? resale_value : scaled_price;
    NovaLanded_ExecuteControlSet(state, outfit->on_sell_expr);
    ++result.sold;
  }
  if (result.sold > 0)
    NovaWeapon_RebuildBanksFromOwnedOutfits(state);
  return result;
}

void NovaLanded_CloseOutfitterSession(GameState &state) {
  // 0x0048ea70 removes each owned temporary outfit as the modal exits, then
  // rebuilds derived stats before capping the current meters to those maxima.
  for (std::size_t i = 0; i < state.scenario.outfits.size() &&
                          i < state.inventory.outfit_owned_count.size();
       ++i) {
    if (state.inventory.outfit_owned_count[i] > 0 &&
        (state.scenario.outfits[i].flags & 0x0010U) != 0U) {
      state.inventory.outfit_owned_count[i] = 0;
    }
  }
  NovaWeapon_RebuildBanksFromOwnedOutfits(state);
  state.stat_cache_valid = false;
  const PlayerEffectiveStats stats = Outfit_ComputePlayerEffectiveStats(state);
  state.player.fuel_points =
      std::min(state.player.fuel_points, stats.fuel_capacity);
  state.player.shield_points =
      std::min(state.player.shield_points, stats.max_shield_points);
  state.player.armor_points =
      std::min(state.player.armor_points, stats.max_armor_points);
}

// Ghidra DAT_007d4bbc / DAT_007d4bc0: rank-derived price scales, both set to
// 1.0 at 0x00491f9c and folded multiplicatively by each active rank's modifier
// (rank record +0x08 * 0.01). Rank price modifiers are not modelled yet, so
// both stay 1.0. DAT_007d4bbc scales the first trade-in stage; DAT_007d4bc0
// scales the second trade-in stage and the new ship's list price.
// TODO(decomp): model the rank price scales.
constexpr float kRankResaleScale = 1.0F;   // DAT_007d4bbc
constexpr float kRankPurchaseScale = 1.0F; // DAT_007d4bc0

// Ghidra 0x00498dc0 / 0x004948b0 / 0x00492f30: the base trade-in from
// Ship_ComputeTradeInValue scaled through two identical tech-discount stages,
// both using the current ship's tech level and the destination stellar's.
std::int32_t NovaLanded_ShipTradeInValue(const GameState &state,
                                         std::int16_t stellar_id) {
  const ShipClass *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (ship == nullptr || stellar == nullptr)
    return 0;
  const std::int32_t value =
      NovaLanded_ScaledStorePrice(Ship_ComputeTradeInValue(state),
                                  ship->tech_level,
                                  stellar->tech_level,
                                  kRankResaleScale);
  return NovaLanded_ScaledStorePrice(
      value, ship->tech_level, stellar->tech_level, kRankPurchaseScale);
}

// Ghidra 0x00498dc0 / 0x004948b0: the selected ship's scaled list price before
// any trade-in credit (the shipyard's "Ship Price" row).
std::int32_t NovaLanded_ShipPrice(const GameState &state,
                                  std::int16_t stellar_id,
                                  std::int16_t ship_id) {
  const ShipClass *ship = state.scenario.Ship(ship_id);
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (ship == nullptr || stellar == nullptr)
    return 0;
  return NovaLanded_ScaledStorePrice(
      ship->cost, ship->tech_level, stellar->tech_level, kRankPurchaseScale);
}

std::int32_t NovaLanded_ShipPurchasePrice(const GameState &state,
                                          std::int16_t stellar_id,
                                          std::int16_t ship_id) {
  return std::max(0,
                  NovaLanded_ShipPrice(state, stellar_id, ship_id) -
                      NovaLanded_ShipTradeInValue(state, stellar_id));
}

// Ghidra 0x00498dc0 NovaUi_UpdateSelectedShipPurchaseAllowed (partial port:
// selected/current class and net trade-in affordability; the original's full
// availability checks remain TODO(decomp)).
bool NovaLanded_CanBuyShip(const GameState &state,
                           std::int16_t stellar_id,
                           std::int16_t ship_id) {
  return IsValidShip(state, ship_id) &&
         ship_id != state.player.ship_class_id + 0x80 &&
         state.player.credits >=
             NovaLanded_ShipPurchasePrice(state, stellar_id, ship_id);
}

// Ghidra 0x00498dc0's escort-hire arm. The multiplier is DAT_00575950,
// the double 0.1; the original's FIST/residual correction truncates toward
// zero, not round-to-nearest.
std::int32_t NovaLanded_ShipHirePrice(const GameState &state,
                                      std::int16_t stellar_id,
                                      std::int16_t ship_id) {
  const ShipClass *ship = state.scenario.Ship(ship_id);
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (ship == nullptr || stellar == nullptr)
    return 0;
  const std::int32_t scaled = NovaLanded_ScaledStorePrice(
      ship->cost, ship->tech_level, stellar->tech_level);
  return std::max(0,
                  static_cast<std::int32_t>(static_cast<float>(scaled) * 0.1F));
}

// Ghidra 0x00498dc0 (hire arm): affordability against the hire price only --
// no trade-in, no current-class rejection. The Require/Availability gates
// already ran when the listing was built (0x00469e90).
bool NovaLanded_CanHireShip(const GameState &state,
                            std::int16_t stellar_id,
                            std::int16_t ship_id) {
  return IsValidShip(state, ship_id) &&
         state.player.credits >=
             NovaLanded_ShipHirePrice(state, stellar_id, ship_id);
}

// Ghidra 0x00492f30's hire-mode confirm arm.
int NovaLanded_HireShip(GameState &state,
                        std::int16_t stellar_id,
                        std::int16_t ship_id) {
  if (!NovaLanded_CanHireShip(state, stellar_id, ship_id))
    return -1;
  state.player.credits -= NovaLanded_ShipHirePrice(state, stellar_id, ship_id);
  const int slot = NovaShipClass_SpawnEscortShipFromClass(
      state, static_cast<std::int16_t>(ship_id - 0x80), stellar_id);
  if (slot == -1) {
    // Refund: the original deducts before spawning and leaves the player
    // charged even on the (unreachable in practice) allocation failure;
    // the port restores the credits instead.
    state.player.credits +=
        NovaLanded_ShipHirePrice(state, stellar_id, ship_id);
    return -1;
  }
  // The original rerolls ShipClassDef +0xa2a to rand(100)+1 after a hire so
  // the class drops off the daily hire list; the clean-room keeps the roll in
  // GameState.ship_class_threshold_rolls.
  const std::size_t def = static_cast<std::size_t>(ship_id - 0x80);
  if (def < state.ship_class_threshold_rolls.size()) {
    state.ship_class_threshold_rolls[def] = static_cast<std::int16_t>(
        std::uniform_int_distribution<int>{0, 99}(state.rng) + 1);
  }
  return slot;
}

bool Player_SwapShipWithEscort(GameState &state,
                               std::int16_t stellar_id,
                               std::int16_t ship_id,
                               std::string_view player_ship_name) {
  if (!NovaLanded_CanBuyShip(state, stellar_id, ship_id))
    return false;
  const ShipClass *old_ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  const ShipClass *new_ship = state.scenario.Ship(ship_id);
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  state.player.credits -=
      NovaLanded_ShipPurchasePrice(state, stellar_id, ship_id);
  NovaLanded_ExecuteControlSet(state, old_ship->on_retire_expr);
  state.player.ship_class_id = static_cast<std::int16_t>(ship_id - 0x80);
  state.player.ship_name.assign(player_ship_name.empty() ? new_ship->short_name
                                                         : player_ship_name);
  for (std::size_t i = 0; i < 0x200 && i < state.scenario.outfits.size(); ++i)
    if (!state.scenario.outfits[i].persistent_on_ship_swap)
      state.inventory.outfit_owned_count[i] = 0;
  for (std::size_t i = 0; i < new_ship->default_outfit_ids.size(); ++i) {
    const auto id = new_ship->default_outfit_ids[i];
    if (IsValidOutfit(state, id) && new_ship->default_outfit_counts[i] > 0)
      (void)Outfit_AddInstalledOutfit(
          state, id, new_ship->default_outfit_counts[i]);
  }
  // Player_SwapShipWithEscort (0x00423fa0) seeds the new ship's weapon
  // banks from its mounted stock weapons, then runs
  // Weapon_ReconcileOutfitPoolWithWeaponBanks so the stock guns become owned,
  // sellable outfits. Our ShipClass carries the stock as stock_weapons (the
  // loader's default_weapon_ammo/secondary mapping), so seed + reconcile here.
  NovaWeapon_SeedBanksFromShipStock(state, state.player.ship_class_id);
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
  state.stat_cache_valid = false;
  const PlayerEffectiveStats stats = Outfit_ComputePlayerEffectiveStats(state);
  state.player.active_weapon_bank_slot = -1;
  state.player.shield_points = stats.max_shield_points;
  state.player.armor_points = stats.max_armor_points;
  state.player.fuel_points = stats.fuel_capacity;
  state.player.pos_x = static_cast<float>(stellar->pos_x);
  state.player.pos_y = static_cast<float>(stellar->pos_y);
  state.player.vel_x = 0.0F;
  state.player.vel_y = 0.0F;
  NovaLanded_ExecuteControlSet(state, new_ship->on_purchase_expr);
  return true;
}

// ---------------------------------------------------------------------------
// Escort fleet trade + payroll (Ghidra 0x004229d0 / 0x004232d0)
// ---------------------------------------------------------------------------
namespace {

// Ghidra Ship_FormatLocalizedCountWord (0x00465d90): 1..10 load STR# 0x89
// "Date/Numbers" entries 0x1d..0x26 ("one".."ten"); anything else is decimal
// digits. The translate_first input-map highlight quirk is not modelled (see
// the identical travel.cpp FormatArrivalCountWord).
[[nodiscard]] std::string FormatLocalizedCountWord(int count) {
  if (count < 1 || count > 10) {
    return std::to_string(count);
  }
  if (auto word = NovaHud_LoadStringEntry(
          0x89, static_cast<std::uint16_t>(count + 0x1c))) {
    return *word;
  }
  return std::to_string(count);
}

// Ghidra CString_AppendFormattedQuantity (0x00465c10): plain digits below
// 1000, comma grouping below one million, x.xxM above.
[[nodiscard]] std::string FormattedQuantity(std::uint32_t value) {
  if (value < 1000) {
    return std::to_string(value);
  }
  if (value < 1000000) {
    const auto thousands = value / 1000;
    const auto remainder = value % 1000;
    return std::to_string(thousands) + "," +
           std::string(remainder < 100 ? 1 : 0, '0') +
           std::string(remainder < 10 ? 1 : 0, '0') + std::to_string(remainder);
  }
  const auto millions = value / 1000000;
  const auto fraction = (value % 1000000) / 10000;
  return std::to_string(millions) + "." +
         std::string(fraction < 10 ? 1 : 0, '0') + std::to_string(fraction) +
         "M";
}

// One 1-based entry from the misc game-strings pool STR# 0x7d2.
[[nodiscard]] std::string MiscString(std::uint16_t entry) {
  return NovaHud_LoadStringEntry(0x7d2, entry).value_or(std::string{});
}

// The cached "credit"/"credits" pair (Ghidra DAT_0072f2cc / DAT_0072f2ec,
// STR# 0x7d2 entries 0x20/0x21).
[[nodiscard]] std::string CreditWord(std::uint32_t amount) {
  return MiscString(amount < 2 ? 0x20 : 0x21);
}

// Reseeds the class-default weapon secondary ("carried ammo") counters for
// every one of the 0x100 banks, leaving the loaded-ammo counters untouched —
// the fleet pass's third phase. The clean-room ShipClass carries only the
// eight stock banks, so non-stock slots reset to 0. Keep the NPC-bank init
// cache in step so the stock ammo is not re-expanded after an upgrade zeroed
// it (NovaWeapon_EnsureNpcWeaponBanks).
void ReseedWeaponSecondary(Ship &ship, const ShipClass *cls) {
  ship.npc_weapon_bank_secondary.fill(0);
  if (cls != nullptr) {
    for (const ShipDefaultWeaponBank &stock : cls->stock_weapons) {
      if (stock.weapon_id < 0x80 || stock.weapon_id >= 0x180) {
        continue;
      }
      ship.npc_weapon_bank_secondary[static_cast<std::size_t>(
          stock.weapon_id - 0x80)] = stock.ammo_load;
    }
  }
  ship.npc_weapon_banks_ship_class = ship.ship_class_id;
}

} // namespace

// Ghidra 0x004229d0 Player_ProcessEscortFleetAtStellar.
void Player_ProcessEscortFleetAtStellar(
    GameState &state,
    std::int16_t stellar_id,
    const std::function<void(const std::string &)> &show_text) {
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar != nullptr && (stellar->flags & 0x8U) != 0U) {
    int sold = 0;
    int upgraded = 0;
    std::uint32_t sold_value = 0;
    std::uint32_t upgraded_cost = 0;

    // (1) Sell every escort marked for release (ShipState +0xBE).
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      Ship &ship = state.ShipAt(slot);
      if (!ship.is_active || ship.squad_leader_ship_slot != 0 ||
          ship.ai_behavior_code != 6 || ship.escort_released_mark == 0 ||
          ship.mission_fleet_slot != -1 || NovaAiShip_IsDisabled(state, ship)) {
        continue;
      }
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      const std::int32_t value = cls != nullptr ? cls->escort_sell_value : 0;
      state.player.credits += value;
      sold_value += static_cast<std::uint32_t>(value);
      ship.is_active = false;
      ship.squad_leader_ship_slot = -1;
      state.stat_cache_valid = false;
      ++sold;
    }

    // (2) Upgrade marked escorts (ShipState +0xBF), then (3) refill
    // shield/armor and the weapon secondary for every slot 1..0x3f — the
    // original runs the refill unconditionally inside the same loop.
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      Ship &ship = state.ShipAt(slot);
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      if (ship.is_active && ship.squad_leader_ship_slot == 0 &&
          ship.ai_behavior_code == 6 && ship.escort_upgrade_mark != 0 &&
          cls != nullptr && cls->upgrade_to_ship_class_id != -1 &&
          ship.mission_fleet_slot == -1 &&
          cls->escort_upgrade_cost <= state.player.credits) {
        const std::int32_t cost = cls->escort_upgrade_cost;
        ++upgraded;
        upgraded_cost += static_cast<std::uint32_t>(cost);
        state.player.credits -= cost;
        state.stat_cache_valid = false;
        ship.ship_class_id = cls->upgrade_to_ship_class_id;
        ship.npc_weapon_bank_ammo.fill(0);
        ship.npc_weapon_bank_secondary.fill(0);
        ship.escort_upgrade_mark = 0;
        cls = state.scenario.Ship(
            static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      }
      if (cls != nullptr) {
        // The original reads ShipClassDef.base_shield_points (+0x5C) /
        // base_armor_points (+0x60); the clean-room class carries the same
        // Bible Shield/Armor values as base_shield/base_armor.
        ship.shield_points = static_cast<float>(cls->base_shield);
        ship.armor_points = static_cast<float>(cls->base_armor);
      }
      ReseedWeaponSecondary(ship, cls);
    }

    if (sold > 0 || upgraded > 0) {
      std::string text;
      if (sold > 0) {
        text += FormatLocalizedCountWord(sold);
        text += " ";
        text += MiscString(sold == 1 ? 0x12a : 299);
        text += " ";
        text += MiscString(300);
        text += " ";
        text += FormattedQuantity(sold_value);
        text += " ";
        text += CreditWord(sold_value);
        text += ".";
        if (upgraded > 0) {
          text += "\r\r";
        }
      }
      if (upgraded > 0) {
        text += FormatLocalizedCountWord(upgraded);
        text += " ";
        text += MiscString(upgraded == 1 ? 0x12a : 299);
        text += " ";
        text += MiscString(0x12d);
        text += " ";
        text += FormattedQuantity(upgraded_cost);
        text += " ";
        text += CreditWord(upgraded_cost);
        text += ".";
      }
      if (show_text) {
        show_text(text);
      }
      // The sale/upgrade booking takes (sold + upgraded) / 2 days. The
      // original also emits a debug-only date trace before ticking.
      const std::int16_t days =
          static_cast<std::int16_t>((sold + upgraded) / 2);
      for (std::int16_t i = 0; i < days; ++i) {
        Mission_TickDailyWorldUpdate(state);
      }
    }
  }
  // Tail: unconditionally charge one payroll period (0x004232d0).
  Player_ProcessEscortPayroll(state, 1, show_text);
}

// Ghidra 0x004232d0 Player_ProcessEscortPayroll.
void Player_ProcessEscortPayroll(
    GameState &state,
    std::int16_t periods,
    const std::function<void(const std::string &)> &show_text) {
  int defected = 0;
  for (std::int16_t period = 0; period < periods; ++period) {
    for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
      Ship &ship = state.ShipAt(slot);
      if (!ship.is_active || ship.squad_leader_ship_slot != 0 ||
          ship.ai_behavior_code != 6 || NovaAiShip_IsDisabled(state, ship)) {
        continue;
      }
      // A mission-fleet escort whose mission has spawned its fleet is exempt
      // from upkeep (and cannot defect for non-payment).
      const std::int16_t fleet_slot = ship.mission_fleet_slot;
      bool mission_fleet_escort = false;
      if (fleet_slot >= 0 && static_cast<std::size_t>(fleet_slot) <
                                 GameState::kMaxActiveMissions) {
        const auto index = static_cast<std::size_t>(fleet_slot);
        mission_fleet_escort =
            state.active_mission_runtime_flags[index].is_active &&
            state.active_missions[index].ship_behavior == 1;
      }
      if (mission_fleet_escort || ship.escort_origin_mark == 0) {
        continue;
      }
      const ShipClass *cls = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      // Upkeep is trunc(base_cost * 0.01); the 0.01 is the shared double at
      // 0x00575298 (x87 FIST truncation idiom at 0x004233bb).
      const std::int32_t upkeep =
          cls != nullptr
              ? static_cast<std::int32_t>(static_cast<double>(cls->cost) * 0.01)
              : 0;
      if (state.player.credits < upkeep) {
        ++defected;
        if (ship.ai_behavior_code == 6 && fleet_slot == -1 && cls != nullptr &&
            cls->default_ai_behavior < 3) {
          // TODO(decomp(0x00469810)) skipped: the defecting escort takes its
          // share of the player's cargo/junk
          // (Player_TransferCargoAndJunkToEscortByRatio); per-escort cargo
          // bins are not modelled yet (same skip as escort_formation.cpp and
          // ship_visual.cpp).
        }
        state.stat_cache_valid = false;
        ship.is_active = false;
        ship.squad_leader_ship_slot = -1;
        ship.ai_behavior_code = 1;
      } else {
        state.player.credits -= upkeep;
        state.stat_cache_valid = false;
      }
    }
  }
  if (defected > 0 && show_text) {
    show_text(MiscString(defected == 1 ? 0x12e : 0x12f));
  }
}

} // namespace game
