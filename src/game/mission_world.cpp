#include "mission.hpp"

#include "brgr_archive.hpp"
#include "compatibility.hpp"
#include "game_state.hpp"
#include "government.hpp"
#include "log.hpp"
#include "mission_internal.hpp"
#include "mission_script.hpp"
#include "nova_random.hpp"
#include "outfit.hpp"
#include "rank.hpp"
#include "targeting.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace game {

using mission_detail::kResourceIdBase;

// Ghidra 0x00466c40 Mission_AdvanceGameDate.
void Mission_AdvanceGameDate(GameDate &date) {
  std::int16_t month_days = 31;
  switch (date.month) {
  case 4:
  case 6:
  case 9:
  case 11:
    month_days = 30;
    break;
  case 2:
    // Leap quirk kept: (year + (year < 0 ? 3 : 0)) & 3 == fixup in the
    // original, i.e. simply year divisible by 4.
    month_days = (date.year & 3) == 0 ? 29 : 28;
    break;
  default:
    break;
  }
  date.day = static_cast<std::int16_t>(date.day + 1);
  if (date.day > month_days) {
    date.day = 1;
    date.month = static_cast<std::int16_t>(date.month + 1);
    if (date.month > 12) {
      date.month = 1;
      date.year = static_cast<std::int16_t>(date.year + 1);
    }
  }
}

// Ghidra 0x0043f080 Mission_ComputeDateAfterSteps. The original
// leaves the out buffer untouched for steps < 1; callers only invoke it with
// a positive count (see Mission_ActivateAtSlot).
GameDate Mission_ComputeDateAfterSteps(const GameState &state,
                                       std::int16_t steps) {
  GameDate out = state.date;
  for (std::int16_t i = 0; i < steps; ++i) {
    Mission_AdvanceGameDate(out);
  }
  return out;
}

// Ghidra 0x00465550 Stellar_ComputeHyperspaceTravelDays.
int NovaStellar_ComputeHyperspaceTravelDays(const GameState &state,
                                            const Ship &ship) {
  const ShipClass *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(ship.ship_class_id + kResourceIdBase));
  if (ship_class == nullptr) {
    return 1;
  }
  int days = ship_class->mass_tons <= 99 ? 1 : 2;
  if (ship_class->mass_tons > 199) {
    ++days;
  }
  // The jump-time outfit arm applies only to the player (ShipState +0x86
  // ship_instance_id == 0): every owned outfit whose one of the four
  // ModTypes is 0x16 adds owned-count x ModVal days.
  if (ship.ship_instance_id == 0) {
    for (std::size_t outfit = 0;
         outfit < state.inventory.outfit_owned_count.size();
         ++outfit) {
      const std::int16_t owned = state.inventory.outfit_owned_count[outfit];
      if (owned <= 0) {
        continue;
      }
      const Outfit *def = state.scenario.Outfit(
          static_cast<std::int16_t>(outfit + kResourceIdBase));
      if (def == nullptr) {
        continue;
      }
      if (def->mod_type == 0x16) {
        days += owned * def->mod_val;
      }
      for (std::size_t i = 0; i < def->alt_mod_types.size(); ++i) {
        if (def->alt_mod_types[i] == 0x16) {
          days += owned * def->alt_mod_vals[i];
        }
      }
    }
  }
  return std::max(days, 1);
}

