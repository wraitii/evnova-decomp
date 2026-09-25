#include "mission.hpp"

#include "brgr_archive.hpp"
#include "compatibility.hpp"
#include "game_state.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
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
using mission_detail::ResolveContainingSystem;

// @port 0x0044A4D0 100% divergence
// DIVERGENCE(original): the trailing <PSRK>/<SSRK> scan is not ported; the
// government id is parsed directly in the substitution pass.
// Ghidra 0x0044a4d0 Ship_ExpandStringPlaceholders. See the header comment for
// the grammar. Character-state machine over the text (the original walks the
// shared DAT_007c8a10 buffer in place):
//   0 copy-through ('{' -> 1)   1 header: g/G/p/P/b/B/'!'
//   2 digit accumulation        3 scan to opening quote of the chosen arm
//   4 copy arm (\\-escapes)     5 scan past the opening quote (rejected arm)
//   6 skip rejected arm         7 post-arm: '}' ends, '"' copies another arm
//   8 discard until '}'
void Mission_ExpandStringPlaceholders(const GameState &state,
                                      std::string &text) {
  std::string out;
  out.reserve(text.size());
  int machine = 0;
  bool negate = false; // '!' seen in the current header (never reset: quirk)
  bool escaped = false;
  int count = 0;
  char condition = 'P'; // 'P' = {pN} registration, 'B' = {bN} control bit
  for (const char ch : text) {
    switch (machine) {
    case 1:
      if (ch == 'g' || ch == 'G') {
        // g_player_is_male ('m' latch): 1 = male -> first arm; '!' swaps.
        const bool male = state.control.male != negate;
        machine = male ? 3 : 5;
      } else if (ch == 'p' || ch == 'P') {
        count = 0;
        condition = 'P';
        machine = 2;
      } else if (ch == 'b' || ch == 'B') {
        count = 0;
        condition = 'B';
        machine = 2;
      } else if (ch == '!') {
        negate = true;
      }
      // Any other header character is swallowed and the machine stays in 1
      // (the original has no terminator branch in this state).
      break;
    case 2: {
      if (ch >= '0' && ch <= '9') {
        count = count * 10 + (ch - '0');
        break;
      }
      // Condition bodies (Bible dësc grammar):
      //   {pN}/{PN}: registration test. The original refuses the first arm
      //     only when the current expression ship class is unlicensed
      //     (is_licensed_runtime == 0) and the shareware day counter has
      //     reached N (ncb Pxxx semantics); the licensed game always passes.
      //     The port models a registered game, so it keys on
      //     control.registered and treats an unregistered run as false.
      //   {bN}/{BN}: Nova control bit N. The original reads DAT_005914cc[N],
      //     the 10,000-entry control-bit array mirrored by control.bits.
      const bool pass =
          condition == 'P'
              ? state.control.registered
              : (count >= 0 &&
                 state.control.ControlBit(static_cast<std::uint32_t>(count)));
      bool take_first_arm = pass;
      if (negate) {
        take_first_arm = !take_first_arm;
      }
      if (take_first_arm) {
        machine = ch == '"' ? 4 : 3;
      } else {
        machine = ch == '"' ? 6 : 5;
      }
      break;
    }
    case 3:
      if (ch == '"') {
        machine = 4;
      }
      break;
    case 4:
      if (ch == '\\') {
        escaped = true;
      } else if (ch != '"' || escaped) {
        out += ch;
        escaped = false;
      } else {
        machine = 8;
      }
      break;
    case 5:
      if (ch == '"') {
        machine = 6;
      }
      break;
    case 6:
      if (ch == '\\') {
        escaped = true;
      } else if (ch != '"' || escaped) {
        escaped = false;
      } else {
        machine = 7;
      }
      break;
    case 7:
      if (ch == '}') {
        machine = 0;
      } else if (ch == '"') {
        machine = 4;
      }
      break;
    case 8:
      if (ch == '}') {
        machine = 0;
      }
      break;
    default:
      if (ch == '{') {
        machine = 1;
      } else {
        out += ch;
      }
      break;
    }
  }
  // The original's trailing <PSRK...>/<SSRK...> scan latched the government id
  // in g_expanded_psrk_ship_class / _ssrk for a later substitution pass. The
  // port parses each <PRKnnn>/<SRKnnn> token directly in
  // Mission_ExpandMissionWildcards instead (see
  // ReplacePerGovernmentRankTokens).
  text = std::move(out);
}

