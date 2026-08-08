#include "landed_store.hpp"

#include "outfit.hpp"
#include "weapon.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

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
  std::uint32_t lo = 0;
  std::uint32_t hi = 0;
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

[[nodiscard]] std::int32_t RoundNearest(float value) {
  return static_cast<std::int32_t>(std::round(value));
}

} // namespace

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
            return IsValidOutfit(state, id) &&
                   state.inventory.outfit_owned_count[id - 0x80] > 0;
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

LandedStoreSession NovaLanded_OpenOutfitterSession(const GameState &state,
                                                   std::int16_t stellar_id) {
  LandedStoreSession session;
  session.kind = LandedStoreKind::kOutfitter;
  session.opening_outfit_counts = state.inventory.outfit_owned_count;
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr)
    return session;
  const auto expression_state = NovaLanded_ControlExpressionState(state);
  for (std::size_t i = 0; i < state.scenario.outfits.size() && i < 0x200; ++i) {
    const Outfit &outfit = state.scenario.outfits[i];
    // ScenarioData keeps resource-addressable holes in the 0x200-entry table.
    // The original marks absent runtime definitions invalid; an empty record
    // name is our corresponding validity marker.
    if (outfit.name.empty())
      continue;
    const bool owned = state.inventory.outfit_owned_count[i] > 0;
    bool visible = HasTech(*stellar, outfit.tech_level);
    if (owned && (stellar->availability_flags & 0x400U) != 0U &&
        (outfit.flags & 0x0008U) == 0U)
      visible = true;
    if (!owned && (outfit.flags & 0x4000U) != 0U)
      visible = visible && NovaControlExpression_Evaluate(
                               outfit.availability_expr, expression_state);
    if (!owned && (outfit.flags & 0x0100U) != 0U)
      visible =
          visible && MeetsRequire(state, outfit.require_lo, outfit.require_hi);
    if (owned && (outfit.flags & 0x0800U) != 0U)
      visible = true;
    if (visible)
      session.available_ids.push_back(static_cast<std::int16_t>(i + 0x80));
  }
  // 0x0046a220: an eligible item with flag 0x1000 suppresses later resource
  // IDs carrying the same display weight. This is intentionally applied
  // before the weight ordering, matching the original's resource-order pass.
  for (std::size_t i = 0; i < session.available_ids.size(); ++i) {
    const Outfit *outfit = state.scenario.Outfit(session.available_ids[i]);
    if ((outfit->flags & 0x1000U) == 0U)
      continue;
    const auto weight = outfit->display_weight;
    session.available_ids.erase(
        std::remove_if(
            session.available_ids.begin() + static_cast<std::ptrdiff_t>(i + 1),
            session.available_ids.end(),
            [&state, weight](std::int16_t candidate) {
              return state.scenario.Outfit(candidate)->display_weight == weight;
            }),
        session.available_ids.end());
  }
  std::stable_sort(session.available_ids.begin(),
                   session.available_ids.end(),
                   [&state](auto a, auto b) {
                     return state.scenario.Outfit(a)->display_weight >
                            state.scenario.Outfit(b)->display_weight;
                   });
  return session;
}