namespace {

// The original's month/day lower-bound arm for the First* triple; the year
// bound is handled by the caller. Reproduces 0x0046c829-0x0046c842:
// - FirstMonth == 0 gates on FirstDay alone (day-of-month, any month);
// - FirstMonth > 0 && FirstDay == 0 gates on the month alone;
// - both > 0 gate on the month*0x20 + day composite.
[[nodiscard]] bool CronFirstMonthDayAllows(const CronEventDef &def,
                                           const GameDate &date) {
  if (def.first_month == 0) {
    return !(def.first_day > 0 && date.day < def.first_day);
  }
  if (def.first_month < 1 || def.first_day != 0) {
    return !(def.first_month > 0 && def.first_day > 0 &&
             date.month * 0x20 + date.day <
                 def.first_month * 0x20 + def.first_day);
  }
  return date.month >= def.first_month;
}

// The original's month/day upper-bound arm for the Last* triple (0x0046c8b0+).
// LastMonth == 0 gates on LastDay alone; LastMonth > 0 && LastDay == 0 gates on
// the month alone; both > 0 gate on the month*0x20 + day composite.
[[nodiscard]] bool CronLastMonthDayAllows(const CronEventDef &def,
                                          const GameDate &date) {
  if (def.last_month == 0) {
    return def.last_day <= 0 || date.day <= def.last_day;
  }
  if (def.last_month < 1 || def.last_day != 0) {
    return !(def.last_month > 0 && def.last_day > 0 &&
             def.last_month * 0x20 + def.last_day <
                 date.month * 0x20 + date.day);
  }
  return date.month <= def.last_month;
}

// Ghidra 0x0046c800 Frame_IsRectVisibleInViewport (the crön-event arm; the
// same helper also culls rects elsewhere). Called with a g_cron_event_states
// block pointer it reads FirstYear/FirstMonth/FirstDay (+2/+4/+6) and
// LastYear/LastMonth/LastDay (+0x10/+0x12/+0x14) as an activation date
// window against the current game date:
// - FirstYear > 0 && year < FirstYear -> too early. FirstMonth == 0 gates on
//   FirstDay alone (day-of-month, any month); FirstMonth > 0 && FirstDay == 0
//   gates on the month alone; both > 0 gate on month*0x20 + day. A negative
//   FirstMonth or FirstDay field means "ignore this field" (Bible: 0 or -1).
// - Last* mirror the check with the comparisons inverted.
// Quirks preserved verbatim, including the month*0x20 composite comparison.
[[nodiscard]] bool CronEventDateWindowAllows(const CronEventDef &def,
                                             const GameDate &date) {
  if (kApplyOriginalBugFixes) {
    // BUGFIX(original): the original checks the year bounds and the month/day
    // bounds independently, so a range whose endpoints share a month/day
    // composite (1/1/1178-1/1/1179) collapses to that single day-of-year.
    // Apply the month/day bound only at the boundary year, making a multi-year
    // range the contiguous interval the Bible describes.
    if (def.first_year > 0) {
      if (date.year < def.first_year) {
        return false;
      }
      if (date.year == def.first_year && !CronFirstMonthDayAllows(def, date)) {
        return false;
      }
    } else if (!CronFirstMonthDayAllows(def, date)) {
      return false;
    }
    if (def.last_year > 0) {
      if (date.year > def.last_year) {
        return false;
      }
      if (date.year == def.last_year && !CronLastMonthDayAllows(def, date)) {
        return false;
      }
    } else if (!CronLastMonthDayAllows(def, date)) {
      return false;
    }
    return true;
  }
  // Original behaviour, quirks preserved verbatim (month/day checked in every
  // year, independently of the year bounds).
  if (def.first_year > 0 && date.year < def.first_year) {
    return false;
  }
  if (!CronFirstMonthDayAllows(def, date)) {
    return false;
  }
  if (def.last_year > 0 && def.last_year < date.year) {
    return false;
  }
  if (!CronLastMonthDayAllows(def, date)) {
    return false;
  }
  return true;
}

// Ghidra 0x00439750 Mission_ActivateCronEvent. Fires the event's OnStart
// set-string through the reaction-script executor. With flags 0x0001 the
// original re-runs it while the Require mask and EnableOn expression hold,
// bailing out after 0x2711 iterations (a guard against infinite loops).
void Mission_ActivateCronEvent(GameState &state, std::int16_t cron_index) {
  const CronEventDef &def =
      state.scenario.cron_events[static_cast<std::size_t>(cron_index)];
  if ((def.flags & 0x0001U) == 0U) {
    Mission_ExecuteReactionScript(
        state, def.on_start, MissionScriptContext{"cron OnStart"});
    return;
  }
  std::int16_t iterations = 0;
  while (iterations < 0x2711) {
    if (!NovaOutfit_EvaluateRequireMask(
            state, def.require_lo, def.require_hi) ||
        !Mission_CheckReactionConditionSatisfied(state, def.enable_on)) {
      break;
    }
    Mission_ExecuteReactionScript(
        state, def.on_start, MissionScriptContext{"cron OnStart (repeat)"});
    ++iterations;
  }
}

// Ghidra 0x004398b0 Mission_TerminateCronEvent. Fires OnEnd; flags 0x0002
// selects the same iterative arm as activation.
void Mission_TerminateCronEvent(GameState &state, std::int16_t cron_index) {
  const CronEventDef &def =
      state.scenario.cron_events[static_cast<std::size_t>(cron_index)];
  if ((def.flags & 0x0002U) == 0U) {
    Mission_ExecuteReactionScript(
        state, def.on_end, MissionScriptContext{"cron OnEnd"});
    return;
  }
  std::int16_t iterations = 0;
  while (iterations < 0x2711) {
    if (!NovaOutfit_EvaluateRequireMask(
            state, def.require_lo, def.require_hi) ||
        !Mission_CheckReactionConditionSatisfied(state, def.enable_on)) {
      break;
    }
    Mission_ExecuteReactionScript(
        state, def.on_end, MissionScriptContext{"cron OnEnd (repeat)"});
    ++iterations;
  }
}

} // namespace