// ---------------------------------------------------------------------------
// Mission-text wildcard expansion (Bible "special symbols")
// ---------------------------------------------------------------------------

namespace {

// @port 0x00445D70 100%
// Ghidra 0x00445d70 Mission_ReplaceSubstringInMissionText: replace every
// occurrence of `token` in `text` with `replacement` (the original loops
// CStringBuffer_ReplaceSubstring 0x004bc100 over the shared scratch buffer
// until no match remains).
void ReplaceMissionToken(std::string &text,
                         const std::string &token,
                         const std::string &replacement) {
  if (token.empty()) {
    return;
  }
  std::size_t pos = 0;
  while ((pos = text.find(token, pos)) != std::string::npos) {
    text.replace(pos, token.size(), replacement);
    pos += replacement.size();
  }
}

// Stellar name for <DST>/<RST>; the locals in 0x004444f0 start as "[Error]"
// and keep it only when the id is out of range -- the original copies an
// empty display name verbatim.
[[nodiscard]] std::string MissionStellarName(const GameState &state,
                                             std::int16_t stellar_id) {
  if (stellar_id < 0 ||
      stellar_id >= static_cast<std::int16_t>(state.scenario.stellars.size())) {
    return "[Error]";
  }
  const auto &stellar =
      state.scenario.stellars[static_cast<std::size_t>(stellar_id)];
  return stellar.name;
}

// System name for <DSY>/<RSY>. The original resolves the stellar's owning
// system through System_ResolveVisibleSystemForTravel, then the discovery
// slot, then System_FindSystemContainingStellar; the port's membership map is
// already the visible-system resolution, so only the visible resolve runs
// here; when it fails the original keeps "[Error]" (no raw-id fallback).
// Like stellar names, an empty system name is copied verbatim.
[[nodiscard]] std::string MissionSystemName(const GameState &state,
                                            std::int16_t system_id) {
  // No raw-id fallback: when visibility resolution fails the original keeps
  // "[Error]".
  const std::int16_t resolved =
      Misn_ResolveVisibleSystemForTravel(state, system_id);
  if (resolved < 0 ||
      resolved >= static_cast<std::int16_t>(state.scenario.systems.size())) {
    return "[Error]";
  }
  return state.scenario.systems[static_cast<std::size_t>(resolved)].name;
}

// @port 0x00465c10 100%
// Ghidra 0x00465c10 CString_AppendFormattedQuantity: plain digits below 1000,
// comma grouping below one million, x.xxM above.
[[nodiscard]] std::string FormatMissionQuantity(std::uint32_t value) {
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

// <PAY>: PayVal encoding shared with the acceptance-credit gate - positive is
// the credit amount, below -50000 is an acceptance cost (abs - 50000), and
// -40035..-40001 is a percentage (|PayVal| - 40000 hundredths, DOUBLE_00575508)
// of the player's current credits. The remaining negative encodings show 0.
[[nodiscard]] std::string MissionPayText(const GameState &state,
                                         std::int32_t pay_val) {
  std::int64_t shown = 0;
  if (pay_val > 0) {
    shown = pay_val;
  } else if (pay_val < -50000) {
    shown = -static_cast<std::int64_t>(pay_val) - 50000;
  } else if (pay_val < -40000 && pay_val >= -40035) {
    // Percent-of-holdings arm: float arithmetic with half-even rounding like
    // the x87 FISTP path (cf. the reputation loop in
    // Mission_ResolveMissionSuccess).
    const float magnitude = static_cast<float>(-pay_val - 40000);
    const float scaled =
        static_cast<float>(state.player.credits) * magnitude * 0.01F;
    shown = std::lrint(scaled);
    if (shown < 0) {
      shown = 0;
    }
  }
  return FormatMissionQuantity(static_cast<std::uint32_t>(shown));
}

// <CT>: the STR# 0xfa1 commodity name for the mission cargo type, with the
// leading '*' quantityless marker stripped (Bible note on quantityless cargo).
[[nodiscard]] std::string MissionCargoName(std::int16_t cargo_type) {
  if (cargo_type < 0 || cargo_type >= 0x100) {
    return "[Error]";
  }
  auto name = NovaResources_LoadPatchedStringEntry(
      0xfa1, static_cast<std::uint16_t>(cargo_type + 1), 0x238c);
  if (!name) {
    return "[Error]";
  }
  if (!name->empty() && name->front() == '*') {
    name->erase(name->begin());
  }
  return *name;
}

// STR# 0x7d2 entry 0x155 ("captain"): the original's fallback when no rank
// applies to <PRK>/<SRK>/<RRK> or a per-government <PRKnnn>/<SRKnnn>.
[[nodiscard]] std::string MissionRankFallback() {
  return NovaHud_LoadStringEntry(0x7d2, 0x155).value_or("captain");
}

// <PRKnnn>/<SRKnnn> per-government rank names (Bible 1796-1799). `nnn` is the
// government resource id (0x80-based), so the government index is nnn - 0x80;
// a well-formed token with an out-of-range id is left untouched (the original
// pre-scan stores -1 and skips it). DIVERGENCE: the original pre-scanned
// <PRK...>/<SRK...> into g_expanded_psrk_ship_class / _ssrk and substituted
// from crossed buffers in Stellar_BuildTravelDestinationDescription
// (0x004444f0, byte verified: <PRKnnn> read the ssrk-gated frame and both
// per-government loops used ShortName +0xde). That broke shipped text (the
// Federation <PRK128> briefing would read "captain" with no <SRK...> present),
// so each token is parsed directly here and uses the Bible field (ConvName for
// PRK, ShortName for SRK).
void ReplacePerGovernmentRankTokens(const GameState &state,
                                    std::string &text,
                                    std::string_view prefix,
                                    bool use_short_name) {
  std::size_t pos = 0;
  while ((pos = text.find(prefix, pos)) != std::string::npos) {
    std::size_t i = pos + prefix.size();
    const std::size_t digits_begin = i;
    std::uint32_t id = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      if (id <= 0x17fU) {
        id = id * 10U + static_cast<std::uint32_t>(text[i] - '0');
      }
      ++i;
    }
    // The original requires digits and a closing '>' before it substitutes.
    if (i == digits_begin || i >= text.size() || text[i] != '>') {
      pos += prefix.size();
      continue;
    }
    if (id < 0x80U || id > 0x17fU) {
      pos = i + 1;
      continue;
    }
    const std::int16_t slot = Rank_HighestWeightedActiveSlotForGovernment(
        state, static_cast<std::int16_t>(id - 0x80U), use_short_name);
    std::string replacement = MissionRankFallback();
    if (slot >= 0 &&
        static_cast<std::size_t>(slot) < state.scenario.ranks.size()) {
      const RankDef &rank =
          state.scenario.ranks[static_cast<std::size_t>(slot)];
      replacement = use_short_name ? rank.short_name : rank.conv_name;
    }
    text.replace(pos, i + 1 - pos, replacement);
    pos += replacement.size();
  }
}

} // namespace