LandedStoreSession NovaLanded_OpenShipyardSession(const GameState &state,
                                                  std::int16_t stellar_id) {
  LandedStoreSession session;
  session.kind = LandedStoreKind::kShipyard;
  session.opening_outfit_counts = state.inventory.outfit_owned_count;
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (stellar == nullptr)
    return session;
  const auto expression_state = NovaLanded_ControlExpressionState(state);
  for (std::size_t i = 0; i < state.scenario.ships.size() && i < 0x200; ++i) {
    const ShipClass &ship = state.scenario.ships[i];
    // 0x00469e90 first applies the per-class daily BuyRandom availability
    // latch. A zero BuyRandom is unconditional: the class is never offered
    // for purchase. Nova's many mission/paint/loadout variants deliberately
    // use zero here, so admitting them based on tech alone floods the list
    // with duplicate hulls. Positive-percent daily rolls remain TODO(decomp)
    // until the per-day runtime availability state is represented.
    if (ship.display_name.empty() || ship.buy_random <= 0 ||
        !HasTech(*stellar, ship.tech_level))
      continue;
    if ((ship.availability_flags & 0x0200U) != 0U &&
        !MeetsRequire(state, ship.require_lo, ship.require_hi))
      continue;
    if ((ship.availability_flags & 0x0100U) != 0U &&
        !NovaControlExpression_Evaluate(ship.availability_expr,
                                        expression_state))
      continue;
    session.available_ids.push_back(static_cast<std::int16_t>(i + 0x80));
  }
  // 0x00469e90 uses the corresponding ship-class flag 0x4000.
  for (std::size_t i = 0; i < session.available_ids.size(); ++i) {
    const ShipClass *ship = state.scenario.Ship(session.available_ids[i]);
    if ((ship->availability_flags & 0x4000U) == 0U)
      continue;
    const auto weight = ship->display_weight;
    session.available_ids.erase(
        std::remove_if(
            session.available_ids.begin() + static_cast<std::ptrdiff_t>(i + 1),
            session.available_ids.end(),
            [&state, weight](std::int16_t candidate) {
              return state.scenario.Ship(candidate)->display_weight == weight;
            }),
        session.available_ids.end());
  }
  std::stable_sort(session.available_ids.begin(),
                   session.available_ids.end(),
                   [&state](auto a, auto b) {
                     return state.scenario.Ship(a)->display_weight >
                            state.scenario.Ship(b)->display_weight;
                   });
  return session;
}

std::int32_t NovaLanded_ScaledStorePrice(std::int32_t base_price,
                                         std::int16_t item_tech,
                                         std::int16_t stellar_tech,
                                         float scale) {
  if (base_price <= 0)
    return 0;
  std::int32_t value = base_price;
  if (item_tech >= 0 && stellar_tech >= 0 && item_tech < 6 &&
      stellar_tech < 6 && item_tech < stellar_tech && base_price > 99) {
    value = RoundNearest(
        static_cast<float>(base_price) *
        static_cast<float>(100 - (stellar_tech - item_tech) * 3) * 0.01F);
  }
  value = std::max(1, RoundNearest(static_cast<float>(value) * scale));
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

std::int32_t NovaLanded_FreeMass(const GameState &state) {
  const ShipClass *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (ship == nullptr)
    return 0;
  std::int32_t free_mass = ship->free_mass;
  for (std::size_t i = 0; i < state.inventory.outfit_owned_count.size() &&
                          i < state.scenario.outfits.size();
       ++i) {
    free_mass -=
        static_cast<std::int32_t>(state.inventory.outfit_owned_count[i]) *
        state.scenario.outfits[i].PurchaseMass(ship->mass_tons);
  }
  return free_mass;
}

bool NovaLanded_CanBuyOutfit(const GameState &state,
                             std::int16_t stellar_id,
                             std::int16_t outfit_id) {
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  const Outfit *outfit = state.scenario.Outfit(outfit_id);
  if (stellar == nullptr || outfit == nullptr ||
      !HasTech(*stellar, outfit->tech_level) ||
      !MeetsRequire(state, outfit->require_lo, outfit->require_hi) ||
      !NovaControlExpression_Evaluate(outfit->availability_expr,
                                      NovaLanded_ControlExpressionState(state)))
    return false;
  const ShipClass *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (ship == nullptr ||
      outfit->PurchaseMass(ship->mass_tons) > NovaLanded_FreeMass(state))
    return false;
  const OutfitOwnership ownership = Outfit_ClampOwnedCountToLimits(
      state, static_cast<std::int16_t>(outfit_id - 0x80));
  return ownership.effective_owned < ownership.max_allowed &&
         state.player.credits >=
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
    if (Outfit_AddInstalledOutfit(
            state, static_cast<std::int16_t>(outfit_id - 0x80), 1) != 1)
      break;
    state.player.credits -= price;
    NovaLanded_ExecuteControlSet(
        state, state.scenario.Outfit(outfit_id)->on_purchase_expr);
  }
  if (bought > 0)
    NovaWeapon_RebuildBanksFromOwnedOutfits(state);
  return bought;
}