// Ghidra 0x00439500 Mission_TickDailyCronEvents. Once-per-game-day driver
// over the 0x200 crön slots (called from Mission_TickDailyWorldUpdate right
// after the calendar advance):
// - idle events roll rand(101) against the trigger odds; on a hit, the
//   date-window, Require-mask and EnableOn gates arm the event: active,
//   duration_counter = duration, then either the pre-holdoff wait or an
//   immediate OnStart (with an immediate OnEnd when duration == 0, latching
//   duration_counter to -1 so only the post-holdoff wait remains).
// - active events count down: holdoff first (a pre-holdoff expiry fires
//   OnStart; a post-holdoff expiry deactivates), then duration (OnEnd;
//   deactivation unless a post-holdoff wait keeps the slot busy).
// Events with no TimeLimit (duration == -1, the absent-slot sentinel) are
// never touched.
// Under kApplyOriginalBugFixes four confirmed quirks are corrected (see
// docs/known_original_bugs.md, crön entries): Random is a true 1..100 percent
// roll (the original draws 0..100 and tests <=); a multi-year date range is
// the contiguous interval the Bible describes (the original applies the
// month/day bound in every year); a duration-0 event runs OnEnd once instead
// of twice (the original leaves the slot active with a zero holdoff); and the
// post-end wait uses PostHoldoff while latching duration_counter to -1 after
// OnEnd (the original reloads PreHoldoff at duration end and leaves the
// counter at 0, so the slot re-runs OnStart/OnEnd after the wait and never
// deactivates).
void Mission_TickDailyCronEvents(GameState &state) {
  const std::size_t count = std::min(state.scenario.cron_events.size(),
                                     state.cron_event_states.size());
  for (std::size_t index = 0; index < count; ++index) {
    const CronEventDef &def = state.scenario.cron_events[index];
    auto &runtime = state.cron_event_states[index];
    if (def.duration < 0 || !def.present) {
      continue;
    }
    if (!runtime.is_active) {
      // Original draws NovaRandom_Range(0x65) = 0..100 inclusive and tests
      // roll <= odds, so Random 0 still fires ~1/101 of eligible days.
      // BUGFIX(original): roll 1..100 so the fields are true percentages.
      const int odds_roll =
          kApplyOriginalBugFixes
              ? std::uniform_int_distribution<int>{1, 100}(state.rng)
              : std::uniform_int_distribution<int>{0, 100}(state.rng);
      if (odds_roll > def.trigger_odds) {
        continue;
      }
      if (!CronEventDateWindowAllows(def, state.date) ||
          !NovaOutfit_EvaluateRequireMask(
              state, def.require_lo, def.require_hi) ||
          !Mission_CheckReactionConditionSatisfied(state, def.enable_on)) {
        continue;
      }
      runtime.is_active = true;
      runtime.duration_counter = def.duration;
      if (def.pre_holdoff < 1) {
        runtime.holdoff_counter = 0;
        Mission_ActivateCronEvent(state, static_cast<std::int16_t>(index));
        if (def.duration == 0) {
          Mission_TerminateCronEvent(state, static_cast<std::int16_t>(index));
          runtime.duration_counter = -1;
          if (def.post_holdoff > 0) {
            runtime.holdoff_counter = def.post_holdoff;
          } else if (kApplyOriginalBugFixes) {
            // BUGFIX(original): the original leaves the slot active with a
            // zero holdoff, so the next daily tick runs OnEnd a second time.
            runtime.is_active = false;
          }
        }
      } else {
        runtime.holdoff_counter = def.pre_holdoff;
      }
    } else if (runtime.holdoff_counter < 1) {
      --runtime.duration_counter;
      if (runtime.duration_counter < 1) {
        Mission_TerminateCronEvent(state, static_cast<std::int16_t>(index));
        if (kApplyOriginalBugFixes) {
          // BUGFIX(original): latch the event as ended, so the post-holdoff
          // arm deactivates it instead of re-running OnStart/OnEnd once the
          // wait expires.
          runtime.duration_counter = -1;
        }
        if (def.post_holdoff < 1) {
          runtime.is_active = false;
        } else {
          // Original quirk: the post-end wait is loaded from PreHoldoff, not
          // PostHoldoff (0x004395d9 reads block +0x22 = PreHoldoff), so with
          // PreHoldoff == 0 the slot never deactivates and re-runs OnEnd every
          // day. BUGFIX(original): use the Bible's PostHoldoff.
          runtime.holdoff_counter =
              kApplyOriginalBugFixes ? def.post_holdoff : def.pre_holdoff;
        }
      }
    } else {
      --runtime.holdoff_counter;
      if (runtime.holdoff_counter < 1) {
        if (runtime.duration_counter < 0) {
          runtime.is_active = false;
        } else {
          Mission_ActivateCronEvent(state, static_cast<std::int16_t>(index));
          if (runtime.duration_counter == 0) {
            Mission_TerminateCronEvent(state, static_cast<std::int16_t>(index));
            if (kApplyOriginalBugFixes) {
              // BUGFIX(original): the zero-duration event has now run OnStart
              // and OnEnd; wait out PostHoldoff (or deactivate) instead of
              // re-running OnEnd on the next daily tick.
              runtime.duration_counter = -1;
              if (def.post_holdoff > 0) {
                runtime.holdoff_counter = def.post_holdoff;
              } else {
                runtime.is_active = false;
              }
            }
          }
        }
      }
    }
  }
}