// @port 0x004444F0 80% gameplay,ui,bugfix
// Ghidra 0x004444f0 Stellar_BuildTravelDestinationDescription (wildcard pass)
// + 0x00445d70 Mission_ReplaceSubstringInMissionText. Expands the Bible
// mission-text wildcards in `text` and returns the result. `offering_list`
// selects the offer-row arm (mission_id = definition index, targets read from
// mission_target_resolutions, pay from the definition); the active arm reads
// the accepted mission slot. Tokens without a modeled source expand to the
// original's "[Error]" sentinel.
std::string Mission_ExpandMissionWildcards(const GameState &state,
                                           std::string_view text,
                                           bool offering_list,
                                           std::int16_t mission_id) {
  std::int16_t travel_stellar = -1;
  std::int16_t travel_system = -1;
  std::int16_t return_stellar = -1;
  std::int16_t return_system = -1;
  std::int16_t cargo_type = -1;
  std::int16_t cargo_qty = 0;
  std::int32_t pay_val = 0;
  std::string special_ship_name;
  const ActiveMission *active = nullptr;
  if (offering_list) {
    if (mission_id >= 0 &&
        static_cast<std::size_t>(mission_id) < state.scenario.missions.size()) {
      const auto &target =
          state
              .mission_target_resolutions[static_cast<std::size_t>(mission_id)];
      travel_stellar = target.travel_stellar_id;
      travel_system = target.travel_system_id;
      return_stellar = target.return_stellar_id;
      return_system = target.return_system_id;
      cargo_type = target.cargo_type_id;
      cargo_qty = target.cargo_qty_tons;
      pay_val = state.scenario.missions[static_cast<std::size_t>(mission_id)]
                    .resource_delta_or_cost;
    }
  } else if (mission_id >= 0 &&
             static_cast<std::size_t>(mission_id) <
                 state.active_missions.size() &&
             state
                 .active_mission_runtime_flags[static_cast<std::size_t>(
                     mission_id)]
                 .is_active) {
    active = &state.active_missions[static_cast<std::size_t>(mission_id)];
    travel_stellar = active->travel_stellar_id;
    return_stellar = active->return_stellar_id;
    travel_system = ResolveContainingSystem(state, travel_stellar);
    return_system = ResolveContainingSystem(state, return_stellar);
    cargo_type = active->cargo_type_id;
    cargo_qty = active->cargo_qty_tons;
    pay_val = active->resource_delta_or_cost;
  }

  // <SN>: g_active_misn +0x40 mission_fleet_name, drawn at acceptance from
  // the +0x2a pool (Mission_PopulateMissionSlotFromDef 0x0043f8c0); empty
  // when no pool was drawn. Re-resolving the stored (pool, entry) pair
  // yields the same text (cf. MissionShipPoolString in hud_renderer.cpp).
  // Inactive slots keep the "[Error]" sentinel (the original's LAB arm
  // skips the mission-token fill when the slot is not active).
  if (active != nullptr && active->special_ship_name_entry >= 1) {
    if (auto name = NovaHud_LoadStringEntry(
            static_cast<std::uint16_t>(active->special_ship_name_string_id),
            static_cast<std::uint16_t>(active->special_ship_name_entry))) {
      special_ship_name = *name;
    }
  }
  std::string destination = MissionStellarName(state, travel_stellar);
  std::string destination_system = MissionSystemName(state, travel_system);
  const std::string return_dest = MissionStellarName(state, return_stellar);
  const std::string return_system_name =
      MissionSystemName(state, return_system);
  // An unresolvable destination falls back to the return destination when that
  // one resolves (the original's "[Error]" equality swap).
  if (destination == "[Error]" && return_dest != "[Error]") {
    destination = return_dest;
  }
  if (destination_system == "[Error]" && return_system_name != "[Error]") {
    destination_system = return_system_name;
  }

  // Player identity: pilot.first_name is the full name (DAT_007d20b7),
  // pilot.last_name the nickname (DAT_007d21b7; <PNN> falls back to the full
  // name when unset).
  const std::string player_name = state.pilot.first_name;
  const std::string nickname = state.pilot.last_name.empty()
                                   ? state.pilot.first_name
                                   : state.pilot.last_name;
  std::string ship_type = "[Error]";
  if (state.player.ship_class_id >= 0) {
    if (const auto *ship_class = state.scenario.Ship(static_cast<std::int16_t>(
            state.player.ship_class_id + kResourceIdBase));
        ship_class != nullptr && !ship_class->display_name.empty()) {
      ship_type = ship_class->display_name;
    }
  }

  std::string result(text);
  ReplaceMissionToken(result, "<DST>", destination);
  ReplaceMissionToken(result, "<DSY>", destination_system);
  ReplaceMissionToken(result, "<RST>", return_dest);
  ReplaceMissionToken(result, "<RSY>", return_system_name);
  ReplaceMissionToken(result, "<CT>", MissionCargoName(cargo_type));
  ReplaceMissionToken(
      result, "<CQ>", cargo_type >= 0 ? std::to_string(cargo_qty) : "[Error]");
  ReplaceMissionToken(
      result, "<SN>", active != nullptr ? special_ship_name : "[Error]");
  // <DL> (0x004444f0 active-slot arm): the runtime flags' absolute deadline
  // date formatted with the full month names; when the deadline equals the
  // current date the original's format buffer keeps its empty init content,
  // so the token expands to "".
  if (active != nullptr && mission_id >= 0 &&
      static_cast<std::size_t>(mission_id) <
          state.active_mission_runtime_flags.size()) {
    const MissionRuntimeFlags &slot =
        state
            .active_mission_runtime_flags[static_cast<std::size_t>(mission_id)];
    if (slot.deadline_year != 0) {
      const GameDate deadline{
          slot.deadline_year, slot.deadline_month, slot.deadline_day};
      std::string deadline_text;
      if (deadline.year != state.date.year ||
          deadline.month != state.date.month ||
          deadline.day != state.date.day) {
        deadline_text = NovaText_FormatDateString(
            deadline, false, state.date_prefix, state.date_suffix);
      }
      ReplaceMissionToken(result, "<DL>", deadline_text);
    } else {
      ReplaceMissionToken(result, "<DL>", "[Error]");
    }
  } else {
    // Offer-row arm: the per-definition target block's deadline, formatted
    // the same way. The buffer keeps its "[Error]" init only when the stored
    // deadline equals today; no-TimeLimit missions have zeroed block fields
    // (0x0043d240 leaves them untouched for steps < 1), so their <DL>
    // expands to the zero date, matching the original's garbage.
    std::string deadline_text = std::string("[Error]");
    if (mission_id >= 0 && static_cast<std::size_t>(mission_id) <
                               state.mission_target_resolutions.size()) {
      const MissionTargetResolution &target =
          state
              .mission_target_resolutions[static_cast<std::size_t>(mission_id)];
      const GameDate deadline{
          target.deadline_year, target.deadline_month, target.deadline_day};
      if (deadline.year != state.date.year ||
          deadline.month != state.date.month ||
          deadline.day != state.date.day) {
        deadline_text = NovaText_FormatDateString(
            deadline, false, state.date_prefix, state.date_suffix);
      }
    }
    ReplaceMissionToken(result, "<DL>", deadline_text);
  }
  ReplaceMissionToken(result, "<PN>", player_name);
  ReplaceMissionToken(result, "<PNN>", nickname);
  ReplaceMissionToken(result,
                      "<PSN>",
                      state.player.ship_name.empty() ? std::string("[Error]")
                                                     : state.player.ship_name);
  ReplaceMissionToken(result, "<PST>", ship_type);
  // <OSN>: the speaking mission ship's personality display name, read from
  // the g_mission_speaker_ship_slot latch (valid slots 1..0x3f only). Outside
  // an announcement context it keeps the [Error] sentinel.
  std::string speaker_name = "[Error]";
  if (state.mission_speaker_ship_slot >= 0 &&
      state.mission_speaker_ship_slot < 0x40) {
    const Ship &speaker =
        state.ShipAt(static_cast<std::size_t>(state.mission_speaker_ship_slot));
    if (speaker.pers_def_slot >= 0 &&
        static_cast<std::size_t>(speaker.pers_def_slot) <
            state.scenario.pers_defs.size()) {
      const std::string &name =
          state.scenario
              .pers_defs[static_cast<std::size_t>(speaker.pers_def_slot)]
              .display_name;
      if (!name.empty()) {
        speaker_name = name;
      }
    }
  }
  ReplaceMissionToken(result, "<OSN>", speaker_name);
  // <PRK>/<SRK>/<RRK>: the highest-weighted active rank's ConvName / ShortName
  // (Stellar_BuildTravelDestinationDescription 0x004444f0), and the full name
  // at GameState.recently_activated_rank_id for <RRK>. With no applicable rank
  // the original falls back to STR# 0x7d2 entry 0x155 ("captain").
  const std::int16_t prk_slot = Rank_HighestWeightedActiveSlot(state, false);
  const std::int16_t srk_slot = Rank_HighestWeightedActiveSlot(state, true);
  const std::int16_t rrk_slot = state.recently_activated_rank_id;
  ReplaceMissionToken(
      result,
      "<PRK>",
      prk_slot < 0
          ? MissionRankFallback()
          : state.scenario.ranks[static_cast<std::size_t>(prk_slot)].conv_name);
  ReplaceMissionToken(
      result,
      "<SRK>",
      srk_slot < 0 ? MissionRankFallback()
                   : state.scenario.ranks[static_cast<std::size_t>(srk_slot)]
                         .short_name);
  ReplaceMissionToken(
      result,
      "<RRK>",
      rrk_slot < 0
          ? MissionRankFallback()
          : state.scenario.ranks[static_cast<std::size_t>(rrk_slot)].full_name);
  ReplaceMissionToken(result, "<PAY>", MissionPayText(state, pay_val));
  // FUN_004d45a0: the registration name, or the shared "EV Nova Community"
  // string when no name is registered. The port has no registration system.
  ReplaceMissionToken(result, "<REG>", "EV Nova Community");
  // BUGFIX(original): the original's crossed pre-scan buffers break shipped
  // <PRKnnn>/<SRKnnn> text. See ReplacePerGovernmentRankTokens.
  if (kApplyOriginalBugFixes) {
    ReplacePerGovernmentRankTokens(
        state, result, "<PRK", /*use_short_name=*/false);
    ReplacePerGovernmentRankTokens(
        state, result, "<SRK", /*use_short_name=*/true);
  }
  return result;
}