std::int16_t NovaLanded_SellOutfit(GameState &state,
                                   LandedStoreSession &session,
                                   std::int16_t outfit_id,
                                   std::int16_t requested) {
  if (requested <= 0 || !IsValidOutfit(state, outfit_id))
    return 0;
  Outfit const *outfit = state.scenario.Outfit(outfit_id);
  if ((outfit->flags & 0x0008U) != 0U)
    return 0; // original no-sell marker
  const std::size_t index = static_cast<std::size_t>(outfit_id - 0x80);
  const std::int16_t allowed =
      std::min(requested, state.inventory.outfit_owned_count[index]);
  const ShipClass *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (ship == nullptr)
    return 0;
  const std::int32_t purchase_mass = outfit->PurchaseMass(ship->mass_tons);
  std::int16_t removed = 0;
  for (; removed < allowed; ++removed) {
    // 0x0048ea70 rejects removal of a negative-mass outfit when that removal
    // would leave the hull over its mass allowance. Check per unit so a
    // modifier-assisted sale can still complete its valid prefix.
    if (purchase_mass < 0 && NovaLanded_FreeMass(state) + purchase_mass < 0)
      break;
    state.inventory.outfit_owned_count[index]--;
    OutfitMarkStatsDirty(state);
  }
  if (removed > 0) {
    // Pre-opening stock is paid at the normal resale rate; purchases made in
    // this session are refunded at their paid cost (the original snapshot
    // distinguishes these two cases).
    const std::int16_t opening = session.opening_outfit_counts[index];
    const std::int16_t before = static_cast<std::int16_t>(
        state.inventory.outfit_owned_count[index] + removed);
    const std::int16_t refund_count =
        std::max<std::int16_t>(0, before - opening);
    const std::int16_t resale_count =
        static_cast<std::int16_t>(removed - std::min(removed, refund_count));
    state.player.credits +=
        refund_count * outfit->cost + resale_count * (outfit->cost / 2);
    NovaLanded_ExecuteControlSet(state, outfit->on_sell_expr);
    NovaWeapon_RebuildBanksFromOwnedOutfits(state);
  }
  return removed;
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

std::int32_t NovaLanded_ShipTradeInValue(const GameState &state,
                                         std::int16_t stellar_id) {
  const ShipClass *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (ship == nullptr || stellar == nullptr)
    return 0;
  std::int64_t total = NovaLanded_ScaledStorePrice(
      ship->cost, ship->tech_level, stellar->tech_level);
  for (std::size_t i = 0; i < state.scenario.outfits.size() && i < 0x200; ++i)
    total += static_cast<std::int64_t>(state.inventory.outfit_owned_count[i]) *
             state.scenario.outfits[i].cost / 2;
  return static_cast<std::int32_t>(
      std::min<std::int64_t>(total, std::numeric_limits<std::int32_t>::max()));
}

std::int32_t NovaLanded_ShipPurchasePrice(const GameState &state,
                                          std::int16_t stellar_id,
                                          std::int16_t ship_id) {
  const ShipClass *ship = state.scenario.Ship(ship_id);
  const Stellar *stellar = state.scenario.Stellar(stellar_id);
  if (ship == nullptr || stellar == nullptr)
    return 0;
  return std::max(0,
                  NovaLanded_ScaledStorePrice(
                      ship->cost, ship->tech_level, stellar->tech_level) -
                      NovaLanded_ShipTradeInValue(state, stellar_id));
}

bool NovaLanded_CanBuyShip(const GameState &state,
                           std::int16_t stellar_id,
                           std::int16_t ship_id) {
  return IsValidShip(state, ship_id) &&
         ship_id != state.player.ship_class_id + 0x80 &&
         state.player.credits >=
             NovaLanded_ShipPurchasePrice(state, stellar_id, ship_id);
}

bool NovaLanded_ReplacePlayerShip(GameState &state,
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
  NovaWeapon_RebuildBanksFromOwnedOutfits(state);
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

} // namespace game