// Ghidra 0x00423540 Player_CollectStellarTribute. Daily tribute pass: every
// available stellar carrying the +0x46 marker (its system visible + the 0x20
// availability bit, set by the display-state refresh) pays its Tribute value
// (payload +0x0a, default 1000 x TechLevel) and bumps its day counter
// (StellarDef +0x2a) unless the currently docked stellar carries the same
// 0x20 marker. TODO(decomp): the domination flow that grants a stellar the
// +0x46 marker is not modelled, so the pass stays idle in practice.
void Player_CollectStellarTribute(GameState &state) {
  const std::size_t count =
      std::min(state.scenario.stellars.size(), static_cast<std::size_t>(0x800));
  for (std::size_t i = 0; i < count; ++i) {
    Stellar &stellar = state.scenario.stellars[i];
    if (!stellar.is_available || !stellar.dominated) {
      continue;
    }
    const Stellar *docked = state.scenario.Stellar(
        static_cast<std::int16_t>(state.travel.selected_stellar_id));
    if (docked == nullptr || (docked->availability_flags & 0x20U) == 0U) {
      ++stellar.domination_days;
    }
    state.player.credits += stellar.tribute;
    // g_playerInventoryAndLoadoutDirty = 1: the port's stat cache is the
    // consumer of that latch.
    state.InvalidateDerivedStatCaches();
  }
}