MissionDialogText Mission_LoadSelectionDialogText(const GameState &state,
                                                  std::uint16_t desc_id,
                                                  bool offering_list,
                                                  std::int16_t mission_id) {
  MissionDialogText message;
  const auto desc = NovaResource_LoadDescription(desc_id);
  if (!desc) {
    return message;
  }
  message.text = desc->text;
  message.dialog_variant = desc->dialog_variant;
  // Ui_LoadSelectionDialogResource runs the ^-placeholder pass at load time
  // (0x004c6d50); the mission wildcard pass is Stellar_BuildTravelDestination-
  // Description (0x004444f0).
  Mission_ExpandStringPlaceholders(state, message.text);
  message.text = Mission_ExpandMissionWildcards(
      state, message.text, offering_list, mission_id);
  return message;
}

namespace {

// @port 0x00468600 70% gameplay
// Ghidra 0x00468600 Stellar_FormatElapsedTravelTime. TODO(decomp): the
// offer-row <DL> arm reads the mission target-resolution table (dates not
// tracked); elapsed-days params 3/4 are ignored by the original body too.
// Shared body of NovaText_FormatDateString (0x00468450) and
// Stellar_FormatElapsedTravelTime (0x00468600): "MONTH DAYst, YEAR" with the
// STR# 0x89 month table and day suffixes (st/nd/rd by last digit, th
// otherwise, 11-13 forced back to th).
[[nodiscard]] std::string FormatDateString(const GameDate &date,
                                           std::uint16_t month_entry,
                                           std::string_view prefix,
                                           std::string_view suffix) {
  std::string out{prefix};
  if (const auto month = NovaHud_LoadStringEntry(0x89, month_entry)) {
    out += *month;
  }
  out += ' ';
  out += std::to_string(date.day);
  std::uint16_t ordinal_suffix = 0x1c; // "th"
  const int digit = date.day % 10;
  if (digit == 1) {
    ordinal_suffix = 0x19;
  } else if (digit == 2) {
    ordinal_suffix = 0x1a;
  } else if (digit == 3) {
    ordinal_suffix = 0x1b;
  }
  if (date.day > 10 && date.day < 14) {
    ordinal_suffix = 0x1c;
  }
  if (const auto text = NovaHud_LoadStringEntry(0x89, ordinal_suffix)) {
    out += *text;
  }
  out += ", ";
  out += std::to_string(date.year);
  out += suffix;
  return out;
}

} // namespace

// @port 0x00468450 100%
// Ghidra 0x00468450 NovaText_FormatDateString.
std::string NovaText_FormatDateString(const GameDate &date,
                                      bool abbreviated_month,
                                      std::string_view prefix,
                                      std::string_view suffix) {
  // The UI sites (BBS date 0x00441620, mission-info window, starmap status
  // bar) use the abbreviated month names (STR# 0x89 entries 13-24);
  // Stellar_FormatElapsedTravelTime (arrival message / <DL> token) uses the
  // full names (entries 1-12).
  return FormatDateString(date,
                          static_cast<std::uint16_t>(
                              abbreviated_month ? date.month + 12 : date.month),
                          prefix,
                          suffix);
}

} // namespace game