// Ghidra 0x00424f90 System_UpdateDisasterStates (per-game-day sweep of the
// 0x100 öops slots; runs from the daily world-update driver).
void System_UpdateDisasterStates(GameState &state) {
  for (auto &def : state.scenario.disaster_defs) {
    if (!def.present) {
      // Undefined slot: reset to the idle sentinels the loader leaves.
      def.active_stellar = -1;
      def.days_remaining = -1;
      def.started_once = false;
      continue;
    }
    if (def.days_remaining >= 1) {
      // Active: burn one day off the remaining duration.
      --def.days_remaining;
      continue;
    }
    if (def.started_once) {
      // Dead arm in the shipped code (started_once is never set), kept for
      // parity: a once-started disaster with a negative duration repeats
      // for two days.
      if (def.duration_days < 0) {
        def.days_remaining = 2;
      }
      continue;
    }
    // Idle and never started: roll the per-day chance, then gate on the
    // ActivateOn expression.
    const std::int16_t roll = RandomBelow(state, 100);
    if (roll + 1 > def.start_chance_percent) {
      continue;
    }
    if (!Mission_CheckReactionConditionSatisfied(state,
                                                 def.activation_expression)) {
      continue;
    }
    if (def.target_stellar < static_cast<std::int16_t>(kResourceIdBase)) {
      if (def.target_stellar != -1) {
        // Other sub-0x80 codes are inert (the original's empty inner arm).
        continue;
      }
      // "Any": pick a random available stellar that is not a travel-only
      // (flags 0x20) lane. The original rejects samples over the full 0x800
      // g_stellar_defs table; the clean-room table is shorter, so build the
      // eligible set explicitly (equivalent while the unmodelled tail slots
      // stay unavailable).
      std::vector<std::size_t> candidates;
      for (std::size_t i = 0; i < state.scenario.stellars.size(); ++i) {
        const auto &stellar = state.scenario.stellars[i];
        if (stellar.is_available && (stellar.flags & 0x20U) == 0U) {
          candidates.push_back(i);
        }
      }
      if (candidates.empty()) {
        continue;
      }
      std::uniform_int_distribution<std::size_t> pick(0, candidates.size() - 1);
      def.active_stellar =
          static_cast<std::int16_t>(candidates[pick(state.rng)]);
      def.days_remaining = def.duration_days;
      continue;
    }
    // Bound stellar: rebase the 0x80-based resource id to the 0-based index
    // the exchange compares against.
    def.active_stellar =
        static_cast<std::int16_t>(def.target_stellar - kResourceIdBase);
    def.days_remaining = def.duration_days;
  }
}

// Ghidra 0x00466cb0 ShipClass_RerollShipClassAvailabilityChances (daily
// world-update driver; see mission.hpp for the remaining skipped slices).
void Mission_TickDailyWorldUpdate(GameState &state) {
  Mission_AdvanceGameDate(state.date);
  Mission_TickDailyCronEvents(state);
  // Active-mission deadline countdown (MisnActive +0x45, 16 slots). The
  // -32000 no-deadline sentinel stays negative and is never touched.
  for (auto &mission : state.active_missions) {
    if (mission.time_limit_days_remaining > 0) {
      mission.time_limit_days_remaining =
          static_cast<std::int16_t>(mission.time_limit_days_remaining - 1);
    }
  }
  Player_CollectStellarTribute(state);
  System_UpdateDisasterStates(state);
  const std::size_t system_count =
      std::min(state.scenario.systems.size(), GameState::kMaxSystems);
  for (std::size_t i = 0; i < system_count; ++i) {
    if (state.scenario.systems[i].is_visible &&
        state.reinforcement_retrigger_delay[i] > 0) {
      --state.reinforcement_retrigger_delay[i];
    }
  }
  // Per-stellar daily schedule + garrison resupply (0x800 x 0x498 loop):
  // available stellars only. TODO(decomp) skipped inside this loop: the two
  // daily-zeroed scratch fields (StellarDef +0x2e/+0x494) have no modelled
  // consumer.
  const std::size_t stellar_count =
      std::min(state.scenario.stellars.size(), static_cast<std::size_t>(0x800));
  for (std::size_t i = 0; i < stellar_count; ++i) {
    Stellar &stellar = state.scenario.stellars[i];
    if (!stellar.is_available) {
      continue;
    }
    // Garrison resupply (gated on the +0x46 marker like the income pass): a
    // garrison size above 1000 wraps modulo 1000, and the count creeps back
    // up one ship per 0x1c2-roll hit while below quota.
    if (stellar.dominated) {
      int max = stellar.max_ship_count;
      if (max > 1000) {
        max %= 1000;
      }
      if (stellar.present_ship_count < max &&
          std::uniform_int_distribution<int>{0, 0x1c1}(state.rng) == 0) {
        ++stellar.present_ship_count;
      }
    }
    // Schedule countdown (StellarDef +0x47c, shared with the engagement-
    // access counter). Runs only while the stellar is sprite-active: a
    // negative seed pins the countdown at 1 (never fires); an expiring
    // countdown latches 0xffff, clears the sprite handle and fires the
    // schedule set-string once.
    if (NovaTargeting_IsStellarActive(stellar)) {
      if (stellar.schedule_days < 0) {
        stellar.destroyed_days_remaining = 1;
      } else if (--stellar.destroyed_days_remaining < 1) {
        stellar.destroyed_days_remaining = -1;
        stellar.strength = stellar.strength_capacity;
        Mission_ExecuteReactionScript(state,
                                      stellar.schedule_script,
                                      MissionScriptContext{"stellar schedule"});
      }
    } else {
      stellar.destroyed_days_remaining = -1;
    }
  }
  // Active-rank daily salary (0x00466d63): for each active + defined rank with
  // a nonzero Salary, pay it unless the player is already at/above SalaryCap
  // (a cap of 0 or -1 means uncapped); clamp credits back to >= 0 after each
  // payment. The clamp only runs on the paid arm, mirroring the original.
  for (const RankDef &rank : state.scenario.ranks) {
    if (!rank.active || !rank.defined || rank.salary == 0) {
      continue;
    }
    const std::int32_t salary = static_cast<std::int32_t>(rank.salary);
    const std::int32_t cap = static_cast<std::int32_t>(rank.salary_cap);
    if (state.player.credits < cap || cap < 1) {
      state.player.credits += salary;
    }
    if (state.player.credits < 0) {
      state.player.credits = 0;
    }
  }
  // Ship/outfit availability rerolls (the driver's tail): every ship class
  // gets fresh 1..100 licensed threshold/limit rolls, every outfit a fresh
  // 1..100 stock roll. The per-system reinforcement cooldown (SystemDef
  // +0xC4 reinf_cooldown_days, printed as dude_prob +0x1c) is ticked above.
  const std::size_t ship_count =
      std::min(state.scenario.ships.size(), static_cast<std::size_t>(0x300));
  for (std::size_t i = 0; i < ship_count; ++i) {
    state.ship_class_limit_rolls[i] = static_cast<std::int16_t>(
        std::uniform_int_distribution<int>{0, 99}(state.rng) + 1);
    state.ship_class_threshold_rolls[i] = static_cast<std::int16_t>(
        std::uniform_int_distribution<int>{0, 99}(state.rng) + 1);
  }
  const std::size_t outfit_count =
      std::min(state.scenario.outfits.size(), static_cast<std::size_t>(0x200));
  for (std::size_t i = 0; i < outfit_count; ++i) {
    state.outfit_stock_rolls[i] = static_cast<std::int16_t>(
        std::uniform_int_distribution<int>{0, 99}(state.rng) + 1);
  }
}

} // namespace game
